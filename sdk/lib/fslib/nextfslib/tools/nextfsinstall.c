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
 * With --boot-code and --boot-loader it also fills the boot area, the first
 * 4 KiB of the volume that format 5 keeps out of the file system:
 *
 *   nextfsinstall --image boot.img --size 512 --label NEXTNT \
 *                 --source <tree> --dest \\ReactOS \
 *                 --boot-code nextfs.bin \
 *                 --boot-loader /ReactOS/nexldr.sys --boot-load 0x1000
 *
 * Sector 0 gets the volume boot record, LBA 1 the boot descriptor. The
 * descriptor is not a file system structure and is not read by anything but the
 * boot record: it lists the payload as runs of absolute sectors, straight from
 * the extent list of the installed file, because a 512-byte real-mode program
 * cannot walk a copy-on-write tree and this tool can.
 *
 * What it still does not do: no partition table, no NEXTLDR.INI writing. Those
 * belong to the image layout, and keeping them out means the same tool writes a
 * plain image file, a loopback device, or a partition on a real disk.
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


/* ------------------------------------------------------------------ */
/* Boot area                                                          */
/* ------------------------------------------------------------------ */

#define BOOT_SECTOR_SIZE      512u
#define BOOT_DESC_LBA         1u
#define BOOT_DESC_SECTORS     4u
#define BOOT_DESC_RUN_MAX     ((BOOT_DESC_SECTORS * BOOT_SECTOR_SIZE - 24u) / 12u)

#pragma pack(push, 1)
typedef struct _BOOT_DESC_HEADER
{
    char   Magic[4];        /* "NFB1"                           */
    nfs_u16 Version;        /* 1                                */
    nfs_u16 SectorSize;
    nfs_u32 LoadSegment;
    nfs_u32 EntryOffset;
    nfs_u32 EntrySegment;
    nfs_u16 RunCount;
    nfs_u16 Reserved;
} BOOT_DESC_HEADER, *PBOOT_DESC_HEADER;

typedef struct _BOOT_DESC_RUN
{
    nfs_u64 Lba;
    nfs_u32 Sectors;
    nfs_u32 Reserved;
} BOOT_DESC_RUN, *PBOOT_DESC_RUN;

typedef struct _BOOT_DESC
{
    BOOT_DESC_HEADER Header;
    BOOT_DESC_RUN    Runs[BOOT_DESC_RUN_MAX];
} BOOT_DESC, *PBOOT_DESC;
#pragma pack(pop)

/*
 * The boot record reads the superblock and the descriptor at fixed sector
 * numbers, and it holds them as immediates inside the two instructions that
 * fill in the disk address packet: 8 for the superblock and 1 for the
 * descriptor, both counted from the start of the volume. On a volume that
 * occupies the whole disk those are disk sector numbers; inside a partition
 * they are not, and the difference is the partition's starting sector, which
 * only the installer knows. Adding it here rather than at run time keeps the
 * record free of arithmetic -- 512 bytes do not have room for it -- and makes
 * what is on the disk a plain absolute number.
 *
 * The encoding looked for is what the assembler produces for
 *     mov dword ptr [DapLba], <lba>
 * in 16-bit code: 66 C7 06, the 16-bit DapLba address, then the immediate.
 */
static int
PatchBootSectorBase(unsigned char *Sector, nfs_u64 BaseLba)
{
    unsigned Patched = 0;
    unsigned i;

    if (BaseLba > 0xFFFFFFFFull)
    {
        fprintf(stderr, "nextfsinstall: --hidden-sectors %llu is past 32 bits\n",
                (unsigned long long)BaseLba);
        return NFS_EINVAL;
    }

    for (i = 0; i + 9 <= BOOT_SECTOR_SIZE; i++)
    {
        nfs_u32 Immediate;
        nfs_u32 Value;

        if (Sector[i] != 0x66 || Sector[i + 1] != 0xC7 || Sector[i + 2] != 0x06)
            continue;

        Immediate = (nfs_u32)Sector[i + 5] | ((nfs_u32)Sector[i + 6] << 8) |
                    ((nfs_u32)Sector[i + 7] << 16) | ((nfs_u32)Sector[i + 8] << 24);
        if (Immediate != 8 && Immediate != 1)
            continue;

        Value = Immediate + (nfs_u32)BaseLba;
        Sector[i + 5] = (unsigned char)(Value & 0xFF);
        Sector[i + 6] = (unsigned char)((Value >> 8) & 0xFF);
        Sector[i + 7] = (unsigned char)((Value >> 16) & 0xFF);
        Sector[i + 8] = (unsigned char)((Value >> 24) & 0xFF);
        Patched++;
    }

    if (Patched != 2)
    {
        fprintf(stderr,
                "nextfsinstall: the boot record does not carry the two sector "
                "numbers of boot/nextldr/bootsect/nextfs.S (%u of 2 found)\n",
                Patched);
        return NFS_EINVAL;
    }

    return NFS_OK;
}

