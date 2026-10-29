/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_volinfo.c
 * PURPOSE:     NextFS volume information and file system control
 * LICENSE:     GPL-3.0-or-later
 */

#include <ntifs.h>
#include <ntdddisk.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

#define NEXTFS_EPOCH_DELTA 116444736000000000LL

static ULONG
NextFsLabelUnits(const NFS_VOLUME_INFO *Info, WCHAR *Out, ULONG MaxUnits)
{
    ULONG Units = 0;

    while (Units < MaxUnits && Units < NFS_LABEL_MAX && Info->Label[Units] != 0)
    {
        Out[Units] = (WCHAR)(UCHAR)Info->Label[Units];
        Units++;
    }
    return Units;
}

NTSTATUS NTAPI
NextFsQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    FS_INFORMATION_CLASS InfoClass = IrpSp->Parameters.QueryVolume.FsInformationClass;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    NFS_VOLUME_INFO Info;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsQueryVolumeInfo(Vcb->Volume, &Info);
    ExReleaseResourceLite(&Vcb->Resource);

    switch (InfoClass)
    {
        case FileFsVolumeInformation:
        {
            PFILE_FS_VOLUME_INFORMATION VolumeInfo = (PFILE_FS_VOLUME_INFORMATION)Buffer;
            ULONG Needed = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) +
                           NFS_LABEL_MAX * sizeof(WCHAR);

            if (!Buffer || Length < FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel))
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);

            VolumeInfo->VolumeCreationTime.QuadPart = 0;
            VolumeInfo->VolumeSerialNumber = 0x4E465300;   /* "NFS" */
            VolumeInfo->SupportsObjects = FALSE;
            VolumeInfo->VolumeLabelLength = (USHORT)(NextFsLabelUnits(&Info, VolumeInfo->VolumeLabel,
                                                                      NFS_LABEL_MAX) * sizeof(WCHAR));
            return NextFsCompleteIo(Irp, STATUS_SUCCESS,
                                    FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) +
                                    VolumeInfo->VolumeLabelLength);
        }

        case FileFsAttributeInformation:
        {
            static const WCHAR FsName[] = { 'N', 'e', 'x', 't', 'F', 'S' };
            PFILE_FS_ATTRIBUTE_INFORMATION Attribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
            ULONG NameBytes = (sizeof(FsName) - sizeof(WCHAR));
            ULONG Needed = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + NameBytes;

            if (!Buffer || Length < FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName))
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);

            Attribute->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES |
                                              FILE_UNICODE_ON_DISK |
                                              FILE_SUPPORTS_SPARSE_FILES;
            Attribute->MaximumComponentNameLength = NFS_NAME_MAX;
            Attribute->FileSystemNameLength = NameBytes;
            if (Length < Needed)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW,
                                        FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName));
            RtlCopyMemory(Attribute->FileSystemName, FsName, NameBytes);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, Needed);
        }

        case FileFsSizeInformation:
        case FileFsFullSizeInformation:
        {
            ULONG Needed = (InfoClass == FileFsFullSizeInformation) ?
                           sizeof(FILE_FS_FULL_SIZE_INFORMATION) : sizeof(FILE_FS_SIZE_INFORMATION);
            ULONG UnitsPerCluster = Info.ClusterSize / 512;

            if (!Buffer || Length < Needed)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);

            if (InfoClass == FileFsFullSizeInformation)
            {
                PFILE_FS_FULL_SIZE_INFORMATION Full = (PFILE_FS_FULL_SIZE_INFORMATION)Buffer;

                Full->TotalAllocationUnits.QuadPart = (LONGLONG)Info.TotalClusters;
                Full->CallerAvailableAllocationUnits.QuadPart = (LONGLONG)Info.FreeClusters;
                Full->ActualAvailableAllocationUnits.QuadPart = (LONGLONG)Info.FreeClusters;
                Full->SectorsPerAllocationUnit = UnitsPerCluster;
                Full->BytesPerSector = 512;
            }
            else
            {
                PFILE_FS_SIZE_INFORMATION Size = (PFILE_FS_SIZE_INFORMATION)Buffer;

                Size->TotalAllocationUnits.QuadPart = (LONGLONG)Info.TotalClusters;
                Size->AvailableAllocationUnits.QuadPart = (LONGLONG)Info.FreeClusters;
                Size->SectorsPerAllocationUnit = UnitsPerCluster;
                Size->BytesPerSector = 512;
            }
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, Needed);
        }

        case FileFsDeviceInformation:
        {
            PFILE_FS_DEVICE_INFORMATION Device = (PFILE_FS_DEVICE_INFORMATION)Buffer;

            if (!Buffer || Length < sizeof(FILE_FS_DEVICE_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_FS_DEVICE_INFORMATION));

            Device->DeviceType = FILE_DEVICE_DISK;
            Device->Characteristics = FILE_DEVICE_IS_MOUNTED;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_FS_DEVICE_INFORMATION));
        }

        case FileFsObjectIdInformation:
            return NextFsCompleteIo(Irp, STATUS_NOT_SUPPORTED, 0);

        case FileFsControlInformation:
        case FileFsLabelInformation:
        {
            /* Quota control information and volume labels in the NT sense do not
               exist here: NextFS has a label in the superblock, which is set with
               nextformat, not by a running system. */
            return NextFsCompleteIo(Irp, STATUS_NOT_SUPPORTED, 0);
        }

        default:
            DPRINT1("NextFS: unsupported volume class %u\n", InfoClass);
            return NextFsCompleteIo(Irp, STATUS_NOT_IMPLEMENTED, 0);
    }
}

