/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/lib/fs/nextfs_core.c
 * PURPOSE:     Read-only NextFS reader: superblock, checkpoint tree, extents
 * LICENSE:     GPL-3.0-or-later
 *
 * This is the part of the boot path that has to be right, so it is written to do
 * exactly one thing: turn a byte source into a name -> record lookup and a
 * record -> bytes read. No writes, no journal, no allocation of its own -- the
 * caller passes a read callback and two allocator functions, which is what makes
 * the same file compile in the loader (FrLdr heap) and in the host test that
 * checks it against an image built by nextformat.
 *
 * The three failure modes it is built around:
 *
 *   1. **A torn node.** Every node is CRC32C'd over its whole 4 KiB, carries its
 *      own block number, and is only trusted when both match. A leaf with one bad
 *      record is not fatal: the record is skipped, because the alternative is
 *      refusing to boot over an unrelated file.
 *   2. **A newest generation that does not verify.** The superblock keeps the
 *      previous checkpoint root for exactly this: if the live one fails, the
 *      reader walks the previous one and says so, instead of giving up.
 *   3. **A volume that was not shut down.** The tree is the state as of the last
 *      checkpoint. Anything newer lives in the PIFT area and the journal, and
 *      this reader opens neither -- see nextfs.h for why.
 */

/* The ARC prototypes live in the same header, so the loader's types have to be
   visible before it; a host build defines NEXTFS_BOOT_NO_ARC and needs neither. */
#ifndef NEXTFS_BOOT_NO_ARC
#include <nextldr.h>
#endif

#include <fs/nextfs.h>

/* ------------------------------------------------------------------ */
/* CRC32C (Castagnoli), the polynomial the whole format uses           */
/* ------------------------------------------------------------------ */

static NFSB_U32 NfsbCrcTable[256];
static NFSB_U8 NfsbCrcReady;

static void
NfsbCrcInit(void)
{
    NFSB_U32 i, j, v;
    const NFSB_U32 Poly = 0x82F63B78u;   /* bit-reflected 0x1EDC6F41 */

    for (i = 0; i < 256; i++)
    {
        v = i;
        for (j = 0; j < 8; j++)
            v = (v >> 1) ^ ((v & 1) ? Poly : 0);
        NfsbCrcTable[i] = v;
    }
    NfsbCrcReady = 1;
}

NFSB_U32
NFSB_Crc32c(NFSB_U32 Seed, const void *Buffer, NFSB_U32 Length)
{
    const NFSB_U8 *p = (const NFSB_U8 *)Buffer;
    NFSB_U32 crc = ~Seed;

    if (!NfsbCrcReady)
        NfsbCrcInit();

    while (Length--)
        crc = NfsbCrcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

    return ~crc;
}

/* ------------------------------------------------------------------ */
/* Arena: chunked, so a pointer handed out stays valid for the mount.  */
/* ------------------------------------------------------------------ */

#define NFSB_CHUNK_MIN 4096u

typedef struct _NFSB_CHUNK
{
    struct _NFSB_CHUNK *Next;
    NFSB_U32 Used;
    NFSB_U32 Capacity;
    NFSB_U8 Data[1];
} NFSB_CHUNK;

static void *
NfsbArenaAlloc(NFSB_VOLUME *Volume, NFSB_U32 Size)
{
    NFSB_CHUNK *Chunk = (NFSB_CHUNK *)Volume->ArenaCurrent;

    Size = (Size + 7u) & ~7u;            /* keep everything 8-byte aligned */

    if (!Chunk || Chunk->Capacity - Chunk->Used < Size)
    {
        NFSB_U32 Capacity = (Size > NFSB_CHUNK_MIN) ? Size : NFSB_CHUNK_MIN;
        NFSB_CHUNK *New;

        New = (NFSB_CHUNK *)Volume->Heap.Alloc((NFSB_U32)(sizeof(NFSB_CHUNK) + Capacity));
        if (!New)
            return 0;
        New->Next = Chunk;
        New->Used = 0;
        New->Capacity = Capacity;
        Volume->ArenaCurrent = New;
        Volume->ArenaHead = New;
        Chunk = New;
    }

    {
        void *Result = Chunk->Data + Chunk->Used;
        Chunk->Used += Size;
        return Result;
    }
}

