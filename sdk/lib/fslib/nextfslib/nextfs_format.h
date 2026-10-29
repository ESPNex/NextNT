/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_format.h
 * PURPOSE:     NextFS on-disk format, PIFT records and the library API
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * v0.3: multi-level checkpoint tree, two live generations, PIFT split in two
 * halves. Portable C, no OS dependencies. Everything that touches the device goes
 * through nfs_bdev callbacks, so the same code serves nextformat (a file image),
 * the self-test and, in a later phase, nextfs.sys.
 */

#ifndef _NEXTFS_FORMAT_H_
#define _NEXTFS_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  nfs_u8;
typedef uint16_t nfs_u16;
typedef uint32_t nfs_u32;
typedef uint64_t nfs_u64;

typedef nfs_u64 nfs_fileid;
typedef nfs_u64 nfs_seq;

#define NFS_SUPER_MAGIC        "NFSB"
#define NFS_SUPER_SIZE         1024u
#define NFS_FORMAT_VERSION     3u
#define NFS_BOOT_OEM           "NEXTFS  "
#define NFS_NODE_MAGIC         "NFBT"
#define NFS_FRAME_MAGIC        "NFJN"
#define NFS_REC_MAGIC          "NFPR"

#define NFS_CLUSTER_MIN        4096u
#define NFS_NODE_SIZE          4096u
#define NFS_FRAME_SIZE         4096u
#define NFS_FRAME_HDR_SIZE          32u

#define NFS_NAME_MAX           255u
#define NFS_EXTENT_MAX         64u
#define NFS_LABEL_MAX          64u

/* Root directory is fileid 1; 0 is never a valid fileid. */
#define NFS_ROOT_ID            1ull
#define NFS_INVALID_ID         0ull

/* Superblock flags */
#define NFS_SB_DIRTY           0x00000001u
#define NFS_SB_NEEDS_CHECK     0x00000002u

/* Journal frame flags */
#define NFS_FRAME_COMMITTED    0x00000001u
#define NFS_FRAME_CHECKPOINT   0x00000002u

/* PIFT record flags */
#define NFS_FLAG_DIR           0x00000001u
#define NFS_FLAG_DELETED       0x00000002u
#define NFS_FLAG_INLINE_DATA   0x00000004u
#define NFS_FLAG_SNAPSHOT      0x00000008u   /* a pinned checkpoint generation    */

/* nfs_extent.lcn value for a sparse hole */
#define NFS_HOLE               0xFFFFFFFFFFFFFFFFull

/* Error codes: negative, errno-ish but self contained. */
#define NFS_OK                 0
#define NFS_EINVAL            -2
#define NFS_EIO               -5
#define NFS_ENOENT           -13
#define NFS_ENOSPC           -14
#define NFS_EEXIST           -17
#define NFS_ECORRUPT         -30
#define NFS_EUNSUPPORTED     -38

/* ------------------------------------------------------------------ */
/* On-disk structures. All little-endian, all packed.                  */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

typedef struct _NFS_ROOT_REF
{
    nfs_u64 Block;
    nfs_u64 Generation;
} NFS_ROOT_REF, *PNFS_ROOT_REF;

typedef struct _NFS_EXTENT
{
    nfs_u64 Vcn;        /* first virtual cluster                      */
    nfs_u64 Lcn;        /* first physical cluster, or NFS_HOLE        */
    nfs_u64 Length;     /* clusters; 0 ends the list                  */
} NFS_EXTENT, *PNFS_EXTENT;

typedef struct _NFS_SUPER
{
    char       Magic[4];
    nfs_u16    Version;
    nfs_u16    HeaderSize;
    nfs_u32    ClusterSize;
    nfs_u64    TotalClusters;

    nfs_u8     VolumeUuid[16];
    nfs_u64    Generation;

    nfs_u64    JournalFirst;
    nfs_u32    JournalClusters;
    nfs_u64    JournalSeqFirst;
    nfs_u64    JournalSeqNext;

    nfs_u64    PiftAreaFirst;     /* whole PIFT area, split in two halves */
    nfs_u32    PiftAreaClusters;
    nfs_u64    PiftFirst;         /* half used by the live generation  */
    nfs_u32    PiftClusters;      /* size of that half                 */
    nfs_u64    PiftHead;          /* bytes appended in that half       */
    nfs_u64    PiftSeqNext;

    NFS_ROOT_REF CheckpointRoot;      /* current CoW checkpoint tree    */
    NFS_ROOT_REF CheckpointRootPrev;  /* previous generation, fallback  */

    nfs_u64    BitmapFirst;
    nfs_u32    BitmapClusters;
    nfs_u64    FreeClusters;

    nfs_u32    Flags;
    char       Label[NFS_LABEL_MAX];
    /* Every field above, this padding and Crc32c fill exactly NFS_SUPER_SIZE. */
    nfs_u8     Reserved[NFS_SUPER_SIZE - 232 - 4];
    nfs_u32    Crc32c;
} NFS_SUPER, *PNFS_SUPER;

