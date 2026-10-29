/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_tree.c
 * PURPOSE:     Copy-on-write B+tree for the PIFT checkpoint store
 * LICENSE:     GPL-3.0-or-later
 *
 * Layout (see DESIGN.md 1.2):
 *   level 0        leaf: packed PIFT records, 4-byte length prefix each
 *   level > 0      internal: Entries x { u64 KeyMax, u64 ChildBlock }
 *
 * KeyMax of a child is the largest FileId under it, so a search can pick the
 * first child whose KeyMax >= FileId. Every node carries its own Generation and
 * a CRC32C; a block whose generation does not match the root reference is stale
 * and is ignored, which is what makes the copy-on-write swap atomic.
 */

#include "nextfs_internal.h"

#include <string.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

#define NFS_CLUSTER_BYTES(Volume) ((Volume)->Super.ClusterSize)

#define NFS_TREE_MAX_DEPTH 8
#define NFS_INTERNAL_ENTRY_SIZE ((nfs_u32)(sizeof(nfs_u64) * 2))
#define NFS_INTERNAL_MAX_ENTRIES \
    ((NFS_NODE_SIZE - sizeof(NFS_NODE_HDR)) / NFS_INTERNAL_ENTRY_SIZE)

typedef struct _NFS_TREE_NODE
{
    nfs_u64 Block;
    nfs_u64 KeyMax;
} NFS_TREE_NODE, *PNFS_TREE_NODE;

static nfs_u32
NfsNodeCrc(const nfs_u8 *Node)
{
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
    nfs_u32 Stored = Header->Crc32c;
    nfs_u32 Computed;

    Header->Crc32c = 0;
    Computed = NfsCrc32c(0, Node, NFS_NODE_SIZE);
    Header->Crc32c = Stored;
    return Computed;
}

static int
NfsNodeRead(PNFS_VOLUME Volume, nfs_u64 Block, nfs_u8 *Node)
{
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
    int Status;

    Status = NfsDevRead(Volume, Block * NFS_CLUSTER_BYTES(Volume), Node, NFS_NODE_SIZE);
    if (Status != NFS_OK)
        return Status;
    if (memcmp(Header->Magic, NFS_NODE_MAGIC, 4) != 0)
        return NFS_ECORRUPT;
    if (Header->Block != Block)
        return NFS_ECORRUPT;
    if (NfsNodeCrc(Node) != Header->Crc32c)
        return NFS_ECORRUPT;
    if (Header->Level > NFS_TREE_MAX_DEPTH)
        return NFS_ECORRUPT;
    return NFS_OK;
}

