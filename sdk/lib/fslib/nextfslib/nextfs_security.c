/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_security.c
 * PURPOSE:     NextFS security descriptors: deduplicated store, no $Secure index
 * LICENSE:     GPL-3.0-or-later
 *
 * How NTFS does it: every file has a $SECURITY_DESCRIPTOR in $STANDARD_INFORMATION,
 * and the descriptor itself lives in $Secure, a named stream keyed by a hash with a
 * global $SII index and refcounts. Two indexes, one journal, and a whole class of
 * bugs where the refcount and the file disagree.
 *
 * How NextFS does it: the descriptor is a *file* inside \.security, named after its
 * CRC32C. Two files with the same ACL point at the same name, so deduplication is
 * not an index that can drift -- it is the filesystem itself. The PIFT record only
 * carries the 64-bit id and the length, and the blob is read through the ordinary
 * copy-on-write data path, snapshotted like any other file.
 *
 *   PIFT record { ..., SecurityId = CRC32C, SecurityLength }
 *                 |
 *                 +--> \.security\@sd-1a2b3c4d  (content = the self-relative SD)
 *
 * The descriptor is validated before it is stored (revision, self-relative flag,
 * every offset inside the blob), so a caller cannot park a descriptor in the
 * volume that would make the next reader walk off the end of the buffer.
 *
 * SecurityId == 0 means "no descriptor": the FSD synthesises the volume default.
 * A blob nobody points at any more is removed by NfsSecurityGc, which is called at
 * the end of a mount and after a delete.
 */

#include "nextfs_internal.h"

#include <string.h>

#define NFS_SECURITY_DIR      ".security"
#define NFS_SECURITY_PREFIX   "@sd-"
#define NFS_SECURITY_NAME_MAX 24

/* Offsets inside a self-relative SECURITY_DESCRIPTOR (Win32 layout). */
#define NFS_SD_MIN_LENGTH     20u
#define NFS_SD_REVISION       1u
#define NFS_SD_OFF_REVISION   0u
#define NFS_SD_OFF_CONTROL    2u
#define NFS_SD_OFF_OWNER      4u
#define NFS_SD_OFF_GROUP      8u
#define NFS_SD_OFF_SACL       12u
#define NFS_SD_OFF_DACL       16u
#define NFS_SD_SELF_RELATIVE  0x8000u

static void
SecurityIdName(nfs_u32 Id, nfs_u16 *Out, nfs_u32 *OutLength)
{
    static const char Hex[] = "0123456789abcdef";
    char Text[NFS_SECURITY_NAME_MAX];
    nfs_u32 i, Length = 0;

    for (i = 0; NFS_SECURITY_PREFIX[i] != 0; i++)
        Text[Length++] = NFS_SECURITY_PREFIX[i];
    for (i = 0; i < 8; i++)
        Text[Length++] = Hex[(Id >> ((7 - i) * 4)) & 0xF];
    Text[Length] = 0;                 /* NfsNameFromAscii walks to the terminator */

    *OutLength = NfsNameFromAscii(Text, Out, NFS_NAME_MAX);
}

/* The directory that holds the descriptors, created on first use. */
static int
SecurityDirId(PNFS_VOLUME Volume, nfs_fileid *OutId, int Create)
{
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 Length = NfsNameFromAscii(NFS_SECURITY_DIR, Name, NFS_NAME_MAX);
    NFS_PIFT_RECORD *Record = NfsChildrenLookup(Volume, NFS_ROOT_ID, Name, Length);

    if (Record && !(Record->Flags & NFS_FLAG_DELETED))
    {
        if (!(Record->Flags & NFS_FLAG_DIR))
            return NFS_ECORRUPT;
        *OutId = Record->FileId;
        return NFS_OK;
    }
    if (!Create)
        return NFS_ENOENT;

    return NfsCreate(Volume, NFS_ROOT_ID, Name, Length, 1, OutId);
}

/* A descriptor blob by id: the record, not its content. */
static int
SecurityBlobLookup(PNFS_VOLUME Volume, nfs_u32 Id, NFS_PIFT_RECORD *OutRecord)
{
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 Length = 0;
    nfs_fileid Dir;
    NFS_PIFT_RECORD *Record;
    int Status;

    Status = SecurityDirId(Volume, &Dir, 0);
    if (Status != NFS_OK)
        return Status;

    SecurityIdName(Id, Name, &Length);
    Record = NfsChildrenLookup(Volume, Dir, Name, Length);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (OutRecord)
        *OutRecord = *Record;
    return NFS_OK;
}

/* Is any live record still pointing at this blob? The PIFT is in RAM, so this is
   a walk over an array, not a tree. */
