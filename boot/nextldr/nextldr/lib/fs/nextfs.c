/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/lib/fs/nextfs.c
 * PURPOSE:     ARC file-system interface for NextFS (read-only)
 * LICENSE:     GPL-3.0-or-later
 *
 * The whole of this file is translation: path in, handle out; handle in, bytes
 * out. The format itself belongs to nextfs_core.c, which knows nothing about ARC
 * beyond a read callback and two allocator functions -- that is what lets the
 * same code be checked on the host against an image nextformat wrote.
 *
 * There is no write path, no Create, no Rename: the loader mounts, reads, and
 * that is the entire contract. Everything here returns EACCES for anything that
 * would modify the volume, so a mistake upstairs (a component opening the boot
 * volume for write, say) fails instead of corrupting it.
 */

#include <nextldr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(DISK);

#include <fs/nextfs.h>

#define TAG_NEXTFS_VOLUME 'VftF'
#define TAG_NEXTFS_FILE   'FftF'

/* One mounted volume per device, exactly like the other file systems. */
static PNFSB_VOLUME NextFsVolumes[MAX_FDS];

/* Partial-sector reads go through this: ARC wants sector granularity. */
static UCHAR NextFsBounce[SECTOR_SIZE];

typedef struct _NEXTFS_FILE_HANDLE
{
    PNFSB_VOLUME Volume;
    const NFSB_RECORD *Record;
    ULONGLONG Offset;
    ULONG FileNameLength;
    UCHAR Attributes;
    CHAR FileName[RTL_FIELD_SIZE(FILEINFORMATION, FileName)];
} NEXTFS_FILE_HANDLE, *PNEXTFS_FILE_HANDLE;

/* ------------------------------------------------------------------ */
/* Glue callbacks                                                      */
/* ------------------------------------------------------------------ */

/*
 * Byte-exact read out of an ARC device. ARC reads whole sectors, so a request
 * that does not start or end on a boundary is served by bouncing through one
 * sector: first the head, then the aligned middle, then the tail. A short read
 * is an error here, never a silent truncation.
 */
static int
NextFsDeviceRead(void *Context, NFSB_U64 Offset, void *Buffer, NFSB_U32 Length)
{
    ULONG DeviceId = (ULONG)(ULONG_PTR)Context;
    LARGE_INTEGER Position;
    UCHAR *Out = (UCHAR *)Buffer;
    ARC_STATUS Status;
    ULONG Count, Chunk, InSector;

    while (Length > 0)
    {
        InSector = (ULONG)(Offset % SECTOR_SIZE);
        Chunk = SECTOR_SIZE - InSector;
        if (Chunk > Length)
            Chunk = Length;

        if (InSector == 0 && Chunk == SECTOR_SIZE)
        {
            Position.QuadPart = (LONGLONG)Offset;
            Status = ArcSeek(DeviceId, &Position, SeekAbsolute);
            if (Status != ESUCCESS)
                return NFSB_EIO;
            Status = ArcRead(DeviceId, Out, SECTOR_SIZE, &Count);
            if (Status != ESUCCESS || Count != SECTOR_SIZE)
                return NFSB_EIO;
        }
        else
        {
            Position.QuadPart = (LONGLONG)(Offset - InSector);
            Status = ArcSeek(DeviceId, &Position, SeekAbsolute);
            if (Status != ESUCCESS)
                return NFSB_EIO;
            Status = ArcRead(DeviceId, NextFsBounce, SECTOR_SIZE, &Count);
            if (Status != ESUCCESS || Count != SECTOR_SIZE)
                return NFSB_EIO;
            RtlCopyMemory(Out, NextFsBounce + InSector, Chunk);
        }

        Out += Chunk;
        Offset += Chunk;
        Length -= Chunk;
    }

    return NFSB_OK;
}

static void *
NextFsHeapAlloc(NFSB_U32 Size)
{
    return FrLdrTempAlloc(Size, TAG_NEXTFS_VOLUME);
}

static void
NextFsHeapFree(void *Block)
{
    FrLdrTempFree(Block, TAG_NEXTFS_VOLUME);
}

/* ------------------------------------------------------------------ */
/* ARC entry points                                                    */
/* ------------------------------------------------------------------ */

ARC_STATUS
NextFsClose(ULONG FileId)
{
    PNEXTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    if (FileHandle)
        FrLdrTempFree(FileHandle, TAG_NEXTFS_FILE);
    return ESUCCESS;
}

ARC_STATUS
NextFsGetFileInformation(ULONG FileId, FILEINFORMATION* Information)
{
    PNEXTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    RtlZeroMemory(Information, sizeof(*Information));
    Information->EndingAddress.QuadPart = FileHandle->Record->Size;
    Information->CurrentAddress.QuadPart = FileHandle->Offset;
    Information->Attributes = FileHandle->Attributes;

    Information->FileNameLength =
        min(FileHandle->FileNameLength, sizeof(Information->FileName) - 1);
    RtlCopyMemory(Information->FileName, FileHandle->FileName,
                  Information->FileNameLength);
    Information->FileName[Information->FileNameLength] = ANSI_NULL;

    return ESUCCESS;
}

