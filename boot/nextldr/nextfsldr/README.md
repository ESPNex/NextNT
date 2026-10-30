# nextfsldr — the NextFS first stage

This directory is the beginning of the end for the old boot architecture, and it
is worth being precise about what that architecture was, because most of it is
still here and still needed for every file system that is not NextFS.

## What was there before

    BIOS
     └─ volume boot record, one per file system    (fat.S, ntfs.S, ext.S, btrfs.S, …)
         └─ NEXTLDR.SYS, first two kilobytes        (frldr16.bin, linked at 0x0F800)
             └─ the same file, from 0x10000         (nextldr_pe, entered at 0xF803/0xFA00)
                 └─ rosload.exe → winldr → ntoskrnl

Two of those layers exist only because the boot sector cannot read a directory:
the boot sector has to find the loader, and the loader's first two kilobytes have
to finish loading the rest of the loader. Each file system therefore needed its
own boot sector, and the loader needed to know how it had been loaded.

## What this stage does instead

    BIOS
     └─ nextfs.S, 512 bytes: read the superblock, read the descriptor, load a flat image
         └─ nextfsldr.sys at 0x70000: mount NextFS, find the loader, load it as a PE
             └─ nextldr at 0x10000, entered at its own entry point
                 └─ rosload.exe → winldr → ntoskrnl

Nothing in the chain reads a FAT or an MFT, and nothing in it needs a second
stage to finish loading a file: the file system is read once, by code that is
allowed to be a whole 32-bit program.

## The three things it must get exactly right

**The real-mode service.** nextldr still calls into real mode for INT 13h, INT 10h
and INT 15h through the register block at `0x6F08` and the entry point at
`0x6F00`. Those addresses are in `x86common.h`, and this loader fills them with
its own dispatcher, linked to run at `0x0F800` — below the 64 KiB limit that a
real-mode code selector has, and no longer inside the file the loader is in. The
dispatcher implements the same `FNID_*` table with the same `REGS` layout, so
nextldr cannot tell the difference.

**The image.** `nextldr.sys` is the old stub plus a PE. The PE inside it starts
at an offset nobody wrote down, so `peload.c` scans for an MZ header whose
`e_lfanew` names a PE signature of the right machine and optional-header kind,
and treats every file offset in the PE as relative to *that* header. A loader
that is a bare PE, with no stub in front, loads through the same path.

**The handover.** Protected mode on, selectors `0x08`/`0x10`, stack at `0x0F000`,
interrupts off, `DL` = boot drive, `DH` = partition. `arch/i386/entry.S` has
read the machine in that state since FreeLoader was written, so that is the
state it gets.

## Memory

    0x005000  INT 13h bounce buffer (12 sectors)
    0x006F00  nextldr's real-mode BSS: register set, entry point, boot drive
    0x00F000  nextldr's 32-bit stack top
    0x00F800  this loader's real-mode half
    0x010000  nextldr_pe, at the image base its header names (max 0x60000 bytes)
    0x070000  this loader: code, data and the low half's image
    0x078000  .bss
    0x07A000  the NextFS record arena, to 0x088000
    0x088000  where nextldr's own memory manager takes over

## Preparing a volume to boot this way

    nextfsinstall --image boot.img --size 64 --label NEXTNT \
                  --source <tree with ReactOS/nextldr.sys, ReactOS/nextfsldr.sys> \
                  --dest '\ReactOS' \
                  --boot-code nextfs.bin \
                  --boot-loader /ReactOS/nextfsldr.sys \
                  --boot-load 0x7000

`/NEXTLDR.CNF` at the volume root, if present, is a one-line override of the path
to the second stage. It exists so a test volume can point somewhere else without
a rebuild.

## Where it stands

Built, booted and observed in QEMU: the boot record loads this stage from a 64 MiB
NextFS volume, the stage mounts the volume (label, format 5, five records), finds
`\ReactOS\nextldr.sys` through the reader shared with the loader, loads its PE at
`0x10000` and jumps to `0x10f38`. The screen says so, line by line:

    NextFS stage 1
    NF: volume NEXTNT, format 5, 5 records
    NF: /ReactOS/nextldr.sys, 329216 bytes
    NF: image at 0x00010000, entry 0x00010f38
    Starting NextNT loader...

nextldr then runs and faults: `#GP(0x18)` on the far jump into the real-mode half,
after the loader has been through several BIOS calls of its own. The GDT and the
descriptor at `0x18` are intact when it happens, so the next thing to look at is
whether the loader expects its own real-mode entry (its `SwitchToReal` path, with
its own GDT loaded) rather than the one this stage installs. That is the open
item, and it is a debugging problem, not a design one.

## What is not here yet

- A partition offset. The stage reads the volume it was booted from at absolute
  LBA 0, which is what the volume boot record does too; booting NextFS from a
  partition needs the descriptor to carry the offset.
- Relocations. The PE is loaded at the base its header names, and an image linked
  elsewhere is a build error rather than something moved at boot.
- UEFI, and every architecture that does not use the PC BIOS path.
- The other file systems. They still have their boot sectors and their stub
  chain, and they keep them until the same treatment is worth giving them.
