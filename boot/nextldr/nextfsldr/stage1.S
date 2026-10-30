/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/stage1.S
 * PURPOSE:     NextFS first stage: 16-bit entry and the real-mode half
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * The old way into NextNT was a boot sector that knew a file system and a
 * NEXTLDR.SYS that began with two kilobytes of real-mode code the boot sector
 * jumped into. The file system lives in that first half, which is why there was
 * a boot sector for every format and an extldr for the ones that could not fit.
 *
 * This file is the other half of the new arrangement. The volume boot record
 * loads one flat image and jumps to it; everything that reads a directory, a
 * tree or an extent list happens later, in 32-bit code, against the file system
 * that actually holds the loader. What stays in real mode is what only real
 * mode can do: INT 13h, INT 10h, INT 15h, and the other BIOS services nextldr
 * asks for through a fixed register block. That is a service, not a file
 * system, and it is the same service nextldr's own stub used to provide -- so
 * nextldr keeps working, and only the way it gets into memory is new.
 *
 * Two halves, one image:
 *
 *   .text16   the entry the boot record jumps to, at the image's first byte.
 *             It copies the second half down to 0x0F800, enables A20, loads the
 *             GDT and enters protected mode. It addresses no data of its own.
 *   .lowblob  linked to run at 0x0F800, below the first megabyte, because the
 *             real-mode code selector has a 64 KiB limit. It is the dispatcher:
 *             switch to real mode, run the INT the register block asks for,
 *             switch back. nextldr finds it through BSS_RealModeEntry.
 */

.intel_syntax noprefix

#define IMAGE_BASE          0x70000
#define LOW_BLOB            0x0F800
#define PMODE_CS            0x08
#define PMODE_DS            0x10
#define RMODE_CS            0x18
#define RMODE_DS            0x20
#define CR0_PE              0x00000001

#define STACK32             0x0F000
#define DRIVE_SCRATCH       0x900

/* nextldr's real-mode BSS */
#define BSS_INT_VECTOR      0x6F34
#define BSS_REGISTERS       0x6F08
#define REGS_EAX            0
#define REGS_EBX            4
#define REGS_ECX            8
#define REGS_EDX            12
#define REGS_ESI            16
#define REGS_EDI            20
#define REGS_EBP            24
#define REGS_DS             28
#define REGS_ES             30
#define REGS_FS             32
#define REGS_GS             34
#define REGS_EFLAGS         36

#define EFLAGS_CF           0x0001
#define EFLAGS_ZF           0x0040
#define EFLAGS_SF           0x0080

#define FNID_Int386             0
#define FNID_Reboot             1
#define FNID_Relocator16Boot    2
#define FNID_PxeCallApi         3
#define FNID_PnpBiosGetDeviceNodeCount  4
#define FNID_PnpBiosGetDeviceNode       5
#define FNID_PnpBiosGetDockStationInformation 6

/* ------------------------------------------------------------------ */
/* The entry the boot record jumps to: 0x7000:0000                     */
/* ------------------------------------------------------------------ */

.code16
.section .text16, "ax"

.global stage1_entry16
stage1_entry16:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00

    /* What the BIOS told us. The register block cannot hold it yet. */
    mov     byte ptr [DRIVE_SCRATCH], dl

    /* Copy the real-mode half down to 0x0F800, where the 16-bit code
       selector can reach it. */
    cld
    mov     ax, 0x7000
    mov     ds, ax
    mov     si, OFFSET __lowblob_seg_off
    mov     ax, LOW_BLOB >> 4
    mov     es, ax
    xor     di, di
    mov     cx, OFFSET __lowblob_size
    rep     movsb

    /* A20, through the keyboard controller. */
    call    enable_a20

    /* From here on, flat addresses with base zero. The BIOS drive number goes
       into the real-mode half, which nothing else in this loader touches: low
       memory between 0x400 and 0x4FF belongs to the BIOS data area and what it
       leaves above that is not ours to trust either. */
    xor     ax, ax
    mov     ds, ax
    mov     al, byte ptr [DRIVE_SCRATCH]
    mov     byte ptr [boot_drive], al
    lgdt    ds:[gdtptr]

    mov     eax, cr0
    or      eax, CR0_PE
    mov     cr0, eax

    /* ljmp 0x08:stage1_entry32, as 32-bit offset in 16-bit code */
    .byte   0x66, 0xEA
    .long   stage1_entry32
    .word   PMODE_CS