static void
NfsbArenaDestroy(NFSB_VOLUME *Volume)
{
    NFSB_CHUNK *Chunk = (NFSB_CHUNK *)Volume->ArenaHead;

    while (Chunk)
    {
        NFSB_CHUNK *Next = Chunk->Next;
        Volume->Heap.Free(Chunk);
        Chunk = Next;
    }
    Volume->ArenaHead = 0;
    Volume->ArenaCurrent = 0;
}

/* ------------------------------------------------------------------ */
/* Device and node helpers                                             */
/* ------------------------------------------------------------------ */

static int
NfsbReadDevice(NFSB_VOLUME *Volume, NFSB_U64 Offset, void *Buffer, NFSB_U32 Length)
{
    return Volume->Io.Read(Volume->Io.Context, Offset, Buffer, Length);
}

/* CRC of a node with its own Crc32c field taken as zero. */
static NFSB_U32
NfsbNodeCrc(NFSB_NODE_HDR *Header, NFSB_U8 *Node)
{
    NFSB_U32 Stored = Header->Crc32c;
    NFSB_U32 Computed;

    Header->Crc32c = 0;
    Computed = NFSB_Crc32c(0, Node, NFSB_NODE_SIZE);
    Header->Crc32c = Stored;
    return Computed;
}

static int
NfsbReadNode(NFSB_VOLUME *Volume, NFSB_U64 Block, NFSB_U8 *Node)
{
    NFSB_NODE_HDR *Header = (NFSB_NODE_HDR *)Node;
    int Status;

    if (Block == 0 || Block >= Volume->Super.TotalClusters)
        return NFSB_ECORRUPT;

    Status = NfsbReadDevice(Volume, Block * (NFSB_U64)Volume->Super.ClusterSize,
                            Node, NFSB_NODE_SIZE);
    if (Status != NFSB_OK)
        return Status;

    if (Header->Magic[0] != NFSB_NODE_MAGIC[0] || Header->Magic[1] != NFSB_NODE_MAGIC[1] ||
        Header->Magic[2] != NFSB_NODE_MAGIC[2] || Header->Magic[3] != NFSB_NODE_MAGIC[3])
        return NFSB_ECORRUPT;
    if (Header->Block != Block)
        return NFSB_ECORRUPT;            /* a node that names another block is a copy */
    if (Header->Level > NFSB_TREE_MAX_DEPTH)
        return NFSB_ECORRUPT;
    if (Header->DataOffset < sizeof(NFSB_NODE_HDR) ||
        (NFSB_U32)Header->DataOffset + Header->UsedBytes > NFSB_NODE_SIZE)
        return NFSB_ECORRUPT;
    if (NfsbNodeCrc(Header, Node) != Header->Crc32c)
        return NFSB_ECORRUPT;

    return NFSB_OK;
}

/* ------------------------------------------------------------------ */
/* The record index being built                                        */
/* ------------------------------------------------------------------ */

static NFSB_U32
NfsbFileIdHash(NFSB_U64 FileId)
{
    NFSB_U64 x = FileId * 0x9E3779B97F4A7C15ull;

    x ^= x >> 29;
    return (NFSB_U32)(x ^ (x >> 32));
}

static void
NfsbRehash(NFSB_VOLUME *Volume, NFSB_U32 NewSize)
{
    NFSB_U32 *Hash, i;

    Hash = (NFSB_U32 *)Volume->Heap.Alloc(NewSize * sizeof(NFSB_U32));
    if (!Hash)
        return;                          /* dedup degrades, lookups still work */

    for (i = 0; i < NewSize; i++)
        Hash[i] = 0xFFFFFFFFu;

    for (i = 0; i < Volume->RecordCount; i++)
    {
        NFSB_U32 Slot = NfsbFileIdHash(Volume->Records[i].FileId) & (NewSize - 1);

        while (Hash[Slot] != 0xFFFFFFFFu)
            Slot = (Slot + 1) & (NewSize - 1);
        Hash[Slot] = i;
    }

    if (Volume->Hash)
        Volume->Heap.Free(Volume->Hash);
    Volume->Hash = Hash;
    Volume->HashSize = NewSize;
}

