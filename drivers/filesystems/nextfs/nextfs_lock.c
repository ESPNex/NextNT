/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_lock.c
 * PURPOSE:     NextFS byte-range locks, oplocks and the fast I/O table
 * LICENSE:     GPL-3.0-or-later
 *
 * Two decisions live in this file.
 *
 * 1. **Locks and oplocks are real.** NextFS gives both, with the same objects
 *    NTFS and fastfat use (FILE_LOCK and OPLOCK from FsRtl), so the semantics
 *    applications depend on -- LockFile, LockFileEx, the byte ranges a database
 *    takes, the batch oplock Word and Explorer use to know the file is closed --
 *    do not change when a volume is NextFS instead of NTFS.
 *
 * 2. **Data does not go through the cache manager.** nextfslib already keeps the
 *    PIFT in RAM and makes every write copy-on-write through the extent list, so
 *    routing file data through Cc as well would mean two caches that must agree
 *    and a lazy writer that has to re-enter the same commit path. So the fast I/O
 *    table is registered and answers the metadata calls itself, while
 *    CheckIfPossible/Read/Write say "no" and the I/O manager falls back to
 *    IRP_MJ_READ/IRP_MJ_WRITE, which nextfs_rw.c serves straight from the PIFT.
 *    What is *not* here is therefore deliberate, not missing: see DESIGN.md §7.
 */

#include <ntifs.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

/* ------------------------------------------------------------------ */
/* Fast I/O: metadata answers, no data                                 */
/* ------------------------------------------------------------------ */

static BOOLEAN
NextFsQueryStandardBlocking(PNEXTFS_VCB Vcb, nfs_u32 FileId, NFS_STANDARD_INFORMATION *Info)
{
    int NfsStatus;

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsQueryStandardInfo(Vcb->Volume, FileId, Info);
    ExReleaseResourceLite(&Vcb->Resource);
    return NfsStatus == NFS_OK;
}

static PNEXTFS_FILE
NextFsFileOf(PFILE_OBJECT FileObject)
{
    PNEXTFS_FILE File = (PNEXTFS_FILE)FileObject->FsContext2;

    if (!File || File->Magic != NEXTFS_FILE_MAGIC)
        return NULL;
    return File;
}

/*
 * Fast I/O can be called at DISPATCH_LEVEL with Wait == FALSE. Anything that
 * would have to take our resource is only answered when Wait is set, which is the
 * contract: returning FALSE is always allowed, blocking is not.
 */
static BOOLEAN NTAPI
NextFsFastIoQueryBasicInfo(PFILE_OBJECT FileObject, BOOLEAN Wait,
                           PFILE_BASIC_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus,
                           PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File;
    NFS_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!Wait)
        return FALSE;
    File = NextFsFileOf(FileObject);
    if (!File)
        return FALSE;
    if (!NextFsQueryStandardBlocking(File->Vcb, File->FileId, &Info))
        return FALSE;

    Buffer->CreationTime = NextFsUnixToNtTime(Info.CreationTime);
    Buffer->LastAccessTime = NextFsUnixToNtTime(Info.LastAccessTime);
    Buffer->LastWriteTime = NextFsUnixToNtTime(Info.LastWriteTime);
    Buffer->ChangeTime = NextFsUnixToNtTime(Info.ChangeTime);
    Buffer->FileAttributes = NextFsNtAttributes(&Info);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);
    return TRUE;
}

static BOOLEAN NTAPI
NextFsFastIoQueryStandardInfo(PFILE_OBJECT FileObject, BOOLEAN Wait,
                              PFILE_STANDARD_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus,
                              PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File;
    NFS_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!Wait)
        return FALSE;
    File = NextFsFileOf(FileObject);
    if (!File)
        return FALSE;
    if (!NextFsQueryStandardBlocking(File->Vcb, File->FileId, &Info))
        return FALSE;

    Buffer->AllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
    Buffer->EndOfFile.QuadPart = (LONGLONG)Info.EndOfFile;
    Buffer->NumberOfLinks = Info.NumberOfLinks ? Info.NumberOfLinks : 1;
    Buffer->DeletePending = (BOOLEAN)Info.DeletePending;
    Buffer->Directory = (BOOLEAN)Info.Directory;

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);
    return TRUE;
}

static BOOLEAN NTAPI
NextFsFastIoCheckIfPossible(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
                            ULONG Length, BOOLEAN Wait, ULONG LockKey,
                            BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus,
                            PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(CheckForReadOperation);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    /* Data stays on the IRP path on purpose (see the comment at the top). */
    return FALSE;
}

