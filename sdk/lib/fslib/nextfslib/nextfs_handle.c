/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_handle.c
 * PURPOSE:     File handles: the object the FSD keeps per FILE_OBJECT
 * LICENSE:     GPL-3.0-or-later
 *
 * An IRP handler cannot keep a pointer into the PIFT: a write-through commit can
 * move the record array. A handle is therefore an index into a per-volume table
 * of {fileid, position, access, flags}, which stays valid across commits and is
 * validated on every call.
 *
 * The handle table is also where the position lives, i.e. it owns the semantics
 * of the current file pointer (NfsHandleSeek, and read/write advance it), which
 * is what IRP_MJ_READ and IRP_MJ_WRITE use when the caller did not supply an
 * explicit offset.
 */

#include "nextfs_internal.h"

#include <string.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

int
NfsHandleTableInit(PNFS_VOLUME Volume)
{
    Volume->Handles = (PNFS_HANDLE)NfsMemAlloc(NFS_HANDLE_MAX * sizeof(NFS_HANDLE));
    if (!Volume->Handles)
        return NFS_ENOSPC;
    memset(Volume->Handles, 0, NFS_HANDLE_MAX * sizeof(NFS_HANDLE));
    return NFS_OK;
}

void
NfsHandleTableDestroy(PNFS_VOLUME Volume)
{
    NfsMemFree(Volume->Handles);
    Volume->Handles = NULL;
}

static PNFS_HANDLE
NfsHandleAt(PNFS_VOLUME Volume, nfs_u32 Handle)
{
    if (!Volume->Handles || Handle == 0 || Handle > NFS_HANDLE_MAX)
        return NULL;
    if (!Volume->Handles[Handle - 1].InUse)
        return NULL;
    return &Volume->Handles[Handle - 1];
}

int
NfsHandleOpen(PNFS_VOLUME Volume, const char *Path, nfs_u32 Disposition,
              int IsDirectory, nfs_u32 Access, nfs_u32 *OutHandle)
{
    nfs_fileid FileId = 0;
    int Created = 0;
    nfs_u32 Slot;
    int Status;

    if (!Volume || !OutHandle)
        return NFS_EINVAL;

    Status = NfsOpenPath(Volume, Path, Disposition, IsDirectory, &FileId, &Created);
    if (Status != NFS_OK)
        return Status;

    if (!Volume->Handles)
        return NFS_EINVAL;

    for (Slot = 0; Slot < NFS_HANDLE_MAX; Slot++)
    {
        PNFS_HANDLE Handle = &Volume->Handles[Slot];

        if (Handle->InUse)
            continue;

        memset(Handle, 0, sizeof(*Handle));
        Handle->Volume = Volume;
        Handle->FileId = FileId;
        Handle->Position = 0;
        Handle->InUse = 1;
        Handle->Flags = Access & (NFS_HANDLE_READ | NFS_HANDLE_WRITE);
        if (IsDirectory)
            Handle->Flags |= NFS_HANDLE_DIRECTORY;
        if (Created)
            Handle->Flags |= NFS_HANDLE_CREATED;

        *OutHandle = Slot + 1;
        return NFS_OK;
    }

    return NFS_ENOSPC;      /* handle table full */
}

int
NfsHandleRead(PNFS_VOLUME Volume, nfs_u32 Handle, void *Buffer, nfs_u32 Length,
              nfs_u32 *Read)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    nfs_u32 Done = 0;
    int Status;

    if (!Entry)
        return NFS_EINVAL;
    if (Entry->Flags & NFS_HANDLE_DIRECTORY)
        return NFS_EINVAL;
    if (Entry->Flags & NFS_HANDLE_WRITE && !(Entry->Flags & NFS_HANDLE_READ))
        return NFS_EINVAL;

    Status = NfsRead(Volume, Entry->FileId, Entry->Position, Buffer, Length, &Done);
    if (Status != NFS_OK)
        return Status;

    Entry->Position += Done;
    if (Read)
        *Read = Done;
    return NFS_OK;
}