NTSTATUS NTAPI
NextFsSetVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return NextFsCompleteIo(Irp, STATUS_NOT_SUPPORTED, 0);
}

/* ------------------------------------------------------------------ */
/* FSCTL                                                               */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsGetRetrievalPointers(PNEXTFS_VCB Vcb, PNEXTFS_FILE File, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PSTARTING_VCN_INPUT_BUFFER Input = (PSTARTING_VCN_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    PRETRIEVAL_POINTERS_BUFFER Output = (PRETRIEVAL_POINTERS_BUFFER)Irp->UserBuffer;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    NFS_PIFT_RECORD Record;
    ULONG i, Count = 0, Needed;
    int NfsStatus;

    if (!Input || IrpSp->Parameters.FileSystemControl.InputBufferLength < sizeof(STARTING_VCN_INPUT_BUFFER))
        return STATUS_INVALID_PARAMETER;
    if (!Output)
        return STATUS_INVALID_PARAMETER;
    if (!File || File->Magic != NEXTFS_FILE_MAGIC)
        return STATUS_INVALID_HANDLE;

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsHandleQuery(Vcb->Volume, File->Handle, &Record);
    ExReleaseResourceLite(&Vcb->Resource);
    if (NfsStatus != NFS_OK)
        return NextFsNfsStatus(NfsStatus);

    /* The extents are already in the PIFT record: the answer is a copy, not a
       disk walk. That is the whole point of the Pre-Indexed File Table. */
    for (i = 0; i < Record.ExtentCount && i < NFS_EXTENT_MAX; i++)
    {
        if (Record.Extents[i].Lcn == NFS_HOLE)
            continue;
        Count++;
    }

    Needed = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents) +
             Count * sizeof(((PRETRIEVAL_POINTERS_BUFFER)0)->Extents[0]);
    if (OutputLength < Needed)
    {
        Output->ExtentCount = Count;
        return STATUS_BUFFER_OVERFLOW;
    }

    Output->ExtentCount = Count;
    Output->StartingVcn.QuadPart = (LONGLONG)Input->StartingVcn.QuadPart;

    Count = 0;
    for (i = 0; i < Record.ExtentCount && i < NFS_EXTENT_MAX; i++)
    {
        if (Record.Extents[i].Lcn == NFS_HOLE)
            continue;
        Output->Extents[Count].NextVcn.QuadPart =
            (LONGLONG)(i + 1 < Record.ExtentCount ? Record.Extents[i + 1].Vcn
                                                  : Record.Size / Vcb->ClusterSize + 1);
        Output->Extents[Count].Lcn.QuadPart = (LONGLONG)Record.Extents[i].Lcn;
        Count++;
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
NextFsFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    ULONG ControlCode = IrpSp->Parameters.FileSystemControl.FsControlCode;
    NTSTATUS Status;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    switch (ControlCode)
    {
        /* Oplocks: acquiring one, and acknowledging a break, are FSCTLs. FsRtl
           owns the state machine, including the break timeouts. */
        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:
        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        {
            ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;

            Status = NextFsOplockControl(File, Irp, ControlCode, OutputLength);
            if (Status == STATUS_PENDING)
                return STATUS_PENDING;   /* FsRtl completes it when the break lands */
            return NextFsCompleteIo(Irp, Status, 0);
        }

        case FSCTL_IS_VOLUME_DIRTY:
        {
            PULONG Dirty = (PULONG)Irp->AssociatedIrp.SystemBuffer;
            NFS_VOLUME_INFO Info;

            if (!Dirty || IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
                return NextFsCompleteIo(Irp, STATUS_BUFFER_TOO_SMALL, 0);

            ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
            NfsQueryVolumeInfo(Vcb->Volume, &Info);
            ExReleaseResourceLite(&Vcb->Resource);

            /* A mounted NextFS volume is "volatile" by definition: the PIFT is
               the truth in RAM and the on-disk copy is only read at boot. */
            *Dirty = (Info.Flags & NFS_SB_DIRTY) ? 1 : 0;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }

        case FSCTL_MARK_VOLUME_DIRTY:
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);

        case FSCTL_LOCK_VOLUME:
        case FSCTL_UNLOCK_VOLUME:
        case FSCTL_DISMOUNT_VOLUME:
        case FSCTL_ALLOW_EXTENDED_DASD_IO:
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);

        case FSCTL_SET_SPARSE:
            /* Every file is sparse already: a write past EOF leaves a hole that
               reads back as zeros and costs nothing. */
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);

        case FSCTL_GET_RETRIEVAL_POINTERS:
            Status = NextFsGetRetrievalPointers(Vcb, File, Irp);
            return NextFsCompleteIo(Irp, Status, NT_SUCCESS(Status) ?
                                    IrpSp->Parameters.FileSystemControl.OutputBufferLength : 0);

        case FSCTL_GET_NTFS_VOLUME_DATA:
        case FSCTL_GET_NTFS_FILE_RECORD:
            /* Deliberately absent: there is no MFT to describe. */
            return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

        default:
            DPRINT1("NextFS: unsupported FSCTL 0x%08x\n", ControlCode);
            return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* EOF */