/*
 * The boot record is a 512-byte image built from boot/nextldr/bootsect/nextfs.S.
 * Its code is written verbatim -- this tool does not know what is inside it --
 * except for the two sector numbers described above.
 */
static int
WriteBootCode(PNFS_BDEV Device, const char *Path, nfs_u64 BaseLba)
{
    unsigned char Sector[BOOT_SECTOR_SIZE];
    FILE *File;
    size_t Read;
    int Status;

    File = fopen(Path, "rb");
    if (!File)
    {
        fprintf(stderr, "nextfsinstall: cannot open boot code %s: %s\n", Path,
                strerror(errno));
        return NFS_EIO;
    }

    Read = fread(Sector, 1, sizeof(Sector), File);
    if (Read != sizeof(Sector) || fgetc(File) != EOF)
    {
        fprintf(stderr, "nextfsinstall: %s is not exactly %u bytes\n", Path,
                BOOT_SECTOR_SIZE);
        fclose(File);
        return NFS_EINVAL;
    }
    fclose(File);

    if (Sector[510] != 0x55 || Sector[511] != 0xAA)
    {
        fprintf(stderr, "nextfsinstall: %s has no 0xAA55 signature\n", Path);
        return NFS_EINVAL;
    }

    Status = PatchBootSectorBase(Sector, BaseLba);
    if (Status != NFS_OK)
        return Status;

    return Device->Write(Device->Context, 0, Sector, sizeof(Sector));
}

/*
 * The descriptor comes from the extent list of a file that is already on the
 * volume. Holes are refused rather than skipped: a sparse loader would boot
 * garbage, and the only way a loader file becomes sparse is a bug worth
 * reporting at install time.
 */
/*
 * Resolve a volume path one component at a time. NfsOpenPath applies a creation
 * disposition and reports every failure as one code; here the question is only
 * "where is this file", so walking gives an error a human can act on.
 */
static int
LookupVolumePath(PNFS_VOLUME Volume, const char *VolumePath, NFS_PIFT_RECORD *OutRecord)
{
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 NameLength;
    nfs_fileid Parent = NFS_ROOT_ID;
    const char *Cursor = VolumePath;

    while (*Cursor == '/' || *Cursor == '\\')
        Cursor++;

    while (*Cursor)
    {
        const char *End = Cursor;
        char Component[NFS_NAME_MAX + 1];
        size_t Length;

        while (*End && *End != '/' && *End != '\\')
            End++;
        Length = (size_t)(End - Cursor);
        if (Length == 0 || Length > NFS_NAME_MAX)
            return NFS_EINVAL;
        memcpy(Component, Cursor, Length);
        Component[Length] = 0;

        NameLength = NfsNameFromAscii(Component, Name, NFS_NAME_MAX);
        if (NameLength == 0)
            return NFS_EINVAL;

        if (NfsLookup(Volume, Parent, Name, NameLength, OutRecord) != NFS_OK)
            return NFS_ENOENT;

        Parent = OutRecord->FileId;
        Cursor = End;
        while (*Cursor == '/' || *Cursor == '\\')
            Cursor++;
    }

    return NFS_OK;
}

