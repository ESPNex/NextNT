/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_pift.c
 * PURPOSE:     PIFT record codec and the in-RAM PIFT index
 * LICENSE:     GPL-3.0-or-later
 *
 * The PIFT (Pre-Indexed File Table) is the only index NextFS has. At runtime
 * it lives in RAM and is authoritative; on disk it exists as an append-only,
 * versioned write-through copy plus periodic checkpoints. Nothing reads the
 * on-disk copy except mount and nextchk.
 */

#include "nextfs_internal.h"

#include <string.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

nfs_u64
NfsPiftRecordSize(const NFS_PIFT_RECORD *Record)
{
    return sizeof(NFS_PIFT_REC_HDR) + sizeof(NFS_PIFT_BODY) +
           ((nfs_u64)Record->NameLen * sizeof(nfs_u16)) +
           ((nfs_u64)Record->ExtentCount * sizeof(NFS_EXTENT));
}

nfs_u32
NfsPiftSerialize(const NFS_PIFT_RECORD *Record, void *Buffer, nfs_u32 Capacity)
{
    NFS_PIFT_REC_HDR *Header = (NFS_PIFT_REC_HDR *)Buffer;
    NFS_PIFT_BODY *Body;
    nfs_u8 *Cursor;
    nfs_u32 BodyLen;

    BodyLen = (nfs_u32)(sizeof(NFS_PIFT_BODY) +
                        ((nfs_u64)Record->NameLen * sizeof(nfs_u16)) +
                        ((nfs_u64)Record->ExtentCount * sizeof(NFS_EXTENT)));
    if (sizeof(NFS_PIFT_REC_HDR) + BodyLen > Capacity)
        return 0;

    memcpy(Header->Magic, NFS_REC_MAGIC, 4);
    Header->BodyLen = BodyLen;

    Body = (NFS_PIFT_BODY *)(Header + 1);
    Body->FileId = Record->FileId;
    Body->Generation = Record->Generation;
    Body->Parent = Record->Parent;
    Body->Seq = Record->Seq;
    Body->Flags = Record->Flags;
    Body->Attributes = Record->Attributes;
    Body->Size = Record->Size;
    Body->AllocSize = Record->AllocSize;
    Body->Ctime = Record->Ctime;
    Body->Mtime = Record->Mtime;
    Body->Atime = Record->Atime;
    Body->Btime = Record->Btime;
    Body->SnapshotRoot = Record->SnapshotRoot;
    Body->SnapshotGeneration = Record->SnapshotGeneration;
    Body->SecurityId = Record->SecurityId;
    Body->SecurityLength = Record->SecurityLength;
    Body->NameLen = Record->NameLen;
    Body->Reserved0 = 0;
    Body->ExtentCount = Record->ExtentCount;

    Cursor = (nfs_u8 *)(Body + 1);
    if (Record->NameLen)
        memcpy(Cursor, Record->Name, (size_t)Record->NameLen * sizeof(nfs_u16));
    Cursor += (size_t)Record->NameLen * sizeof(nfs_u16);
    if (Record->ExtentCount)
        memcpy(Cursor, Record->Extents, (size_t)Record->ExtentCount * sizeof(NFS_EXTENT));

    Header->Crc32c = NfsCrc32c(0, Header + 1, BodyLen);
    return (nfs_u32)(sizeof(NFS_PIFT_REC_HDR) + BodyLen);
}

