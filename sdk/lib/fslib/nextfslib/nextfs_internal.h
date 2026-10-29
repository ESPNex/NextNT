/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_internal.h
 * PURPOSE:     Internal state shared by the nextfslib translation units
 * LICENSE:     GPL-3.0-or-later
 */

#ifndef _NEXTFS_INTERNAL_H_
#define _NEXTFS_INTERNAL_H_

#include "nextfs_format.h"

#define NFS_HASH_EMPTY  0xFFFFFFFFu
#define NFS_HASH_TOMB   0xFFFFFFFEu

/* Journal/PIFT area frames are 4 KiB: 32 byte header + payload. */
#define NFS_FRAME_PAYLOAD_MAX (NFS_FRAME_SIZE - NFS_FRAME_HDR_SIZE)

/* High-water mark: compact when the PIFT area is more than half full. */
#define NFS_PIFT_HIGH_WATER(Super) \
    (((nfs_u64)(Super).PiftClusters * (Super).ClusterSize) / 2)

/* Sorted, per-directory child set kept in RAM (the on-disk namespace tree is
   only needed for checkpoints, so the runtime index lives here). */
typedef struct _NFS_CHILD_SET
{
    nfs_fileid Parent;
    nfs_u32 *Indices;        /* record indices, sorted by case-folded name */
    nfs_u32  Count;
    nfs_u32  Capacity;
    struct _NFS_CHILD_SET *Next;
} NFS_CHILD_SET, *PNFS_CHILD_SET;

#define NFS_CHILD_BUCKETS 512

/* An interval of clusters held alive by snapshot generations. Intervals are
   sorted by Start, never overlap, and every one has Count > 0. */
typedef struct _NFS_PIN
{
    nfs_u64 Start;
    nfs_u64 Length;
    nfs_u32 Count;
    nfs_u32 Pad;
} NFS_PIN, *PNFS_PIN;

struct _NFS_VOLUME
{
    NFS_BDEV Device;                 /* callbacks, copied at mount       */

    NFS_SUPER Super;

    nfs_u8  *Bitmap;                 /* 1 bit per cluster                */
    nfs_u32  BitmapBytes;

    NFS_PIFT_RECORD *Records;        /* append-only array of records     */
    nfs_u32  RecordCount;
    nfs_u32  RecordCapacity;

    nfs_u32 *Hash;                   /* fileid -> record index + 1       */
    nfs_u32  HashSize;               /* power of two                     */
    nfs_u32  HashUsed;

    nfs_u64  JournalFrames;
    nfs_u64  Transactions;           /* since the last checkpoint        */

    nfs_u64  NextFileId;             /* fileid allocator                 */

    PNFS_CHILD_SET Children[NFS_CHILD_BUCKETS];

    NFS_STATS Stats;

    NFS_PIN *Pins;                   /* snapshot pin intervals, sorted   */
    nfs_u32  PinCount;
    nfs_u32  PinCapacity;
};

/* Superblock + device helpers (nextfs_super.c) */
int NfsDevRead(PNFS_VOLUME Volume, nfs_u64 Offset, void *Buffer, nfs_u32 Length);
int NfsDevWrite(PNFS_VOLUME Volume, nfs_u64 Offset, const void *Buffer, nfs_u32 Length);
int NfsDevFlush(PNFS_VOLUME Volume);
int NfsReadSuper(PNFS_BDEV Device, PNFS_SUPER Super);
int NfsWriteSuper(PNFS_VOLUME Volume);
int NfsMountReplay(PNFS_VOLUME Volume);

/* Bitmap allocator (nextfs_super.c) */
int NfsClusterAlloc(PNFS_VOLUME Volume, nfs_u64 Count, nfs_u64 *FirstCluster);
void NfsClusterFree(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count);
void NfsClusterMarkUsed(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count);
void NfsClusterMarkFree(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count);
int NfsBitmapLoad(PNFS_VOLUME Volume);
int NfsBitmapFlush(PNFS_VOLUME Volume);

/* PIFT records and the RAM index (nextfs_pift.c) */
nfs_u32 NfsPiftSerialize(const NFS_PIFT_RECORD *Record, void *Buffer, nfs_u32 Capacity);
int NfsPiftDeserialize(const void *Buffer, nfs_u32 Length, NFS_PIFT_RECORD *Record);
nfs_u64 NfsPiftRecordSize(const NFS_PIFT_RECORD *Record);

