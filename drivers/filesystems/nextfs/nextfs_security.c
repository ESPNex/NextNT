/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs_security.c
 * PURPOSE:     IRP_MJ_QUERY_SECURITY / IRP_MJ_SET_SECURITY
 * LICENSE:     GPL-3.0-or-later
 *
 * The descriptors themselves are the library's business: a PIFT record carries
 * the CRC32C of its descriptor and the blob is a file in \.security, so two files
 * with the same ACL point at the same blob (see nextfslib/nextfs_security.c).
 * This file is only the Win32 face of that store:
 *
 *   - QUERY_SECURITY reports the length first, copies the descriptor out trimmed
 *     to the parts the caller asked for, and answers with the volume default when
 *     the file has no descriptor of its own;
 *   - SET_SECURITY measures and copies the caller's descriptor out of user memory
 *     (both of these IRPs are METHOD_NEITHER: the pointer is a user address that
 *     must not be dereferenced once the request can block), then hands it to the
 *     library, which validates it again before storing it;
 *   - a null descriptor removes it, and the file goes back to the default.
 *
 * Two deliberate limits, both written down in DESIGN.md §8: the descriptor is
 * trimmed in place rather than rebuilt part by part, and the permission to change
 * it is checked on the handle's granted access instead of being an SeAccessCheck
 * against the stored descriptor. The second one is the next thing to fix, and it
 * is the reason this comment exists.
 */

#include <ntifs.h>

#include "nextfs.h"

/* The layout of the on-the-wire descriptor, spelled out here so the driver reads
   user memory with its own offsets instead of trusting a header it cannot see. */
typedef struct _NEXTFS_SD_RELATIVE
{
    UCHAR Revision;
    UCHAR Sbz1;
    USHORT Control;
    ULONG Owner;
    ULONG Group;
    ULONG Sacl;
    ULONG Dacl;
} NEXTFS_SD_RELATIVE, *PNEXTFS_SD_RELATIVE;

typedef struct _NEXTFS_ACL_HEADER
{
    UCHAR AclRevision;
    UCHAR Sbz1;
    USHORT AclSize;
    USHORT AceCount;
    USHORT Sbz2;
} NEXTFS_ACL_HEADER, *PNEXTFS_ACL_HEADER;

#define NEXTFS_SD_MIN_LENGTH   20u
#define NEXTFS_SD_SID_HEADER   8u
#define NEXTFS_SD_ACL_HEADER   8u

#define NEXTFS_SE_SELF_RELATIVE 0x8000u
#define NEXTFS_SE_DACL_PRESENT  0x0004u
#define NEXTFS_SE_SACL_PRESENT  0x0010u

/* SECURITY_INFORMATION bits, under our own names so nothing depends on which
   header the build happens to pull in. */
#define NEXTFS_SI_OWNER 0x00000001u
#define NEXTFS_SI_GROUP 0x00000002u
#define NEXTFS_SI_DACL  0x00000004u
#define NEXTFS_SI_SACL  0x00000008u

/*
 * The volume default: full access to Everyone, owner Administrators, group
 * Everyone. A NextFS volume with no descriptor set behaves like the FAT volumes
 * it is meant to replace, which is what an installer or a recovery tool expects
 * from a filesystem that has never been given an ACL.
 *
 *   20-byte header, owner S-1-5-32-544 (16), group S-1-1-0 (12), then a DACL
 *   with one ACCESS_ALLOWED ACE for S-1-1-0 carrying FILE_ALL_ACCESS (0x1F01FF).
 *
 * It is a constant and it is shared by every file without a descriptor; nothing
 * ever writes through the pointer it hands out.
 */
