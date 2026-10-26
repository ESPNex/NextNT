/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     lxcore memory tests: library loading, copy-on-write file views and fork
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "lxhost.h"

#define RX (LX_PROT_READ | LX_PROT_EXEC)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define RO LX_PROT_READ
#define PAGE_DOWN(v) LX_ALIGN_DOWN((v), LX_PAGE_SIZE)
#define PAGE_UP(v) LX_ALIGN_UP((v), LX_PAGE_SIZE)

typedef struct _LX_TEST_PROC
{
    LX_PROCESS Lx;
    LX_THREAD Thread;
    MI_ADDRESS_SPACE Space;
    struct _EPROCESS_HOST EProcess;
} LX_TEST_PROC;

typedef struct _LX_TEST_FILE
{
    TEST_FILE Data;
    LX_HOST_FILE Host;
    LX_FILE File;
    HANDLE Handle;
} LX_TEST_FILE;

typedef struct _LX_TEST_SEGMENT
{
    ULONG64 Offset;
    ULONG64 VAddr;
    ULONG64 FileSize;
    ULONG64 MemSize;
    ULONG Prot;
} LX_TEST_SEGMENT;

typedef struct _LX_TEST_LAYOUT
{
    const char *Name;
    ULONG64 FileSize;
    ULONG64 Align;
    ULONG Count;
    LX_TEST_SEGMENT Segments[4];
    ULONG64 RelroVAddr;
    ULONG64 RelroSize;
} LX_TEST_LAYOUT;

static const LX_TEST_LAYOUT LibTinfo =
{
    "libtinfo.so.6.6", 0x40d70, 0x10000, 2,
    { { 0x0, 0x0, 0x30758, 0x30758, RX }, { 0x3cb50, 0x4cb50, 0x3930, 0x3d78, RW } },
    0x4cb50, 0x34b0
};

static const LX_TEST_LAYOUT LibC =
{
    "libc.so.6", 0x1b4950, 0x10000, 2,
    { { 0x0, 0x0, 0x1a8998, 0x1a8998, RX }, { 0x1ad080, 0x1bd080, 0x4458, 0x10dc0, RW } },
    0, 0
};

static const LX_TEST_LAYOUT LibGtk =
{
    "libgtk-4.so.1", 0xbcb860, 0x10000, 2,
    { { 0x0, 0x0, 0xb8bddc, 0xb8bddc, RX }, { 0xb98f60, 0xb98f60, 0x31fa0, 0x39b50, RW } },
    0xb98f60, 0x270a0
};

static const LX_TEST_LAYOUT LibLlvm =
{
    "libLLVM.so.21.1", 0x7f99a78, 0x10000, 4,
    {
        { 0x0, 0x0, 0x43cd4a0, 0x43cd4a0, RO },
        { 0x43cd4a0, 0x43dd4a0, 0x395e018, 0x395e018, RX },
        { 0x7d2b4b8, 0x7d4b4b8, 0x220630, 0x220b48, RW },
        { 0x7f4bae8, 0x7f7bae8, 0x45f90, 0xf35ca, RW },
    },
    0x7d4b4b8, 0x220b48
};

static const LX_TEST_LAYOUT LibPage4k =
{
    "libpage4k.so", 0x22000, 0x1000, 2,
    { { 0x0, 0x0, 0x1d5a0, 0x1d5a0, RX }, { 0x1ec40, 0x1fc40, 0x1830, 0x2a10, RW } },
    0, 0
};

static const LX_TEST_LAYOUT ExeBash =
{
    "bash", 0x188dd8, 0x10000, 2,
    { { 0x0, 0x0, 0x16e514, 0x16e514, RX }, { 0x17c9a0, 0x17c9a0, 0xbb30, 0x173e0, RW } },
    0, 0
};

static TEST_WORLD World;
static ULONG NextPid = 1;

static
void
ProcUse(LX_TEST_PROC *Proc)
{
    LxHostSpace = &Proc->Space;
    LxHostThread = &Proc->Thread;
    if (__atomic_load_n(&World.CpuSpace[0], __ATOMIC_SEQ_CST) != &Proc->Space)
        WorldAttach(&World, 0, &Proc->Space);
}

static
void
ProcInit(LX_TEST_PROC *Proc)
{
    ULONG Index;

    memset(Proc, 0, sizeof(*Proc));
    CHECK(NT_SUCCESS(MiAddressSpaceCreate(&World.System, &Proc->Space)));
    Proc->Lx.Pid = NextPid++;
    Proc->Lx.Mm = &Proc->Lx;
    Proc->Lx.CapEffective = ~0ULL;
    for (Index = 0; Index < RTL_NUMBER_OF(Proc->Lx.Limits); Index++)
        Proc->Lx.Limits[Index].Cur = Proc->Lx.Limits[Index].Max = LX_RLIM_INFINITY;
    Proc->Lx.Limits[LX_RLIMIT_MEMLOCK].Cur = Proc->Lx.Limits[LX_RLIMIT_MEMLOCK].Max = 8ULL * 1024 * 1024;
    InitializeListHead(&Proc->Lx.Vmas);
    KeInitializeEvent(&Proc->Lx.MmLock, SynchronizationEvent, TRUE);
    Proc->Thread.Process = &Proc->Lx;
    Proc->EProcess.Space = &Proc->Space;
    ProcUse(Proc);
}

static
void
ProcFork(LX_TEST_PROC *Parent, LX_TEST_PROC *Child)
{
    ProcInit(Child);
    ProcUse(Parent);
    LxMmLock(&Parent->Lx);
    CHECK(LxMmFork(&Child->Lx, &Parent->Lx) == 0);
    CHECK(NT_SUCCESS(MiCloneAddressSpace(&Parent->Space, &Child->Space)));
    LxMmUnlock(&Parent->Lx);
}

static
void
ProcDestroy(LX_TEST_PROC *Proc)
{
    ProcUse(Proc);
    LxMmRelease(&Proc->Lx);
    WorldAttach(&World, 0, NULL);
    ProcessDestroy(&World, &Proc->Space);
    LxHostSpace = NULL;
    LxHostThread = NULL;
}

static
void
FileOpen(LX_TEST_FILE *File, ULONG64 Size, const char *Path)
{
    memset(File, 0, sizeof(*File));
    FileCreate(&File->Data, Size);
    File->Handle = LxHostCreateFile(&File->Host, &File->Data);
    File->File.RefCount = 1;
    File->File.Type = LX_S_IFREG;
    File->File.Path = (PSTR)Path;
    File->File.Handle = File->Handle;
}

static
void
FileClose(LX_TEST_FILE *File)
{
    CHECK(File->File.RefCount == 1);
    ZwClose(File->Handle);
    FileDestroy(&File->Data);
}

static
LONG64
Map(LX_TEST_PROC *Proc, ULONG64 Address, ULONG64 Length, ULONG Prot, ULONG Flags, LX_TEST_FILE *File, ULONG64 Offset)
{
    ProcUse(Proc);
    return LxMmMap(&Proc->Lx, Address, Length, Prot, Flags, (File != NULL) ? &File->File : NULL, Offset);
}

