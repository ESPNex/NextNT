/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_snapshot.c
 * PURPOSE:     Snapshots and the pin (refcount) set that keeps them alive
 * LICENSE:     GPL-3.0-or-later
 *
 * A snapshot is a pinned checkpoint generation: the root of a copy-on-write
 * tree the live file system is free to walk away from. Nothing is copied — that
 * tree is already immutable — so the only work is stopping the allocator from
 * reusing the blocks it owns.
 *
 * Ownership is tracked as intervals, not per cluster: a snapshot pins its tree
 * nodes (one cluster each) and the extent ranges of the records inside them, so
 * the RAM cost follows the number of extents, not the number of clusters. The
 * set is rebuilt at mount by walking every snapshot tree, which makes the pins
 * derived state: a crash cannot leave the volume with pins that disagree with
 * the trees that justify them.
 *
 * Rules:
 *   - NfsClusterFree gives back only the clusters no pin covers; the pin set
 *     stays untouched because the snapshot still owns them.
 *   - Releasing a snapshot drops its intervals; a cluster that falls to zero is
 *     freed only if the live file system does not map it either.
 */

#include "nextfs_internal.h"

#include <string.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

/* The snapshot entries are ordinary records in this directory of the root. */
#define NFS_SNAPSHOT_DIR ".snapshots"
#define NFS_PIN_WALK_MAX 65536

/* ------------------------------------------------------------------ */
/* Pin set                                                             */
/* ------------------------------------------------------------------ */

int
NfsPinInit(PNFS_VOLUME Volume)
{
    Volume->PinCapacity = 64;
    Volume->PinCount = 0;
    Volume->Pins = (PNFS_PIN)NfsMemAlloc((size_t)Volume->PinCapacity * sizeof(NFS_PIN));
    return Volume->Pins ? NFS_OK : NFS_ENOSPC;
}

void
NfsPinDestroy(PNFS_VOLUME Volume)
{
    NfsMemFree(Volume->Pins);
    Volume->Pins = NULL;
    Volume->PinCount = 0;
    Volume->PinCapacity = 0;
}

nfs_u32
NfsPinCoveredBy(PNFS_VOLUME Volume, nfs_u64 Cluster)
{
    nfs_u32 Low = 0, High = Volume->PinCount;

    while (Low < High)
    {
        nfs_u32 Mid = Low + (High - Low) / 2;

        if (Cluster < Volume->Pins[Mid].Start)
            High = Mid;
        else if (Cluster >= Volume->Pins[Mid].Start + Volume->Pins[Mid].Length)
            Low = Mid + 1;
        else
            return Volume->Pins[Mid].Count;
    }
    return 0;
}

/*
 * Add (Delta = +1) or drop (Delta = -1) one reference over [Start, Start+Length).
 * Pieces are split at both boundaries so the intervals never overlap; when a
 * piece loses its last reference, OnZero is told which clusters became free.
 */
