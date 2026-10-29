/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_finfo.c
 * PURPOSE:     NextFS IRP_MJ_QUERY_INFORMATION / IRP_MJ_SET_INFORMATION
 * LICENSE:     GPL-3.0-or-later
 *
 * All the metadata a class asks for comes from one call, NfsQueryStandardInfo();
 * that keeps "creation time from FileBasicInformation" and "creation time from
 * FileAllInformation" from ever disagreeing.
 */

#include <ntifs.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

/* NextFS stores Unix seconds; NT wants 100 ns ticks since 1601. */
#define NEXTFS_EPOCH_DELTA 116444736000000000LL

LARGE_INTEGER
NextFsUnixToNtTime(nfs_u64 Seconds)
{
    LARGE_INTEGER Value;

    Value.QuadPart = (LONGLONG)Seconds * 10000000LL + NEXTFS_EPOCH_DELTA;
    return Value;
}

static nfs_u64
NextFsTimeToUnix(PLARGE_INTEGER Time)
{
    LONGLONG Ticks;

    if (!Time)
        return 0;
    Ticks = Time->QuadPart - NEXTFS_EPOCH_DELTA;
    if (Ticks <= 0)
        return 0;
    return (nfs_u64)(Ticks / 10000000LL);
}

ULONG
NextFsNtAttributes(const NFS_STANDARD_INFORMATION *Info)
{
    ULONG Attributes = FILE_ATTRIBUTE_NORMAL;

    if (Info->Attributes & 0x01)     /* read-only, mirrors NFS_ATTR_READONLY */
        Attributes = FILE_ATTRIBUTE_READONLY;
    if (Info->Directory)
        Attributes = FILE_ATTRIBUTE_DIRECTORY;

    /* Hidden and system live in the same word nextfslib stores. */
    if (Info->Attributes & 0x02)
        Attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (Info->Attributes & 0x04)
        Attributes |= FILE_ATTRIBUTE_SYSTEM;
    if (Info->Attributes & 0x20)
        Attributes |= FILE_ATTRIBUTE_ARCHIVE;

    return Attributes;
}

static NTSTATUS
NextFsQueryInfo(PNEXTFS_VCB Vcb, PNEXTFS_FILE File, nfs_u32 FileId,
                NFS_STANDARD_INFORMATION *Info)
{
    int NfsStatus;

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsQueryStandardInfo(Vcb->Volume, FileId, Info);
    ExReleaseResourceLite(&Vcb->Resource);

    if (NfsStatus != NFS_OK)
        return NextFsNfsStatus(NfsStatus);
    if (File && File->Flags & NEXTFS_FILE_DELETE_ON_CLOSE)
        Info->DeletePending = 1;
    return STATUS_SUCCESS;
}

/* Byte length of the stored UTF-8 path, for FileNameInformation. */
static ULONG
NextFsPathLength(PNEXTFS_FILE File)
{
    ULONG Length = 0;

    if (!File || !File->Path)
        return 0;
    while (File->Path[Length] != 0)
        Length++;
    return Length;
}

