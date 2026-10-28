/*
 * PROJECT:     NextNT
 * FILE:        drivers/filesystems/nextfs/nextfs.h
 * PURPOSE:     NextFS driver internal declarations
 * LICENSE:     GPL-3.0-or-later
 */

#ifndef _NEXTFS_DRIVER_H_
#define _NEXTFS_DRIVER_H_

/* nextfslib uses these names; the kernel side only needs the opaque handle. */
typedef PVOID NFS_VOLUME_HANDLE;

/* Defined in nextfs.c, used by the mount path. */
extern PDRIVER_OBJECT NextFsDriverObject;

#endif /* _NEXTFS_DRIVER_H_ */