/* Index of an existing record with this file id, or -1. */
static int
NfsbFindRecord(NFSB_VOLUME *Volume, NFSB_U64 FileId)
{
    NFSB_U32 Slot;

    if (!Volume->Hash)
        return -1;

    Slot = NfsbFileIdHash(FileId) & (Volume->HashSize - 1);
    while (Volume->Hash[Slot] != 0xFFFFFFFFu)
    {
        NFSB_U32 Index = Volume->Hash[Slot];

        if (Volume->Records[Index].FileId == FileId)
            return (int)Index;
        Slot = (Slot + 1) & (Volume->HashSize - 1);
    }
    return -1;
}

static int
NfsbAppendRecord(NFSB_VOLUME *Volume, NFSB_RECORD *Record)
{
    int Existing;

    /* The tree can hold more than one version of a record; the one with the
       highest sequence number is the one the filesystem would have used. */
    Existing = NfsbFindRecord(Volume, Record->FileId);
    if (Existing >= 0)
    {
        if (Volume->Records[Existing].Seq >= Record->Seq)
            return NFSB_OK;
        Volume->Records[Existing] = *Record;
        return NFSB_OK;
    }

    if (Volume->RecordCount == Volume->RecordCapacity)
    {
        NFSB_U32 Capacity = Volume->RecordCapacity ? Volume->RecordCapacity * 2 : 64;
        NFSB_RECORD *Records;

        Records = (NFSB_RECORD *)Volume->Heap.Alloc(Capacity * (NFSB_U32)sizeof(NFSB_RECORD));
        if (!Records)
            return NFSB_ENOMEM;
        if (Volume->Records)
        {
            NFSB_U32 i;

            for (i = 0; i < Volume->RecordCount; i++)
                Records[i] = Volume->Records[i];
            Volume->Heap.Free(Volume->Records);
        }
        Volume->Records = Records;
        Volume->RecordCapacity = Capacity;
    }

    Volume->Records[Volume->RecordCount] = *Record;
    Volume->RecordCount++;

    if (!Volume->Hash || Volume->RecordCount * 4 >= Volume->HashSize * 3)
        NfsbRehash(Volume, Volume->HashSize ? Volume->HashSize * 2 : 128);

    return NFSB_OK;
}

/* Decode one serialised record and copy its variable tail into the arena. */
static int
NfsbParseRecord(NFSB_VOLUME *Volume, const NFSB_U8 *Buffer, NFSB_U32 Length)
{
    const NFSB_REC_HDR *Header = (const NFSB_REC_HDR *)Buffer;
    const NFSB_BODY *Body;
    const NFSB_U8 *Cursor;
    NFSB_RECORD Record;
    NFSB_U32 Tail;

    if (Length < sizeof(NFSB_REC_HDR) + sizeof(NFSB_BODY))
        return NFSB_ECORRUPT;
    if (Header->Magic[0] != NFSB_REC_MAGIC[0] || Header->Magic[1] != NFSB_REC_MAGIC[1] ||
        Header->Magic[2] != NFSB_REC_MAGIC[2] || Header->Magic[3] != NFSB_REC_MAGIC[3])
        return NFSB_ECORRUPT;
    if (Header->BodyLen < sizeof(NFSB_BODY) ||
        (NFSB_U64)sizeof(NFSB_REC_HDR) + Header->BodyLen > Length)
        return NFSB_ECORRUPT;
    if (Header->Crc32c != NFSB_Crc32c(0, Header + 1, Header->BodyLen))
        return NFSB_ECORRUPT;

    Body = (const NFSB_BODY *)(Header + 1);
    if (Body->NameLen > NFSB_NAME_MAX || Body->ExtentCount > NFSB_EXTENT_MAX)
        return NFSB_ECORRUPT;

    Tail = Body->NameLen * (NFSB_U32)sizeof(NFSB_U16) +
           Body->ExtentCount * (NFSB_U32)sizeof(NFSB_EXTENT);
    if ((NFSB_U64)sizeof(NFSB_BODY) + Tail > Header->BodyLen)
        return NFSB_ECORRUPT;

    Record.FileId = Body->FileId;
    Record.Parent = Body->Parent;
    Record.Seq = Body->Seq;
    Record.Size = Body->Size;
    Record.AllocSize = Body->AllocSize;
    Record.Flags = Body->Flags;
    Record.Attributes = Body->Attributes;
    Record.NameLen = Body->NameLen;
    Record.ExtentCount = Body->ExtentCount;
    Record.Name = 0;
    Record.Extents = 0;

    if (Tail)
    {
        NFSB_U8 *Arena = (NFSB_U8 *)NfsbArenaAlloc(Volume, Tail);
        NFSB_U32 NameBytes = Body->NameLen * (NFSB_U32)sizeof(NFSB_U16);
        NFSB_U32 i;

        if (!Arena)
            return NFSB_ENOMEM;

        Cursor = (const NFSB_U8 *)(Body + 1);
        for (i = 0; i < NameBytes; i++)
            Arena[i] = Cursor[i];
        for (i = 0; i < Body->ExtentCount * (NFSB_U32)sizeof(NFSB_EXTENT); i++)
            Arena[NameBytes + i] = Cursor[NameBytes + i];

        Record.Name = (const NFSB_U16 *)Arena;
        Record.Extents = (const NFSB_EXTENT *)(Arena + NameBytes);
    }

    return NfsbAppendRecord(Volume, &Record);
}

