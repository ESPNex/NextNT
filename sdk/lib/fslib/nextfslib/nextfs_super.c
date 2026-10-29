/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_super.c
 * PURPOSE:     NextFS format (mkfs), superblock, cluster bitmap
 * LICENSE:     GPL-3.0-or-later
 */

#include "nextfs_internal.h"

#include <stdlib.h>
#include <string.h>

static NFS_ALLOC_FN g_NfsAlloc = NULL;
static NFS_FREE_FN  g_NfsFree = NULL;

void *
NfsMemAlloc(size_t Size)
{
    if (g_NfsAlloc)
        return g_NfsAlloc(Size);
    return malloc(Size);
}

void
NfsMemFree(void *Block)
{
    if (!Block)
        return;
    if (g_NfsFree)
        g_NfsFree(Block);
    else
        free(Block);
}

void
NfsSetAllocators(NFS_ALLOC_FN Alloc, NFS_FREE_FN Free)
{
    g_NfsAlloc = Alloc;
    g_NfsFree = Free;
}

/* ------------------------------------------------------------------ */
/* Device helpers                                                      */
/* ------------------------------------------------------------------ */

int
NfsDevRead(PNFS_VOLUME Volume, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    int Status = Volume->Device.Read(Volume->Device.Context, Offset, Buffer, Length);
    if (Status == NFS_OK)
        Volume->Stats.Reads++;
    return Status;
}

int
NfsDevWrite(PNFS_VOLUME Volume, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    int Status = Volume->Device.Write(Volume->Device.Context, Offset, Buffer, Length);
    if (Status == NFS_OK)
        Volume->Stats.Writes++;
    return Status;
}

