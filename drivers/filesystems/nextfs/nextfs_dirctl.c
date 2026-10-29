/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_dirctl.c
 * PURPOSE:     NextFS IRP_MJ_DIRECTORY_CONTROL (query directory)
 * LICENSE:     GPL-3.0-or-later
 *
 * This is where the old NTFS design hurts most: to list a directory NTFS walks a
 * B+ tree of $INDEX_ALLOCATION blocks scattered over the disk. Here the directory
 * is a slice of the RAM PIFT, NfsEnumerate() hands out one record per call, and
 * the wildcard filter is applied before the record is copied out -- so a listing
 * never touches the disk at all once the volume is mounted.
 *
 * The restart index the caller passes (FileIndex) is honoured by replaying the
 * filtered enumeration from the start: it is a RAM walk of a few hundred records,
 * and it keeps the contract exact instead of approximating it with a saved cursor
 * that a concurrent create would invalidate.
 */

#include <ntifs.h>

#include "nextfs.h"

#define NDEBUG
#include <debug.h>

#define NEXTFS_EPOCH_DELTA 116444736000000000LL

static LARGE_INTEGER
NextFsDirTime(nfs_u64 Seconds)
{
    LARGE_INTEGER Value;

    Value.QuadPart = (LONGLONG)Seconds * 10000000LL + NEXTFS_EPOCH_DELTA;
    return Value;
}

static ULONG
NextFsDirAttributes(const NFS_PIFT_RECORD *Record)
{
    if (Record->Flags & NFS_FLAG_DIR)
        return FILE_ATTRIBUTE_DIRECTORY;
    if (Record->Attributes & 0x01)
        return FILE_ATTRIBUTE_READONLY;
    return FILE_ATTRIBUTE_ARCHIVE;
}

/* Bytes needed for one entry of the requested class, not counting the name. */
static ULONG
NextFsDirEntryHeader(FILE_INFORMATION_CLASS InfoClass)
{
    switch (InfoClass)
    {
        case FileNamesInformation:            return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        case FileDirectoryInformation:        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        case FileFullDirectoryInformation:    return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        case FileBothDirectoryInformation:    return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        case FileIdFullDirectoryInformation:  return FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName);
        case FileIdBothDirectoryInformation:  return FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName);
        default:                              return 0;
    }
}

/* Copy one PIFT record into the caller's buffer as the requested class.
   Returns the aligned entry size, or 0 if the class is not supported. */