/* ------------------------------------------------------------------ */
/* Checkpoint tree                                                     */
/* ------------------------------------------------------------------ */

static int NfsbWalkNode(NFSB_VOLUME *Volume, NFSB_U64 Block, NFSB_U32 Depth,
                        NFSB_U8 *Scratch);

static int
NfsbWalkLeaf(NFSB_VOLUME *Volume, NFSB_U8 *Node)
{
    NFSB_NODE_HDR *Header = (NFSB_NODE_HDR *)Node;
    NFSB_U32 Cursor = Header->DataOffset;
    NFSB_U32 End = (NFSB_U32)Header->DataOffset + Header->UsedBytes;

    while (Cursor + sizeof(NFSB_U32) <= End)
    {
        NFSB_U32 RecordLen;
        int Status;

        RecordLen = 0;
        {
            NFSB_U8 *p = Node + Cursor;
            RecordLen = (NFSB_U32)p[0] | ((NFSB_U32)p[1] << 8) |
                        ((NFSB_U32)p[2] << 16) | ((NFSB_U32)p[3] << 24);
        }
        Cursor += sizeof(NFSB_U32);
        if (RecordLen > End - Cursor)
            return NFSB_ECORRUPT;

        /* One unreadable record does not condemn the node: skip it and keep
           going, the same way a bad sector in one file must not stop the boot. */
        Status = NfsbParseRecord(Volume, Node + Cursor, RecordLen);
        if (Status == NFSB_ENOMEM)
            return Status;

        Cursor += RecordLen;
    }

    return NFSB_OK;
}

static int
NfsbWalkNode(NFSB_VOLUME *Volume, NFSB_U64 Block, NFSB_U32 Depth, NFSB_U8 *Scratch)
{
    NFSB_NODE_HDR *Header;
    int Status;

    if (Depth > NFSB_TREE_MAX_DEPTH)
        return NFSB_ECORRUPT;

    Status = NfsbReadNode(Volume, Block, Scratch);
    if (Status != NFSB_OK)
        return Status;

    Header = (NFSB_NODE_HDR *)Scratch;

    if (Header->Level == 0)
        return NfsbWalkLeaf(Volume, Scratch);

    {
        NFSB_U32 Cursor = Header->DataOffset;
        NFSB_U32 Entry, End = (NFSB_U32)Header->DataOffset + Header->UsedBytes;

        for (Entry = 0; Entry < Header->Entries; Entry++)
        {
            NFSB_U64 Child = 0;
            NFSB_U32 i;

            if (Cursor + 16 > End)
                return NFSB_ECORRUPT;
            for (i = 0; i < 8; i++)
                Child |= (NFSB_U64)Scratch[Cursor + 8 + i] << (8 * i);
            Cursor += 16;               /* key_max, then child */

            Status = NfsbWalkNode(Volume, Child, Depth + 1, Scratch);
            if (Status != NFSB_OK)
                return Status;
        }
    }

    return NFSB_OK;
}

/* ------------------------------------------------------------------ */
/* Superblock                                                          */
/* ------------------------------------------------------------------ */

static NFSB_U32
NfsbSuperCrc(const NFSB_SUPER *Super)
{
    NFSB_SUPER Copy = *Super;

    Copy.Crc32c = 0;
    return NFSB_Crc32c(0, &Copy, NFSB_SUPER_SIZE - sizeof(NFSB_U32));
}

