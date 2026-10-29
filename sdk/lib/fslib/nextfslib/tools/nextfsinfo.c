/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tools/nextfsinfo.c
 * PURPOSE:     nextfsinfo — mount a NextFS image and list it (read-only demo)
 * LICENSE:     GPL-3.0-or-later
 *
 * usage: nextfsinfo <image> [--depth N] [--put <host file> <name>]
 *                     [--snapshot <name>] [--cat-snapshot <snapshot> <name>]
 */

#include "../nextfs_format.h"

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

static void PrintEntry(const NFS_PIFT_RECORD *Record, const char *Prefix)
{
    char Name[512];

    if (NfsNameToAscii(Record->Name, Record->NameLen, Name, sizeof(Name)) != NFS_OK)
        strcpy(Name, "?");

    printf("%s%-32s fileid=%-6llu size=%-9llu extents=%u%s\n", Prefix, Name,
           (unsigned long long)Record->FileId, (unsigned long long)Record->Size,
           Record->ExtentCount,
           (Record->Flags & NFS_FLAG_DIR) ? " [dir]" : "");
}

static void ListDir(PNFS_VOLUME Volume, nfs_fileid Directory, const char *Prefix, int Depth)
{
    NFS_PIFT_RECORD Record;
    nfs_u32 Index = 0;
    char ChildPrefix[512];

    while (NfsReadDir(Volume, Directory, Index, &Record) == NFS_OK)
    {
        PrintEntry(&Record, Prefix);
        if ((Record.Flags & NFS_FLAG_DIR) && Depth > 1)
        {
            snprintf(ChildPrefix, sizeof(ChildPrefix), "%s    ", Prefix);
            ListDir(Volume, Record.FileId, ChildPrefix, Depth - 1);
        }
        Index++;
    }
}

