/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tools/nextformat.c
 * PURPOSE:     nextformat — create a NextFS volume (mkfs)
 * LICENSE:     GPL-3.0-or-later
 *
 * usage: nextformat <image-or-device> [--label NAME] [--cluster SIZE]
 *
 * In phase 2 this becomes the backend of `format` (fmifs) and of usetup.
 */

#include "../nextfs_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _IMAGE_DEVICE
{
    FILE *File;
} IMAGE_DEVICE, *PIMAGE_DEVICE;

static int
ImageRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;

    if (fseeko(Device->File, (off_t)Offset, SEEK_SET) != 0)
        return NFS_EIO;
    if (fread(Buffer, 1, Length, Device->File) != Length)
        return NFS_EIO;
    return NFS_OK;
}

static int
ImageWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;

    if (fseeko(Device->File, (off_t)Offset, SEEK_SET) != 0)
        return NFS_EIO;
    if (fwrite(Buffer, 1, Length, Device->File) != Length)
        return NFS_EIO;
    return NFS_OK;
}

static int
ImageSize(void *Context, nfs_u64 *Bytes)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;
    off_t End;

    if (fseeko(Device->File, 0, SEEK_END) != 0)
        return NFS_EIO;
    End = ftello(Device->File);
    if (End < 0)
        return NFS_EIO;
    *Bytes = (nfs_u64)End;
    return NFS_OK;
}

static int
ImageFlush(void *Context)
{
    PIMAGE_DEVICE Device = (PIMAGE_DEVICE)Context;

    return (fflush(Device->File) == 0) ? NFS_OK : NFS_EIO;
}

int
main(int argc, char **argv)
{
    IMAGE_DEVICE Image;
    NFS_BDEV Device;
    const char *Path = NULL;
    const char *Label = "NextFS";
    nfs_u32 ClusterSize = NFS_CLUSTER_MIN;
    int i, Status;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            Label = argv[++i];
        else if (strcmp(argv[i], "--cluster") == 0 && i + 1 < argc)
            ClusterSize = (nfs_u32)strtoul(argv[++i], NULL, 0);
        else
            Path = argv[i];
    }

    if (!Path)
    {
        fprintf(stderr, "usage: %s <image-or-device> [--label NAME] [--cluster SIZE]\n", argv[0]);
        return 1;
    }

    Image.File = fopen(Path, "r+b");
    if (!Image.File)
    {
        perror(Path);
        return 1;
    }

    memset(&Device, 0, sizeof(Device));
    Device.Context = &Image;
    Device.Read = ImageRead;
    Device.Write = ImageWrite;
    Device.Size = ImageSize;
    Device.Flush = ImageFlush;

    {
        nfs_u64 Bytes = 0;

        if (ImageSize(&Image, &Bytes) == NFS_OK)
            printf("nextformat: %s (%llu bytes, cluster %u, label \"%s\")\n",
                   Path, (unsigned long long)Bytes, ClusterSize, Label);
    }

    Status = NfsFormat(&Device, Label, ClusterSize);
    fclose(Image.File);

    if (Status != NFS_OK)
    {
        fprintf(stderr, "NfsFormat failed: %d\n", Status);
        return 1;
    }

    printf("NextFS volume created.\n");
    return 0;
}