static
LONG64
Unmap(LX_TEST_PROC *Proc, ULONG64 Address, ULONG64 Length)
{
    ProcUse(Proc);
    return LxMmUnmap(&Proc->Lx, Address, Length);
}

static
LONG64
Protect(LX_TEST_PROC *Proc, ULONG64 Address, ULONG64 Length, ULONG Prot)
{
    ProcUse(Proc);
    return LxMmProtect(&Proc->Lx, Address, Length, Prot);
}

static
LONG64
Syscall(LX_TEST_PROC *Proc, LONG64 (*Handler)(PLX_CALL), ULONG64 A0, ULONG64 A1, ULONG64 A2, ULONG64 A3)
{
    LX_CALL Call;

    ProcUse(Proc);
    memset(&Call, 0, sizeof(Call));
    Call.Thread = &Proc->Thread;
    Call.Process = &Proc->Lx;
    Call.Args[0] = A0;
    Call.Args[1] = A1;
    Call.Args[2] = A2;
    Call.Args[3] = A3;
    return Handler(&Call);
}

static
LONG
Access(LX_TEST_PROC *Proc, ULONG64 Va, void *Buffer, SIZE_T Length, BOOLEAN Write)
{
    PUCHAR Cursor = Buffer;

    ProcUse(Proc);
    while (Length != 0)
    {
        SIZE_T Chunk = min(Length, (SIZE_T)(LX_PAGE_SIZE - (Va & (LX_PAGE_SIZE - 1))));
        ULONG Tries;

        for (Tries = 0;; Tries++)
        {
            NTSTATUS Status = Write ? UserWrite(&World, 0, Va, Cursor, Chunk) : UserRead(&World, 0, Va, Cursor, Chunk);

            if (NT_SUCCESS(Status))
                break;
            Proc->Thread.FaultBus = FALSE;
            if ((Tries > 8) || !LxMmHandleFault(&Proc->Lx, Va, Write ? LX_PROT_WRITE : LX_PROT_READ))
                return Proc->Thread.FaultBus ? LX_SIGBUS : LX_SIGSEGV;
        }
        Va += Chunk;
        Cursor += Chunk;
        Length -= Chunk;
    }
    return 0;
}

static
LONG
Read8(LX_TEST_PROC *Proc, ULONG64 Va, UCHAR *Value)
{
    return Access(Proc, Va, Value, 1, FALSE);
}

static
LONG
Write8(LX_TEST_PROC *Proc, ULONG64 Va, UCHAR Value)
{
    return Access(Proc, Va, &Value, 1, TRUE);
}

static
UCHAR
FileByte(LX_TEST_FILE *File, ULONG64 Offset)
{
    return (Offset < File->Data.Size) ? File->Data.Data[Offset] : 0;
}

static
ULONG64
Frame(LX_TEST_PROC *Proc, ULONG64 Va)
{
    ULONG64 Physical = 0;
    UCHAR Value;

    if ((Read8(Proc, Va, &Value) != 0) || !MiPtTranslate(&Proc->Space, Va, &Physical, NULL))
        return 0;
    return Physical >> PAGE_SHIFT;
}

static
BOOLEAN
RangeMatchesFile(LX_TEST_PROC *Proc, ULONG64 Va, ULONG64 Length, LX_TEST_FILE *File, ULONG64 Offset)
{
    UCHAR Buffer[64];
    ULONG64 Done;

    for (Done = 0; Done < Length; Done += LX_PAGE_SIZE)
    {
        ULONG64 Probe[2] = { Done, min(Done + LX_PAGE_SIZE, Length) - sizeof(Buffer) };
        ULONG Index, i;

        for (Index = 0; Index < 2; Index++)
        {
            ULONG64 At = (Probe[Index] > Length - sizeof(Buffer)) ? Length - sizeof(Buffer) : Probe[Index];

            if (Access(Proc, Va + At, Buffer, sizeof(Buffer), FALSE) != 0)
                return FALSE;
            for (i = 0; i < sizeof(Buffer); i++)
            {
                if (Buffer[i] != FileByte(File, Offset + At + i))
                    return FALSE;
            }
        }
    }
    return TRUE;
}

static
BOOLEAN
RangeIsZero(LX_TEST_PROC *Proc, ULONG64 Va, ULONG64 Length)
{
    UCHAR Value;
    ULONG64 Done;

    for (Done = 0; Done < Length; Done += LX_PAGE_SIZE)
    {
        if ((Read8(Proc, Va + Done, &Value) != 0) || (Value != 0))
            return FALSE;
    }
    return (Read8(Proc, Va + Length - 1, &Value) == 0) && (Value == 0);
}

