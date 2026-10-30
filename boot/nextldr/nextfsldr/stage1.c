/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/stage1.c
 * PURPOSE:     NextFS first stage: find and load the second stage
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * What this program is allowed to be wrong about, and what it is not:
 *
 *   - It reads exactly one volume, the one the BIOS booted from, at absolute
 *     LBA zero. A partition offset is not in the boot descriptor yet; when the
 *     descriptor grows one, the single function NfldrVolumeRead changes.
 *   - It does not write anything. There is no journal to replay and no
 *     allocation to update, which is the whole reason it can read a file
 *     system's tree this early.
 *   - It is not nextldr and does not try to be: no menus, no INI parsing, no
 *     drivers. It brings one file into memory and hands over the machine.
 *
 * The diagnostics matter as much as the code. A boot loader that fails in
 * silence is a boot loader that gets debugged by guesswork at 3 a.m., so every
 * failure here names itself, on the screen and on the first serial port.
 */

#include "nextfsldr.h"

#include <fs/nextfs.h>
#include <fs/nextfs_manifest.h>

/* Provided by stage1.S */
void NfldrJumpToLoader(u32 EntryPoint, u8 Drive, u8 Partition) __attribute__((noreturn));

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

#define COM1                0x3F8
#define VGA_TEXT            0xB8000u
#define VGA_COLUMNS         80u
#define VGA_ROWS            25u
#define VGA_ATTRIBUTE       0x07u

static u32 ConsoleRow;
static u32 ConsoleColumn;

static void SerialInit(void)
{
    volatile u8 *Port = (volatile u8 *)COM1;

    Port[1] = 0x00;                 /* no interrupts */
    Port[3] = 0x80;                 /* divisor latch */
    Port[0] = 0x01;                 /* 115200 */
    Port[1] = 0x00;
    Port[3] = 0x03;                 /* 8N1 */
    Port[2] = 0xC7;                 /* FIFO on, cleared */
    Port[4] = 0x0B;                 /* DTR, RTS, OUT2 */
}

static void SerialPutChar(char Character)
{
    volatile u8 *Port = (volatile u8 *)COM1;
    u32 Spin = 100000;

    while ((Port[5] & 0x20) == 0 && Spin-- != 0)
        ;

    Port[0] = (u8)Character;
}

static void VgaPutChar(char Character)
{
    volatile u16 *Screen = (volatile u16 *)VGA_TEXT;
    u32 Index;

    if (Character == '\n')
    {
        ConsoleColumn = 0;
        ConsoleRow++;
    }
    else if (Character == '\r')
    {
        ConsoleColumn = 0;
    }
    else if (Character == '\b')
    {
        if (ConsoleColumn > 0)
            ConsoleColumn--;
    }
    else
    {
        Index = ConsoleRow * VGA_COLUMNS + ConsoleColumn;
        if (Index < VGA_COLUMNS * VGA_ROWS)
            Screen[Index] = (u16)(VGA_ATTRIBUTE << 8) | (u8)Character;
        if (++ConsoleColumn >= VGA_COLUMNS)
        {
            ConsoleColumn = 0;
            ConsoleRow++;
        }
    }

    if (ConsoleRow >= VGA_ROWS)
    {
        /* Scroll one line: copy the text pages down and blank the last row. */
        for (Index = 0; Index < (VGA_ROWS - 1) * VGA_COLUMNS; Index++)
            Screen[Index] = Screen[Index + VGA_COLUMNS];
        for (; Index < VGA_ROWS * VGA_COLUMNS; Index++)
            Screen[Index] = (u16)(VGA_ATTRIBUTE << 8) | ' ';
        ConsoleRow = VGA_ROWS - 1;
    }
}

static void ConsolePutChar(char Character)
{
    if (Character == '\n')
        SerialPutChar('\r');
    SerialPutChar(Character);
    VgaPutChar(Character);
}

void NfldrBiosInit(void)
{
    SerialInit();
    ConsoleRow = 0;
    ConsoleColumn = 0;
}

void NfldrPuts(const char *Text)
{
    while (*Text != 0)
        ConsolePutChar(*Text++);
}