NTSTATUS NTAPI
NextFsQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    FILE_INFORMATION_CLASS InfoClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryFile.Length;
    NFS_STANDARD_INFORMATION Info;
    NTSTATUS Status;
    nfs_u32 FileId;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    if (File && File->Magic == NEXTFS_FILE_MAGIC)
        FileId = File->FileId;
    else
        FileId = NFS_ROOT_ID;         /* a handle to the volume root */

    if (InfoClass == FileNameInformation)
    {
        PFILE_NAME_INFORMATION NameInfo = (PFILE_NAME_INFORMATION)Buffer;
        ULONG Bytes, Needed;

        Bytes = NextFsPathLength(File);
        Needed = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + Bytes;
        if (!Buffer || Length < FIELD_OFFSET(FILE_NAME_INFORMATION, FileName))
            return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);
        if (Length < Needed)
            return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW,
                                    FIELD_OFFSET(FILE_NAME_INFORMATION, FileName));
        NameInfo->FileNameLength = Bytes;
        RtlCopyMemory(NameInfo->FileName, File->Path, Bytes);
        return NextFsCompleteIo(Irp, STATUS_SUCCESS, Needed);
    }

    Status = NextFsQueryInfo(Vcb, File, FileId, &Info);
    if (!NT_SUCCESS(Status))
        return NextFsCompleteIo(Irp, Status, 0);

    switch (InfoClass)
    {
        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION Basic = (PFILE_BASIC_INFORMATION)Buffer;

            if (Length < sizeof(FILE_BASIC_INFORMATION) || !Basic)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_BASIC_INFORMATION));

            Basic->CreationTime = NextFsUnixToNtTime(Info.CreationTime);
            Basic->LastAccessTime = NextFsUnixToNtTime(Info.LastAccessTime);
            Basic->LastWriteTime = NextFsUnixToNtTime(Info.LastWriteTime);
            Basic->ChangeTime = NextFsUnixToNtTime(Info.ChangeTime);
            Basic->FileAttributes = (ULONG)NextFsNtAttributes(&Info);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_BASIC_INFORMATION));
        }

        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION Standard = (PFILE_STANDARD_INFORMATION)Buffer;

            if (Length < sizeof(FILE_STANDARD_INFORMATION) || !Standard)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_STANDARD_INFORMATION));

            Standard->AllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
            Standard->EndOfFile.QuadPart = (LONGLONG)Info.EndOfFile;
            Standard->NumberOfLinks = Info.NumberOfLinks ? Info.NumberOfLinks : 1;
            Standard->DeletePending = (BOOLEAN)Info.DeletePending;
            Standard->Directory = (BOOLEAN)Info.Directory;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_STANDARD_INFORMATION));
        }

        case FileInternalInformation:
        {
            PFILE_INTERNAL_INFORMATION Internal = (PFILE_INTERNAL_INFORMATION)Buffer;

            if (Length < sizeof(FILE_INTERNAL_INFORMATION) || !Internal)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_INTERNAL_INFORMATION));

            /* The PIFT file id *is* the index number: it never changes for a file,
               not even across a rename, so it is a stable file id for callers. */
            Internal->IndexNumber.QuadPart = (LONGLONG)FileId;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_INTERNAL_INFORMATION));
        }

        case FileEaInformation:
        {
            PFILE_EA_INFORMATION Ea = (PFILE_EA_INFORMATION)Buffer;

            if (Length < sizeof(FILE_EA_INFORMATION) || !Ea)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_EA_INFORMATION));
            Ea->EaSize = 0;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_EA_INFORMATION));
        }

        case FileAccessInformation:
        {
            PFILE_ACCESS_INFORMATION Access = (PFILE_ACCESS_INFORMATION)Buffer;

            if (Length < sizeof(FILE_ACCESS_INFORMATION) || !Access)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_ACCESS_INFORMATION));
            Access->AccessFlags = (File && (File->Access & NFS_HANDLE_WRITE)) ?
                                  (FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                                   FILE_WRITE_ATTRIBUTES | SYNCHRONIZE) : SYNCHRONIZE;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_ACCESS_INFORMATION));
        }

        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION Position = (PFILE_POSITION_INFORMATION)Buffer;

            if (Length < sizeof(FILE_POSITION_INFORMATION) || !Position)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_POSITION_INFORMATION));
            Position->CurrentByteOffset = IrpSp->FileObject->CurrentByteOffset;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_POSITION_INFORMATION));
        }

        case FileModeInformation:
        {
            PFILE_MODE_INFORMATION Mode = (PFILE_MODE_INFORMATION)Buffer;

            if (Length < sizeof(FILE_MODE_INFORMATION) || !Mode)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_MODE_INFORMATION));
            Mode->Mode = IrpSp->FileObject->Flags & (FO_WRITE_THROUGH | FO_SEQUENTIAL_ONLY);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_MODE_INFORMATION));
        }

        case FileAlignmentInformation:
        {
            PFILE_ALIGNMENT_INFORMATION Alignment = (PFILE_ALIGNMENT_INFORMATION)Buffer;

            if (Length < sizeof(FILE_ALIGNMENT_INFORMATION) || !Alignment)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_ALIGNMENT_INFORMATION));
            Alignment->AlignmentRequirement = 0;   /* byte aligned, NextFS has no sector rule */
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_ALIGNMENT_INFORMATION));
        }

        case FileNetworkOpenInformation:
        {
            PFILE_NETWORK_OPEN_INFORMATION Network = (PFILE_NETWORK_OPEN_INFORMATION)Buffer;

            if (Length < sizeof(FILE_NETWORK_OPEN_INFORMATION) || !Network)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_NETWORK_OPEN_INFORMATION));

            Network->CreationTime = NextFsUnixToNtTime(Info.CreationTime);
            Network->LastAccessTime = NextFsUnixToNtTime(Info.LastAccessTime);
            Network->LastWriteTime = NextFsUnixToNtTime(Info.LastWriteTime);
            Network->ChangeTime = NextFsUnixToNtTime(Info.ChangeTime);
            Network->AllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
            Network->EndOfFile.QuadPart = (LONGLONG)Info.EndOfFile;
            Network->FileAttributes = NextFsNtAttributes(&Info);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_NETWORK_OPEN_INFORMATION));
        }

        case FileAttributeTagInformation:
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION Tag = (PFILE_ATTRIBUTE_TAG_INFORMATION)Buffer;

            if (Length < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION) || !Tag)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, sizeof(FILE_ATTRIBUTE_TAG_INFORMATION));
            Tag->FileAttributes = NextFsNtAttributes(&Info);
            Tag->ReparseTag = 0;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, sizeof(FILE_ATTRIBUTE_TAG_INFORMATION));
        }

        case FileStreamInformation:
        {
            PFILE_STREAM_INFORMATION Stream = (PFILE_STREAM_INFORMATION)Buffer;
            ULONG Needed = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) +
                           sizeof(WCHAR);        /* empty name, "::$DATA" is implied */

            if (Length < Needed || !Stream)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);

            RtlZeroMemory(Stream, Needed);
            Stream->NextEntryOffset = 0;
            Stream->StreamNameLength = 0;
            Stream->StreamSize.QuadPart = (LONGLONG)Info.EndOfFile;
            Stream->StreamAllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, Needed);
        }

        case FileAllInformation:
        {
            PFILE_ALL_INFORMATION All = (PFILE_ALL_INFORMATION)Buffer;
            ULONG NameBytes = NextFsPathLength(File);
            ULONG Needed = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName) + NameBytes;

            if (!Buffer || Length < FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName))
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, Needed);

            All->BasicInformation.CreationTime = NextFsUnixToNtTime(Info.CreationTime);
            All->BasicInformation.LastAccessTime = NextFsUnixToNtTime(Info.LastAccessTime);
            All->BasicInformation.LastWriteTime = NextFsUnixToNtTime(Info.LastWriteTime);
            All->BasicInformation.ChangeTime = NextFsUnixToNtTime(Info.ChangeTime);
            All->BasicInformation.FileAttributes = NextFsNtAttributes(&Info);
            All->StandardInformation.AllocationSize.QuadPart = (LONGLONG)Info.AllocationSize;
            All->StandardInformation.EndOfFile.QuadPart = (LONGLONG)Info.EndOfFile;
            All->StandardInformation.NumberOfLinks = Info.NumberOfLinks ? Info.NumberOfLinks : 1;
            All->StandardInformation.DeletePending = (BOOLEAN)Info.DeletePending;
            All->StandardInformation.Directory = (BOOLEAN)Info.Directory;
            All->InternalInformation.IndexNumber.QuadPart = (LONGLONG)FileId;
            All->EaInformation.EaSize = 0;
            All->AccessInformation.AccessFlags = SYNCHRONIZE |
                ((File && (File->Access & NFS_HANDLE_WRITE)) ? (FILE_READ_DATA | FILE_WRITE_DATA) : FILE_READ_DATA);
            All->PositionInformation.CurrentByteOffset = IrpSp->FileObject->CurrentByteOffset;
            All->ModeInformation.Mode = IrpSp->FileObject->Flags & FO_WRITE_THROUGH;
            All->AlignmentInformation.AlignmentRequirement = 0;

            if (Length < Needed)
                return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW,
                                        FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName));
            All->NameInformation.FileNameLength = NameBytes;
            RtlCopyMemory(All->NameInformation.FileName, File ? File->Path : "\\", NameBytes);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, Needed);
        }

        case FileAlternateNameInformation:
            /* No 8.3 names: NextFS keeps one name per file, and inventing a
               second one is exactly the kind of duplicated state it removes. */
            return NextFsCompleteIo(Irp, STATUS_OBJECT_NAME_NOT_FOUND, 0);

        default:
            DPRINT1("NextFS: unsupported query information class %u\n", InfoClass);
            return NextFsCompleteIo(Irp, STATUS_NOT_IMPLEMENTED, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Set information                                                     */
/* ------------------------------------------------------------------ */

static NTSTATUS
NextFsSetBasicInfo(PNEXTFS_VCB Vcb, nfs_u32 FileId, PFILE_BASIC_INFORMATION Basic)
{
    NFS_FILE_INFO Info;
    int NfsStatus;

    RtlZeroMemory(&Info, sizeof(Info));

    if (Basic->CreationTime.QuadPart)
    {
        Info.SetMask |= NFS_SET_BTIME;
        Info.Btime = NextFsTimeToUnix(&Basic->CreationTime);
    }
    if (Basic->LastAccessTime.QuadPart)
    {
        Info.SetMask |= NFS_SET_ATIME;
        Info.Atime = NextFsTimeToUnix(&Basic->LastAccessTime);
    }
    if (Basic->LastWriteTime.QuadPart)
    {
        Info.SetMask |= NFS_SET_MTIME;
        Info.Mtime = NextFsTimeToUnix(&Basic->LastWriteTime);
    }
    if (Basic->ChangeTime.QuadPart)
    {
        Info.SetMask |= NFS_SET_CTIME;
        Info.Ctime = NextFsTimeToUnix(&Basic->ChangeTime);
    }
    if (Basic->FileAttributes != 0xFFFFFFFF)
    {
        ULONG Attributes = 0;

        if (Basic->FileAttributes & FILE_ATTRIBUTE_READONLY)
            Attributes |= 0x01;
        if (Basic->FileAttributes & FILE_ATTRIBUTE_HIDDEN)
            Attributes |= 0x02;
        if (Basic->FileAttributes & FILE_ATTRIBUTE_SYSTEM)
            Attributes |= 0x04;
        if (Basic->FileAttributes & FILE_ATTRIBUTE_ARCHIVE)
            Attributes |= 0x20;
        Info.SetMask |= NFS_SET_ATTRIBUTES;
        Info.Attributes = Attributes;
    }

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsSetFileInfo(Vcb->Volume, FileId, &Info);
    ExReleaseResourceLite(&Vcb->Resource);

    return NextFsNfsStatus(NfsStatus);
}

/* Split "\dir\sub\name" into the parent path and the leaf, for rename. */
static NTSTATUS
NextFsSplitPath(PCHAR Path, PCHAR ParentBuffer, ULONG ParentBytes,
                PCSTR *Leaf)
{
    ULONG Length = 0, Last = 0, i;

    while (Path[Length] != 0)
        Length++;
    for (i = 0; i < Length; i++)
    {
        if (Path[i] == '\\' || Path[i] == '/')
            Last = i;
    }

    if (Last == 0)
    {
        if (ParentBytes < 2)
            return STATUS_BUFFER_TOO_SMALL;
        ParentBuffer[0] = '\\';
        ParentBuffer[1] = 0;
    }
    else
    {
        if (Last + 1 > ParentBytes)
            return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(ParentBuffer, Path, Last);
        ParentBuffer[Last] = 0;
    }

    *Leaf = Path + Last + ((Last == 0) ? 1 : 1);
    return (*Leaf)[0] ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID;
}

static NTSTATUS
NextFsRenameFile(PNEXTFS_VCB Vcb, PNEXTFS_FILE File, PCHAR NewPath, BOOLEAN Replace)
{
    CHAR ParentPath[NEXTFS_PATH_MAX_BYTES];
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 NameLength;
    nfs_fileid ParentId = NFS_ROOT_ID;
    PCSTR Leaf;
    NTSTATUS Status;
    int NfsStatus;

    Status = NextFsSplitPath(NewPath, ParentPath, sizeof(ParentPath), &Leaf);
    if (!NT_SUCCESS(Status))
        return Status;

    NameLength = NfsNameFromAscii(Leaf, Name, NFS_NAME_MAX);
    if (NameLength == 0)
        return STATUS_OBJECT_NAME_INVALID;

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    if (ParentPath[0] != 0 && !(ParentPath[0] == '\\' && ParentPath[1] == 0))
    {
        nfs_u32 Handle = 0;

        NfsStatus = NfsHandleOpen(Vcb->Volume, ParentPath, NFS_DISPOSITION_OPEN, 1,
                                  NFS_HANDLE_READ, &Handle);
        if (NfsStatus != NFS_OK)
        {
            ExReleaseResourceLite(&Vcb->Resource);
            return NextFsNfsStatus(NfsStatus);
        }
        {
            NFS_PIFT_RECORD ParentRecord;

            NfsStatus = NfsHandleQuery(Vcb->Volume, Handle, &ParentRecord);
            if (NfsStatus == NFS_OK)
                ParentId = ParentRecord.FileId;
            NfsHandleClose(Vcb->Volume, Handle);
        }
        if (NfsStatus != NFS_OK)
        {
            ExReleaseResourceLite(&Vcb->Resource);
            return NextFsNfsStatus(NfsStatus);
        }
    }

    NfsStatus = NfsRename(Vcb->Volume, File->FileId, ParentId, Name, NameLength,
                          Replace ? 1 : 0);
    ExReleaseResourceLite(&Vcb->Resource);

    if (NfsStatus != NFS_OK)
        return NextFsNfsStatus(NfsStatus);

    NextFsSetFilePath(File, NewPath);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
NextFsSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    FILE_INFORMATION_CLASS InfoClass = IrpSp->Parameters.SetFile.FileInformationClass;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.SetFile.Length;
    int NfsStatus = NFS_OK;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (!File || File->Magic != NEXTFS_FILE_MAGIC || File->Handle == 0)
        return NextFsCompleteIo(Irp, STATUS_INVALID_HANDLE, 0);

    /* Truncating, renaming or marking for delete all change what another handle
       sees, so an oplock has to break first. */
    {
        NTSTATUS OplockStatus = NextFsCheckOplock(File, Irp);

        if (OplockStatus == STATUS_PENDING)
            return STATUS_PENDING;
        if (!NT_SUCCESS(OplockStatus))
            return NextFsCompleteIo(Irp, OplockStatus, 0);
    }

    switch (InfoClass)
    {
        case FileBasicInformation:
        {
            NTSTATUS Status;

            if (!Buffer || Length < sizeof(FILE_BASIC_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);
            Status = NextFsSetBasicInfo(Vcb, File->FileId, (PFILE_BASIC_INFORMATION)Buffer);
            return NextFsCompleteIo(Irp, Status, 0);
        }

        case FileEndOfFileInformation:
        {
            PFILE_END_OF_FILE_INFORMATION EndOfFile = (PFILE_END_OF_FILE_INFORMATION)Buffer;

            if (!Buffer || Length < sizeof(FILE_END_OF_FILE_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);
            if (!(File->Access & NFS_HANDLE_WRITE))
                return NextFsCompleteIo(Irp, STATUS_ACCESS_DENIED, 0);
            if (EndOfFile->EndOfFile.QuadPart < 0)
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

            ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
            NfsStatus = NfsHandleSetEndOfFile(Vcb->Volume, File->Handle,
                                              (nfs_u64)EndOfFile->EndOfFile.QuadPart);
            ExReleaseResourceLite(&Vcb->Resource);
            return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
        }

        case FileAllocationInformation:
        {
            /* NextFS allocates exactly what the data needs and a write past EOF
               leaves a hole, so "reserve this much" is already true. */
            PFILE_ALLOCATION_INFORMATION Allocation = (PFILE_ALLOCATION_INFORMATION)Buffer;

            if (!Buffer || Length < sizeof(FILE_ALLOCATION_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);
            if (Allocation->AllocationSize.QuadPart < 0)
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
        }

        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION Position = (PFILE_POSITION_INFORMATION)Buffer;

            if (!Buffer || Length < sizeof(FILE_POSITION_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);
            if (Position->CurrentByteOffset.QuadPart < 0)
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

            IrpSp->FileObject->CurrentByteOffset = Position->CurrentByteOffset;
            ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
            NfsStatus = NfsHandleSeek(Vcb->Volume, File->Handle,
                                      (nfs_u64)Position->CurrentByteOffset.QuadPart);
            ExReleaseResourceLite(&Vcb->Resource);
            return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
        }

        case FileDispositionInformation:
        {
            PFILE_DISPOSITION_INFORMATION Disposition = (PFILE_DISPOSITION_INFORMATION)Buffer;

            if (!Buffer || Length < sizeof(FILE_DISPOSITION_INFORMATION))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

            ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
            NfsStatus = NfsHandleSetDeleteOnClose(Vcb->Volume, File->Handle,
                                                  Disposition->DeleteFile ? 1 : 0);
            ExReleaseResourceLite(&Vcb->Resource);
            if (NfsStatus != NFS_OK)
                return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);

            if (Disposition->DeleteFile)
                File->Flags |= NEXTFS_FILE_DELETE_ON_CLOSE;
            else
                File->Flags &= ~NEXTFS_FILE_DELETE_ON_CLOSE;
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
        }

        case FileRenameInformation:
        {
            PFILE_RENAME_INFORMATION Rename = (PFILE_RENAME_INFORMATION)Buffer;
            UNICODE_STRING NewName;
            PCHAR Target = NULL;
            NTSTATUS Status;

            if (!Buffer || Length < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
                return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

            /* The new name is either relative to RootDirectory or absolute. */
            NewName.Length = (USHORT)Rename->FileNameLength;
            NewName.MaximumLength = (USHORT)Rename->FileNameLength;
            NewName.Buffer = Rename->FileName;
            Target = NextFsBuildPath(Vcb, Rename->RootDirectory, &NewName);
            if (!Target)
                return NextFsCompleteIo(Irp, STATUS_OBJECT_NAME_INVALID, 0);

            Status = NextFsRenameFile(Vcb, File, Target, Rename->ReplaceIfExists != 0);
            NextFsFreePath(Target);
            return NextFsCompleteIo(Irp, Status, 0);
        }

        case FileLinkInformation:
            /* No hard links: one name per file is part of what makes the PIFT a
               table and not a graph. */
            return NextFsCompleteIo(Irp, STATUS_NOT_SUPPORTED, 0);

        case FileValidDataLengthInformation:
            /* Nothing is ever read back as stale data: a hole reads as zeros. */
            return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);

        default:
            DPRINT1("NextFS: unsupported set information class %u\n", InfoClass);
            return NextFsCompleteIo(Irp, STATUS_NOT_IMPLEMENTED, 0);
    }
}

NTSTATUS NTAPI
NextFsQueryEa(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return NextFsCompleteIo(Irp, STATUS_NO_EAS_ON_FILE, 0);
}

NTSTATUS NTAPI
NextFsSetEa(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return NextFsCompleteIo(Irp, STATUS_EAS_NOT_SUPPORTED, 0);
}

/* EOF */