static const UCHAR NextFsDefaultDescriptor[NEXTFS_SD_MIN_LENGTH + 16 + 12 + 28] =
{
    /* revision, sbz1, control = self-relative | dacl present */
    0x01, 0x00, 0x04, 0x80,
    /* owner 20, group 36, sacl 0, dacl 48 */
    0x14, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00,
    /* S-1-5-32-544 Administrators */
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,
    /* S-1-1-0 Everyone */
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
    /* ACL: revision 2, 28 bytes, one ACE */
    0x02, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* ACCESS_ALLOWED, no flags, 20 bytes, FILE_ALL_ACCESS, Everyone */
    0x00, 0x00, 0x14, 0x00, 0xFF, 0x01, 0x1F, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
};

#define NEXTFS_DEFAULT_SD_LENGTH ((ULONG)sizeof(NextFsDefaultDescriptor))

/* ------------------------------------------------------------------ */
/* Reading a descriptor out of user memory                             */
/* ------------------------------------------------------------------ */

/* Length of a SID whose 8-byte header is already readable. */
static ULONG
NextFsSidLength(PUCHAR Sid)
{
    UCHAR Count;

    RtlCopyMemory(&Count, Sid + 1, sizeof(Count));
    return NEXTFS_SD_SID_HEADER + ((ULONG)Count * sizeof(ULONG));
}

/*
 * Measure a self-relative descriptor that lives in the caller's address space.
 * Offsets are followed one at a time under one exception frame, so a truncated or
 * hostile descriptor produces a status, never a fault. Bounded by NFS_SECURITY_MAX
 * for the same reason the library bounds what it accepts.
 */
