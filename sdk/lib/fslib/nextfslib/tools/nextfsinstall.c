/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tools/nextfsinstall.c
 * PURPOSE:     nextfsinstall — format a NextFS volume and populate it from a tree
 * LICENSE:     GPL-3.0-or-later
 *
 * usage: nextfsinstall --image <file> --size <MiB> [--cluster N] [--label NAME]
 *                      --source <dir> [--dest <path>] [--no-format]
 *
 * This is the half of the boot switch that runs on a build machine: take a tree
 * (the ReactOS install tree, say), lay it out on a NextFS volume, and hand the
 * volume to nextldr. usetup will call the same library calls later -- the reason
 * this tool exists first is that a filesystem with no installable image cannot be
 * tested at all, and the boot path is exactly what has to be tested.
 *
 * What it deliberately does not do: no boot sector, no partition table, no
 * NEXTLDR.INI writing. Those belong to the image layout, not to the file system,
 * and keeping them out means this tool can be pointed at a plain image file, a
 * loopback device, or a partition on a real disk without knowing the difference.
 */

#include "../nextfs_format.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_CHUNK (256u * 1024u)

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

/* ------------------------------------------------------------------ */
/* Installing one directory                                            */
/* ------------------------------------------------------------------ */

typedef struct _INSTALL_STATE
{
    PNFS_VOLUME Volume;
    nfs_u32 Files;
    nfs_u32 Directories;
    nfs_u64 Bytes;
    nfs_u32 Failures;
} INSTALL_STATE, *PINSTALL_STATE;

/* Create a name in Parent, either a directory or a file, and return its id. */
static int
CreateEntry(PNFS_VOLUME Volume, nfs_fileid Parent, const char *Name, int Directory,
            nfs_fileid *OutId)
{
    nfs_u16 Units[NFS_NAME_MAX];
    nfs_u32 Length = NfsNameFromAscii(Name, Units, NFS_NAME_MAX);

    if (Length == 0)
        return NFS_EINVAL;
    return NfsCreate(Volume, Parent, Units, Length, Directory, OutId);
}

static int
InstallFile(PINSTALL_STATE State, nfs_fileid Parent, const char *Name, const char *Path)
{
    nfs_fileid FileId = 0;
    FILE *Input;
    u_int8_t *Buffer;
    nfs_u64 Offset = 0;
    int Status;

    Status = CreateEntry(State->Volume, Parent, Name, 0, &FileId);
    if (Status != NFS_OK)
    {
        fprintf(stderr, "nextfsinstall: cannot create %s (%d)\n", Path, Status);
        State->Failures++;
        return Status;
    }

    Input = fopen(Path, "rb");
    if (!Input)
    {
        fprintf(stderr, "nextfsinstall: cannot open %s: %s\n", Path, strerror(errno));
        State->Failures++;
        return NFS_EIO;
    }

    Buffer = (u_int8_t *)malloc(COPY_CHUNK);
    if (!Buffer)
    {
        fclose(Input);
        State->Failures++;
        return NFS_ENOSPC;
    }

    for (;;)
    {
        size_t Got = fread(Buffer, 1, COPY_CHUNK, Input);
        nfs_u32 Written = 0;

        if (Got == 0)
            break;
        Status = NfsWrite(State->Volume, FileId, Offset, Buffer, (nfs_u32)Got, &Written);
        if (Status != NFS_OK || Written != Got)
        {
            fprintf(stderr, "nextfsinstall: write failed on %s (%d)\n", Path, Status);
            State->Failures++;
            break;
        }
        Offset += Written;
        State->Bytes += Written;
    }

    free(Buffer);
    fclose(Input);
    State->Files++;
    return NFS_OK;
}

static int
InstallTree(PINSTALL_STATE State, nfs_fileid Parent, const char *Source)
{
    DIR *Directory;
    struct dirent *Entry;

    Directory = opendir(Source);
    if (!Directory)
    {
        fprintf(stderr, "nextfsinstall: cannot read %s: %s\n", Source, strerror(errno));
        State->Failures++;
        return NFS_EIO;
    }

    while ((Entry = readdir(Directory)) != NULL)
    {
        char Child[4096];
        struct stat Info;
        nfs_fileid Id = 0;
        int Status;

        if (strcmp(Entry->d_name, ".") == 0 || strcmp(Entry->d_name, "..") == 0)
            continue;

        snprintf(Child, sizeof(Child), "%s/%s", Source, Entry->d_name);
        if (stat(Child, &Info) != 0)
        {
            fprintf(stderr, "nextfsinstall: cannot stat %s: %s\n", Child, strerror(errno));
            State->Failures++;
            continue;
        }

        if (S_ISDIR(Info.st_mode))
        {
            Status = CreateEntry(State->Volume, Parent, Entry->d_name, 1, &Id);
            if (Status != NFS_OK)
            {
                fprintf(stderr, "nextfsinstall: cannot mkdir %s (%d)\n", Child, Status);
                State->Failures++;
                continue;
            }
            State->Directories++;
            InstallTree(State, Id, Child);
        }
        else if (S_ISREG(Info.st_mode))
        {
            InstallFile(State, Parent, Entry->d_name, Child);
        }
        /* Sockets, fifos and symlinks are not files a boot volume needs; a link
           is skipped on purpose rather than silently copied as its target. */
    }

    closedir(Directory);
    return NFS_OK;
}

