/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/include/fs/nextfs_manifest.h
 * PURPOSE:     BOOT.MAN: the boot manifest that replaces NEXTLDR.INI and ARC paths
 * LICENSE:     GPL-3.0-or-later
 *
 * NEXTLDR.INI describes a boot in the vocabulary of the firmware it was born
 * with: an ARC device path, a list of "operating systems", and a kernel name
 * whose directory is assumed to be the volume's system directory. That made
 * sense when the loader could be handed a disk, a CD or a network boot from
 * anywhere. It does not make sense on a machine that boots one NextFS volume:
 * there is exactly one way to name a file, and the volume itself can say which
 * file is the kernel.
 *
 * BOOT.MAN is that: a small text manifest at the root of the boot volume. It
 * names the files a boot needs -- kernel, HAL, hive, drivers -- gives each one a
 * hash, and says which operating system to start. Everything here is read-only
 * and allocation-free: the parser walks a buffer somebody else already loaded.
 *
 * The grammar, in full (a line is LF or CRLF terminated, leading and trailing
 * blanks are ignored, '#' starts a comment):
 *
 *     version 1                       header, before any section
 *     default NextNT                  name of the [os ...] to boot
 *     timeout 5                       seconds to wait for a key, 0 = no menu
 *
 *     [os NextNT]                     one section per operating system
 *     kernel = \ReactOS\ntoskrnl.exe  hash=1f0c3a95 size=1720320
 *     hal    = \ReactOS\hal.dll       hash=0b7d1e44
 *     hive   = \ReactOS\system32\config\SYSTEM
 *     driver = \ReactOS\system32\drivers\nextfs.sys
 *
 * A line inside a section is "role = path [key=value ...]". The role is a single
 * word (up to 15 characters, matched case-insensitively); the known roles are
 * kernel, hal, hive, driver, module, config and setup, and anything else is kept
 * as it is, because the file is allowed to grow without a new parser. The path
 * is either one word or a quoted string. The attributes are hash= (8 hex digits,
 * FNV-1a 32 of the file's bytes, case-insensitive hex, an optional 0x) and
 * size= (decimal or 0x hex): both are optional, and a missing hash means the
 * file is loaded unverified instead of refused.
 *
 * Lines that do not fit are counted, not fatal: a manifest written for a newer
 * loader still boots, and the count is what tells the person at the screen that
 * something was skipped. A manifest that is missing entirely is not an error
 * either -- the loader then keeps its old behaviour, which is why this whole
 * path can be shipped switched off.
 */

#pragma once

#define NFMAN_MAX_ENTRIES   32
#define NFMAN_ROLE_MAX      16
#define NFMAN_SECTION_MAX   32
#define NFMAN_PATH_MAX      128
#define NFMAN_TEXT_MAX      4096

typedef struct _NFMAN_ENTRY
{
    char Role[NFMAN_ROLE_MAX];
    char Section[NFMAN_SECTION_MAX];
    char Path[NFMAN_PATH_MAX];
    unsigned long long Size;    /* 0 = not stated */
    unsigned int Hash;          /* 0 = not stated, otherwise FNV-1a 32 */
    unsigned int Attributes;    /* NFMAN_ATTR_* */
} NFMAN_ENTRY, *PNFMAN_ENTRY;

#define NFMAN_ATTR_SIZE_STATED  0x00000001u
#define NFMAN_ATTR_HASH_STATED  0x00000002u
#define NFMAN_ATTR_QUOTED_PATH  0x00000004u

typedef struct _NFMAN
{
    unsigned int Version;
    unsigned int Timeout;
    unsigned int Entries;
    unsigned int Skipped;
    char DefaultOs[NFMAN_SECTION_MAX];
    NFMAN_ENTRY Table[NFMAN_MAX_ENTRIES];
} NFMAN, *PNFMAN;

/* FNV-1a, 32 bit. Cheap, non-cryptographic, and enough to catch a truncated
   read or a file somebody edited on the volume; it is not a security boundary
   and does not pretend to be one. */
#define NFMAN_FNV1A_INIT    2166136261u
unsigned int NfmanFnv1a(unsigned int Seed, const void *Data, unsigned int Length);

/* False when the buffer has no directives at all, which is how a caller tells
   "not a manifest" (a text file somebody put there) from "an empty one". */
int NfmanParse(NFMAN *Manifest, const char *Text, unsigned int Length);

/* First entry with this role in this section. A null Section means: the default
   operating system, or the only section when no default is named. */
const NFMAN_ENTRY *NfmanFind(const NFMAN *Manifest, const char *Section, const char *Role);
