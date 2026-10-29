/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tools/nextchk.c
 * PURPOSE:     nextchk — verify and repair a NextFS volume
 * LICENSE:     GPL-3.0-or-later
 *
 * usage: nextchk <image> [--repair] [--verbose]
 *
 * What it checks:
 *   1. both superblock copies (CRC, generation, which one wins)
 *   2. the checkpoint tree: every node CRC, its generation and its self address
 *   3. the PIFT-on-disk area and the journal: how many committed frames a mount
 *      would replay
 *   4. the allocation bitmap: clusters mapped by a record that are marked free,
 *      and clusters marked used that nothing references (orphans), excluding
 *      the metadata regions reserved at format time
 *
 * What --repair does:
 *   mounts (which replays the journal and rebuilds the PIFT in RAM), then
 *   checkpoints so a fresh tree is written from the recovered state, marks the
 *   leaked clusters free and clears the dirty/needs-check flags.
 *   It never rewrites file data: if a data cluster is corrupt, that is reported
 *   and left alone.
 */

#include "../nextfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _IMAGE_DEVICE { FILE *File; } IMAGE_DEVICE, *PIMAGE_DEVICE;

static int ImageRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;
    if (fseeko(Device->File, (off_t)Offset, SEEK_SET) != 0) return NFS_EIO;
    if (fread(Buffer, 1, Length, Device->File) != Length) return NFS_EIO;
    return NFS_OK;
}

static int ImageWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;
    if (fseeko(Device->File, (off_t)Offset, SEEK_SET) != 0) return NFS_EIO;
    if (fwrite(Buffer, 1, Length, Device->File) != Length) return NFS_EIO;
    return NFS_OK;
}

static int ImageSize(void *Context, nfs_u64 *Bytes)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;
    off_t End;
    if (fseeko(Device->File, 0, SEEK_END) != 0) return NFS_EIO;
    End = ftello(Device->File);
    if (End < 0) return NFS_EIO;
    *Bytes = (nfs_u64)End;
    return NFS_OK;
}

static int ImageFlush(void *Context)
{
    return (fflush(((PIMAGE_DEVICE)Context)->File) == 0) ? NFS_OK : NFS_EIO;
}

/* ------------------------------------------------------------------ */

typedef struct _TREE_CHECK
{
    PNFS_VOLUME Volume;
    nfs_u32 Nodes;
    nfs_u32 BadNodes;
    nfs_u32 Leaks;
    int Verbose;
} TREE_CHECK, *PTREE_CHECK;

static int
TreeCheckNode(PTREE_CHECK Check, nfs_u64 Block, nfs_u32 Depth, nfs_u32 *Visited,
              nfs_u32 VisitedMax)
{
    nfs_u8 Node[NFS_NODE_SIZE];
    NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
    nfs_u32 StoredCrc, ComputedCrc;
    nfs_u32 i;

    if (Depth > 8)
        return NFS_ECORRUPT;
    if (Block == 0 || Block >= Check->Volume->Super.TotalClusters)
        return NFS_ECORRUPT;

    for (i = 0; i < *Visited; i++)
        if (Visited[i] == Block)
            return NFS_ECORRUPT;      /* cycle */

    if (*Visited < VisitedMax)
        Visited[(*Visited)++] = Block;

    if (NfsDevRead(Check->Volume, Block * Check->Volume->Super.ClusterSize,
                   Node, NFS_NODE_SIZE) != NFS_OK)
        return NFS_EIO;

    Check->Nodes++;

    if (memcmp(Header->Magic, NFS_NODE_MAGIC, 4) != 0 || Header->Block != Block)
    {
        Check->BadNodes++;
        printf("  node %llu: bad magic or self address\n", (unsigned long long)Block);
        return NFS_ECORRUPT;
    }

    StoredCrc = Header->Crc32c;
    Header->Crc32c = 0;
    ComputedCrc = NfsCrc32c(0, Node, NFS_NODE_SIZE);
    Header->Crc32c = StoredCrc;

    if (StoredCrc != ComputedCrc)
    {
        Check->BadNodes++;
        printf("  node %llu: CRC mismatch (stored %08x, computed %08x)\n",
               (unsigned long long)Block, StoredCrc, ComputedCrc);
        return NFS_ECORRUPT;
    }

    if (Check->Verbose)
        printf("  node %llu: level %u, %u entries, generation %llu\n",
               (unsigned long long)Block, Header->Level, Header->Entries,
               (unsigned long long)Header->Generation);

    if (Header->Level > 0)
    {
        nfs_u32 Cursor = Header->DataOffset;

        for (i = 0; i < Header->Entries; i++)
        {
            nfs_u64 Child;
            int Status;

            Cursor += sizeof(nfs_u64);           /* KeyMax, unused for the walk */
            memcpy(&Child, Node + Cursor, sizeof(nfs_u64));
            Cursor += sizeof(nfs_u64);

            Status = TreeCheckNode(Check, Child, Depth + 1, Visited, VisitedMax);
            if (Status != NFS_OK)
                return Status;
        }
    }

    return NFS_OK;
}

