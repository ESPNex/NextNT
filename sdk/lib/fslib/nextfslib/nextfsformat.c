/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfsformat.c
 * PURPOSE:     The NextFS formatter entry point for the setup library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * This is the only file of nextfslib that is not portable C. It turns an NT
 * volume path into the four block-device callbacks nextfslib asks for, calls
 * NfsFormat, and then mounts and unmounts once so the volume leaves with a
 * checkpoint written -- which is what nextldr needs, because the loader reads the
 * checkpoint tree and never the journal.
 *
 * Everything below the callbacks is the same code nextfs.sys runs: the setup path
 * and the runtime path cannot drift apart, because there is only one of them.
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>

/* DISK_GEOMETRY_EX and IOCTL_DISK_GET_DRIVE_GEOMETRY_EX come from the DDK
 * ntdddisk.h, already reachable through the ndk headers above; including the
 * psdk copy as well redeclares MEDIA_TYPE and every F5_ geometry. */

#include <fmifs/fmifs.h>

#include "nextfs_format.h"

/* A volume smaller than this is a mistake, not a NextFS volume: the superblock,
 * two PIFT halves and a journal do not fit in less. */
#define NEXTFS_MIN_BYTES (1024ull * 1024ull)

/* nextfslib defaults its allocator hooks to malloc/free, which a native DLL does
 * not have. The build renames those two symbols to these (CMakeLists.txt), so the
 * library's default allocator IS the process heap and no CRT is needed. */
void *
NextfsSetupAlloc(size_t Size)
{
    return RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
}

void
NextfsSetupFree(void *Block)
{
    RtlFreeHeap(RtlGetProcessHeap(), 0, Block);
}

/* nextfslib hands out a monotonic counter without a clock installed. A file
 * system on disk should carry real dates, so install one: NT time (100ns since
 * 1601) to Unix seconds, which is what the format stores. */
static nfs_u64
NextfsSetupClock(VOID)
{
    LARGE_INTEGER Now;

    NtQuerySystemTime(&Now);
    return (nfs_u64)(Now.QuadPart / 10000000ull) - 11644473600ull;
}

typedef struct _NEXTFS_VOLUME_IO
{
    HANDLE    Handle;
    HANDLE    Event;
    ULONG     SectorSize;
    ULONGLONG Bytes;
} NEXTFS_VOLUME_IO, *PNEXTFS_VOLUME_IO;

static
NTSTATUS
NextfsReadWrite(
    _In_ PNEXTFS_VOLUME_IO Io,
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONGLONG Offset,
    _In_ BOOLEAN Write)
{
    LARGE_INTEGER ByteOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    ByteOffset.QuadPart = (LONGLONG)Offset;

    Status = Write
        ? NtWriteFile(Io->Handle, Io->Event, NULL, NULL, &IoStatusBlock,
                      Buffer, Length, &ByteOffset, NULL)
        : NtReadFile(Io->Handle, Io->Event, NULL, NULL, &IoStatusBlock,
                     Buffer, Length, &ByteOffset, NULL);

    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Io->Event, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    /* A short transfer on a partition means the geometry lied about the size;
     * nextfslib must see it as an error and not as data. */
    if (IoStatusBlock.Information != Length)
        return STATUS_UNEXPECTED_IO_ERROR;

    return STATUS_SUCCESS;
}

static int
NextfsDevRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    return NT_SUCCESS(NextfsReadWrite(Context, Buffer, Length, Offset, FALSE))
           ? NFS_OK : NFS_EIO;
}

static int
NextfsDevWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    return NT_SUCCESS(NextfsReadWrite(Context, (PVOID)Buffer, Length, Offset, TRUE))
           ? NFS_OK : NFS_EIO;
}

static int
NextfsDevSize(void *Context, nfs_u64 *Bytes)
{
    PNEXTFS_VOLUME_IO Io = Context;

    *Bytes = Io->Bytes;
    return NFS_OK;
}

static int
NextfsDevFlush(void *Context)
{
    PNEXTFS_VOLUME_IO Io = Context;
    IO_STATUS_BLOCK IoStatusBlock;

    return NT_SUCCESS(NtFlushBuffersFile(Io->Handle, &IoStatusBlock))
           ? NFS_OK : NFS_EIO;
}

