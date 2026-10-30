/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/bootsect/nextfs.S
 * PURPOSE:     NextFS volume boot record
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * A NextFS volume had no boot code and nowhere to put one: the superblock began
 * at offset 0, so sector 0 belonged to the file system. Format 5 reserves the
 * first 4 KiB of the volume as a boot area and moves the superblock to 4096;
 * this file is what goes into that area.
 *
 * What it does:
 *
 *   1. reads the superblock from LBA 8 and refuses anything that is not a
 *      NextFS volume in the format this loader understands;
 *   2. reads the boot descriptor from LBA 1, which the installer wrote;
 *   3. loads the payload -- a flat image listed as runs of absolute sectors --
 *      at the segment the descriptor names, and jumps to the entry it names.
 *
 * It parses no directory and walks no tree. The descriptor is built at install
 * time from the payload's real extent list by the file system library, which
 * already knows how. Keeping the copy-on-write walk out of real mode is the
 * point: 512 bytes cannot hold it, and a second implementation of the format in
 * real mode is a second thing that can be wrong.
 *
 * Boot descriptor, from LBA 1 (sector 0 is this boot record):
 *
 *   +0   char[4]  "NFB1"
 *   +4   u16      version (1)
 *   +6   u16      sector size (512)
 *   +8   u32      load segment
 *   +12  u32      entry offset
 *   +16  u32      entry segment
 *   +20  u16      run count
 *   +22  u16      reserved
 *   +24  runs: { u64 lba, u32 sectors, u32 reserved }, up to 168
 *
 * A payload has at most NFS_EXTENT_MAX (64) extents, so four sectors are enough.
 */

#include <asm.inc>

.code16

#define SUPER_LBA       8               /* 4096 / 512 */
#define SUPER_BUF       HEX(7E00)
#define DESC_LBA        1
#define DESC_SECTORS    4
#define DESC_BUF        HEX(8000)
#define DESC_RUN_MAX    168
#define MAX_CHUNK       127             /* the INT 13h AH=42h limit BIOSes honour */

start:
    jmp     main
    nop

/* OEM name: not a BPB, just the eight bytes tools expect to find here. */
    .ascii  "NFTLDR  "

main:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, HEX(7C00)
    sti
    mov     byte ptr [DriveNumber], dl

    /* 1. the superblock, at LBA 8 */
    mov     word ptr [DapCount], 8
    mov     word ptr [DapOffset], SUPER_BUF
    mov     word ptr [DapSegment], 0
    mov     dword ptr [DapLba], SUPER_LBA
    mov     dword ptr [DapLba+4], 0
    call    ReadDap
    jc      ErrDisk

    mov     eax, HEX(4253464E)                  /* "NFSB" */
    cmp     dword ptr [SUPER_BUF], eax
    jne     ErrNotNextFs
    cmp     word ptr [SUPER_BUF+4], 5           /* NFS_FORMAT_VERSION */
    jne     ErrNotNextFs
    cmp     word ptr [SUPER_BUF+6], 1024        /* header size */
    jne     ErrNotNextFs

    /* 2. the boot descriptor, from LBA 1 */
    mov     word ptr [DapCount], DESC_SECTORS
    mov     word ptr [DapOffset], DESC_BUF
    mov     word ptr [DapSegment], 0
    mov     dword ptr [DapLba], DESC_LBA
    mov     dword ptr [DapLba+4], 0
    call    ReadDap
    jc      ErrDisk

    mov     eax, HEX(3142464E)                  /* "NFB1" */
    cmp     dword ptr [DESC_BUF], eax
    jne     ErrNoDescriptor
    cmp     word ptr [DESC_BUF+4], 1            /* descriptor version */
    jne     ErrNoDescriptor
    mov     ax, word ptr [DESC_BUF+20]          /* run count */
    test    ax, ax
    jz      ErrNoDescriptor
    cmp     ax, DESC_RUN_MAX
    ja      ErrNoDescriptor

    /* 3. load the runs, upward, from the load segment the descriptor names */
    mov     ax, word ptr [DESC_BUF+8]
    mov     word ptr [LoadSegment], ax
    mov     si, DESC_BUF + 24
    mov     cx, word ptr [DESC_BUF+20]