/* Walk a path like "\ReactOS\System32" inside the volume, creating the
   directories it names so the tree can be placed below a subdirectory. */
static int
EnsureDestination(PNFS_VOLUME Volume, const char *Path, nfs_fileid *OutId)
{
    nfs_fileid Current = NFS_ROOT_ID;
    const char *p = Path;

    while (*p)
    {
        char Component[NFS_NAME_MAX * 4];
        size_t Length = 0;
        nfs_fileid Id = 0;
        int Status;

        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        while (*p && *p != '\\' && *p != '/')
        {
            if (Length + 1 < sizeof(Component))
                Component[Length++] = *p;
            p++;
        }
        Component[Length] = 0;
        if (Length == 0)
            continue;

        Status = CreateEntry(Volume, Current, Component, 1, &Id);
        if (Status == NFS_EEXIST)
        {
            nfs_u16 Units[NFS_NAME_MAX];
            nfs_u32 UnitsLength = NfsNameFromAscii(Component, Units, NFS_NAME_MAX);
            NFS_PIFT_RECORD Record;

            Status = NfsLookup(Volume, Current, Units, UnitsLength, &Record);
            if (Status == NFS_OK)
                Id = Record.FileId;
        }
        if (Status != NFS_OK)
            return Status;
        Current = Id;
    }

    *OutId = Current;
    return NFS_OK;
}

int
main(int argc, char **argv)
{
    const char *ImagePath = NULL;
    const char *SourcePath = NULL;
    const char *Destination = "";
    const char *Label = "NEXTFSBOOT";
    unsigned long long SizeMiB = 0;
    nfs_u32 ClusterSize = 4096;
    int Format = 1;
    IMAGE_DEVICE ImageDevice;
    NFS_BDEV Device;
    PNFS_VOLUME Volume = NULL;
    INSTALL_STATE State;
    int i, Status;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc)
            ImagePath = argv[++i];
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            SourcePath = argv[++i];
        else if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc)
            Destination = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            Label = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            SizeMiB = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--cluster") == 0 && i + 1 < argc)
            ClusterSize = (nfs_u32)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--no-format") == 0)
            Format = 0;
        else
        {
            fprintf(stderr,
                    "usage: nextfsinstall --image <file> --size <MiB> [--cluster N]\n"
                    "                     [--label NAME] --source <dir> [--dest <path>]\n"
                    "                     [--no-format]\n");
            return 2;
        }
    }

    if (!ImagePath || !SourcePath)
    {
        fprintf(stderr, "nextfsinstall: --image and --source are required\n");
        return 2;
    }

    ImageDevice.File = fopen(ImagePath, Format ? "wb+" : "rb+");
    if (!ImageDevice.File)
    {
        fprintf(stderr, "nextfsinstall: cannot open %s: %s\n", ImagePath, strerror(errno));
        return 1;
    }

    if (Format)
    {
        if (SizeMiB == 0)
        {
            fprintf(stderr, "nextfsinstall: --size is required when formatting\n");
            fclose(ImageDevice.File);
            return 2;
        }
        if (ftruncate(fileno(ImageDevice.File), (off_t)(SizeMiB * 1024ull * 1024ull)) != 0)
        {
            fprintf(stderr, "nextfsinstall: cannot size %s: %s\n", ImagePath, strerror(errno));
            fclose(ImageDevice.File);
            return 1;
        }
    }

    Device.Context = &ImageDevice;
    Device.Read = ImageRead;
    Device.Write = ImageWrite;
    Device.Size = ImageSize;
    Device.Flush = ImageFlush;

    if (Format)
    {
        Status = NfsFormat(&Device, Label, ClusterSize);
        if (Status != NFS_OK)
        {
            fprintf(stderr, "nextfsinstall: format failed (%d)\n", Status);
            fclose(ImageDevice.File);
            return 1;
        }
        printf("formatted %s: %llu MiB, cluster %u, label \"%s\"\n", ImagePath, SizeMiB,
               (unsigned)ClusterSize, Label);
    }

    Status = NfsMount(&Device, &Volume);
    if (Status != NFS_OK)
    {
        fprintf(stderr, "nextfsinstall: mount failed (%d)\n", Status);
        fclose(ImageDevice.File);
        return 1;
    }

    memset(&State, 0, sizeof(State));
    State.Volume = Volume;

    {
        nfs_fileid Root = NFS_ROOT_ID;

        if (Destination[0])
        {
            Status = EnsureDestination(Volume, Destination, &Root);
            if (Status != NFS_OK)
            {
                fprintf(stderr, "nextfsinstall: cannot create %s (%d)\n", Destination, Status);
                NfsUnmount(Volume);
                fclose(ImageDevice.File);
                return 1;
            }
        }

        InstallTree(&State, Root, SourcePath);
    }

    NfsUnmount(Volume);              /* checkpoints: the boot reader needs the tree */
    ImageFlush(&ImageDevice);
    fclose(ImageDevice.File);

    printf("installed %u files in %u directories, %llu bytes%s%s\n", State.Files,
           State.Directories, (unsigned long long)State.Bytes,
           Destination[0] ? " under " : "", Destination[0] ? Destination : "");

    if (State.Failures)
    {
        printf("%u entries failed\n", State.Failures);
        return 1;
    }
    return 0;
}
