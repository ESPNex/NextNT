/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tests/nextfs_boot_selftest.c
 * PURPOSE:     Check the boot loader's NextFS reader against a real image
 * LICENSE:     GPL-3.0-or-later
 *
 * Builds a NextFS image with nextfslib (the code nextformat uses), closes it the
 * way a clean shutdown does -- NfsUnmount -> NfsSync -> checkpoint -- writes it
 * to a file, then mounts *that file* with the reader from
 * boot/nextldr/nextldr/lib/fs/nextfs_core.c and compares every lookup and every
 * read against what the library wrote. It also breaks a copy of the image twice:
 * once the primary superblock, once the newest checkpoint root, to prove the two
 * fallbacks work.
 *
 * Not part of the CMake build (it compiles a file that belongs to the loader), so
 * build it by hand from sdk/lib/fslib/nextfslib:
 *
 *   gcc -std=gnu11 -O2 -DNEXTFS_BOOT_NO_ARC \
 *       -I../../../../boot/nextldr/nextldr/include -I. \
 *       -o /tmp/nextfs_boot_selftest tests/nextfs_boot_selftest.c \
 *       ../../../../boot/nextldr/nextldr/lib/fs/nextfs_core.c \
 *       nextfs_crc32c.c nextfs_super.c nextfs_pift.c nextfs_journal.c \
 *       nextfs_tree.c nextfs_snapshot.c nextfs_security.c nextfs_namespace.c \
 *       nextfs_handle.c nextfs_volume.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "nextfs_format.h"
#include "fs/nextfs.h"

#define IMAGE_BYTES (16u * 1024u * 1024u)

static unsigned char *g_Image;
static int g_Checks;
static int g_Failures;

#define CHECK(Cond, What)                                                   \
    do {                                                                    \
        g_Checks++;                                                         \
        if (!(Cond)) {                                                      \
            g_Failures++;                                                   \
            printf("  FAIL  %s  (line %d)\n", What, __LINE__);              \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* nextfslib block device over the in-memory image                     */
/* ------------------------------------------------------------------ */

static int
LibRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    (void)Context;
    if (Offset + Length > IMAGE_BYTES)
        return NFS_EIO;
    memcpy(Buffer, g_Image + Offset, Length);
    return NFS_OK;
}

static int
LibWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    (void)Context;
    if (Offset + Length > IMAGE_BYTES)
        return NFS_EIO;
    memcpy(g_Image + Offset, Buffer, Length);
    return NFS_OK;
}

