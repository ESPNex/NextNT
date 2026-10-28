/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs.c
 * PURPOSE:     NextFS file system driver (skeleton, phase 1)
 * LICENSE:     GPL-3.0-or-later
 *
 * Status: this driver mounts a NextFS volume and loads the PIFT into RAM using
 * nextfslib. The I/O paths (create/read/write/directory control) are declared
 * and dispatched, but they return STATUS_NOT_IMPLEMENTED: they are phase 2 and
 * are marked TODO(phase2) below. Nothing here is wired into the build yet, see
 * the note at the end of DESIGN.md.
 *
 * Why a skeleton: the point of phase 1 is that the on-disk format, the PIFT and
 * the crash behaviour are correct and tested (nextfslib/self-test). A file
 * system driver that pretends to support IRP_MJ_WRITE before the format is
 * proven would only move the bugs into the cache manager.
 */

#include <ntifs.h>
#include <ntdddisk.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

/* The shared on-disk format library (include dir added by CMakeLists.txt). */
#include "nextfs_format.h"

/* ------------------------------------------------------------------ */
/* Block device callbacks over an NT volume device object              */
/* ------------------------------------------------------------------ */

typedef struct _NFS_NT_DEVICE
{
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    LARGE_INTEGER PartitionOffset;
    LARGE_INTEGER Size;
} NFS_NT_DEVICE, *PNFS_NT_DEVICE;

static NTSTATUS
NtfsNextReadWrite(PNFS_NT_DEVICE Device, LARGE_INTEGER Offset, PVOID Buffer,
                  ULONG Length, BOOLEAN Write)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;
    LARGE_INTEGER Target;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Target.QuadPart = Device->PartitionOffset.QuadPart + Offset.QuadPart;

    Irp = IoBuildSynchronousFsdRequest(Write ? IRP_MJ_WRITE : IRP_MJ_READ,
                                       Device->DeviceObject, Buffer, Length,
                                       &Target, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (IoCallDriver(Device->DeviceObject, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    return IoStatus.Status;
}

static int
NextFsDevRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    LARGE_INTEGER Position;
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;

    Position.QuadPart = (LONGLONG)Offset;
    return NT_SUCCESS(NtfsNextReadWrite(Device, Position, Buffer, Length, FALSE)) ? 0 : -5;
}

static int
NextFsDevWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    LARGE_INTEGER Position;
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;

    Position.QuadPart = (LONGLONG)Offset;
    return NT_SUCCESS(NtfsNextReadWrite(Device, Position, (PVOID)Buffer, Length, TRUE)) ? 0 : -5;
}

static int
NextFsDevSize(void *Context, nfs_u64 *Bytes)
{
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;

    *Bytes = (nfs_u64)Device->Size.QuadPart;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Volume control block                                                */
/* ------------------------------------------------------------------ */

typedef struct _NEXTFS_VCB
{
    ULONG Magic;
    PVPB Vpb;
    PDEVICE_OBJECT VolumeDeviceObject;
    PDEVICE_OBJECT TargetDeviceObject;
    NFS_NT_DEVICE NtDevice;
    NFS_VOLUME_HANDLE Pift;      /* opaque nextfslib volume             */
    ERESOURCE Resource;
} NEXTFS_VCB, *PNEXTFS_VCB;

#define NEXTFS_VCB_MAGIC 'sfxN'

/* ------------------------------------------------------------------ */
/* Dispatch stubs                                                      */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsUnimplemented(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    /* TODO(phase2): the real FSD paths. Listed here so the gaps are explicit:
       IRP_MJ_CREATE, IRP_MJ_READ, IRP_MJ_WRITE, IRP_MJ_QUERY_INFORMATION,
       IRP_MJ_SET_INFORMATION, IRP_MJ_DIRECTORY_CONTROL,
       IRP_MJ_QUERY_VOLUME_INFORMATION, IRP_MJ_FILE_SYSTEM_CONTROL. */
    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsMountVolume(PDEVICE_OBJECT VolumeDeviceObject, PVPB Vpb)
{
    NTSTATUS Status;
    PNEXTFS_VCB Vcb;
    PDEVICE_OBJECT DeviceObject;
    NFS_BDEV Block;
    PNFS_VOLUME Volume = NULL;
    int LibraryStatus;

    Status = IoCreateDevice(NextFsDriverObject, sizeof(NEXTFS_VCB),
                            NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    Vcb = (PNEXTFS_VCB)DeviceObject->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(*Vcb));
    Vcb->Magic = NEXTFS_VCB_MAGIC;
    Vcb->Vpb = Vpb;
    Vcb->VolumeDeviceObject = DeviceObject;
    Vcb->TargetDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject, VolumeDeviceObject);
    if (!Vcb->TargetDeviceObject)
    {
        IoDeleteDevice(DeviceObject);
        return STATUS_UNSUCCESSFUL;
    }

    ExInitializeResourceLite(&Vcb->Resource);

    /* Phase 2: fill PartitionOffset/Size from the volume parameters. */
    Vcb->NtDevice.DeviceObject = Vcb->TargetDeviceObject;
    Vcb->NtDevice.PartitionOffset.QuadPart = 0;
    Vcb->NtDevice.Size.QuadPart = 0;

    Block.Context = &Vcb->NtDevice;
    Block.Read = NextFsDevRead;
    Block.Write = NextFsDevWrite;
    Block.Size = NextFsDevSize;
    Block.Flush = NULL;

    LibraryStatus = NfsMount(&Block, &Volume);
    if (LibraryStatus != 0)
    {
        DPRINT1("NextFS: NfsMount failed (%d), not a NextFS volume\n", LibraryStatus);
        ExDeleteResourceLite(&Vcb->Resource);
        IoDetachDevice(Vcb->TargetDeviceObject);
        IoDeleteDevice(DeviceObject);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Vcb->Pift = (NFS_VOLUME_HANDLE)Volume;

    DeviceObject->Flags |= DO_DIRECT_IO;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    Vpb->DeviceObject = DeviceObject;
    Vpb->RealDevice = VolumeDeviceObject;

    DPRINT1("NextFS: mounted, %d files in the PIFT\n", 0);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

PDRIVER_OBJECT NextFsDriverObject;

NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    NextFsDriverObject = DriverObject;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = NextFsUnimplemented;

    /* TODO(phase2): IRP_MJ_CREATE on a volume device triggers NextFsMountVolume,
       exactly like fastfat's FatMountVolume. */

    DriverObject->DriverUnload = NULL;
    return STATUS_SUCCESS;
}

/* EOF */
