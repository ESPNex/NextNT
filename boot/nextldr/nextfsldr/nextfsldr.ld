/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/nextfsldr.ld
 * PURPOSE:     Layout of the NextFS first stage
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * One image, loaded by the volume boot record at 0x70000 and jumped to at its
 * first byte. Three things are pinned here, and each of them is pinned by
 * something outside this file:
 *
 *   - the image base is 0x70000 because nextldr_pe is linked for 0x10000 and
 *     grows to about 0x62000, and because everything the BIOS writes to has to
 *     stay below the first megabyte;
 *   - the real-mode half runs at 0x0F800 because the 16-bit code selector has a
 *     64 KiB limit and nextldr jumps there through a bare offset;
 *   - .bss is not in the image because the NextFS arena is far larger than
 *     anything worth carrying on a floppy-sized boot image.
 */

ENTRY(stage1_entry16)

SECTIONS
{
    . = 0x70000;

    .text16 : { *(.text16) }

    /* Everything else the compiler produced: the C code lands in .text, and a
       section this script forgets is a section linked at an address nobody
       meant -- which is how the first version of this file spent an afternoon
       failing at 'mov ss, ax'. */
    .text32 : { *(.text32) *(.text) *(SORT(.text.*)) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    /* Where the real-mode half sits in the file, and where it ends up. */
    __lowblob_lma = .;

    .lowblob 0xF800 : AT(__lowblob_lma)
    {
        __lowblob_start = .;
        *(.lowblob)
        __lowblob_end = .;
    }

    __lowblob_seg_off = __lowblob_lma - 0x70000;
    __lowblob_size = SIZEOF(.lowblob);

    .bss 0x78000 (NOLOAD) :
    {
        __bss_start = .;
        *(.bss) *(COMMON)
        __bss_end = .;
    }

    ASSERT(__lowblob_lma + SIZEOF(.lowblob) <= 0x78000,
           "the first stage has grown into its own .bss")
    ASSERT(SIZEOF(.lowblob) <= 0x800,
           "the real-mode half no longer fits between 0x0F800 and 0x10000")
    ASSERT(__bss_end <= 0x7A000,
           ".bss has grown into the NextFS arena")
    ASSERT(__lowblob_lma <= 0x77FFF,
           "the low half would be loaded outside the first image segment")

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