int
NfsHandleWrite(PNFS_VOLUME Volume, nfs_u32 Handle, const void *Buffer, nfs_u32 Length,
               nfs_u32 *Written)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    nfs_u32 Done = 0;
    int Status;

    if (!Entry)
        return NFS_EINVAL;
    if (Entry->Flags & NFS_HANDLE_DIRECTORY)
        return NFS_EINVAL;
    if (!(Entry->Flags & NFS_HANDLE_WRITE))
        return NFS_EINVAL;

    Status = NfsWrite(Volume, Entry->FileId, Entry->Position, Buffer, Length, &Done);
    if (Status != NFS_OK)
        return Status;

    Entry->Position += Done;
    if (Written)
        *Written = Done;
    return NFS_OK;
}

int
NfsHandleSeek(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u64 Offset)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    NFS_PIFT_RECORD *Record;

    if (!Entry)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, Entry->FileId);
    if (!Record)
        return NFS_ENOENT;
    if (Offset > Record->Size)
        Offset = Record->Size;      /* seeking past EOF clamps, like NT */

    Entry->Position = Offset;
    return NFS_OK;
}

int
NfsHandleQuery(PNFS_VOLUME Volume, nfs_u32 Handle, NFS_PIFT_RECORD *OutRecord)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    NFS_PIFT_RECORD *Record;

    if (!Entry)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, Entry->FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    if (OutRecord)
        *OutRecord = *Record;
    return NFS_OK;
}

int
NfsHandleReadDir(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u32 Index,
                 NFS_PIFT_RECORD *OutRecord)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);

    if (!Entry)
        return NFS_EINVAL;
    if (!(Entry->Flags & NFS_HANDLE_DIRECTORY))
        return NFS_EINVAL;

    return NfsReadDir(Volume, Entry->FileId, Index, OutRecord);
}

int
NfsHandleSetEndOfFile(PNFS_VOLUME Volume, nfs_u32 Handle, nfs_u64 Size)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    int Status;

    if (!Entry || (Entry->Flags & NFS_HANDLE_DIRECTORY))
        return NFS_EINVAL;
    if (!(Entry->Flags & NFS_HANDLE_WRITE))
        return NFS_EINVAL;

    Status = NfsTruncate(Volume, Entry->FileId, Size);
    if (Status != NFS_OK)
        return Status;

    if (Entry->Position > Size)
        Entry->Position = Size;
    return NFS_OK;
}

int
NfsHandleSetDeleteOnClose(PNFS_VOLUME Volume, nfs_u32 Handle, int DeleteOnClose)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);

    if (!Entry)
        return NFS_EINVAL;

    if (DeleteOnClose)
        Entry->Flags |= NFS_HANDLE_DELETE_ON_CLOSE;
    else
        Entry->Flags &= ~NFS_HANDLE_DELETE_ON_CLOSE;
    return NFS_OK;
}

int
NfsHandleClose(PNFS_VOLUME Volume, nfs_u32 Handle)
{
    PNFS_HANDLE Entry = NfsHandleAt(Volume, Handle);
    nfs_fileid FileId;
    int Delete = 0;

    if (!Entry)
        return NFS_EINVAL;

    FileId = Entry->FileId;
    Delete = (Entry->Flags & NFS_HANDLE_DELETE_ON_CLOSE) != 0;
    memset(Entry, 0, sizeof(*Entry));

    if (!Delete || FileId == NFS_ROOT_ID)
        return NFS_OK;

    /* Delete-on-close: the file goes with the last handle. A directory must be
       empty first, exactly like a real delete. */
    {
        NFS_PIFT_RECORD *Record = NfsIndexGet(Volume, FileId);

        if (!Record || (Record->Flags & NFS_FLAG_DELETED))
            return NFS_OK;
        if ((Record->Flags & NFS_FLAG_DIR) && !NfsDirectoryIsEmpty(Volume, FileId))
            return NFS_EINVAL;
    }

    return NfsRemove(Volume, FileId);
}