static int
SecurityReferenced(PNFS_VOLUME Volume, nfs_u32 Id)
{
    nfs_u32 i;

    if (Id == 0)
        return 0;
    for (i = 0; i < Volume->RecordCount; i++)
    {
        if ((Volume->Records[i].Flags & NFS_FLAG_DELETED) == 0 &&
            Volume->Records[i].SecurityId == Id)
            return 1;
    }
    return 0;
}

/*
 * Validate a self-relative security descriptor. This is deliberately strict: the
 * blob is going to be handed to SeAccessCheck by the FSD, and a descriptor whose
 * offsets do not make sense is an escalation waiting to happen.
 */
static int
SecurityValidate(const nfs_u8 *Blob, nfs_u32 Length)
{
    nfs_u16 Revision, Control;
    nfs_u32 OffOwner, OffGroup, OffSacl, OffDacl;

    if (!Blob || Length < NFS_SD_MIN_LENGTH || Length > NFS_SECURITY_MAX)
        return NFS_EINVAL;

    memcpy(&Revision, Blob + NFS_SD_OFF_REVISION, sizeof(Revision));
    memcpy(&Control, Blob + NFS_SD_OFF_CONTROL, sizeof(Control));
    memcpy(&OffOwner, Blob + NFS_SD_OFF_OWNER, sizeof(OffOwner));
    memcpy(&OffGroup, Blob + NFS_SD_OFF_GROUP, sizeof(OffGroup));
    memcpy(&OffSacl, Blob + NFS_SD_OFF_SACL, sizeof(OffSacl));
    memcpy(&OffDacl, Blob + NFS_SD_OFF_DACL, sizeof(OffDacl));

    if (Revision != NFS_SD_REVISION)
        return NFS_EINVAL;
    if (!(Control & NFS_SD_SELF_RELATIVE))
        return NFS_EINVAL;          /* an absolute descriptor names kernel addresses */

    /* Every offset is either 0 (absent) or inside the blob. */
    if ((OffOwner && OffOwner >= Length) || (OffGroup && OffGroup >= Length) ||
        (OffSacl && OffSacl >= Length) || (OffDacl && OffDacl >= Length))
        return NFS_EINVAL;

    return NFS_OK;
}

/* Write a new blob. The name is its CRC, so a second caller with the same
   descriptor finds the file instead of creating a twin. */