RunLoop:
    test    cx, cx
    jz      RunLoaded

    mov     eax, dword ptr [si]                 /* lba */
    mov     dword ptr [CurLba], eax
    mov     eax, dword ptr [si+4]
    mov     dword ptr [CurLba+4], eax
    mov     ax, word ptr [si+8]                 /* sectors left in this run */
    mov     word ptr [CurCount], ax

ChunkLoop:
    movzx   ax, word ptr [CurCount]
    test    ax, ax
    jz      NextRun
    cmp     ax, MAX_CHUNK
    jbe     ChunkSizeOk
    mov     ax, MAX_CHUNK
ChunkSizeOk:
    mov     word ptr [DapCount], ax
    mov     word ptr [DapOffset], 0
    mov     ax, word ptr [LoadSegment]
    mov     word ptr [DapSegment], ax
    mov     eax, dword ptr [CurLba]
    mov     dword ptr [DapLba], eax
    mov     eax, dword ptr [CurLba+4]
    mov     dword ptr [DapLba+4], eax

    push    cx
    push    si
    call    ReadDap
    pop     si
    pop     cx
    jc      ErrDisk

    movzx   eax, word ptr [DapCount]
    mov     dx, ax
    shl     ax, 5                               /* 32 bytes per sector */
    add     word ptr [LoadSegment], ax
    movzx   eax, dx
    add     dword ptr [CurLba], eax
    adc     dword ptr [CurLba+4], 0
    sub     word ptr [CurCount], dx
    jmp     ChunkLoop

NextRun:
    add     si, 12
    dec     cx
    jmp     RunLoop

RunLoaded:
    cli
    mov     ax, word ptr [DESC_BUF+16]          /* entry segment */
    mov     bx, word ptr [DESC_BUF+12]          /* entry offset  */
    push    ax
    push    bx
    retf

/* ------------------------------------------------------------------ */
/* INT 13h AH=42h, with three retries. Buffer: DapCount sectors at     */
/* DapLba, to DapSegment:DapOffset.                                    */
/* ------------------------------------------------------------------ */
ReadDap:
    mov     byte ptr [DiskRetries], 3
Retry:
    mov     word ptr [DapSize], 16
    push    si
    mov     si, offset DapSize
    mov     dl, byte ptr [DriveNumber]
    mov     ah, HEX(42)
    int     HEX(13)
    pop     si
    jnc     ReadOk
    dec     byte ptr [DiskRetries]
    jnz     Retry
    stc
    ret
ReadOk:
    clc
    ret

/* ------------------------------------------------------------------ */
/* Console                                                            */
/* ------------------------------------------------------------------ */
Puts:
    push    ax
    push    bx
    push    si
NextChar:
    lodsb
    test    al, al
    jz      PutsDone
    mov     ah, HEX(0E)
    mov     bx, HEX(07)
    int     HEX(10)
    jmp     NextChar
PutsDone:
    pop     si
    pop     bx
    pop     ax
    ret

Fail:
    call    Puts
Halt:
    cli
    hlt
    jmp     Halt

ErrDisk:
    mov     si, offset MsgDisk
    jmp     Fail
ErrNotNextFs:
    mov     si, offset MsgBadVol
    jmp     Fail
ErrNoDescriptor:
    mov     si, offset MsgNoDescriptor
    jmp     Fail

/* ------------------------------------------------------------------ */
/* Data                                                               */
/* ------------------------------------------------------------------ */
.align 2
DapSize:        .word   16
DapCount:       .word   0
DapOffset:      .word   0
DapSegment:     .word   0
DapLba:         .quad   0
CurLba:         .quad   0
CurCount:       .word   0
LoadSegment:    .word   0
DriveNumber:    .byte   0
DiskRetries:    .byte   3

MsgDisk:        .asciz  "NF: disk error\r\n"
MsgBadVol:      .asciz  "NF: bad volume\r\n"
MsgNoDescriptor:.asciz  "NF: no descriptor\r\n"

    .org 510
    .word HEX(AA55)