static int
NfsPinModify(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length, int Delta,
             void (*OnZero)(void *Context, nfs_u64 Start, nfs_u64 Length),
             void *Context)
{
    nfs_u64 End = Start + Length;
    nfs_u32 First = 0, Last, i, Needed, Found = 0;
    nfs_u64 Cursor = Start;
    PNFS_PIN Out;

    if (Length == 0)
        return NFS_OK;

    while (First < Volume->PinCount &&
           Volume->Pins[First].Start + Volume->Pins[First].Length <= Start)
        First++;

    Last = First;
    while (Last < Volume->PinCount && Volume->Pins[Last].Start < End)
        Last++;

    Needed = Volume->PinCount + (Last - First) * 3 + 4;
    Out = (PNFS_PIN)NfsMemAlloc((size_t)Needed * sizeof(NFS_PIN));
    if (!Out)
        return NFS_ENOSPC;

    for (i = 0; i < First; i++)
        Out[Found++] = Volume->Pins[i];

    for (i = First; i < Last; i++)
    {
        nfs_u64 EStart = Volume->Pins[i].Start;
        nfs_u64 EEnd = EStart + Volume->Pins[i].Length;
        nfs_u32 Count = Volume->Pins[i].Count;
        nfs_u64 OverlapStart = (EStart > Start) ? EStart : Start;
        nfs_u64 OverlapEnd = (EEnd < End) ? EEnd : End;

        if (Delta > 0 && EStart > Cursor && Cursor < End)
        {
            Out[Found].Start = Cursor;
            Out[Found].Length = ((EStart < End) ? EStart : End) - Cursor;
            Out[Found].Count = 1;
            Out[Found].Pad = 0;
            Found++;
        }

        if (EStart < Start)
        {
            Out[Found].Start = EStart;
            Out[Found].Length = Start - EStart;
            Out[Found].Count = Count;
            Out[Found].Pad = 0;
            Found++;
        }

        if (OverlapEnd > OverlapStart)
        {
            if (Delta > 0)
            {
                Out[Found].Start = OverlapStart;
                Out[Found].Length = OverlapEnd - OverlapStart;
                Out[Found].Count = Count + 1;
                Out[Found].Pad = 0;
                Found++;
            }
            else if (Count > 1)
            {
                Out[Found].Start = OverlapStart;
                Out[Found].Length = OverlapEnd - OverlapStart;
                Out[Found].Count = Count - 1;
                Out[Found].Pad = 0;
                Found++;
            }
            else if (OnZero)
            {
                OnZero(Context, OverlapStart, OverlapEnd - OverlapStart);
            }
        }

        if (EEnd > End)
        {
            Out[Found].Start = End;
            Out[Found].Length = EEnd - End;
            Out[Found].Count = Count;
            Out[Found].Pad = 0;
            Found++;
        }

        if (EEnd > Cursor)
            Cursor = EEnd;
    }

    if (Delta > 0 && Cursor < End)
    {
        Out[Found].Start = Cursor;
        Out[Found].Length = End - Cursor;
        Out[Found].Count = 1;
        Out[Found].Pad = 0;
        Found++;
    }

    for (i = Last; i < Volume->PinCount; i++)
        Out[Found++] = Volume->Pins[i];

    /* Merge neighbours that carry the same count and touch. */
    {
        nfs_u32 Write = 0;

        for (i = 0; i < Found; i++)
        {
            if (Write > 0 &&
                Out[Write - 1].Start + Out[Write - 1].Length == Out[i].Start &&
                Out[Write - 1].Count == Out[i].Count)
            {
                Out[Write - 1].Length += Out[i].Length;
                continue;
            }
            Out[Write++] = Out[i];
        }
        Found = Write;
    }

    NfsMemFree(Volume->Pins);
    Volume->Pins = Out;
    Volume->PinCapacity = Needed;
    Volume->PinCount = Found;
    Volume->Stats.PinnedRanges = Found;
    return NFS_OK;
}

int
NfsPinAddRange(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length)
{
    return NfsPinModify(Volume, Start, Length, 1, NULL, NULL);
}

int
NfsPinSubRange(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length,
               void (*OnZero)(void *Context, nfs_u64 Start, nfs_u64 Length),
               void *Context)
{
    return NfsPinModify(Volume, Start, Length, -1, OnZero, Context);
}

/*
 * Free [Start, End) except the parts a snapshot still pins. This is what
 * NfsClusterFree calls, so no code path can free a cluster out from under a
 * snapshot.
 */