static ULONG
NextFsWriteDirEntry(FILE_INFORMATION_CLASS InfoClass, PVOID Buffer,
                    const NFS_PIFT_RECORD *Record, ULONG Index)
{
    ULONG NameBytes = Record->NameLen * sizeof(WCHAR);
    ULONG Header = NextFsDirEntryHeader(InfoClass);
    ULONG Size = Header + NameBytes;
    ULONG Aligned = (Size + 7) & ~7u;

    if (InfoClass == FileNamesInformation)
    {
        PFILE_NAMES_INFORMATION Entry = (PFILE_NAMES_INFORMATION)Buffer;

        Entry->NextEntryOffset = Aligned;
        Entry->FileNameLength = NameBytes;
        RtlCopyMemory(Entry->FileName, Record->Name, NameBytes);
        return Aligned;
    }

    if (InfoClass == FileDirectoryInformation)
    {
        PFILE_DIRECTORY_INFORMATION Entry = (PFILE_DIRECTORY_INFORMATION)Buffer;

        Entry->NextEntryOffset = Aligned;
        Entry->FileIndex = Index;
        Entry->CreationTime = NextFsDirTime(Record->Btime);
        Entry->LastAccessTime = NextFsDirTime(Record->Atime);
        Entry->LastWriteTime = NextFsDirTime(Record->Mtime);
        Entry->ChangeTime = NextFsDirTime(Record->Ctime);
        Entry->EndOfFile.QuadPart = (LONGLONG)Record->Size;
        Entry->AllocationSize.QuadPart = (LONGLONG)Record->AllocSize;
        Entry->FileAttributes = NextFsDirAttributes(Record);
        Entry->FileNameLength = NameBytes;
        RtlCopyMemory(Entry->FileName, Record->Name, NameBytes);
        return Aligned;
    }

    if (InfoClass == FileFullDirectoryInformation || InfoClass == FileBothDirectoryInformation)
    {
        PFILE_BOTH_DIR_INFORMATION Entry = (PFILE_BOTH_DIR_INFORMATION)Buffer;

        Entry->NextEntryOffset = Aligned;
        Entry->FileIndex = Index;
        Entry->CreationTime = NextFsDirTime(Record->Btime);
        Entry->LastAccessTime = NextFsDirTime(Record->Atime);
        Entry->LastWriteTime = NextFsDirTime(Record->Mtime);
        Entry->ChangeTime = NextFsDirTime(Record->Ctime);
        Entry->EndOfFile.QuadPart = (LONGLONG)Record->Size;
        Entry->AllocationSize.QuadPart = (LONGLONG)Record->AllocSize;
        Entry->FileAttributes = NextFsDirAttributes(Record);
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->ShortNameLength = 0;         /* no 8.3 names in NextFS */
        RtlZeroMemory(Entry->ShortName, sizeof(Entry->ShortName));
        RtlCopyMemory(Entry->FileName, Record->Name, NameBytes);
        return Aligned;
    }

    if (InfoClass == FileIdBothDirectoryInformation)
    {
        PFILE_ID_BOTH_DIR_INFORMATION Entry = (PFILE_ID_BOTH_DIR_INFORMATION)Buffer;

        Entry->NextEntryOffset = Aligned;
        Entry->FileIndex = Index;
        Entry->CreationTime = NextFsDirTime(Record->Btime);
        Entry->LastAccessTime = NextFsDirTime(Record->Atime);
        Entry->LastWriteTime = NextFsDirTime(Record->Mtime);
        Entry->ChangeTime = NextFsDirTime(Record->Ctime);
        Entry->EndOfFile.QuadPart = (LONGLONG)Record->Size;
        Entry->AllocationSize.QuadPart = (LONGLONG)Record->AllocSize;
        Entry->FileAttributes = NextFsDirAttributes(Record);
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->ShortNameLength = 0;
        RtlZeroMemory(Entry->ShortName, sizeof(Entry->ShortName));
        Entry->FileId.QuadPart = (LONGLONG)Record->FileId;
        RtlCopyMemory(Entry->FileName, Record->Name, NameBytes);
        return Aligned;
    }

    if (InfoClass == FileIdFullDirectoryInformation)
    {
        PFILE_ID_FULL_DIR_INFORMATION Entry = (PFILE_ID_FULL_DIR_INFORMATION)Buffer;

        Entry->NextEntryOffset = Aligned;
        Entry->FileIndex = Index;
        Entry->CreationTime = NextFsDirTime(Record->Btime);
        Entry->LastAccessTime = NextFsDirTime(Record->Atime);
        Entry->LastWriteTime = NextFsDirTime(Record->Mtime);
        Entry->ChangeTime = NextFsDirTime(Record->Ctime);
        Entry->EndOfFile.QuadPart = (LONGLONG)Record->Size;
        Entry->AllocationSize.QuadPart = (LONGLONG)Record->AllocSize;
        Entry->FileAttributes = NextFsDirAttributes(Record);
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->FileId.QuadPart = (LONGLONG)Record->FileId;
        RtlCopyMemory(Entry->FileName, Record->Name, NameBytes);
        return Aligned;
    }

    return 0;
}

