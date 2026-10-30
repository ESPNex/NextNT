/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/lib/fs/bootman.c
 * PURPOSE:     Read BOOT.MAN from the boot volume and apply it to the boot options
 * LICENSE:     GPL-3.0-or-later
 */

#include <nextldr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);

/* ntldr/ntldropts.h is next to the code that uses it, not in the shared include
   tree, so the one function this file needs is declared here. */
PCSTR NtLdrGetOption(PCSTR Options, PCSTR OptionName);

#include <fs/nextfs_manifest.h>
#include <fs/bootman.h>

#define BOOTMAN_FILE        "BOOT.MAN"
#define BOOTMAN_CHUNK       4096

static NFMAN BootMan;
static BOOLEAN BootManLoaded = FALSE;
static CHAR BootManText[NFMAN_TEXT_MAX];
static UCHAR BootManBuffer[BOOTMAN_CHUNK];

static BOOLEAN
BootManSlurp(PCSTR BootPath, PCSTR Name, PCHAR Buffer, ULONG BufferSize, PULONG OutSize)
{
    FILEINFORMATION Information;
    ULONG FileId, Size, Count = 0;
    ARC_STATUS Status;

    Status = FsOpenFile(Name, BootPath, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        TRACE("BootManSlurp: '%s' at '%s' not opened (%lu)\n", Name, BootPath, Status);
        return FALSE;
    }

    Status = ArcGetFileInformation(FileId, &Information);
    if (Status != ESUCCESS || Information.EndingAddress.HighPart != 0 ||
        Information.EndingAddress.LowPart == 0 || Information.EndingAddress.LowPart > BufferSize)
    {
        ERR("BootManSlurp: '%s' has an unusable size\n", Name);
        ArcClose(FileId);
        return FALSE;
    }
    Size = Information.EndingAddress.LowPart;

    Status = ArcRead(FileId, Buffer, Size, &Count);
    ArcClose(FileId);

    if (Status != ESUCCESS || Count != Size)
    {
        ERR("BootManSlurp: '%s' read %lu of %lu bytes (%lu)\n", Name, Count, Size, Status);
        return FALSE;
    }

    *OutSize = Size;
    return TRUE;
}

BOOLEAN
BootManLoad(PCSTR BootPath)
{
    ULONG Size = 0;

    if (BootManLoaded)
        return TRUE;

    if (!BootManSlurp(BootPath, BOOTMAN_FILE, BootManText, sizeof(BootManText), &Size))
        return FALSE;

    if (!NfmanParse(&BootMan, BootManText, Size))
    {
        ERR("BootManLoad: '%s' is not a boot manifest\n", BOOTMAN_FILE);
        return FALSE;
    }

    BootManLoaded = TRUE;
    TRACE("BootManLoad: version %lu, %lu files, default '%s'\n",
          BootMan.Version, BootMan.Entries, BootMan.DefaultOs);
    return TRUE;
}

const NFMAN *
BootManGet(VOID)
{
    return BootManLoaded ? &BootMan : NULL;
}

const NFMAN_ENTRY *
BootManFind(PCSTR Role)
{
    if (!BootManLoaded)
        return NULL;

    return NfmanFind(&BootMan, NULL, Role);
}

/* "\\ReactOS\\ntoskrnl.exe" with BootPath "multi(0)disk(0)...\\ReactOS\\" becomes
   "ntoskrnl.exe"; anything outside BootPath keeps its leading separator, which
   the loader reads as a path from the root of the volume. */
PCSTR
BootManPathFor(PCSTR BootPath, PCSTR Role)
{
    const NFMAN_ENTRY *Entry = BootManFind(Role);
    PCSTR Tail, Name;
    SIZE_T Length;

    if (Entry == NULL)
        return NULL;

    Name = Entry->Path;
    if (*Name == '\\' || *Name == '/')
        Name++;

    if (BootPath != NULL)
    {
        Tail = strrchr(BootPath, ')');
        Tail = (Tail != NULL) ? Tail + 1 : BootPath;

        while (*Tail == '\\' || *Tail == '/')
            Tail++;

        Length = strlen(Tail);
        if (Length != 0 && _strnicmp(Name, Tail, Length) == 0)
        {
            PCSTR Rest = Name + Length;
            while (*Rest == '\\' || *Rest == '/')
                Rest++;
            if (*Rest != ANSI_NULL)
                return Rest;
        }
    }

    return Entry->Path;
}