static BOOLEAN NTAPI
NextFsFastIoRead(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                 BOOLEAN Wait, ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus,
                 PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

static BOOLEAN NTAPI
NextFsFastIoWrite(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                  BOOLEAN Wait, ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus,
                  PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

/* These two guard a section creation, not a data path: taking the volume
   resource is enough, and the oplock-level checks are in NextFsCheckOplock. */
static VOID NTAPI
NextFsFastIoAcquireFile(PFILE_OBJECT FileObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    if (File)
        ExAcquireResourceExclusiveLite(&File->Vcb->Resource, TRUE);
}

static VOID NTAPI
NextFsFastIoReleaseFile(PFILE_OBJECT FileObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    if (File)
        ExReleaseResourceLite(&File->Vcb->Resource);
}

/* ------------------------------------------------------------------ */
/* Byte-range locks                                                    */
/* ------------------------------------------------------------------ */

static BOOLEAN NTAPI
NextFsFastIoLock(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
                 PLARGE_INTEGER Length, PEPROCESS ProcessId, ULONG Key,
                 BOOLEAN FailImmediately, BOOLEAN ExclusiveLock, PIO_STATUS_BLOCK IoStatus,
                 PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || !File->FileLock)
        return FALSE;

    /* FsRtlPrivateLock fills IoStatus and returns whether the fast path took it. */
    return FsRtlFastLock(File->FileLock, FileObject, FileOffset, Length, ProcessId,
                         Key, FailImmediately, ExclusiveLock, IoStatus, NULL, FALSE);
}

static BOOLEAN NTAPI
NextFsFastIoUnlockSingle(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset,
                         PLARGE_INTEGER Length, PEPROCESS ProcessId, ULONG Key,
                         PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || !File->FileLock)
        return FALSE;

    IoStatus->Status = FsRtlFastUnlockSingle(File->FileLock, FileObject, FileOffset,
                                             Length, ProcessId, Key, NULL, FALSE);
    return TRUE;
}

static BOOLEAN NTAPI
NextFsFastIoUnlockAll(PFILE_OBJECT FileObject, PEPROCESS ProcessId,
                      PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || !File->FileLock)
        return FALSE;

    IoStatus->Status = FsRtlFastUnlockAll(File->FileLock, FileObject, ProcessId, NULL);
    return TRUE;
}

static BOOLEAN NTAPI
NextFsFastIoUnlockAllByKey(PFILE_OBJECT FileObject, PVOID ProcessId, ULONG Key,
                           PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File = NextFsFileOf(FileObject);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || !File->FileLock)
        return FALSE;

    IoStatus->Status = FsRtlFastUnlockAllByKey(File->FileLock, FileObject,
                                               (PEPROCESS)ProcessId, Key, NULL);
    return TRUE;
}

NTSTATUS NTAPI
NextFsLockControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_FILE File = NextFsFileOf(IrpSp->FileObject);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || !File->FileLock)
    {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_HANDLE;
    }

    /* FsRtl owns the whole byte-range story: LOCK_CONTROL on a file object is
       exactly what FsRtlProcessFileLock understands. */
    Status = FsRtlProcessFileLock(File->FileLock, Irp, NULL);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Oplocks                                                             */
/* ------------------------------------------------------------------ */

NTSTATUS
NextFsInitializeLocking(PNEXTFS_FILE File)
{
    File->FileLock = FsRtlAllocateFileLock(NULL, NULL);
    if (!File->FileLock)
        return STATUS_INSUFFICIENT_RESOURCES;

    FsRtlInitializeOplock(&File->Oplock);
    File->OplockInitialized = TRUE;
    return STATUS_SUCCESS;
}

VOID
NextFsUninitializeLocking(PNEXTFS_FILE File, PFILE_OBJECT FileObject)
{
    if (!File)
        return;

    if (File->FileLock)
    {
        /* Every range this file object still holds goes now: a handle that closes
           must not leave locks behind for the next open. */
        if (FileObject)
        {
            FsRtlFastUnlockAll(File->FileLock, FileObject,
                               IoGetCurrentProcess(), NULL);
            FsRtlFastUnlockAllByKey(File->FileLock, FileObject,
                                    IoGetCurrentProcess(), 0, NULL);
        }
        FsRtlFreeFileLock(File->FileLock);
        File->FileLock = NULL;
    }

    if (File->OplockInitialized)
    {
        FsRtlUninitializeOplock(&File->Oplock);
        File->OplockInitialized = FALSE;
    }
}

/*
 * Called before anything that changes the file's data or metadata. FsRtl reports
 * STATUS_PENDING while an oplock break is in flight; the IRP is then owned by the
 * oplock code and this driver must not touch or complete it.
 */
NTSTATUS
NextFsCheckOplock(PNEXTFS_FILE File, PIRP Irp)
{
    if (!File || !File->OplockInitialized)
        return STATUS_SUCCESS;
    return FsRtlCheckOplock(&File->Oplock, Irp, 0, NULL, NULL);
}

