/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/include/fs/bootman.h
 * PURPOSE:     BOOT.MAN, from the loader's side
 * LICENSE:     GPL-3.0-or-later
 *
 * nextfs_manifest.c parses the file and knows nothing about disks. This is the
 * part that opens one: it reads BOOT.MAN off the boot volume through the same
 * ARC layer the rest of the loader uses, keeps the parsed copy, and answers two
 * questions -- where is the kernel, and does it still hash to what the manifest
 * says.
 *
 * It is switched off unless the boot options contain BOOTMAN, so a volume with a
 * manifest on it keeps booting the old way until somebody asks for the new one.
 */

#pragma once

#include <fs/nextfs_manifest.h>

/* Reads and parses <BootPath>\BOOT.MAN. FALSE when it is absent, unreadable or
   not a manifest; that is not an error condition, it is the default. */
BOOLEAN BootManLoad(PCSTR BootPath);

/* The parsed manifest, or NULL. */
const NFMAN *BootManGet(VOID);

/* First entry with this role, in the default operating system. */
const NFMAN_ENTRY *BootManFind(PCSTR Role);

/* The entry's path, made relative to BootPath when it points inside it, and
   with a leading separator dropped: that is the form the loader's KERNEL= and
   HAL= options expect. Returns NULL when the role is not in the manifest. */
PCSTR BootManPathFor(PCSTR BootPath, PCSTR Role);

/* Streams the file named by the entry and compares FNV-1a with the manifest.
   TRUE when the entry states no hash (nothing to check) and when it matches. */
BOOLEAN BootManVerify(PCSTR BootPath, const NFMAN_ENTRY *Entry, PULONG ActualHash);

/* The whole thing: when "BOOTMAN" is in BootOptions and the volume has a
   manifest, rewrites KERNEL= and HAL= to what the manifest says and checks the
   kernel's hash. Returns TRUE when the options were replaced. */
BOOLEAN BootManApply(PCSTR SystemPartition, PCHAR BootOptions, ULONG BootOptionsSize);
