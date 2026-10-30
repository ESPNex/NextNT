/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/nextfsldr.h
 * PURPOSE:     Shared constants for the NextFS first stage
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * The whole first stage is one flat image in real memory, so every address here
 * is absolute physical memory, and this map is the contract that ties it to
 * boot/nextldr/nextldr/include/arch/pc/x86common.h. The values are not ours to
 * invent: nextldr's own code reads its real-mode entry point, its register set
 * and the boot drive from fixed addresses, and it will keep doing so until the
 * boot ABI grows a structure of its own.
 *
 *   0x05000 - 0x067FF   INT 13h bounce buffer
 *   0x06F00 - 0x06FFF   nextldr's real-mode BSS (register set, entry point)
 *   0x0F000             nextldr's 32-bit stack top
 *   0x0F800 - 0x0FFFF   this loader's real-mode half (copied there by the entry)
 *   0x10000 - 0x6FFFF   nextldr_pe, at the image base its own header names
 *   0x70000 - 0x77FFF   this loader: code, data, and the low half's image
 *   0x78000 - 0x7FFFF   this loader: .bss, the NextFS arena and buffers
 */

#pragma once

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define NFSLDR_IMAGE_BASE       0x70000u
#define NFSLDR_IMAGE_LIMIT      0x78000u
#define NFSLDR_BSS_BASE         0x78000u
#define NFSLDR_BSS_LIMIT        0x7A000u

/* The NextFS reader keeps every record of the checkpoint tree it mounts, so it
   gets everything between .bss and the memory nextldr's own manager expects to
   start with. It is dead the moment the loader takes over. */
#define NFSLDR_ARENA_BASE       0x7A000u
#define NFSLDR_ARENA_END        0x88000u

/* Overridable so the PE loader can be exercised on a development machine,
   where 0x10000 is not ours to write to. */
#ifndef NFSLDR_PE_BASE
#define NFSLDR_PE_BASE          0x10000u
#endif
#ifndef NFSLDR_PE_LIMIT
#define NFSLDR_PE_LIMIT         0x70000u
#endif

#define NFSLDR_LOW_BLOB         0xF800u
#define NFSLDR_STACK32          0xF000u
#define NFSLDR_STACK16          0x6F00u
#define NFSLDR_BSS16            0x6F00u
#define NFSLDR_DRIVE_SCRATCH    0x900u

#define NFSLDR_BOUNCE           0x5000u
#define NFSLDR_BOUNCE_SECTORS   12u
#define NFSLDR_SECTOR           512u

/* nextldr's real-mode BSS, from x86common.h */
#define NFSLDR_BSS_REALMODE_ENTRY   (NFSLDR_BSS16 + 0)
#define NFSLDR_BSS_REGISTERS        (NFSLDR_BSS16 + 8)
#define NFSLDR_BSS_INT_VECTOR       (NFSLDR_BSS16 + 52)
#define NFSLDR_BSS_BOOT_DRIVE       (NFSLDR_BSS16 + 108)
#define NFSLDR_BSS_BOOT_PARTITION   (NFSLDR_BSS16 + 109)

/* REGS layout, from pcbios.h */
#define NFSLDR_REGS_EAX     0
#define NFSLDR_REGS_EBX     4
#define NFSLDR_REGS_ECX     8
#define NFSLDR_REGS_EDX     12
#define NFSLDR_REGS_ESI     16
#define NFSLDR_REGS_EDI     20
#define NFSLDR_REGS_EBP     24
#define NFSLDR_REGS_DS      28
#define NFSLDR_REGS_ES      30
#define NFSLDR_REGS_FS      32
#define NFSLDR_REGS_GS      34
#define NFSLDR_REGS_EFLAGS  36

#define NFSLDR_EFLAGS_CF    0x0001u

/* The drive the BIOS booted from, written by the 16-bit entry into the
   real-mode half and read from there by everything else. */
extern u8 boot_drive;
#define NFSLDR_BIOS_DRIVE()     (boot_drive)
#define NFSLDR_BOOT_PARTITION   0

/* Where the second stage is, unless an NEXTLDR.CNF at the volume root says
   otherwise. The name is the old one on purpose: this is the same loader,
   reached a different way. */
#define NFSLDR_DEFAULT_PATH     "/ReactOS/nextldr.sys"
#define NFSLDR_CONFIG_PATH      "/NEXTLDR.CNF"
/* The manifest that says which files a boot is made of, and what they should
   hash to. Optional: without one, this stage behaves as it did before it
   existed, which is what makes the whole path shippable early. */
#define NFSLDR_MANIFEST_PATH    "/BOOT.MAN"
#define NFSLDR_PATH_MAX         128u
#define NFSLDR_HEADER_MAX       4096u

/* Real-mode calls out of the 32-bit half. */
void NfldrCallReal(u16 FunctionId);

/* Where nextldr's own entry point is, so that a real-mode call made from the
   loader comes back to the loader: its stub returns through this address. */
void NfldrSetLoaderEntry(u32 EntryPoint);

/* Where the copy of the interrupt vector table is, so that the real-mode half
   can put back any entry that a later stage wipes. */
void NfldrSetIvtBackup(u32 Address);

/* 32-bit helpers */
void NfldrPanic(const char *Message) __attribute__((noreturn));
void NfldrPuts(const char *Text);
void NfldrPrintHex(u32 Value);
void NfldrPrintDec(u32 Value);
void NfldrBiosInit(void);
int  NfldrBiosReadSectors(u64 Lba, u32 Count, void *Buffer);

/* PE loading */
int NfldrLoadPe(const u8 *Header, u32 HeaderSize,
                int (*ReadFile)(u64 Offset, void *Buffer, u32 Length, void *Context),
                void *Context, u64 FileSize, u32 *EntryPoint);