static int
NfsNodeWrite(PNFS_VOLUME Volume, nfs_u64 Block, nfs_u16 Level, nfs_u16 Entries,
             const nfs_u8 *Payload, nfs_u32 PayloadLen, nfs_u64 Generation)
{
    nfs_u8 Node[NFS_NODE_SIZE];
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;

    memset(Node, 0, sizeof(Node));
    memcpy(Header->Magic, NFS_NODE_MAGIC, 4);
    Header->Level = Level;
    Header->Entries = Entries;
    Header->Generation = Generation;
    Header->Block = Block;
    Header->Next = 0;
    Header->DataOffset = (nfs_u16)sizeof(NFS_NODE_HDR);
    Header->UsedBytes = (nfs_u16)PayloadLen;

    if (PayloadLen)
        memcpy(Node + sizeof(NFS_NODE_HDR), Payload, PayloadLen);

    Header->Crc32c = 0;
    Header->Crc32c = NfsCrc32c(0, Node, NFS_NODE_SIZE);

    return NfsDevWrite(Volume, Block * NFS_CLUSTER_BYTES(Volume), Node, NFS_NODE_SIZE);
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

int
NfsTreeBuild(PNFS_VOLUME Volume, nfs_u64 Generation, nfs_u64 *OutRoot)
{
    NFS_TREE_NODE *Level = NULL;
    nfs_u32 LevelCount = 0, LevelCapacity = 0;
    nfs_u32 Cursor = 0;
    int Status = NFS_OK;

    *OutRoot = 0;

    /* Leaves: pack the live records. */
    while (Cursor < Volume->RecordCount)
    {
        nfs_u8 Payload[NFS_NODE_SIZE - sizeof(NFS_NODE_HDR)];
        nfs_u32 Used = 0, Start = Cursor;
        nfs_u64 Block = 0;
        nfs_u64 KeyMax = 0;
        nfs_u16 Entries = 0;
        int SawRecord = 0;

        while (Cursor < Volume->RecordCount)
        {
            nfs_u64 Size;
            nfs_u8 RecordBuffer[NFS_NAME_MAX * 2 + NFS_EXTENT_MAX * 24 + 128];
            nfs_u32 RecordLen;

            if (Volume->Records[Cursor].Flags & NFS_FLAG_DELETED)
            {
                Cursor++;
                continue;
            }

            RecordLen = NfsPiftSerialize(&Volume->Records[Cursor], RecordBuffer,
                                         sizeof(RecordBuffer));
            if (RecordLen == 0)
            {
                Cursor++;
                continue;
            }

            Size = sizeof(nfs_u32) + RecordLen;
            if (Used + Size > sizeof(Payload) && SawRecord)
                break;

            memcpy(Payload + Used, &RecordLen, sizeof(nfs_u32));
            Used += sizeof(nfs_u32);
            memcpy(Payload + Used, RecordBuffer, RecordLen);
            Used += RecordLen;
            Entries++;
            SawRecord = 1;
            if (Volume->Records[Cursor].FileId > KeyMax)
                KeyMax = Volume->Records[Cursor].FileId;
            Cursor++;
        }

        if (!SawRecord)
            continue;
        (void)Start;

        Status = NfsClusterAlloc(Volume, 1, &Block);
        if (Status != NFS_OK)
            goto Fail;

        Status = NfsNodeWrite(Volume, Block, 0, Entries, Payload, Used, Generation);
        if (Status != NFS_OK)
            goto Fail;

        if (LevelCount == LevelCapacity)
        {
            nfs_u32 NewCapacity = LevelCapacity ? LevelCapacity * 2 : 32;
            NFS_TREE_NODE *NewLevel =
                (NFS_TREE_NODE *)NfsMemAlloc((size_t)NewCapacity * sizeof(NFS_TREE_NODE));

            if (!NewLevel)
            {
                Status = NFS_ENOSPC;
                goto Fail;
            }
            if (Level)
            {
                memcpy(NewLevel, Level, (size_t)LevelCount * sizeof(NFS_TREE_NODE));
                NfsMemFree(Level);
            }
            Level = NewLevel;
            LevelCapacity = NewCapacity;
        }

        Level[LevelCount].Block = Block;
        Level[LevelCount].KeyMax = KeyMax;
        LevelCount++;
    }

    if (LevelCount == 0)
    {
        /* An empty volume still needs a root: an empty leaf. */
        nfs_u64 Block = 0;

        Status = NfsClusterAlloc(Volume, 1, &Block);
        if (Status != NFS_OK)
            goto Fail;
        Status = NfsNodeWrite(Volume, Block, 0, 0, NULL, 0, Generation);
        if (Status != NFS_OK)
            goto Fail;
        *OutRoot = Block;
        goto Done;
    }

    /* Internal levels until a single node is left. */
    {
        nfs_u16 LevelNumber = 1;

        while (LevelCount > 1)
        {
            NFS_TREE_NODE *Parent = NULL;
            nfs_u32 ParentCount = 0, ParentCapacity = 0, Index = 0;

            while (Index < LevelCount)
            {
                nfs_u8 Payload[NFS_NODE_SIZE - sizeof(NFS_NODE_HDR)];
                nfs_u32 Used = 0;
                nfs_u16 Entries = 0;
                nfs_u64 Block = 0, KeyMax = 0;

                while (Index < LevelCount && Entries < NFS_INTERNAL_MAX_ENTRIES)
                {
                    memcpy(Payload + Used, &Level[Index].KeyMax, sizeof(nfs_u64));
                    Used += sizeof(nfs_u64);
                    memcpy(Payload + Used, &Level[Index].Block, sizeof(nfs_u64));
                    Used += sizeof(nfs_u64);
                    Entries++;
                    KeyMax = Level[Index].KeyMax;
                    Index++;
                }

                Status = NfsClusterAlloc(Volume, 1, &Block);
                if (Status != NFS_OK)
                {
                    NfsMemFree(Parent);
                    goto Fail;
                }

                Status = NfsNodeWrite(Volume, Block, LevelNumber, Entries, Payload, Used,
                                      Generation);
                if (Status != NFS_OK)
                {
                    NfsMemFree(Parent);
                    goto Fail;
                }

                if (ParentCount == ParentCapacity)
                {
                    nfs_u32 NewCapacity = ParentCapacity ? ParentCapacity * 2 : 16;
                    NFS_TREE_NODE *NewParent =
                        (NFS_TREE_NODE *)NfsMemAlloc((size_t)NewCapacity * sizeof(NFS_TREE_NODE));

                    if (!NewParent)
                    {
                        NfsMemFree(Parent);
                        Status = NFS_ENOSPC;
                        goto Fail;
                    }
                    if (Parent)
                    {
                        memcpy(NewParent, Parent, (size_t)ParentCount * sizeof(NFS_TREE_NODE));
                        NfsMemFree(Parent);
                    }
                    Parent = NewParent;
                    ParentCapacity = NewCapacity;
                }

                Parent[ParentCount].Block = Block;
                Parent[ParentCount].KeyMax = KeyMax;
                ParentCount++;
            }

            NfsMemFree(Level);
            Level = Parent;
            LevelCount = ParentCount;
            LevelCapacity = ParentCapacity;
            LevelNumber++;
        }
    }

    *OutRoot = Level[0].Block;
    Status = NFS_OK;

Done:
Fail:
    NfsMemFree(Level);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Walk (mount) and collect (to free an old tree)                      */
/* ------------------------------------------------------------------ */

static int
NfsTreeWalkNode(PNFS_VOLUME Volume, nfs_u64 Block, nfs_u32 Depth,
                int (*Callback)(void *Context, const NFS_PIFT_RECORD *Record),
                void *Context)
{
    nfs_u8 Node[NFS_NODE_SIZE];
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
    int Status;

    if (Depth > NFS_TREE_MAX_DEPTH)
        return NFS_ECORRUPT;

    Status = NfsNodeRead(Volume, Block, Node);
    if (Status != NFS_OK)
        return Status;

    if (Header->Level == 0)
    {
        nfs_u32 Cursor = Header->DataOffset;
        nfs_u32 End = (nfs_u32)Header->DataOffset + Header->UsedBytes;

        while (Cursor + sizeof(nfs_u32) <= End)
        {
            nfs_u32 RecordLen;
            NFS_PIFT_RECORD Record;

            memcpy(&RecordLen, Node + Cursor, sizeof(nfs_u32));
            Cursor += sizeof(nfs_u32);
            if (RecordLen > End - Cursor)
                return NFS_ECORRUPT;

            if (NfsPiftDeserialize(Node + Cursor, RecordLen, &Record) == NFS_OK)
            {
                if (Callback)
                {
                    Status = Callback(Context, &Record);
                    if (Status != NFS_OK)
                        return Status;
                }
            }
            Cursor += RecordLen;
        }
        return NFS_OK;
    }

    {
        nfs_u32 Cursor = Header->DataOffset;
        nfs_u32 Entry;

        for (Entry = 0; Entry < Header->Entries; Entry++)
        {
            nfs_u64 Child;
            nfs_u64 KeyMax;

            memcpy(&KeyMax, Node + Cursor, sizeof(nfs_u64));
            Cursor += sizeof(nfs_u64);
            memcpy(&Child, Node + Cursor, sizeof(nfs_u64));
            Cursor += sizeof(nfs_u64);
            (void)KeyMax;

            Status = NfsTreeWalkNode(Volume, Child, Depth + 1, Callback, Context);
            if (Status != NFS_OK)
                return Status;
        }
    }

    return NFS_OK;
}

int
NfsTreeWalk(PNFS_VOLUME Volume, nfs_u64 Root,
            int (*Callback)(void *Context, const NFS_PIFT_RECORD *Record),
            void *Context)
{
    if (Root == 0)
        return NFS_OK;
    return NfsTreeWalkNode(Volume, Root, 0, Callback, Context);
}

static int
NfsTreeCollectNode(PNFS_VOLUME Volume, nfs_u64 Block, nfs_u32 Depth,
                   nfs_u64 *Blocks, nfs_u32 Max, nfs_u32 *Count)
{
    nfs_u8 Node[NFS_NODE_SIZE];
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
    int Status;

    if (Depth > NFS_TREE_MAX_DEPTH || Block == 0)
        return NFS_OK;

    if (*Count >= Max)
        return NFS_ENOSPC;

    Blocks[(*Count)++] = Block;

    Status = NfsNodeRead(Volume, Block, Node);
    if (Status != NFS_OK)
        return Status;

    if (Header->Level > 0)
    {
        nfs_u32 Cursor = Header->DataOffset;
        nfs_u32 Entry;

        for (Entry = 0; Entry < Header->Entries; Entry++)
        {
            nfs_u64 Child;

            Cursor += sizeof(nfs_u64);
            memcpy(&Child, Node + Cursor, sizeof(nfs_u64));
            Cursor += sizeof(nfs_u64);

            Status = NfsTreeCollectNode(Volume, Child, Depth + 1, Blocks, Max, Count);
            if (Status != NFS_OK)
                return Status;
        }
    }

    return NFS_OK;
}

int
NfsTreeCollect(PNFS_VOLUME Volume, nfs_u64 Root, nfs_u64 *Blocks, nfs_u32 Max,
               nfs_u32 *Count)
{
    *Count = 0;
    if (Root == 0)
        return NFS_OK;
    return NfsTreeCollectNode(Volume, Root, 0, Blocks, Max, Count);
}
