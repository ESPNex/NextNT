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
/* ------------------------------------------------------------------ */

static nfs_u16
NfsFoldChar(nfs_u16 c)
{
    if (c >= 'A' && c <= 'Z')
        return (nfs_u16)(c - 'A' + 'a');
    return c;
}

static int
NfsNameEqual(const NFS_PIFT_RECORD *Record, const nfs_u16 *Name, nfs_u32 Length)
{
    nfs_u32 i;

    if (Record->NameLen != Length)
        return 0;
    for (i = 0; i < Length; i++)
    {
        if (NfsFoldChar(Record->Name[i]) != NfsFoldChar(Name[i]))
            return 0;
    }
    return 1;
}

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

    /* 4. Publish in RAM. */
    for (i = First; i <= Last; i++)
    {
        Records[i].Seq = Seq;
        Records[i].Mtime = NfsNow();
        if (NfsIndexInsert(Volume, &Records[i]) != NFS_OK)
            return NFS_ENOSPC;
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

typedef struct _NFS_CHAIN_BLOCK
{
    nfs_u64 Block;
    nfs_u32 StartRecord;
    nfs_u32 RecordCount;
} NFS_CHAIN_BLOCK;

int
NfsCheckpoint(PNFS_VOLUME Volume)
{
    NFS_CHAIN_BLOCK Blocks[256];
    nfs_u32 BlockCount = 0;
    nfs_u32 i, Cursor = 0, LiveCount = 0;
    nfs_u64 OldChain[512];
    nfs_u32 OldCount = 0;
    nfs_u64 Generation = Volume->Super.Generation + 1;
    int Status;

    /* 1. Walk the current chain so its blocks can be freed afterwards. */
    {
        nfs_u64 Block = Volume->Super.CheckpointRoot.Block;
        while (Block != 0 && OldCount < 512)
        {
            nfs_u8 Node[NFS_NODE_SIZE];
            NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
            OldChain[OldCount++] = Block;
            if (NfsDevRead(Volume, Block * NFS_CLUSTER_BYTES(Volume), Node, NFS_NODE_SIZE) != NFS_OK)
                break;
            if (memcmp(Header->Magic, NFS_NODE_MAGIC, 4) != 0)
                break;
            Block = Header->Next;
        }
    }

    /* 2. Plan the new chain: pack live records into NFS_NODE_SIZE nodes. */
    while (Cursor < Volume->RecordCount)
    {
        nfs_u32 Used = sizeof(NFS_NODE_HDR);
        nfs_u32 Start = Cursor;

        while (Cursor < Volume->RecordCount)
        {
            nfs_u64 Size = sizeof(nfs_u32) +
                           NfsPiftRecordSize(&Volume->Records[Cursor]);
            if (Volume->Records[Cursor].Flags & NFS_FLAG_DELETED)
            {
                Cursor++;
                continue;
            }
            if (Used + Size > NFS_NODE_SIZE)
            {
                if (Cursor == Start)
                {
                    /* A single record bigger than a node: still pack it. */
                    Used += (nfs_u32)Size;
                    Cursor++;
                }
                break;
            }
            Used += (nfs_u32)Size;
            Cursor++;
        }

        if (BlockCount >= 256)
            return NFS_ENOSPC;   /* v0.1: chain of at most 256 nodes */

        Blocks[BlockCount].Block = 0;
        Blocks[BlockCount].StartRecord = Start;
        Blocks[BlockCount].RecordCount = Cursor - Start;
        BlockCount++;

    }

    if (BlockCount == 0)
        return NFS_OK;

    /* 3. Allocate a block for each new node. */
    for (i = 0; i < BlockCount; i++)
    {
        Status = NfsClusterAlloc(Volume, 1, &Blocks[i].Block);
        if (Status != NFS_OK)
            return Status;
    }

    /* 4. Write the nodes. Next is known because all blocks are allocated. */
    for (i = 0; i < BlockCount; i++)
    {
        nfs_u8 Node[NFS_NODE_SIZE];
        NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
        nfs_u8 *Payload = Node + sizeof(NFS_NODE_HDR);
        nfs_u32 j, Used;

        memset(Node, 0, sizeof(Node));
        memcpy(Header->Magic, NFS_NODE_MAGIC, 4);
        Header->Level = 0;
        Header->Generation = Generation;
        Header->Block = Blocks[i].Block;
        Header->Next = (i + 1 < BlockCount) ? Blocks[i + 1].Block : 0;
        Header->DataOffset = sizeof(NFS_NODE_HDR);

        Used = 0;
        for (j = 0; j < Blocks[i].RecordCount; j++)
        {
            nfs_u32 Index = Blocks[i].StartRecord + j;
            nfs_u8 RecordBuffer[NFS_NAME_MAX * 2 + NFS_EXTENT_MAX * 24 + 128];
            nfs_u32 RecordLen;

            if (Volume->Records[Index].Flags & NFS_FLAG_DELETED)
                continue;

            RecordLen = NfsPiftSerialize(&Volume->Records[Index], RecordBuffer,
                                         sizeof(RecordBuffer));
            if (RecordLen == 0)
                continue;
            memcpy(Payload + Used, &RecordLen, sizeof(nfs_u32));
            Used += sizeof(nfs_u32);
            memcpy(Payload + Used, RecordBuffer, RecordLen);
            Used += RecordLen;
            LiveCount++;
        }

        Header->Entries = (nfs_u16)Blocks[i].RecordCount;
        Header->UsedBytes = (nfs_u16)Used;
        Header->Crc32c = 0;
        Header->Crc32c = NfsCrc32c(0, Node, NFS_NODE_SIZE);

        Status = NfsDevWrite(Volume, Blocks[i].Block * NFS_CLUSTER_BYTES(Volume),
                             Node, NFS_NODE_SIZE);
        if (Status != NFS_OK)
            return Status;
    }

    Status = NfsDevFlush(Volume);
    if (Status != NFS_OK)
        return Status;

    /* 5. Publish: a single superblock pair write swaps the root. */
    Volume->Super.CheckpointRoot.Block = Blocks[0].Block;
    Volume->Super.CheckpointRoot.Generation = Generation;
    Volume->Super.Generation = Generation;
    Volume->Super.PiftHead = 0;                     /* area is reset    */
    Volume->Super.JournalSeqFirst = Volume->Super.JournalSeqNext;
    NfsSetDirty(Volume, 0);

    Status = NfsWriteSuper(Volume);
    if (Status != NFS_OK)
        return Status;

    /* 6. Retire the old chain and persist the bitmap. */
    for (i = 0; i < OldCount; i++)
        NfsClusterFree(Volume, OldChain[i], 1);
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

    Status = NfsIndexInit(Volume);
    if (Status != NFS_OK)
        goto Fail;

    /* 1. Checkpoint chain: the last compacted state of the PIFT. */
    {
        nfs_u64 Block = Volume->Super.CheckpointRoot.Block;

        while (Block != 0)
        {
            nfs_u8 Node[NFS_NODE_SIZE];
            NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
            nfs_u32 Cursor;

            Status = NfsDevRead(Volume, Block * NFS_CLUSTER_BYTES(Volume), Node, NFS_NODE_SIZE);
            if (Status != NFS_OK)
                goto Fail;
            if (memcmp(Header->Magic, NFS_NODE_MAGIC, 4) != 0)
            {
                Status = NFS_ECORRUPT;
                goto Fail;
            }
            {
                nfs_u32 StoredCrc = Header->Crc32c;
                Header->Crc32c = 0;
                if (StoredCrc != NfsCrc32c(0, Node, NFS_NODE_SIZE))
                {
                    Status = NFS_ECORRUPT;
                    goto Fail;
                }
                Header->Crc32c = StoredCrc;
            }

            Cursor = Header->DataOffset;
            {
                nfs_u32 End = (nfs_u32)Header->DataOffset + Header->UsedBytes;

                while (Cursor + sizeof(nfs_u32) <= End)
                {
                    nfs_u32 RecordLen;
                    NFS_PIFT_RECORD Record;

                    memcpy(&RecordLen, Node + Cursor, sizeof(nfs_u32));
                    Cursor += sizeof(nfs_u32);
                    if (RecordLen > End - Cursor)
                        break;
                    if (NfsPiftDeserialize(Node + Cursor, RecordLen, &Record) == NFS_OK)
                    {
                        NFS_PIFT_RECORD *Existing = NfsIndexGet(Volume, Record.FileId);
                        if (!Existing || Existing->Seq <= Record.Seq)
                        {
                            if (NfsIndexInsert(Volume, &Record) != NFS_OK)
                            {
                                Status = NFS_ENOSPC;
                                goto Fail;
                            }
                        }
                    }
                    Cursor += RecordLen;
                }
            }

            Block = Header->Next;
        }
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

    NfsSetDirty(Volume, 1);
    NfsWriteSuper(Volume);

    *OutVolume = Volume;
    return NFS_OK;

Fail:
    NfsIndexDestroy(Volume);
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
    NfsIndexDestroy(Volume);
    NfsMemFree(Volume->Bitmap);
    NfsMemFree(Volume);
}

void
NfsUnmount(PNFS_VOLUME Volume)
{
    if (!Volume)
        return;
    NfsSync(Volume);
    NfsIndexDestroy(Volume);
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
    nfs_u32 i;
    int Status;

    if (!Volume || !Name || NameLength == 0 || NameLength > NFS_NAME_MAX)
        return NFS_EINVAL;

    ParentRecord = NfsIndexGet(Volume, Parent);
    if (!ParentRecord || !(ParentRecord->Flags & NFS_FLAG_DIR))
        return NFS_ENOENT;

    for (i = 0; i < Volume->RecordCount; i++)
    {
        if (Volume->Records[i].Flags & NFS_FLAG_DELETED)
            continue;
        if (Volume->Records[i].Parent == Parent &&
            NfsNameEqual(&Volume->Records[i], Name, NameLength))
            return NFS_EEXIST;
    }

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
    nfs_u32 i;

    if (!Volume)
        return NFS_EINVAL;

    for (i = 0; i < Volume->RecordCount; i++)
    {
        NFS_PIFT_RECORD *Record = &Volume->Records[i];

        if ((Record->Flags & NFS_FLAG_DELETED) || Record->Parent != Parent)
            continue;
        if (NfsNameEqual(Record, Name, NameLength))
        {
            if (OutRecord)
                *OutRecord = *Record;
            return NFS_OK;
        }
    }
    return NFS_ENOENT;
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

typedef struct _NFS_DIR_ENTRY
{
    nfs_u32 Index;
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 NameLen;
    nfs_u64 FileId;
} NFS_DIR_ENTRY;

static int
NfsDirEntryCompare(const void *A, const void *B)
{
    const NFS_DIR_ENTRY *Left = (const NFS_DIR_ENTRY *)A;
    const NFS_DIR_ENTRY *Right = (const NFS_DIR_ENTRY *)B;
    nfs_u32 i, Limit = Left->NameLen < Right->NameLen ? Left->NameLen : Right->NameLen;

    for (i = 0; i < Limit; i++)
    {
        nfs_u16 a = NfsFoldChar(Left->Name[i]);
        nfs_u16 b = NfsFoldChar(Right->Name[i]);
        if (a != b)
            return (a < b) ? -1 : 1;
    }
    if (Left->NameLen == Right->NameLen)
        return 0;
    return (Left->NameLen < Right->NameLen) ? -1 : 1;
}

int
NfsReadDir(PNFS_VOLUME Volume, nfs_fileid Directory, nfs_u32 Index,
           NFS_PIFT_RECORD *OutRecord)
{
    NFS_DIR_ENTRY *Entries;
    nfs_u32 Count = 0, i;
    int Status = NFS_ENOENT;

    if (!Volume || !OutRecord)
        return NFS_EINVAL;

    Entries = (NFS_DIR_ENTRY *)NfsMemAlloc((size_t)(Volume->RecordCount + 1) * sizeof(NFS_DIR_ENTRY));
    if (!Entries)
        return NFS_ENOSPC;

    for (i = 0; i < Volume->RecordCount; i++)
    {
        NFS_PIFT_RECORD *Record = &Volume->Records[i];

        if ((Record->Flags & NFS_FLAG_DELETED) || Record->Parent != Directory)
            continue;
        Entries[Count].Index = i;
        Entries[Count].NameLen = Record->NameLen;
        memcpy(Entries[Count].Name, Record->Name, (size_t)Record->NameLen * sizeof(nfs_u16));
        Entries[Count].FileId = Record->FileId;
        Count++;
    }

    if (Count)
        qsort(Entries, Count, sizeof(NFS_DIR_ENTRY), NfsDirEntryCompare);

    if (Index < Count)
    {
        *OutRecord = Volume->Records[Entries[Index].Index];
        Status = NFS_OK;
    }

    NfsMemFree(Entries);
    return Status;
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

    for (i = 0; i < Volume->RecordCount; i++)
    {
        if (!(Volume->Records[i].Flags & NFS_FLAG_DELETED) &&
            Volume->Records[i].Parent == FileId)
            return NFS_EINVAL;   /* v0.1: only empty directories */
    }

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

static int
NfsMapCluster(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record, nfs_u64 Vcn,
              nfs_u64 *Lcn)
{
    nfs_u32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record->Extents[i];

        if (Vcn >= Extent->Vcn && Vcn < Extent->Vcn + Extent->Length)
        {
            if (Extent->Lcn == NFS_HOLE)
                *Lcn = NFS_HOLE;
            else
                *Lcn = Extent->Lcn + (Vcn - Extent->Vcn);
            return NFS_OK;
        }
    }
    return NFS_ENOENT;
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
        nfs_u64 Lcn;
        int Status;

        if (Chunk > Length - Done)
            Chunk = Length - Done;
        if (Position + Chunk > Record->Size)
            Chunk = (nfs_u32)(Record->Size - Position);

        Status = NfsMapCluster(Volume, Record, Vcn, &Lcn);
        if (Status != NFS_OK)
        {
            memset(Out + Done, 0, Chunk);   /* hole reads as zeroes */
            Done += Chunk;
            continue;
        }
        if (Lcn == NFS_HOLE)
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

int
NfsWrite(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 Offset,
         const void *Buffer, nfs_u32 Length, nfs_u32 *Written)
{
    NFS_PIFT_RECORD *Record;
    NFS_PIFT_RECORD Updated;
    nfs_u64 ClusterSize;
    nfs_u32 i;
    int Status;

    if (!Volume || !Buffer)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Record->Flags & NFS_FLAG_DIR)
        return NFS_EINVAL;

    ClusterSize = Volume->Super.ClusterSize;
    Updated = *Record;

    if (Offset == 0)
    {
        /* Rewrite: allocate fresh clusters (copy-on-write), then publish. */
        nfs_u64 Clusters = (Length + ClusterSize - 1) / ClusterSize;
        nfs_u64 First = 0;
        nfs_u8 *Cluster = NULL;

        if (Clusters > 0)
        {
            Status = NfsClusterAlloc(Volume, Clusters, &First);
            if (Status != NFS_OK)
                return Status;

            Cluster = (nfs_u8 *)NfsMemAlloc((size_t)Clusters * ClusterSize);
            if (!Cluster)
            {
                NfsClusterFree(Volume, First, Clusters);
                return NFS_ENOSPC;
            }
            memset(Cluster, 0, (size_t)Clusters * ClusterSize);
            memcpy(Cluster, Buffer, Length);

            Status = NfsDevWrite(Volume, First * ClusterSize, Cluster,
                                 (nfs_u32)(Clusters * ClusterSize));
            NfsMemFree(Cluster);
            if (Status != NFS_OK)
            {
                NfsClusterFree(Volume, First, Clusters);
                return Status;
            }

            for (i = 0; i < Record->ExtentCount; i++)
            {
                if (Record->Extents[i].Lcn != NFS_HOLE)
                    NfsClusterFree(Volume, Record->Extents[i].Lcn,
                                   Record->Extents[i].Length);
            }

            Updated.ExtentCount = 1;
            Updated.Extents[0].Vcn = 0;
            Updated.Extents[0].Lcn = First;
            Updated.Extents[0].Length = Clusters;
            Updated.AllocSize = Clusters * ClusterSize;
        }
        else
        {
            for (i = 0; i < Record->ExtentCount; i++)
            {
                if (Record->Extents[i].Lcn != NFS_HOLE)
                    NfsClusterFree(Volume, Record->Extents[i].Lcn,
                                   Record->Extents[i].Length);
            }
            Updated.ExtentCount = 0;
            Updated.AllocSize = 0;
        }

        Updated.Size = Length;
    }
    else if (Offset == Record->Size)
    {
        /* Append. Aligned appends add an extent; unaligned ones patch the
           tail cluster in place (documented v0.1 limitation: no snapshot). */
        nfs_u64 Tail = Record->Size % ClusterSize;
        nfs_u32 Consumed = 0;

        if (Tail != 0)
        {
            nfs_u64 LastVcn = Record->Size / ClusterSize;
            nfs_u64 Lcn = NFS_HOLE;
            nfs_u32 Chunk = (nfs_u32)(ClusterSize - Tail);
            nfs_u8 *ClusterBuffer;

            Status = NfsMapCluster(Volume, Record, LastVcn, &Lcn);
            if (Status != NFS_OK || Lcn == NFS_HOLE)
                return NFS_EUNSUPPORTED;

            if (Chunk > Length)
                Chunk = Length;

            ClusterBuffer = (nfs_u8 *)NfsMemAlloc((size_t)ClusterSize);
            if (!ClusterBuffer)
                return NFS_ENOSPC;
            Status = NfsDevRead(Volume, Lcn * ClusterSize, ClusterBuffer, (nfs_u32)ClusterSize);
            if (Status == NFS_OK)
            {
                memcpy(ClusterBuffer + Tail, Buffer, Chunk);
                Status = NfsDevWrite(Volume, Lcn * ClusterSize, ClusterBuffer,
                                     (nfs_u32)ClusterSize);
            }
            NfsMemFree(ClusterBuffer);
            if (Status != NFS_OK)
                return Status;
            Consumed = Chunk;
        }

        if (Consumed < Length)
        {
            nfs_u32 Remaining = Length - Consumed;
            nfs_u64 Clusters = (Remaining + ClusterSize - 1) / ClusterSize;
            nfs_u64 First = 0;
            nfs_u8 *Cluster;

            Status = NfsClusterAlloc(Volume, Clusters, &First);
            if (Status != NFS_OK)
                return Status;

            Cluster = (nfs_u8 *)NfsMemAlloc((size_t)Clusters * ClusterSize);
            if (!Cluster)
            {
                NfsClusterFree(Volume, First, Clusters);
                return NFS_ENOSPC;
            }
            memset(Cluster, 0, (size_t)Clusters * ClusterSize);
            memcpy(Cluster, (const nfs_u8 *)Buffer + Consumed, Remaining);
            Status = NfsDevWrite(Volume, First * ClusterSize, Cluster,
                                 (nfs_u32)(Clusters * ClusterSize));
            NfsMemFree(Cluster);
            if (Status != NFS_OK)
            {
                NfsClusterFree(Volume, First, Clusters);
                return Status;
            }

            if (Updated.ExtentCount >= NFS_EXTENT_MAX)
            {
                NfsClusterFree(Volume, First, Clusters);
                return NFS_EUNSUPPORTED;   /* extent array full in v0.1 */
            }

            /* Start the new extent at the cluster after the last mapped one. */
            {
                nfs_u64 NextVcn = ((Record->Size + ClusterSize - 1) / ClusterSize);
                nfs_u32 LastIndex = Updated.ExtentCount;

                if (LastIndex > 0)
                {
                    NFS_EXTENT *Last = &Updated.Extents[LastIndex - 1];
                    if (Last->Lcn != NFS_HOLE &&
                        Last->Lcn + Last->Length == First &&
                        Last->Vcn + Last->Length == NextVcn)
                    {
                        Last->Length += Clusters;
                        Updated.AllocSize += Clusters * ClusterSize;
                        goto Commit;
                    }
                }

                Updated.Extents[LastIndex].Vcn = NextVcn;
                Updated.Extents[LastIndex].Lcn = First;
                Updated.Extents[LastIndex].Length = Clusters;
                Updated.ExtentCount = LastIndex + 1;
                Updated.AllocSize += Clusters * ClusterSize;
            }
        }

Commit:
        Updated.Size = Record->Size + Length;
    }
    else
    {
        /* Middle-of-file writes need extent splitting: phase 2. */
        return NFS_EUNSUPPORTED;
    }

    Status = NfsCommitRecords(Volume, &Updated, 1);
    if (Status != NFS_OK)
        return Status;

    if (Written)
        *Written = Length;
    return NFS_OK;
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