void
NfsPinFreeGaps(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length)
{
    nfs_u64 End = Start + Length;
    nfs_u64 Cursor = Start;
    nfs_u32 i, First = 0;

    if (Length == 0)
        return;

    if (!Volume->Pins || Volume->PinCount == 0)
    {
        NfsClusterMarkFree(Volume, Start, Length);
        return;
    }

    while (First < Volume->PinCount &&
           Volume->Pins[First].Start + Volume->Pins[First].Length <= Start)
        First++;

    for (i = First; i < Volume->PinCount && Volume->Pins[i].Start < End; i++)
    {
        nfs_u64 PinStart = Volume->Pins[i].Start;
        nfs_u64 PinEnd = PinStart + Volume->Pins[i].Length;

        if (PinStart > Cursor)
            NfsClusterMarkFree(Volume, Cursor,
                               ((PinStart < End) ? PinStart : End) - Cursor);
        if (PinEnd > Cursor)
            Cursor = PinEnd;
        if (Cursor >= End)
            break;
    }

    if (Cursor < End)
        NfsClusterMarkFree(Volume, Cursor, End - Cursor);
}

/* ------------------------------------------------------------------ */
/* Pinning a snapshot tree                                             */
/* ------------------------------------------------------------------ */

typedef struct _NFS_PIN_WALK
{
    PNFS_VOLUME Volume;
} NFS_PIN_WALK, *PNFS_PIN_WALK;

static int
NfsPinRecordExtents(void *Context, const NFS_PIFT_RECORD *Record)
{
    PNFS_PIN_WALK Walk = (PNFS_PIN_WALK)Context;
    nfs_u32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record->Extents[i];

        if (Extent->Lcn == NFS_HOLE || Extent->Length == 0)
            continue;
        if (NfsPinAddRange(Walk->Volume, Extent->Lcn, Extent->Length) != NFS_OK)
            return NFS_ENOSPC;
    }
    return NFS_OK;
}

static int
NfsSnapshotPinTree(PNFS_VOLUME Volume, nfs_u64 Root)
{
    nfs_u64 *Blocks;
    nfs_u32 Count = 0, i;
    NFS_PIN_WALK Walk;
    int Status;

    if (Root == 0)
        return NFS_OK;

    Blocks = (nfs_u64 *)NfsMemAlloc(NFS_PIN_WALK_MAX * sizeof(nfs_u64));
    if (!Blocks)
        return NFS_ENOSPC;

    Status = NfsTreeCollect(Volume, Root, Blocks, NFS_PIN_WALK_MAX, &Count);
    if (Status == NFS_OK)
    {
        for (i = 0; i < Count; i++)
        {
            Status = NfsPinAddRange(Volume, Blocks[i], 1);
            if (Status != NFS_OK)
                break;
        }
    }
    NfsMemFree(Blocks);
    if (Status != NFS_OK)
        return Status;

    Walk.Volume = Volume;
    return NfsTreeWalk(Volume, Root, NfsPinRecordExtents, &Walk);
}

/* ------------------------------------------------------------------ */
/* Releasing a snapshot tree                                           */
/* ------------------------------------------------------------------ */

typedef struct _NFS_RELEASE_CONTEXT
{
    PNFS_VOLUME Volume;
    nfs_u8 *Live;          /* 1 bit per cluster: the live system owns it */
} NFS_RELEASE_CONTEXT, *PNFS_RELEASE_CONTEXT;

static void
NfsLiveMark(PNFS_RELEASE_CONTEXT Release, nfs_u64 Start, nfs_u64 Length)
{
    nfs_u64 Cluster;

    for (Cluster = Start;
         Cluster < Start + Length && Cluster < Release->Volume->Super.TotalClusters;
         Cluster++)
        Release->Live[Cluster >> 3] |= (nfs_u8)(1u << (Cluster & 7));
}

static void
NfsReleaseCluster(void *Context, nfs_u64 Start, nfs_u64 Length)
{
    PNFS_RELEASE_CONTEXT Release = (PNFS_RELEASE_CONTEXT)Context;
    nfs_u64 Cluster;

    for (Cluster = Start; Cluster < Start + Length; Cluster++)
    {
        if ((Release->Live[Cluster >> 3] >> (Cluster & 7)) & 1)
            continue;                     /* the live file system still maps it */
        NfsClusterMarkFree(Release->Volume, Cluster, 1);
    }
}

