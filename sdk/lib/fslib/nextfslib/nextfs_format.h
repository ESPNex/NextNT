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
/* Format 5: the superblock moved out of offset 0. Sector 0 of a volume is
   where a BIOS hands control, so the first 4 KiB -- one minimum-size cluster --
   is a boot area and the superblock starts right after it. Formats 1..4 kept
   the superblock at offset 0, which made a NextFS volume impossible to boot
   from without overwriting the superblock. */
#define NFS_FORMAT_VERSION     5u
#define NFS_BOOT_AREA_SIZE     4096u
#define NFS_SUPER_OFFSET       NFS_BOOT_AREA_SIZE
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
#define NFS_SECURITY_MAX       65536u   /* largest descriptor blob         */
#define NFS_SECURITY_DIR       ".security"
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

/* File attribute bits, the same values the Win32 API uses. */
#define NFS_ATTRIBUTE_READONLY    0x00000001u
#define NFS_ATTRIBUTE_HIDDEN      0x00000002u
#define NFS_ATTRIBUTE_SYSTEM      0x00000004u
#define NFS_ATTRIBUTE_DIRECTORY   0x00000010u
#define NFS_ATTRIBUTE_ARCHIVE     0x00000020u
#define NFS_ATTRIBUTE_NORMAL      0x00000080u

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
    nfs_u64 SecurityId;    /* CRC32C of the descriptor blob, 0 = none    */
    nfs_u32 SecurityLength;/* length of that blob                        */
    nfs_u32 NameLen;       /* UTF-16 code units                         */
    nfs_u32 ExtentCount;
    nfs_u32 Reserved0;
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
    nfs_u64    SecurityId;           /* CRC32C of the descriptor blob, 0 = none */
    nfs_u32    SecurityLength;       /* length of that blob, bytes          */
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

/* What NfsSetFileInfo may change. */
#define NFS_SET_ATTRIBUTES    0x00000001u
#define NFS_SET_CTIME         0x00000002u
#define NFS_SET_MTIME         0x00000004u
#define NFS_SET_ATIME         0x00000008u
#define NFS_SET_BTIME         0x00000010u

typedef struct _NFS_FILE_INFO
{
    nfs_u32 SetMask;
    nfs_u32 Attributes;
    nfs_u64 Ctime;
    nfs_u64 Mtime;
    nfs_u64 Atime;
    nfs_u64 Btime;
} NFS_FILE_INFO, *PNFS_FILE_INFO;

/* Creation dispositions, the Win32/NT values the FSD receives in
   FILE_CREATE_DISPOSITION_INFORMATION. */
#define NFS_DISPOSITION_SUPERSEDE      0u
#define NFS_DISPOSITION_OPEN           1u
#define NFS_DISPOSITION_CREATE         2u
#define NFS_DISPOSITION_OPEN_IF        3u
#define NFS_DISPOSITION_OVERWRITE      4u
#define NFS_DISPOSITION_OVERWRITE_IF   5u

/* A file handle, the object the FSD keeps per FILE_OBJECT. */
#define NFS_HANDLE_MAX 64

typedef struct _NFS_HANDLE
{
    PNFS_VOLUME Volume;
    nfs_fileid  FileId;
    nfs_u64     Position;
    nfs_u32     InUse;
    nfs_u32     Flags;          /* NFS_HANDLE_*                        */
} NFS_HANDLE, *PNFS_HANDLE;

#define NFS_HANDLE_READ       0x00000001u
#define NFS_HANDLE_WRITE      0x00000002u
#define NFS_HANDLE_DIRECTORY  0x00000004u
#define NFS_HANDLE_CREATED    0x00000008u
#define NFS_HANDLE_DELETE_ON_CLOSE 0x00000010u

/* What FileBasicInformation + FileStandardInformation + FileAllInformation need
   for one file, in one call. */
typedef struct _NFS_STANDARD_INFORMATION
{
    nfs_u64 CreationTime;
    nfs_u64 LastAccessTime;
    nfs_u64 LastWriteTime;
    nfs_u64 ChangeTime;
    nfs_u64 AllocationSize;
    nfs_u64 EndOfFile;
    nfs_u32 Attributes;
    nfs_u32 NumberOfLinks;      /* NextFS has no hard links yet: always 1 */
    nfs_u32 Directory;
    nfs_u32 DeletePending;
} NFS_STANDARD_INFORMATION, *PNFS_STANDARD_INFORMATION;

/* What the FSD shows for a volume. */
typedef struct _NFS_VOLUME_INFO
{
    nfs_u32    ClusterSize;
    nfs_u64    TotalClusters;
    nfs_u64    FreeClusters;
    nfs_u64    TotalBytes;
    nfs_u64    FreeBytes;
    nfs_u64    Generation;
    nfs_u32    Files;
    nfs_u32    Snapshots;
    nfs_u32    Flags;
    char       Label[NFS_LABEL_MAX];
    nfs_u8     Uuid[16];
} NFS_VOLUME_INFO, *PNFS_VOLUME_INFO;

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

/* Rename or move. With ReplaceExisting a target of the same name is deleted
   first, atomically with the rename: one PIFT commit, no delete window. */
