/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/peload.c
 * PURPOSE:     Load nextldr's PE image from the first stage
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * nextldr.sys is two files concatenated: the two kilobytes of real-mode stub
 * this project is getting rid of, and the PE image that was always behind them.
 * The old boot sectors jumped into the stub and the stub finished reading the
 * file itself, which is why every file system needed its own boot sector.
 *
 * Here the file is found by the file system that holds it, so the stub has no
 * work left to do: this loader walks to the PE header -- wherever in the file it
 * starts -- and lays the image down at the base the header names. A loader that
 * is a bare PE, with no stub in front, loads just as well; scanning for the
 * header instead of trusting an offset is what makes both cases one case.
 *
 * No relocations. The image is linked for 0x10000 and the first stage puts it
 * there; an image linked for an address this loader would have to move is a
 * build error the user should see, not something to paper over at boot.
 */

#include "nextfsldr.h"

#define IMAGE_FILE_MACHINE_I386     0x014Cu
#define IMAGE_NT_OPTIONAL_MAGIC_32  0x010Bu

#define COFF_SIZE                   20u
#define SECTION_SIZE                40u

/* Offsets inside the 32-bit optional header */
#define OPT_MAGIC                   0u
#define OPT_ENTRY_POINT             16u
#define OPT_IMAGE_BASE              28u
#define OPT_SIZE_OF_IMAGE           56u
#define OPT_SIZE_OF_HEADERS         60u
#define OPT_DIRECTORY_COUNT         92u
#define OPT_DIRECTORIES             96u

/* Offsets inside a section header */
#define SEC_VIRTUAL_SIZE            8u
#define SEC_VIRTUAL_ADDRESS         12u
#define SEC_RAW_SIZE                16u
#define SEC_RAW_OFFSET              20u

static u16 LoadU16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 LoadU32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void ZeroBlock(u8 *p, u32 Length)
{
    while (Length--)
        *p++ = 0;
}

/* The header scan.
 *
 * The image does not necessarily start at the beginning of the file: nextldr.sys
 * is frldr16.bin with a PE appended, and the PE inside it begins wherever the
 * real-mode stub ends. So the scan looks for an MZ header whose e_lfanew points
 * at a PE signature that describes something which could run here, and reports
 * where that MZ header is -- because every file offset inside the PE's own
 * headers is relative to it, not to the beginning of the file. */
#define IMAGE_DOS_HEADER_E_LFANEW   60u

static u32 FindPeHeader(const u8 *Header, u32 HeaderSize)
{
    u32 Offset;
    const u8 *Coff;
    const u8 *Opt;
    u32 PeOffset;
    u16 Sections;

    for (Offset = 0; Offset + 64 + COFF_SIZE + 224 <= HeaderSize; Offset += 4)
    {
        if (Header[Offset] != 'M' || Header[Offset + 1] != 'Z')
            continue;

        PeOffset = LoadU32(Header + Offset + IMAGE_DOS_HEADER_E_LFANEW);
        if (PeOffset < 64 || PeOffset > HeaderSize)
            continue;
        if (Offset + PeOffset + 4 + COFF_SIZE + 224 > HeaderSize)
            continue;
        if (Header[Offset + PeOffset] != 'P' ||
            Header[Offset + PeOffset + 1] != 'E' ||
            Header[Offset + PeOffset + 2] != 0 ||
            Header[Offset + PeOffset + 3] != 0)
        {
            continue;
        }

        Coff = Header + Offset + PeOffset + 4;
        Sections = LoadU16(Coff + 2);
        if (LoadU16(Coff) != IMAGE_FILE_MACHINE_I386)
            continue;
        if (Sections == 0 || Sections > 32)
            continue;
        if (LoadU16(Coff + 16) < 224)
            continue;

        Opt = Coff + COFF_SIZE;
        if (LoadU16(Opt + OPT_MAGIC) != IMAGE_NT_OPTIONAL_MAGIC_32)
            continue;

        return Offset;
    }

    return 0xFFFFFFFFu;
}

int NfldrLoadPe(const u8 *Header,
                u32 HeaderSize,
                int (*ReadFile)(u64 Offset, void *Buffer, u32 Length, void *Context),
                void *Context,
                u64 FileSize,
                u32 *EntryPoint)
{
    u32 Offset, PeOffset, Base, ImageSize, Entry;
    const u8 *Coff, *Opt, *Sec;
    u16 Sections, Index;
    int Status;

    /* Where the PE starts inside the file: zero for a bare image, the size of
       the real-mode stub for the nextldr.sys this project still ships. Every
       file offset in the PE's own headers is relative to that point, and the
       image base corresponds to that point as well. */
    Offset = FindPeHeader(Header, HeaderSize);
    if (Offset == 0xFFFFFFFFu)
        return -1;                              /* no PE inside the file */

    /* Offset is now the start of the PE file inside this one, so the signature
       is e_lfanew bytes into it. */
    PeOffset = LoadU32(Header + Offset + IMAGE_DOS_HEADER_E_LFANEW);
    Coff = Header + Offset + PeOffset + 4;
    Opt = Coff + COFF_SIZE;
    Sections = LoadU16(Coff + 2);

    Base = LoadU32(Opt + OPT_IMAGE_BASE);
    ImageSize = LoadU32(Opt + OPT_SIZE_OF_IMAGE);
    Entry = LoadU32(Opt + OPT_ENTRY_POINT);

    if (Base != NFSLDR_PE_BASE)
        return -2;                              /* wrong base, and no relocations */
    if (ImageSize == 0 || Base + ImageSize > NFSLDR_PE_LIMIT)
        return -3;                              /* would run into this loader */

    Sec = Opt + LoadU16(Coff + 16);

    /* Headers are part of the image too; the loader reads its own export table
       out of them, so they have to be there. */
    if ((u64)Offset + LoadU32(Opt + OPT_SIZE_OF_HEADERS) <= FileSize)
    {
        Status = ReadFile(Offset, (void *)Base, LoadU32(Opt + OPT_SIZE_OF_HEADERS), Context);
        if (Status != 0)
            return -6;
    }


    for (Index = 0; Index < Sections; Index++, Sec += SECTION_SIZE)
    {
        u32 VirtualSize = LoadU32(Sec + SEC_VIRTUAL_SIZE);
        u32 VirtualAddress = LoadU32(Sec + SEC_VIRTUAL_ADDRESS);
        u32 RawSize = LoadU32(Sec + SEC_RAW_SIZE);
        u32 RawOffset = LoadU32(Sec + SEC_RAW_OFFSET);
        u32 Span = (VirtualSize > RawSize) ? VirtualSize : RawSize;
        u8 *Destination = (u8 *)(Base + VirtualAddress);

        if (Span == 0)
            continue;
        if (VirtualAddress >= ImageSize || VirtualAddress + Span > ImageSize)
            return -4;                          /* a section outside the image */
        if (Base + VirtualAddress + Span > NFSLDR_PE_LIMIT)
            return -3;

        if (RawSize != 0)
        {
            if ((u64)Offset + RawOffset + RawSize > FileSize)
                return -5;                      /* the file is shorter than it claims */

            Status = ReadFile((u64)Offset + RawOffset, Destination, RawSize, Context);
            if (Status != 0)
                return -6;
        }

        if (VirtualSize > RawSize)
            ZeroBlock(Destination + RawSize, VirtualSize - RawSize);
    }

    *EntryPoint = Base + Entry;
    return (int)Base;
}
