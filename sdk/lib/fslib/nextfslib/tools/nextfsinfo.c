/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tools/nextfsinfo.c
 * PURPOSE:     nextfsinfo — mount a NextFS image and list it (read-only demo)
 * LICENSE:     GPL-3.0-or-later
 *
 * usage: nextfsinfo <image> [--all]
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
    printf("  journal %u clusters (seq %llu..%llu), PIFT area %u clusters, head %llu\n",
           Super->JournalClusters, (unsigned long long)Super->JournalSeqFirst,
           (unsigned long long)Super->JournalSeqNext, Super->PiftClusters,
           (unsigned long long)Super->PiftHead);
    NfsGetStats(Volume, &Stats);
    printf("  replayed frames: %llu, commits: %llu, checkpoints: %llu\n",
           (unsigned long long)Stats.ReplayedFrames,
           (unsigned long long)Stats.JournalCommits,
           (unsigned long long)Stats.Checkpoints);

    printf("root:\n");
    ListDir(Volume, NFS_ROOT_ID, "  ", Depth);

    NfsUnmount(Volume);
    fclose(Image.File);
    return 0;
}