typedef struct _NFS_NODE_HDR
{
    char    Magic[4];
    nfs_u16 Level;      /* 0 = leaf, >0 = internal {key_max, child} array */
    nfs_u16 Entries;
    nfs_u64 Generation;
    nfs_u64 Block;
    nfs_u64 Next;       /* next leaf of the checkpoint chain, 0 = end    */
    nfs_u16 DataOffset; /* where the record payload starts               */
    nfs_u16 UsedBytes;
    nfs_u32 Crc32c;
} NFS_NODE_HDR, *PNFS_NODE_HDR;

typedef struct _NFS_FRAME_HDR
{
    char    Magic[4];
    nfs_u32 Flags;
    nfs_u64 Seq;
    nfs_u32 PayloadLen;
    nfs_u32 Crc32c;
    nfs_u64 Reserved;
} NFS_FRAME_HDR, *PNFS_FRAME_HDR;

typedef struct _NFS_PIFT_REC_HDR
{
    char     Magic[4];
    nfs_u32  BodyLen;      /* bytes of the serialised body              */
    nfs_u32  Crc32c;       /* over the body                             */
} NFS_PIFT_REC_HDR, *PNFS_PIFT_REC_HDR;

typedef struct _NFS_PIFT_BODY
{
    nfs_u64 FileId;
    nfs_u64 Generation;
    nfs_u64 Parent;
    nfs_u64 Seq;
    nfs_u32 Flags;
    nfs_u32 Attributes;
    nfs_u64 Size;
    nfs_u64 AllocSize;
    nfs_u64 Ctime, Mtime, Atime, Btime;
    nfs_u64 SnapshotRoot;  /* snapshot records: the pinned tree root    */
    nfs_u64 SnapshotGeneration;
    nfs_u32 NameLen;       /* UTF-16 code units                         */
    nfs_u32 ExtentCount;
} NFS_PIFT_BODY, *PNFS_PIFT_BODY;

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* In-memory PIFT record. Fixed-size arrays: 255 name units + 64       */
/* extents. ~1.9 KiB each; a compacting allocator is a phase-2 item.    */
/* ------------------------------------------------------------------ */

typedef struct _NFS_PIFT_RECORD
{
    nfs_fileid FileId;
    nfs_u64    Generation;
    nfs_fileid Parent;
    nfs_seq    Seq;
    nfs_u32    Flags;
    nfs_u32    Attributes;
    nfs_u64    Size;
    nfs_u64    AllocSize;
    nfs_u64    Ctime, Mtime, Atime, Btime;
    nfs_u64    SnapshotRoot;         /* only for NFS_FLAG_SNAPSHOT records */
    nfs_u64    SnapshotGeneration;
    nfs_u32    NameLen;
    nfs_u16    Name[NFS_NAME_MAX];
    nfs_u32    ExtentCount;
    NFS_EXTENT Extents[NFS_EXTENT_MAX];
} NFS_PIFT_RECORD, *PNFS_PIFT_RECORD;

/* ------------------------------------------------------------------ */
/* Block device callbacks                                              */
/* ------------------------------------------------------------------ */

typedef struct _NFS_BDEV
{
    void *Context;
    int (*Read)(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length);
    int (*Write)(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length);
    int (*Size)(void *Context, nfs_u64 *Bytes);
    int (*Flush)(void *Context);
} NFS_BDEV, *PNFS_BDEV;

/* ------------------------------------------------------------------ */
/* Volume handle                                                       */
/* ------------------------------------------------------------------ */

typedef struct _NFS_VOLUME NFS_VOLUME, *PNFS_VOLUME;

typedef struct _NFS_STATS
{
    nfs_u64 Reads;
    nfs_u64 Writes;
    nfs_u64 JournalCommits;
    nfs_u64 Checkpoints;
    nfs_u64 ReplayedFrames;
    nfs_u32 Files;
    nfs_u32 Snapshots;
    nfs_u32 UsedFallbackRoot;   /* mount had to use the previous tree root */
    nfs_u32 PinnedRanges;       /* snapshot pin intervals in RAM        */
} NFS_STATS, *PNFS_STATS;