NTSTATUS NTAPI
NextFsDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    if (IrpSp->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY)
    {
        /* No change notification yet: the honest answer is "not supported", not a
           blocked IRP that never completes. Explorer falls back to polling. */
        return NextFsCompleteIo(Irp, STATUS_NOT_SUPPORTED, 0);
    }

    if (IrpSp->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    if (!File || File->Magic != NEXTFS_FILE_MAGIC || !(File->Flags & NEXTFS_FILE_DIRECTORY))
        return NextFsCompleteIo(Irp, STATUS_INVALID_PARAMETER, 0);

    {
        FILE_INFORMATION_CLASS InfoClass = IrpSp->Parameters.QueryDirectory.FileInformationClass;
        ULONG BufferLength = IrpSp->Parameters.QueryDirectory.Length;
        PVOID Buffer = Irp->UserBuffer ? Irp->UserBuffer : Irp->AssociatedIrp.SystemBuffer;
        ULONG RestartIndex = IrpSp->Parameters.QueryDirectory.FileIndex;
        ULONG Header = NextFsDirEntryHeader(InfoClass);
        CHAR Pattern[NEXTFS_PATH_MAX_BYTES];
        nfs_u32 Cookie = 0;
        ULONG Index = 0;
        ULONG Used = 0;
        ULONG Entries = 0;

        if (Header == 0)
        {
            DPRINT1("NextFS: unsupported directory class %u\n", InfoClass);
            return NextFsCompleteIo(Irp, STATUS_INVALID_INFO_CLASS, 0);
        }
        if (!Buffer || BufferLength < Header + sizeof(WCHAR))
            return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, 0);

        if (!IrpSp->Parameters.QueryDirectory.FileName ||
            IrpSp->Parameters.QueryDirectory.FileName->Length == 0)
        {
            Pattern[0] = '*';
            Pattern[1] = 0;
        }
        else if (NextFsUnicodeToUtf8(IrpSp->Parameters.QueryDirectory.FileName,
                                     Pattern, sizeof(Pattern)) == 0)
        {
            return NextFsCompleteIo(Irp, STATUS_OBJECT_NAME_INVALID, 0);
        }

        /* Restart: NT re-issues the query with FileIndex 0 and the same pattern.
           A non-zero index means "the app asked for that entry" (rare, mostly
           backup tools), and the loop below honours it by skipping in RAM. */

        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        for (;;)
        {
            NFS_PIFT_RECORD Record;
            int NfsStatus;

            NfsStatus = NfsEnumerate(Vcb->Volume, File->FileId, &Cookie, Pattern, &Record);
            if (NfsStatus != NFS_OK)
            {
                if (Entries == 0)
                {
                    ExReleaseResourceLite(&Vcb->Resource);
                    return NextFsCompleteIo(Irp,
                                            (NfsStatus == NFS_ENOENT) ? STATUS_NO_MORE_FILES
                                                                      : NextFsNfsStatus(NfsStatus),
                                            0);
                }
                break;
            }

            if (Index++ < RestartIndex)
                continue;               /* the caller asked to resume past these */

            {
                ULONG EntrySize = Header + Record.NameLen * sizeof(WCHAR);
                ULONG Aligned = (EntrySize + 7) & ~7u;
                ULONG Written;

                if (Used + Aligned > BufferLength)
                {
                    if (Entries == 0)
                    {
                        ExReleaseResourceLite(&Vcb->Resource);
                        return NextFsCompleteIo(Irp, STATUS_BUFFER_OVERFLOW, 0);
                    }
                    break;              /* the buffer is full: this is the batch */
                }

                Written = NextFsWriteDirEntry(InfoClass, (PUCHAR)Buffer + Used, &Record,
                                              Index - 1);
                Used += Written;
                Entries++;

                /* NextEntryOffset of the last entry must be 0, and it is patched
                   at the end of the batch. */
                if (Used + (Header + sizeof(WCHAR)) > BufferLength)
                    break;
            }
        }

        ExReleaseResourceLite(&Vcb->Resource);

        /* Terminate the chain: clear the NextEntryOffset of the last entry. */
        {
            PUCHAR Last = (PUCHAR)Buffer;
            ULONG Offset = 0;

            while (Offset + NextFsDirEntryHeader(InfoClass) <= Used)
            {
                ULONG Next = *(PULONG)(Last + Offset);

                if (Next == 0 || Offset + Next >= Used)
                {
                    *(PULONG)(Last + Offset) = 0;
                    break;
                }
                Offset += Next;
            }
        }

        Status = (Entries > 0) ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
        return NextFsCompleteIo(Irp, Status, Used);
    }
}

/* EOF */
