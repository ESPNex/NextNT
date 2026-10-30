/*
 * PROJECT:     NextNT
 * FILE:        sdk/include/reactos/libs/fslib/nextfslib.h
 * PURPOSE:     Public definitions for the NextFS filesystem library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 */

#ifndef __NEXTFSLIB_H
#define __NEXTFSLIB_H

#include <fmifs/fmifs.h>

/*
 * Format a volume as NextFS. This is the PULIB_FORMAT entry point, so the setup
 * library (base/setup/lib/fsutil.c) can offer NextFS next to FAT, NTFS and Btrfs,
 * and the format is done by the same library code nextfs.sys runs at runtime.
 *
 * The volume is left unmounted and checkpointed: nextldr reads the checkpoint
 * tree and never the journal, so a volume straight out of the formatter has to
 * be clean, and this one is.
 */
BOOLEAN
NTAPI
NextfsFormat(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN QuickFormat,
    IN BOOLEAN BackwardCompatible,
    IN MEDIA_TYPE MediaType,
    IN PUNICODE_STRING Label,
    IN ULONG ClusterSize);

/*
 * There is no NextFS checker yet, and this deliberately does not pretend there
 * is: it reports "not implemented" instead of looking at the volume and calling
 * it clean. Registering it (rather than a NULL ChkdskFunc) keeps the error
 * message honest in the setup UI.
 */
BOOLEAN
NTAPI
NextfsChkdsk(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID pUnknown1,
    IN PVOID pUnknown2,
    IN PVOID pUnknown3,
    IN PVOID pUnknown4,
    IN PULONG ExitStatus);

#endif /* __NEXTFSLIB_H */
