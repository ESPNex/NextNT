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

/* ------------------------------------------------------------------ */
/* Path resolution and creation dispositions (IRP_MJ_CREATE)           */
/* ------------------------------------------------------------------ */

#define NFS_PATH_MAX_COMPONENTS 128

typedef struct _NFS_PATH_ITER
{
    const char *Cursor;
} NFS_PATH_ITER, *PNFS_PATH_ITER;

/* Next component, or 0 when the path is exhausted. "." and ".." come back as
   such; the caller decides what they mean. */
static nfs_u32
NfsPathNext(PNFS_PATH_ITER Iter, const char **Start)
{
    const char *Cursor = Iter->Cursor;
    nfs_u32 Length = 0;

    while (*Cursor == '\\' || *Cursor == '/')
        Cursor++;
    if (*Cursor == 0)
    {
        Iter->Cursor = Cursor;
        return 0;
    }

    *Start = Cursor;
    while (Cursor[Length] && Cursor[Length] != '\\' && Cursor[Length] != '/')
        Length++;
    Iter->Cursor = Cursor + Length;
    return Length;
}

static int
NfsPathComponentIs(const char *Component, nfs_u32 Length, const char *Word)
{
    nfs_u32 i;

    for (i = 0; i < Length; i++)
        if (Component[i] != Word[i] || Word[i] == 0)
            return 0;
    return Word[Length] == 0;
}

/*
 * Walk a path from the root. On the last component the disposition decides
 * whether an existing file is opened as is, removed (supersede/overwrite) or
 * refused; everything before the last component must be an existing directory.
 */