/* The 8042 sequence nextldr's own stub uses, kept identical on purpose:
   machines that needed the pulse there need it here. */
enable_a20:
    call    empty_8042
    mov     al, 0xD1
    out     0x64, al
    call    empty_8042
    mov     al, 0xDF
    out     0x60, al
    call    empty_8042
    mov     al, 0xFF
    out     0x64, al
    call    empty_8042
    ret

empty_8042:
    .word   0x00EB, 0x00EB
    in      al, 0x64
    cmp     al, 0xFF
    jz      empty_8042_done
    test    al, 2
    jnz     empty_8042
empty_8042_done:
    ret

/* ------------------------------------------------------------------ */
/* The 32-bit half starts here                                         */
/* ------------------------------------------------------------------ */

.code32
.section .text32, "ax"

.global stage1_entry32
stage1_entry32:
    mov     ax, PMODE_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, STACK32

    /* .bss is not in the image: the six hundred bytes of the NextFS arena and
       the buffers are zeroed here, before anything can look at them. */
    mov     edi, __bss_start
    mov     ecx, __bss_end
    sub     ecx, edi
    add     ecx, 3
    shr     ecx, 2
    xor     eax, eax
    cld
    rep     stosd

    /* Where nextldr must find its way back into real mode. */
    mov     dword ptr [0x6F00], offset switch_to_real16

    call    NfldrMain

    /* NfldrMain does not return; if it ever does, stop here. */
1:  cli
    hlt
    jmp     1b

/* ------------------------------------------------------------------ */
/* Real-mode calls                                                     */
/* ------------------------------------------------------------------ */

/* VOID NfldrCallReal(USHORT FunctionId);
 *
 * The register block at 0x6F08 is already filled in by the caller. We keep the
 * 32-bit stack, hand the machine to the dispatcher, and get it back in the
 * continue address it jumps to. */
.global NfldrCallReal
NfldrCallReal:
    push    ebp
    mov     ebp, esp
    pushad
    mov     dword ptr [stack32_save], esp
    mov     bx, word ptr [ebp + 8]

    /* ljmp RMODE_CS:switch_to_real16_local. Ours, not nextldr's: the return path
       below comes back to NfldrRealReturn, which is this loader's own code. */
    .byte   0xEA
    .long   switch_to_real16_local
    .word   RMODE_CS

.global NfldrRealReturn
NfldrRealReturn:
    mov     ax, PMODE_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, dword ptr [stack32_save]
    popad
    pop     ebp
    ret

/* VOID NfldrJumpToLoader(ULONG EntryPoint, UCHAR Drive, UCHAR Partition);
 *
 * The handover. Protected mode is on, the selectors are the ones the loader
 * expects, the stack is where it expects it, and DL/DH hold the drive and the
 * partition, because that is the contract boot/nextldr/nextldr/arch/i386/entry.S
 * has kept since FreeLoader was written. */
.global NfldrJumpToLoader
NfldrJumpToLoader:
    mov     ecx, dword ptr [esp + 8]        /* Drive */
    mov     edx, dword ptr [esp + 12]       /* Partition */
    mov     dh, dl
    mov     dl, cl
    mov     ecx, dword ptr [esp + 4]        /* EntryPoint, kept out of AX */

    /* Selectors through BX, because AX is where the loader expects... nothing in
       particular, but EAX is where the entry point has to end up, and a push
       across the stack switch below would be a push onto a different stack. */
    mov     bx, PMODE_DS
    mov     ds, bx
    mov     es, bx
    mov     fs, bx
    mov     gs, bx
    mov     ss, bx
    mov     esp, STACK32

    mov     eax, ecx
    cli
    jmp     eax

.section .data, "aw"
.align 4
stack32_save:
    .long   STACK32

/* ------------------------------------------------------------------ */
/* The real-mode half. Linked to run at 0x0F800.                       */
/* ------------------------------------------------------------------ */

.section .lowblob, "ax"
.code16