static
NTSTATUS
NextfsOpenVolume(
    _In_ PCUNICODE_STRING DriveRoot,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    HANDLE VolumeHandle;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)DriveRoot,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtOpenFile(&VolumeHandle,
                        FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NO_INTERMEDIATE_BUFFERING);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NextfsFormat: cannot open '%wZ', Status 0x%08lx\n",
                DriveRoot, Status);
        return Status;
    }

    *Handle = VolumeHandle;
    return STATUS_SUCCESS;
}

/* Best effort: a volume that is not mounted cannot be locked, and that is fine. */
static
VOID
NextfsLockAndDismount(
    _In_ HANDLE VolumeHandle,
    _In_ HANDLE Event)
{
    IO_STATUS_BLOCK IoStatusBlock;

    NtFsControlFile(VolumeHandle, Event, NULL, NULL, &IoStatusBlock,
                    FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
    if (IoStatusBlock.Status == STATUS_PENDING)
        NtWaitForSingleObject(Event, FALSE, NULL);

    NtFsControlFile(VolumeHandle, Event, NULL, NULL, &IoStatusBlock,
                    FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    if (IoStatusBlock.Status == STATUS_PENDING)
        NtWaitForSingleObject(Event, FALSE, NULL);
}

static
NTSTATUS
NextfsQueryGeometry(
    _In_ PNEXTFS_VOLUME_IO Io)
{
    DISK_GEOMETRY_EX Geometry;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Status = NtDeviceIoControlFile(Io->Handle, Io->Event, NULL, NULL,
                                   &IoStatusBlock,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                   NULL, 0,
                                   &Geometry, sizeof(Geometry));
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Io->Event, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Io->SectorSize = Geometry.Geometry.BytesPerSector;
    Io->Bytes = (ULONGLONG)Geometry.DiskSize.QuadPart;
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
NextfsFormat(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN QuickFormat,
    IN BOOLEAN BackwardCompatible,
    IN MEDIA_TYPE MediaType,
    IN PUNICODE_STRING Label,
    IN ULONG ClusterSize)
{
    NEXTFS_VOLUME_IO Io;
    NFS_BDEV Device;
    ANSI_STRING AnsiLabel;
    char VolumeLabel[NFS_LABEL_MAX];
    nfs_u32 Cluster;
    NTSTATUS Status;
    int NfsStatus;
    BOOLEAN Success = FALSE;

    UNREFERENCED_PARAMETER(QuickFormat);       /* NextFS has no slow format */
    UNREFERENCED_PARAMETER(BackwardCompatible);/* and no older format version */
    UNREFERENCED_PARAMETER(MediaType);

    RtlZeroMemory(&Io, sizeof(Io));
    RtlZeroMemory(VolumeLabel, sizeof(VolumeLabel));

    Status = NtCreateEvent(&Io.Event, EVENT_ALL_ACCESS, NULL,
                           NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = NextfsOpenVolume(DriveRoot, &Io.Handle);
    if (!NT_SUCCESS(Status))
        goto Quit;

    NextfsLockAndDismount(Io.Handle, Io.Event);

    Status = NextfsQueryGeometry(&Io);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NextfsFormat: geometry query failed, Status 0x%08lx\n", Status);
        goto Quit;
    }

    if (Io.Bytes < NEXTFS_MIN_BYTES)
    {
        DPRINT1("NextfsFormat: volume is %I64u bytes, too small for NextFS\n",
                Io.Bytes);
        Status = STATUS_DISK_FULL;
        goto Quit;
    }

    /* The label rides in the superblock, as an ASCII string of at most
     * NFS_LABEL_MAX bytes, NUL terminated. */
    if (Label && Label->Length)
    {
        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&AnsiLabel, Label, TRUE)))
        {
            ULONG Length = AnsiLabel.Length;

            if (Length > NFS_LABEL_MAX - 1)
                Length = NFS_LABEL_MAX - 1;
            RtlCopyMemory(VolumeLabel, AnsiLabel.Buffer, Length);
            VolumeLabel[Length] = ANSI_NULL;
            RtlFreeAnsiString(&AnsiLabel);
        }
    }

    /* Cluster size: the caller may ask for one, otherwise the library default.
     * Anything that is not a whole multiple of the format minimum is ignored
     * rather than rounded, because a silently different cluster size is worse
     * than the default one. */
    Cluster = NFS_CLUSTER_MIN;
    if (ClusterSize >= NFS_CLUSTER_MIN && (ClusterSize % NFS_CLUSTER_MIN) == 0)
        Cluster = ClusterSize;
    if (Io.SectorSize > Cluster)
    {
        ULONG Rounded = 0;

        while (Rounded < Io.SectorSize)
            Rounded += NFS_CLUSTER_MIN;
        Cluster = Rounded;
    }

    /* In this build the library's default allocator already is the process heap;
     * installing it again keeps the entry point correct even if nextfslib is
     * ever built with a CRT behind it. */
    NfsSetAllocators(NextfsSetupAlloc, NextfsSetupFree);
    NfsSetClock(NextfsSetupClock);

    Device.Context = &Io;
    Device.Read    = NextfsDevRead;
    Device.Write   = NextfsDevWrite;
    Device.Size    = NextfsDevSize;
    Device.Flush   = NextfsDevFlush;

    DPRINT1("NextfsFormat: '%wZ', %I64u bytes, cluster %lu, label '%s'\n",
            DriveRoot, Io.Bytes, Cluster, VolumeLabel);

    NfsStatus = NfsFormat(&Device, VolumeLabel, Cluster);
    if (NfsStatus != NFS_OK)
    {
        DPRINT1("NextfsFormat: NfsFormat failed, %d\n", NfsStatus);
        Status = STATUS_UNEXPECTED_IO_ERROR;
        goto Quit;
    }

    /*
     * Mount and unmount: NfsUnmount goes through NfsSync, which writes the first
     * checkpoint and clears the dirty flag. Without this step the volume would be
     * a superblock and nothing else -- bootable by nothing, least of all nextldr.
     */
    {
        NFS_VOLUME *Volume = NULL;

        NfsStatus = NfsMount(&Device, &Volume);
        if (NfsStatus != NFS_OK)
        {
            DPRINT1("NextfsFormat: the new volume does not mount, %d\n", NfsStatus);
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto Quit;
        }

        /* NfsUnmount is void: it syncs on the way out. Ask for the checkpoint
         * explicitly first, so a failure is still reportable. */
        NfsStatus = NfsSync(Volume);
        if (NfsStatus != NFS_OK)
        {
            DPRINT1("NextfsFormat: checkpoint failed, %d\n", NfsStatus);
            NfsDiscard(Volume);
            Status = STATUS_UNEXPECTED_IO_ERROR;
            goto Quit;
        }

        NfsUnmount(Volume);
    }

    Success = TRUE;

Quit:
    if (Io.Handle)
    {
        IO_STATUS_BLOCK IoStatusBlock;

        NtFsControlFile(Io.Handle, Io.Event, NULL, NULL, &IoStatusBlock,
                        FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
        NtClose(Io.Handle);
    }
    if (Io.Event)
        NtClose(Io.Event);

    if (!Success)
        DPRINT1("NextfsFormat: failed, Status 0x%08lx\n", Status);

    if (Callback)
        Callback(DONE, 0, &Success);

    return Success;
}

BOOLEAN
NTAPI
NextfsChkdsk(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID pUnknown1,
    IN PVOID pUnknown2,
    IN PVOID pUnknown3,
    IN PVOID pUnknown4,
    IN PULONG ExitStatus)
{
    BOOLEAN Success = FALSE;

    UNREFERENCED_PARAMETER(DriveRoot);
    UNREFERENCED_PARAMETER(FixErrors);
    UNREFERENCED_PARAMETER(Verbose);
    UNREFERENCED_PARAMETER(CheckOnlyIfDirty);
    UNREFERENCED_PARAMETER(ScanDrive);
    UNREFERENCED_PARAMETER(pUnknown1);
    UNREFERENCED_PARAMETER(pUnknown2);
    UNREFERENCED_PARAMETER(pUnknown3);
    UNREFERENCED_PARAMETER(pUnknown4);

    /* NextFS checks its own structures at mount (superblock CRC, node CRC and
     * block number, checkpoint fallback), so there is nothing here yet. Saying
     * "clean" without looking would be a lie, so say what is true. */
    if (ExitStatus)
        *ExitStatus = STATUS_NOT_IMPLEMENTED;

    if (Callback)
        Callback(DONE, 0, &Success);

    return Success;
}

/* EOF */
