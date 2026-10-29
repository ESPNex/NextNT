/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_namespace.c
 * PURPOSE:     Namespace operations the FSD needs: rename/move, file information,
 *              restoring a file from a snapshot
 * LICENSE:     GPL-3.0-or-later
 *
 * These are the operations that make NextFS usable as a system volume. They are
 * written against the same transaction machinery as every other change: one PIFT
 * record commit is the whole operation, which is why a rename with replacement
 * has no delete window (NTFS needs a transaction or a rollback file for that).
 */

#include "nextfs_internal.h"

#include <string.h>

#define NFS_REVERT_CHUNK (64 * 1024)

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

/* True when Candidate is FileId itself or lives below it. */
static int
NfsIsSelfOrDescendant(PNFS_VOLUME Volume, nfs_fileid Candidate, nfs_fileid FileId)
{
    nfs_u32 Guard = 0;

    while (Candidate != 0 && Guard++ < 4096)
    {
        NFS_PIFT_RECORD *Record;

        if (Candidate == FileId)
            return 1;

        Record = NfsIndexGet(Volume, Candidate);
        if (!Record)
            return 0;
        Candidate = Record->Parent;
    }
    return 0;
}

int
NfsRename(PNFS_VOLUME Volume, nfs_fileid FileId, nfs_fileid NewParent,
          const nfs_u16 *NewName, nfs_u32 NameLength, int ReplaceExisting)
{
    NFS_PIFT_RECORD *Source, Updated;
    NFS_PIFT_RECORD *Target, Replaced;
    NFS_PIFT_RECORD *ParentRecord;
    nfs_u32 i;
    int Status;

    if (!Volume || !NewName || NameLength == 0 || NameLength > NFS_NAME_MAX)
        return NFS_EINVAL;
    if (FileId == NFS_ROOT_ID || NewParent == 0)
        return NFS_EINVAL;

    Source = NfsIndexGet(Volume, FileId);
    if (!Source || (Source->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Source->Flags & NFS_FLAG_SNAPSHOT)
        return NFS_EINVAL;                 /* snapshots are not files */

    ParentRecord = NfsIndexGet(Volume, NewParent);
    if (!ParentRecord || (ParentRecord->Flags & NFS_FLAG_DELETED) ||
        !(ParentRecord->Flags & NFS_FLAG_DIR))
        return NFS_ENOENT;

    /* Moving a directory inside itself would detach the subtree. */
    if (NfsIsSelfOrDescendant(Volume, NewParent, FileId))
        return NFS_EINVAL;

    Target = NfsChildrenLookup(Volume, NewParent, NewName, NameLength);
    if (Target && Target->FileId != FileId)
    {
        if (!ReplaceExisting)
            return NFS_EEXIST;
        if (Target->Flags & NFS_FLAG_SNAPSHOT)
            return NFS_EINVAL;
        if ((Target->Flags & NFS_FLAG_DIR) &&
            NfsChildrenAt(Volume, Target->FileId, 0) != NULL)
            return NFS_EINVAL;             /* not an empty directory */

        /* Delete the target in the same transaction as the rename. */
        Replaced = *Target;
        Replaced.Flags |= NFS_FLAG_DELETED;
        Replaced.Generation++;

        Updated = *Source;
        Updated.Parent = NewParent;
        Updated.NameLen = NameLength;
        memset(Updated.Name, 0, sizeof(Updated.Name));
        memcpy(Updated.Name, NewName, (size_t)NameLength * sizeof(nfs_u16));
        Updated.Generation++;

        {
            NFS_PIFT_RECORD Batch[2];

            Batch[0] = Replaced;
            Batch[1] = Updated;
            Status = NfsCommitRecords(Volume, Batch, 2);
        }
        if (Status != NFS_OK)
            return Status;

        for (i = 0; i < Target->ExtentCount; i++)
        {
            if (Target->Extents[i].Lcn != NFS_HOLE)
                NfsClusterFree(Volume, Target->Extents[i].Lcn,
                               Target->Extents[i].Length);
        }
        if (Volume->Stats.Files > 0)
            Volume->Stats.Files--;
        return NFS_OK;
    }

    if (Target && Target->FileId == FileId &&
        Source->Parent == NewParent && Source->NameLen == NameLength)
    {
        nfs_u32 i2;

        for (i2 = 0; i2 < NameLength; i2++)
        {
            nfs_u16 A = Source->Name[i2], B = NewName[i2];

            if (A >= 'A' && A <= 'Z')
                A = (nfs_u16)(A - 'A' + 'a');
            if (B >= 'A' && B <= 'Z')
                B = (nfs_u16)(B - 'A' + 'a');
            if (A != B)
                break;
        }
        if (i2 == NameLength)
            return NFS_OK;                 /* same name, nothing to do */
    }

    Updated = *Source;
    Updated.Parent = NewParent;
    Updated.NameLen = NameLength;
    memset(Updated.Name, 0, sizeof(Updated.Name));
    memcpy(Updated.Name, NewName, (size_t)NameLength * sizeof(nfs_u16));
    Updated.Generation++;

    return NfsCommitRecords(Volume, &Updated, 1);
}

int
NfsSetFileInfo(PNFS_VOLUME Volume, nfs_fileid FileId, const NFS_FILE_INFO *Info)
{
    NFS_PIFT_RECORD *Record, Updated;

    if (!Volume || !Info)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    Updated = *Record;
    if (Info->SetMask & NFS_SET_ATTRIBUTES)
        Updated.Attributes = Info->Attributes;
    if (Info->SetMask & NFS_SET_CTIME)
        Updated.Ctime = Info->Ctime;
    if (Info->SetMask & NFS_SET_MTIME)
        Updated.Mtime = Info->Mtime;
    if (Info->SetMask & NFS_SET_ATIME)
        Updated.Atime = Info->Atime;
    if (Info->SetMask & NFS_SET_BTIME)
        Updated.Btime = Info->Btime;
    Updated.Generation++;

    return NfsCommitRecords(Volume, &Updated, 1);
}

int
NfsRevertFromSnapshot(PNFS_VOLUME Volume, nfs_fileid SnapshotId, nfs_fileid FileId,
                      nfs_fileid *OutFileId)
{
    NFS_PIFT_RECORD Snapshot;
    NFS_PIFT_RECORD *Live;
    nfs_u8 *Buffer;
    nfs_u64 Offset = 0;
    nfs_fileid Target = FileId;
    int Status;

    if (!Volume)
        return NFS_EINVAL;

    Status = NfsSnapshotOpenById(Volume, SnapshotId, FileId, &Snapshot);
    if (Status != NFS_OK)
        return Status;
    if (Snapshot.Flags & NFS_FLAG_DIR)
        return NFS_EUNSUPPORTED;           /* directories come later */

    Live = NfsIndexGet(Volume, FileId);
    if (!Live || (Live->Flags & NFS_FLAG_DELETED))
    {
        /* The file was deleted: bring it back where it was. */
        NFS_PIFT_RECORD *ParentRecord = NfsIndexGet(Volume, Snapshot.Parent);

        if (!ParentRecord || !(ParentRecord->Flags & NFS_FLAG_DIR))
            return NFS_ENOENT;
        if (NfsChildrenLookup(Volume, Snapshot.Parent, Snapshot.Name,
                             Snapshot.NameLen))
            return NFS_EEXIST;             /* the name is taken by another file */

        Status = NfsCreate(Volume, Snapshot.Parent, Snapshot.Name, Snapshot.NameLen, 0,
                           &Target);
        if (Status != NFS_OK)
            return Status;
    }
    else if (Live->Flags & NFS_FLAG_DIR)
    {
        return NFS_EUNSUPPORTED;
    }

    /* Rewrite the content from the pinned tree, copy-on-write through NfsWrite. */
    Status = NfsTruncate(Volume, Target, 0);
    if (Status != NFS_OK)
        return Status;

    if (Snapshot.Size)
    {
        Buffer = (nfs_u8 *)NfsMemAlloc(NFS_REVERT_CHUNK);
        if (!Buffer)
            return NFS_ENOSPC;

        while (Offset < Snapshot.Size)
        {
            nfs_u32 Want = (nfs_u32)((Snapshot.Size - Offset > NFS_REVERT_CHUNK)
                                         ? NFS_REVERT_CHUNK
                                         : (Snapshot.Size - Offset));
            nfs_u32 Got = 0;

            Status = NfsSnapshotRead(Volume, SnapshotId, FileId, Offset, Buffer, Want,
                                     &Got);
            if (Status != NFS_OK || Got == 0)
                break;

            Status = NfsWrite(Volume, Target, Offset, Buffer, Got, NULL);
            if (Status != NFS_OK)
                break;

            Offset += Got;
        }

        NfsMemFree(Buffer);
        if (Status != NFS_OK)
            return Status;
    }

    /* Timestamps and attributes come back with the data. */
    {
        NFS_FILE_INFO Info;

        memset(&Info, 0, sizeof(Info));
        Info.SetMask = NFS_SET_ATTRIBUTES | NFS_SET_CTIME | NFS_SET_MTIME |
                       NFS_SET_ATIME | NFS_SET_BTIME;
        Info.Attributes = Snapshot.Attributes;
        Info.Ctime = Snapshot.Ctime;
        Info.Mtime = Snapshot.Mtime;
        Info.Atime = Snapshot.Atime;
        Info.Btime = Snapshot.Btime;
        Status = NfsSetFileInfo(Volume, Target, &Info);
        if (Status != NFS_OK)
            return Status;
    }

    if (OutFileId)
        *OutFileId = Target;
    return NFS_OK;
}