static
ULONG64
LoadLibrary(LX_TEST_PROC *Proc, LX_TEST_FILE *File, const LX_TEST_LAYOUT *Layout)
{
    const LX_TEST_SEGMENT *First = &Layout->Segments[0], *Last = &Layout->Segments[Layout->Count - 1];
    ULONG64 MapStart = PAGE_DOWN(First->VAddr), MapLength = Last->VAddr + Last->MemSize - MapStart;
    ULONG64 MapEnd0 = PAGE_UP(First->VAddr + First->FileSize), LoadBase, Bias;
    BOOLEAN Holes = FALSE;
    LONG64 Result;
    ULONG Index;

    if (Layout->Align > LX_PAGE_SIZE)
    {
        ULONG64 Reserve = (MapLength >= Layout->Align) ? MapLength + Layout->Align : 2 * Layout->Align;
        ULONG64 Start, Aligned, End;

        Result = Map(Proc, 0, Reserve, LX_PROT_NONE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
        CHECK(Result > 0);
        if (Result <= 0)
            return 0;
        Start = (ULONG64)Result;
        Aligned = LX_ALIGN_UP(Start, Layout->Align);
        Result = Map(Proc, Aligned, MapLength, First->Prot, LX_MAP_PRIVATE | LX_MAP_FIXED, File, PAGE_DOWN(First->Offset));
        CHECK(Result == (LONG64)Aligned);
        if (Aligned > Start)
            CHECK(Unmap(Proc, Start, Aligned - Start) == 0);
        End = PAGE_UP(Aligned + MapLength);
        if (Start + Reserve > End)
            CHECK(Unmap(Proc, End, Start + Reserve - End) == 0);
        LoadBase = Aligned;
    }
    else
    {
        Result = Map(Proc, 0, MapLength, First->Prot, LX_MAP_PRIVATE, File, PAGE_DOWN(First->Offset));
        CHECK(Result > 0);
        LoadBase = (ULONG64)Result;
    }
    Bias = LoadBase - MapStart;

    for (Index = 1; Index < Layout->Count; Index++)
    {
        if (PAGE_DOWN(Layout->Segments[Index].VAddr) != PAGE_UP(Layout->Segments[Index - 1].VAddr +
                                                                Layout->Segments[Index - 1].FileSize))
            Holes = TRUE;
    }
    if (Holes)
        CHECK(Protect(Proc, Bias + MapEnd0, PAGE_DOWN(Last->VAddr) - MapEnd0, LX_PROT_NONE) == 0);

    for (Index = 0; Index < Layout->Count; Index++)
    {
        const LX_TEST_SEGMENT *Segment = &Layout->Segments[Index];
        ULONG64 SegStart = PAGE_DOWN(Segment->VAddr), SegEnd = PAGE_UP(Segment->VAddr + Segment->FileSize);
        ULONG64 Zero = Bias + Segment->VAddr + Segment->FileSize, ZeroEnd = Bias + Segment->VAddr + Segment->MemSize;
        ULONG64 ZeroPage = PAGE_UP(Zero);

        if ((Index != 0) && (SegEnd > SegStart))
        {
            Result = Map(Proc, Bias + SegStart, SegEnd - SegStart, Segment->Prot, LX_MAP_PRIVATE | LX_MAP_FIXED, File,
                         PAGE_DOWN(Segment->Offset));
            CHECK(Result == (LONG64)(Bias + SegStart));
        }
        if (ZeroEnd > Zero)
        {
            if (ZeroEnd < ZeroPage)
                ZeroPage = ZeroEnd;
            if (ZeroPage > Zero)
            {
                PUCHAR Zeros = calloc(1, ZeroPage - Zero);

                CHECK(Access(Proc, Zero, Zeros, ZeroPage - Zero, TRUE) == 0);
                free(Zeros);
            }
            if (ZeroEnd > ZeroPage)
            {
                Result = Map(Proc, ZeroPage, ZeroEnd - ZeroPage, Segment->Prot,
                             LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_ANONYMOUS, NULL, 0);
                CHECK(Result == (LONG64)ZeroPage);
            }
        }
    }

    if (Layout->RelroSize != 0)
    {
        ULONG64 Relro = PAGE_DOWN(Bias + Layout->RelroVAddr);

        CHECK(Write8(Proc, Bias + Layout->RelroVAddr, 0x5A) == 0);
        CHECK(Protect(Proc, Relro, PAGE_DOWN(Bias + Layout->RelroVAddr + Layout->RelroSize) - Relro, RO) == 0);
    }
    return Bias;
}

static
void
CheckLibrary(LX_TEST_PROC *Proc, LX_TEST_FILE *File, const LX_TEST_LAYOUT *Layout, ULONG64 Bias)
{
    ULONG Index;

    for (Index = 0; Index < Layout->Count; Index++)
    {
        const LX_TEST_SEGMENT *Segment = &Layout->Segments[Index];
        ULONG64 Va = Bias + Segment->VAddr;
        ULONG64 Skip = (Layout->RelroSize != 0 && Segment->VAddr == Layout->RelroVAddr) ? 1 : 0;

        CHECK(RangeMatchesFile(Proc, Va + Skip, Segment->FileSize - Skip, File, Segment->Offset + Skip));
        if (Segment->MemSize > Segment->FileSize)
            CHECK(RangeIsZero(Proc, Va + Segment->FileSize, Segment->MemSize - Segment->FileSize));
        if (!(Segment->Prot & LX_PROT_WRITE))
            CHECK(Write8(Proc, Va + Segment->FileSize / 2, 0x77) == LX_SIGSEGV);
    }
}

static
ULONG64
LoadExecutable(LX_TEST_PROC *Proc, LX_TEST_FILE *File, const LX_TEST_LAYOUT *Layout, ULONG64 DynamicBase)
{
    ULONG64 Low = ~0ULL, High = 0, Bias, Base, PreviousEnd = 0;
    ULONG PreviousProt = 0, Index;
    PVOID Address;
    SIZE_T Size;

    for (Index = 0; Index < Layout->Count; Index++)
    {
        Low = min(Low, PAGE_DOWN(Layout->Segments[Index].VAddr));
        High = max(High, PAGE_UP(Layout->Segments[Index].VAddr + Layout->Segments[Index].MemSize));
    }
    Bias = DynamicBase - LX_ALIGN_DOWN(Low, LX_ALLOC_GRANULARITY);
    Base = LX_ALIGN_DOWN(Low + Bias, LX_ALLOC_GRANULARITY);
    Address = (PVOID)(ULONG_PTR)Base;
    Size = (SIZE_T)(LX_ALIGN_UP(High + Bias, LX_ALLOC_GRANULARITY) - Base);
    ProcUse(Proc);
    CHECK(NT_SUCCESS(ZwAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_RESERVE, PAGE_NOACCESS)));

    for (Index = 0; Index < Layout->Count; Index++)
    {
        const LX_TEST_SEGMENT *Segment = &Layout->Segments[Index];
        ULONG64 Start = PAGE_DOWN(Segment->VAddr + Bias);
        ULONG64 FileEnd = PAGE_UP(Segment->VAddr + Segment->FileSize + Bias);
        ULONG64 End = PAGE_UP(Segment->VAddr + Segment->MemSize + Bias);

        CHECK(Map(Proc, Start, FileEnd - Start, Segment->Prot, LX_MAP_PRIVATE | LX_MAP_FIXED, File,
                  PAGE_DOWN(Segment->Offset)) == (LONG64)Start);
        if (End > FileEnd)
            CHECK(Map(Proc, FileEnd, End - FileEnd, Segment->Prot, LX_MAP_PRIVATE | LX_MAP_FIXED, NULL, 0) == (LONG64)FileEnd);
        if (Segment->MemSize > Segment->FileSize)
        {
            ProcUse(Proc);
            CHECK(LxMmZero(&Proc->Lx, Segment->VAddr + Segment->FileSize + Bias, FileEnd) == 0);
        }
        if (Start < PreviousEnd)
            Protect(Proc, Start, LX_PAGE_SIZE, Segment->Prot | PreviousProt);
        PreviousEnd = End;
        PreviousProt = Segment->Prot;
    }
    return Bias;
}

static
ULONG64
PrivatePages(LX_TEST_PROC *Proc)
{
    return (ULONG64)MI_ATOMIC_READ64(&Proc->Space.CommittedPages);
}

static
ULONG64
WritablePages(const LX_TEST_LAYOUT *Layout)
{
    ULONG64 Pages = 0;
    ULONG Index;

    for (Index = 0; Index < Layout->Count; Index++)
    {
        if (Layout->Segments[Index].Prot & LX_PROT_WRITE)
            Pages += (PAGE_UP(Layout->Segments[Index].VAddr + Layout->Segments[Index].MemSize) -
                      PAGE_DOWN(Layout->Segments[Index].VAddr)) / LX_PAGE_SIZE;
    }
    return Pages;
}