/* One snapshot, as reported by NfsSnapshotList. */
typedef struct _NFS_SNAPSHOT_INFO
{
    nfs_fileid Id;
    nfs_u64    TreeRoot;
    nfs_u64    Generation;
    nfs_u64    Created;
    nfs_u32    NameLen;
    nfs_u16    Name[NFS_NAME_MAX];
} NFS_SNAPSHOT_INFO, *PNFS_SNAPSHOT_INFO;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* CRC32C (Castagnoli), used by every on-disk structure. */
nfs_u32 NfsCrc32c(nfs_u32 Seed, const void *Buffer, size_t Length);

/* Format a device. Destroys whatever was there. */
int NfsFormat(PNFS_BDEV Device, const char *Label, nfs_u32 ClusterSize);

/* Mount (loads the PIFT into RAM and replays the journal if needed). */
int NfsMount(PNFS_BDEV Device, PNFS_VOLUME *OutVolume);
void NfsUnmount(PNFS_VOLUME Volume);
/* Drop a volume without flushing (crash simulation, force dismount). */
void NfsDiscard(PNFS_VOLUME Volume);

/* Flush, checkpoint the PIFT and clear the dirty flag. */
int NfsSync(PNFS_VOLUME Volume);

/* Namespace operations. Names are UTF-16, not NUL terminated, Length units. */
int NfsCreate(PNFS_VOLUME Volume, nfs_fileid Parent, const nfs_u16 *Name,
              nfs_u32 NameLength, int IsDirectory, nfs_fileid *OutFileId);
int NfsLookup(PNFS_VOLUME Volume, nfs_fileid Parent, const nfs_u16 *Name,
              nfs_u32 NameLength, NFS_PIFT_RECORD *OutRecord);
int NfsOpenById(PNFS_VOLUME Volume, nfs_fileid FileId, NFS_PIFT_RECORD *OutRecord);
int NfsRemove(PNFS_VOLUME Volume, nfs_fileid FileId);
int NfsReadDir(PNFS_VOLUME Volume, nfs_fileid Directory, nfs_u32 Index,
               NFS_PIFT_RECORD *OutRecord);

/* Snapshots: a pinned checkpoint generation. Every block a snapshot owns (its
   tree nodes and the file data its records map) stays allocated until the
   snapshot is deleted; the pin set lives in RAM and is rebuilt at mount. */
int NfsSnapshotCreate(PNFS_VOLUME Volume, const nfs_u16 *Name, nfs_u32 NameLength,
                      nfs_fileid *OutId);
int NfsSnapshotDelete(PNFS_VOLUME Volume, nfs_fileid SnapshotId);
int NfsSnapshotList(PNFS_VOLUME Volume, nfs_u32 Index, NFS_SNAPSHOT_INFO *OutInfo);
int NfsSnapshotLookup(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid Parent,
                      const nfs_u16 *Name, nfs_u32 NameLength,
                      NFS_PIFT_RECORD *OutRecord);
int NfsSnapshotOpenById(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid FileId,
                        NFS_PIFT_RECORD *OutRecord);
int NfsSnapshotRead(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid FileId,
                    nfs_u64 Offset, void *Buffer, nfs_u32 Length, nfs_u32 *Read);

/* Data. Any offset and length; the touched clusters are copy-on-write and a
   write past EOF creates a sparse hole. Length 0 with an offset truncates. */
int NfsWrite(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 Offset,
             const void *Buffer, nfs_u32 Length, nfs_u32 *Written);
int NfsRead(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 Offset,
            void *Buffer, nfs_u32 Length, nfs_u32 *Read);
/* Grow (with a hole) or shrink a file. */
int NfsTruncate(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u64 NewSize);

void NfsGetStats(PNFS_VOLUME Volume, PNFS_STATS Stats);
const NFS_SUPER *NfsGetSuper(PNFS_VOLUME Volume);

/* ASCII/UTF-8 helper for tools and tests: "name" -> UTF-16 */
nfs_u32 NfsNameFromAscii(const char *Ascii, nfs_u16 *Out, nfs_u32 MaxUnits);
int NfsNameToAscii(const nfs_u16 *Name, nfs_u32 Length, char *Out, nfs_u32 MaxBytes);

/* Allocation hooks: override for kernel pool. Defaults to malloc/free. */
typedef void *(*NFS_ALLOC_FN)(size_t Size);
typedef void (*NFS_FREE_FN)(void *Block);
void NfsSetAllocators(NFS_ALLOC_FN Alloc, NFS_FREE_FN Free);

#ifdef __cplusplus
}
#endif

#endif /* _NEXTFS_FORMAT_H_ */