int
NfsOpenPath(PNFS_VOLUME Volume, const char *Path, nfs_u32 Disposition,
            int IsDirectory, nfs_fileid *OutFileId, int *OutCreated)
{
    NFS_PATH_ITER Iter;
    const char *Component;
    nfs_u32 Length;
    nfs_fileid Parent = NFS_ROOT_ID;
    nfs_fileid Current = NFS_ROOT_ID;
    nfs_u32 Depth = 0;
    nfs_u16 Name[NFS_NAME_MAX];
    int Created = 0;
    int Status = NFS_OK;

    if (!Volume || !Path)
        return NFS_EINVAL;
    if (Disposition > NFS_DISPOSITION_OVERWRITE_IF)
        return NFS_EINVAL;

    Iter.Cursor = Path;

    for (;;)
    {
        NFS_PIFT_RECORD *Record;
        nfs_fileid Next = 0;
        int Last;

        Length = NfsPathNext(&Iter, &Component);
        if (Length == 0)
            break;                       /* trailing separators */

        if (++Depth > NFS_PATH_MAX_COMPONENTS)
            return NFS_EINVAL;

        /* Look ahead: is another component coming? */
        {
            NFS_PATH_ITER Probe = Iter;
            const char *Dummy;

            Last = (NfsPathNext(&Probe, &Dummy) == 0);
        }

        if (NfsPathComponentIs(Component, Length, "."))
        {
            if (Last)
                break;
            continue;
        }

        if (NfsPathComponentIs(Component, Length, ".."))
        {
            if (Last)
                break;
            Record = NfsIndexGet(Volume, Current);
            if (!Record)
                return NFS_ECORRUPT;
            Current = (Record->Parent == 0) ? NFS_ROOT_ID : Record->Parent;
            Parent = Current;
            continue;
        }

        if (Length > NFS_NAME_MAX * 4)
            return NFS_EINVAL;           /* cannot even hold NFS_NAME_MAX units */

        {
            nfs_u32 i;

            for (i = 0; i < Length; i++)
            {
                unsigned char Ch = (unsigned char)Component[i];

                /* Every byte of a UTF-8 sequence is >= 0x80, so this check still
                   sees the separators NT refuses and never a continuation byte. */
                if (Ch < 0x20 || Ch == ':' || Ch == '*' || Ch == '?' || Ch == '"' ||
                    Ch == '<' || Ch == '>' || Ch == '|')
                    return NFS_EINVAL;   /* characters NT refuses too */
            }
        }

        /* UTF-8 path component to UTF-16 units, the same decoder NfsNameFromAscii
           uses, so "perche'" and "perché" are different names and neither is cut. */
        Length = NfsUtf8NameDecode(Component, Length, Name, NFS_NAME_MAX);
        if (Length == 0)
            return NFS_EINVAL;

        Record = NfsChildrenLookup(Volume, Current, Name, Length);

        if (!Last)
        {
            if (!Record || (Record->Flags & NFS_FLAG_DELETED))
                return NFS_ENOENT;
            if (!(Record->Flags & NFS_FLAG_DIR))
                return NFS_ENOENT;       /* a file cannot have children */
            Current = Record->FileId;
            Parent = Current;
            continue;
        }

        /* Last component: apply the disposition. */
        switch (Disposition)
        {
        case NFS_DISPOSITION_CREATE:
            if (Record && !(Record->Flags & NFS_FLAG_DELETED))
                return NFS_EEXIST;
            Status = NfsCreate(Volume, Current, Name, Length, IsDirectory, &Next);
            Created = 1;
            break;

        case NFS_DISPOSITION_OPEN:
            if (!Record || (Record->Flags & NFS_FLAG_DELETED))
                return NFS_ENOENT;
            if ((Record->Flags & NFS_FLAG_DIR) && Record->FileId == NFS_ROOT_ID)
                Next = NFS_ROOT_ID;
            else
                Next = Record->FileId;
            if (IsDirectory && !(Record->Flags & NFS_FLAG_DIR))
                return NFS_EINVAL;
            break;

        case NFS_DISPOSITION_OPEN_IF:
            if (Record && !(Record->Flags & NFS_FLAG_DELETED))
            {
                Next = Record->FileId;
                if (IsDirectory && !(Record->Flags & NFS_FLAG_DIR))
                    return NFS_EINVAL;
            }
            else
            {
                Status = NfsCreate(Volume, Current, Name, Length, IsDirectory, &Next);
                Created = 1;
            }
            break;

        case NFS_DISPOSITION_SUPERSEDE:
        case NFS_DISPOSITION_OVERWRITE:
        case NFS_DISPOSITION_OVERWRITE_IF:
            if (Record && !(Record->Flags & NFS_FLAG_DELETED))
            {
                if (Record->Flags & NFS_FLAG_DIR)
                    return NFS_EINVAL;
                Next = Record->FileId;
                Status = NfsTruncate(Volume, Next, 0);
            }
            else if (Disposition == NFS_DISPOSITION_OVERWRITE)
            {
                return NFS_ENOENT;       /* overwrite needs an existing file */
            }
            else
            {
                Status = NfsCreate(Volume, Current, Name, Length, IsDirectory, &Next);
                Created = 1;
            }
            break;

        default:
            return NFS_EINVAL;
        }

        if (Status != NFS_OK)
            return Status;

        /* The object that was opened has to be of the kind the caller asked
           for: a directory does not open as a file and vice versa. */
        {
            NFS_PIFT_RECORD *Existing = NfsIndexGet(Volume, Next);

            if (Existing)
            {
                int IsDir = (Existing->Flags & NFS_FLAG_DIR) != 0;

                if (IsDir != (IsDirectory != 0))
                    return NFS_EINVAL;
            }
        }

        Parent = Current;
        if (OutFileId)
            *OutFileId = Next;
        if (OutCreated)
            *OutCreated = Created;
        return NFS_OK;
    }

    /* The path named the root itself (empty, ".", "..", or only separators). */
    if (!IsDirectory)
        return NFS_EINVAL;
    if (OutFileId)
        *OutFileId = NFS_ROOT_ID;
    if (OutCreated)
        *OutCreated = 0;
    (void)Parent;
    return NFS_OK;
}

