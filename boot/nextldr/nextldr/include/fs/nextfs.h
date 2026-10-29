/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/include/fs/nextfs.h
 * PURPOSE:     Read-only NextFS reader for the boot loader
 * LICENSE:     GPL-3.0-or-later
 *
 * Why this file exists at all, when nextfslib already knows the format: the boot
 * loader is a freestanding 32-bit program that gets its memory from a heap that
 * can be freed at exit and its bytes from ARC firmware, and nextfslib is written
 * for a kernel that has a pageable pool and hands it IRPs. Sharing one
 * implementation would mean sharing one set of compromises, so this is a second,
 * much smaller reader -- and the only thing the two are allowed to share is the
 * on-disk format, which is spelled out below and checked by static asserts.
 *
 * What it reads, and what it deliberately does not:
 *
 *   - the superblock (primary, then the backup in the last cluster);
 *   - the checkpoint tree of the live generation, falling back to the previous
 *     one when the newest does not verify;
 *   - file data through the extent list of each record.
 *
 * It does *not* open the journal, and it does not scan the PIFT-on-disk area. A
 * volume that was shut down cleanly is checkpointed at unmount, so the tree holds
 * everything; a volume that is dirty keeps its last consistent checkpoint, which
 * is stale by at most the transactions in flight. Being one checkpoint behind is
 * recoverable; replaying a half-written journal during boot is not.
 */

#pragma once

/* ------------------------------------------------------------------ */
/* Portable core types (no OS dependency beyond a read callback)       */
/* ------------------------------------------------------------------ */

typedef unsigned char      NFSB_U8;
typedef unsigned short     NFSB_U16;
typedef unsigned int       NFSB_U32;
typedef unsigned long long NFSB_U64;

#define NFSB_SUPER_MAGIC     "NFSB"
#define NFSB_NODE_MAGIC      "NFBT"
#define NFSB_REC_MAGIC       "NFPR"

#define NFSB_SUPER_SIZE      1024u
#define NFSB_NODE_SIZE       4096u
#define NFSB_FORMAT_VERSION  4u
#define NFSB_NAME_MAX        255u
#define NFSB_EXTENT_MAX      64u
#define NFSB_TREE_MAX_DEPTH  8u
#define NFSB_ROOT_ID         1ull
#define NFSB_HOLE            0xFFFFFFFFFFFFFFFFull

#define NFSB_FLAG_DIR        0x00000001u
#define NFSB_FLAG_DELETED    0x00000002u

#define NFSB_SB_DIRTY        0x00000001u

/* Same status values as nextfslib, so a message can be shared between them. */
#define NFSB_OK              0
#define NFSB_EIO            -5
#define NFSB_ENOENT        -13
#define NFSB_ENOMEM        -12
#define NFSB_ECORRUPT      -30

#pragma pack(push, 1)

typedef struct _NFSB_ROOT_REF
{
    NFSB_U64 Block;
    NFSB_U64 Generation;
} NFSB_ROOT_REF, *PNFSB_ROOT_REF;

typedef struct _NFSB_EXTENT
{
    NFSB_U64 Vcn;
    NFSB_U64 Lcn;
    NFSB_U64 Length;
} NFSB_EXTENT, *PNFSB_EXTENT;

typedef struct _NFSB_SUPER
{
    char     Magic[4];
    NFSB_U16 Version;
    NFSB_U16 HeaderSize;
    NFSB_U32 ClusterSize;
    NFSB_U64 TotalClusters;

    NFSB_U8  VolumeUuid[16];
    NFSB_U64 Generation;

    NFSB_U64 JournalFirst;
    NFSB_U32 JournalClusters;
    NFSB_U64 JournalSeqFirst;
    NFSB_U64 JournalSeqNext;

    NFSB_U64 PiftAreaFirst;
    NFSB_U32 PiftAreaClusters;
    NFSB_U64 PiftFirst;
    NFSB_U32 PiftClusters;
    NFSB_U64 PiftHead;
    NFSB_U64 PiftSeqNext;

    NFSB_ROOT_REF CheckpointRoot;
    NFSB_ROOT_REF CheckpointRootPrev;

    NFSB_U64 BitmapFirst;
    NFSB_U32 BitmapClusters;
    NFSB_U64 FreeClusters;

    NFSB_U32 Flags;
    char     Label[64];
    NFSB_U8  Reserved[NFSB_SUPER_SIZE - 232 - 4];
    NFSB_U32 Crc32c;
} NFSB_SUPER, *PNFSB_SUPER;

typedef struct _NFSB_NODE_HDR
{
    char     Magic[4];
    NFSB_U16 Level;
    NFSB_U16 Entries;
    NFSB_U64 Generation;
    NFSB_U64 Block;
    NFSB_U64 Next;
    NFSB_U16 DataOffset;
    NFSB_U16 UsedBytes;
    NFSB_U32 Crc32c;
} NFSB_NODE_HDR, *PNFSB_NODE_HDR;

typedef struct _NFSB_REC_HDR
{
    char     Magic[4];
    NFSB_U32 BodyLen;
    NFSB_U32 Crc32c;
} NFSB_REC_HDR, *PNFSB_REC_HDR;