int NfsRename(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_fileid NewParent,
              const nfs_u16 *NewName, nfs_u32 NameLength, int ReplaceExisting);
/* Resolve a backslash path ("\dir\sub\file", "." and ".." allowed) and apply an
   NT creation disposition in one step, which is what IRP_MJ_CREATE needs. */
int NfsOpenPath(PNFS_VOLUME Volume, const char *Path, nfs_u32 Disposition,
                int IsDirectory, nfs_fileid *OutFileId, int *OutCreated);
int NfsQueryVolumeInfo(PNFS_VOLUME Volume, NFS_VOLUME_INFO *OutInfo);

/* File handles: what the FSD keeps per FILE_OBJECT. The handle is a slot index
   in the volume, so an IRP handler never holds a pointer that can go stale. */
int  NfsHandleTableInit(PNFS_VOLUME Volume);
void NfsHandleTableDestroy(PNFS_VOLUME Volume);
int  NfsHandleOpen(PNFS_VOLUME Volume, const char *Path, nfs_u32 Disposition,
                   int IsDirectory, nfs_u32 Access, nfs_u32 *OutHandle);
int  NfsHandleRead(PNFS_VOLUME Volume, nfs_u32 Handle, void *Buffer, nfs_u32 Length,
                   nfs_u32 *Read);
int  NfsHandleWrite(PNFS_VOLUME Volume, nfs_u32 Handle, const void *Buffer,
                    nfs_u32 Length, nfs_u32 *Written);
int  NfsHandleSeek(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u64 Offset);
int  NfsHandleQuery(PNFS_VOLUME Volume, nfs_u32 Handle, NFS_PIFT_RECORD *OutRecord);
int  NfsHandleReadDir(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u32 Index,
                      NFS_PIFT_RECORD *OutRecord);
int  NfsHandleSetEndOfFile(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u64 Size);
/* Delete-on-close: the file goes when the last handle closes, which is how NT
   implements IRP_MJ_SET_INFORMATION/FileDispositionInformation. */
int  NfsHandleSetDeleteOnClose(PNFS_VOLUME Volume, nfs_u32 Handle, int DeleteOnClose);
int  NfsHandleClose(PNFS_VOLUME Volume, nfs_u32 Handle);

/* NT wildcard match ("*" and "?", case-insensitive; "*.*" also matches a name
   without a dot, which is the DOS-compatible rule NT keeps). */
int NfsMatchPattern(const nfs_u16 *Name, nfs_u32 NameLength, const char *Pattern);
/* Enumerate a directory with a pattern; Cookie is the resume index (0 to start). */
int NfsEnumerate(PNFS_VOLUME Volume, nfs_fileid Directory, nfs_u32 *Cookie,
                 const char *Pattern, NFS_PIFT_RECORD *OutRecord);
/* A directory with no live children. */
int NfsDirectoryIsEmpty(PNFS_VOLUME Volume, nfs_fileid Directory);
/* Everything the information classes ask for, in one call. */
int NfsQueryStandardInfo(PNFS_VOLUME Volume, nfs_fileid FileId,
                         NFS_STANDARD_INFORMATION *OutInfo);

/* Security descriptors: deduplicated blobs in \\.security, no index. SecurityId
   is the CRC32C of the descriptor, so two files with the same ACL share a blob by
   construction. Query with a NULL buffer to learn the length (NFS_ENOSPC).
   A file with SecurityId 0 has no descriptor: the FSD supplies the default. */
int NfsSecurityQuery(PNFS_VOLUME Volume, nfs_fileid FileId, void *Buffer,
                     nfs_u32 Capacity, nfs_u32 *Length);
int NfsSecuritySet(PNFS_VOLUME Volume, nfs_fileid FileId, const void *Blob,
                   nfs_u32 Length);
/* Drop blobs nobody references any more; returns NFS_OK when there is nothing to do. */
int NfsSecurityGc(PNFS_VOLUME Volume);

/* Attributes and timestamps. */
int NfsSetFileInfo(PNFS_VOLUME Volume, nfs_fileid FileId,
                   const NFS_FILE_INFO *Info);
/* Restore a file from a snapshot: its content, size and timestamps come back.
   If the live file was deleted, it is re-created in the parent it had. */
int NfsRevertFromSnapshot(PNFS_VOLUME Volume, nfs_fileid SnapshotId,
                          nfs_fileid FileId, nfs_fileid *OutFileId);

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

/* Clock: the FSD passes real time (Unix seconds). Without it nextfslib hands out
   a monotonic counter, which keeps records ordered but is not a date. */
typedef nfs_u64 (*NFS_CLOCK_FN)(void);
void NfsSetClock(NFS_CLOCK_FN Clock);
void NfsGetTime(nfs_u64 *Seconds);

/* Allocation hooks: override for kernel pool. Defaults to malloc/free. */
typedef void *(*NFS_ALLOC_FN)(size_t Size);
typedef void (*NFS_FREE_FN)(void *Block);
void NfsSetAllocators(NFS_ALLOC_FN Alloc, NFS_FREE_FN Free);

#ifdef __cplusplus
}
#endif

#endif /* _NEXTFS_FORMAT_H_ */
