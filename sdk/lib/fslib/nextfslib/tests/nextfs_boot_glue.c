/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tests/nextfs_boot_glue.c
 * PURPOSE:     Check the loader's ARC glue over a real NextFS image
 * LICENSE:     GPL-3.0-or-later
 *
 * Drives boot/nextldr/nextldr/lib/fs/nextfs.c through the DEVVTBL exactly the way
 * fs.c does: mount, open by path, read, seek, query, close -- plus the things that
 * must be refused (a write open, a missing file, a seek past the end). ARC is
 * stubbed over pread by tests/bootstub/, which reproduces the contract of the
 * real loader headers and nothing else.
 *
 *   gcc -std=gnu11 -O2 -Itests/bootstub -I../../../../boot/nextldr/nextldr/include -I. \
 *       -o /tmp/nextfs_boot_glue tests/nextfs_boot_glue.c \
 *       ../../../../boot/nextldr/nextldr/lib/fs/nextfs.c \
 *       ../../../../boot/nextldr/nextldr/lib/fs/nextfs_core.c \
 *       nextfs_crc32c.c nextfs_super.c nextfs_pift.c nextfs_journal.c \
 *       nextfs_tree.c nextfs_snapshot.c nextfs_security.c nextfs_namespace.c \
 *       nextfs_handle.c nextfs_volume.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "nextfs_format.h"
#include "fs/nextfs.h"

#define IMAGE_BYTES (16u * 1024u * 1024u)

static unsigned char *g_Image;
static int g_Checks, g_Failures;
static int g_Fd = -1;

#define CHECK(Cond, What)                                          \
    do {                                                           \
        g_Checks++;                                                \
        if (!(Cond)) {                                             \
            g_Failures++;                                          \
            printf("  FAIL  %s  (line %d)\n", What, __LINE__);      \
        }                                                          \
    } while (0)

/* ---- nextfslib device ---- */
static int LibRead(void *C, nfs_u64 O, void *B, nfs_u32 L)
{ (void)C; if (O + L > IMAGE_BYTES) return NFS_EIO; memcpy(B, g_Image + O, L); return NFS_OK; }
static int LibWrite(void *C, nfs_u64 O, const void *B, nfs_u32 L)
{ (void)C; if (O + L > IMAGE_BYTES) return NFS_EIO; memcpy(g_Image + O, B, L); return NFS_OK; }
static int LibSize(void *C, nfs_u64 *B) { (void)C; *B = IMAGE_BYTES; return NFS_OK; }

/* ---- ARC stub ---- */
static PVOID g_Specific[MAX_FDS];

PVOID FrLdrTempAlloc(size_t Size, ULONG Tag) { (void)Tag; return malloc(Size); }
VOID FrLdrTempFree(PVOID Block, ULONG Tag) { (void)Tag; free(Block); }

ARC_STATUS ArcSeek(ULONG DeviceId, LARGE_INTEGER *Position, SEEKMODE SeekMode)
{
    (void)DeviceId; (void)SeekMode;
    if (lseek(g_Fd, (off_t)Position->QuadPart, SEEK_SET) < 0)
        return EIO;
    return ESUCCESS;
}

ARC_STATUS ArcRead(ULONG DeviceId, VOID *Buffer, ULONG N, ULONG *Count)
{
    ssize_t Got;
    (void)DeviceId;
    Got = read(g_Fd, Buffer, N);
    if (Got < 0)
        return EIO;
    *Count = (ULONG)Got;
    return ESUCCESS;
}

ULONG FsGetDeviceId(ULONG FileId) { (void)FileId; return 0; }
VOID *FsGetDeviceSpecific(ULONG FileId) { return (FileId < MAX_FDS) ? g_Specific[FileId] : NULL; }
VOID FsSetDeviceSpecific(ULONG FileId, PVOID Specific) { if (FileId < MAX_FDS) g_Specific[FileId] = Specific; }
ARC_STATUS FsGetVolumeSize(ULONG DeviceId, ULONGLONG *VolumeSize) { (void)DeviceId; *VolumeSize = IMAGE_BYTES; return ESUCCESS; }
VOID FileSystemError(PCSTR Message) { printf("  [loader] %s\n", Message); }

