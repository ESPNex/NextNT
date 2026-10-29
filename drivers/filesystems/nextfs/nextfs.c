/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs.c
 * PURPOSE:     NextFS file system driver: entry point, mount, dispatch table
 * LICENSE:     GPL-3.0-or-later
 *
 * What this driver is, in one paragraph: nextfslib owns the on-disk format, the
 * PIFT, the journal and the copy-on-write data path, and is self-tested outside
 * the kernel (sdk/lib/fslib/nextfslib/tests). This file adds the three things a
 * kernel needs and a library cannot have: an allocator, a block device, and a
 * dispatch table. So the driver deliberately contains no file system logic --
 * if a rule about NextFS lives here instead of in nextfslib, it cannot be tested
 * and it is in the wrong place.
 */

#include <ntifs.h>
#include <ntdddisk.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

/* ------------------------------------------------------------------ */
/* Allocation: nextfslib asks us, we ask the pool                       */
/* ------------------------------------------------------------------ */

static void *
NextFsAlloc(size_t Size)
{
    return ExAllocatePoolWithTag(NonPagedPool, Size, NEXTFS_POOL_TAG);
}

static void
NextFsFree(void *Block)
{
    if (Block)
        ExFreePoolWithTag(Block, NEXTFS_POOL_TAG);
}

/* ------------------------------------------------------------------ */
/* Status translation                                                  */
/* ------------------------------------------------------------------ */

NTSTATUS
NextFsNfsStatus(int NfsStatus)
{
    switch (NfsStatus)
    {
        case NFS_OK:            return STATUS_SUCCESS;
        case NFS_EINVAL:        return STATUS_INVALID_PARAMETER;
        case NFS_ENOENT:        return STATUS_OBJECT_NAME_NOT_FOUND;
        case NFS_EEXIST:        return STATUS_OBJECT_NAME_COLLISION;
        case NFS_ENOSPC:        return STATUS_DISK_FULL;
        case NFS_ECORRUPT:      return STATUS_FILE_CORRUPT_ERROR;
        case NFS_EUNSUPPORTED:  return STATUS_NOT_SUPPORTED;
        case NFS_EIO:
        default:                return STATUS_IO_DEVICE_ERROR;
    }
}

