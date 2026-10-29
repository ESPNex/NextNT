/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs.h
 * PURPOSE:     NextFS driver internal declarations
 * LICENSE:     GPL-3.0-or-later
 *
 * The driver is a thin translation layer: every IRP path in nextfs_create.c,
 * nextfs_rw.c, nextfs_finfo.c, nextfs_dirctl.c and nextfs_volinfo.c maps onto one
 * call in nextfslib, which owns the format, the PIFT and the journal. Nothing in
 * this header knows what a PIFT record looks like on disk.
 */

#ifndef _NEXTFS_DRIVER_H_
#define _NEXTFS_DRIVER_H_

#include "nextfs_format.h"

#define NEXTFS_VCB_MAGIC   0x4678734Eu   /* 'N','s','xF' */
#define NEXTFS_FILE_MAGIC  0x4668734Eu
#define NEXTFS_POOL_TAG    'sfxN'

/* Longest path this driver will build from a UNICODE_STRING: four UTF-8 bytes
   per unit, for NFS_PATH_MAX_COMPONENTS components would be unbounded, so the
   limit is a single buffer rather than a component count. */
#define NEXTFS_PATH_MAX_BYTES (4u * NFS_NAME_MAX * 8u)

/* ------------------------------------------------------------------ */
/* Block device glue                                                   */
/* ------------------------------------------------------------------ */

/* The context handed to NFS_BDEV: one volume device object to call down to. */
typedef struct _NFS_NT_DEVICE
{
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    LARGE_INTEGER PartitionOffset;
    LARGE_INTEGER Size;
} NFS_NT_DEVICE, *PNFS_NT_DEVICE;

/* ------------------------------------------------------------------ */
/* Per-volume control block                                            */
/* ------------------------------------------------------------------ */

typedef struct _NEXTFS_VCB NEXTFS_VCB, *PNEXTFS_VCB;

struct _NEXTFS_VCB
{
    ULONG Magic;
    PVPB Vpb;
    PDEVICE_OBJECT VolumeDeviceObject;   /* our own FSD volume device        */
    PDEVICE_OBJECT TargetDeviceObject;   /* the disk device below us         */
    NFS_NT_DEVICE NtDevice;
    NFS_BDEV Block;
    PNFS_VOLUME Volume;                  /* nextfslib, RAM PIFT included     */
    ERESOURCE Resource;
    ULONG ClusterSize;
    LARGE_INTEGER VolumeSize;
    volatile LONG OpenCount;
    BOOLEAN Shutdown;
};

/* ------------------------------------------------------------------ */
/* Per-FILE_OBJECT context (in FileObject->FsContext2)                 */
/* ------------------------------------------------------------------ */

typedef struct _NEXTFS_FILE
{
    ULONG Magic;
    PNEXTFS_VCB Vcb;
    nfs_u32 Handle;          /* slot in the volume handle table             */
    nfs_u32 Access;          /* NFS_HANDLE_READ / NFS_HANDLE_WRITE          */
    nfs_u32 FileId;
    ULONG Flags;
    PCHAR Path;              /* UTF-8, kept current across renames          */
} NEXTFS_FILE, *PNEXTFS_FILE;

#define NEXTFS_FILE_DIRECTORY        0x0001
#define NEXTFS_FILE_DELETE_ON_CLOSE  0x0002
#define NEXTFS_FILE_CREATED          0x0004
#define NEXTFS_FILE_APPEND           0x0008

/* ------------------------------------------------------------------ */
/* Shared helpers (nextfs.c)                                           */
/* ------------------------------------------------------------------ */

extern PDRIVER_OBJECT NextFsDriverObject;

NTSTATUS NextFsNfsStatus(int NfsStatus);
NTSTATUS NextFsCompleteIo(PIRP Irp, NTSTATUS Status, ULONG_PTR Information);
PNEXTFS_VCB NextFsVcb(PDEVICE_OBJECT DeviceObject);

/* UTF-16 to the UTF-8 the library speaks. Returns the byte count or 0 on
   overflow/illegal name; the output is always NUL terminated. */
ULONG NextFsUnicodeToUtf8(PCUNICODE_STRING String, PCHAR Buffer, ULONG MaxBytes);
/* Same conversion for a caller-owned unit buffer (directory patterns). */
ULONG NextFsUnitsToUtf8(PCWSTR Units, ULONG UnitCount, PCHAR Buffer, ULONG MaxBytes);

/* Full path of a child of FileObject (or of FileObject itself when Name is
   NULL). Pool allocated, caller frees with NextFsFreePath. "\\" for the root. */
PCHAR NextFsBuildPath(PNEXTFS_VCB Vcb, PFILE_OBJECT Related, PUNICODE_STRING Name);
void NextFsFreePath(PCHAR Path);

/* Open (with the NT disposition already translated) and build the context. */
NTSTATUS NextFsOpenFile(PNEXTFS_VCB Vcb, PCHAR Path, ULONG CreateDisposition,
                        BOOLEAN IsDirectory, nfs_u32 Access, PNEXTFS_FILE *OutFile);

/* Replace the stored path of an open file, used by rename. */
NTSTATUS NextFsSetFilePath(PNEXTFS_FILE File, PCHAR NewPath);

/* ------------------------------------------------------------------ */
/* Dispatch entry points                                               */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI NextFsCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsQueryEa(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsSetEa(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsSetVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI NextFsShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* The create disposition NT asks for, translated to the NextFS one. */
ULONG NextFsTranslateDisposition(ULONG CreateDisposition);

#endif /* _NEXTFS_DRIVER_H_ */