int main(void)
{
    NFS_BDEV Device;
    PNFS_VOLUME Volume = NULL;
    const DEVVTBL *Table;
    ULONG FileId = 0, Count = 0;
    CHAR Buffer[256];
    FILEINFORMATION Info;
    LARGE_INTEGER Position;
    int Fd;

    memset(&Device, 0, sizeof(Device));
    Device.Read = LibRead;
    Device.Write = LibWrite;
    Device.Size = LibSize;

    printf("[1] build the image\n");
    g_Image = calloc(1, IMAGE_BYTES);
    CHECK(NfsFormat(&Device, "GLUETEST", 4096) == NFS_OK, "format");
    CHECK(NfsMount(&Device, &Volume) == NFS_OK, "mount");
    {
        nfs_u32 Handle = 0, Written = 0;
        CHECK(NfsHandleOpen(Volume, "\\boot.ini", NFS_DISPOSITION_OVERWRITE_IF, 0,
                            NFS_HANDLE_WRITE, &Handle) == NFS_OK, "create boot.ini");
        CHECK(NfsHandleWrite(Volume, Handle, "multi(0)disk(0)rdisk(0)partition(1)", 34,
                             &Written) == NFS_OK && Written == 34, "write boot.ini");
        NfsHandleClose(Volume, Handle);
    }
    NfsUnmount(Volume);
    Volume = NULL;

    Fd = open("/tmp/glue.img", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(Fd >= 0 && write(Fd, g_Image, IMAGE_BYTES) == (ssize_t)IMAGE_BYTES, "write image");
    close(Fd);
    free(g_Image);

    printf("[2] mount through the ARC table\n");
    g_Fd = open("/tmp/glue.img", O_RDONLY);
    Table = NextFsMount(0);
    CHECK(Table != NULL, "NextFsMount returns a device table");
    if (!Table)
        return 1;
    CHECK(Table->ServiceName[0] == 'n', "service name is \"nextfs\"");
    CHECK(NextFsGetVolumeSize(0) == IMAGE_BYTES, "volume size");

    printf("[3] open, read, seek, query, close\n");
    CHECK(Table->Open("\\boot.ini", OpenReadOnly, &FileId) == ESUCCESS, "open \\boot.ini");
    memset(Buffer, 0, sizeof(Buffer));
    CHECK(Table->Read(FileId, Buffer, 34, &Count) == ESUCCESS && Count == 34, "read 34 bytes");
    CHECK(memcmp(Buffer, "multi(0)disk(0)rdisk(0)partition(1)", 34) == 0, "the bytes");
    CHECK(Table->Read(FileId, Buffer, 10, &Count) == ESUCCESS && Count == 0,
          "reading past the end gives nothing");

    Position.QuadPart = 8;               /* "multi(0)" is eight bytes */
    CHECK(Table->Seek(FileId, &Position, SeekAbsolute) == ESUCCESS, "seek to 8");
    CHECK(Table->Read(FileId, Buffer, 6, &Count) == ESUCCESS && Count == 6 &&
          memcmp(Buffer, "disk(0", 6) == 0, "read from the middle");

    Position.QuadPart = 4;
    CHECK(Table->Seek(FileId, &Position, SeekRelative) == ESUCCESS, "seek relative");
    CHECK(Table->GetFileInformation(FileId, &Info) == ESUCCESS, "file information");
    CHECK(Info.EndingAddress.QuadPart == 34, "end of file");
    CHECK(Info.CurrentAddress.QuadPart == 18, "current position");
    CHECK(Info.FileNameLength == 8 && memcmp(Info.FileName, "boot.ini", 8) == 0, "file name");
    CHECK(Table->Close(FileId) == ESUCCESS, "close");

    printf("[4] what must be refused\n");
    FileId = 0;
    CHECK(Table->Open("\\boot.ini", OpenReadWrite, &FileId) == EACCES,
          "a write open is refused");
    FileId = 0;
    CHECK(Table->Open("\\nope.sys", OpenReadOnly, &FileId) == ENOENT, "a missing file");
    FileId = 0;
    CHECK(Table->Open("\\boot.ini", OpenReadOnly, &FileId) == ESUCCESS, "reopen");
    Position.QuadPart = 100;
    CHECK(Table->Seek(FileId, &Position, SeekAbsolute) == EINVAL, "seek past the end");
    Table->Close(FileId);

    close(g_Fd);
    unlink("/tmp/glue.img");

    printf("== %d checks, %d failures ==\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