static int
WriteBootDescriptor(PNFS_VOLUME Volume, PNFS_BDEV Device, const char *VolumePath,
                    nfs_u32 LoadSegment, nfs_u32 EntrySegOffset, nfs_u64 BaseLba)
{
    static BOOT_DESC Descriptor;
    NFS_PIFT_RECORD Record;
    const NFS_SUPER *Super;
    nfs_u32 i;
    nfs_u64 SectorsPerCluster;
    int Status;

    Super = NfsGetSuper(Volume);
    if (Super->ClusterSize < NFS_BOOT_AREA_SIZE)
    {
        fprintf(stderr, "nextfsinstall: cluster size %u cannot hold a boot area\n",
                (unsigned)Super->ClusterSize);
        return NFS_EINVAL;
    }
    SectorsPerCluster = Super->ClusterSize / BOOT_SECTOR_SIZE;

    memset(&Record, 0, sizeof(Record));
    Status = LookupVolumePath(Volume, VolumePath, &Record);
    if (Status != NFS_OK)
    {
        fprintf(stderr, "nextfsinstall: --boot-loader %s is not on the volume (%d)\n",
                VolumePath, Status);
        return Status;
    }
    if (Record.Flags & NFS_FLAG_DIR)
    {
        fprintf(stderr, "nextfsinstall: --boot-loader %s is a directory\n", VolumePath);
        return NFS_EINVAL;
    }
    if (Record.ExtentCount == 0)
    {
        fprintf(stderr, "nextfsinstall: --boot-loader %s is empty\n", VolumePath);
        return NFS_EINVAL;
    }
    if (Record.ExtentCount > BOOT_DESC_RUN_MAX)
    {
        fprintf(stderr, "nextfsinstall: --boot-loader has %u extents, the descriptor holds %u\n",
                (unsigned)Record.ExtentCount, (unsigned)BOOT_DESC_RUN_MAX);
        return NFS_ENOSPC;
    }

    memset(&Descriptor, 0, sizeof(Descriptor));
    memcpy(Descriptor.Header.Magic, "NFB1", 4);
    Descriptor.Header.Version = 1;
    Descriptor.Header.SectorSize = BOOT_SECTOR_SIZE;
    Descriptor.Header.LoadSegment = LoadSegment;
    Descriptor.Header.EntrySegment = EntrySegOffset >> 16;
    Descriptor.Header.EntryOffset = EntrySegOffset & 0xFFFFu;

    for (i = 0; i < Record.ExtentCount; i++)
    {
        const NFS_EXTENT *Extent = &Record.Extents[i];

        if (Extent->Length == 0)
            break;
        if (Extent->Lcn == NFS_HOLE)
        {
            fprintf(stderr, "nextfsinstall: --boot-loader is sparse at vcn %llu\n",
                    (unsigned long long)Extent->Vcn);
            return NFS_EINVAL;
        }
        /* Absolute sector numbers, like the boot record's: the record loads
           these runs with INT 13h and never adds anything to them. */
        Descriptor.Runs[i].Lba = BaseLba + Extent->Lcn * SectorsPerCluster;
        Descriptor.Runs[i].Sectors = (nfs_u32)(Extent->Length * SectorsPerCluster);
    }
    Descriptor.Header.RunCount = (nfs_u16)i;

    Status = Device->Write(Device->Context, BOOT_DESC_LBA * BOOT_SECTOR_SIZE,
                           &Descriptor, BOOT_DESC_SECTORS * BOOT_SECTOR_SIZE);
    if (Status != NFS_OK)
        return Status;

    printf("boot descriptor: %u runs, load segment 0x%04x, entry %04x:%04x\n",
           (unsigned)Descriptor.Header.RunCount, (unsigned)LoadSegment,
           (unsigned)Descriptor.Header.EntrySegment,
           (unsigned)Descriptor.Header.EntryOffset);
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
    const char *BootCodePath = NULL;
    const char *BootLoaderPath = NULL;
    nfs_u32 BootLoadSegment = 0x1000;
    nfs_u32 BootEntry = 0;
    nfs_u64 HiddenSectors = 0;
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
        else if (strcmp(argv[i], "--boot-code") == 0 && i + 1 < argc)
            BootCodePath = argv[++i];
        else if (strcmp(argv[i], "--boot-loader") == 0 && i + 1 < argc)
            BootLoaderPath = argv[++i];
        else if (strcmp(argv[i], "--boot-load") == 0 && i + 1 < argc)
            BootLoadSegment = (nfs_u32)strtoul(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "--boot-entry") == 0 && i + 1 < argc)
            BootEntry = (nfs_u32)strtoul(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "--hidden-sectors") == 0 && i + 1 < argc)
            HiddenSectors = strtoull(argv[++i], NULL, 10);
        else
        {
            fprintf(stderr,
                    "usage: nextfsinstall --image <file> --size <MiB> [--cluster N]\n"
                    "                     [--label NAME] --source <dir> [--dest <path>]\n"
                    "                     [--no-format]\n"
                    "                     [--boot-code <512-byte VBR>]\n"
                    "                     [--boot-loader <volume path>]\n"
                    "                     [--boot-load <hex segment>] [--boot-entry <hex seg:off>]\n"
                    "                     [--hidden-sectors <n>]\n");
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

    if (BootLoaderPath)
    {
        nfs_u32 Entry = BootEntry ? BootEntry : BootLoadSegment << 16;

        Status = WriteBootDescriptor(Volume, &Device, BootLoaderPath, BootLoadSegment, Entry,
                                     HiddenSectors);
        if (Status != NFS_OK)
        {
            NfsUnmount(Volume);
            fclose(ImageDevice.File);
            return 1;
        }
    }

    if (BootCodePath)
    {
        Status = WriteBootCode(&Device, BootCodePath, HiddenSectors);
        if (Status != NFS_OK)
        {
            NfsUnmount(Volume);
            fclose(ImageDevice.File);
            return 1;
        }
        if (HiddenSectors)
            printf("boot record: %s in sector 0, volume starts at sector %llu\n",
                   BootCodePath, (unsigned long long)HiddenSectors);
        else
            printf("boot record: %s in sector 0\n", BootCodePath);
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