int
NfsQueryVolumeInfo(PNFS_VOLUME Volume, NFS_VOLUME_INFO *OutInfo)
{
    if (!Volume || !OutInfo)
        return NFS_EINVAL;

    memset(OutInfo, 0, sizeof(*OutInfo));
    OutInfo->ClusterSize = Volume->Super.ClusterSize;
    OutInfo->TotalClusters = Volume->Super.TotalClusters;
    OutInfo->FreeClusters = Volume->Super.FreeClusters;
    OutInfo->TotalBytes = Volume->Super.TotalClusters * Volume->Super.ClusterSize;
    OutInfo->FreeBytes = Volume->Super.FreeClusters * Volume->Super.ClusterSize;
    OutInfo->Generation = Volume->Super.Generation;
    OutInfo->Files = Volume->Stats.Files;
    OutInfo->Snapshots = Volume->Stats.Snapshots;
    OutInfo->Flags = Volume->Super.Flags;
    memcpy(OutInfo->Label, Volume->Super.Label, NFS_LABEL_MAX);
    memcpy(OutInfo->Uuid, Volume->Super.VolumeUuid, 16);
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Enumeration with NT wildcards (IRP_MJ_DIRECTORY_CONTROL)            */
/* ------------------------------------------------------------------ */

static nfs_u16
NfsFold(nfs_u16 Unit)
{
    if (Unit >= 'A' && Unit <= 'Z')
        return (nfs_u16)(Unit - 'A' + 'a');
    return Unit;
}

static int
NfsMatchHere(const nfs_u16 *Name, nfs_u32 NameLength, const char *Pattern,
             nfs_u32 PatternLength)
{
    nfs_u32 n = 0, p = 0, StarPattern = 0, StarName = 0;

    /* Iterative wildcard match with a single backtrack point: "*" is greedy but
       remembers where it was, so "a*b*c" works without recursion. */
    while (n < NameLength)
    {
        if (p < PatternLength && Pattern[p] == '*')
        {
            StarPattern = ++p;
            StarName = n;
            continue;
        }

        if (p < PatternLength &&
            (nfs_u16)(unsigned char)Pattern[p] == NfsFold(Name[n]))
        {
            p++;
            n++;
            continue;
        }

        if (p < PatternLength && Pattern[p] == '?')
        {
            p++;
            n++;
            continue;
        }

        if (StarPattern)
        {
            p = StarPattern;
            n = ++StarName;
            continue;
        }

        return 0;
    }

    while (p < PatternLength && Pattern[p] == '*')
        p++;

    return p == PatternLength;
}

int
NfsMatchPattern(const nfs_u16 *Name, nfs_u32 NameLength, const char *Pattern)
{
    nfs_u32 PatternLength;

    if (!Name || !Pattern)
        return 0;

    PatternLength = (nfs_u32)strlen(Pattern);

    /* NT keeps the DOS rule: "*.*" also matches names without an extension. */
    if (PatternLength == 3 && Pattern[0] == '*' && Pattern[1] == '.' && Pattern[2] == '*')
        return 1;

    return NfsMatchHere(Name, NameLength, Pattern, PatternLength);
}

int
NfsEnumerate(PNFS_VOLUME Volume, nfs_fileid Directory, nfs_u32 *Cookie,
             const char *Pattern, NFS_PIFT_RECORD *OutRecord)
{
    nfs_u32 Index;

    if (!Volume || !Cookie || !OutRecord)
        return NFS_EINVAL;

    for (Index = *Cookie;; Index++)
    {
        NFS_PIFT_RECORD *Record = NfsChildrenAt(Volume, Directory, Index);

        if (!Record)
            return NFS_ENOENT;      /* end of the directory */

        if (Record->Flags & NFS_FLAG_DELETED)
            continue;
        if (Pattern && !NfsMatchPattern(Record->Name, Record->NameLen, Pattern))
            continue;

        *OutRecord = *Record;
        *Cookie = Index + 1;
        return NFS_OK;
    }
}

int
NfsDirectoryIsEmpty(PNFS_VOLUME Volume, nfs_fileid Directory)
{
    nfs_u32 Index;

    if (!Volume)
        return NFS_EINVAL;

    for (Index = 0;; Index++)
    {
        NFS_PIFT_RECORD *Record = NfsChildrenAt(Volume, Directory, Index);

        if (!Record)
            return 1;
        if (Record->Flags & NFS_FLAG_DELETED)
            continue;
        return 0;
    }
}

int
NfsQueryStandardInfo(PNFS_VOLUME Volume, nfs_fileid FileId,
                     NFS_STANDARD_INFORMATION *OutInfo)
{
    NFS_PIFT_RECORD *Record;

    if (!Volume || !OutInfo)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    memset(OutInfo, 0, sizeof(*OutInfo));
    OutInfo->CreationTime = Record->Btime;
    OutInfo->LastAccessTime = Record->Atime;
    OutInfo->LastWriteTime = Record->Mtime;
    OutInfo->ChangeTime = Record->Ctime;
    OutInfo->AllocationSize = Record->AllocSize;
    OutInfo->EndOfFile = Record->Size;
    OutInfo->Attributes = Record->Attributes;
    OutInfo->NumberOfLinks = 1;
    OutInfo->Directory = (Record->Flags & NFS_FLAG_DIR) ? 1 : 0;
    return NFS_OK;
}