ARC_STATUS
NextFsOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
    PNEXTFS_FILE_HANDLE FileHandle;
    PNFSB_VOLUME Volume;
    const NFSB_RECORD *Record;
    ULONG DeviceId, Length;

    if (OpenMode != OpenReadOnly)
        return EACCES;

    DeviceId = FsGetDeviceId(*FileId);
    Volume = (DeviceId < MAX_FDS) ? NextFsVolumes[DeviceId] : NULL;
    if (!Volume)
        return ENOENT;

    Record = NFSB_LookupPath(Volume, Path);
    if (!Record)
        return ENOENT;

    FileHandle = FrLdrTempAlloc(sizeof(NEXTFS_FILE_HANDLE), TAG_NEXTFS_FILE);
    if (!FileHandle)
        return ENOMEM;
    RtlZeroMemory(FileHandle, sizeof(NEXTFS_FILE_HANDLE));

    FileHandle->Volume = Volume;
    FileHandle->Record = Record;
    FileHandle->Offset = 0;

    /* ARC file attributes carry the same bits as the NextFS record. */
    FileHandle->Attributes = (UCHAR)(Record->Attributes &
                                     (0x01 | 0x02 | 0x04 | 0x10 | 0x20));

    /* The name ARC reports back is the last component the caller asked for. */
    for (Length = 0; Path[Length] != ANSI_NULL; Length++)
        ;
    while (Length > 0 && Path[Length - 1] != '\\' && Path[Length - 1] != '/')
        Length--;
    {
        ULONG i = 0;

        while (Path[Length + i] != ANSI_NULL &&
               i < RTL_FIELD_SIZE(FILEINFORMATION, FileName) - 1)
        {
            FileHandle->FileName[i] = Path[Length + i];
            i++;
        }
        FileHandle->FileNameLength = i;
    }

    FsSetDeviceSpecific(*FileId, FileHandle);
    return ESUCCESS;
}

ARC_STATUS
NextFsRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count)
{
    PNEXTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);
    NFSB_U32 Read = 0;
    int Status;

    *Count = 0;
    if (!FileHandle || !N)
        return EINVAL;

    Status = NFSB_Read(FileHandle->Volume, FileHandle->Record, FileHandle->Offset,
                       Buffer, N, &Read);
    if (Status != NFSB_OK)
        return EIO;

    FileHandle->Offset += Read;
    *Count = Read;
    return ESUCCESS;
}

ARC_STATUS
NextFsSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode)
{
    PNEXTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);
    LARGE_INTEGER NewPosition = *Position;

    switch (SeekMode)
    {
        case SeekAbsolute:
            break;
        case SeekRelative:
            NewPosition.QuadPart += (LONGLONG)FileHandle->Offset;
            break;
        default:
            return EINVAL;
    }

    if (NewPosition.QuadPart < 0 ||
        (ULONGLONG)NewPosition.QuadPart > FileHandle->Record->Size)
        return EINVAL;

    FileHandle->Offset = (ULONGLONG)NewPosition.QuadPart;
    return ESUCCESS;
}

ULONGLONG
NextFsGetVolumeSize(ULONG DeviceId)
{
    PNFSB_VOLUME Volume;

    if (DeviceId >= MAX_FDS)
        return 0;
    Volume = NextFsVolumes[DeviceId];
    if (!Volume)
        return 0;
    return Volume->Super.TotalClusters * (NFSB_U64)Volume->Super.ClusterSize;
}

const DEVVTBL NextFsFuncTable =
{
    NextFsClose,
    NextFsGetFileInformation,
    NextFsOpen,
    NextFsRead,
    NextFsSeek,
    (PCWSTR)L"nextfs",
};

const DEVVTBL*
NextFsMount(ULONG DeviceId)
{
    PNFSB_VOLUME Volume = NULL;
    NFSB_IO Io;
    NFSB_HEAP Heap;
    ULONGLONG DeviceBytes = 0;
    int Status;

    if (DeviceId >= MAX_FDS)
        return NULL;

    /* A previous mount on the same device (a retry after a failure) is dropped. */
    if (NextFsVolumes[DeviceId])
    {
        NFSB_Unmount(NextFsVolumes[DeviceId]);
        NextFsVolumes[DeviceId] = NULL;
    }

    FsGetVolumeSize(DeviceId, &DeviceBytes);

    Io.Read = NextFsDeviceRead;
    Io.Context = (void *)(ULONG_PTR)DeviceId;
    Io.DeviceBytes = (NFSB_U64)DeviceBytes;

    Heap.Alloc = NextFsHeapAlloc;
    Heap.Free = NextFsHeapFree;

    Status = NFSB_Mount(&Io, &Heap, &Volume);
    if (Status != NFSB_OK)
    {
        /* Silent until now: a mount that fails here leaves the loader
           trying every other file system and then saying nothing. */
        TRACE("NextFsMount: device %lu is not a NextFS volume (status %ld, %lu bytes)\n",
              DeviceId, (long)Status, (unsigned long)DeviceBytes);
        return NULL;
    }

    if (Volume->DirtyAtMount)
        FileSystemError("NextFS volume was not shut down cleanly: booting from its last checkpoint.");
    if (Volume->UsedFallbackRoot)
        FileSystemError("NextFS: newest checkpoint did not verify, using the previous generation.");

    NextFsVolumes[DeviceId] = Volume;
    return &NextFsFuncTable;
}

/* EOF */
