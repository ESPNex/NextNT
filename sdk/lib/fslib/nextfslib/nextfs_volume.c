/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_volume.c
 * PURPOSE:     Mount/unmount, transactions, checkpointing, namespace and data
 * LICENSE:     GPL-3.0-or-later
 */

#include "nextfs_internal.h"

#include <stdlib.h>
#include <string.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

#define NFS_CLUSTER_BYTES(Volume) ((Volume)->Super.ClusterSize)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* The PIFT-on-disk area is split in two halves, one per live generation. */
static nfs_u64
NfsPiftHalfClusters(const NFS_SUPER *Super)
{
    return Super->PiftAreaClusters / 2;
}

static nfs_u64
NfsPiftHalfFirst(const NFS_SUPER *Super, nfs_u64 Generation)
{
    return Super->PiftAreaFirst + (((Generation & 1) ? NfsPiftHalfClusters(Super) : 0));
}

/* ------------------------------------------------------------------ */

static nfs_u64
NfsNow(void)
{
    /* v0.1 keeps a monotonic pseudo-timestamp so records still order. */
    static nfs_u64 Counter = 0;
    return ++Counter;
}

void
NfsSetDirty(PNFS_VOLUME Volume, int Dirty)
{
    if (Dirty)
        Volume->Super.Flags |= NFS_SB_DIRTY;
    else
        Volume->Super.Flags &= ~NFS_SB_DIRTY;
}

/* ------------------------------------------------------------------ */
/* Transactions                                                        */
/* ------------------------------------------------------------------ */

static int
NfsCommitChunk(PNFS_VOLUME Volume, const nfs_u8 *Payload, nfs_u32 PayloadLen,
               NFS_PIFT_RECORD *Records, nfs_u32 First, nfs_u32 Last)
{
    nfs_u8 Frame[NFS_FRAME_SIZE];
    NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
    nfs_seq Seq;
    nfs_u32 i;
    int Status;

    Seq = Volume->Super.JournalSeqNext++;

    /* 1. Uncommitted journal frame. */
    Status = NfsJournalWriteFrame(Volume, Seq, 0, Payload, PayloadLen);
    if (Status != NFS_OK)
        return Status;

    /* 2. Write-through copy in the PIFT-on-disk area. */
    memset(Frame, 0, sizeof(Frame));
    memcpy(Header->Magic, NFS_FRAME_MAGIC, 4);
    Header->Flags = 0;
    Header->Seq = Seq;
    Header->PayloadLen = PayloadLen;
    memcpy(Frame + NFS_FRAME_HDR_SIZE, Payload, PayloadLen);
    Header->Crc32c = NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, PayloadLen);
    Status = NfsPiftAppend(Volume, Frame, NFS_FRAME_SIZE);
    if (Status != NFS_OK)
        return Status;

    /* 3. Commit point. */
    Status = NfsJournalCommit(Volume, Seq);
    if (Status != NFS_OK)
        return Status;
    Volume->Stats.JournalCommits++;

    /* 4. Publish in RAM and keep the child index in step. */
    for (i = First; i <= Last; i++)
    {
        nfs_u32 Index = 0;

        Records[i].Seq = Seq;
        Records[i].Mtime = NfsNow();

        if (NfsIndexLocate(Volume, Records[i].FileId, &Index) == NFS_OK)
            NfsChildrenRemove(Volume, Index);

        if (NfsIndexInsertEx(Volume, &Records[i], &Index) != NFS_OK)
            return NFS_ENOSPC;

        if (!(Records[i].Flags & NFS_FLAG_DELETED))
        {
            if (NfsChildrenAdd(Volume, Index) != NFS_OK)
                return NFS_ENOSPC;
        }
    }

    return NFS_OK;
}