int
NfsDevFlush(PNFS_VOLUME Volume)
{
    if (Volume->Device.Flush)
        return Volume->Device.Flush(Volume->Device.Context);
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Superblock                                                          */
/* ------------------------------------------------------------------ */

static nfs_u32
NfsSuperCrc(const NFS_SUPER *Super)
{
    NFS_SUPER Copy = *Super;

    Copy.Crc32c = 0;
    return NfsCrc32c(0, &Copy, NFS_SUPER_SIZE - sizeof(nfs_u32));
}

static int
NfsSuperValid(const NFS_SUPER *Super, nfs_u64 DeviceBytes)
{
    if (memcmp(Super->Magic, NFS_SUPER_MAGIC, 4) != 0)
        return 0;
    if (Super->Version != NFS_FORMAT_VERSION)
        return 0;
    if (Super->HeaderSize != NFS_SUPER_SIZE)
        return 0;
    if (Super->ClusterSize < NFS_CLUSTER_MIN ||
        (Super->ClusterSize & (Super->ClusterSize - 1)) != 0)
        return 0;
    if (Super->TotalClusters == 0 ||
        Super->TotalClusters * Super->ClusterSize > DeviceBytes)
        return 0;
    if (Super->Crc32c != NfsSuperCrc(Super))
        return 0;
    return 1;
}

int
NfsReadSuper(PNFS_BDEV Device, PNFS_SUPER Super)
{
    NFS_SUPER Primary, Backup;
    nfs_u64 DeviceBytes = 0;
    nfs_u64 BackupOffset;
    int PrimaryOk, BackupOk;

    if (Device->Size)
        Device->Size(Device->Context, &DeviceBytes);

    if (Device->Read(Device->Context, 0, &Primary, NFS_SUPER_SIZE) != NFS_OK)
        return NFS_EIO;
    PrimaryOk = NfsSuperValid(&Primary, DeviceBytes);

    BackupOffset = 0;
    if (PrimaryOk)
    {
        /* The backup lives in the last cluster of the volume. */
        BackupOffset = (Primary.TotalClusters - 1) * Primary.ClusterSize;
    }
    else if (DeviceBytes >= NFS_CLUSTER_MIN)
    {
        /* Try the tail of the device with the smallest legal cluster. */
        BackupOffset = ((DeviceBytes / NFS_CLUSTER_MIN) - 1) * NFS_CLUSTER_MIN;
    }

    BackupOk = 0;
    if (BackupOffset != 0)
    {
        if (Device->Read(Device->Context, BackupOffset, &Backup, NFS_SUPER_SIZE) != NFS_OK)
            memset(&Backup, 0, sizeof(Backup));
        BackupOk = NfsSuperValid(&Backup, DeviceBytes);
    }

    if (PrimaryOk && BackupOk)
        *Super = (Primary.Generation >= Backup.Generation) ? Primary : Backup;
    else if (PrimaryOk)
        *Super = Primary;
    else if (BackupOk)
        *Super = Backup;
    else
        return NFS_ECORRUPT;

    return NFS_OK;
}

int
NfsWriteSuper(PNFS_VOLUME Volume)
{
    nfs_u64 BackupOffset;
    int Status;

    Volume->Super.Crc32c = NfsSuperCrc(&Volume->Super);

    Status = NfsDevWrite(Volume, 0, &Volume->Super, NFS_SUPER_SIZE);
    if (Status != NFS_OK)
        return Status;
    Status = NfsDevFlush(Volume);
    if (Status != NFS_OK)
        return Status;

    BackupOffset = (Volume->Super.TotalClusters - 1) * Volume->Super.ClusterSize;
    Status = NfsDevWrite(Volume, BackupOffset, &Volume->Super, NFS_SUPER_SIZE);
    if (Status != NFS_OK)
        return Status;

    return NfsDevFlush(Volume);
}

const NFS_SUPER *
NfsGetSuper(PNFS_VOLUME Volume)
{
    return &Volume->Super;
}

/* ------------------------------------------------------------------ */
/* Cluster bitmap                                                      */
/* ------------------------------------------------------------------ */

void
NfsClusterMarkUsed(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count)
{
    nfs_u64 i;

    for (i = 0; i < Count; i++)
    {
        nfs_u64 Cluster = FirstCluster + i;
        if (Cluster >= Volume->Super.TotalClusters)
            return;
        if (!(Volume->Bitmap[(size_t)(Cluster >> 3)] & (1u << (Cluster & 7))))
        {
            Volume->Bitmap[(size_t)(Cluster >> 3)] |= (nfs_u8)(1u << (Cluster & 7));
            if (Volume->Super.FreeClusters > 0)
                Volume->Super.FreeClusters--;
        }
    }
}

void
NfsClusterMarkFree(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count)
{
    nfs_u64 i;

    for (i = 0; i < Count; i++)
    {
        nfs_u64 Cluster = FirstCluster + i;
        if (Cluster >= Volume->Super.TotalClusters)
            return;
        if (Volume->Bitmap[(size_t)(Cluster >> 3)] & (1u << (Cluster & 7)))
        {
            Volume->Bitmap[(size_t)(Cluster >> 3)] &= (nfs_u8)~(1u << (Cluster & 7));
            Volume->Super.FreeClusters++;
        }
    }
}

static int
NfsClusterIsUsed(PNFS_VOLUME Volume, nfs_u64 Cluster)
{
    return (Volume->Bitmap[(size_t)(Cluster >> 3)] >> (Cluster & 7)) & 1;
}

int
NfsClusterAlloc(PNFS_VOLUME Volume, nfs_u64 Count, nfs_u64 *FirstCluster)
{
    nfs_u64 Cluster = 0, RunStart = 0, Run = 0;

    if (Count == 0)
        return NFS_EINVAL;
    if (Count > Volume->Super.FreeClusters)
        return NFS_ENOSPC;

    /* First fit. The allocator keeps a hint in phase 2. */
    for (Cluster = 0; Cluster < Volume->Super.TotalClusters; Cluster++)
    {
        if (NfsClusterIsUsed(Volume, Cluster))
        {
            Run = 0;
            continue;
        }

        if (Run == 0)
            RunStart = Cluster;
        Run++;

        if (Run == Count)
        {
            NfsClusterMarkUsed(Volume, RunStart, Count);
            *FirstCluster = RunStart;
            return NFS_OK;
        }
    }

    return NFS_ENOSPC;
}

void
NfsClusterFree(PNFS_VOLUME Volume, nfs_u64 FirstCluster, nfs_u64 Count)
{
    NfsClusterMarkFree(Volume, FirstCluster, Count);
}

int
NfsBitmapLoad(PNFS_VOLUME Volume)
{
    nfs_u64 Offset = Volume->Super.BitmapFirst * Volume->Super.ClusterSize;
    nfs_u32 Length = Volume->Super.BitmapClusters * Volume->Super.ClusterSize;
    int Status;

    Volume->BitmapBytes = (nfs_u32)((Volume->Super.TotalClusters + 7) / 8);

    /* The on-disk bitmap occupies whole clusters; the RAM copy only needs the
       bits, so read into a buffer big enough for both. */
    {
        nfs_u32 Allocation = (Length > Volume->BitmapBytes) ? Length : Volume->BitmapBytes;

        Volume->Bitmap = (nfs_u8 *)NfsMemAlloc(Allocation);
        if (!Volume->Bitmap)
            return NFS_ENOSPC;
        memset(Volume->Bitmap, 0, Allocation);
    }

    Status = NfsDevRead(Volume, Offset, Volume->Bitmap, Length);
    if (Status != NFS_OK)
    {
        NfsMemFree(Volume->Bitmap);
        Volume->Bitmap = NULL;
        return Status;
    }
    return NFS_OK;
}

int
NfsBitmapFlush(PNFS_VOLUME Volume)
{
    nfs_u64 Offset = Volume->Super.BitmapFirst * Volume->Super.ClusterSize;
    nfs_u32 Length = Volume->Super.BitmapClusters * Volume->Super.ClusterSize;
    nfs_u8 *Buffer;
    int Status;

    Buffer = (nfs_u8 *)NfsMemAlloc(Length);
    if (!Buffer)
        return NFS_ENOSPC;
    memset(Buffer, 0, Length);
    memcpy(Buffer, Volume->Bitmap, Volume->BitmapBytes);

    Status = NfsDevWrite(Volume, Offset, Buffer, Length);
    NfsMemFree(Buffer);
    if (Status != NFS_OK)
        return Status;
    return NfsDevFlush(Volume);
}

/* ------------------------------------------------------------------ */
/* Format                                                              */
/* ------------------------------------------------------------------ */

static void
NfsWriteBootStub(PNFS_BDEV Device, nfs_u32 ClusterSize)
{
    nfs_u8 Stub[512];

    memset(Stub, 0, sizeof(Stub));
    Stub[0] = 0xEB;   /* jump short */
    Stub[1] = 0x3C;
    Stub[2] = 0x90;
    memcpy(&Stub[3], NFS_BOOT_OEM, 8);   /* "NEXTFS  " */
    Stub[510] = 0x55;
    Stub[511] = 0xAA;

    Device->Write(Device->Context, 0, Stub, sizeof(Stub));
    (void)ClusterSize;
}

int
NfsFormat(PNFS_BDEV Device, const char *Label, nfs_u32 ClusterSize)
{
    NFS_VOLUME Volume;
    NFS_SUPER *Super = &Volume.Super;
    NFS_PIFT_RECORD Root;
    nfs_u64 DeviceBytes = 0;
    nfs_u64 TotalClusters, Reserved, Cluster;
    nfs_u64 BitmapClusters, JournalClusters, PiftClusters, PiftAreaClusters;
    nfs_u64 PiftAreaFirst;
    nfs_u32 i;

    if (!Device || !Device->Read || !Device->Write)
        return NFS_EINVAL;
    if (!Device->Size || Device->Size(Device->Context, &DeviceBytes) != NFS_OK)
        return NFS_EINVAL;

    if (ClusterSize == 0)
        ClusterSize = NFS_CLUSTER_MIN;
    if (ClusterSize < NFS_CLUSTER_MIN || (ClusterSize & (ClusterSize - 1)) != 0)
        return NFS_EINVAL;

    TotalClusters = DeviceBytes / ClusterSize;
    if (TotalClusters < 64)
        return NFS_EINVAL;   /* too small to hold a volume */

    BitmapClusters = ((TotalClusters + 7) / 8 + ClusterSize - 1) / ClusterSize;
    JournalClusters = TotalClusters / 1024;
    if (JournalClusters < 8)
        JournalClusters = 8;
    if (JournalClusters > 8192)
        JournalClusters = 8192;

    /* [0] stub, [1] super, [2..] bitmap, journal, PIFT-on-disk area, then the
       data area, and the last cluster holds the superblock backup. The PIFT
       area is bounded (1/16 of the volume, 16..4096 clusters): the rest is
       file data and copy-on-write metadata. */
    PiftAreaClusters = TotalClusters / 16;
    if (PiftAreaClusters < 32)
        PiftAreaClusters = 32;
    if (PiftAreaClusters > 8192)
        PiftAreaClusters = 8192;

    /* Two halves, one per live generation: a checkpoint switches writes to the
       half the fallback generation is not using, so the fallback always finds
       the records that were appended after it was built. */
    PiftClusters = PiftAreaClusters / 2;

    Reserved = 2 + BitmapClusters + JournalClusters + PiftAreaClusters + 1;
    if (TotalClusters <= Reserved + 16)
        return NFS_EINVAL;

    PiftAreaFirst = 2 + BitmapClusters + JournalClusters;

    memset(&Volume, 0, sizeof(Volume));
    Volume.Device = *Device;

    memset(Super, 0, sizeof(*Super));
    memcpy(Super->Magic, NFS_SUPER_MAGIC, 4);
    Super->Version = NFS_FORMAT_VERSION;
    Super->HeaderSize = NFS_SUPER_SIZE;
    Super->ClusterSize = ClusterSize;
    Super->TotalClusters = TotalClusters;
    Super->Generation = 1;
    for (i = 0; i < sizeof(Super->VolumeUuid); i++)
        Super->VolumeUuid[i] = (nfs_u8)(0xA5u ^ (i * 37u) ^ (DeviceBytes & 0xFF));

    Super->JournalFirst = 2 + BitmapClusters;
    Super->JournalClusters = (nfs_u32)JournalClusters;
    Super->JournalSeqFirst = 1;
    Super->JournalSeqNext = 1;
    Super->PiftAreaFirst = PiftAreaFirst;
    Super->PiftAreaClusters = (nfs_u32)PiftAreaClusters;
    Super->PiftFirst = PiftAreaFirst + ((Super->Generation & 1) ? PiftClusters : 0);
    Super->PiftClusters = (nfs_u32)PiftClusters;
    Super->PiftHead = 0;
    Super->PiftSeqNext = 1;
    Super->BitmapFirst = 2;
    Super->BitmapClusters = (nfs_u32)BitmapClusters;
    Super->Flags = NFS_SB_DIRTY;
    if (Label)
    {
        size_t Length = strlen(Label);
        if (Length >= NFS_LABEL_MAX)
            Length = NFS_LABEL_MAX - 1;
        memcpy(Super->Label, Label, Length);
    }

    /* Bitmap in memory: everything reserved first. */
    if (NfsBitmapLoad(&Volume) != NFS_OK)
        return NFS_ENOSPC;
    memset(Volume.Bitmap, 0, Volume.BitmapBytes);   /* a new volume is all free */
    Volume.Super.FreeClusters = TotalClusters;
    NfsClusterMarkUsed(&Volume, 0, PiftAreaFirst + PiftAreaClusters);
    NfsClusterMarkUsed(&Volume, TotalClusters - 1, 1);   /* super backup */

    /* Root directory record. */
    memset(&Root, 0, sizeof(Root));
    Root.FileId = NFS_ROOT_ID;
    Root.Generation = 1;
    Root.Parent = NFS_INVALID_ID;
    Root.Seq = 0;
    Root.Flags = NFS_FLAG_DIR;
    Root.Attributes = 0x10;   /* FILE_ATTRIBUTE_DIRECTORY */
    Root.NameLen = 0;
    Root.ExtentCount = 0;

    if (NfsIndexInit(&Volume) != NFS_OK)
    {
        NfsMemFree(Volume.Bitmap);
        return NFS_ENOSPC;
    }
    if (NfsIndexInsert(&Volume, &Root) != NFS_OK)
    {
        NfsIndexDestroy(&Volume);
        NfsMemFree(Volume.Bitmap);
        return NFS_ENOSPC;
    }

    /* Root metadata goes into the first checkpoint chain leaf. */
    {
        NFS_PIFT_RECORD Records[1];
        Records[0] = Root;
        if (NfsCommitRecords(&Volume, Records, 1) != NFS_OK)
        {
            NfsIndexDestroy(&Volume);
            NfsMemFree(Volume.Bitmap);
            return NFS_EIO;
        }
    }

    /* Compact the root record into the first checkpoint chain. */
    if (NfsCheckpoint(&Volume) != NFS_OK)
    {
        NfsIndexDestroy(&Volume);
        NfsMemFree(Volume.Bitmap);
        return NFS_EIO;
    }

    NfsWriteBootStub(Device, ClusterSize);
    NfsBitmapFlush(&Volume);
    Super->Flags = 0;             /* a formatted volume is clean */
    Super->Generation++;
    NfsWriteSuper(&Volume);

    NfsIndexDestroy(&Volume);
    NfsMemFree(Volume.Bitmap);
    (void)Cluster;
    return NFS_OK;
}