int main(int argc, char **argv)
{
    IMAGE_DEVICE Image;
    NFS_BDEV Device;
    PNFS_VOLUME Volume = NULL;
    const NFS_SUPER *Super;
    NFS_STATS Stats;
    const char *PutSource = NULL, *PutName = NULL;
    const char *SnapshotName = NULL;
    const char *CatSnapshot = NULL, *CatName = NULL;
    int Depth = 1, i, Status;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <image> [--depth N]\n", argv[0]);
        return 1;
    }
    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
            Depth = atoi(argv[++i]);
        else if (strcmp(argv[i], "--put") == 0 && i + 2 < argc)
        {
            PutSource = argv[++i];
            PutName = argv[++i];
        }
        else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc)
            SnapshotName = argv[++i];
        else if (strcmp(argv[i], "--cat-snapshot") == 0 && i + 2 < argc)
        {
            CatSnapshot = argv[++i];
            CatName = argv[++i];
        }
    }

    Image.File = fopen(argv[1], "r+b");
    if (!Image.File)
    {
        perror(argv[1]);
        return 1;
    }

    memset(&Device, 0, sizeof(Device));
    Device.Context = &Image;
    Device.Read = ImageRead;
    Device.Write = ImageWrite;
    Device.Size = ImageSize;

    Status = NfsMount(&Device, &Volume);
    if (Status != NFS_OK)
    {
        fprintf(stderr, "NfsMount failed: %d\n", Status);
        fclose(Image.File);
        return 1;
    }

    Super = NfsGetSuper(Volume);
    printf("NextFS volume \"%s\"\n", Super->Label);
    printf("  cluster %u, %llu clusters, %llu free, generation %llu, flags 0x%x\n",
           Super->ClusterSize, (unsigned long long)Super->TotalClusters,
           (unsigned long long)Super->FreeClusters,
           (unsigned long long)Super->Generation, Super->Flags);
    printf("  journal %u clusters (seq %llu..%llu)\n", Super->JournalClusters,
           (unsigned long long)Super->JournalSeqFirst,
           (unsigned long long)Super->JournalSeqNext);
    printf("  PIFT area %u clusters in two halves; live half starts at cluster %llu "
           "(%u clusters), head %llu\n",
           Super->PiftAreaClusters, (unsigned long long)Super->PiftFirst,
           Super->PiftClusters, (unsigned long long)Super->PiftHead);
    printf("  checkpoint root %llu (generation %llu), fallback root %llu (generation %llu)\n",
           (unsigned long long)Super->CheckpointRoot.Block,
           (unsigned long long)Super->CheckpointRoot.Generation,
           (unsigned long long)Super->CheckpointRootPrev.Block,
           (unsigned long long)Super->CheckpointRootPrev.Generation);
    NfsGetStats(Volume, &Stats);
    printf("  replayed frames: %llu, commits: %llu, checkpoints: %llu\n",
           (unsigned long long)Stats.ReplayedFrames,
           (unsigned long long)Stats.JournalCommits,
           (unsigned long long)Stats.Checkpoints);

    /* Optional writes, so the tools can drive a quick end-to-end test. */
    if (PutSource && PutName)
    {
        FILE *Source = fopen(PutSource, "rb");
        nfs_u16 FileName[NFS_NAME_MAX];
        nfs_u32 NameLength = NfsNameFromAscii(PutName, FileName, NFS_NAME_MAX);
        nfs_fileid FileId = 0;

        if (!Source)
        {
            perror(PutSource);
        }
        else
        {
            static nfs_u8 Contents[1 << 20];
            size_t Length = fread(Contents, 1, sizeof(Contents), Source);
            fclose(Source);

            Status = NfsCreate(Volume, NFS_ROOT_ID, FileName, NameLength, 0, &FileId);
            if (Status == NFS_OK)
                Status = NfsWrite(Volume, FileId, 0, Contents, (nfs_u32)Length, NULL);
            printf("put %s (%zu bytes): %s\n", PutName, Length,
                   Status == NFS_OK ? "ok" : "failed");
        }
    }

    if (SnapshotName)
    {
        nfs_u16 Name[NFS_NAME_MAX];
        nfs_u32 NameLength = NfsNameFromAscii(SnapshotName, Name, NFS_NAME_MAX);
        nfs_fileid Id = 0;

        Status = NfsSnapshotCreate(Volume, Name, NameLength, &Id);
        printf("snapshot \"%s\": %s\n", SnapshotName,
               Status == NFS_OK ? "ok" : "failed");
    }

    printf("snapshots:\n");
    {
        NFS_SNAPSHOT_INFO Info;
        nfs_u32 Index;
        int Any = 0;

        for (Index = 0; NfsSnapshotList(Volume, Index, &Info) == NFS_OK; Index++)
        {
            char Name[NFS_NAME_MAX + 1];

            if (NfsNameToAscii(Info.Name, Info.NameLen, Name, sizeof(Name)) != NFS_OK)
                strcpy(Name, "?");
            printf("  %-24s id=%llu tree root %llu, generation %llu\n", Name,
                   (unsigned long long)Info.Id, (unsigned long long)Info.TreeRoot,
                   (unsigned long long)Info.Generation);
            Any = 1;
        }
        if (!Any)
            printf("  none\n");
    }

    if (CatSnapshot && CatName)
    {
        nfs_u16 SnapshotNode[NFS_NAME_MAX];
        nfs_u16 FileNode[NFS_NAME_MAX];
        nfs_u32 SnapshotLength = NfsNameFromAscii(CatSnapshot, SnapshotNode, NFS_NAME_MAX);
        nfs_u32 FileLength = NfsNameFromAscii(CatName, FileNode, NFS_NAME_MAX);
        NFS_PIFT_RECORD SnapshotRecord, FileRecord;

        /* Snapshots are records inside the .snapshots directory of the root. */
        {
            nfs_u16 DirectoryNode[NFS_NAME_MAX];
            nfs_u32 DirectoryLength = NfsNameFromAscii(".snapshots", DirectoryNode,
                                                       NFS_NAME_MAX);
            NFS_PIFT_RECORD DirectoryRecord;

            Status = NfsLookup(Volume, NFS_ROOT_ID, DirectoryNode, DirectoryLength,
                               &DirectoryRecord);
            if (Status == NFS_OK)
                Status = NfsLookup(Volume, DirectoryRecord.FileId, SnapshotNode,
                                   SnapshotLength, &SnapshotRecord);
        }
        if (Status == NFS_OK)
            Status = NfsSnapshotLookup(Volume, SnapshotRecord.FileId, NFS_ROOT_ID,
                                       FileNode, FileLength, &FileRecord);
        if (Status != NFS_OK)
        {
            printf("cat %s from %s: not found (lookup=%d, snapshot id=%llu)\n", CatName,
                   CatSnapshot, Status, (unsigned long long)SnapshotRecord.FileId);
        }
        else
        {
            char Contents[4096];
            nfs_u32 Got = 0;

            Status = NfsSnapshotRead(Volume, SnapshotRecord.FileId, FileRecord.FileId,
                                     0, Contents, sizeof(Contents), &Got);
            if (Status == NFS_OK)
                printf("cat %s from %s (%u bytes): %.*s\n", CatName, CatSnapshot, Got,
                       (int)Got, Contents);
            else
                printf("cat %s from %s: read failed\n", CatName, CatSnapshot);
        }
    }

    printf("root:\n");
    ListDir(Volume, NFS_ROOT_ID, "  ", Depth);

    NfsUnmount(Volume);
    fclose(Image.File);
    return 0;
}