static
void
LoadsAndShares(const LX_TEST_LAYOUT *Layout)
{
    LX_TEST_PROC First, Second;
    LX_TEST_FILE File;
    ULONG64 Bias1, Bias2, Text;
    ULONG Logged = LxHostLogged;

    FileOpen(&File, Layout->FileSize, Layout->Name);
    ProcInit(&First);
    Bias1 = LoadLibrary(&First, &File, Layout);
    CheckLibrary(&First, &File, Layout, Bias1);
    ProcInit(&Second);
    Bias2 = LoadLibrary(&Second, &File, Layout);
    CheckLibrary(&Second, &File, Layout, Bias2);

    Text = Layout->Segments[0].VAddr + 2 * LX_PAGE_SIZE;
    CHECK(Frame(&First, Bias1 + Text) != 0 && Frame(&First, Bias1 + Text) == Frame(&Second, Bias2 + Text));
    if (Layout->Align > LX_PAGE_SIZE)
    {
        CHECK(PrivatePages(&First) <= WritablePages(Layout) + 8);
        CHECK(LxHostLogged == Logged);
    }
    if (TestFailures != 0)
        fprintf(stderr, "  layout %s: committed %llu pages, writable %llu\n", Layout->Name,
                (unsigned long long)PrivatePages(&First), (unsigned long long)WritablePages(Layout));

    ProcDestroy(&Second);
    ProcDestroy(&First);
    FileClose(&File);
}