typedef struct _NFSB_BODY
{
    NFSB_U64 FileId;
    NFSB_U64 Generation;
    NFSB_U64 Parent;
    NFSB_U64 Seq;
    NFSB_U32 Flags;
    NFSB_U32 Attributes;
    NFSB_U64 Size;
    NFSB_U64 AllocSize;
    NFSB_U64 Ctime;
    NFSB_U64 Mtime;
    NFSB_U64 Atime;
    NFSB_U64 Btime;
    NFSB_U64 SnapshotRoot;
    NFSB_U64 SnapshotGeneration;
    NFSB_U64 SecurityId;
    NFSB_U32 SecurityLength;
    NFSB_U32 NameLen;
    NFSB_U32 ExtentCount;
    NFSB_U32 Reserved0;
} NFSB_BODY, *PNFSB_BODY;

#pragma pack(pop)

/* If any of these fail, the on-disk format changed and this reader is lying
   about the layout. Better a compile error than a boot that reads garbage. */
typedef char NfsbAssertSuperSize[(sizeof(NFSB_SUPER) == NFSB_SUPER_SIZE) ? 1 : -1];
typedef char NfsbAssertNodeHdrSize[(sizeof(NFSB_NODE_HDR) == 40) ? 1 : -1];
typedef char NfsbAssertRecHdrSize[(sizeof(NFSB_REC_HDR) == 12) ? 1 : -1];
typedef char NfsbAssertBodySize[(sizeof(NFSB_BODY) == 128) ? 1 : -1];
typedef char NfsbAssertExtentSize[(sizeof(NFSB_EXTENT) == 24) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* The reader                                                          */
/* ------------------------------------------------------------------ */

/* One read of Length bytes at Offset. Returns 0 or a negative status. */
typedef int (*NFSB_READ)(void *Context, NFSB_U64 Offset, void *Buffer, NFSB_U32 Length);
typedef void *(*NFSB_ALLOC)(NFSB_U32 Size);
typedef void (*NFSB_FREE)(void *Block);

typedef struct _NFSB_IO
{
    NFSB_READ Read;
    void *Context;
    NFSB_U64 DeviceBytes;   /* 0 when unknown: only the primary superblock is tried */
} NFSB_IO, *PNFSB_IO;

typedef struct _NFSB_HEAP
{
    NFSB_ALLOC Alloc;
    NFSB_FREE Free;
} NFSB_HEAP, *PNFSB_HEAP;

/* A record, with its name and extents copied into the volume's arena. */
typedef struct _NFSB_RECORD
{
    NFSB_U64 FileId;
    NFSB_U64 Parent;
    NFSB_U64 Seq;
    NFSB_U64 Size;
    NFSB_U64 AllocSize;
    NFSB_U32 Flags;
    NFSB_U32 Attributes;
    NFSB_U32 NameLen;
    NFSB_U32 ExtentCount;
    const NFSB_U16 *Name;
    const NFSB_EXTENT *Extents;
} NFSB_RECORD, *PNFSB_RECORD;

typedef struct _NFSB_VOLUME
{
    NFSB_IO Io;
    NFSB_HEAP Heap;

    NFSB_SUPER Super;

    NFSB_RECORD *Records;
    NFSB_U32 RecordCount;
    NFSB_U32 RecordCapacity;

    void *ArenaHead;        /* chunk list: a pointer handed out never moves */
    void *ArenaCurrent;

    NFSB_U32 *Hash;         /* fileid -> record index, for the tree's versions */
    NFSB_U32 HashSize;

    NFSB_U8  UsedFallbackRoot;
    NFSB_U8  DirtyAtMount;
    NFSB_U8  FromBackupSuper;
    NFSB_U8  Spare;
} NFSB_VOLUME, *PNFSB_VOLUME;

int NFSB_Mount(const NFSB_IO *Io, const NFSB_HEAP *Heap, NFSB_VOLUME **OutVolume);
void NFSB_Unmount(NFSB_VOLUME *Volume);

/* Path components separated by '\\' or '/', leading separators ignored. */
const NFSB_RECORD *NFSB_LookupPath(NFSB_VOLUME *Volume, const char *Path);
const NFSB_RECORD *NFSB_LookupChild(NFSB_VOLUME *Volume, NFSB_U64 Parent,
                                    const char *Name, NFSB_U32 NameLength);

int NFSB_Read(NFSB_VOLUME *Volume, const NFSB_RECORD *Record, NFSB_U64 Offset,
              void *Buffer, NFSB_U32 Length, NFSB_U32 *Read);
NFSB_U32 NFSB_Crc32c(NFSB_U32 Seed, const void *Buffer, NFSB_U32 Length);

/* ------------------------------------------------------------------ */
/* ARC glue (nextfs.c). Compiled out in the host-side test build.       */
/* ------------------------------------------------------------------ */

#ifndef NEXTFS_BOOT_NO_ARC

#include <fs.h>

const DEVVTBL* NextFsMount(ULONG DeviceId);

ARC_STATUS NextFsClose(ULONG FileId);
ARC_STATUS NextFsGetFileInformation(ULONG FileId, FILEINFORMATION* Information);
ARC_STATUS NextFsOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId);
ARC_STATUS NextFsRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count);
ARC_STATUS NextFsSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode);

ULONGLONG NextFsGetVolumeSize(ULONG DeviceId);

#endif /* !NEXTFS_BOOT_NO_ARC */