static int
NfsReleaseRecordExtents(void *Context, const NFS_PIFT_RECORD *Record)
{
    PNFS_RELEASE_CONTEXT Release = (PNFS_RELEASE_CONTEXT)Context;
    nfs_u32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record->Extents[i];

        if (Extent->Lcn == NFS_HOLE || Extent->Length == 0)
            continue;
        if (NfsPinSubRange(Release->Volume, Extent->Lcn, Extent->Length,
                           NfsReleaseCluster, Release) != NFS_OK)
            return NFS_ENOSPC;
    }
    return NFS_OK;
}

static int
NfsReleaseMarkRecord(void *Context, const NFS_PIFT_RECORD *Record)
{
    PNFS_RELEASE_CONTEXT Release = (PNFS_RELEASE_CONTEXT)Context;
    nfs_u32 i;

    if (Record->Flags & NFS_FLAG_DELETED)
        return NFS_OK;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        if (Record->Extents[i].Lcn == NFS_HOLE)
            continue;
        NfsLiveMark(Release, Record->Extents[i].Lcn, Record->Extents[i].Length);
    }
    return NFS_OK;
}

static void
NfsSnapshotMarkLive(PNFS_VOLUME Volume, PNFS_RELEASE_CONTEXT Release)
{
    nfs_u32 r;

    /* Metadata regions and the superblock backup are never handed out. */
    NfsLiveMark(Release, 0,
                Volume->Super.PiftAreaFirst + Volume->Super.PiftAreaClusters);
    if (Volume->Super.TotalClusters)
        NfsLiveMark(Release, Volume->Super.TotalClusters - 1, 1);

    /* The live namespace in RAM. */
    for (r = 0; r < Volume->RecordCount; r++)
        NfsReleaseMarkRecord(Release, &Volume->Records[r]);

    /* Both live trees: their records and their own nodes. */
    NfsTreeWalk(Volume, Volume->Super.CheckpointRoot.Block, NfsReleaseMarkRecord,
                Release);
    NfsTreeWalk(Volume, Volume->Super.CheckpointRootPrev.Block, NfsReleaseMarkRecord,
                Release);

    {
        nfs_u64 *Nodes = (nfs_u64 *)NfsMemAlloc(NFS_PIN_WALK_MAX * sizeof(nfs_u64));
        nfs_u32 Count = 0, i;

        if (!Nodes)
            return;

        if (NfsTreeCollect(Volume, Volume->Super.CheckpointRoot.Block, Nodes,
                           NFS_PIN_WALK_MAX, &Count) == NFS_OK)
            for (i = 0; i < Count; i++)
                NfsLiveMark(Release, Nodes[i], 1);

        Count = 0;
        if (NfsTreeCollect(Volume, Volume->Super.CheckpointRootPrev.Block, Nodes,
                           NFS_PIN_WALK_MAX, &Count) == NFS_OK)
            for (i = 0; i < Count; i++)
                NfsLiveMark(Release, Nodes[i], 1);

        NfsMemFree(Nodes);
    }
}