static
void
ExecutableLoads(void)
{
    LX_TEST_PROC Proc;
    LX_TEST_FILE File;
    ULONG64 Bias;

    FileOpen(&File, ExeBash.FileSize, ExeBash.Name);
    ProcInit(&Proc);
    Bias = LoadExecutable(&Proc, &File, &ExeBash, 0x40000000);
    CheckLibrary(&Proc, &File, &ExeBash, Bias);
    CHECK(PrivatePages(&Proc) <= WritablePages(&ExeBash) + 8);
    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
ForkSharesAndIsolates(void)
{
    LX_TEST_PROC Parent, Child, Other;
    LX_TEST_FILE Tinfo, Libc;
    ULONG64 TinfoBias, LibcBias, OtherBias, Data, Text;
    UCHAR Value = 0;

    FileOpen(&Tinfo, LibTinfo.FileSize, LibTinfo.Name);
    FileOpen(&Libc, LibC.FileSize, LibC.Name);
    ProcInit(&Parent);
    TinfoBias = LoadLibrary(&Parent, &Tinfo, &LibTinfo);
    LibcBias = LoadLibrary(&Parent, &Libc, &LibC);
    Data = LibcBias + LibC.Segments[1].VAddr + 0x100;
    Text = LibcBias + 5 * LX_PAGE_SIZE;
    CHECK(Write8(&Parent, Data, 0x11) == 0);

    ProcFork(&Parent, &Child);
    CHECK(Read8(&Child, Data, &Value) == 0 && Value == 0x11);
    CHECK(Write8(&Child, Data, 0x22) == 0);
    CHECK(Read8(&Parent, Data, &Value) == 0 && Value == 0x11);
    CHECK(Write8(&Parent, Data, 0x33) == 0);
    CHECK(Read8(&Child, Data, &Value) == 0 && Value == 0x22);
    CHECK(Frame(&Parent, Text) == Frame(&Child, Text));
    CheckLibrary(&Child, &Tinfo, &LibTinfo, TinfoBias);

    ProcInit(&Other);
    OtherBias = LoadLibrary(&Other, &Libc, &LibC);
    CHECK(Frame(&Other, OtherBias + 5 * LX_PAGE_SIZE) == Frame(&Parent, Text));

    ProcDestroy(&Child);
    CHECK(Read8(&Parent, Data, &Value) == 0 && Value == 0x33);
    CheckLibrary(&Parent, &Tinfo, &LibTinfo, TinfoBias);
    ProcDestroy(&Other);
    ProcDestroy(&Parent);
    FileClose(&Libc);
    FileClose(&Tinfo);
}

static
void
FixedOverViews(void)
{
    LX_TEST_PROC Proc;
    LX_TEST_FILE File;
    ULONG64 Base;
    UCHAR Value;

    FileOpen(&File, 0x100000, "data.bin");
    ProcInit(&Proc);
    Base = (ULONG64)Map(&Proc, 0, 0x100000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK((LONG64)Base > 0);
    CHECK(RangeMatchesFile(&Proc, Base, 0x100000, &File, 0));

    CHECK(Map(&Proc, Base + 0x48000, 0x10000, RW, LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_ANONYMOUS, NULL, 0) ==
          (LONG64)(Base + 0x48000));
    CHECK(RangeIsZero(&Proc, Base + 0x48000, 0x10000));
    CHECK(Write8(&Proc, Base + 0x48010, 0x99) == 0);
    CHECK(RangeMatchesFile(&Proc, Base, 0x48000, &File, 0));
    CHECK(RangeMatchesFile(&Proc, Base + 0x58000, 0xA8000, &File, 0x58000));

    CHECK(Map(&Proc, Base + 0x80000, 0x20000, RO, LX_MAP_PRIVATE | LX_MAP_FIXED, &File, 0x10000) ==
          (LONG64)(Base + 0x80000));
    CHECK(RangeMatchesFile(&Proc, Base + 0x80000, 0x20000, &File, 0x10000));
    CHECK(RangeMatchesFile(&Proc, Base + 0xA0000, 0x60000, &File, 0xA0000));

    CHECK(Unmap(&Proc, Base + 0xC0000, 0x8000) == 0);
    CHECK(Read8(&Proc, Base + 0xC4000, &Value) == LX_SIGSEGV);
    CHECK(RangeMatchesFile(&Proc, Base + 0xC8000, 0x38000, &File, 0xC8000));
    CHECK(Read8(&Proc, Base + 0x48010, &Value) == 0 && Value == 0x99);

    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
WritableCopyAndDiscard(void)
{
    LX_TEST_PROC Proc, Other;
    LX_TEST_FILE File;
    ULONG64 Base, OtherBase;
    UCHAR Value = 0, Original;

    FileOpen(&File, 0x40000, "cow.bin");
    Original = FileByte(&File, 0x1010);
    ProcInit(&Proc);
    ProcInit(&Other);
    Base = (ULONG64)Map(&Proc, 0, 0x40000, RO, LX_MAP_PRIVATE, &File, 0);
    OtherBase = (ULONG64)Map(&Other, 0, 0x40000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK(Write8(&Proc, Base + 0x1010, 0x42) == LX_SIGSEGV);
    CHECK(Protect(&Proc, Base, 0x2000, RW) == 0);
    CHECK(Write8(&Proc, Base + 0x1010, 0x42) == 0);
    CHECK(Read8(&Proc, Base + 0x1010, &Value) == 0 && Value == 0x42);
    CHECK(FileByte(&File, 0x1010) == Original);
    CHECK(Read8(&Other, OtherBase + 0x1010, &Value) == 0 && Value == Original);
    CHECK(Frame(&Proc, Base + 0x8000) == Frame(&Other, OtherBase + 0x8000));
    CHECK(Syscall(&Proc, LxSysMadvise, Base, 0x2000, LX_MADV_DONTNEED, 0) == 0);
    CHECK(Read8(&Proc, Base + 0x1010, &Value) == 0 && Value == Original);
    ProcDestroy(&Other);
    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
ProtectUpgradeKeepsLayout(void)
{
    LX_TEST_PROC Proc, Other, Child;
    LX_TEST_FILE File;
    ULONG64 Base, OtherBase, Tables;
    UCHAR Value = 0;

    FileOpen(&File, 0x40000, "upgrade.bin");
    ProcInit(&Proc);
    ProcInit(&Other);
    Base = (ULONG64)Map(&Proc, 0, 0x40000, RO, LX_MAP_PRIVATE, &File, 0);
    OtherBase = (ULONG64)Map(&Other, 0, 0x40000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK(RangeMatchesFile(&Proc, Base, 0x40000, &File, 0));
    Tables = (ULONG64)MI_ATOMIC_READ64(&Proc.Space.PageTablePages);
    CHECK(Protect(&Proc, Base + 0x30000, 0x10000, LX_PROT_NONE) == 0);
    CHECK(Protect(&Proc, Base + 0x10000, 0x10000, RX) == 0);
    CHECK(RangeMatchesFile(&Proc, Base, 0x30000, &File, 0));
    CHECK(Read8(&Proc, Base + 0x30000, &Value) == LX_SIGSEGV);
    CHECK(Write8(&Proc, Base + 0x10000, 0x42) == LX_SIGSEGV);
    CHECK(Write8(&Proc, Base, 0x42) == LX_SIGSEGV);
    CHECK(Frame(&Proc, Base + 0x10000) == Frame(&Other, OtherBase + 0x10000));
    CHECK(Protect(&Proc, Base + 0x20000, 0x1000, RW) == 0);
    CHECK(Write8(&Proc, Base + 0x20010, 0x42) == 0);
    CHECK(Read8(&Other, OtherBase + 0x20010, &Value) == 0 && Value == FileByte(&File, 0x20010));
    CHECK(Read8(&Proc, Base + 0x30000, &Value) == LX_SIGSEGV);
    CHECK(Frame(&Proc, Base + 0x8000) == Frame(&Other, OtherBase + 0x8000));

    ProcFork(&Proc, &Child);
    CHECK(Read8(&Child, Base + 0x20010, &Value) == 0 && Value == 0x42);
    CHECK(RangeMatchesFile(&Child, Base, 0x20000, &File, 0));
    CHECK(Read8(&Child, Base + 0x30000, &Value) == LX_SIGSEGV);
    CHECK(Write8(&Child, Base + 0x10000, 0x43) == LX_SIGSEGV);
    CHECK(Frame(&Child, Base + 0x8000) == Frame(&Other, OtherBase + 0x8000));
    CHECK((ULONG64)MI_ATOMIC_READ64(&Child.Space.PageTablePages) <= Tables + 2);
    ProcDestroy(&Child);
    ProcDestroy(&Other);
    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
ForkDirtyFileView(void)
{
    LX_TEST_PROC Parent, Child;
    LX_TEST_FILE File;
    ULONG64 Base;
    UCHAR Value = 0, Original;

    FileOpen(&File, 0x40000, "dirty.bin");
    Original = FileByte(&File, 0x8010);
    ProcInit(&Parent);
    Base = (ULONG64)Map(&Parent, 0, 0x40000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK(Protect(&Parent, Base, 0x2000, RW) == 0);
    CHECK(Write8(&Parent, Base + 0x10, 0x11) == 0);
    ProcFork(&Parent, &Child);
    CHECK(Read8(&Child, Base + 0x10, &Value) == 0 && Value == 0x11);
    CHECK(Write8(&Child, Base + 0x10, 0x22) == 0);
    CHECK(Read8(&Parent, Base + 0x10, &Value) == 0 && Value == 0x11);
    CHECK(Write8(&Parent, Base + 0x10, 0x33) == 0);
    CHECK(Read8(&Child, Base + 0x10, &Value) == 0 && Value == 0x22);
    CHECK(Read8(&Child, Base + 0x8010, &Value) == 0 && Value == Original);
    CHECK(Frame(&Parent, Base + 0x8000) == Frame(&Child, Base + 0x8000));
    CHECK(Write8(&Child, Base + 0x8010, 0x44) == LX_SIGSEGV);
    CHECK(Unmap(&Child, Base, 0x40000) == 0);
    ProcDestroy(&Child);
    CHECK(Read8(&Parent, Base + 0x10, &Value) == 0 && Value == 0x33);
    CHECK(Write8(&Parent, Base + 0x11, 0x55) == 0);
    CHECK(FileByte(&File, 0x10) != 0x33 || Original == 0x33);
    ProcDestroy(&Parent);
    FileClose(&File);
}

static
void
RemapKeepsCopies(void)
{
    LX_TEST_PROC Proc;
    LX_TEST_FILE File;
    ULONG64 Base;
    LONG64 Moved;
    UCHAR Value = 0;

    FileOpen(&File, 0x80000, "remap.bin");
    ProcInit(&Proc);
    Base = (ULONG64)Map(&Proc, 0, 0x10000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK(Protect(&Proc, Base, 0x10000, RW) == 0);
    CHECK(Write8(&Proc, Base + 0x20, 0x5C) == 0);
    Moved = Syscall(&Proc, LxSysMremap, Base, 0x10000, 0x40000, LX_MREMAP_MAYMOVE);
    CHECK(Moved > 0);
    if (Moved > 0)
    {
        CHECK(Read8(&Proc, (ULONG64)Moved + 0x20, &Value) == 0 && Value == 0x5C);
        CHECK(RangeMatchesFile(&Proc, (ULONG64)Moved + 0x10000, 0x30000, &File, 0x10000));
    }
    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
BeyondEndOfFile(void)
{
    LX_TEST_PROC Proc;
    LX_TEST_FILE File;
    ULONG64 Base;
    UCHAR Value;

    FileOpen(&File, 0x1800, "short.bin");
    ProcInit(&Proc);
    Base = (ULONG64)Map(&Proc, 0, 0x10000, RO, LX_MAP_PRIVATE, &File, 0);
    CHECK((LONG64)Base > 0);
    CHECK(Read8(&Proc, Base + 0x17FF, &Value) == 0 && Value == FileByte(&File, 0x17FF));
    CHECK(Read8(&Proc, Base + 0x1900, &Value) == 0 && Value == 0);
    CHECK(Read8(&Proc, Base + 0x2000, &Value) == LX_SIGBUS);
    ProcDestroy(&Proc);
    FileClose(&File);
}

static
void
HeapFaultCost(void)
{
    LX_TEST_PROC Proc;
    ULONG64 Base, Va, Pages = (64ULL << 20) / LX_PAGE_SIZE, Queries, Scanned;

    ProcInit(&Proc);
    Base = (ULONG64)Map(&Proc, 0, 64ULL << 20, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK((LONG64)Base > 0);
    Queries = LxHostQueries;
    Scanned = LxHostCommittedScan;
    for (Va = Base; Va < Base + (64ULL << 20); Va += LX_PAGE_SIZE)
        CHECK(Write8(&Proc, Va, 1) == 0);
    Queries = LxHostQueries - Queries;
    Scanned = LxHostCommittedScan - Scanned;
    CHECK(Queries < Pages);
    CHECK(Scanned < 64 * (Pages / (LX_ALLOC_GRANULARITY / LX_PAGE_SIZE)));
    if ((Queries >= Pages) || (Scanned >= 64 * (Pages / (LX_ALLOC_GRANULARITY / LX_PAGE_SIZE))))
        fprintf(stderr, "  heap: %llu queries, %llu committed pages scanned for %llu pages\n",
                (unsigned long long)Queries, (unsigned long long)Scanned, (unsigned long long)Pages);
    ProcDestroy(&Proc);
}

static
void
ForkCost(void)
{
    LX_TEST_PROC Parent, Child;
    ULONG64 Size = 128ULL << 20, Pages = Size / LX_PAGE_SIZE, Base, Va, Copies;
    double Start, Clone, Exit, Rewrite;

    ProcInit(&Parent);
    Base = (ULONG64)Map(&Parent, 0, Size, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK((LONG64)Base > 0);
    for (Va = Base; Va < Base + Size; Va += LX_PAGE_SIZE)
        CHECK(Write8(&Parent, Va, 1) == 0);

    if (getenv("LXMM_FORK_LOOP") != NULL)
    {
        int Loop = atoi(getenv("LXMM_FORK_LOOP"));

        while (Loop-- > 0)
        {
            ProcFork(&Parent, &Child);
            ProcDestroy(&Child);
            for (Va = Base; Va < Base + Size; Va += LX_PAGE_SIZE)
                Write8(&Parent, Va, 1);
        }
    }
    Start = NowSeconds();
    ProcFork(&Parent, &Child);
    Clone = NowSeconds() - Start;
    CHECK(Write8(&Child, Base, 2) == 0);
    Start = NowSeconds();
    ProcDestroy(&Child);
    Exit = NowSeconds() - Start;

    Copies = (ULONG64)MI_ATOMIC_READ64(&Parent.Space.CopyOnWriteFaults);
    Start = NowSeconds();
    for (Va = Base; Va < Base + Size; Va += LX_PAGE_SIZE)
        CHECK(Write8(&Parent, Va, 3) == 0);
    Rewrite = NowSeconds() - Start;
    Copies = (ULONG64)MI_ATOMIC_READ64(&Parent.Space.CopyOnWriteFaults) - Copies;
    CHECK(Copies == 0);
    fprintf(stderr, "  fork %llu pages: clone %.1f ms (%.0f ns/page), child exit %.1f ms, parent rewrite %.1f ms, "
            "%llu copies\n", (unsigned long long)Pages, Clone * 1000, Clone * 1e9 / Pages, Exit * 1000,
            Rewrite * 1000, (unsigned long long)Copies);
    ProcDestroy(&Parent);
}

static
void
ExecLoadCost(void)
{
    LX_TEST_PROC Proc;
    LX_TEST_FILE Bash, Libc, Tinfo;
    ULONG Round, Rounds = 16;
    ULONG64 Queries;
    double Start, Load = 0;

    if (getenv("LXMM_LOAD_ROUNDS") != NULL)
        Rounds = (ULONG)atoi(getenv("LXMM_LOAD_ROUNDS"));
    FileOpen(&Bash, ExeBash.FileSize, ExeBash.Name);
    FileOpen(&Libc, LibC.FileSize, LibC.Name);
    FileOpen(&Tinfo, LibTinfo.FileSize, LibTinfo.Name);
    Queries = LxHostQueries;
    for (Round = 0; Round < Rounds; Round++)
    {
        ULONG64 Bias;

        ProcInit(&Proc);
        Start = NowSeconds();
        Bias = LoadExecutable(&Proc, &Bash, &ExeBash, 0x40000000);
        LoadLibrary(&Proc, &Tinfo, &LibTinfo);
        LoadLibrary(&Proc, &Libc, &LibC);
        Load += NowSeconds() - Start;
        if (Round == 0)
            CheckLibrary(&Proc, &Bash, &ExeBash, Bias);
        ProcDestroy(&Proc);
    }
    fprintf(stderr, "  exec bash + tinfo + libc: %.1f us per load, %.1f queries per load\n", Load * 1e6 / Rounds,
            (double)(LxHostQueries - Queries) / Rounds);
    FileClose(&Tinfo);
    FileClose(&Libc);
    FileClose(&Bash);
}

static
void
ForkSmallProcessCost(void)
{
    LX_TEST_PROC Parent, Child;
    ULONG64 Size = 6ULL << 20, Pages = Size / LX_PAGE_SIZE, Base, Copies = 0, Seed = 1;
    double Start, Clone = 0, Writes = 0, Exit = 0, Rewrite = 0;
    ULONG Round, Index, Rounds = 32;

    if (getenv("LXMM_SMALL_ROUNDS") != NULL)
        Rounds = (ULONG)atoi(getenv("LXMM_SMALL_ROUNDS"));
    ProcInit(&Parent);
    Base = (ULONG64)Map(&Parent, 0, Size, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK((LONG64)Base > 0);
    for (Index = 0; Index < Pages; Index++)
        CHECK(Write8(&Parent, Base + Index * LX_PAGE_SIZE, 1) == 0);

    for (Round = 0; Round < Rounds; Round++)
    {
        Start = NowSeconds();
        ProcFork(&Parent, &Child);
        Clone += NowSeconds() - Start;
        Start = NowSeconds();
        for (Index = 0; Index < 100; Index++)
        {
            Seed = Seed * 6364136223846793005ULL + 1442695040888963407ULL;
            CHECK(Write8(&Child, Base + ((Seed >> 33) % Pages) * LX_PAGE_SIZE, 2) == 0);
        }
        Writes += NowSeconds() - Start;
        Start = NowSeconds();
        ProcDestroy(&Child);
        Exit += NowSeconds() - Start;
        Copies += (ULONG64)MI_ATOMIC_READ64(&Child.Space.CopyOnWriteFaults);
        Start = NowSeconds();
        for (Index = 0; Index < 300; Index++)
        {
            Seed = Seed * 6364136223846793005ULL + 1442695040888963407ULL;
            CHECK(Write8(&Parent, Base + ((Seed >> 33) % Pages) * LX_PAGE_SIZE, 3) == 0);
        }
        Rewrite += NowSeconds() - Start;
    }
    fprintf(stderr, "  fork %llu-page process: clone %.1f us, child 100 writes %.1f us, child exit %.1f us, "
            "parent 300 writes %.1f us, %.1f child copies per fork\n", (unsigned long long)Pages,
            Clone * 1e6 / Rounds, Writes * 1e6 / Rounds, Exit * 1e6 / Rounds, Rewrite * 1e6 / Rounds,
            (double)Copies / Rounds);
    ProcDestroy(&Parent);
}

static
void
ForkLibrariesCost(void)
{
    static const LX_TEST_LAYOUT *Layouts[] = { &LibC, &LibTinfo, &LibGtk, &LibLlvm };
    LX_TEST_PROC Parent, Child;
    LX_TEST_FILE Files[4];
    ULONG64 Bias[4], Copies;
    LONG64 ChildTables = 0;
    double Start, Vmas = 0, Clone = 0, Reset = 0, Exit = 0;
    ULONG Index, Segment, Round, Rounds = 8;

    if (getenv("LXMM_LIBS_ROUNDS") != NULL)
        Rounds = (ULONG)atoi(getenv("LXMM_LIBS_ROUNDS"));
    ProcInit(&Parent);
    for (Index = 0; Index < 4; Index++)
    {
        const LX_TEST_LAYOUT *Layout = Layouts[Index];

        FileOpen(&Files[Index], Layout->FileSize, Layout->Name);
        Bias[Index] = LoadLibrary(&Parent, &Files[Index], Layout);
        for (Segment = 0; Segment < Layout->Count; Segment++)
        {
            const LX_TEST_SEGMENT *Seg = &Layout->Segments[Segment];
            ULONG64 Va, Step = (Seg->Prot & LX_PROT_WRITE) ? LX_PAGE_SIZE : 4 * LX_PAGE_SIZE;
            UCHAR Value;

            for (Va = PAGE_DOWN(Bias[Index] + Seg->VAddr); Va < Bias[Index] + Seg->VAddr + Seg->MemSize; Va += Step)
            {
                ULONG64 Touch = (Va < Bias[Index] + Seg->VAddr) ? Bias[Index] + Seg->VAddr : Va;

                CHECK(Read8(&Parent, Touch, &Value) == 0);
                if ((Seg->Prot & LX_PROT_WRITE) &&
                    ((Layout->RelroSize == 0) || (Touch >= PAGE_DOWN(Bias[Index] + Layout->RelroVAddr + Layout->RelroSize))))
                    CHECK(Write8(&Parent, Touch, Value) == 0);
            }
        }
    }

    for (Round = 0; Round < Rounds; Round++)
    {
        ProcInit(&Child);
        ProcUse(&Parent);
        LxMmLock(&Parent.Lx);
        Start = NowSeconds();
        CHECK(LxMmFork(&Child.Lx, &Parent.Lx) == 0);
        Vmas += NowSeconds() - Start;
        Start = NowSeconds();
        CHECK(NT_SUCCESS(MiCloneAddressSpace(&Parent.Space, &Child.Space)));
        Clone += NowSeconds() - Start;
        ChildTables = MI_ATOMIC_READ64(&Child.Space.PageTablePages);
        LxMmUnlock(&Parent.Lx);
        if (Round == 0)
        {
            for (Index = 0; Index < 4; Index++)
                CheckLibrary(&Child, &Files[Index], Layouts[Index], Bias[Index]);
        }
        ProcUse(&Child);
        Start = NowSeconds();
        LxMmReset(&Child.Lx);
        Reset += NowSeconds() - Start;
        if (Round == 0)
        {
            MEMORY_BASIC_INFORMATION Information;
            ULONG64 Cursor = 0;

            while (NT_SUCCESS(ZwQueryVirtualMemory(NtCurrentProcess(), (PVOID)(ULONG_PTR)Cursor, MemoryBasicInformation,
                                                   &Information, sizeof(Information), NULL)) &&
                   (Cursor < LX_HOST_USER_LIMIT))
            {
                CHECK(Information.State == MEM_FREE);
                if ((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize <= Cursor)
                    break;
                Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
            }
            CHECK(Child.Space.VadRoot.NodeCount == 0);
        }
        Start = NowSeconds();
        ProcDestroy(&Child);
        Exit += NowSeconds() - Start;
    }
    Copies = (ULONG64)MI_ATOMIC_READ64(&Parent.Space.CopyOnWriteFaults);
    for (Index = 0; Index < 4; Index++)
        CheckLibrary(&Parent, &Files[Index], Layouts[Index], Bias[Index]);
    fprintf(stderr, "  fork with libraries: vmas %.3f ms, clone %.3f ms, exec reset %.3f ms, exit %.3f ms per fork, "
            "%llu parent copies, %lld parent page tables, %lld child page tables\n", Vmas * 1000 / Rounds,
            Clone * 1000 / Rounds, Reset * 1000 / Rounds, Exit * 1000 / Rounds, (unsigned long long)Copies,
            (long long)MI_ATOMIC_READ64(&Parent.Space.PageTablePages), (long long)ChildTables);
    ProcDestroy(&Parent);
    for (Index = 0; Index < 4; Index++)
        FileClose(&Files[Index]);
}

static
ULONG
VmaCount(LX_TEST_PROC *Proc)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = Proc->Lx.Vmas.Flink; Entry != &Proc->Lx.Vmas; Entry = Entry->Flink)
        Count++;
    return Count;
}

static
ULONG64
VmaBytes(LX_TEST_PROC *Proc)
{
    PLIST_ENTRY Entry;
    ULONG64 Bytes = 0;

    for (Entry = Proc->Lx.Vmas.Flink; Entry != &Proc->Lx.Vmas; Entry = Entry->Flink)
        Bytes += CONTAINING_RECORD(Entry, LX_VMA, Link)->End - CONTAINING_RECORD(Entry, LX_VMA, Link)->Start;
    return Bytes;
}

static
ULONG
ResidentPages(LX_TEST_PROC *Proc, ULONG64 Vector, ULONG64 Base, ULONG Pages)
{
    ULONG Index, Count = 0;
    UCHAR Value = 0;

    ProcUse(Proc);
    CHECK(LxMmMincore(&Proc->Lx, Base, Base + Pages * LX_PAGE_SIZE, Vector) == 0);
    for (Index = 0; Index < Pages; Index++)
    {
        CHECK(Read8(Proc, Vector + Index, &Value) == 0);
        Count += Value & 1;
    }
    return Count;
}

static
void
LockSemantics(void)
{
    LX_TEST_PROC Proc, Child;
    ULONG64 Vector, Private, Shared, Base, Bytes;
    ULONG Vmas, Index;

    ProcInit(&Proc);
    Vector = (ULONG64)Map(&Proc, 0, LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    Private = (ULONG64)Map(&Proc, 0, 8 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    Shared = (ULONG64)Map(&Proc, 0, 8 * LX_PAGE_SIZE, LX_PROT_WRITE, LX_MAP_SHARED | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK(((LONG64)Vector > 0) && ((LONG64)Private > 0) && ((LONG64)Shared > 0));
    CHECK(Write8(&Proc, Vector, 0) == 0);
    Vmas = VmaCount(&Proc);

    CHECK(ResidentPages(&Proc, Vector, Private, 8) == 0);
    CHECK(Syscall(&Proc, LxSysMlock, Private, LX_PAGE_SIZE + 1, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 2 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Private, 8) == 2);
    CHECK(Syscall(&Proc, LxSysMlock2, Private, 2 * LX_PAGE_SIZE, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 2 * LX_PAGE_SIZE);
    CHECK(Syscall(&Proc, LxSysMsync, Private, LX_PAGE_SIZE, LX_MS_INVALIDATE, 0) == -LX_EBUSY);
    CHECK(Syscall(&Proc, LxSysMadvise, Private, LX_PAGE_SIZE, LX_MADV_DONTNEED, 0) == -LX_EINVAL);
    CHECK(Syscall(&Proc, LxSysMunlock, Private, 8 * LX_PAGE_SIZE, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);
    CHECK(VmaCount(&Proc) == Vmas);
    Base = (ULONG64)Map(&Proc, 0, LX_PAGE_SIZE, RO, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK(Unmap(&Proc, Base, LX_PAGE_SIZE) == 0);
    CHECK(Syscall(&Proc, LxSysMlock, Base, LX_PAGE_SIZE, 0, 0) == -LX_ENOMEM);
    CHECK(Syscall(&Proc, LxSysMlock2, Private, LX_PAGE_SIZE, 2, 0) == -LX_EINVAL);

    for (Index = 0; Index < 4; Index++)
        CHECK(Write8(&Proc, Shared + Index * LX_PAGE_SIZE, 1) == 0);
    CHECK(ResidentPages(&Proc, Vector, Shared, 8) == 4);
    CHECK(Syscall(&Proc, LxSysMlock2, Shared, 8 * LX_PAGE_SIZE, LX_MLOCK_ONFAULT, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 8 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Shared, 8) == 4);
    CHECK(Syscall(&Proc, LxSysMlock2, Shared, 8 * LX_PAGE_SIZE, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 8 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Shared, 8) == 8);
    CHECK(Syscall(&Proc, LxSysMunlockall, 0, 0, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);

    Proc.Lx.CapEffective = 0;
    Proc.Lx.Limits[LX_RLIMIT_MEMLOCK].Cur = LX_PAGE_SIZE - 1;
    CHECK(Syscall(&Proc, LxSysMlock, Private, LX_PAGE_SIZE, 0, 0) == -LX_ENOMEM);
    Proc.Lx.Limits[LX_RLIMIT_MEMLOCK].Cur = 0;
    CHECK(Syscall(&Proc, LxSysMlock, Private, LX_PAGE_SIZE, 0, 0) == -LX_EPERM);
    CHECK(Map(&Proc, 0, LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_LOCKED, NULL, 0) == -LX_EPERM);
    CHECK(Syscall(&Proc, LxSysMlockall, LX_MCL_CURRENT, 0, 0, 0) == -LX_EPERM);
    Proc.Lx.Limits[LX_RLIMIT_MEMLOCK].Cur = 2 * LX_PAGE_SIZE;
    CHECK(Map(&Proc, 0, 4 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_LOCKED, NULL, 0) ==
          -LX_EAGAIN);
    CHECK(Syscall(&Proc, LxSysMlockall, LX_MCL_CURRENT, 0, 0, 0) == -LX_ENOMEM);
    CHECK(Syscall(&Proc, LxSysMlock, Private, 2 * LX_PAGE_SIZE, 0, 0) == 0);
    CHECK(Syscall(&Proc, LxSysMunlock, Private, 2 * LX_PAGE_SIZE, 0, 0) == 0);
    Proc.Lx.CapEffective = ~0ULL;
    Proc.Lx.Limits[LX_RLIMIT_MEMLOCK].Cur = 8ULL * 1024 * 1024;

    Base = (ULONG64)Map(&Proc, 0, 8 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK((LONG64)Base > 0);
    CHECK(Syscall(&Proc, LxSysMlock, Base, 8 * LX_PAGE_SIZE, 0, 0) == 0);
    CHECK(Unmap(&Proc, Base + 2 * LX_PAGE_SIZE, 4 * LX_PAGE_SIZE) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 4 * LX_PAGE_SIZE);
    CHECK(Syscall(&Proc, LxSysMunlock, Base + 2 * LX_PAGE_SIZE, 8 * LX_PAGE_SIZE, 0, 0) == -LX_ENOMEM);
    CHECK(Unmap(&Proc, Base, 8 * LX_PAGE_SIZE) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);

    Base = (ULONG64)Map(&Proc, 0, 4 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_LOCKED, NULL, 0);
    CHECK((LONG64)Base > 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 4 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Base, 4) == 4);
    ProcFork(&Proc, &Child);
    CHECK(LxMmLockedBytes(&Child.Lx) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 4 * LX_PAGE_SIZE);
    ProcDestroy(&Child);
    CHECK(Unmap(&Proc, Base, 4 * LX_PAGE_SIZE) == 0);

    CHECK(Syscall(&Proc, LxSysMlockall, LX_MCL_ONFAULT, 0, 0, 0) == -LX_EINVAL);
    CHECK(Syscall(&Proc, LxSysMlockall, 8, 0, 0, 0) == -LX_EINVAL);
    CHECK(Syscall(&Proc, LxSysMlockall, LX_MCL_CURRENT | LX_MCL_FUTURE, 0, 0, 0) == 0);
    Bytes = VmaBytes(&Proc);
    CHECK(LxMmLockedBytes(&Proc.Lx) == Bytes);
    CHECK(ResidentPages(&Proc, Vector, Shared, 8) == 8);
    Base = (ULONG64)Map(&Proc, 0, 4 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK((LONG64)Base > 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == Bytes + 4 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Base, 4) == 4);
    CHECK(Syscall(&Proc, LxSysMunlockall, 0, 0, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);
    CHECK(Unmap(&Proc, Base, 4 * LX_PAGE_SIZE) == 0);
    Base = (ULONG64)Map(&Proc, 0, 4 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);
    CHECK(ResidentPages(&Proc, Vector, Base, 4) == 0);
    CHECK(Syscall(&Proc, LxSysMlockall, LX_MCL_FUTURE | LX_MCL_ONFAULT, 0, 0, 0) == 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 0);
    CHECK(Unmap(&Proc, Base, 4 * LX_PAGE_SIZE) == 0);
    Base = (ULONG64)Map(&Proc, 0, 4 * LX_PAGE_SIZE, RW, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0);
    CHECK(LxMmLockedBytes(&Proc.Lx) == 4 * LX_PAGE_SIZE);
    CHECK(ResidentPages(&Proc, Vector, Base, 4) == 0);
    LxMmReset(&Proc.Lx);
    CHECK(Proc.Lx.MmLockDefault == 0);
    ProcDestroy(&Proc);
}

void
TestLxMm(void)
{
    int Stack;
    void *Heap = malloc(16);

    if (((ULONG64)(ULONG_PTR)&Stack < LX_HOST_USER_LIMIT) || ((ULONG64)(ULONG_PTR)Heap < LX_HOST_USER_LIMIT))
    {
        fprintf(stderr, "lxmm: host memory below 4 GB, test skipped\n");
        free(Heap);
        return;
    }
    free(Heap);

    WorldCreate(&World, 160 * 1024, 1, 400 * 1024);
    LxHostWorld = &World;

    LoadsAndShares(&LibTinfo);
    LoadsAndShares(&LibC);
    LoadsAndShares(&LibGtk);
    LoadsAndShares(&LibLlvm);
    LoadsAndShares(&LibPage4k);
    ExecutableLoads();
    ForkSharesAndIsolates();
    FixedOverViews();
    WritableCopyAndDiscard();
    ProtectUpgradeKeepsLayout();
    ForkDirtyFileView();
    RemapKeepsCopies();
    BeyondEndOfFile();
    LockSemantics();
    HeapFaultCost();
    ForkCost();
    ExecLoadCost();
    ForkSmallProcessCost();
    ForkLibrariesCost();

    CHECK(WorldCheck(&World) == 0);
    WorldExpectClean(&World, 160 * 1024);
    WorldDestroy(&World);
    LxHostWorld = NULL;
}