/* FSCTL_REQUEST_* and FSCTL_*_BREAK_* all go through one FsRtl call. */
NTSTATUS
NextFsOplockControl(PNEXTFS_FILE File, PIRP Irp, ULONG ControlCode, ULONG OutputLength)
{
    UNREFERENCED_PARAMETER(ControlCode);

    if (!File || !File->OplockInitialized)
        return STATUS_INVALID_DEVICE_REQUEST;

    return FsRtlOplockFsctrl(&File->Oplock, Irp, OutputLength);
}

/* ------------------------------------------------------------------ */
/* The table itself                                                    */
/* ------------------------------------------------------------------ */

static BOOLEAN NTAPI
NextFsFastIoQueryNetworkOpenInfo(PFILE_OBJECT FileObject, BOOLEAN Wait,
                                 PFILE_NETWORK_OPEN_INFORMATION Buffer,
                                 PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_FILE File;
    NFS_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!Wait)
        return FALSE;
    File = NextFsFileOf(FileObject);
    if (!File)
        return FALSE;
    if (!NextFsQueryStandardBlocking(File->Vcb, File->FileId, &Info))
        return FALSE;

    Buffer->CreationTime = NextFsUnixToNtTime(Info.CreationTime);
    Buffer->LastAccessTime = NextFsUnixToNtTime(Info.LastAccessTime);
    Buffer->LastWriteTime = NextFsUnixToNtTime(Info.LastWriteTime);
    Buffer->ChangeTime = NextFsUnixToNtTime(Info.ChangeTime);
    Buffer->AllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
    Buffer->EndOfFile.QuadPart = (LONGLONG)Info.EndOfFile;
    Buffer->FileAttributes = NextFsNtAttributes(&Info);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return TRUE;
}

/* Nothing is cached in this driver, so the cache-manager flush hooks have nothing
   to serialise against; they say so instead of pretending otherwise. */
static NTSTATUS NTAPI
NextFsFastIoAcquireForCcFlush(PFILE_OBJECT FileObject, PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
NextFsFastIoReleaseForCcFlush(PFILE_OBJECT FileObject, PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);
    return STATUS_SUCCESS;
}

/*
 * Positional, one line per field of FAST_IO_DISPATCH and nothing skipped: the
 * order is the struct's, so a field added by a future SDK cannot silently shift
 * the rest. The slots left NULL are the cache-manager and MDL entry points that
 * nextfs_rw.c handles through IRPs.
 */
static FAST_IO_DISPATCH NextFsFastIoDispatch =
{
    sizeof(FAST_IO_DISPATCH),           /* SizeOfFastIoDispatch */
    NextFsFastIoCheckIfPossible,        /* FastIoCheckIfPossible: data stays on IRP */
    NextFsFastIoRead,                   /* FastIoRead */
    NextFsFastIoWrite,                  /* FastIoWrite */
    NextFsFastIoQueryBasicInfo,         /* FastIoQueryBasicInfo */
    NextFsFastIoQueryStandardInfo,      /* FastIoQueryStandardInfo */
    NextFsFastIoLock,                   /* FastIoLock */
    NextFsFastIoUnlockSingle,           /* FastIoUnlockSingle */
    NextFsFastIoUnlockAll,              /* FastIoUnlockAll */
    NextFsFastIoUnlockAllByKey,         /* FastIoUnlockAllByKey */
    NULL,                               /* FastIoDeviceControl */
    NextFsFastIoAcquireFile,            /* AcquireFileForNtCreateSection */
    NextFsFastIoReleaseFile,            /* ReleaseFileForNtCreateSection */
    NULL,                               /* FastIoDetachDevice */
    NextFsFastIoQueryNetworkOpenInfo,   /* FastIoQueryNetworkOpenInfo */
    NULL,                               /* AcquireForModWrite */
    NULL,                               /* MdlRead */
    NULL,                               /* MdlReadComplete */
    NULL,                               /* PrepareMdlWrite */
    NULL,                               /* MdlWriteComplete */
    NULL,                               /* FastIoReadCompressed */
    NULL,                               /* FastIoWriteCompressed */
    NULL,                               /* MdlReadCompleteCompressed */
    NULL,                               /* MdlWriteCompleteCompressed */
    NULL,                               /* FastIoQueryOpen */
    NULL,                               /* ReleaseForModWrite */
    NextFsFastIoAcquireForCcFlush,      /* AcquireForCcFlush */
    NextFsFastIoReleaseForCcFlush,      /* ReleaseForCcFlush */
};

VOID
NextFsRegisterFastIo(PDRIVER_OBJECT DriverObject)
{
    DriverObject->FastIoDispatch = &NextFsFastIoDispatch;
}

/* EOF */