static int
NfsSnapshotReleaseTree(PNFS_VOLUME Volume, nfs_u64 Root)
{
    nfs_u64 *Blocks;
    nfs_u32 Count = 0, i;
    NFS_RELEASE_CONTEXT Release;
    int Status;

    if (Root == 0)
        return NFS_OK;

    Release.Volume = Volume;
    Release.Live = (nfs_u8 *)NfsMemAlloc((size_t)Volume->BitmapBytes);
    if (!Release.Live)
        return NFS_ENOSPC;
    memset(Release.Live, 0, Volume->BitmapBytes);

    NfsSnapshotMarkLive(Volume, &Release);

    Blocks = (nfs_u64 *)NfsMemAlloc(NFS_PIN_WALK_MAX * sizeof(nfs_u64));
    if (!Blocks)
    {
        NfsMemFree(Release.Live);
        return NFS_ENOSPC;
    }

    Status = NfsTreeCollect(Volume, Root, Blocks, NFS_PIN_WALK_MAX, &Count);
    if (Status == NFS_OK)
    {
        for (i = 0; i < Count; i++)
        {
            Status = NfsPinSubRange(Volume, Blocks[i], 1, NfsReleaseCluster, &Release);
            if (Status != NFS_OK)
                break;
        }
    }
    NfsMemFree(Blocks);

    if (Status == NFS_OK)
        Status = NfsTreeWalk(Volume, Root, NfsReleaseRecordExtents, &Release);

    NfsMemFree(Release.Live);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Snapshot operations                                                 */
/* ------------------------------------------------------------------ */

static int
NfsSnapshotDir(PNFS_VOLUME Volume, int Create, nfs_fileid *OutDir)
{
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 Length = NfsNameFromAscii(NFS_SNAPSHOT_DIR, Name, NFS_NAME_MAX);
    NFS_PIFT_RECORD Record;

    if (NfsLookup(Volume, NFS_ROOT_ID, Name, Length, &Record) == NFS_OK)
    {
        if (OutDir)
            *OutDir = Record.FileId;
        return NFS_OK;
    }
    if (!Create)
        return NFS_ENOENT;

    return NfsCreate(Volume, NFS_ROOT_ID, Name, Length, 1, OutDir);
}

static int
NfsSnapshotFind(PNFS_VOLUME Volume, nfs_fileid SnapshotId, NFS_PIFT_RECORD *Out)
{
    NFS_PIFT_RECORD *Record = NfsIndexGet(Volume, SnapshotId);

    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (!(Record->Flags & NFS_FLAG_SNAPSHOT))
        return NFS_EINVAL;
    if (Out)
        *Out = *Record;
    return NFS_OK;
}

int
NfsSnapshotCreate(PNFS_VOLUME Volume, const nfs_u16 *Name, nfs_u32 NameLength,
                  nfs_fileid *OutId)
{
    NFS_PIFT_RECORD Record;
    nfs_fileid Directory = 0;
    nfs_u64 Root;
    int Status;

    if (!Volume || !Name || NameLength == 0 || NameLength > NFS_NAME_MAX)
        return NFS_EINVAL;

    Status = NfsSnapshotDir(Volume, 1, &Directory);
    if (Status != NFS_OK)
        return Status;

    if (NfsChildrenLookup(Volume, Directory, Name, NameLength))
        return NFS_EEXIST;

    /* The snapshot pins the last checkpoint: an immutable, published tree. */
    Status = NfsSync(Volume);
    if (Status != NFS_OK)
        return Status;

    Root = Volume->Super.CheckpointRoot.Block;
    if (Root == 0)
        return NFS_ECORRUPT;

    Status = NfsSnapshotPinTree(Volume, Root);
    if (Status != NFS_OK)
        return Status;

    memset(&Record, 0, sizeof(Record));
    Record.FileId = Volume->NextFileId++;
    Record.Generation = 1;
    Record.Parent = Directory;
    Record.Flags = NFS_FLAG_SNAPSHOT;
    Record.Attributes = 0x80;
    Record.NameLen = NameLength;
    memcpy(Record.Name, Name, (size_t)NameLength * sizeof(nfs_u16));
    Record.SnapshotRoot = Root;
    Record.SnapshotGeneration = Volume->Super.CheckpointRoot.Generation;

    Status = NfsCommitRecords(Volume, &Record, 1);
    if (Status != NFS_OK)
        return Status;

    Volume->Stats.Snapshots++;
    if (OutId)
        *OutId = Record.FileId;
    return NFS_OK;
}

int
NfsSnapshotDelete(PNFS_VOLUME Volume, nfs_fileid SnapshotId)
{
    NFS_PIFT_RECORD Record, Updated;
    int Status;

    Status = NfsSnapshotFind(Volume, SnapshotId, &Record);
    if (Status != NFS_OK)
        return Status;

    Status = NfsSnapshotReleaseTree(Volume, Record.SnapshotRoot);
    if (Status != NFS_OK)
        return Status;

    Updated = Record;
    Updated.Flags |= NFS_FLAG_DELETED;
    Updated.Generation++;

    Status = NfsCommitRecords(Volume, &Updated, 1);
    if (Status != NFS_OK)
        return Status;

    if (Volume->Stats.Snapshots > 0)
        Volume->Stats.Snapshots--;
    return NFS_OK;
}

int
NfsSnapshotList(PNFS_VOLUME Volume, nfs_u32 Index, NFS_SNAPSHOT_INFO *OutInfo)
{
    nfs_fileid Directory = 0;
    nfs_u32 Seen = 0, Child;
    int Status;

    if (!OutInfo)
        return NFS_EINVAL;

    Status = NfsSnapshotDir(Volume, 0, &Directory);
    if (Status != NFS_OK)
        return Status;

    for (Child = 0;; Child++)
    {
        NFS_PIFT_RECORD *Record = NfsChildrenAt(Volume, Directory, Child);

        if (!Record)
            break;
        if (!(Record->Flags & NFS_FLAG_SNAPSHOT) || (Record->Flags & NFS_FLAG_DELETED))
            continue;
        if (Seen++ != Index)
            continue;

        memset(OutInfo, 0, sizeof(*OutInfo));
        OutInfo->Id = Record->FileId;
        OutInfo->TreeRoot = Record->SnapshotRoot;
        OutInfo->Generation = Record->SnapshotGeneration;
        OutInfo->Created = Record->Mtime;
        OutInfo->NameLen = Record->NameLen;
        memcpy(OutInfo->Name, Record->Name, (size_t)Record->NameLen * sizeof(nfs_u16));
        return NFS_OK;
    }

    return NFS_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Reading a snapshot                                                  */
/* ------------------------------------------------------------------ */

typedef struct _NFS_SNAPSHOT_LOOKUP
{
    nfs_fileid Parent;
    const nfs_u16 *Name;
    nfs_u32 NameLength;
    nfs_fileid FileId;
    int ByName;
    int Found;
    NFS_PIFT_RECORD Record;
} NFS_SNAPSHOT_LOOKUP, *PNFS_SNAPSHOT_LOOKUP;

static nfs_u16
NfsFoldUnit(nfs_u16 Unit)
{
    if (Unit >= 'A' && Unit <= 'Z')
        return (nfs_u16)(Unit - 'A' + 'a');
    return Unit;
}

static int
NfsSnapshotLookupCallback(void *Context, const NFS_PIFT_RECORD *Record)
{
    PNFS_SNAPSHOT_LOOKUP Lookup = (PNFS_SNAPSHOT_LOOKUP)Context;
    nfs_u32 i;

    if (Lookup->Found || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_OK;

    if (Lookup->ByName)
    {
        if (Record->Parent != Lookup->Parent ||
            Record->NameLen != Lookup->NameLength)
            return NFS_OK;
        for (i = 0; i < Lookup->NameLength; i++)
            if (NfsFoldUnit(Record->Name[i]) != NfsFoldUnit(Lookup->Name[i]))
                return NFS_OK;
    }
    else if (Record->FileId != Lookup->FileId)
    {
        return NFS_OK;
    }

    Lookup->Record = *Record;
    Lookup->Found = 1;
    return NFS_OK;
}

static int
NfsSnapshotWalk(PNFS_VOLUME Volume, nfs_fileid SnapshotId, PNFS_SNAPSHOT_LOOKUP Lookup)
{
    NFS_PIFT_RECORD Snapshot;
    int Status;

    Status = NfsSnapshotFind(Volume, SnapshotId, &Snapshot);
    if (Status != NFS_OK)
        return Status;

    Status = NfsTreeWalk(Volume, Snapshot.SnapshotRoot, NfsSnapshotLookupCallback,
                         Lookup);
    if (Status != NFS_OK)
        return Status;
    return Lookup->Found ? NFS_OK : NFS_ENOENT;
}

int
NfsSnapshotLookup(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid Parent,
                  const nfs_u16 *Name, nfs_u32 NameLength,
                  NFS_PIFT_RECORD *OutRecord)
{
    NFS_SNAPSHOT_LOOKUP Lookup;
    int Status;

    if (!Volume || !Name || NameLength == 0)
        return NFS_EINVAL;

    memset(&Lookup, 0, sizeof(Lookup));
    Lookup.Parent = Parent;
    Lookup.Name = Name;
    Lookup.NameLength = NameLength;
    Lookup.ByName = 1;

    Status = NfsSnapshotWalk(Volume, SnapshotId, &Lookup);
    if (Status != NFS_OK)
        return Status;

    if (OutRecord)
        *OutRecord = Lookup.Record;
    return NFS_OK;
}

int
NfsSnapshotOpenById(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid FileId,
                    NFS_PIFT_RECORD *OutRecord)
{
    NFS_SNAPSHOT_LOOKUP Lookup;
    int Status;

    if (!Volume)
        return NFS_EINVAL;

    memset(&Lookup, 0, sizeof(Lookup));
    Lookup.FileId = FileId;

    Status = NfsSnapshotWalk(Volume, SnapshotId, &Lookup);
    if (Status != NFS_OK)
        return Status;

    if (OutRecord)
        *OutRecord = Lookup.Record;
    return NFS_OK;
}

int
NfsSnapshotRead(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid FileId,
                nfs_u64 Offset, void *Buffer, nfs_u32 Length, nfs_u32 *Read)
{
    NFS_PIFT_RECORD Record;
    nfs_u8 *Out = (nfs_u8 *)Buffer;
    nfs_u32 Done = 0;
    nfs_u64 ClusterSize;
    int Status;

    if (!Volume || !Buffer)
        return NFS_EINVAL;

    Status = NfsSnapshotOpenById(Volume, SnapshotId, FileId, &Record);
    if (Status != NFS_OK)
        return Status;
    if (Record.Flags & NFS_FLAG_DIR)
        return NFS_EINVAL;

    ClusterSize = Volume->Super.ClusterSize;

    while (Done < Length && Offset + Done < Record.Size)
    {
        nfs_u64 Position = Offset + Done;
        nfs_u64 Vcn = Position / ClusterSize;
        nfs_u32 InCluster = (nfs_u32)(Position % ClusterSize);
        nfs_u32 Chunk = (nfs_u32)(ClusterSize - InCluster);
        nfs_u64 Lcn = NFS_HOLE;

        if (Chunk > Length - Done)
            Chunk = Length - Done;
        if (Position + Chunk > Record.Size)
            Chunk = (nfs_u32)(Record.Size - Position);

        Status = NfsMapClusterRecord(Volume, &Record, Vcn, &Lcn);
        if (Status != NFS_OK || Lcn == NFS_HOLE)
        {
            memset(Out + Done, 0, Chunk);
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

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

int
NfsSnapshotPinsRebuild(PNFS_VOLUME Volume)
{
    nfs_u32 r;
    int Status;

    Volume->PinCount = 0;
    Volume->Stats.Snapshots = 0;

    for (r = 0; r < Volume->RecordCount; r++)
    {
        const NFS_PIFT_RECORD *Record = &Volume->Records[r];

        if (!(Record->Flags & NFS_FLAG_SNAPSHOT))
            continue;
        if (Record->Flags & NFS_FLAG_DELETED)
            continue;
        if (Record->SnapshotRoot == 0)
            continue;

        Status = NfsSnapshotPinTree(Volume, Record->SnapshotRoot);
        if (Status != NFS_OK)
            return Status;
        Volume->Stats.Snapshots++;
    }

    Volume->Stats.PinnedRanges = Volume->PinCount;
    return NFS_OK;
}