int
NfsCommitRecords(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Records, nfs_u32 Count)
{
    nfs_u8 Payload[NFS_FRAME_PAYLOAD_MAX];
    nfs_u32 PayloadLen = 0;
    nfs_u32 Applied = 0;
    nfs_u32 i;
    int Status;

    if (Count == 0)
        return NFS_OK;

    /* Compact before writing if the write-through area is already past its
       high-water mark: appending first and checkpointing afterwards would fail
       on a full area instead of making room. */
    if (Volume->Super.PiftHead > NFS_PIFT_HIGH_WATER(Volume->Super) ||
        Volume->Super.PiftHead + NFS_FRAME_SIZE >
            (nfs_u64)Volume->Super.PiftClusters * Volume->Super.ClusterSize)
    {
        Status = NfsCheckpoint(Volume);
        if (Status != NFS_OK)
            return Status;
    }

    for (i = 0; i < Count; i++)
    {
        nfs_u8 RecordBuffer[NFS_NAME_MAX * 2 + NFS_EXTENT_MAX * 24 + 128];
        nfs_u32 RecordLen;

        RecordLen = NfsPiftSerialize(&Records[i], RecordBuffer, sizeof(RecordBuffer));
        if (RecordLen == 0)
            return NFS_EINVAL;

        if (PayloadLen + RecordLen > sizeof(Payload))
        {
            if (PayloadLen == 0)
                return NFS_EINVAL;   /* a single record must fit in a frame */

            Status = NfsCommitChunk(Volume, Payload, PayloadLen, Records, Applied, i - 1);
            if (Status != NFS_OK)
                return Status;
            Applied = i;
            PayloadLen = 0;
        }

        memcpy(Payload + PayloadLen, RecordBuffer, RecordLen);
        PayloadLen += RecordLen;
    }

    if (PayloadLen)
    {
        Status = NfsCommitChunk(Volume, Payload, PayloadLen, Records, Applied, Count - 1);
        if (Status != NFS_OK)
            return Status;
    }

    Volume->Transactions++;
    NfsSetDirty(Volume, 1);

    if (Volume->Super.PiftHead > NFS_PIFT_HIGH_WATER(Volume->Super))
        return NfsCheckpoint(Volume);

    /* Never let the ring overwrite frames that replay may still need. */
    if (Volume->Super.JournalSeqNext - Volume->Super.JournalSeqFirst + 1 >=
        Volume->JournalFrames)
        return NfsCheckpoint(Volume);

    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Checkpoint: write the live records as a fresh copy-on-write chain   */
/* ------------------------------------------------------------------ */

int
NfsCheckpoint(PNFS_VOLUME Volume)
{
    nfs_u64 OldBlocks[8192];
    nfs_u32 OldCount = 0;
    nfs_u64 Root = 0;
    nfs_u64 Generation = Volume->Super.Generation + 1;
    nfs_u32 i;
    int Status;

    /* 1. Remember the blocks of the *previous* generation: those are the ones
       this checkpoint retires. The current tree becomes the new fallback, so a
       volume can always mount from the generation before. */
    Status = NfsTreeCollect(Volume, Volume->Super.CheckpointRootPrev.Block,
                            OldBlocks, 8192, &OldCount);
    if (Status != NFS_OK)
    {
        /* A corrupt old tree must not stop a checkpoint: we simply cannot
           reclaim its blocks. Report nothing, leak them until nextchk. */
        OldCount = 0;
    }

    /* 2. Build a fresh copy-on-write tree. */
    Status = NfsTreeBuild(Volume, Generation, &Root);
    if (Status != NFS_OK)
        return Status;

    Status = NfsDevFlush(Volume);
    if (Status != NFS_OK)
        return Status;

    /* 3. Publish with one superblock pair write. */
    Volume->Super.CheckpointRootPrev = Volume->Super.CheckpointRoot;
    Volume->Super.CheckpointRoot.Block = Root;
    Volume->Super.CheckpointRoot.Generation = Generation;
    Volume->Super.Generation = Generation;
    /* Fresh write-through half for the new generation: the fallback generation
       keeps its own, so mounting it still sees every record it ever appended. */
    Volume->Super.PiftFirst = NfsPiftHalfFirst(&Volume->Super, Generation);
    Volume->Super.PiftClusters = (nfs_u32)NfsPiftHalfClusters(&Volume->Super);
    Volume->Super.PiftHead = 0;
    Volume->Super.JournalSeqFirst = Volume->Super.JournalSeqNext;
    NfsSetDirty(Volume, 0);

    Status = NfsWriteSuper(Volume);
    if (Status != NFS_OK)
        return Status;

    /* 4. Retire the old tree. */
    for (i = 0; i < OldCount; i++)
        NfsClusterFree(Volume, OldBlocks[i], 1);

    Status = NfsBitmapFlush(Volume);
    if (Status != NFS_OK)
        return Status;

    Volume->Transactions = 0;
    Volume->Stats.Checkpoints++;
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Mount / unmount / sync                                              */
/* ------------------------------------------------------------------ */

static int
NfsMountAcceptRecord(void *Context, const NFS_PIFT_RECORD *Record)
{
    PNFS_VOLUME Volume = (PNFS_VOLUME)Context;
    NFS_PIFT_RECORD *Existing;

    Existing = NfsIndexGet(Volume, Record->FileId);
    if (Existing && Existing->Seq > Record->Seq)
        return NFS_OK;           /* a newer write-through record already won */

    return (NfsIndexInsert(Volume, Record) == NFS_OK) ? NFS_OK : NFS_ENOSPC;
}

int
NfsMount(PNFS_BDEV Device, PNFS_VOLUME *OutVolume)
{
    PNFS_VOLUME Volume;
    int Status;

    if (!Device || !OutVolume)
        return NFS_EINVAL;

    Volume = (PNFS_VOLUME)NfsMemAlloc(sizeof(NFS_VOLUME));
    if (!Volume)
        return NFS_ENOSPC;
    memset(Volume, 0, sizeof(*Volume));
    Volume->Device = *Device;

    Status = NfsReadSuper(Device, &Volume->Super);
    if (Status != NFS_OK)
        goto Fail;

    Volume->JournalFrames =
        ((nfs_u64)Volume->Super.JournalClusters * Volume->Super.ClusterSize) / NFS_FRAME_SIZE;

    Status = NfsBitmapLoad(Volume);
    if (Status != NFS_OK)
        goto Fail;

    Status = NfsChildrenInit(Volume);
    if (Status != NFS_OK)
        goto Fail;

    Status = NfsIndexInit(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* The pin set exists before any block can be freed: the allocator consults
       it, and it is filled once the records are known (step 4). */
    Status = NfsPinInit(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* 1. Checkpoint tree: the last compacted state of the PIFT. If the newest
       generation does not verify, fall back to the previous one and flag the
       volume: nextchk has to look at it. */
    Status = NfsTreeWalk(Volume, Volume->Super.CheckpointRoot.Block,
                         NfsMountAcceptRecord, Volume);
    if (Status != NFS_OK)
    {
        Status = NfsTreeWalk(Volume, Volume->Super.CheckpointRootPrev.Block,
                             NfsMountAcceptRecord, Volume);
        if (Status != NFS_OK)
            goto Fail;
        /* Follow the fallback generation to its own PIFT half and read all of
           it: the highest sequence per file wins, so stale frames are inert. */
        Volume->Super.PiftFirst = NfsPiftHalfFirst(&Volume->Super,
                                                   Volume->Super.CheckpointRootPrev.Generation);
        Volume->Super.PiftClusters = (nfs_u32)NfsPiftHalfClusters(&Volume->Super);
        Volume->Super.PiftHead = NfsPiftHalfClusters(&Volume->Super) *
                                 Volume->Super.ClusterSize;
        Volume->Super.Flags |= NFS_SB_NEEDS_CHECK;
        Volume->Stats.UsedFallbackRoot = 1;
    }

    /* 2. PIFT-on-disk area: newer than the checkpoint, still not read at runtime. */
    Status = NfsPiftScan(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* 3. Journal: re-apply committed transactions that never reached RAM. */
    Status = NfsJournalReplay(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* Everything committed has been applied: the ring can start over. Without
       this the stale JournalSeqFirst would make the first commit look like a
       ring wrap and force an immediate checkpoint. */
    Volume->Super.JournalSeqFirst = Volume->Super.JournalSeqNext;

    if (!NfsIndexGet(Volume, NFS_ROOT_ID))
    {
        Status = NFS_ECORRUPT;   /* a volume without a root is not a volume */
        goto Fail;
    }

    /* Compute the fileid allocator and the file count. */
    {
        nfs_u32 i;
        nfs_u64 MaxId = NFS_ROOT_ID;
        nfs_u32 Live = 0;

        for (i = 0; i < Volume->RecordCount; i++)
        {
            if (Volume->Records[i].FileId > MaxId)
                MaxId = Volume->Records[i].FileId;
            if (!(Volume->Records[i].Flags & NFS_FLAG_DELETED))
                Live++;
        }
        Volume->NextFileId = MaxId + 1;
    Volume->Stats.Files = Live;
    }

    Status = NfsChildrenRebuild(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* 4. Snapshots: pin every generation a snapshot record points at. The pins
       are derived, so a crash cannot leave them out of step with the trees. */
    Status = NfsSnapshotPinsRebuild(Volume);
    if (Status != NFS_OK)
        goto Fail;

    NfsSetDirty(Volume, 1);
    NfsWriteSuper(Volume);

    *OutVolume = Volume;
    return NFS_OK;

Fail:
    NfsChildrenDestroy(Volume);
    NfsIndexDestroy(Volume);
    NfsPinDestroy(Volume);
    NfsMemFree(Volume->Bitmap);
    NfsMemFree(Volume);
    return Status;
}

int
NfsSync(PNFS_VOLUME Volume)
{
    int Status;

    Status = NfsCheckpoint(Volume);
    if (Status != NFS_OK)
        return Status;
    Status = NfsBitmapFlush(Volume);
    if (Status != NFS_OK)
        return Status;
    NfsSetDirty(Volume, 0);
    return NfsWriteSuper(Volume);
}

void
NfsDiscard(PNFS_VOLUME Volume)
{
    /* Drop the volume without checkpointing: what a power cut does to RAM. */
    if (!Volume)
        return;
    NfsChildrenDestroy(Volume);
    NfsIndexDestroy(Volume);
    NfsPinDestroy(Volume);
    NfsMemFree(Volume->Bitmap);
    NfsMemFree(Volume);
}

void
NfsUnmount(PNFS_VOLUME Volume)
{
    if (!Volume)
        return;
    NfsSync(Volume);
    NfsChildrenDestroy(Volume);
    NfsIndexDestroy(Volume);
    NfsPinDestroy(Volume);
    NfsMemFree(Volume->Bitmap);
    NfsMemFree(Volume);
}

void
NfsGetStats(PNFS_VOLUME Volume, PNFS_STATS Stats)
{
    if (Volume && Stats)
        *Stats = Volume->Stats;
}

/* ------------------------------------------------------------------ */
/* Namespace                                                           */
/* ------------------------------------------------------------------ */

int
NfsCreate(PNFS_VOLUME Volume, nfs_fileid Parent, const nfs_u16 *Name,
          nfs_u32 NameLength, int IsDirectory, nfs_fileid *OutFileId)
{
    NFS_PIFT_RECORD Record;
    NFS_PIFT_RECORD *ParentRecord;
    int Status;

    if (!Volume || !Name || NameLength == 0 || NameLength > NFS_NAME_MAX)
        return NFS_EINVAL;

    ParentRecord = NfsIndexGet(Volume, Parent);
    if (!ParentRecord || !(ParentRecord->Flags & NFS_FLAG_DIR))
        return NFS_ENOENT;

    if (NfsChildrenLookup(Volume, Parent, Name, NameLength))
        return NFS_EEXIST;

    memset(&Record, 0, sizeof(Record));
    Record.FileId = Volume->NextFileId++;
    Record.Generation = 1;
    Record.Parent = Parent;
    Record.Flags = IsDirectory ? NFS_FLAG_DIR : 0;
    Record.Attributes = IsDirectory ? 0x10 : 0x80;
    Record.NameLen = NameLength;
    memcpy(Record.Name, Name, (size_t)NameLength * sizeof(nfs_u16));
    Record.Btime = Record.Mtime = NfsNow();

    Status = NfsCommitRecords(Volume, &Record, 1);
    if (Status != NFS_OK)
        return Status;

    Volume->Stats.Files++;
    if (OutFileId)
        *OutFileId = Record.FileId;
    return NFS_OK;
}

int
NfsLookup(PNFS_VOLUME Volume, nfs_fileid Parent, const nfs_u16 *Name,
          nfs_u32 NameLength, NFS_PIFT_RECORD *OutRecord)
{
    NFS_PIFT_RECORD *Record;

    if (!Volume)
        return NFS_EINVAL;

    Record = NfsChildrenLookup(Volume, Parent, Name, NameLength);
    if (!Record)
        return NFS_ENOENT;
    if (OutRecord)
        *OutRecord = *Record;
    return NFS_OK;
}

int
NfsOpenById(PNFS_VOLUME Volume, nfs_fileid FileId, NFS_PIFT_RECORD *OutRecord)
{
    NFS_PIFT_RECORD *Record;

    if (!Volume)
        return NFS_EINVAL;
    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (OutRecord)
        *OutRecord = *Record;
    return NFS_OK;
}

int
NfsReadDir(PNFS_VOLUME Volume, nfs_fileid Directory, nfs_u32 Index,
           NFS_PIFT_RECORD *OutRecord)
{
    NFS_PIFT_RECORD *Record;

    if (!Volume || !OutRecord)
        return NFS_EINVAL;

    Record = NfsChildrenAt(Volume, Directory, Index);
    if (!Record)
        return NFS_ENOENT;
    *OutRecord = *Record;
    return NFS_OK;
}

int
NfsRemove(PNFS_VOLUME Volume, nfs_fileid FileId)
{
    NFS_PIFT_RECORD *Record;
    NFS_PIFT_RECORD Updated;
    nfs_u32 i;
    int Status;

    if (!Volume || FileId == NFS_ROOT_ID)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    if (NfsChildrenAt(Volume, FileId, 0) != NULL)
        return NFS_EINVAL;   /* only empty directories can be removed */

    Updated = *Record;
    Updated.Flags |= NFS_FLAG_DELETED;
    Updated.Generation++;

    Status = NfsCommitRecords(Volume, &Updated, 1);
    if (Status != NFS_OK)
        return Status;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        if (Record->Extents[i].Lcn != NFS_HOLE)
            NfsClusterFree(Volume, Record->Extents[i].Lcn, Record->Extents[i].Length);
    }
    if (Volume->Stats.Files > 0)
        Volume->Stats.Files--;
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */

int
NfsMapClusterRecord(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record,
                    nfs_u64 Vcn, nfs_u64 *Lcn)
{
    nfs_u32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record->Extents[i];

        if (Vcn >= Extent->Vcn && Vcn < Extent->Vcn + Extent->Length)
        {
            *Lcn = (Extent->Lcn == NFS_HOLE) ? NFS_HOLE : Extent->Lcn + (Vcn - Extent->Vcn);
            return NFS_OK;
        }
    }
    *Lcn = NFS_HOLE;
    return NFS_ENOENT;
}

int
NfsCoalesceExtents(NFS_PIFT_RECORD *Record)
{
    nfs_u32 In, Out = 0;

    for (In = 0; In < Record->ExtentCount; In++)
    {
        NFS_EXTENT Current = Record->Extents[In];

        if (Current.Length == 0)
            continue;

        if (Out > 0)
        {
            NFS_EXTENT *Previous = &Record->Extents[Out - 1];
            int Mergeable = 0;

            if (Previous->Vcn + Previous->Length != Current.Vcn)
                Mergeable = 0;
            else if (Previous->Lcn == NFS_HOLE && Current.Lcn == NFS_HOLE)
                Mergeable = 1;
            else if (Previous->Lcn != NFS_HOLE && Current.Lcn != NFS_HOLE &&
                     Previous->Lcn + Previous->Length == Current.Lcn)
                Mergeable = 1;

            if (Mergeable)
            {
                Previous->Length += Current.Length;
                continue;
            }
        }

        Record->Extents[Out++] = Current;
    }

    Record->ExtentCount = Out;
    return NFS_OK;
}

static int
NfsBuildExtents(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Source,
                NFS_PIFT_RECORD *Record, nfs_u64 TotalVcns,
                nfs_u64 ReplaceFirst, nfs_u64 ReplaceLast, nfs_u64 NewBase)
{
    nfs_u64 Vcn;
    nfs_u64 AllocSize = 0;

    Record->ExtentCount = 0;

    for (Vcn = 0; Vcn < TotalVcns; Vcn++)
    {
        nfs_u64 Lcn;

        if (NewBase != NFS_HOLE && Vcn >= ReplaceFirst && Vcn <= ReplaceLast)
        {
            Lcn = NewBase + (Vcn - ReplaceFirst);
        }
        else
        {
            NfsMapClusterRecord(Volume, Source, Vcn, &Lcn);
        }

        if (Record->ExtentCount > 0)
        {
            NFS_EXTENT *Previous = &Record->Extents[Record->ExtentCount - 1];

            if (Previous->Vcn + Previous->Length == Vcn &&
                ((Previous->Lcn == NFS_HOLE && Lcn == NFS_HOLE) ||
                 (Previous->Lcn != NFS_HOLE && Lcn != NFS_HOLE &&
                  Previous->Lcn + Previous->Length == Lcn)))
            {
                Previous->Length++;
                if (Lcn != NFS_HOLE)
                    AllocSize += Volume->Super.ClusterSize;
                continue;
            }
        }

        if (Record->ExtentCount >= NFS_EXTENT_MAX)
            return NFS_EUNSUPPORTED;

        Record->Extents[Record->ExtentCount].Vcn = Vcn;
        Record->Extents[Record->ExtentCount].Lcn = Lcn;
        Record->Extents[Record->ExtentCount].Length = 1;
        Record->ExtentCount++;

        if (Lcn != NFS_HOLE)
            AllocSize += Volume->Super.ClusterSize;
    }

    Record->AllocSize = AllocSize;
    return NFS_OK;
}

static void
NfsFreeExtentRange(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record,
                   nfs_u64 First, nfs_u64 Last)
{
    nfs_u32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record->Extents[i];
        nfs_u64 ExtentEnd = Extent->Vcn + Extent->Length - 1;
        nfs_u64 OverlapFirst, OverlapLast;

        if (Extent->Lcn == NFS_HOLE || Extent->Length == 0)
            continue;

        OverlapFirst = Extent->Vcn > First ? Extent->Vcn : First;
        OverlapLast = ExtentEnd < Last ? ExtentEnd : Last;
        if (OverlapFirst > OverlapLast)
            continue;

        /* Only the overlapped clusters are replaced by this write: the parts of
           the extent outside the range stay mapped and must stay allocated. */
        NfsClusterFree(Volume, Extent->Lcn + (OverlapFirst - Extent->Vcn),
                       OverlapLast - OverlapFirst + 1);
    }
}

/*
 * Write Length bytes at Offset, with copy-on-write semantics: the clusters that
 * the write touches are never modified in place, they are allocated fresh, the
 * untouched bytes of a partially covered cluster are carried over, and the old
 * clusters are released once the new mapping is published. Holes are allowed
 * between extents, so a write past EOF creates a sparse gap.
 */
int
NfsWriteRange(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Record, nfs_u64 Offset,
              const void *Buffer, nfs_u32 Length, NFS_PIFT_RECORD *Updated)
{
    nfs_u64 ClusterSize = Volume->Super.ClusterSize;
    nfs_u64 OldSize = Record->Size;
    nfs_u64 NewSize;
    nfs_u64 FirstVcn, LastVcn, VcnCount, NewBase = NFS_HOLE;
    nfs_u8 *Staging = NULL;
    nfs_u32 i;
    int Status;

    *Updated = *Record;

    if (Length == 0)
        return NfsTruncateRecord(Volume, Record, Offset, Updated);

    NewSize = Offset + Length;
    if (NewSize < OldSize)
        NewSize = OldSize;

    FirstVcn = Offset / ClusterSize;
    LastVcn = (Offset + Length - 1) / ClusterSize;
    VcnCount = LastVcn - FirstVcn + 1;

    if (VcnCount > 4096)
        return NFS_EUNSUPPORTED;   /* v0.2: one write may span 16 MiB */

    Status = NfsClusterAlloc(Volume, VcnCount, &NewBase);
    if (Status != NFS_OK)
        return Status;

    Staging = (nfs_u8 *)NfsMemAlloc((size_t)(VcnCount * ClusterSize));
    if (!Staging)
    {
        NfsClusterFree(Volume, NewBase, VcnCount);
        return NFS_ENOSPC;
    }
    memset(Staging, 0, (size_t)(VcnCount * ClusterSize));

    for (i = 0; i < VcnCount; i++)
    {
        nfs_u64 Vcn = FirstVcn + i;
        nfs_u64 ClusterStart = Vcn * ClusterSize;
        nfs_u64 ClusterEnd = ClusterStart + ClusterSize;
        nfs_u64 WriteStart = (Offset > ClusterStart) ? Offset : ClusterStart;
        nfs_u64 WriteEnd = (Offset + Length < ClusterEnd) ? Offset + Length : ClusterEnd;
        nfs_u64 Lcn = NFS_HOLE;

        /* Carry over the bytes this cluster keeps from the old mapping. */
        if (WriteStart > ClusterStart || WriteEnd < ClusterEnd)
        {
            if (NfsMapClusterRecord(Volume, Record, Vcn, &Lcn) == NFS_OK && Lcn != NFS_HOLE)
            {
                Status = NfsDevRead(Volume, Lcn * ClusterSize, Staging + (size_t)(i * ClusterSize),
                                    (nfs_u32)ClusterSize);
                if (Status != NFS_OK)
                {
                    NfsMemFree(Staging);
                    NfsClusterFree(Volume, NewBase, VcnCount);
                    return Status;
                }
            }
        }

        if (WriteEnd > WriteStart)
        {
            nfs_u64 SourceOffset = WriteStart - Offset;

            memcpy(Staging + (size_t)(i * ClusterSize) + (size_t)(WriteStart - ClusterStart),
                   (const nfs_u8 *)Buffer + (size_t)SourceOffset,
                   (size_t)(WriteEnd - WriteStart));
        }
    }

    Status = NfsDevWrite(Volume, NewBase * ClusterSize, Staging,
                         (nfs_u32)(VcnCount * ClusterSize));
    NfsMemFree(Staging);
    if (Status != NFS_OK)
    {
        NfsClusterFree(Volume, NewBase, VcnCount);
        return Status;
    }

    /* Publish the mapping, then release the clusters it replaced. */
    {
        nfs_u64 TotalVcns = (NewSize + ClusterSize - 1) / ClusterSize;

        Status = NfsBuildExtents(Volume, Record, Updated, TotalVcns, FirstVcn, LastVcn, NewBase);
        if (Status != NFS_OK)
        {
            NfsClusterFree(Volume, NewBase, VcnCount);
            return Status;
        }
        NfsFreeExtentRange(Volume, Record, FirstVcn, LastVcn);
    }

    Updated->Size = NewSize;
    return NFS_OK;
}

int
NfsTruncateRecord(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Record, nfs_u64 NewSize,
                  NFS_PIFT_RECORD *Updated)
{
    nfs_u64 ClusterSize = Volume->Super.ClusterSize;
    nfs_u64 TotalVcns;
    nfs_u64 ReplaceFirst = 0, ReplaceLast = (nfs_u64)-1, NewBase = NFS_HOLE;
    nfs_u64 OldBoundaryLcn = NFS_HOLE;
    int ReplacingBoundary = 0;
    int Status;

    *Updated = *Record;
    if (NewSize == Record->Size)
        return NFS_OK;

    TotalVcns = (NewSize + ClusterSize - 1) / ClusterSize;

    /* Shrinking into the middle of a cluster: rewrite that cluster with a
       zeroed tail, copy-on-write, so a later growth cannot resurrect stale
       bytes. Reads of a hole must always be zeroes. */
    if (NewSize < Record->Size && NewSize > 0 && (NewSize % ClusterSize) != 0)
    {
        nfs_u64 Vcn = NewSize / ClusterSize;
        nfs_u64 Lcn = NFS_HOLE;

        if (NfsMapClusterRecord(Volume, Record, Vcn, &Lcn) == NFS_OK && Lcn != NFS_HOLE)
        {
            nfs_u8 *Buffer = (nfs_u8 *)NfsMemAlloc((size_t)ClusterSize);
            nfs_u64 NewCluster = 0;

            if (!Buffer)
                return NFS_ENOSPC;

            Status = NfsDevRead(Volume, Lcn * ClusterSize, Buffer, (nfs_u32)ClusterSize);
            if (Status == NFS_OK)
            {
                memset(Buffer + (size_t)(NewSize % ClusterSize), 0,
                       (size_t)(ClusterSize - (NewSize % ClusterSize)));

                Status = NfsClusterAlloc(Volume, 1, &NewCluster);
                if (Status == NFS_OK)
                {
                    Status = NfsDevWrite(Volume, NewCluster * ClusterSize, Buffer,
                                         (nfs_u32)ClusterSize);
                    if (Status != NFS_OK)
                        NfsClusterFree(Volume, NewCluster, 1);
                }
            }
            NfsMemFree(Buffer);

            if (Status != NFS_OK)
                return Status;

            OldBoundaryLcn = Lcn;
            ReplaceFirst = Vcn;
            ReplaceLast = Vcn;
            NewBase = NewCluster;
            ReplacingBoundary = 1;
        }
    }

    /* Release everything fully past the new end. */
    if (Record->Size > NewSize)
    {
        nfs_u32 i;

        for (i = 0; i < Record->ExtentCount; i++)
        {
            const NFS_EXTENT *Extent = &Record->Extents[i];
            nfs_u64 Keep, Drop;

            if (Extent->Lcn == NFS_HOLE || Extent->Length == 0)
                continue;
            if (Extent->Vcn + Extent->Length <= TotalVcns)
                continue;

            Keep = (TotalVcns > Extent->Vcn) ? TotalVcns - Extent->Vcn : 0;
            Drop = Extent->Length - Keep;
            if (Drop > 0)
                NfsClusterFree(Volume, Extent->Lcn + Keep, Drop);
        }
    }

    Status = NfsBuildExtents(Volume, Record, Updated, TotalVcns,
                             ReplaceFirst, ReplaceLast, NewBase);
    if (Status != NFS_OK)
    {
        if (ReplacingBoundary)
            NfsClusterFree(Volume, NewBase, 1);
        return Status;
    }

    if (ReplacingBoundary)
        NfsClusterFree(Volume, OldBoundaryLcn, 1);

    Updated->Size = NewSize;
    return NFS_OK;
}

int
NfsRead(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 Offset,
        void *Buffer, nfs_u32 Length, nfs_u32 *Read)
{
    NFS_PIFT_RECORD *Record;
    nfs_u8 *Out = (nfs_u8 *)Buffer;
    nfs_u32 Done = 0;
    nfs_u64 ClusterSize;

    if (!Volume || !Buffer)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Record->Flags & NFS_FLAG_DIR)
        return NFS_EINVAL;

    ClusterSize = Volume->Super.ClusterSize;

    while (Done < Length && Offset + Done < Record->Size)
    {
        nfs_u64 Position = Offset + Done;
        nfs_u64 Vcn = Position / ClusterSize;
        nfs_u32 InCluster = (nfs_u32)(Position % ClusterSize);
        nfs_u32 Chunk = (nfs_u32)(ClusterSize - InCluster);
        nfs_u64 Lcn = NFS_HOLE;
        int Status;

        if (Chunk > Length - Done)
            Chunk = Length - Done;
        if (Position + Chunk > Record->Size)
            Chunk = (nfs_u32)(Record->Size - Position);

        Status = NfsMapClusterRecord(Volume, Record, Vcn, &Lcn);
        if (Status != NFS_OK || Lcn == NFS_HOLE)
        {
            memset(Out + Done, 0, Chunk);   /* a hole reads as zeroes */
            Done += Chunk;
            continue;
        }

        Status = NfsDevRead(Volume, Lcn * ClusterSize + InCluster, Out + Done, Chunk);
        if (Status != NFS_OK)
            return Status;
        Done += Chunk;
    }

    if (Read)
        *Read = Done;
    return NFS_OK;
}

int
NfsWrite(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 Offset,
         const void *Buffer, nfs_u32 Length, nfs_u32 *Written)
{
    NFS_PIFT_RECORD *Record;
    NFS_PIFT_RECORD Updated;
    int Status;

    if (!Volume || (!Buffer && Length))
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Record->Flags & NFS_FLAG_DIR)
        return NFS_EINVAL;

    Status = NfsWriteRange(Volume, Record, Offset, Buffer, Length, &Updated);
    if (Status != NFS_OK)
        return Status;

    Status = NfsCommitRecords(Volume, &Updated, 1);
    if (Status != NFS_OK)
        return Status;

    if (Written)
        *Written = Length;
    return NFS_OK;
}

int
NfsTruncate(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 NewSize)
{
    NFS_PIFT_RECORD *Record;
    NFS_PIFT_RECORD Updated;
    int Status;

    if (!Volume)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Record->Flags & NFS_FLAG_DIR)
        return NFS_EINVAL;

    Status = NfsTruncateRecord(Volume, Record, NewSize, &Updated);
    if (Status != NFS_OK)
        return Status;
    return NfsCommitRecords(Volume, &Updated, 1);
}

/* ------------------------------------------------------------------ */
/* Name helpers                                                        */
/* ------------------------------------------------------------------ */

nfs_u32
NfsNameFromAscii(const char *Ascii, nfs_u16 *Out, nfs_u32 MaxUnits)
{
    nfs_u32 Length = 0;

    while (Ascii[Length] != 0 && Length < MaxUnits)
    {
        Out[Length] = (nfs_u16)(nfs_u8)Ascii[Length];
        Length++;
    }
    return Length;
}

int
NfsNameToAscii(const nfs_u16 *Name, nfs_u32 Length, char *Out, nfs_u32 MaxBytes)
{
    nfs_u32 i;

    if (Length + 1 > MaxBytes)
        return NFS_EINVAL;
    for (i = 0; i < Length; i++)
        Out[i] = (Name[i] < 0x80) ? (char)Name[i] : '?';
    Out[Length] = 0;
    return NFS_OK;
}