/* Entered from protected mode with CS:RMODE_CS.
 *
 * Two ways in, and the difference is where the exit goes. nextldr comes in
 * through the address it found in BSS_RealModeEntry and must be returned to its
 * own entry point, because that is the code that reloads the IDT, restores its
 * 32-bit stack from its own variable and jumps through its own ContinueAddress.
 * This loader's own INT 13h calls come in through switch_to_real16_local and go
 * back where NfldrCallReal expects. They both want the same real-mode machine,
 * so the only thing that differs is one byte, and it survives the round trip in
 * low memory because there is nowhere else to keep it. */
switch_to_real16:
    mov     byte ptr cs:[return_to_local], 0
    jmp     enter_real16

switch_to_real16_local:
    mov     byte ptr cs:[return_to_local], 1

enter_real16:
    /* Put back any interrupt vector that went missing. Something in the boot
       path writes over the bottom of low memory, and an interrupt vector that
       reads as 0000:0000 turns the next BIOS call into a jump to address zero.
       Only entries that are actually gone are restored, so a handler the
       firmware installed later is left where it is. The backup lives in the
       loader's own .bss below one megabyte, and its physical address is
       written into ivt_backup_ptr before the loader starts. */
    push    ds
    push    es
    push    si
    push    di
    push    ax
    push    cx

    mov     eax, dword ptr cs:[ivt_backup_ptr]
    mov     si, ax
    and     si, 0x000F
    shr     eax, 4
    mov     ds, ax
    xor     ax, ax
    mov     es, ax
    xor     di, di
    mov     cx, 256
.Lvector_loop:
    cmp     word ptr es:[di + 2], 0
    jne     .Lvector_next
    mov     ax, word ptr ds:[si]
    mov     word ptr es:[di], ax
    mov     ax, word ptr ds:[si + 2]
    mov     word ptr es:[di + 2], ax
.Lvector_next:
    add     di, 4
    add     si, 4
    loop    .Lvector_loop

    pop     cx
    pop     ax
    pop     di
    pop     si
    pop     es
    pop     ds

    /* Restore the segment registers to the real-mode limit */
    mov     ax, RMODE_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    /* Protected mode off */
    mov     eax, cr0
    and     eax, 0xFFFFFFFE
    mov     cr0, eax

    /* Clear the prefetch queue and correct CS */
    .byte   0xEA
    .word   inrmode
    .word   0

inrmode:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    xor     esp, esp
    mov     sp, word ptr ds:[stack16]

    /* Real-mode IDT */
    lidt    ds:[rmode_idtptr]

    sti

    /* Dispatch on the function id in BX */
    shl     bx, 1
    call    word ptr ds:[CallbackTable + bx]

    /* Fall through: back to protected mode. */
exit_to_protected:
    cli
    mov     word ptr ds:[stack16], sp

    lgdt    ds:[gdtptr]

    mov     eax, cr0
    or      eax, CR0_PE
    mov     cr0, eax

    /* ljmp PMODE_CS:inpmode32, with a 32-bit offset: the segment is 32-bit, so
       the stub that lands there is 32-bit code and says so. */
    .byte   0x66, 0xEA
    .long   inpmode32
    .word   PMODE_CS

.code32
inpmode32:
    mov     ax, PMODE_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    cmp     byte ptr ds:[return_to_local], 0
    jne     .Llocal_return

    /* nextldr: its own entry point, which reloads its IDT, its 32-bit stack and
       continues through its ContinueAddress, exactly as its own stub did. */
    jmp     dword ptr ds:[pm_entrypoint]

.Llocal_return:
    jmp     NfldrRealReturn

/* The entry point of the image nextldr was loaded from. Written by the C side
   before the jump, because only it knows where the PE said to start. */
/* Where the saved copy of the interrupt vector table is, written by the C side
   before the loader starts. */
.global NfldrSetIvtBackup
NfldrSetIvtBackup:
    mov     eax, dword ptr [esp + 4]
    mov     dword ptr ds:[ivt_backup_ptr], eax
    ret

.global NfldrSetLoaderEntry
NfldrSetLoaderEntry:
    mov     eax, dword ptr [esp + 4]
    mov     dword ptr ds:[pm_entrypoint], eax
    ret

.align 4
.global pm_entrypoint
pm_entrypoint:
    .long   0

return_to_local:
    .byte   0

.align 4
ivt_backup_ptr:
    .long   0