static int
NfsbSuperValid(const NFSB_SUPER *Super, NFSB_U64 DeviceBytes)
{
    if (Super->Magic[0] != NFSB_SUPER_MAGIC[0] || Super->Magic[1] != NFSB_SUPER_MAGIC[1] ||
        Super->Magic[2] != NFSB_SUPER_MAGIC[2] || Super->Magic[3] != NFSB_SUPER_MAGIC[3])
        return 0;
    if (Super->Version != NFSB_FORMAT_VERSION || Super->HeaderSize != NFSB_SUPER_SIZE)
        return 0;
    if (Super->ClusterSize < 4096 ||
        (Super->ClusterSize & (Super->ClusterSize - 1)) != 0)
        return 0;
    if (Super->TotalClusters == 0)
        return 0;
    if (DeviceBytes && Super->TotalClusters * (NFSB_U64)Super->ClusterSize > DeviceBytes)
        return 0;
    if (Super->Crc32c != NfsbSuperCrc(Super))
        return 0;
    return 1;
}

static int
NfsbReadSuper(NFSB_VOLUME *Volume)
{
    NFSB_SUPER Primary, Backup;
    NFSB_U64 BackupOffset = 0;
    int PrimaryOk, BackupOk;

    PrimaryOk = 0;
    if (NfsbReadDevice(Volume, NFSB_SUPER_OFFSET, &Primary, NFSB_SUPER_SIZE) == NFSB_OK)
        PrimaryOk = NfsbSuperValid(&Primary, Volume->Io.DeviceBytes);

    /* The backup lives in the last cluster: after a torn primary this is the
       only thing that can still save the boot. */
    if (PrimaryOk)
        BackupOffset = (Primary.TotalClusters - 1) * (NFSB_U64)Primary.ClusterSize;
    else if (Volume->Io.DeviceBytes >= 4096)
        BackupOffset = ((Volume->Io.DeviceBytes / 4096) - 1) * 4096ull;

    BackupOk = 0;
    if (BackupOffset != 0 &&
        NfsbReadDevice(Volume, BackupOffset, &Backup, NFSB_SUPER_SIZE) == NFSB_OK)
        BackupOk = NfsbSuperValid(&Backup, Volume->Io.DeviceBytes);

    if (PrimaryOk && BackupOk)
    {
        if (Primary.Generation >= Backup.Generation)
            Volume->Super = Primary;
        else
        {
            Volume->Super = Backup;
            Volume->FromBackupSuper = 1;
        }
    }
    else if (PrimaryOk)
        Volume->Super = Primary;
    else if (BackupOk)
    {
        Volume->Super = Backup;
        Volume->FromBackupSuper = 1;
    }
    else
        return NFSB_ECORRUPT;

    return NFSB_OK;
}

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                     */
/* ------------------------------------------------------------------ */