int NfsIndexInit(PNFS_VOLUME Volume);
void NfsIndexDestroy(PNFS_VOLUME Volume);
int NfsIndexInsert(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record);
int NfsIndexInsertEx(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record,
                     nfs_u32 *OutIndex);
int NfsIndexLocate(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_u32 *OutIndex);

/* Sorted child index (nextfs_pift.c) */
int  NfsChildrenInit(PNFS_VOLUME Volume);
void NfsChildrenDestroy(PNFS_VOLUME Volume);
int  NfsChildrenAdd(PNFS_VOLUME Volume, nfs_u32 RecordIndex);
void NfsChildrenRemove(PNFS_VOLUME Volume, nfs_u32 RecordIndex);
int  NfsChildrenRebuild(PNFS_VOLUME Volume);
NFS_PIFT_RECORD *NfsChildrenLookup(PNFS_VOLUME Volume, nfs_fileid Parent,
                                   const nfs_u16 *Name, nfs_u32 NameLength);
NFS_PIFT_RECORD *NfsChildrenAt(PNFS_VOLUME Volume, nfs_fileid Parent, nfs_u32 Index);

/* Generic data mapping (nextfs_volume.c) */
int  NfsWriteRange(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Record, nfs_u64 Offset,
                   const void *Buffer, nfs_u32 Length, NFS_PIFT_RECORD *Updated);
int  NfsTruncateRecord(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Record, nfs_u64 NewSize,
                       NFS_PIFT_RECORD *Updated);
int  NfsMapClusterRecord(PNFS_VOLUME Volume, const NFS_PIFT_RECORD *Record,
                         nfs_u64 Vcn, nfs_u64 *Lcn);
int  NfsCoalesceExtents(NFS_PIFT_RECORD *Record);
NFS_PIFT_RECORD *NfsIndexGet(PNFS_VOLUME Volume, nfs_fileid FileId);
nfs_u32 NfsIndexHashFileId(nfs_fileid FileId);

/* Journal (nextfs_journal.c) */
nfs_u64 NfsJournalNextSeq(PNFS_VOLUME Volume);
int NfsJournalWriteFrame(PNFS_VOLUME Volume, nfs_seq Seq, nfs_u32 Flags,
                         const void *Payload, nfs_u32 PayloadLen);
int NfsJournalCommit(PNFS_VOLUME Volume, nfs_seq Seq);
int NfsJournalReplay(PNFS_VOLUME Volume);
int NfsJournalScanCommitted(PNFS_VOLUME Volume, nfs_u64 *Seqs, nfs_u32 Max,
                            nfs_u32 *Count);
int NfsSeqIsCommitted(PNFS_VOLUME Volume, nfs_seq Seq);
int NfsPiftAppend(PNFS_VOLUME Volume, const void *Frames, nfs_u32 Length);
int NfsPiftScan(PNFS_VOLUME Volume);

/* Copy-on-write B+tree for the checkpoint store (nextfs_tree.c) */
int NfsTreeBuild(PNFS_VOLUME Volume, nfs_u64 Generation, nfs_u64 *OutRoot);
int NfsTreeWalk(PNFS_VOLUME Volume, nfs_u64 Root,
                int (*Callback)(void *Context, const NFS_PIFT_RECORD *Record),
                void *Context);
int NfsTreeCollect(PNFS_VOLUME Volume, nfs_u64 Root, nfs_u64 *Blocks,
                   nfs_u32 Max, nfs_u32 *Count);

/* Snapshot pin set (nextfs_snapshot.c) */
int  NfsPinInit(PNFS_VOLUME Volume);
void NfsPinDestroy(PNFS_VOLUME Volume);
int  NfsPinAddRange(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length);
int  NfsPinSubRange(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length,
                    void (*OnZero)(void *Context, nfs_u64 Start, nfs_u64 Length),
                    void *Context);
void NfsPinFreeGaps(PNFS_VOLUME Volume, nfs_u64 Start, nfs_u64 Length);
nfs_u32 NfsPinCoveredBy(PNFS_VOLUME Volume, nfs_u64 Cluster);
int  NfsSnapshotPinsRebuild(PNFS_VOLUME Volume);
int  NfsSnapshotRecover(PNFS_VOLUME Volume);

/* Checkpointing + transactions (nextfs_volume.c) */
int NfsCheckpoint(PNFS_VOLUME Volume);
int NfsCommitRecords(PNFS_VOLUME Volume, NFS_PIFT_RECORD *Records, nfs_u32 Count);
void NfsSetDirty(PNFS_VOLUME Volume, int Dirty);

#endif /* _NEXTFS_INTERNAL_H_ */