trace_count:
    .word   0
trace_ring:
    .space  128

.code16

CallbackTable:
    .word   Int386
    .word   Reboot
    .word   Relocator16Boot
    .word   PxeCallApi
    .word   PnpBiosGetDeviceNodeCount
    .word   PnpBiosGetDeviceNode
    .word   PnpBiosGetDockStationInformation

stack16:
    .word   0x6F00

/* The drive the BIOS booted from, filled in by the entry. */
.global boot_drive
boot_drive:
    .byte   0x80

/* INT with the register block's registers, as int386.inc does it, so that
   nextldr's Int386() sees exactly what it saw before. */
Int386:
    /* DEBUG: remember the stack pointer on the way in and on the way out, so
       that a BIOS that does not balance its own stack shows up as drift. */
    push    ax
    push    bx
    push    ds
    xor     ax, ax
    mov     ds, ax
    mov     bx, word ptr ds:[trace_count]
    inc     word ptr ds:[trace_count]
    and     bx, 15
    shl     bx, 3
    mov     ax, sp
    mov     word ptr ds:[trace_ring + bx], ax
    pop     ds
    pop     bx
    pop     ax

    push    ds
    push    es
    push    fs
    push    gs
    pushad

    mov     al, byte ptr ds:[BSS_INT_VECTOR]
    mov     byte ptr ds:[Int386_opcode], al

    /* Flags: keep CF, ZF, SF from the register block */
    pushf
    pop     cx
    and     cx, not (EFLAGS_CF or EFLAGS_ZF or EFLAGS_SF)
    mov     ax, word ptr ds:[BSS_REGISTERS + REGS_EFLAGS]
    and     ax, (EFLAGS_CF or EFLAGS_ZF or EFLAGS_SF)
    or      ax, cx
    push    ax
    popf

    mov     ax, word ptr ds:[BSS_REGISTERS + REGS_DS]
    mov     ds, ax
    mov     ax, word ptr ds:[BSS_REGISTERS + REGS_ES]
    mov     es, ax
    mov     ax, word ptr ds:[BSS_REGISTERS + REGS_FS]
    mov     fs, ax
    mov     ax, word ptr ds:[BSS_REGISTERS + REGS_GS]
    mov     gs, ax

    mov     eax, dword ptr cs:[BSS_REGISTERS + REGS_EBP]
    test    eax, eax
    jz      Int386_registers
    mov     ebp, eax
Int386_registers:
    mov     eax, dword ptr cs:[BSS_REGISTERS + REGS_EAX]
    mov     ebx, dword ptr cs:[BSS_REGISTERS + REGS_EBX]
    mov     ecx, dword ptr cs:[BSS_REGISTERS + REGS_ECX]
    mov     edx, dword ptr cs:[BSS_REGISTERS + REGS_EDX]
    mov     esi, dword ptr cs:[BSS_REGISTERS + REGS_ESI]
    mov     edi, dword ptr cs:[BSS_REGISTERS + REGS_EDI]

    .byte   0xCD
Int386_opcode:
    .byte   0x00

    mov     dword ptr cs:[BSS_REGISTERS + REGS_EAX], eax
    mov     dword ptr cs:[BSS_REGISTERS + REGS_EBX], ebx
    mov     dword ptr cs:[BSS_REGISTERS + REGS_ECX], ecx
    mov     dword ptr cs:[BSS_REGISTERS + REGS_EDX], edx
    mov     dword ptr cs:[BSS_REGISTERS + REGS_ESI], esi
    mov     dword ptr cs:[BSS_REGISTERS + REGS_EDI], edi
    mov     dword ptr cs:[BSS_REGISTERS + REGS_EBP], ebp

    mov     ax, ds
    mov     word ptr cs:[BSS_REGISTERS + REGS_DS], ax
    mov     ax, es
    mov     word ptr cs:[BSS_REGISTERS + REGS_ES], ax
    mov     ax, fs
    mov     word ptr cs:[BSS_REGISTERS + REGS_FS], ax
    mov     ax, gs
    mov     word ptr cs:[BSS_REGISTERS + REGS_GS], ax

    pushfd
    pop     dword ptr cs:[BSS_REGISTERS + REGS_EFLAGS]

    popad
    pop     gs
    pop     fs
    pop     es
    pop     ds

    /* DEBUG: the matching exit stack pointer for the entry above. */
    push    ax
    push    bx
    push    ds
    xor     ax, ax
    mov     ds, ax
    mov     bx, word ptr ds:[trace_count]
    dec     bx
    and     bx, 15
    shl     bx, 3
    mov     ax, sp
    mov     word ptr ds:[trace_ring + bx + 2], ax
    mov     al, byte ptr ds:[BSS_INT_VECTOR]
    mov     byte ptr ds:[trace_ring + bx + 4], al
    pop     ds
    pop     bx
    pop     ax
    ret