/* A label comes off the volume, so it is printed as a counted string: nothing
   on disk is allowed to decide where this loader's output ends. */
static void NfldrPutCounted(const char *Text, u32 Limit)
{
    u32 Index;

    if (Text[0] == 0)
    {
        NfldrPuts("(unlabelled)");
        return;
    }

    for (Index = 0; Index < Limit && Text[Index] != 0; Index++)
        ConsolePutChar(Text[Index]);
}

void NfldrPrintHex(u32 Value)
{
    static const char Digits[] = "0123456789abcdef";
    int Shift;

    NfldrPuts("0x");
    for (Shift = 28; Shift >= 0; Shift -= 4)
        ConsolePutChar(Digits[(Value >> Shift) & 0xF]);
}

void NfldrPrintDec(u32 Value)
{
    char Buffer[16];
    int Index = 0;

    if (Value == 0)
    {
        ConsolePutChar('0');
        return;
    }

    while (Value != 0 && Index < (int)sizeof(Buffer))
    {
        Buffer[Index++] = (char)('0' + (Value % 10));
        Value /= 10;
    }

    while (Index-- > 0)
        ConsolePutChar(Buffer[Index]);
}

void NfldrPanic(const char *Message)
{
    NfldrPuts("\nNF: ");
    NfldrPuts(Message);
    NfldrPuts("\n");

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Memory: one bump allocator, and a free that does nothing            */
/* ------------------------------------------------------------------ */

static u32 ArenaNext;

static void *ArenaAlloc(NFSB_U32 Size)
{
    u32 Address = (ArenaNext + 15u) & ~15u;
    u32 End = Address + ((Size + 15u) & ~15u);

    if (Address < NFSLDR_ARENA_BASE)
        Address = NFSLDR_ARENA_BASE;
    if (End > NFSLDR_ARENA_END)
        return 0;

    ArenaNext = End;
    return (void *)Address;
}

static void ArenaFree(void *Block)
{
    /* Deliberately empty: the volume is mounted once and the loader takes the
       machine before anything would be released. */
    (void)Block;
}

/* ------------------------------------------------------------------ */
/* BIOS sector reads                                                   */
/* ------------------------------------------------------------------ */

#define DAP_ADDRESS         0x4F00u
#define DAP_SEGMENT         (DAP_ADDRESS >> 4)

typedef struct _NFSLDR_DAP
{
    u8 Size;
    u8 Reserved;
    u16 Count;
    u16 Offset;
    u16 Segment;
    u32 LbaLow;
    u32 LbaHigh;
} NFSLDR_DAP;

static int BiosReadOnce(u64 Lba, u8 Count, void *Buffer)
{
    volatile NFSLDR_DAP *Dap = (volatile NFSLDR_DAP *)DAP_ADDRESS;
    volatile u32 *Registers = (volatile u32 *)NFSLDR_BSS_REGISTERS;
    volatile u16 *Segments;
    u32 Flags;

    Dap->Size = 16;
    Dap->Reserved = 0;
    Dap->Count = Count;
    Dap->Offset = (u16)((u32)Buffer & 0xF);
    Dap->Segment = (u16)((u32)Buffer >> 4);
    Dap->LbaLow = (u32)Lba;
    Dap->LbaHigh = (u32)(Lba >> 32);

    Registers[NFSLDR_REGS_EAX / 4] = 0x4200;            /* AH=42h, extended read */
    /* ES:BX is the buffer. The segment is the buffer shifted down four bits and
       the offset is what is left, which is not zero for a buffer the C compiler
       put at an arbitrary address inside the NextFS arena. */
    Registers[NFSLDR_REGS_EBX / 4] = Dap->Offset;
    Registers[NFSLDR_REGS_ECX / 4] = 0;
    Registers[NFSLDR_REGS_EDX / 4] = NFSLDR_BIOS_DRIVE(); /* DL = drive */
    Registers[NFSLDR_REGS_ESI / 4] = 0;
    Registers[NFSLDR_REGS_EDI / 4] = 0;
    Registers[NFSLDR_REGS_EBP / 4] = 0;

    Segments = (volatile u16 *)NFSLDR_BSS_REGISTERS;
    Segments[NFSLDR_REGS_DS / 2] = DAP_SEGMENT;
    Segments[NFSLDR_REGS_ES / 2] = Dap->Segment;
    Segments[NFSLDR_REGS_FS / 2] = 0;
    Segments[NFSLDR_REGS_GS / 2] = 0;
    Segments[NFSLDR_REGS_EFLAGS / 2] = 0;

    *(volatile u8 *)NFSLDR_BSS_INT_VECTOR = 0x13;

    NfldrCallReal(0);                                   /* FNID_Int386 */

    Flags = *(volatile u16 *)(NFSLDR_BSS_REGISTERS + NFSLDR_REGS_EFLAGS);
    return (Flags & NFSLDR_EFLAGS_CF) ? -1 : 0;
}

static void BiosResetDisk(void)
{
    volatile u32 *Registers = (volatile u32 *)NFSLDR_BSS_REGISTERS;

    Registers[NFSLDR_REGS_EAX / 4] = 0x0000;            /* AH=00h, reset */
    Registers[NFSLDR_REGS_EDX / 4] = NFSLDR_BIOS_DRIVE();
    *(volatile u8 *)NFSLDR_BSS_INT_VECTOR = 0x13;
    NfldrCallReal(0);
}

int NfldrBiosReadSectors(u64 Lba, u32 Count, void *Buffer)
{
    u8 *Destination = (u8 *)Buffer;
    u32 Chunk;
    int Attempt;

    while (Count != 0)
    {
        Chunk = Count;
        if (Chunk > NFSLDR_BOUNCE_SECTORS)
            Chunk = NFSLDR_BOUNCE_SECTORS;

        for (Attempt = 0; Attempt < 3; Attempt++)
        {
            if (BiosReadOnce(Lba, (u8)Chunk, (void *)NFSLDR_BOUNCE) == 0)
                break;
            BiosResetDisk();
        }

        if (Attempt == 3)
            return -1;

        /* The BIOS wrote into the bounce buffer; move it where the caller
           wants it, one byte at a time in a register, because there is no
           memcpy here and a rep movsb would need another selector dance. */
        {
            u32 Index;
            const u8 *Source = (const u8 *)NFSLDR_BOUNCE;
            for (Index = 0; Index < Chunk * NFSLDR_SECTOR; Index++)
                Destination[Index] = Source[Index];
        }

        Lba += Chunk;
        Count -= Chunk;
        Destination += Chunk * NFSLDR_SECTOR;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* NextFS: one read callback, then the library's own reader            */
/* ------------------------------------------------------------------ */

static int VolumeRead(void *Context, NFSB_U64 Offset, void *Buffer, NFSB_U32 Length)
{
    u8 *Destination = (u8 *)Buffer;
    u64 Sector;
    u32 Skip;

    (void)Context;

    while (Length != 0)
    {
        Sector = Offset / NFSLDR_SECTOR;
        Skip = (u32)(Offset % NFSLDR_SECTOR);

        if (Skip == 0 && Length >= NFSLDR_SECTOR && (u32)Destination < 0x100000)
        {
            /* Whole sectors. Every address this loader owns is below the first
               megabyte, so a BIOS that refuses to cross a 64 KiB boundary is
               never asked to. */
            u32 Chunk = Length / NFSLDR_SECTOR;
            if (Chunk > NFSLDR_BOUNCE_SECTORS)
                Chunk = NFSLDR_BOUNCE_SECTORS;
            if (NfldrBiosReadSectors(Sector, Chunk, Destination) != 0)
                return NFSB_EIO;
            Destination += Chunk * NFSLDR_SECTOR;
            Offset += Chunk * NFSLDR_SECTOR;
            Length -= Chunk * NFSLDR_SECTOR;
            continue;
        }

        /* A partial sector: read it whole into the bounce buffer and take the
           part that was asked for. */
        {
            u32 Take = NFSLDR_SECTOR - Skip;
            u32 Index;
            const u8 *Source;

            if (Take > Length)
                Take = Length;
            if (NfldrBiosReadSectors(Sector, 1, (void *)NFSLDR_BOUNCE) != 0)
                return NFSB_EIO;
            Source = (const u8 *)NFSLDR_BOUNCE + Skip;
            for (Index = 0; Index < Take; Index++)
                Destination[Index] = Source[Index];
            Destination += Take;
            Offset += Take;
            Length -= Take;
        }
    }

    return NFSB_OK;
}

static NFSB_RECORD *gLoaderRecord;
static u8 gHeader[NFSLDR_HEADER_MAX];

static int LoaderRead(u64 Offset, void *Buffer, u32 Length, void *Context)
{
    NFSB_VOLUME *Volume = (NFSB_VOLUME *)Context;
    NFSB_U32 Done = 0;

    if (NFSB_Read(Volume, gLoaderRecord, Offset, Buffer, Length, &Done) != NFSB_OK)
        return -1;
    return (Done == Length) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Boot manifest                                                       */
/* ------------------------------------------------------------------ */

/* A whole file into a buffer, or as much of it as fits. */
static u32 VolumeReadAll(NFSB_VOLUME *Volume, const NFSB_RECORD *Record, void *Buffer, u32 Limit)
{
    NFSB_U32 Done = 0;

    if (NFSB_Read(Volume, Record, 0, Buffer, Limit, &Done) != NFSB_OK)
        return 0;

    return Done;
}

/* FNV-1a over a file, one buffer at a time. The manifest hashes the bytes on
   the volume, not the image they are turned into: the file is what a bad write
   truncates or a stale copy replaces, and it is what the hash names. */
static int VolumeHash(NFSB_VOLUME *Volume, const NFSB_RECORD *Record,
                      void *Buffer, u32 BufferSize, u32 *Hash)
{
    NFSB_U64 Offset = 0;
    u32 Value = NFMAN_FNV1A_INIT;

    while (Offset < Record->Size)
    {
        NFSB_U32 Want = BufferSize, Done = 0;

        if ((NFSB_U64)Want > Record->Size - Offset)
            Want = (NFSB_U32)(Record->Size - Offset);

        if (NFSB_Read(Volume, Record, Offset, Buffer, Want, &Done) != NFSB_OK || Done != Want)
            return -1;

        Value = NfmanFnv1a(Value, Buffer, Done);
        Offset += Done;
    }

    *Hash = Value;
    return 0;
}

void NfldrMain(void)
{
    NFSB_IO Io;
    NFSB_HEAP Heap;
    NFSB_VOLUME *Volume = 0;
    NFSB_RECORD *Record;
    const char *Path = NFSLDR_DEFAULT_PATH;
    u8 *Header = gHeader;
    u32 Entry = 0;
    int Status;
    NFMAN *Manifest = 0;
    const NFMAN_ENTRY *Kernel = 0;

    NfldrBiosInit();
    NfldrPuts("NextFS stage 1\n");

    /* One read before the file system is asked for anything: if the signature
       is not there, the trouble is the disk path and not the format, and saying
       "corrupt volume" for a disk error costs an hour every time. */
    if (NfldrBiosReadSectors(8, 1, (void *)NFSLDR_BOUNCE) != 0)
    {
        NfldrPuts("NF: the boot disk cannot be read, drive ");
        NfldrPrintHex(NFSLDR_BIOS_DRIVE());
        NfldrPuts("\n");
        NfldrPanic("no disk");
    }
    {
        const u8 *Signature = (const u8 *)NFSLDR_BOUNCE;
        if (Signature[0] != 'N' || Signature[1] != 'F' ||
            Signature[2] != 'S' || Signature[3] != 'B')
        {
            NfldrPanic("sector 8 is not a NextFS superblock");
        }
    }

    ArenaNext = NFSLDR_ARENA_BASE;

    Io.Read = VolumeRead;
    Io.Context = 0;
    Io.DeviceBytes = 0;                 /* only the primary superblock is tried */
    Heap.Alloc = ArenaAlloc;
    Heap.Free = ArenaFree;

    Status = NFSB_Mount(&Io, &Heap, &Volume);
    if (Status != NFSB_OK)
    {
        NfldrPuts("NF: cannot mount the volume (");
        NfldrPrintDec((u32)(-Status));
        NfldrPuts(")\n");
        NfldrPanic("no NextFS volume to boot from");
    }

    NfldrPuts("NF: volume ");
    NfldrPutCounted(Volume->Super.Label, sizeof(Volume->Super.Label));
    NfldrPuts(", format ");
    NfldrPrintDec(Volume->Super.Version);
    if (Volume->DirtyAtMount)
        NfldrPuts(" (dirty: last checkpoint)");
    if (Volume->UsedFallbackRoot)
        NfldrPuts(" (previous checkpoint)");
    NfldrPuts(", ");
    NfldrPrintDec(Volume->RecordCount);
    NfldrPuts(" records\n");

    /* BOOT.MAN, when there is one, says which file is the kernel. It is read
       before NEXTLDR.CNF so that the manifest wins, and its absence is not an
       error: a volume without one boots the old way. */
    Record = (NFSB_RECORD *)NFSB_LookupPath(Volume, NFSLDR_MANIFEST_PATH);
    if (Record != 0 && Record->Size != 0 && Record->Size <= NFMAN_TEXT_MAX)
    {
        char *Text = (char *)ArenaAlloc(NFMAN_TEXT_MAX);
        NFMAN *Parsed = (NFMAN *)ArenaAlloc(sizeof(NFMAN));
        u32 Read = 0;

        if (Text != 0 && Parsed != 0)
            Read = VolumeReadAll(Volume, Record, Text, NFMAN_TEXT_MAX);

        if (Read != 0 && NfmanParse(Parsed, Text, Read))
        {
            Manifest = Parsed;
            Kernel = NfmanFind(Manifest, 0, "kernel");

            NfldrPuts("NF: ");
            NfldrPuts(NFSLDR_MANIFEST_PATH);
            NfldrPuts(" version ");
            NfldrPrintDec(Manifest->Version);
            NfldrPuts(", ");
            NfldrPrintDec(Manifest->Entries);
            NfldrPuts(" files");
            if (Manifest->DefaultOs[0] != 0)
            {
                NfldrPuts(", booting ");
                NfldrPutCounted(Manifest->DefaultOs, NFMAN_SECTION_MAX);
            }
            if (Manifest->Skipped != 0)
            {
                NfldrPuts(", ");
                NfldrPrintDec(Manifest->Skipped);
                NfldrPuts(" lines skipped");
            }
            NfldrPuts("\n");

            if (Kernel == 0)
            {
                NfldrPuts("NF: the manifest names no kernel, using the old path\n");
                Manifest = 0;
            }
            else
            {
                Path = Kernel->Path;
            }
        }
        else if (Read != 0)
        {
            NfldrPuts("NF: ");
            NfldrPuts(NFSLDR_MANIFEST_PATH);
            NfldrPuts(" is not a manifest, ignoring it\n");
        }
    }
    else if (Record != 0)
    {
        NfldrPuts("NF: ");
        NfldrPuts(NFSLDR_MANIFEST_PATH);
        NfldrPuts(" is too large to be a manifest\n");
    }

    /* A file at the root of the volume overrides where the second stage is,
       so a test image can point at another loader without touching the code. */
    Record = (NFSB_RECORD *)NFSB_LookupPath(Volume, NFSLDR_CONFIG_PATH);
    if (Record != 0)
    {
        NFSB_U32 Read = 0;
        static char Config[NFSLDR_PATH_MAX];
        u32 Index = 0, Start = 0;

        if (NFSB_Read(Volume, Record, 0, Config, NFSLDR_PATH_MAX - 1, &Read) == NFSB_OK)
        {
            Config[Read] = 0;
            while (Index < Read && (Config[Index] == ' ' || Config[Index] == '\t'))
                Index++;
            Start = Index;
            while (Index < Read && Config[Index] != '\r' && Config[Index] != '\n' && Config[Index] != 0)
                Index++;
            Config[Index] = 0;
            if (Config[Start] != 0)
                Path = &Config[Start];
        }
    }

    gLoaderRecord = (NFSB_RECORD *)NFSB_LookupPath(Volume, Path);
    if (gLoaderRecord == 0)
    {
        NfldrPuts("NF: ");
        NfldrPuts(Path);
        NfldrPuts(" is not on the volume\n");
        NfldrPanic("second stage not found");
    }

    NfldrPuts("NF: ");
    NfldrPuts(Path);
    NfldrPuts(", ");
    NfldrPrintDec((u32)gLoaderRecord->Size);
    NfldrPuts(" bytes\n");

    if (gLoaderRecord->Size > NFSLDR_HEADER_MAX)
    {
        Status = NFSB_Read(Volume, gLoaderRecord, 0, Header, NFSLDR_HEADER_MAX, 0);
        if (Status != NFSB_OK)
            NfldrPanic("cannot read the second stage's header");
    }
    else
    {
        NfldrPanic("second stage is too small to be an image");
    }

    Status = NfldrLoadPe(Header, NFSLDR_HEADER_MAX, LoaderRead, Volume,
                         gLoaderRecord->Size, &Entry);
    if (Status < 0)
    {
        NfldrPuts("NF: the image will not load (");
        NfldrPrintDec((u32)(-Status));
        NfldrPuts(")\n");
        NfldrPanic("second stage is not a loadable image");
    }

    /* The hash is checked before the jump, never after: a kernel that starts
       from a truncated image fails somewhere far harder to read than here. */
    if (Manifest != 0 && Kernel != 0 && (Kernel->Attributes & NFMAN_ATTR_HASH_STATED))
    {
        u8 *Buffer = (u8 *)ArenaAlloc(4096);
        u32 Hash = 0;

        if (Buffer == 0 || VolumeHash(Volume, gLoaderRecord, Buffer, 4096, &Hash) != 0)
            NfldrPanic("cannot hash the boot image");

        NfldrPuts("NF: image hash ");
        NfldrPrintHex(Hash);

        if (Hash != Kernel->Hash)
        {
            NfldrPuts(", the manifest says ");
            NfldrPrintHex(Kernel->Hash);
            NfldrPuts("\n");
            NfldrPanic("the boot image does not match the manifest");
        }

        NfldrPuts(", as the manifest says\n");
    }

    if (Entry < NFSLDR_PE_BASE || Entry >= NFSLDR_PE_LIMIT)
    {
        NfldrPuts("NF: the image says its entry point is ");
        NfldrPrintHex(Entry);
        NfldrPuts("\n");
        NfldrPanic("the second stage would be entered outside its own image");
    }

    NfldrPuts("NF: image at ");
    NfldrPrintHex((u32)Status);
    NfldrPuts(", entry ");
    NfldrPrintHex(Entry);
    NfldrPuts("\nStarting NextNT loader...\n");

    /* nextldr returns from every real-mode call through this address: its own
       entry point, which reloads its IDT and continues where it left off. */
    NfldrSetLoaderEntry(Entry);

    *(volatile u8 *)NFSLDR_BSS_BOOT_DRIVE = NFSLDR_BIOS_DRIVE();
    *(volatile u8 *)NFSLDR_BSS_BOOT_PARTITION = NFSLDR_BOOT_PARTITION;

    NfldrJumpToLoader(Entry, NFSLDR_BIOS_DRIVE(), NFSLDR_BOOT_PARTITION);
}

/* ------------------------------------------------------------------ */
/* The compiler's idea of the C library                               */
/* ------------------------------------------------------------------ */

void *memcpy(void *Destination, const void *Source, unsigned int Length)
{
    u8 *To = (u8 *)Destination;
    const u8 *From = (const u8 *)Source;

    while (Length--)
        *To++ = *From++;

    return Destination;
}

void *memset(void *Destination, int Value, unsigned int Length)
{
    u8 *To = (u8 *)Destination;

    while (Length--)
        *To++ = (u8)Value;

    return Destination;
}
