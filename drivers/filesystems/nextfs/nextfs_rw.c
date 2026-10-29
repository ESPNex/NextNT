/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_rw.c
 * PURPOSE:     NextFS IRP_MJ_READ, IRP_MJ_WRITE, IRP_MJ_FLUSH_BUFFERS
 * LICENSE:     GPL-3.0-or-later
 *
 * Every write lands in NfsHandleWrite, which does the copy-on-write of the
 * touched clusters and the write-through of the PIFT record before it returns.
 * There is no deferred metadata here: the only thing this layer adds is the pool
 * buffer copy and the NT status codes.
 */

#include <ntifs.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

/* Direct I/O gives us an MDL; buffered gives us a system buffer. Both are
   supported so the driver works whether or not the cache manager is enabled. */
static PVOID
NextFsMapBuffer(PIRP Irp)
{
    if (Irp->MdlAddress)
        return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    return Irp->AssociatedIrp.SystemBuffer;
}

/* Offsets: an explicit one wins; -1 means "use the file object position". */
static LONGLONG
NextFsResolveOffset(PIO_STACK_LOCATION IrpSp, PFILE_OBJECT FileObject)
{
    LARGE_INTEGER Offset = IrpSp->Parameters.Read.ByteOffset;

    if (Offset.QuadPart == -1)
        return FileObject->CurrentByteOffset.QuadPart;
    return Offset.QuadPart;
}

NTSTATUS NTAPI
NextFsRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    PVOID Buffer;
    ULONG Length;
    nfs_u32 Read = 0;
    LONGLONG Offset;
    int NfsStatus;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (!File || File->Magic != NEXTFS_FILE_MAGIC || File->Handle == 0)
        return NextFsCompleteIo(Irp, STATUS_INVALID_HANDLE, 0);

    Length = IrpSp->Parameters.Read.Length;
    if (Length == 0)
        return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
    if (!(File->Access & NFS_HANDLE_READ))
        return NextFsCompleteIo(Irp, STATUS_ACCESS_DENIED, 0);
    if (File->Flags & NEXTFS_FILE_DIRECTORY)
        return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Buffer = NextFsMapBuffer(Irp);
    if (!Buffer)
        return NextFsCompleteIo(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    Offset = NextFsResolveOffset(IrpSp, IrpSp->FileObject);
    if (Offset < 0)
        return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsHandleSeek(Vcb->Volume, File->Handle, (nfs_u64)Offset);
    if (NfsStatus == NFS_OK)
        NfsStatus = NfsHandleRead(Vcb->Volume, File->Handle, Buffer, Length, &Read);
    ExReleaseResourceLite(&Vcb->Resource);

    if (NfsStatus != NFS_OK)
        return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);

    if (Read == 0)
        return NextFsCompleteIo(Irp, STATUS_END_OF_FILE, 0);

    IrpSp->FileObject->CurrentByteOffset.QuadPart = Offset + Read;
    return NextFsCompleteIo(Irp, STATUS_SUCCESS, Read);
}

NTSTATUS NTAPI
NextFsWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    PVOID Buffer;
    ULONG Length;
    nfs_u32 Written = 0;
    LONGLONG Offset;
    int NfsStatus;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (!File || File->Magic != NEXTFS_FILE_MAGIC || File->Handle == 0)
        return NextFsCompleteIo(Irp, STATUS_INVALID_HANDLE, 0);

    Length = IrpSp->Parameters.Write.Length;
    if (Length == 0)
        return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
    if (!(File->Access & NFS_HANDLE_WRITE))
        return NextFsCompleteIo(Irp, STATUS_ACCESS_DENIED, 0);
    if (File->Flags & NEXTFS_FILE_DIRECTORY)
        return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Buffer = NextFsMapBuffer(Irp);
    if (!Buffer)
        return NextFsCompleteIo(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    Offset = NextFsResolveOffset(IrpSp, IrpSp->FileObject);
    if (Offset < 0)
        return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

    if (IrpSp->Parameters.Write.ByteOffset.QuadPart == -1 &&
        (File->Flags & NEXTFS_FILE_APPEND))
    {
        NFS_PIFT_RECORD Record;

        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        NfsStatus = NfsHandleQuery(Vcb->Volume, File->Handle, &Record);
        ExReleaseResourceLite(&Vcb->Resource);
        if (NfsStatus != NFS_OK)
            return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
        Offset = (LONGLONG)Record.Size;
    }

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsHandleSeek(Vcb->Volume, File->Handle, (nfs_u64)Offset);
    if (NfsStatus == NFS_OK)
        NfsStatus = NfsHandleWrite(Vcb->Volume, File->Handle, Buffer, Length, &Written);
    ExReleaseResourceLite(&Vcb->Resource);

    if (NfsStatus != NFS_OK)
        return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);

    IrpSp->FileObject->CurrentByteOffset.QuadPart = Offset + Written;

    /* Write-through: a FILE_FLAG_WRITE_THROUGH handle pays for the checkpoint
       here instead of at close time. */
    if (IrpSp->FileObject->Flags & FO_WRITE_THROUGH)
    {
        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        NfsSync(Vcb->Volume);
        ExReleaseResourceLite(&Vcb->Resource);
    }

    return NextFsCompleteIo(Irp, STATUS_SUCCESS, Written);
}

NTSTATUS NTAPI
NextFsFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    int NfsStatus;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    /* The PIFT is already on disk: every change write-throughs its record as it
       happens. A flush is therefore a checkpoint of the tree and bitmap, not the
       first time anything is written. */
    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsSync(Vcb->Volume);
    ExReleaseResourceLite(&Vcb->Resource);

    return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
}

NTSTATUS NTAPI
NextFsShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);

    if (Vcb && Vcb->Volume)
    {
        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        NfsSync(Vcb->Volume);
        Vcb->Shutdown = TRUE;
        ExReleaseResourceLite(&Vcb->Resource);
    }

    return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
}

/* EOF */