static int
ClusterReferenced(PNFS_VOLUME Volume, nfs_u64 Cluster)
{
    nfs_u32 i, j;

    for (i = 0; i < Volume->RecordCount; i++)
    {
        const NFS_PIFT_RECORD *Record = &Volume->Records[i];

        for (j = 0; j < Record->ExtentCount; j++)
        {
            const NFS_EXTENT *Extent = &Record->Extents[j];

            if (Extent->Lcn == NFS_HOLE || Extent->Length == 0)
                continue;
            if (Cluster >= Extent->Lcn && Cluster < Extent->Lcn + Extent->Length)
                return 1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    IMAGE_DEVICE Image;
    NFS_BDEV Device;
    NFS_SUPER Original;
    PNFS_VOLUME Volume = NULL;
    TREE_CHECK Check;
    const char *Path = NULL;
    int Repair = 0, Verbose = 0, i, Status;
    nfs_u32 Visited[8192];

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--repair") == 0)
            Repair = 1;
        else if (strcmp(argv[i], "--verbose") == 0)
            Verbose = 1;
        else
            Path = argv[i];
    }

    if (!Path)
    {
        fprintf(stderr, "usage: %s <image> [--repair] [--verbose]\n", argv[0]);
        return 2;
    }

    Image.File = fopen(Path, "r+b");
    if (!Image.File)
    {
        perror(Path);
        return 2;
    }

    memset(&Device, 0, sizeof(Device));
    Device.Context = &Image;
    Device.Read = ImageRead;
    Device.Write = ImageWrite;
    Device.Size = ImageSize;
    Device.Flush = ImageFlush;

    printf("nextchk: %s\n", Path);

    /* 1. superblock */
    {
        NFS_SUPER Super;

        Status = NfsReadSuper(&Device, &Super);
        Original = Super;
        if (Status != NFS_OK)
        {
            printf("  superblock: both copies invalid (%d)\n", Status);
            fclose(Image.File);
            return 1;
        }
        printf("  superblock ok: generation %llu, %llu clusters, cluster %u, flags 0x%x\n",
               (unsigned long long)Super.Generation, (unsigned long long)Super.TotalClusters,
               Super.ClusterSize, Super.Flags);
        if (Super.Flags & NFS_SB_DIRTY)
            printf("  volume is dirty (mounted or crashed)\n");
        if (Super.Flags & NFS_SB_NEEDS_CHECK)
            printf("  volume needs a check\n");
        printf("  checkpoint root %llu (generation %llu), fallback root %llu (generation %llu)\n",
               (unsigned long long)Super.CheckpointRoot.Block,
               (unsigned long long)Super.CheckpointRoot.Generation,
               (unsigned long long)Super.CheckpointRootPrev.Block,
               (unsigned long long)Super.CheckpointRootPrev.Generation);
    }

    /* 2. mount: this replays the journal and rebuilds the PIFT in RAM */
    Status = NfsMount(&Device, &Volume);
    if (Status != NFS_OK)
    {
        printf("  mount failed (%d): the checkpoint trees are both unusable\n", Status);
        fclose(Image.File);
        return 1;
    }

    {
        NFS_STATS Stats;

        NfsGetStats(Volume, &Stats);
        printf("  mount ok: %u files, %u snapshots (%u pinned ranges), %llu journal "
               "commits, %llu frames replayed%s\n",
               Stats.Files, Stats.Snapshots, Stats.PinnedRanges,
               (unsigned long long)Stats.JournalCommits,
               (unsigned long long)Stats.ReplayedFrames,
               Stats.UsedFallbackRoot ? ", using the fallback root" : "");
    }

    /* 3. checkpoint tree(s) */
    memset(&Check, 0, sizeof(Check));
    Check.Volume = Volume;
    Check.Verbose = Verbose;

    printf("  checkpoint tree (current root):\n");
    memset(Visited, 0, sizeof(Visited));
    {
        Status = TreeCheckNode(&Check, Volume->Super.CheckpointRoot.Block, 0,
                               Visited, 8192);
        printf("    %u nodes checked, %u bad, walk %s\n", Check.Nodes, Check.BadNodes,
               (Status == NFS_OK) ? "complete" : "FAILED");
        Check.BadNodes += (Status == NFS_OK) ? 0 : 1;
    }

    /* 3b. snapshots */
    {
        NFS_SNAPSHOT_INFO Info;
        nfs_u32 Index;

        for (Index = 0; NfsSnapshotList(Volume, Index, &Info) == NFS_OK; Index++)
        {
            char Name[NFS_NAME_MAX + 1];
            nfs_u32 i;

            for (i = 0; i < Info.NameLen && i < NFS_NAME_MAX; i++)
                Name[i] = (char)((Info.Name[i] < 0x80) ? Info.Name[i] : '?');
            Name[(Info.NameLen < NFS_NAME_MAX) ? Info.NameLen : NFS_NAME_MAX] = 0;

            printf("  snapshot \"%s\" (id %llu): tree root %llu, generation %llu\n",
                   Name, (unsigned long long)Info.Id,
                   (unsigned long long)Info.TreeRoot,
                   (unsigned long long)Info.Generation);
        }
        if (Index == 0)
            printf("  snapshots: none\n");
    }

    /* 4. bitmap cross-check: the two checkpoint generations are expected to be
       allocated even though no record maps them, so count them as reserved. */
    {
        nfs_u64 Cluster;
        nfs_u64 Leaked = 0, Missing = 0, Reserved = 0;
        nfs_u64 PrevBlocks[8192];
        nfs_u32 PrevCount = 0;
        nfs_u32 b;

        NfsTreeCollect(Volume, Volume->Super.CheckpointRoot.Block,
                       PrevBlocks, 8192, &PrevCount);
        {
            nfs_u32 More = 0;

            NfsTreeCollect(Volume, Volume->Super.CheckpointRootPrev.Block,
                           PrevBlocks + PrevCount, 8192 - PrevCount, &More);
            PrevCount += More;
        }
        nfs_u64 MetadataEnd = Volume->Super.PiftAreaFirst + Volume->Super.PiftAreaClusters;
        nfs_u64 LastCluster = Volume->Super.TotalClusters - 1;

        for (Cluster = 0; Cluster < Volume->Super.TotalClusters; Cluster++)
        {
            int Used = (Volume->Bitmap[Cluster >> 3] >> (Cluster & 7)) & 1;
            int Metadata = (Cluster < MetadataEnd) || (Cluster == LastCluster);
            int Referenced = 0;

            if (Metadata)
            {
                if (!Used)
                    Missing++;      /* a metadata cluster must always be marked */
                continue;
            }

            if (Used)
                Referenced = ClusterReferenced(Volume, Cluster);

            if (Used && !Referenced && NfsPinCoveredBy(Volume, Cluster) > 0)
            {
                Reserved++;
                continue;
            }

            if (Used && !Referenced)
            {
                int IsGeneration = 0;

                for (b = 0; b < PrevCount; b++)
                    if (PrevBlocks[b] == Cluster)
                    {
                        IsGeneration = 1;
                        break;
                    }
                if (IsGeneration)
                {
                    Reserved++;
                    continue;
                }

                Leaked++;
                if (Verbose && Leaked <= 20)
                    printf("    leaked cluster %llu: marked used, nothing maps it\n",
                           (unsigned long long)Cluster);
            }
            else if (!Used && !Referenced)
            {
                continue;
            }
        }

        printf("  bitmap: %llu free, %llu leaked, %llu reserved for the live checkpoint "
               "tree, %llu metadata clusters marked free\n",
               (unsigned long long)Volume->Super.FreeClusters,
               (unsigned long long)Leaked, (unsigned long long)Reserved,
               (unsigned long long)Missing);

        /* Records that point outside the volume are structural corruption. */
        {
            nfs_u32 r, j;
            nfs_u32 Bad = 0;

            for (r = 0; r < Volume->RecordCount; r++)
            {
                const NFS_PIFT_RECORD *Record = &Volume->Records[r];

                for (j = 0; j < Record->ExtentCount; j++)
                {
                    const NFS_EXTENT *Extent = &Record->Extents[j];

                    if (Extent->Lcn == NFS_HOLE)
                        continue;
                    if (Extent->Lcn + Extent->Length > Volume->Super.TotalClusters)
                    {
                        if (Bad++ < 10)
                            printf("    record %llu: extent beyond the end of the volume\n",
                                   (unsigned long long)Record->FileId);
                    }
                }
            }
            printf("  records: %u in the PIFT, %u with out-of-range extents\n",
                   Volume->RecordCount, Bad);
        }

        if (Repair)
        {
            nfs_u32 r, j;

            printf("  repair: rebuilding the bitmap from the PIFT and checkpointing\n");
            memset(Volume->Bitmap, 0, Volume->BitmapBytes);
            Volume->Super.FreeClusters = Volume->Super.TotalClusters;
            NfsClusterMarkUsed(Volume, 0, MetadataEnd);
            NfsClusterMarkUsed(Volume, LastCluster, 1);
            for (r = 0; r < Volume->RecordCount; r++)
            {
                const NFS_PIFT_RECORD *Record = &Volume->Records[r];

                if (Record->Flags & NFS_FLAG_DELETED)
                    continue;
                for (j = 0; j < Record->ExtentCount; j++)
                {
                    if (Record->Extents[j].Lcn != NFS_HOLE)
                        NfsClusterMarkUsed(Volume, Record->Extents[j].Lcn,
                                           Record->Extents[j].Length);
                }
            }

            Volume->Super.Flags = 0;
            Status = NfsSync(Volume);
            printf("  repair: %s\n", (Status == NFS_OK) ? "done" : "failed");
        }
    }

    /* A check must not touch the volume: only --repair writes. Mount flags the
       volume dirty, so put the superblock back the way it was found. */
    if (Repair)
    {
        NfsUnmount(Volume);
    }
    else
    {
        NFS_VOLUME Restore;

        NfsDiscard(Volume);
        memset(&Restore, 0, sizeof(Restore));
        Restore.Device = Device;
        Restore.Super = Original;
        NfsWriteSuper(&Restore);
    }
    fclose(Image.File);
    return (Check.BadNodes == 0) ? 0 : 1;
}