int
NFSB_Mount(const NFSB_IO *Io, const NFSB_HEAP *Heap, NFSB_VOLUME **OutVolume)
{
    NFSB_VOLUME *Volume;
    NFSB_U8 *Scratch;
    int Status;

    if (!Io || !Io->Read || !Heap || !Heap->Alloc || !Heap->Free || !OutVolume)
        return NFSB_ECORRUPT;
    *OutVolume = 0;

    Volume = (NFSB_VOLUME *)Heap->Alloc(sizeof(NFSB_VOLUME));
    if (!Volume)
        return NFSB_ENOMEM;

    {
        NFSB_U8 *p = (NFSB_U8 *)Volume;
        NFSB_U32 i;

        for (i = 0; i < (NFSB_U32)sizeof(NFSB_VOLUME); i++)
            p[i] = 0;
    }
    Volume->Io = *Io;
    Volume->Heap = *Heap;

    Status = NfsbReadSuper(Volume);
    if (Status != NFSB_OK)
        goto Fail;

    Volume->DirtyAtMount = (Volume->Super.Flags & NFSB_SB_DIRTY) ? 1 : 0;

    Scratch = (NFSB_U8 *)Heap->Alloc(NFSB_NODE_SIZE);
    if (!Scratch)
    {
        Status = NFSB_ENOMEM;
        goto Fail;
    }

    /* The cluster size is a power of two of at least 4 KiB, so one node always
       fits in one cluster and the walk never needs two buffers. */

    Status = NfsbWalkNode(Volume, Volume->Super.CheckpointRoot.Block, 0, Scratch);
    if (Status != NFSB_OK)
    {
        /* The newest generation does not verify: start over on the previous one
           rather than pretend the volume is unreadable. */
        NFSB_U32 i;

        for (i = 0; i < Volume->RecordCount; i++)
        {
            Volume->Records[i].FileId = 0;
            Volume->Records[i].Name = 0;
            Volume->Records[i].Extents = 0;
        }
        Volume->RecordCount = 0;
        NfsbArenaDestroy(Volume);
        if (Volume->Hash)
        {
            for (i = 0; i < Volume->HashSize; i++)
                Volume->Hash[i] = 0xFFFFFFFFu;
        }
        Status = NfsbWalkNode(Volume, Volume->Super.CheckpointRootPrev.Block, 0, Scratch);
        if (Status != NFSB_OK)
        {
            Heap->Free(Scratch);
            goto Fail;
        }
        Volume->UsedFallbackRoot = 1;
    }

    Heap->Free(Scratch);

    {
        int Root = NfsbFindRecord(Volume, NFSB_ROOT_ID);

        if (Root < 0 || !(Volume->Records[Root].Flags & NFSB_FLAG_DIR))
        {
            Status = NFSB_ECORRUPT;      /* a volume without a root is not a volume */
            goto Fail;
        }
    }

    *OutVolume = Volume;
    return NFSB_OK;

Fail:
    NfsbArenaDestroy(Volume);
    if (Volume->Records)
        Heap->Free(Volume->Records);
    if (Volume->Hash)
        Heap->Free(Volume->Hash);
    Heap->Free(Volume);
    return Status;
}

void
NFSB_Unmount(NFSB_VOLUME *Volume)
{
    NFSB_HEAP Heap;

    if (!Volume)
        return;

    Heap = Volume->Heap;
    NfsbArenaDestroy(Volume);
    if (Volume->Records)
        Heap.Free(Volume->Records);
    if (Volume->Hash)
        Heap.Free(Volume->Hash);
    Heap.Free(Volume);
}

/* ------------------------------------------------------------------ */
/* Names and lookup                                                    */
/* ------------------------------------------------------------------ */