int
NfsPiftDeserialize(const void *Buffer, nfs_u32 Length, NFS_PIFT_RECORD *Record)
{
    const NFS_PIFT_REC_HDR *Header = (const NFS_PIFT_REC_HDR *)Buffer;
    const NFS_PIFT_BODY *Body;
    const nfs_u8 *Cursor;

    if (Length < sizeof(NFS_PIFT_REC_HDR) + sizeof(NFS_PIFT_BODY))
        return NFS_ECORRUPT;
    if (memcmp(Header->Magic, NFS_REC_MAGIC, 4) != 0)
        return NFS_ECORRUPT;
    if (Header->BodyLen < sizeof(NFS_PIFT_BODY))
        return NFS_ECORRUPT;
    if (sizeof(NFS_PIFT_REC_HDR) + Header->BodyLen > Length)
        return NFS_ECORRUPT;
    if (Header->Crc32c != NfsCrc32c(0, Header + 1, Header->BodyLen))
        return NFS_ECORRUPT;

    Body = (const NFS_PIFT_BODY *)(Header + 1);
    if (Body->NameLen > NFS_NAME_MAX || Body->ExtentCount > NFS_EXTENT_MAX)
        return NFS_ECORRUPT;
    if (sizeof(NFS_PIFT_BODY) +
        (nfs_u64)Body->NameLen * sizeof(nfs_u16) +
        (nfs_u64)Body->ExtentCount * sizeof(NFS_EXTENT) > Header->BodyLen)
        return NFS_ECORRUPT;

    memset(Record, 0, sizeof(*Record));
    Record->FileId = Body->FileId;
    Record->Generation = Body->Generation;
    Record->Parent = Body->Parent;
    Record->Seq = Body->Seq;
    Record->Flags = Body->Flags;
    Record->Attributes = Body->Attributes;
    Record->Size = Body->Size;
    Record->AllocSize = Body->AllocSize;
    Record->Ctime = Body->Ctime;
    Record->Mtime = Body->Mtime;
    Record->Atime = Body->Atime;
    Record->Btime = Body->Btime;
    Record->SnapshotRoot = Body->SnapshotRoot;
    Record->SnapshotGeneration = Body->SnapshotGeneration;
    Record->SecurityId = Body->SecurityId;
    Record->SecurityLength = Body->SecurityLength;
    Record->NameLen = Body->NameLen;
    Record->ExtentCount = Body->ExtentCount;

    Cursor = (const nfs_u8 *)(Body + 1);
    if (Record->NameLen)
        memcpy(Record->Name, Cursor, (size_t)Record->NameLen * sizeof(nfs_u16));
    Cursor += (size_t)Record->NameLen * sizeof(nfs_u16);
    if (Record->ExtentCount)
        memcpy(Record->Extents, Cursor, (size_t)Record->ExtentCount * sizeof(NFS_EXTENT));

    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* In-RAM index: open addressing, linear probing, tombstones.          */
/* ------------------------------------------------------------------ */

nfs_u32
NfsIndexHashFileId(nfs_fileid FileId)
{
    /* splitmix64 finaliser: cheap and well spread for small ids. */
    nfs_u64 x = FileId + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return (nfs_u32)x;
}

static int
NfsIndexGrow(PNFS_VOLUME Volume, nfs_u32 NewSize)
{
    nfs_u32 *OldHash = Volume->Hash;
    nfs_u32 OldSize = Volume->HashSize;
    nfs_u32 *NewHash;
    nfs_u32 i;

    NewHash = (nfs_u32 *)NfsMemAlloc((size_t)NewSize * sizeof(nfs_u32));
    if (!NewHash)
        return NFS_ENOSPC;
    for (i = 0; i < NewSize; i++)
        NewHash[i] = NFS_HASH_EMPTY;

    for (i = 0; i < OldSize; i++)
    {
        nfs_u32 Slot;
        if (OldHash[i] == NFS_HASH_EMPTY || OldHash[i] == NFS_HASH_TOMB)
            continue;
        Slot = NfsIndexHashFileId(Volume->Records[OldHash[i]].FileId) & (NewSize - 1);
        while (NewHash[Slot] != NFS_HASH_EMPTY)
            Slot = (Slot + 1) & (NewSize - 1);
        NewHash[Slot] = OldHash[i];
    }

    NfsMemFree(OldHash);
    Volume->Hash = NewHash;
    Volume->HashSize = NewSize;
    return NFS_OK;
}

int
NfsIndexInit(PNFS_VOLUME Volume)
{
    Volume->HashSize = 256;
    Volume->HashUsed = 0;
    Volume->Hash = (nfs_u32 *)NfsMemAlloc((size_t)Volume->HashSize * sizeof(nfs_u32));
    if (!Volume->Hash)
        return NFS_ENOSPC;
    memset(Volume->Hash, 0xFF, (size_t)Volume->HashSize * sizeof(nfs_u32));
    Volume->RecordCount = 0;
    Volume->RecordCapacity = 0;
    Volume->Records = NULL;
    return NFS_OK;
}

void
NfsIndexDestroy(PNFS_VOLUME Volume)
{
    NfsMemFree(Volume->Hash);
    NfsMemFree(Volume->Records);
    Volume->Hash = NULL;
    Volume->Records = NULL;
    Volume->HashSize = 0;
    Volume->RecordCount = 0;
    Volume->RecordCapacity = 0;
}

static nfs_u32 *
NfsIndexFindSlot(PNFS_VOLUME Volume, nfs_fileid FileId)
{
    nfs_u32 Mask = Volume->HashSize - 1;
    nfs_u32 Slot = NfsIndexHashFileId(FileId) & Mask;
    nfs_u32 Probes = 0;

    while (Probes++ <= Volume->HashSize)
    {
        nfs_u32 Value = Volume->Hash[Slot];

        if (Value == NFS_HASH_EMPTY)
            return &Volume->Hash[Slot];
        if (Value != NFS_HASH_TOMB &&
            Volume->Records[Value].FileId == FileId)
            return &Volume->Hash[Slot];
        Slot = (Slot + 1) & Mask;
    }
    return NULL;
}

NFS_PIFT_RECORD *
NfsIndexGet(PNFS_VOLUME Volume, nfs_fileid FileId)
{
    nfs_u32 *Slot = NfsIndexFindSlot(Volume, FileId);

    if (!Slot || *Slot == NFS_HASH_EMPTY || *Slot == NFS_HASH_TOMB)
        return NULL;
    return &Volume->Records[*Slot];
}

int
NfsIndexLocate(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u32 *OutIndex)
{
    nfs_u32 *Slot = NfsIndexFindSlot(Volume, FileId);

    if (!Slot || *Slot == NFS_HASH_EMPTY || *Slot == NFS_HASH_TOMB)
        return NFS_ENOENT;
    if (OutIndex)
        *OutIndex = *Slot;
    return NFS_OK;
}

int
NfsIndexInsertEx(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record,
                 nfs_u32 *OutIndex)
{
    nfs_u32 *Slot;
    nfs_u32 Index;

    if (Volume->HashUsed * 10 >= Volume->HashSize * 7)
    {
        if (NfsIndexGrow(Volume, Volume->HashSize * 2) != NFS_OK)
            return NFS_ENOSPC;
    }

    Slot = NfsIndexFindSlot(Volume, Record->FileId);
    if (!Slot)
        return NFS_EIO;

    if (*Slot != NFS_HASH_EMPTY && *Slot != NFS_HASH_TOMB)
    {
        /* Replace in place: the record array keeps its slot. */
        Volume->Records[*Slot] = *Record;
        if (OutIndex)
            *OutIndex = *Slot;
        return NFS_OK;
    }

    if (Volume->RecordCount == Volume->RecordCapacity)
    {
        nfs_u32 NewCapacity = Volume->RecordCapacity ? Volume->RecordCapacity * 2 : 64;
        NFS_PIFT_RECORD *NewRecords;

        NewRecords = (NFS_PIFT_RECORD *)NfsMemAlloc((size_t)NewCapacity * sizeof(NFS_PIFT_RECORD));
        if (!NewRecords)
            return NFS_ENOSPC;
        if (Volume->Records)
        {
            memcpy(NewRecords, Volume->Records,
                   (size_t)Volume->RecordCount * sizeof(NFS_PIFT_RECORD));
            NfsMemFree(Volume->Records);
        }
        Volume->Records = NewRecords;
        Volume->RecordCapacity = NewCapacity;
    }

    Index = Volume->RecordCount++;
    Volume->Records[Index] = *Record;
    *Slot = Index;
    Volume->HashUsed++;
    if (OutIndex)
        *OutIndex = Index;
    return NFS_OK;
}

int
NfsIndexInsert(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record)
{
    return NfsIndexInsertEx(Volume, Record, NULL);
}

/* ------------------------------------------------------------------ */
/* Sorted child index                                                  */
/*                                                                     */
/* Lookups by name and directory enumeration never touch the disk: the */
/* child sets below are the runtime namespace index. They are rebuilt  */
/* at mount and updated on every create/remove.                        */
/* ------------------------------------------------------------------ */

static nfs_u16
ChildFold(nfs_u16 c)
{
    if (c >= 'A' && c <= 'Z')
        return (nfs_u16)(c - 'A' + 'a');
    return c;
}

static int
ChildNameCompare(const NFS_PIFT_RECORD *Left, const NFS_PIFT_RECORD *Right)
{
    nfs_u32 i, Limit = Left->NameLen < Right->NameLen ? Left->NameLen : Right->NameLen;

    for (i = 0; i < Limit; i++)
    {
        nfs_u16 a = ChildFold(Left->Name[i]);
        nfs_u16 b = ChildFold(Right->Name[i]);

        if (a != b)
            return (a < b) ? -1 : 1;
    }
    if (Left->NameLen == Right->NameLen)
        return 0;
    return (Left->NameLen < Right->NameLen) ? -1 : 1;
}

static nfs_u32
ChildrenBucket(nfs_fileid Parent)
{
    return NfsIndexHashFileId(Parent ^ 0xC0FFEEull) % NFS_CHILD_BUCKETS;
}

static PNFS_CHILD_SET
ChildrenFind(PNFS_VOLUME Volume, nfs_fileid Parent, int Create)
{
    nfs_u32 Bucket = ChildrenBucket(Parent);
    PNFS_CHILD_SET Set = Volume->Children[Bucket];

    while (Set)
    {
        if (Set->Parent == Parent)
            return Set;
        Set = Set->Next;
    }
    if (!Create)
        return NULL;

    Set = (PNFS_CHILD_SET)NfsMemAlloc(sizeof(NFS_CHILD_SET));
    if (!Set)
        return NULL;
    memset(Set, 0, sizeof(*Set));
    Set->Parent = Parent;
    Set->Next = Volume->Children[Bucket];
    Volume->Children[Bucket] = Set;
    return Set;
}

int
NfsChildrenInit(PNFS_VOLUME Volume)
{
    memset(Volume->Children, 0, sizeof(Volume->Children));
    return NFS_OK;
}

void
NfsChildrenDestroy(PNFS_VOLUME Volume)
{
    nfs_u32 i;

    for (i = 0; i < NFS_CHILD_BUCKETS; i++)
    {
        PNFS_CHILD_SET Set = Volume->Children[i];

        while (Set)
        {
            PNFS_CHILD_SET Next = Set->Next;
            NfsMemFree(Set->Indices);
            NfsMemFree(Set);
            Set = Next;
        }
        Volume->Children[i] = NULL;
    }
}

static nfs_u32
ChildrenLowerBound(PNFS_VOLUME Volume, PNFS_CHILD_SET Set,
                   const NFS_PIFT_RECORD *Record)
{
    nfs_u32 Low = 0, High = Set->Count;

    while (Low < High)
    {
        nfs_u32 Mid = Low + (High - Low) / 2;

        if (ChildNameCompare(&Volume->Records[Set->Indices[Mid]], Record) < 0)
            Low = Mid + 1;
        else
            High = Mid;
    }
    return Low;
}

int
NfsChildrenAdd(PNFS_VOLUME Volume, nfs_u32 RecordIndex)
{
    NFS_PIFT_RECORD *Record = &Volume->Records[RecordIndex];
    PNFS_CHILD_SET Set = ChildrenFind(Volume, Record->Parent, 1);
    nfs_u32 Position;

    if (!Set)
        return NFS_ENOSPC;

    if (Set->Count == Set->Capacity)
    {
        nfs_u32 NewCapacity = Set->Capacity ? Set->Capacity * 2 : 8;
        nfs_u32 *NewIndices = (nfs_u32 *)NfsMemAlloc((size_t)NewCapacity * sizeof(nfs_u32));

        if (!NewIndices)
            return NFS_ENOSPC;
        if (Set->Indices)
        {
            memcpy(NewIndices, Set->Indices, (size_t)Set->Count * sizeof(nfs_u32));
            NfsMemFree(Set->Indices);
        }
        Set->Indices = NewIndices;
        Set->Capacity = NewCapacity;
    }

    Position = ChildrenLowerBound(Volume, Set, Record);
    if (Position < Set->Count)
        memmove(&Set->Indices[Position + 1], &Set->Indices[Position],
                (size_t)(Set->Count - Position) * sizeof(nfs_u32));
    Set->Indices[Position] = RecordIndex;
    Set->Count++;
    return NFS_OK;
}

void
NfsChildrenRemove(PNFS_VOLUME Volume, nfs_u32 RecordIndex)
{
    NFS_PIFT_RECORD *Record = &Volume->Records[RecordIndex];
    PNFS_CHILD_SET Set = ChildrenFind(Volume, Record->Parent, 0);
    nfs_u32 i;

    if (!Set)
        return;

    for (i = 0; i < Set->Count; i++)
    {
        if (Set->Indices[i] == RecordIndex)
        {
            if (i + 1 < Set->Count)
                memmove(&Set->Indices[i], &Set->Indices[i + 1],
                        (size_t)(Set->Count - i - 1) * sizeof(nfs_u32));
            Set->Count--;
            return;
        }
    }
}

int
NfsChildrenRebuild(PNFS_VOLUME Volume)
{
    nfs_u32 i;

    NfsChildrenDestroy(Volume);
    for (i = 0; i < Volume->RecordCount; i++)
    {
        if (Volume->Records[i].Flags & NFS_FLAG_DELETED)
            continue;
        if (NfsChildrenAdd(Volume, i) != NFS_OK)
            return NFS_ENOSPC;
    }
    return NFS_OK;
}

NFS_PIFT_RECORD *
NfsChildrenLookup(PNFS_VOLUME Volume, nfs_fileid Parent,
                  const nfs_u16 *Name, nfs_u32 NameLength)
{
    PNFS_CHILD_SET Set = ChildrenFind(Volume, Parent, 0);
    NFS_PIFT_RECORD Probe;
    nfs_u32 Position;

    if (!Set || NameLength > NFS_NAME_MAX)
        return NULL;

    memset(&Probe, 0, sizeof(Probe));
    Probe.NameLen = NameLength;
    memcpy(Probe.Name, Name, (size_t)NameLength * sizeof(nfs_u16));

    Position = ChildrenLowerBound(Volume, Set, &Probe);
    if (Position >= Set->Count)
        return NULL;
    if (ChildNameCompare(&Volume->Records[Set->Indices[Position]], &Probe) != 0)
        return NULL;
    return &Volume->Records[Set->Indices[Position]];
}

NFS_PIFT_RECORD *
NfsChildrenAt(PNFS_VOLUME Volume, nfs_fileid Parent, nfs_u32 Index)
{
    PNFS_CHILD_SET Set = ChildrenFind(Volume, Parent, 0);

    if (!Set || Index >= Set->Count)
        return NULL;
    return &Volume->Records[Set->Indices[Index]];
}