BOOLEAN
BootManVerify(PCSTR BootPath, const NFMAN_ENTRY *Entry, PULONG ActualHash)
{
    FILEINFORMATION Information;
    ULONG FileId, Remaining, Count;
    ULONG Hash = NFMAN_FNV1A_INIT;
    ARC_STATUS Status;

    if (Entry == NULL || !(Entry->Attributes & NFMAN_ATTR_HASH_STATED))
    {
        /* Nothing promised, nothing to break. */
        return TRUE;
    }

    Status = FsOpenFile(Entry->Path, BootPath, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        ERR("BootManVerify: cannot open '%s' (%lu)\n", Entry->Path, Status);
        return FALSE;
    }

    Status = ArcGetFileInformation(FileId, &Information);
    if (Status != ESUCCESS || Information.EndingAddress.HighPart != 0)
    {
        ArcClose(FileId);
        return FALSE;
    }
    Remaining = Information.EndingAddress.LowPart;

    if (Entry->Attributes & NFMAN_ATTR_SIZE_STATED && Entry->Size != Remaining)
    {
        ERR("BootManVerify: '%s' is %lu bytes, the manifest says %lu\n",
            Entry->Path, Remaining, (ULONG)Entry->Size);
        ArcClose(FileId);
        return FALSE;
    }

    while (Remaining != 0)
    {
        ULONG Want = (Remaining > sizeof(BootManBuffer)) ? sizeof(BootManBuffer) : Remaining;

        Status = ArcRead(FileId, BootManBuffer, Want, &Count);
        if (Status != ESUCCESS || Count != Want)
        {
            ERR("BootManVerify: '%s' truncated after %lu bytes\n", Entry->Path, Remaining);
            ArcClose(FileId);
            return FALSE;
        }

        Hash = NfmanFnv1a(Hash, BootManBuffer, Want);
        Remaining -= Want;
    }

    ArcClose(FileId);

    if (ActualHash != NULL)
        *ActualHash = Hash;

    if (Hash != Entry->Hash)
    {
        ERR("BootManVerify: '%s' hashes to %08lx, the manifest says %08lx\n",
            Entry->Path, Hash, Entry->Hash);
        return FALSE;
    }

    TRACE("BootManVerify: '%s' hash %08lx ok\n", Entry->Path, Hash);
    return TRUE;
}

/* Removes every "/KEY=..." token from the option string, in place. */
static VOID
BootManStripOption(PCHAR Options, ULONG OptionsSize, PCSTR Key)
{
    SIZE_T KeyLength = strlen(Key);
    PCHAR Read = Options, Write = Options;

    while (*Read != ANSI_NULL)
    {
        while (*Read == ' ')
            Read++;

        if (*Read == ANSI_NULL)
            break;

        if (*Read == '/' && _strnicmp(Read + 1, Key, KeyLength) == 0 &&
            Read[1 + KeyLength] == '=')
        {
            /* skip the token, and the blanks that follow it */
            while (*Read != ANSI_NULL && *Read != ' ')
                Read++;
            continue;
        }

        if (Write != Options)
            *Write++ = ' ';

        while (*Read != ANSI_NULL && *Read != ' ')
            *Write++ = *Read++;
    }

    *Write = ANSI_NULL;
    (void)OptionsSize;
}

BOOLEAN
BootManApply(PCSTR SystemPartition, PCHAR BootOptions, ULONG BootOptionsSize)
{
    const NFMAN_ENTRY *Kernel, *Hal;
    ULONG Hash = 0;
    PCSTR Path;

    if (!NtLdrGetOption(BootOptions, "BOOTMAN"))
        return FALSE;

    if (!BootManLoad(SystemPartition))
    {
        UiMessageBox("BOOTMAN was asked for, but there is no usable BOOT.MAN on '%s'.\n"
                     "Booting the old way.", SystemPartition);
        return FALSE;
    }

    Kernel = BootManFind("kernel");
    if (Kernel == NULL)
    {
        ERR("BootManApply: no kernel entry, keeping the old options\n");
        return FALSE;
    }

    if (!BootManVerify(SystemPartition, Kernel, &Hash))
    {
        UiMessageBox("BOOT.MAN does not match the kernel file:\n%s\n"
                     "Refusing to start it.", Kernel->Path);
        return FALSE;
    }

    BootManStripOption(BootOptions, BootOptionsSize, "KERNEL");
    BootManStripOption(BootOptions, BootOptionsSize, "HAL");

    Path = BootManPathFor(SystemPartition, "kernel");
    RtlStringCbCatA(BootOptions, BootOptionsSize, " /KERNEL=");
    RtlStringCbCatA(BootOptions, BootOptionsSize, Path);

    Hal = BootManFind("hal");
    if (Hal != NULL)
    {
        RtlStringCbCatA(BootOptions, BootOptionsSize, " /HAL=");
        RtlStringCbCatA(BootOptions, BootOptionsSize, BootManPathFor(SystemPartition, "hal"));
    }

    TRACE("BootManApply: kernel '%s', hash %08lx\n", Path, Hash);
    return TRUE;
}