Reboot:
    cli
    mov     ax, 0x0003
    int     0x10
    mov     word ptr ds:[0x0472], 0
    .byte   0xEA
    .word   0xFFF0
    .word   0xF000

/* The relocator hand-over: the caller has already written the new CS:IP and
   stack into the register block. Only used when nextldr boots a real-mode
   operating system, which requires an image that is not protected mode code. */
Relocator16Boot:
    cli

    /* Flags */
    pushf
    pop     cx
    and     cx, not (EFLAGS_CF or EFLAGS_ZF or EFLAGS_SF)
    mov     ax, word ptr cs:[BSS_REGISTERS + REGS_EFLAGS]
    and     ax, (EFLAGS_CF or EFLAGS_ZF or EFLAGS_SF)
    or      ax, cx
    push    ax
    popf

    mov     ax, word ptr cs:[BSS_REGISTERS + REGS_DS]
    mov     ds, ax
    mov     ax, word ptr cs:[BSS_REGISTERS + REGS_ES]
    mov     es, ax
    mov     ax, word ptr cs:[BSS_REGISTERS + REGS_FS]
    mov     fs, ax
    mov     ax, word ptr cs:[BSS_REGISTERS + REGS_GS]
    mov     gs, ax

    /* Where to jump */
    mov     eax, dword ptr cs:[0x6F00]
    mov     dword ptr cs:[Relocator16Address], eax

    /* The stack the caller asked for */
    mov     eax, dword ptr cs:[0x6F04]
    mov     ss, ax
    mov     eax, dword ptr cs:[0x6F04]
    and     eax, 0xFFFF
    mov     esp, eax

    mov     eax, dword ptr cs:[BSS_REGISTERS + REGS_EAX]
    mov     ebx, dword ptr cs:[BSS_REGISTERS + REGS_EBX]
    mov     ecx, dword ptr cs:[BSS_REGISTERS + REGS_ECX]
    mov     edx, dword ptr cs:[BSS_REGISTERS + REGS_EDX]
    mov     esi, dword ptr cs:[BSS_REGISTERS + REGS_ESI]
    mov     edi, dword ptr cs:[BSS_REGISTERS + REGS_EDI]

    .byte   0xEA
Relocator16Address:
    .word   0x7C00
    .word   0x0000

/* There is no PXE and no PnP BIOS here, and pretending otherwise would be a
   lie the loader acts on. These return "not present" the way their callers
   already handle. */
PxeCallApi:
    mov     ax, 1
    ret

PnpBiosGetDeviceNodeCount:
    mov     ax, 0x81          /* unsupported function */
    ret

PnpBiosGetDeviceNode:
    mov     ax, 0x81
    ret

PnpBiosGetDockStationInformation:
    mov     ax, 0x81
    ret

.align 4
gdt:
    .word   0x0000, 0x0000
    .word   0x0000, 0x0000         /* NULL */

    .word   0xFFFF, 0x0000
    .word   0x9A00, 0x00CF         /* 32-bit flat CS */
    .word   0xFFFF, 0x0000
    .word   0x9200, 0x00CF         /* 32-bit flat DS */
    .word   0xFFFF, 0x0000
    .word   0x9E00, 0x0000         /* 16-bit real-mode CS */
    .word   0xFFFF, 0x0000
    .word   0x9200, 0x0000         /* 16-bit real-mode DS */

gdtptr:
    .word   0x0027
    .long   gdt

rmode_idtptr:
    .word   0x03FF
    .long   0

.global switch_to_real16
.global gdtptr

/* The blob must fit in the 2 KiB between 0x0F800 and the PE's image base. */
.if (. - gdt) < 0
.endif