/* Decode one UTF-8 code point; returns the bytes consumed (0 at the end). */
static NFSB_U32
NfsbDecodeUtf8(const char *Text, NFSB_U32 *OutCodePoint)
{
    const NFSB_U8 *p = (const NFSB_U8 *)Text;

    if (p[0] == 0)
    {
        *OutCodePoint = 0;
        return 0;
    }
    if (p[0] < 0x80)
    {
        *OutCodePoint = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        *OutCodePoint = ((NFSB_U32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        *OutCodePoint = ((NFSB_U32)(p[0] & 0x0F) << 12) |
                        ((NFSB_U32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        *OutCodePoint = ((NFSB_U32)(p[0] & 0x07) << 18) |
                        ((NFSB_U32)(p[1] & 0x3F) << 12) |
                        ((NFSB_U32)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }

    *OutCodePoint = '?';                 /* malformed byte: it can only miss */
    return 1;
}

/* Case folding for comparing an ASCII path component with a UTF-16 name. */
static NFSB_U32
NfsbFold(NFSB_U32 CodePoint)
{
    if (CodePoint >= 'a' && CodePoint <= 'z')
        return CodePoint - 'a' + 'A';
    return CodePoint;
}

static int
NfsbNameMatches(const NFSB_RECORD *Record, const char *Name, NFSB_U32 NameLength)
{
    NFSB_U32 Units = 0, Bytes = 0;

    while (Bytes < NameLength)
    {
        NFSB_U32 CodePoint, Consumed;

        Consumed = NfsbDecodeUtf8(Name + Bytes, &CodePoint);
        if (Consumed == 0 || Bytes + Consumed > NameLength)
            return 0;
        if (Units >= Record->NameLen)
            return 0;
        if (NfsbFold(Record->Name[Units]) != NfsbFold(CodePoint))
            return 0;
        Units++;
        Bytes += Consumed;
    }

    return Units == Record->NameLen;
}

const NFSB_RECORD *
NFSB_LookupChild(NFSB_VOLUME *Volume, NFSB_U64 Parent, const char *Name,
                 NFSB_U32 NameLength)
{
    NFSB_U32 i;

    if (!Volume || !Name || NameLength == 0)
        return 0;

    /* A linear pass over the PIFT: a boot resolves a handful of paths, and a
       sorted child index would cost more memory than it saves. */
    for (i = 0; i < Volume->RecordCount; i++)
    {
        const NFSB_RECORD *Record = &Volume->Records[i];

        if (Record->FileId == 0 || Record->Parent != Parent)
            continue;
        if (Record->Flags & NFSB_FLAG_DELETED)
            continue;
        if (NfsbNameMatches(Record, Name, NameLength))
            return Record;
    }

    return 0;
}

const NFSB_RECORD *
NFSB_LookupPath(NFSB_VOLUME *Volume, const char *Path)
{
    NFSB_U64 Parent = NFSB_ROOT_ID;
    const NFSB_RECORD *Record;
    const char *p;

    if (!Volume || !Path)
        return 0;

    Record = 0;
    p = Path;

    while (*p)
    {
        const char *Start;
        NFSB_U32 Length;

        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;

        Start = p;
        while (*p && *p != '\\' && *p != '/')
            p++;
        Length = (NFSB_U32)(p - Start);
        if (Length == 1 && Start[0] == '.')
            continue;

        Record = NFSB_LookupChild(Volume, Parent, Start, Length);
        if (!Record)
            return 0;
        if (Record->Flags & NFSB_FLAG_DIR)
        {
            Parent = Record->FileId;
            continue;
        }

        return (*p) ? 0 : Record;        /* a file with a path left over */
    }

    return (Parent == NFSB_ROOT_ID) ? 0 : Record;
}

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */

static NFSB_U64
NfsbMapCluster(const NFSB_RECORD *Record, NFSB_U64 Vcn)
{
    NFSB_U32 i;

    for (i = 0; i < Record->ExtentCount; i++)
    {
        const NFSB_EXTENT *Extent = &Record->Extents[i];

        if (Extent->Length == 0)
            break;
        if (Vcn >= Extent->Vcn && Vcn < Extent->Vcn + Extent->Length)
        {
            if (Extent->Lcn == NFSB_HOLE)
                return NFSB_HOLE;
            return Extent->Lcn + (Vcn - Extent->Vcn);
        }
    }

    return NFSB_HOLE;
}

int
NFSB_Read(NFSB_VOLUME *Volume, const NFSB_RECORD *Record, NFSB_U64 Offset,
          void *Buffer, NFSB_U32 Length, NFSB_U32 *Read)
{
    NFSB_U8 *Out = (NFSB_U8 *)Buffer;
    NFSB_U32 Done = 0;
    NFSB_U64 ClusterSize;

    if (!Volume || !Record || (!Buffer && Length))
        return NFSB_ECORRUPT;
    if (Read)
        *Read = 0;
    if (Record->Flags & NFSB_FLAG_DIR)
        return NFSB_ECORRUPT;

    ClusterSize = Volume->Super.ClusterSize;

    while (Done < Length && Offset + Done < Record->Size)
    {
        NFSB_U64 Position = Offset + Done;
        NFSB_U64 Vcn = Position / ClusterSize;
        NFSB_U32 InCluster = (NFSB_U32)(Position % ClusterSize);
        NFSB_U32 Chunk = (NFSB_U32)(ClusterSize - InCluster);
        NFSB_U64 Lcn;
        int Status;

        if (Chunk > Length - Done)
            Chunk = Length - Done;
        if (Position + Chunk > Record->Size)
            Chunk = (NFSB_U32)(Record->Size - Position);

        Lcn = NfsbMapCluster(Record, Vcn);
        if (Lcn == NFSB_HOLE)
        {
            NFSB_U32 i;

            for (i = 0; i < Chunk; i++)
                Out[Done + i] = 0;       /* a hole reads as zeroes */
            Done += Chunk;
            continue;
        }

        Status = NfsbReadDevice(Volume, Lcn * ClusterSize + InCluster,
                                Out + Done, Chunk);
        if (Status != NFSB_OK)
            return Status;
        Done += Chunk;
    }

    if (Read)
        *Read = Done;
    return NFSB_OK;
}

/* EOF */
