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
    Body->NameLen = Record->NameLen;
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
NfsIndexInsert(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record)
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
    return NFS_OK;
}