static int
LibSize(void *Context, nfs_u64 *Bytes)
{
    (void)Context;
    *Bytes = IMAGE_BYTES;
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* The boot reader's callbacks over a file                             */
/* ------------------------------------------------------------------ */

static int g_Fd = -1;

static int
BootRead(void *Context, NFSB_U64 Offset, void *Buffer, NFSB_U32 Length)
{
    ssize_t Got;
    (void)Context;

    Got = pread(g_Fd, Buffer, Length, (off_t)Offset);
    return (Got == (ssize_t)Length) ? NFSB_OK : NFSB_EIO;
}

static void *
BootAlloc(NFSB_U32 Size)
{
    return malloc(Size);
}

static void
BootFree(void *Block)
{
    free(Block);
}

/* ------------------------------------------------------------------ */
/* Building the image                                                  */
/* ------------------------------------------------------------------ */

static const char *g_Wanted[] =
{
    "\\ReactOS\\System32\\ntoskrnl.exe",
    "\\ReactOS\\System32\\config\\SYSTEM",
    "\\ReactOS\\txtsetup.sif",
    "\\ReactOS\\empty.bin",
    "\\ReactOS\\sparse.bin",
    "\\ReactOS\\citt\xC3\xA0.txt",
};

int
main(void)
{
    NFS_BDEV Device;
    PNFS_VOLUME Volume = NULL;
    NFSB_VOLUME *Boot = NULL;
    NFSB_IO Io;
    NFSB_HEAP Heap;
    unsigned char Chunk[4096];
    unsigned char *Expected[6];
    nfs_u32 ExpectedSize[6];
    int Status, i, Input;

    memset(&Device, 0, sizeof(Device));
    Device.Read = LibRead;
    Device.Write = LibWrite;
    Device.Size = LibSize;

    printf("[1] format 16 MiB, cluster 4096\n");
    g_Image = (unsigned char *)calloc(1, IMAGE_BYTES);
    CHECK(NfsFormat(&Device, "NEXTFSBOOT", 4096) == NFS_OK, "format");
    CHECK(NfsMount(&Device, &Volume) == NFS_OK, "mount for writing");

    printf("[2] create the tree and write the files\n");
    {
        nfs_u16 Name[NFS_NAME_MAX];
        nfs_u32 Length;
        nfs_fileid Dir, Sub, File;

        Length = NfsNameFromAscii("ReactOS", Name, NFS_NAME_MAX);
        CHECK(NfsCreate(Volume, NFS_ROOT_ID, Name, Length, 1, &Dir) == NFS_OK, "mkdir ReactOS");
        Length = NfsNameFromAscii("System32", Name, NFS_NAME_MAX);
        CHECK(NfsCreate(Volume, Dir, Name, Length, 1, &Sub) == NFS_OK, "mkdir System32");
        Length = NfsNameFromAscii("config", Name, NFS_NAME_MAX);
        {
            nfs_fileid Config;

            CHECK(NfsCreate(Volume, Sub, Name, Length, 1, &Config) == NFS_OK, "mkdir config");
            Length = NfsNameFromAscii("SYSTEM", Name, NFS_NAME_MAX);
            CHECK(NfsCreate(Volume, Config, Name, Length, 0, &File) == NFS_OK, "create SYSTEM");
            for (i = 0; i < 3; i++)
            {
                memset(Chunk, (unsigned char)(0x40 + i), sizeof(Chunk));
                CHECK(NfsWrite(Volume, File, (nfs_u64)i * sizeof(Chunk), Chunk,
                               sizeof(Chunk), NULL) == NFS_OK, "write SYSTEM chunk");
            }
        }
    }

    /* The files the check will look up, with contents large enough to span
       clusters, plus one sparse file and one UTF-8 name. */
    for (i = 0; i < 6; i++)
    {
        const char *Path = g_Wanted[i];
        nfs_u32 Handle = 0;
        nfs_u32 Size = 0;
        const char *Payload = NULL;
        char Buffer[64];

        Expected[i] = NULL;
        ExpectedSize[i] = 0;

        if (strstr(Path, "ntoskrnl"))
        {
            Size = 9000;
            Expected[i] = (unsigned char *)malloc(Size);
            for (Input = 0; Input < (int)Size; Input++)
                Expected[i][Input] = (unsigned char)(Input * 7 + 3);
        }
        else if (strstr(Path, "txtsetup"))
        {
            Payload = "[Version]\r\nSignature=\"$ReactOS$\"\r\n";
            Size = (nfs_u32)strlen(Payload);
            Expected[i] = (unsigned char *)malloc(Size);
            memcpy(Expected[i], Payload, Size);
        }
        else if (strstr(Path, "empty"))
        {
            Size = 0;
        }
        else if (strstr(Path, "sparse"))
        {
            Size = 12 * 1024;
            Expected[i] = (unsigned char *)calloc(1, Size);
            /* one cluster in the middle, everything else stays a hole */
            memcpy(Expected[i], "start", 5);
            memcpy(Expected[i] + 4096, "middle", 6);
        }
        else if (strstr(Path, "config\\SYSTEM"))
        {
            Size = 3 * 4096;
            Expected[i] = (unsigned char *)malloc(Size);
            for (Input = 0; Input < (int)Size; Input++)
                Expected[i][Input] = (unsigned char)(0x40 + (Input / 4096));
        }
        else if (strstr(Path, "citt"))
        {
            Payload = "sotto il cielo di Milano\n";
            Size = (nfs_u32)strlen(Payload);
            Expected[i] = (unsigned char *)malloc(Size);
            memcpy(Expected[i], Payload, Size);
        }
        else
        {
            continue;
        }
        ExpectedSize[i] = Size;

        snprintf(Buffer, sizeof(Buffer), "%u", (unsigned)Size);
        (void)0;

        Status = NfsHandleOpen(Volume, Path, NFS_DISPOSITION_OVERWRITE_IF, 0,
                               NFS_HANDLE_WRITE | NFS_HANDLE_READ, &Handle);
        CHECK(Status == NFS_OK, "open for writing");

        if (strstr(Path, "sparse"))
        {
            nfs_u32 Written = 0;

            CHECK(NfsHandleSetEndOfFile(Volume, Handle, Size) == NFS_OK, "set EOF");
            CHECK(NfsHandleSeek(Volume, Handle, 0) == NFS_OK, "seek to the head");
            CHECK(NfsHandleWrite(Volume, Handle, "start", 5, &Written) == NFS_OK, "write head");
            CHECK(NfsHandleSeek(Volume, Handle, 4096) == NFS_OK, "seek into the hole");
            CHECK(NfsHandleWrite(Volume, Handle, "middle", 6, &Written) == NFS_OK, "write middle");
        }
        else if (Size)
        {
            nfs_u32 Written = 0;

            CHECK(NfsHandleWrite(Volume, Handle, Expected[i], Size, &Written) == NFS_OK &&
                  Written == Size, "write contents");
            (void)Written;
        }

        CHECK(NfsHandleClose(Volume, Handle) == NFS_OK, "close");
    }

    printf("[3] checkpoint, then one more file, then a clean shutdown\n");
    CHECK(NfsSync(Volume) == NFS_OK, "checkpoint generation A");
    {
        nfs_u32 Handle = 0, Written = 0;

        CHECK(NfsHandleOpen(Volume, "\\ReactOS\\after-checkpoint.txt",
                            NFS_DISPOSITION_OVERWRITE_IF, 0,
                            NFS_HANDLE_WRITE, &Handle) == NFS_OK, "open the late file");
        CHECK(NfsHandleWrite(Volume, Handle, "later", 5, &Written) == NFS_OK &&
              Written == 5, "write the late file");
        CHECK(NfsHandleClose(Volume, Handle) == NFS_OK, "close the late file");
    }
    NfsUnmount(Volume);
    Volume = NULL;

    {
        int Fd = open("/tmp/boot.img", O_CREAT | O_TRUNC | O_WRONLY, 0644);
        CHECK(Fd >= 0, "create the image file");
        if (Fd >= 0)
        {
            CHECK(write(Fd, g_Image, IMAGE_BYTES) == (ssize_t)IMAGE_BYTES, "write image");
            close(Fd);
        }
    }
    free(g_Image);
    g_Image = NULL;

    printf("[4] mount the image with the boot reader\n");
    g_Fd = open("/tmp/boot.img", O_RDONLY);
    CHECK(g_Fd >= 0, "open the image");

    Io.Read = BootRead;
    Io.Context = NULL;
    Io.DeviceBytes = IMAGE_BYTES;
    Heap.Alloc = BootAlloc;
    Heap.Free = BootFree;

    Status = NFSB_Mount(&Io, &Heap, &Boot);
    CHECK(Status == NFSB_OK, "the boot reader mounts the volume");
    if (Status == NFSB_OK)
    {
        CHECK(Boot->Super.ClusterSize == 4096, "cluster size");
        CHECK(Boot->RecordCount >= 9, "the tree gave us the records");
        CHECK(Boot->DirtyAtMount == 0, "the volume is clean");
        CHECK(Boot->UsedFallbackRoot == 0, "the newest generation verified");

        printf("[5] look up and read back every file\n");
        for (i = 0; i < 6; i++)
        {
            const NFSB_RECORD *Record = NFSB_LookupPath(Boot, g_Wanted[i]);
            unsigned char *Buffer;
            char What[160];

            snprintf(What, sizeof(What), "lookup of %s", g_Wanted[i]);
            CHECK(Record != NULL, What);
            if (!Record)
                continue;

            snprintf(What, sizeof(What), "size of %s (%llu, wanted %u)", g_Wanted[i],
                     (unsigned long long)Record->Size, (unsigned)ExpectedSize[i]);
            CHECK((NFSB_U64)Record->Size == (NFSB_U64)ExpectedSize[i], What);
            snprintf(What, sizeof(What), "not a directory: %s", g_Wanted[i]);
            CHECK(!(Record->Flags & NFSB_FLAG_DIR), What);

            Buffer = (unsigned char *)calloc(1, ExpectedSize[i] + 1);
            {
                NFSB_U32 Read = 0;

                snprintf(What, sizeof(What), "read of %s", g_Wanted[i]);
                CHECK(NFSB_Read(Boot, Record, 0, Buffer, ExpectedSize[i], &Read) == NFSB_OK,
                      What);
                snprintf(What, sizeof(What), "all of %s came back (%u of %u)", g_Wanted[i],
                         (unsigned)Read, (unsigned)ExpectedSize[i]);
                CHECK(Read == ExpectedSize[i], What);
                snprintf(What, sizeof(What), "byte for byte: %s", g_Wanted[i]);
                CHECK(memcmp(Buffer, Expected[i], ExpectedSize[i]) == 0, What);
            }
            free(Buffer);
        }

        printf("[6] paths that must not resolve\n");
        CHECK(NFSB_LookupPath(Boot, "\\ReactOS\\System32\\nope.dll") == NULL, "missing file");
        CHECK(NFSB_LookupPath(Boot, "\\ReactOS\\txtsetup.sif\\more") == NULL, "path through a file");
        CHECK(NFSB_LookupPath(Boot, "\\reactos\\SYSTEM32\\NTOSKRNL.EXE") != NULL,
              "lookup folds case");

        NFSB_Unmount(Boot);
    }

    close(g_Fd);

    printf("[7] a broken primary superblock still boots from the backup\n");
    {
        int In = open("/tmp/boot.img", O_RDONLY);
        int Out = open("/tmp/boot2.img", O_CREAT | O_TRUNC | O_WRONLY, 0644);
        unsigned char *Copy = (unsigned char *)malloc(IMAGE_BYTES);
        ssize_t Got = In >= 0 ? read(In, Copy, IMAGE_BYTES) : -1;

        CHECK(Got == (ssize_t)IMAGE_BYTES, "copy the image");
        memset(Copy, 0, 8);              /* the magic and version of the primary */
        CHECK(write(Out, Copy, IMAGE_BYTES) == (ssize_t)IMAGE_BYTES, "write the copy");
        close(In);
        close(Out);

        g_Fd = open("/tmp/boot2.img", O_RDONLY);
        Status = NFSB_Mount(&Io, &Heap, &Boot);
        CHECK(Status == NFSB_OK, "mount from the backup superblock");
        if (Status == NFSB_OK)
        {
            CHECK(Boot->FromBackupSuper == 1, "the backup is what was used");
            CHECK(NFSB_LookupPath(Boot, "\\ReactOS\\txtsetup.sif") != NULL,
                  "and the tree is still readable");
            NFSB_Unmount(Boot);
        }
        close(g_Fd);
        free(Copy);
    }

    printf("[8] a corrupt live checkpoint root falls back to the previous generation\n");
    {
        int In = open("/tmp/boot.img", O_RDONLY);
        unsigned char *Copy = (unsigned char *)malloc(IMAGE_BYTES);
        NFSB_IO Probe = Io;
        NFSB_VOLUME *First = NULL;
        NFSB_U64 RootBlock = 0;
        int Out;

        CHECK(In >= 0 && read(In, Copy, IMAGE_BYTES) == (ssize_t)IMAGE_BYTES, "copy the image");
        if (In >= 0)
            close(In);

        g_Fd = open("/tmp/boot.img", O_RDONLY);
        CHECK(NFSB_Mount(&Io, &Heap, &First) == NFSB_OK, "mount to learn the root block");
        if (First)
        {
            RootBlock = First->Super.CheckpointRoot.Block;
            CHECK(First->Super.CheckpointRootPrev.Block != 0,
                  "the volume has two generations");
            NFSB_Unmount(First);
        }
        close(g_Fd);

        /* Break the magic of the newest root node: everything under it is now
           unusable, and the reader has to walk the previous root instead. */
        Copy[RootBlock * 4096] ^= 0xFF;
        Out = open("/tmp/boot3.img", O_CREAT | O_TRUNC | O_WRONLY, 0644);
        CHECK(write(Out, Copy, IMAGE_BYTES) == (ssize_t)IMAGE_BYTES, "write the third image");
        close(Out);
        free(Copy);

        g_Fd = open("/tmp/boot3.img", O_RDONLY);
        Probe = Io;
        Status = NFSB_Mount(&Probe, &Heap, &Boot);
        CHECK(Status == NFSB_OK, "mount from the previous generation");
        if (Status == NFSB_OK)
        {
            CHECK(Boot->UsedFallbackRoot == 1, "the fallback root was used");
            CHECK(NFSB_LookupPath(Boot, "\\ReactOS\\txtsetup.sif") != NULL,
                  "the older generation still has the files");
            CHECK(NFSB_LookupPath(Boot, "\\ReactOS\\after-checkpoint.txt") == NULL,
                  "and it does not have the file written after it");
            NFSB_Unmount(Boot);
        }
        close(g_Fd);
    }

    unlink("/tmp/boot.img");
    unlink("/tmp/boot2.img");
    unlink("/tmp/boot3.img");

    printf("== %d checks, %d failures ==\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