NTSTATUS
NextFsCompleteIo(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

PNEXTFS_VCB
NextFsVcb(PDEVICE_OBJECT DeviceObject)
{
    PNEXTFS_VCB Vcb = (PNEXTFS_VCB)DeviceObject->DeviceExtension;

    if (!Vcb || Vcb->Magic != NEXTFS_VCB_MAGIC)
        return NULL;
    return Vcb;
}

/* ------------------------------------------------------------------ */
/* UTF-16 <-> UTF-8                                                    */
/* ------------------------------------------------------------------ */

ULONG
NextFsUnitsToUtf8(PCWSTR Units, ULONG UnitCount, PCHAR Buffer, ULONG MaxBytes)
{
    ULONG i, Used = 0;

    if (!Buffer || MaxBytes == 0)
        return 0;

    for (i = 0; i < UnitCount; i++)
    {
        ULONG CodePoint = Units[i];
        CHAR Encoded[4];
        ULONG Count, k;

        if (CodePoint >= 0xD800 && CodePoint < 0xDC00 && i + 1 < UnitCount &&
            Units[i + 1] >= 0xDC00 && Units[i + 1] < 0xE000)
        {
            CodePoint = 0x10000 + ((CodePoint - 0xD800) << 10) + (Units[i + 1] - 0xDC00);
            i++;
        }
        else if (CodePoint >= 0xD800 && CodePoint < 0xE000)
        {
            CodePoint = '?';        /* unpaired surrogate: a lookup miss, not a crash */
        }

        if (CodePoint < 0x80)
        {
            Encoded[0] = (CHAR)CodePoint;
            Count = 1;
        }
        else if (CodePoint < 0x800)
        {
            Encoded[0] = (CHAR)(0xC0 | (CodePoint >> 6));
            Encoded[1] = (CHAR)(0x80 | (CodePoint & 0x3F));
            Count = 2;
        }
        else if (CodePoint < 0x10000)
        {
            Encoded[0] = (CHAR)(0xE0 | (CodePoint >> 12));
            Encoded[1] = (CHAR)(0x80 | ((CodePoint >> 6) & 0x3F));
            Encoded[2] = (CHAR)(0x80 | (CodePoint & 0x3F));
            Count = 3;
        }
        else
        {
            Encoded[0] = (CHAR)(0xF0 | (CodePoint >> 18));
            Encoded[1] = (CHAR)(0x80 | ((CodePoint >> 12) & 0x3F));
            Encoded[2] = (CHAR)(0x80 | ((CodePoint >> 6) & 0x3F));
            Encoded[3] = (CHAR)(0x80 | (CodePoint & 0x3F));
            Count = 4;
        }

        if (Used + Count + 1 > MaxBytes)
            return 0;
        for (k = 0; k < Count; k++)
            Buffer[Used + k] = Encoded[k];
        Used += Count;
    }

    Buffer[Used] = 0;
    return Used;
}

ULONG
NextFsUnicodeToUtf8(PCUNICODE_STRING String, PCHAR Buffer, ULONG MaxBytes)
{
    if (!String || !String->Buffer)
    {
        if (Buffer && MaxBytes)
            Buffer[0] = 0;
        return 0;
    }
    return NextFsUnitsToUtf8(String->Buffer, String->Length / sizeof(WCHAR),
                             Buffer, MaxBytes);
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

void
NextFsFreePath(PCHAR Path)
{
    if (Path)
        ExFreePoolWithTag(Path, NEXTFS_POOL_TAG);
}

static PCHAR
NextFsDuplicatePath(PCSTR Path)
{
    ULONG Length = 0;
    PCHAR Copy;

    while (Path[Length] != 0)
        Length++;

    Copy = ExAllocatePoolWithTag(NonPagedPool, Length + 1, NEXTFS_POOL_TAG);
    if (!Copy)
        return NULL;
    RtlCopyMemory(Copy, Path, Length + 1);
    return Copy;
}

/*
 * Build the UTF-8 path of "Name inside Related" (or of Related itself when Name
 * is NULL/empty). Related may be the volume root FILE_OBJECT, whose FsContext2 is
 * NULL on purpose: the root has no NEXTFS_FILE because nextfslib addresses it by
 * the constant NFS_ROOT_ID.
 */
PCHAR
NextFsBuildPath(PNEXTFS_VCB Vcb, PFILE_OBJECT Related, PUNICODE_STRING Name)
{
    PNEXTFS_FILE Parent = NULL;
    PCHAR Base = "\\";
    PCHAR NameUtf8 = NULL;
    ULONG NameLength = 0;
    ULONG BaseLength, Total;
    PCHAR Path;

    UNREFERENCED_PARAMETER(Vcb);

    if (Related && Related->FsContext2)
    {
        Parent = (PNEXTFS_FILE)Related->FsContext2;
        if (Parent->Magic != NEXTFS_FILE_MAGIC)
            return NULL;
        Base = Parent->Path;
    }

    if (Name && Name->Length != 0)
    {
        NameUtf8 = ExAllocatePoolWithTag(NonPagedPool, NEXTFS_PATH_MAX_BYTES, NEXTFS_POOL_TAG);
        if (!NameUtf8)
            return NULL;
        NameLength = NextFsUnicodeToUtf8(Name, NameUtf8, NEXTFS_PATH_MAX_BYTES);
        if (NameLength == 0)
        {
            ExFreePoolWithTag(NameUtf8, NEXTFS_POOL_TAG);
            return NULL;
        }
    }

    BaseLength = 0;
    while (Base[BaseLength] != 0)
        BaseLength++;

    Total = BaseLength + (NameLength ? NameLength + 1 : 0) + 1;
    Path = ExAllocatePoolWithTag(NonPagedPool, Total, NEXTFS_POOL_TAG);
    if (Path)
    {
        RtlCopyMemory(Path, Base, BaseLength);
        Path[BaseLength] = 0;
        if (NameLength)
        {
            /* "\\" + name, or "\\dir" + "\\" + name; never "\\\\name". */
            if (BaseLength == 1)
                BaseLength = 0;
            Path[BaseLength] = '\\';
            RtlCopyMemory(Path + BaseLength + 1, NameUtf8, NameLength + 1);
        }
    }

    if (NameUtf8)
        ExFreePoolWithTag(NameUtf8, NEXTFS_POOL_TAG);
    return Path;
}

NTSTATUS
NextFsSetFilePath(PNEXTFS_FILE File, PCHAR NewPath)
{
    PCHAR Copy = NextFsDuplicatePath(NewPath);

    if (!Copy)
        return STATUS_INSUFFICIENT_RESOURCES;
    NextFsFreePath(File->Path);
    File->Path = Copy;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

ULONG
NextFsTranslateDisposition(ULONG CreateDisposition)
{
    switch (CreateDisposition)
    {
        case FILE_SUPERSEDE:    return NFS_DISPOSITION_SUPERSEDE;
        case FILE_OPEN:         return NFS_DISPOSITION_OPEN;
        case FILE_CREATE:       return NFS_DISPOSITION_CREATE;
        case FILE_OPEN_IF:      return NFS_DISPOSITION_OPEN_IF;
        case FILE_OVERWRITE:    return NFS_DISPOSITION_OVERWRITE;
        case FILE_OVERWRITE_IF: return NFS_DISPOSITION_OVERWRITE_IF;
        default:                return 0xFFFFFFFF;
    }
}

NTSTATUS
NextFsOpenFile(PNEXTFS_VCB Vcb, PCHAR Path, ULONG CreateDisposition,
               BOOLEAN IsDirectory, nfs_u32 Access, PNEXTFS_FILE *OutFile)
{
    PNEXTFS_FILE File;
    nfs_u32 Handle = 0;
    int Status;

    *OutFile = NULL;

    Status = NfsHandleOpen(Vcb->Volume, Path, CreateDisposition,
                           IsDirectory ? 1 : 0, Access, &Handle);
    if (Status != NFS_OK)
    {
        DPRINT1("NextFS: open \"%s\" failed (%d)\n", Path, Status);
        return NextFsNfsStatus(Status);
    }

    File = ExAllocatePoolWithTag(NonPagedPool, sizeof(NEXTFS_FILE), NEXTFS_POOL_TAG);
    if (!File)
    {
        NfsHandleClose(Vcb->Volume, Handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(File, sizeof(*File));
    File->Magic = NEXTFS_FILE_MAGIC;
    {
        NFS_PIFT_RECORD Record;

        /* The file id never changes, not even across a rename: it is what the
           information classes hand out as the index number. */
        if (NfsHandleQuery(Vcb->Volume, Handle, &Record) == NFS_OK)
        {
            File->FileId = Record.FileId;
            if (Record.Flags & NFS_FLAG_DIR)
                File->Flags |= NEXTFS_FILE_DIRECTORY;
        }
    }
    File->Vcb = Vcb;
    File->Handle = Handle;
    File->Access = Access;
    if (IsDirectory)
        File->Flags |= NEXTFS_FILE_DIRECTORY;
    File->Path = NextFsDuplicatePath(Path);
    if (!File->Path)
    {
        NfsHandleClose(Vcb->Volume, Handle);
        ExFreePoolWithTag(File, NEXTFS_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        NTSTATUS LockStatus = NextFsInitializeLocking(File);

        if (!NT_SUCCESS(LockStatus))
        {
            NfsHandleClose(Vcb->Volume, Handle);
            NextFsFreePath(File->Path);
            ExFreePoolWithTag(File, NEXTFS_POOL_TAG);
            return LockStatus;
        }
    }

    *OutFile = File;
    return STATUS_SUCCESS;
}

static void
NextFsFreeFile(PNEXTFS_FILE File)
{
    if (!File)
        return;
    NextFsFreePath(File->Path);
    File->Magic = 0;
    ExFreePoolWithTag(File, NEXTFS_POOL_TAG);
}

/* ------------------------------------------------------------------ */
/* Block device callbacks over the volume device object                */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsReadWriteDisk(PNFS_NT_DEVICE Device, LARGE_INTEGER Offset, PVOID Buffer,
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
    return NT_SUCCESS(NextFsReadWriteDisk(Device, Position, Buffer, Length, FALSE)) ? 0 : -5;
}

static int
NextFsDevWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    LARGE_INTEGER Position;
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;

    Position.QuadPart = (LONGLONG)Offset;
    return NT_SUCCESS(NextFsReadWriteDisk(Device, Position, (PVOID)Buffer, Length, TRUE)) ? 0 : -5;
}

static int
NextFsDevSize(void *Context, nfs_u64 *Bytes)
{
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;

    *Bytes = (nfs_u64)Device->Size.QuadPart;
    return 0;
}

static int
NextFsDevFlush(void *Context)
{
    PNFS_NT_DEVICE Device = (PNFS_NT_DEVICE)Context;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS, Device->DeviceObject,
                                       NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
        return -5;
    if (IoCallDriver(Device->DeviceObject, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    return NT_SUCCESS(IoStatus.Status) ? 0 : -5;
}

/* ------------------------------------------------------------------ */
/* Volume geometry                                                     */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsQueryGeometry(PDEVICE_OBJECT VolumeDeviceObject, PNFS_NT_DEVICE NtDevice)
{
    DISK_GEOMETRY_EX Geometry;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                        VolumeDeviceObject, NULL, 0,
                                        &Geometry, sizeof(Geometry),
                                        FALSE, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(VolumeDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    NtDevice->Size.QuadPart = Geometry.DiskSize.QuadPart;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static VOID
NextFsDeleteVcb(PNEXTFS_VCB Vcb)
{
    if (Vcb->Volume)
    {
        NfsUnmount(Vcb->Volume);
        Vcb->Volume = NULL;
    }
    if (Vcb->TargetDeviceObject)
    {
        IoDetachDevice(Vcb->TargetDeviceObject);
        Vcb->TargetDeviceObject = NULL;
    }
    Vcb->Magic = 0;
    if (Vcb->VolumeDeviceObject)
    {
        PDEVICE_OBJECT Device = Vcb->VolumeDeviceObject;
        Vcb->VolumeDeviceObject = NULL;
        IoDeleteDevice(Device);
    }
}

static NTSTATUS
NextFsMountVolume(PDEVICE_OBJECT VolumeDeviceObject, PVPB Vpb,
                  PFILE_OBJECT VolumeFileObject)
{
    NTSTATUS Status;
    PNEXTFS_VCB Vcb;
    PDEVICE_OBJECT DeviceObject;
    PNFS_VOLUME Volume = NULL;
    NFS_VOLUME_INFO Info;
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
        Vcb->VolumeDeviceObject = NULL;
        Vcb->Magic = 0;
        IoDeleteDevice(DeviceObject);
        return STATUS_UNSUCCESSFUL;
    }

    ExInitializeResourceLite(&Vcb->Resource);

    Vcb->NtDevice.DeviceObject = Vcb->TargetDeviceObject;
    Vcb->NtDevice.FileObject = VolumeFileObject;
    Vcb->NtDevice.PartitionOffset.QuadPart = 0;
    Vcb->NtDevice.Size.QuadPart = 0;

    Status = NextFsQueryGeometry(Vcb->TargetDeviceObject, &Vcb->NtDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NextFS: cannot read the volume geometry (0x%x)\n", Status);
        ExDeleteResourceLite(&Vcb->Resource);
        IoDetachDevice(Vcb->TargetDeviceObject);
        Vcb->TargetDeviceObject = NULL;
        Vcb->Magic = 0;
        Vcb->VolumeDeviceObject = NULL;
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    Vcb->VolumeSize = Vcb->NtDevice.Size;
    Vcb->Block.Context = &Vcb->NtDevice;
    Vcb->Block.Read = NextFsDevRead;
    Vcb->Block.Write = NextFsDevWrite;
    Vcb->Block.Size = NextFsDevSize;
    Vcb->Block.Flush = NextFsDevFlush;

    LibraryStatus = NfsMount(&Vcb->Block, &Volume);
    if (LibraryStatus != 0)
    {
        DPRINT1("NextFS: NfsMount failed (%d): not an intact NextFS volume\n", LibraryStatus);
        ExDeleteResourceLite(&Vcb->Resource);
        IoDetachDevice(Vcb->TargetDeviceObject);
        Vcb->TargetDeviceObject = NULL;
        Vcb->Magic = 0;
        Vcb->VolumeDeviceObject = NULL;
        IoDeleteDevice(DeviceObject);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Vcb->Volume = Volume;
    Vcb->ClusterSize = NfsGetSuper(Volume)->ClusterSize;
    if (NfsQueryVolumeInfo(Volume, &Info) != NFS_OK)
    {
        NextFsDeleteVcb(Vcb);
        ExDeleteResourceLite(&Vcb->Resource);
        return STATUS_IO_DEVICE_ERROR;
    }

    DeviceObject->Flags |= DO_DIRECT_IO;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    Vpb->DeviceObject = DeviceObject;
    Vpb->RealDevice = VolumeDeviceObject;
    Vpb->SerialNumber = 0;

    DPRINT1("NextFS: mounted \"%s\", generation %lu, %lu files, %I64u bytes free\n",
            Info.Label, (ULONG)Info.Generation, Info.Files, Info.FreeBytes);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_CREATE                                                       */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI
NextFsCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    NTSTATUS Status;
    ULONG Disposition;
    BOOLEAN IsDirectory;
    nfs_u32 Access;
    PCHAR Path;
    PNEXTFS_FILE File;

    /* First touch of a volume: no VCB yet, so this is the mount, exactly like
       FatMountVolume. */
    if (!Vcb)
    {
        if (!FileObject || FileObject->FileName.Length != 0 || !FileObject->RelatedFileObject)
        {
            /* Nothing to mount from: this is a create against the FSD device
               itself (e.g. a handle to the driver), which is not a file. */
            return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }

        if (!FileObject->DeviceObject->Vpb)
            return NextFsCompleteIo(Irp, STATUS_UNRECOGNIZED_VOLUME, 0);

        Status = NextFsMountVolume(FileObject->DeviceObject,
                                   FileObject->DeviceObject->Vpb, FileObject);
        if (!NT_SUCCESS(Status))
            return NextFsCompleteIo(Irp, Status, 0);

        Vcb = NextFsVcb(DeviceObject);
        if (!Vcb)
            return NextFsCompleteIo(Irp, STATUS_UNEXPECTED_IO_ERROR, 0);

        /* The FILE_OBJECT that triggered the mount *is* the root directory. */
        FileObject->FsContext = Vcb;
        FileObject->FsContext2 = NULL;
        FileObject->Flags |= FO_CACHE_SUPPORTED;
        InterlockedIncrement(&Vcb->OpenCount);
        return NextFsCompleteIo(Irp, STATUS_SUCCESS, FILE_OPENED);
    }

    if (Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    /* The I/O manager folds the disposition into the top byte of Create.Options,
       which is how every NT FSD gets it (see fastfat/create.c). */
    Disposition = NextFsTranslateDisposition((IrpSp->Parameters.Create.Options >> 24) & 0xFF);
    if (Disposition == 0xFFFFFFFF)
        return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

    IsDirectory = (IrpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) != 0;
    Access = NFS_HANDLE_READ;
    if (!(IrpSp->Parameters.Create.SecurityContext &&
          (IrpSp->Parameters.Create.SecurityContext->DesiredAccess & FILE_READ_DATA) == 0))
        Access |= NFS_HANDLE_READ;
    if (IrpSp->Parameters.Create.SecurityContext &&
        (IrpSp->Parameters.Create.SecurityContext->DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)))
        Access |= NFS_HANDLE_WRITE;

    Path = NextFsBuildPath(Vcb, FileObject->RelatedFileObject, &FileObject->FileName);
    if (!Path)
        return NextFsCompleteIo(Irp, STATUS_OBJECT_NAME_INVALID, 0);

    Status = NextFsOpenFile(Vcb, Path, Disposition, IsDirectory, Access, &File);
    if (!NT_SUCCESS(Status))
    {
        NextFsFreePath(Path);
        return NextFsCompleteIo(Irp, Status, 0);
    }
    NextFsFreePath(Path);

    if (IrpSp->Parameters.Create.SecurityContext &&
        (IrpSp->Parameters.Create.SecurityContext->DesiredAccess & FILE_APPEND_DATA))
        File->Flags |= NEXTFS_FILE_APPEND;

    if (IrpSp->Parameters.Create.Options & FILE_DELETE_ON_CLOSE)
    {
        int NfsStatus = NfsHandleSetDeleteOnClose(Vcb->Volume, File->Handle, 1);

        if (NfsStatus != NFS_OK)
        {
            NfsHandleClose(Vcb->Volume, File->Handle);
            NextFsFreeFile(File);
            return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
        }
        File->Flags |= NEXTFS_FILE_DELETE_ON_CLOSE;
    }

    FileObject->Flags |= FO_CACHE_SUPPORTED;

    FileObject->FsContext = Vcb;
    FileObject->FsContext2 = File;
    InterlockedIncrement(&Vcb->OpenCount);

    /* NT wants FILE_CREATED when the object did not exist before this call. */
    return NextFsCompleteIo(Irp, STATUS_SUCCESS,
                            (Disposition == NFS_DISPOSITION_CREATE ||
                             Disposition == NFS_DISPOSITION_OPEN_IF ||
                             Disposition == NFS_DISPOSITION_OVERWRITE_IF) ?
                            FILE_CREATED : FILE_OPENED);
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_CLEANUP / IRP_MJ_CLOSE                                       */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI
NextFsCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File;

    UNREFERENCED_PARAMETER(IrpSp);

    if (Vcb)
    {
        File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
        if (File && File->Magic == NEXTFS_FILE_MAGIC)
        {
            /* The delete-on-close stored in the handle fires here, while the
               volume lock is held: one atomic PIFT commit, no window in which
               another create sees a half-deleted file. */
            ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
            if (NfsHandleClose(Vcb->Volume, File->Handle) != NFS_OK)
                DPRINT1("NextFS: cleanup could not close handle %lu\n", File->Handle);
            File->Handle = 0;
            ExReleaseResourceLite(&Vcb->Resource);
        }
    }

    return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS NTAPI
NextFsClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File;

    if (Vcb && FileObject)
    {
        File = (PNEXTFS_FILE)FileObject->FsContext2;
        if (File && File->Magic == NEXTFS_FILE_MAGIC)
        {
            /* Byte ranges and the oplock go with the last handle: nothing may
               outlive the FILE_OBJECT that owns it. */
            NextFsUninitializeLocking(File, FileObject);
            NextFsFreeFile(File);
        }
        else if (!File && FileObject->FsContext == Vcb &&
                 InterlockedDecrement(&Vcb->OpenCount) == 0)
        {
            /* Last handle on the volume root: tear the volume down. nextfslib
               checkpoints the PIFT here, which is the only moment the on-disk
               copy is written in a batch. */
            NextFsDeleteVcb(Vcb);
            ExDeleteResourceLite(&Vcb->Resource);
            goto Done;
        }
        if (File)
            InterlockedDecrement(&Vcb->OpenCount);
        FileObject->FsContext = NULL;
        FileObject->FsContext2 = NULL;
    }

Done:
    return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
}

/* ------------------------------------------------------------------ */
/* Anything not implemented is answered as such, never "success"        */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI
NextFsUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DPRINT1("NextFS: unsupported IRP major %u minor %u\n",
            IrpSp->MajorFunction, IrpSp->MinorFunction);
    return NextFsCompleteIo(Irp, STATUS_NOT_IMPLEMENTED, 0);
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

    /* nextfslib has no allocator of its own in a kernel build (there is no
       malloc), so it is handed the pool before anything can mount. */
    NfsSetAllocators(NextFsAlloc, NextFsFree);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = NextFsUnsupported;

    DriverObject->MajorFunction[IRP_MJ_CREATE]                 = NextFsCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]                = NextFsCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                  = NextFsClose;
    DriverObject->MajorFunction[IRP_MJ_READ]                   = NextFsRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]                  = NextFsWrite;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]          = NextFsFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]      = NextFsQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]        = NextFsSetInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_EA]               = NextFsQueryEa;
    DriverObject->MajorFunction[IRP_MJ_SET_EA]                 = NextFsSetEa;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL]      = NextFsDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = NextFsQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] = NextFsSetVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL]    = NextFsFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]               = NextFsShutdown;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]           = NextFsLockControl;

    /* Fast I/O answers the metadata calls without an IRP round trip; file data
       stays on the IRP path, the reason is at the top of nextfs_lock.c. */
    NextFsRegisterFastIo(DriverObject);

    DriverObject->DriverUnload = NULL;    /* a mounted FSD cannot be unloaded */

    DPRINT1("NextFS: driver loaded\n");
    return STATUS_SUCCESS;
}

/* EOF */