static NTSTATUS
NextFsMeasureDescriptor(PNEXTFS_SD_RELATIVE Sd, PULONG OutLength)
{
    ULONG Length = NEXTFS_SD_MIN_LENGTH;
    USHORT Control = 0;
    ULONG Offset, Size;
    NTSTATUS Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        ProbeForRead(Sd, NEXTFS_SD_MIN_LENGTH, sizeof(UCHAR));
        Control = Sd->Control;

        if (Sd->Owner != 0)
        {
            Offset = Sd->Owner;
            ProbeForRead((PUCHAR)Sd + Offset, NEXTFS_SD_SID_HEADER, sizeof(UCHAR));
            Size = Offset + NextFsSidLength((PUCHAR)Sd + Offset);
            if (Size > Length)
                Length = Size;
        }
        if (Sd->Group != 0)
        {
            Offset = Sd->Group;
            ProbeForRead((PUCHAR)Sd + Offset, NEXTFS_SD_SID_HEADER, sizeof(UCHAR));
            Size = Offset + NextFsSidLength((PUCHAR)Sd + Offset);
            if (Size > Length)
                Length = Size;
        }
        if (Sd->Dacl != 0)
        {
            PNEXTFS_ACL_HEADER Acl;

            Offset = Sd->Dacl;
            Acl = (PNEXTFS_ACL_HEADER)((PUCHAR)Sd + Offset);
            ProbeForRead(Acl, NEXTFS_SD_ACL_HEADER, sizeof(UCHAR));
            Size = Offset + (ULONG)Acl->AclSize;
            if (Size > Length)
                Length = Size;
        }
        if (Sd->Sacl != 0)
        {
            PNEXTFS_ACL_HEADER Acl;

            Offset = Sd->Sacl;
            Acl = (PNEXTFS_ACL_HEADER)((PUCHAR)Sd + Offset);
            ProbeForRead(Acl, NEXTFS_SD_ACL_HEADER, sizeof(UCHAR));
            Size = Offset + (ULONG)Acl->AclSize;
            if (Size > Length)
                Length = Size;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_INVALID_USER_BUFFER;
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
        return Status;
    if (Length < NEXTFS_SD_MIN_LENGTH || Length > NFS_SECURITY_MAX)
        return STATUS_INVALID_PARAMETER;         /* not a descriptor we can store */

    /* An absolute descriptor names kernel addresses: it cannot be stored, and on
       NT the caller converts it first. Refusing here keeps that conversion in one
       place instead of two. */
    if (!(Control & NEXTFS_SE_SELF_RELATIVE))
        return STATUS_INVALID_PARAMETER;

    *OutLength = Length;
    return STATUS_SUCCESS;
}

/* Keep the parts the caller asked for and drop the rest, so a query for the owner
   does not hand back a DACL the caller is not allowed to see. */
static void
NextFsTrimDescriptor(PUCHAR Descriptor, ULONG Length, SECURITY_INFORMATION Requested)
{
    PNEXTFS_SD_RELATIVE Sd = (PNEXTFS_SD_RELATIVE)Descriptor;

    if (Length < NEXTFS_SD_MIN_LENGTH)
        return;

    if (!(Requested & NEXTFS_SI_OWNER))
        Sd->Owner = 0;
    if (!(Requested & NEXTFS_SI_GROUP))
        Sd->Group = 0;
    if (!(Requested & NEXTFS_SI_SACL))
    {
        Sd->Sacl = 0;
        Sd->Control &= (USHORT)~NEXTFS_SE_SACL_PRESENT;
    }
    if (!(Requested & NEXTFS_SI_DACL))
    {
        Sd->Dacl = 0;
        Sd->Control &= (USHORT)~NEXTFS_SE_DACL_PRESENT;
    }
}

/* Copy a pool copy of the descriptor into the caller's buffer. */
static NTSTATUS
NextFsCopyToUser(PUCHAR UserBuffer, ULONG Capacity, const UCHAR *Descriptor, ULONG Length)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (!UserBuffer || Capacity < Length)
        return STATUS_BUFFER_TOO_SMALL;

    _SEH2_TRY
    {
        ProbeForWrite(UserBuffer, Length, sizeof(UCHAR));
        RtlCopyMemory(UserBuffer, Descriptor, Length);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_INVALID_USER_BUFFER;
    }
    _SEH2_END;

    return Status;
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_QUERY_SECURITY                                               */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI
NextFsQuerySecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File;
    SECURITY_INFORMATION Requested = IrpSp->Parameters.QuerySecurity.SecurityInformation;
    PUCHAR Descriptor;
    ULONG Capacity = IrpSp->Parameters.QuerySecurity.Length;
    ULONG Length = 0;
    nfs_u32 BlobLength = 0;
    nfs_fileid FileId;
    int NfsStatus;
    NTSTATUS Status;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (!IrpSp->FileObject || (IrpSp->FileObject->Flags & FO_DIRECT_DEVICE_OPEN))
        return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    FileId = (File && File->Magic == NEXTFS_FILE_MAGIC) ? File->FileId : NFS_ROOT_ID;

    /* First pass: how long is the descriptor? The library answers with the length
       when it is handed no buffer, which is exactly the shape of this IRP. */
    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsSecurityQuery(Vcb->Volume, FileId, NULL, 0, &BlobLength);
    ExReleaseResourceLite(&Vcb->Resource);
    Length = BlobLength;

    if (NfsStatus == NFS_ENOENT)
    {
        /* Nothing of its own: report the volume default. */
        Length = NEXTFS_DEFAULT_SD_LENGTH;
    }
    else if (NfsStatus != NFS_ENOSPC || Length == 0 || Length > NFS_SECURITY_MAX)
    {
        return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
    }

    Descriptor = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length, NEXTFS_POOL_TAG);
    if (!Descriptor)
        return NextFsCompleteIo(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    if (NfsStatus == NFS_ENOENT)
    {
        RtlCopyMemory(Descriptor, NextFsDefaultDescriptor, Length);
    }
    else
    {
        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Length = BlobLength;
        NfsStatus = NfsSecurityQuery(Vcb->Volume, FileId, Descriptor, Length, &BlobLength);
        ExReleaseResourceLite(&Vcb->Resource);

        if (NfsStatus != NFS_OK)
        {
            ExFreePoolWithTag(Descriptor, NEXTFS_POOL_TAG);
            return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
        }
    }

    NextFsTrimDescriptor(Descriptor, Length, Requested);

    Status = NextFsCopyToUser((PUCHAR)Irp->UserBuffer, Capacity, Descriptor, Length);
    ExFreePoolWithTag(Descriptor, NEXTFS_POOL_TAG);

    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        /* The caller asked "how big?", which is not a failure: it gets the size
           and asks again. */
        return NextFsCompleteIo(Irp, STATUS_BUFFER_TOO_SMALL, Length);
    }
    return NextFsCompleteIo(Irp, Status, NT_SUCCESS(Status) ? Length : 0);
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_SET_SECURITY                                                 */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI
NextFsSetSecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNEXTFS_VCB Vcb = NextFsVcb(DeviceObject);
    PNEXTFS_FILE File;
    PSECURITY_DESCRIPTOR UserDescriptor = IrpSp->Parameters.SetSecurity.SecurityDescriptor;
    SECURITY_INFORMATION Requested = IrpSp->Parameters.SetSecurity.SecurityInformation;
    PUCHAR Descriptor;
    ULONG Length = 0;
    nfs_fileid FileId;
    int NfsStatus;
    NTSTATUS Status;

    if (!Vcb || Vcb->Shutdown)
        return NextFsCompleteIo(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (!IrpSp->FileObject || (IrpSp->FileObject->Flags & FO_DIRECT_DEVICE_OPEN))
        return NextFsCompleteIo(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    File = (PNEXTFS_FILE)IrpSp->FileObject->FsContext2;
    if (!File || File->Magic == 0 || File->Magic != NEXTFS_FILE_MAGIC)
        return NextFsCompleteIo(Irp, STATUS_INVALID_HANDLE, 0);
    FileId = File->FileId;

    /* Changing a descriptor is a write to the object's metadata. NT does not hand
       the FSD the access mask the create asked for -- FILE_OBJECT carries
       WriteAccess/DeleteAccess and not GrantedAccess -- so the test here is
       "this handle may write the file at all": coarse, deny by default, and
       written down in DESIGN.md §8 together with the SeAccessCheck that will
       replace it. */
    if (!IrpSp->FileObject->WriteAccess && !IrpSp->FileObject->DeleteAccess)
        return NextFsCompleteIo(Irp, STATUS_ACCESS_DENIED, 0);

    /* No descriptor at all: take it off the file, back to the volume default. */
    if (!UserDescriptor)
    {
        ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        NfsStatus = NfsSecuritySet(Vcb->Volume, FileId, NULL, 0);
        ExReleaseResourceLite(&Vcb->Resource);
        return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);
    }

    Status = NextFsMeasureDescriptor((PNEXTFS_SD_RELATIVE)UserDescriptor, &Length);
    if (!NT_SUCCESS(Status))
        return NextFsCompleteIo(Irp, Status, 0);

    Descriptor = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length, NEXTFS_POOL_TAG);
    if (!Descriptor)
        return NextFsCompleteIo(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    _SEH2_TRY
    {
        ProbeForRead(UserDescriptor, Length, sizeof(UCHAR));
        RtlCopyMemory(Descriptor, UserDescriptor, Length);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_INVALID_USER_BUFFER;
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Descriptor, NEXTFS_POOL_TAG);
        return NextFsCompleteIo(Irp, Status, 0);
    }

    ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    NfsStatus = NfsSecuritySet(Vcb->Volume, FileId, Descriptor, Length);
    ExReleaseResourceLite(&Vcb->Resource);

    ExFreePoolWithTag(Descriptor, NEXTFS_POOL_TAG);

    if (NfsStatus != NFS_OK)
        return NextFsCompleteIo(Irp, NextFsNfsStatus(NfsStatus), 0);

    /* The change is on disk, so the cached copy of the basic information is now
       one generation old; the file system change notification is what makes an
       Explorer window refresh its column. */
    return NextFsCompleteIo(Irp, STATUS_SUCCESS, 0);
}

/* EOF */