static int
SecurityStoreBlob(PNFS_VOLUME Volume, nfs_u32 Id, const void *Blob, nfs_u32 Length,
                  int *OutCreated)
{
    nfs_u16 Name[NFS_NAME_MAX];
    nfs_u32 NameLength = 0, Written = 0;
    nfs_fileid Dir = 0, FileId = 0;
    int Status;

    *OutCreated = 0;

    Status = SecurityDirId(Volume, &Dir, 1);
    if (Status != NFS_OK)
        return Status;

    if (SecurityBlobLookup(Volume, Id, NULL) == NFS_OK)
        return NFS_OK;                       /* already there: this is the dedup */

    SecurityIdName(Id, Name, &NameLength);
    Status = NfsCreate(Volume, Dir, Name, NameLength, 0, &FileId);
    if (Status != NFS_OK)
        return (Status == NFS_EEXIST) ? NFS_OK : Status;

    Status = NfsWrite(Volume, FileId, 0, Blob, Length, &Written);
    if (Status != NFS_OK || Written != Length)
        return (Status != NFS_OK) ? Status : NFS_EIO;

    *OutCreated = 1;
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int
NfsSecurityQuery(PNFS_VOLUME Volume, nfs_fileid FileId, void *Buffer,
                 nfs_u32 Capacity, nfs_u32 *Length)
{
    NFS_PIFT_RECORD *Record;
    NFS_PIFT_RECORD BlobRecord;
    nfs_u32 Got = 0;
    int Status;

    if (!Volume || !Length)
        return NFS_EINVAL;

    *Length = 0;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;
    if (Record->SecurityId == 0 || Record->SecurityLength == 0)
        return NFS_ENOENT;                   /* no descriptor: the FSD decides */

    Status = SecurityBlobLookup(Volume, (nfs_u32)Record->SecurityId, &BlobRecord);
    if (Status != NFS_OK)
        return Status;                       /* the record points at a missing blob */

    *Length = (nfs_u32)BlobRecord.Size;
    if (!Buffer || Capacity < BlobRecord.Size)
        return NFS_ENOSPC;                   /* the caller now knows how much to ask */

    Status = NfsRead(Volume, BlobRecord.FileId, 0, Buffer, (nfs_u32)BlobRecord.Size, &Got);
    if (Status != NFS_OK)
        return Status;
    if (Got != BlobRecord.Size)
        return NFS_ECORRUPT;                 /* the blob is shorter than the record says */

    *Length = Got;
    return NFS_OK;
}

int
NfsSecuritySet(PNFS_VOLUME Volume, nfs_fileid FileId, const void *Blob, nfs_u32 Length)
{
    NFS_PIFT_RECORD *Record, Updated;
    nfs_u32 NewId;
    nfs_u64 PreviousId;
    int Created = 0;
    int Status;

    if (!Volume)
        return NFS_EINVAL;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    /* Length 0 removes the descriptor: the file goes back to the volume default. */
    if (Length == 0 || !Blob)
    {
        if (Record->SecurityId == 0)
            return NFS_OK;
        PreviousId = Record->SecurityId;
        Updated = *Record;
        Updated.SecurityId = 0;
        Updated.SecurityLength = 0;
        Updated.Generation++;
        Status = NfsCommitRecords(Volume, &Updated, 1);
        if (Status != NFS_OK)
            return Status;
        if (!SecurityReferenced(Volume, (nfs_u32)PreviousId))
            (void)NfsSecurityGc(Volume);
        return NFS_OK;
    }

    Status = SecurityValidate((const nfs_u8 *)Blob, Length);
    if (Status != NFS_OK)
        return Status;

    NewId = NfsCrc32c(0, Blob, Length);
    PreviousId = Record->SecurityId;

    /* Storing the blob creates or writes a file, and that is a PIFT commit: the
       record array can be reallocated underneath us, so the pointer we took at
       the top of this function is not usable after this call. Re-fetch it. */
    Status = SecurityStoreBlob(Volume, NewId, Blob, Length, &Created);
    if (Status != NFS_OK)
        return Status;

    Record = NfsIndexGet(Volume, FileId);
    if (!Record || (Record->Flags & NFS_FLAG_DELETED))
        return NFS_ENOENT;

    Updated = *Record;
    Updated.SecurityId = NewId;
    Updated.SecurityLength = Length;
    Updated.Generation++;
    Status = NfsCommitRecords(Volume, &Updated, 1);
    if (Status != NFS_OK)
        return Status;

    /* The old blob may have just lost its last reference. */
    if (PreviousId != 0 && PreviousId != NewId &&
        !SecurityReferenced(Volume, (nfs_u32)PreviousId))
        (void)NfsSecurityGc(Volume);

    return NFS_OK;
}

/* Parse "@sd-1a2b3c4d" into its id. Returns 0xFFFFFFFF for anything else, which
   the collector treats as "leave it alone". */
static nfs_u32
SecurityNameId(const NFS_PIFT_RECORD *Entry)
{
    nfs_u32 Id = 0, i;

    if (Entry->NameLen != 12)
        return 0xFFFFFFFFu;
    for (i = 4; i < 12; i++)
    {
        nfs_u16 Unit = Entry->Name[i];
        nfs_u32 Digit;

        if (Unit >= '0' && Unit <= '9')
            Digit = Unit - '0';
        else if (Unit >= 'a' && Unit <= 'f')
            Digit = Unit - 'a' + 10;
        else
            return 0xFFFFFFFFu;
        Id = (Id << 4) | Digit;
    }
    return Id;
}

/*
 * Remove every blob no live record points at, then the directory itself if it
 * ended up empty. Called at the end of a mount and whenever a descriptor can lose
 * its last reference.
 *
 * One blob per pass, each pass starting the enumeration over: deleting an entry
 * shifts the enumeration order, and a stale cookie would skip the next one --
 * exactly the kind of bug that turns a garbage collector into a data destroyer.
 */
int
NfsSecurityGc(PNFS_VOLUME Volume)
{
    nfs_fileid Dir = 0;
    nfs_u32 Passes = 0;
    int Status;

    if (!Volume)
        return NFS_EINVAL;

    Status = SecurityDirId(Volume, &Dir, 0);
    if (Status != NFS_OK)
        return NFS_OK;                       /* no directory, nothing to collect */

    for (;;)
    {
        nfs_u32 Cookie = 0;
        nfs_fileid Victim = 0;
        NFS_PIFT_RECORD Entry;

        while (NfsEnumerate(Volume, Dir, &Cookie, NFS_SECURITY_PREFIX "*", &Entry) == NFS_OK)
        {
            nfs_u32 Id = SecurityNameId(&Entry);

            if (Id == 0xFFFFFFFFu || SecurityReferenced(Volume, Id))
                continue;
            Victim = Entry.FileId;
            break;
        }

        if (!Victim)
            break;
        if (++Passes > 4096)                 /* a hostile volume cannot spin us */
            break;
        if (NfsRemove(Volume, Victim) != NFS_OK)
            break;
    }

    /* NfsDirectoryIsEmpty answers 1/0, not an NFS status: asking it for NFS_OK
       would mean "not empty" and the directory would never go away. */
    if (NfsDirectoryIsEmpty(Volume, Dir))
        (void)NfsRemove(Volume, Dir);

    return NFS_OK;
}

/* EOF */
