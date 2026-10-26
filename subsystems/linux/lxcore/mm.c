/*
 * PROJECT:     LiberNT Linux Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Linux memory system calls on top of NT virtual memory
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "lxcore.h"

#define NDEBUG
#include <debug.h>

VOID
LxMmLock(
    _In_ PLX_PROCESS Process)
{
    PKTHREAD Self = KeGetCurrentThread();

    Process = Process->Mm;
    if (Process->MmLockOwner == Self)
    {
        Process->MmLockDepth++;
        return;
    }
    KeEnterCriticalRegion();
    KeWaitForSingleObject(&Process->MmLock, Executive, KernelMode, FALSE, NULL);
    Process->MmLockOwner = Self;
    Process->MmLockDepth = 1;
}

VOID
LxMmUnlock(
    _In_ PLX_PROCESS Process)
{
    Process = Process->Mm;
    if (--Process->MmLockDepth != 0)
        return;
    Process->MmLockOwner = NULL;
    KeSetEvent(&Process->MmLock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();
}

ULONG
LxProtToNt(
    _In_ ULONG Prot)
{
    switch (Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC))
    {
        case LX_PROT_NONE:
            return PAGE_NOACCESS;
        case LX_PROT_READ:
            return PAGE_READONLY;
        case LX_PROT_WRITE:
        case LX_PROT_READ | LX_PROT_WRITE:
            return PAGE_READWRITE;
        case LX_PROT_EXEC:
        case LX_PROT_READ | LX_PROT_EXEC:
            return PAGE_EXECUTE_READ;
        default:
            return PAGE_EXECUTE_READWRITE;
    }
}

static
BOOLEAN
LxProtAllows(
    _In_ ULONG Prot,
    _In_ ULONG Access)
{
    if ((Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) == LX_PROT_NONE)
        return FALSE;
    return (Access == LX_PROT_READ) || ((Prot & Access) != 0);
}

static
BOOLEAN
LxNtAllows(
    _In_ ULONG Protect,
    _In_ ULONG Access)
{
    switch (Protect & 0xFF)
    {
        case PAGE_READONLY:
            return (Access == LX_PROT_READ);
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
            return (Access != LX_PROT_EXEC);
        case PAGE_EXECUTE_READ:
            return (Access != LX_PROT_WRITE);
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return TRUE;
        default:
            return FALSE;
    }
}

static
NTSTATUS
LxVmAllocate(
    _Inout_ PULONG64 Base,
    _In_ ULONG64 Size,
    _In_ ULONG Type,
    _In_ ULONG Protect)
{
    PVOID Address = (PVOID)(ULONG_PTR)*Base;
    SIZE_T RegionSize = (SIZE_T)Size;
    NTSTATUS Status;

    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &RegionSize, Type, Protect);
    if (NT_SUCCESS(Status))
        *Base = (ULONG64)(ULONG_PTR)Address;
    return Status;
}

static
NTSTATUS
LxVmFree(
    _In_ ULONG64 Base,
    _In_ ULONG64 Size,
    _In_ ULONG Type)
{
    PVOID Address = (PVOID)(ULONG_PTR)Base;
    SIZE_T RegionSize = (SIZE_T)Size;

    return ZwFreeVirtualMemory(NtCurrentProcess(), &Address, &RegionSize, Type);
}

static
NTSTATUS
LxVmProtect(
    _In_ ULONG64 Base,
    _In_ ULONG64 Size,
    _In_ ULONG Protect)
{
    PVOID Address = (PVOID)(ULONG_PTR)Base;
    SIZE_T RegionSize = (SIZE_T)Size;
    ULONG OldProtect;

    return ZwProtectVirtualMemory(NtCurrentProcess(), &Address, &RegionSize, Protect, &OldProtect);
}

static
NTSTATUS
LxVmQuery(
    _In_ ULONG64 Address,
    _Out_ PMEMORY_BASIC_INFORMATION Information)
{
    return ZwQueryVirtualMemory(NtCurrentProcess(),
                                (PVOID)(ULONG_PTR)Address,
                                MemoryBasicInformation,
                                Information,
                                sizeof(*Information),
                                NULL);
}

static
VOID
LxVmFlushCode(
    _In_ ULONG64 Base,
    _In_ ULONG64 Size)
{
    ZwFlushInstructionCache(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base, (SIZE_T)Size);
}

static
PLX_VMA
LxVmaFind(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address)
{
    PLIST_ENTRY Entry;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Address < Vma->Start)
            return NULL;
        if (Address < Vma->End)
            return Vma;
    }
    return NULL;
}

static
VOID
LxVmaFree(
    _In_ PLX_VMA Vma)
{
    if (Vma->File != NULL)
        LxFileDereference(Vma->File);
    if ((Vma->Flags & LX_VMA_SHM) && (Vma->Section != NULL))
        LxIpcShmVmaClose(Vma->Section);
    if (Vma->Section != NULL)
        ObDereferenceObject(Vma->Section);
    LxFree(Vma);
}

static
LONG
LxVmaSplit(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address)
{
    PLX_VMA Vma = LxVmaFind(Process, Address);
    PLX_VMA Tail;

    if ((Vma == NULL) || (Vma->Start == Address))
        return 0;

    Tail = LxAlloc(sizeof(*Tail));
    if (Tail == NULL)
        return -LX_ENOMEM;
    Tail->Start = Address;
    Tail->End = Vma->End;
    Tail->Prot = Vma->Prot;
    Tail->Flags = Vma->Flags;
    Tail->File = Vma->File;
    Tail->Section = Vma->Section;
    Tail->Offset = ((Vma->File != NULL) || (Vma->Section != NULL)) ? Vma->Offset + (Address - Vma->Start) : 0;
    Tail->ViewBase = Vma->ViewBase;
    Tail->ViewOffset = Vma->ViewOffset;
    Tail->MapEnd = Vma->MapEnd;
    if (Tail->File != NULL)
        LxFileReference(Tail->File);
    if (Tail->Section != NULL)
        ObReferenceObject(Tail->Section);
    if ((Tail->Flags & LX_VMA_SHM) && (Tail->Section != NULL))
        LxIpcShmVmaOpen(Tail->Section);
    Vma->End = Address;
    InsertHeadList(&Vma->Link, &Tail->Link);
    return 0;
}

static
VOID
LxVmaMerge(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry = Process->Vmas.Flink;

    while ((Entry != &Process->Vmas) && (Entry->Flink != &Process->Vmas))
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);
        PLX_VMA Next = CONTAINING_RECORD(Entry->Flink, LX_VMA, Link);

        if (Vma->Start > End)
            break;
        if ((Next->End >= Start) &&
            (Vma->End == Next->Start) &&
            (Vma->Prot == Next->Prot) &&
            (Vma->Flags == Next->Flags) &&
            (Vma->File == Next->File) &&
            (Vma->Section == Next->Section) &&
            (Vma->ViewBase == Next->ViewBase) &&
            (((Vma->File == NULL) && (Vma->Section == NULL)) || (Vma->Offset + (Vma->End - Vma->Start) == Next->Offset)))
        {
            Vma->End = Next->End;
            RemoveEntryList(&Next->Link);
            LxVmaFree(Next);
            continue;
        }
        Entry = Entry->Flink;
    }
}

static
LONG
LxVmaRemove(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;
    LONG Error;

    Error = LxVmaSplit(Process, Start);
    if (Error == 0)
        Error = LxVmaSplit(Process, End);
    if (Error != 0)
        return Error;

    Entry = Process->Vmas.Flink;
    while (Entry != &Process->Vmas)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        Entry = Entry->Flink;
        if (Vma->Start >= End)
            break;
        if (Vma->Start >= Start)
        {
            RemoveEntryList(&Vma->Link);
            LxVmaFree(Vma);
        }
    }
    return 0;
}

static
LONG
LxVmaInsertView(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Prot,
    _In_ ULONG Flags,
    _In_opt_ PLX_FILE File,
    _In_ ULONG64 Offset,
    _In_opt_ PVOID Section,
    _In_ ULONG64 ViewBase,
    _In_ ULONG64 ViewOffset,
    _In_ ULONG64 MapEnd)
{
    PLX_VMA Vma = LxAlloc(sizeof(*Vma));
    PLIST_ENTRY Entry;
    LONG Error;

    if (Vma == NULL)
        return -LX_ENOMEM;
    Error = LxVmaRemove(Process, Start, End);
    if (Error != 0)
    {
        LxFree(Vma);
        return Error;
    }

    Vma->Start = Start;
    Vma->End = End;
    Vma->Prot = Prot;
    Vma->Flags = Flags;
    Vma->File = File;
    Vma->Section = Section;
    Vma->Offset = ((File != NULL) || (Section != NULL)) ? Offset : 0;
    Vma->ViewBase = ViewBase;
    Vma->ViewOffset = ViewOffset;
    Vma->MapEnd = MapEnd;
    if (File != NULL)
        LxFileReference(File);
    if (Section != NULL)
        ObReferenceObject(Section);
    if ((Flags & LX_VMA_SHM) && (Section != NULL))
        LxIpcShmVmaOpen(Section);

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, LX_VMA, Link)->Start >= End)
            break;
    }
    InsertTailList(Entry, &Vma->Link);
    LxVmaMerge(Process, Start, End);
    return 0;
}

static
LONG
LxVmaInsert(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Prot,
    _In_ ULONG Flags,
    _In_opt_ PLX_FILE File,
    _In_ ULONG64 Offset,
    _In_opt_ PVOID Section)
{
    return LxVmaInsertView(Process, Start, End, Prot, Flags, File, Offset, Section, 0, 0, 0);
}

static
BOOLEAN
LxVmaOverlaps(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->Start >= End)
            return FALSE;
        if (Vma->End > Start)
            return TRUE;
    }
    return FALSE;
}

static
BOOLEAN
LxVmaCovers(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;
    ULONG64 Cursor = Start;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->End <= Cursor)
            continue;
        if (Vma->Start > Cursor)
            return FALSE;
        Cursor = Vma->End;
        if (Cursor >= End)
            return TRUE;
    }
    return FALSE;
}

static
LONG
LxVmaSetProt(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Prot)
{
    PLIST_ENTRY Entry;
    LONG Error;

    Error = LxVmaSplit(Process, Start);
    if (Error == 0)
        Error = LxVmaSplit(Process, End);
    if (Error != 0)
        return Error;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->Start >= End)
            break;
        if (Vma->Start >= Start)
            Vma->Prot = Prot;
    }
    LxVmaMerge(Process, Start, End);
    return 0;
}

static
LONG
LxVmaSetLock(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Lock)
{
    PLIST_ENTRY Entry;
    LONG Error;

    Error = LxVmaSplit(Process, Start);
    if (Error == 0)
        Error = LxVmaSplit(Process, End);
    if (Error != 0)
        return Error;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->Start >= End)
            break;
        if (Vma->Start >= Start)
            Vma->Flags = (Vma->Flags & ~LX_VMA_LOCK_MASK) | Lock;
    }
    LxVmaMerge(Process, Start, End);
    return 0;
}

static
ULONG64
LxMmLockedIn(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;
    ULONG64 Bytes = 0;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->Start >= End)
            break;
        if ((Vma->End > Start) && (Vma->Flags & LX_VMA_LOCKED))
            Bytes += min(Vma->End, End) - max(Vma->Start, Start);
    }
    return Bytes;
}

static
BOOLEAN
LxMmCanLock(
    _In_ PLX_PROCESS Caller)
{
    return (Caller->Limits[LX_RLIMIT_MEMLOCK].Cur != 0) || LxCapable(Caller, LX_CAP_IPC_LOCK);
}

static
BOOLEAN
LxMmLockAllowed(
    _In_ PLX_PROCESS Caller,
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Bytes)
{
    ULONG64 Limit = Caller->Limits[LX_RLIMIT_MEMLOCK].Cur;

    if ((Limit == LX_RLIM_INFINITY) || LxCapable(Caller, LX_CAP_IPC_LOCK))
        return TRUE;
    return (LxMmLockedIn(Process, 0, MAXULONG64) + Bytes) / LX_PAGE_SIZE <= Limit / LX_PAGE_SIZE;
}

static
ULONG64
LxVmAllocationEnd(
    _In_ ULONG64 AllocationBase)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = AllocationBase;

    while (NT_SUCCESS(LxVmQuery(Cursor, &Information)) &&
           ((ULONG64)(ULONG_PTR)Information.AllocationBase == AllocationBase) &&
           (Information.State != MEM_FREE))
    {
        Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
    }
    return Cursor;
}

static
NTSTATUS
LxVmCommitRange(
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Protect)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Cursor < End)
    {
        ULONG64 Stop = End, Commit = Cursor;

        if (NT_SUCCESS(LxVmQuery(Cursor, &Information)) && (Information.State != MEM_FREE))
            Stop = min(End, (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize);
        Status = LxVmAllocate(&Commit, Stop - Cursor, MEM_COMMIT, Protect);
        if (!NT_SUCCESS(Status))
            break;
        Cursor = Stop;
    }
    return Status;
}

static
NTSTATUS
LxVmProtectRange(
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Protect)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Cursor < End)
    {
        ULONG64 Stop;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            return STATUS_CONFLICTING_ADDRESSES;
        Stop = min(End, (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize);
        if (Information.State == MEM_COMMIT)
        {
            Status = LxVmProtect(Cursor, Stop - Cursor, Protect);
            if (!NT_SUCCESS(Status))
                break;
        }
        Cursor = Stop;
    }
    return Status;
}

static
VOID
LxVmDecommitRange(
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;

    while (Cursor < End)
    {
        ULONG64 RegionEnd;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            break;
        RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, End);
        if ((Information.State == MEM_COMMIT) && (Information.Type == MEM_PRIVATE))
            LxVmFree(Cursor, RegionEnd - Cursor, MEM_DECOMMIT);
        else if ((Information.State == MEM_COMMIT) && (Information.Type == MEM_MAPPED))
            LxVmProtect(Cursor, RegionEnd - Cursor, PAGE_NOACCESS);
        Cursor = RegionEnd;
    }
}

static
BOOLEAN
LxVmaIsCow(
    _In_ PLX_VMA Vma)
{
    return (Vma->Flags & (LX_VMA_VIEW | LX_MAP_SHARED)) == LX_VMA_VIEW;
}

static
VOID
LxVmReleaseRange(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start, Last = 0;

    while (Cursor < End)
    {
        ULONG64 RegionEnd, AllocationBase;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            break;
        RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, End);
        AllocationBase = (ULONG64)(ULONG_PTR)Information.AllocationBase;
        if ((Information.State != MEM_FREE) && (AllocationBase != Last) &&
            ((Information.Type == MEM_PRIVATE) || (Information.Type == MEM_MAPPED)))
        {
            Last = AllocationBase;
            if (!LxVmaOverlaps(Process, AllocationBase, LxVmAllocationEnd(AllocationBase)))
            {
                if (Information.Type == MEM_MAPPED)
                {
                    PLIST_ENTRY Entry;

                    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
                    {
                        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

                        if (LxVmaIsCow(Vma) && (Vma->ViewBase == AllocationBase))
                            Vma->ViewBase = 0;
                    }
                    ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)AllocationBase);
                }
                else
                {
                    LxVmFree(AllocationBase, 0, MEM_RELEASE);
                }
            }
        }
        Cursor = RegionEnd;
    }
}

#define LX_PLACE_FRESH  0x1
#define LX_PLACE_SPLIT  0x2

typedef struct _LX_MM_COPY
{
    LIST_ENTRY Link;
    ULONG64 Start;
    ULONG64 End;
    ULONG Prot;
    PUCHAR Data;
} LX_MM_COPY, *PLX_MM_COPY;

static
ULONG
LxCowProtToNt(
    _In_ ULONG Prot)
{
    switch (Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC))
    {
        case LX_PROT_NONE:
            return PAGE_NOACCESS;
        case LX_PROT_READ:
            return PAGE_READONLY;
        case LX_PROT_WRITE:
        case LX_PROT_READ | LX_PROT_WRITE:
            return PAGE_WRITECOPY;
        case LX_PROT_EXEC:
        case LX_PROT_READ | LX_PROT_EXEC:
            return PAGE_EXECUTE_READ;
        default:
            return PAGE_EXECUTE_WRITECOPY;
    }
}

static
ULONG
LxVmaNtProtect(
    _In_ PLX_VMA Vma)
{
    return LxVmaIsCow(Vma) ? LxCowProtToNt(Vma->Prot) : LxProtToNt(Vma->Prot);
}

static
ULONG
LxViewAccess(
    _In_ ULONG Protect)
{
    switch (Protect & 0xFF)
    {
        case PAGE_NOACCESS:
            return 0;
        case PAGE_READONLY:
            return 1;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
            return 3;
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
            return 5;
        default:
            return 7;
    }
}

static
ULONG
LxCowViewMaximum(
    _In_ ULONG Prot)
{
    return (Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) ? LxCowProtToNt(Prot) : PAGE_READONLY;
}

static
BOOLEAN
LxVmaBacked(
    _In_ PLX_VMA Vma,
    _In_ PMEMORY_BASIC_INFORMATION Information)
{
    if (Information->State == MEM_FREE)
        return FALSE;
    if (LxVmaIsCow(Vma))
    {
        return (Information->Type == MEM_MAPPED) && (Vma->ViewBase != 0) &&
               ((ULONG64)(ULONG_PTR)Information->AllocationBase == Vma->ViewBase);
    }
    if (Vma->Flags & LX_VMA_VIEW)
        return Information->Type == MEM_MAPPED;
    return Information->Type == MEM_PRIVATE;
}

static
BOOLEAN
LxVmaAccessibleIn(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->Start >= End)
            break;
        if ((Vma->End > Start) &&
            ((Vma->Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) || (Vma->Flags & LX_VMA_DIRTY)))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static
BOOLEAN
LxVmHasCommit(
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;

    while ((Cursor < End) && NT_SUCCESS(LxVmQuery(Cursor, &Information)))
    {
        if (Information.State == MEM_COMMIT)
            return TRUE;
        Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
    }
    return FALSE;
}

static
LONG
LxMmCopySave(
    _Inout_ PLIST_ENTRY List,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Prot,
    _In_ ULONG Protect)
{
    PLX_MM_COPY Copy;
    LONG Error = 0;

    if (End <= Start)
        return 0;
    Copy = LxAlloc(sizeof(*Copy));
    if (Copy == NULL)
        return -LX_ENOMEM;
    Copy->Data = LxAllocPaged((SIZE_T)(End - Start));
    if (Copy->Data == NULL)
    {
        LxFree(Copy);
        return -LX_ENOMEM;
    }
    Copy->Start = Start;
    Copy->End = End;
    Copy->Prot = Prot;

    LxVmProtectRange(Start, End, PAGE_READONLY);
    _SEH2_TRY
    {
        RtlCopyMemory(Copy->Data, (PVOID)(ULONG_PTR)Start, (SIZE_T)(End - Start));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = -LX_EFAULT;
    }
    _SEH2_END;
    LxVmProtectRange(Start, End, Protect);

    if (Error != 0)
    {
        LxFreePaged(Copy->Data);
        LxFree(Copy);
        return Error;
    }
    InsertTailList(List, &Copy->Link);
    return 0;
}

static
VOID
LxMmCopyRestore(
    _Inout_ PLIST_ENTRY List,
    _In_ BOOLEAN Private)
{
    while (!IsListEmpty(List))
    {
        PLX_MM_COPY Copy = CONTAINING_RECORD(RemoveHeadList(List), LX_MM_COPY, Link);
        ULONG64 Size = Copy->End - Copy->Start;
        NTSTATUS Status;

        if (Private)
            Status = LxVmCommitRange(Copy->Start, Copy->End, PAGE_READWRITE);
        else
            Status = LxVmProtectRange(Copy->Start, Copy->End, PAGE_WRITECOPY);
        if (NT_SUCCESS(Status))
        {
            _SEH2_TRY
            {
                RtlCopyMemory((PVOID)(ULONG_PTR)Copy->Start, Copy->Data, (SIZE_T)Size);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            LxVmProtectRange(Copy->Start, Copy->End, Private ? LxProtToNt(Copy->Prot) : LxCowProtToNt(Copy->Prot));
            if (Copy->Prot & LX_PROT_EXEC)
                LxVmFlushCode(Copy->Start, Size);
        }
        if (!NT_SUCCESS(Status))
            DPRINT1("Linux memory at %I64x-%I64x lost its content while moving: 0x%08lx\n", Copy->Start, Copy->End, Status);
        LxFreePaged(Copy->Data);
        LxFree(Copy);
    }
}

static
NTSTATUS
LxMmCreateCowSection(
    _In_ PLX_FILE File,
    _Out_ PVOID *Section)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Handle, SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_QUERY, &ObjectAttributes, NULL,
                             PAGE_EXECUTE_WRITECOPY, SEC_COMMIT, File->Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ObReferenceObjectByHandle(Handle, 0, NULL, KernelMode, Section, NULL);
    ZwClose(Handle);
    return Status;
}

static
NTSTATUS
LxMmMapCowView(
    _In_ PVOID Section,
    _In_ ULONG64 Base,
    _In_ ULONG64 Offset,
    _In_ ULONG64 Size,
    _In_ ULONG Maximum,
    _In_ ULONG Protect)
{
    SECTION_BASIC_INFORMATION Basic;
    LARGE_INTEGER SectionOffset;
    SIZE_T ViewSize = (SIZE_T)Size;
    PVOID View = (PVOID)(ULONG_PTR)Base;
    HANDLE Handle;
    NTSTATUS Status;

    Status = ObOpenObjectByPointer(Section, OBJ_KERNEL_HANDLE, NULL,
                                   SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_QUERY, NULL, KernelMode, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwQuerySection(Handle, SectionBasicInformation, &Basic, sizeof(Basic), NULL);
    if (NT_SUCCESS(Status) && ((ULONG64)Basic.Size.QuadPart <= Offset))
        Status = STATUS_INVALID_VIEW_SIZE;
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Handle);
        return Status;
    }
    ViewSize = (SIZE_T)min(Size, (ULONG64)Basic.Size.QuadPart - Offset);
    SectionOffset.QuadPart = (LONG64)Offset;
    Status = ZwMapViewOfSection(Handle, NtCurrentProcess(), &View, 0, 0, &SectionOffset, &ViewSize, ViewShare, 0,
                                Maximum);
    ZwClose(Handle);
    if (NT_SUCCESS(Status) && ((ULONG64)(ULONG_PTR)View != Base))
    {
        ZwUnmapViewOfSection(NtCurrentProcess(), View);
        Status = STATUS_CONFLICTING_ADDRESSES;
    }
    if (NT_SUCCESS(Status) && (Protect != Maximum))
    {
        Status = LxVmProtect(Base, ViewSize, Protect);
        if (!NT_SUCCESS(Status))
            ZwUnmapViewOfSection(NtCurrentProcess(), View);
    }
    return Status;
}

static
LONG
LxMmUpgradeView(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Base)
{
    ULONG64 End, ViewOffset = 0;
    PVOID Section = NULL;
    PLIST_ENTRY Entry;
    NTSTATUS Status;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (LxVmaIsCow(Vma) && (Vma->ViewBase == Base))
        {
            Section = Vma->Section;
            ViewOffset = Vma->ViewOffset;
            break;
        }
    }
    if ((Base == 0) || (Section == NULL))
        return 0;

    End = LxVmAllocationEnd(Base);
    ObReferenceObject(Section);
    ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base);
    Status = LxMmMapCowView(Section, Base, ViewOffset, End - Base, PAGE_EXECUTE_WRITECOPY, PAGE_NOACCESS);
    ObDereferenceObject(Section);
    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (!LxVmaIsCow(Vma) || (Vma->ViewBase != Base))
            continue;
        if (!NT_SUCCESS(Status))
            Vma->ViewBase = 0;
        else if (Vma->Start < End)
            LxVmProtect(Vma->Start, min(Vma->End, End) - Vma->Start, LxCowProtToNt(Vma->Prot));
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Linux file mapping lost its view at %I64x-%I64x\n", Base, End);
        return -LxStatusToErrno(Status);
    }
    return 0;
}

static
LONG
LxMmViewAllow(
    _In_ PLX_PROCESS Process,
    _In_ PLX_VMA Vma,
    _In_ ULONG Protect)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG Access = LxViewAccess(Protect);

    if (!LxVmaIsCow(Vma) || (Vma->ViewBase == 0) || !NT_SUCCESS(LxVmQuery(Vma->ViewBase, &Information)) ||
        (Information.Type != MEM_MAPPED) || ((LxViewAccess(Information.AllocationProtect) & Access) == Access))
    {
        return 0;
    }
    return LxMmUpgradeView(Process, Vma->ViewBase);
}

static
LONG
LxMmSplitView(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Base,
    _In_ ULONG64 ZoneStart,
    _In_ ULONG64 ZoneEnd,
    _In_ BOOLEAN Convert,
    _Inout_opt_ PLIST_ENTRY Converted)
{
    ULONG64 End = LxVmAllocationEnd(Base), ViewOffset = 0;
    ULONG64 Pieces[2][2] = { { Base, min(ZoneStart, End) }, { ZoneEnd, End } }, Cursor[2];
    BOOLEAN Used[2] = { FALSE, FALSE }, Mapped[2];
    MEMORY_BASIC_INFORMATION Information;
    ULONG Maximum = PAGE_EXECUTE_WRITECOPY;
    PVOID Section = NULL;
    LIST_ENTRY Dirty;
    PLIST_ENTRY Entry;
    ULONG Index;
    LONG Error;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if ((Vma->Start < End) && (Vma->End > Base) && (Vma->Flags & LX_MAP_SHARED))
            return -LX_EBUSY;
        if (LxVmaIsCow(Vma) && (Vma->ViewBase == Base))
        {
            Section = Vma->Section;
            ViewOffset = Vma->ViewOffset;
        }
    }
    if (Section == NULL)
    {
        ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base);
        return 0;
    }

    Error = LxVmaSplit(Process, ZoneStart);
    if (Error == 0)
        Error = LxVmaSplit(Process, ZoneEnd);
    if (Error != 0)
        return Error;

    InitializeListHead(&Dirty);
    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (!LxVmaIsCow(Vma) || (Vma->ViewBase != Base))
            continue;
        if ((Vma->Start >= ZoneStart) && (Vma->End <= ZoneEnd))
        {
            if (Convert && (Vma->Start < End) &&
                ((Vma->Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) || (Vma->Flags & LX_VMA_DIRTY)))
            {
                Error = LxMmCopySave(Converted, Vma->Start, min(Vma->End, End), Vma->Prot, LxCowProtToNt(Vma->Prot));
                if (Error != 0)
                    goto Fail;
            }
            continue;
        }
        Index = (Vma->End <= ZoneStart) ? 0 : 1;
        if (Vma->Start < Pieces[Index][1])
            Used[Index] = TRUE;
        if ((Vma->Flags & LX_VMA_DIRTY) && (Vma->Start < End))
        {
            Error = LxMmCopySave(&Dirty, Vma->Start, min(Vma->End, End), Vma->Prot, LxCowProtToNt(Vma->Prot));
            if (Error != 0)
                goto Fail;
        }
    }

    if (NT_SUCCESS(LxVmQuery(Base, &Information)) && (Information.Type == MEM_MAPPED))
        Maximum = Information.AllocationProtect;
    ObReferenceObject(Section);
    ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base);
    for (Index = 0; Index < 2; Index++)
    {
        Mapped[Index] = Used[Index] && (Pieces[Index][0] < Pieces[Index][1]) &&
                        NT_SUCCESS(LxMmMapCowView(Section, Pieces[Index][0], ViewOffset + (Pieces[Index][0] - Base),
                                                  Pieces[Index][1] - Pieces[Index][0], Maximum, Maximum));
        Cursor[Index] = Pieces[Index][0];
        if (Used[Index] && !Mapped[Index])
            DPRINT1("Linux file mapping lost its view at %I64x-%I64x\n", Pieces[Index][0], Pieces[Index][1]);
    }

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (!LxVmaIsCow(Vma) || (Vma->ViewBase != Base))
            continue;
        if ((Vma->Start >= ZoneStart) && (Vma->End <= ZoneEnd))
        {
            Vma->ViewBase = 0;
            if (Convert)
            {
                Vma->Flags &= ~(LX_VMA_VIEW | LX_VMA_DIRTY);
                ObDereferenceObject(Vma->Section);
                Vma->Section = NULL;
                Vma->ViewOffset = 0;
            }
            continue;
        }
        Index = (Vma->End <= ZoneStart) ? 0 : 1;
        if (!Mapped[Index])
        {
            Vma->ViewBase = 0;
            continue;
        }
        Vma->ViewBase = Pieces[Index][0];
        Vma->ViewOffset = ViewOffset + (Pieces[Index][0] - Base);
        if (Vma->Start < Pieces[Index][1])
        {
            if (Vma->Start > Cursor[Index])
                LxVmProtect(Cursor[Index], Vma->Start - Cursor[Index], PAGE_NOACCESS);
            if (LxCowProtToNt(Vma->Prot) != Maximum)
                LxVmProtect(Vma->Start, min(Vma->End, Pieces[Index][1]) - Vma->Start, LxCowProtToNt(Vma->Prot));
            Cursor[Index] = min(Vma->End, Pieces[Index][1]);
        }
    }
    for (Index = 0; Index < 2; Index++)
    {
        if (Mapped[Index] && (Cursor[Index] < Pieces[Index][1]))
            LxVmProtect(Cursor[Index], Pieces[Index][1] - Cursor[Index], PAGE_NOACCESS);
    }
    ObDereferenceObject(Section);
    LxMmCopyRestore(&Dirty, FALSE);
    return 0;

Fail:
    while (!IsListEmpty(&Dirty))
    {
        PLX_MM_COPY Copy = CONTAINING_RECORD(RemoveHeadList(&Dirty), LX_MM_COPY, Link);

        LxFreePaged(Copy->Data);
        LxFree(Copy);
    }
    return Error;
}

static
LONG
LxVmPlace(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Mode)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 ZoneEnd = LX_ALIGN_UP(End, LX_ALLOC_GRANULARITY), Cursor;
    LIST_ENTRY Converted;
    LONG Error = 0;

    InitializeListHead(&Converted);
    for (Cursor = LX_ALIGN_DOWN(Start, LX_ALLOC_GRANULARITY); Cursor < ZoneEnd;)
    {
        ULONG64 Base;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            return -LX_ENOMEM;
        if ((Information.State == MEM_FREE) || (Information.Type != MEM_MAPPED))
        {
            Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
            continue;
        }
        if (!(Mode & LX_PLACE_SPLIT))
            return -LX_ENOMEM;
        Base = (ULONG64)(ULONG_PTR)Information.AllocationBase;
        Error = LxMmSplitView(Process, Base, LX_ALIGN_DOWN(Start, LX_ALLOC_GRANULARITY), ZoneEnd, TRUE, &Converted);
        if ((Error == 0) && NT_SUCCESS(LxVmQuery(Cursor, &Information)) && (Information.State != MEM_FREE) &&
            (Information.Type == MEM_MAPPED) && ((ULONG64)(ULONG_PTR)Information.AllocationBase == Base))
        {
            Error = -LX_ENOMEM;
        }
        if (Error != 0)
            break;
    }

    for (Cursor = Start; (Error == 0) && (Cursor < End);)
    {
        ULONG64 RegionEnd;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
        {
            Error = -LX_ENOMEM;
            break;
        }
        RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, End);
        if (Information.State == MEM_FREE)
        {
            ULONG64 Base = LX_ALIGN_DOWN(Cursor, LX_ALLOC_GRANULARITY);
            ULONG64 Limit = LX_ALIGN_UP(RegionEnd, LX_ALLOC_GRANULARITY);

            if (!NT_SUCCESS(LxVmAllocate(&Base, Limit - Base, MEM_RESERVE, PAGE_NOACCESS)))
                Error = -LX_ENOMEM;
        }
        else if (Information.Type != MEM_PRIVATE)
        {
            Error = -LX_ENOMEM;
        }
        else if ((Mode & LX_PLACE_FRESH) && (Information.State == MEM_COMMIT))
        {
            LxVmFree(Cursor, RegionEnd - Cursor, MEM_DECOMMIT);
        }
        Cursor = RegionEnd;
    }

    if (!IsListEmpty(&Converted))
    {
        PLIST_ENTRY Entry;

        for (Entry = Converted.Flink; Entry != &Converted; Entry = Entry->Flink)
        {
            PLX_MM_COPY Copy = CONTAINING_RECORD(Entry, LX_MM_COPY, Link);
            ULONG64 Base = LX_ALIGN_DOWN(Copy->Start, LX_ALLOC_GRANULARITY);

            if (NT_SUCCESS(LxVmQuery(Copy->Start, &Information)) && (Information.State == MEM_FREE))
                LxVmAllocate(&Base, LX_ALIGN_UP(Copy->End, LX_ALLOC_GRANULARITY) - Base, MEM_RESERVE, PAGE_NOACCESS);
        }
        LxMmCopyRestore(&Converted, TRUE);
    }
    return Error;
}

static
LONG
LxMmClearForView(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 ZoneStart,
    _In_ ULONG64 ZoneEnd)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = ZoneStart;

    if (LxVmaAccessibleIn(Process, ZoneStart, ZoneEnd))
        return 1;
    while (Cursor < ZoneEnd)
    {
        ULONG64 Base, End, Low;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            return -LX_ENOMEM;
        if (Information.State == MEM_FREE)
        {
            Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
            continue;
        }
        Base = (ULONG64)(ULONG_PTR)Information.AllocationBase;
        End = LxVmAllocationEnd(Base);
        if (Information.Type == MEM_PRIVATE)
        {
            if (LxVmHasCommit(Base, End))
                return 1;
            if (!NT_SUCCESS(LxVmFree(Base, 0, MEM_RELEASE)))
                return 1;
            if (Base < ZoneStart)
            {
                Low = Base;
                LxVmAllocate(&Low, ZoneStart - Base, MEM_RESERVE, PAGE_NOACCESS);
            }
            if (End > ZoneEnd)
            {
                Low = ZoneEnd;
                LxVmAllocate(&Low, LX_ALIGN_UP(End, LX_ALLOC_GRANULARITY) - ZoneEnd, MEM_RESERVE, PAGE_NOACCESS);
            }
        }
        else if ((Information.Type != MEM_MAPPED) || (LxMmSplitView(Process, Base, ZoneStart, ZoneEnd, FALSE, NULL) != 0))
        {
            return 1;
        }
        Cursor = max(Cursor + LX_PAGE_SIZE, min(End, ZoneEnd));
    }
    return 0;
}

static
LONG
LxMmFill(
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ PLX_FILE File,
    _In_ ULONG64 Offset)
{
    PUCHAR Bounce = LxAllocPaged(LX_IO_CHUNK);
    ULONG64 Done = 0;
    LONG64 Read;
    LONG64 Error;

    if (Bounce == NULL)
        return -LX_ENOMEM;

    while (Done < Length)
    {
        ULONG Chunk = (ULONG)min(Length - Done, LX_IO_CHUNK);

        Read = LxFileReadKernel(File, Bounce, Chunk, Offset + Done);
        if (Read < 0)
        {
            LxFreePaged(Bounce);
            return (LONG)Read;
        }
        if (Read == 0)
            break;
        Error = LxCopyToUser(Address + Done, Bounce, (SIZE_T)Read);
        if (Error != 0)
        {
            LxFreePaged(Bounce);
            return (LONG)Error;
        }
        Done += (ULONG64)Read;
        if ((ULONG64)Read < Chunk)
            break;
    }

    LxFreePaged(Bounce);
    return 0;
}

static
LONG
LxMmPopulateRun(
    _In_ PLX_VMA Vma,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    ULONG NtProtect = LxProtToNt(Vma->Prot);
    LONG Error;

    if (Vma->File == NULL)
        return NT_SUCCESS(LxVmCommitRange(Start, End, NtProtect)) ? 0 : -LX_ENOMEM;

    if (!NT_SUCCESS(LxVmCommitRange(Start, End, PAGE_READWRITE)))
        return -LX_ENOMEM;
    Error = LxMmFill(Start, End - Start, Vma->File, Vma->Offset + (Start - Vma->Start));
    if (Error != 0)
    {
        LxVmDecommitRange(Start, End);
        return Error;
    }
    if (NtProtect != PAGE_READWRITE)
        LxVmProtectRange(Start, End, NtProtect);
    if (Vma->Prot & LX_PROT_EXEC)
        LxVmFlushCode(Start, End - Start);
    return 0;
}

static
LONG
LxMmFillPrivateFile(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    ULONG64 Cursor = Start;

    while (Cursor < End)
    {
        PLX_VMA Vma = LxVmaFind(Process, Cursor);
        ULONG64 Stop;
        LONG Error;

        if (Vma == NULL)
            return -LX_EFAULT;
        Stop = min(Vma->End, End);
        if ((Vma->File != NULL) && !(Vma->Flags & LX_VMA_VIEW))
        {
            Error = LxMmPopulateRun(Vma, Cursor, Stop);
            if (Error != 0)
                return Error;
        }
        Cursor = Stop;
    }
    return 0;
}

static
LONG
LxMmBackVma(
    _In_ PLX_PROCESS Process,
    _In_ PLX_VMA Vma)
{
    ULONG64 Start = Vma->Start, End = Vma->End;
    LONG Error;

    Error = LxVmPlace(Process, Start, End, LX_PLACE_SPLIT);
    if (Error != 0)
        return Error;
    Vma = LxVmaFind(Process, Start);
    if ((Vma == NULL) || !LxVmaIsCow(Vma))
        return 0;
    Vma->Flags &= ~(LX_VMA_VIEW | LX_VMA_DIRTY);
    if (Vma->Section != NULL)
        ObDereferenceObject(Vma->Section);
    Vma->Section = NULL;
    Vma->ViewBase = 0;
    Vma->ViewOffset = 0;
    return LxMmFillPrivateFile(Process, Vma->Start, Vma->End);
}

static
VOID
LxVmReleaseReservations(
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;

    while (Cursor < End)
    {
        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            break;
        if ((Information.State == MEM_RESERVE) && (Information.Type == MEM_PRIVATE) &&
            ((ULONG64)(ULONG_PTR)Information.AllocationBase >= Start))
        {
            LxVmFree((ULONG64)(ULONG_PTR)Information.AllocationBase, 0, MEM_RELEASE);
        }
        Cursor = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;
    }
}

static
LONG
LxMmGrowView(
    _In_ PLX_PROCESS Process,
    _In_ PLX_VMA Vma,
    _In_ ULONG64 Address)
{
    ULONG64 Base = Vma->ViewBase, FileOffset = Vma->ViewOffset, MapEnd = Base, FileEnd, NewSize, OldEnd = Base;
    PLX_FILE File = Vma->File;
    BOOLEAN Writable = ((File->Flags & LX_O_ACCMODE) == LX_O_RDWR), AnyWrite = FALSE;
    FILE_STANDARD_INFORMATION Standard;
    MEMORY_BASIC_INFORMATION Information;
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER SectionOffset;
    IO_STATUS_BLOCK Iosb;
    PLIST_ENTRY Entry;
    SIZE_T ViewSize;
    PVOID View;
    HANDLE Section;
    NTSTATUS Status;

    if ((File == NULL) || (File->Handle == NULL))
        return -LX_EFAULT;
    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Piece = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Piece->ViewBase != Base)
            continue;
        MapEnd = max(MapEnd, min(Piece->End, Piece->MapEnd));
        if (Piece->Prot & LX_PROT_WRITE)
            AnyWrite = TRUE;
    }
    if (!NT_SUCCESS(ZwQueryInformationFile(File->Handle, &Iosb, &Standard, sizeof(Standard), FileStandardInformation)))
        return -LX_EFAULT;
    FileEnd = (ULONG64)Standard.EndOfFile.QuadPart;
    if (FileEnd <= FileOffset + (LX_ALIGN_DOWN(Address, LX_PAGE_SIZE) - Base))
        return -LX_EFAULT;
    NewSize = min(FileEnd - FileOffset, MapEnd - Base);

    if (NT_SUCCESS(LxVmQuery(Base, &Information)) && (Information.Type == MEM_MAPPED) &&
        ((ULONG64)(ULONG_PTR)Information.AllocationBase == Base))
    {
        OldEnd = LxVmAllocationEnd(Base);
        if (OldEnd >= LX_ALIGN_UP(Base + NewSize, LX_PAGE_SIZE))
            return -LX_EFAULT;
        ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base);
    }
    LxVmReleaseReservations(LX_ALIGN_UP(OldEnd, LX_ALLOC_GRANULARITY), LX_ALIGN_UP(MapEnd, LX_ALLOC_GRANULARITY));

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Section, SECTION_ALL_ACCESS, &ObjectAttributes, NULL,
                             Writable ? PAGE_READWRITE : PAGE_READONLY, SEC_COMMIT, File->Handle);
    if (NT_SUCCESS(Status))
    {
        View = (PVOID)(ULONG_PTR)Base;
        ViewSize = (SIZE_T)NewSize;
        SectionOffset.QuadPart = (LONG64)FileOffset;
        Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &View, 0, 0, &SectionOffset, &ViewSize, ViewShare, 0,
                                    (Writable && AnyWrite) ? PAGE_READWRITE : PAGE_READONLY);
        ZwClose(Section);
    }
    if (LX_ALIGN_UP(Base + NewSize, LX_ALLOC_GRANULARITY) < LX_ALIGN_UP(MapEnd, LX_ALLOC_GRANULARITY))
    {
        ULONG64 Tail = LX_ALIGN_UP(Base + NewSize, LX_ALLOC_GRANULARITY);

        LxVmAllocate(&Tail, LX_ALIGN_UP(MapEnd, LX_ALLOC_GRANULARITY) - Tail, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Linux shared mapping cannot grow its view at %I64x: 0x%08lx\n", Base, Status);
        return -LX_EFAULT;
    }

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Piece = CONTAINING_RECORD(Entry, LX_VMA, Link);
        ULONG64 Start = max(Piece->Start, Base), End = min(Piece->End, LX_ALIGN_UP(Base + NewSize, LX_PAGE_SIZE));

        if ((Piece->ViewBase == Base) && (Start < End))
            LxVmProtect(Start, End - Start, LxProtToNt(Piece->Prot));
    }
    return 0;
}

static
LONG
LxMmPopulate(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End,
    _In_ ULONG Access,
    _In_ BOOLEAN Cluster)
{
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor = Start;
    LONG Populated = 0, Healed = 0;
    LONG Error;

    while (Cursor < End)
    {
        PLX_VMA Vma = LxVmaFind(Process, Cursor);
        ULONG64 RegionStart, RegionEnd, FillStart, FillEnd;

        if ((Vma == NULL) || !LxProtAllows(Vma->Prot, Access))
            return -LX_EFAULT;
        if ((Vma->Flags & LX_VMA_VIEW) && (Vma->MapEnd != 0) &&
            (!NT_SUCCESS(LxVmQuery(Cursor, &Information)) || (Information.State != MEM_COMMIT)))
        {
            Error = LxMmGrowView(Process, Vma, Cursor);
            if (Error != 0)
                return Error;
            Populated++;
            continue;
        }
        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            return -LX_EFAULT;
        if (!(Vma->Flags & LX_MAP_SHARED) && !LxVmaBacked(Vma, &Information))
        {
            if (LxVmaIsCow(Vma) && (Vma->ViewBase != 0))
                return -LX_ENXIO;
            if (++Healed > 8)
                return -LX_EFAULT;
            Error = LxMmBackVma(Process, Vma);
            if (Error != 0)
                return Error;
            Populated++;
            continue;
        }
        if (Information.State == MEM_FREE)
            return -LX_EFAULT;

        RegionStart = (ULONG64)(ULONG_PTR)Information.BaseAddress;
        RegionEnd = min(RegionStart + Information.RegionSize, Vma->End);
        if ((Vma->Flags & LX_VMA_VIEW) && (Information.State != MEM_COMMIT))
            return -LX_EFAULT;
        if (Information.State == MEM_COMMIT)
        {
            if (!LxNtAllows(Information.Protect, Access))
                return -LX_EFAULT;
            Cursor = min(RegionEnd, End);
            continue;
        }

        FillStart = Cursor;
        FillEnd = min(RegionEnd, End);
        if (Cluster)
        {
            FillStart = max(max(RegionStart, Vma->Start), LX_ALIGN_DOWN(Cursor, LX_ALLOC_GRANULARITY));
            FillEnd = min(RegionEnd, LX_ALIGN_UP(Cursor + 1, LX_ALLOC_GRANULARITY));
        }
        Error = LxMmPopulateRun(Vma, FillStart, FillEnd);
        if (Error != 0)
            return Error;
        Populated++;
        Cursor = FillEnd;
    }
    return Populated;
}

static
VOID
LxMmLockPopulate(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    ULONG64 Cursor = Start;

    while (Cursor < End)
    {
        PLIST_ENTRY Entry;
        PLX_VMA Vma = NULL;
        ULONG64 Stop;
        UCHAR Byte;

        for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
        {
            Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);
            if (Vma->End > Cursor)
                break;
            Vma = NULL;
        }
        if ((Vma == NULL) || (Vma->Start >= End))
            break;

        Cursor = max(Cursor, Vma->Start);
        Stop = min(Vma->End, End);
        if (((Vma->Flags & LX_VMA_LOCK_MASK) == LX_VMA_LOCKED) && (Vma->Prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)))
        {
            LxMmPopulate(Process, Cursor, Stop, LX_PROT_READ, FALSE);
            for (; Cursor < Stop; Cursor += LX_PAGE_SIZE)
            {
                if (LxCopyFromUser(&Byte, Cursor, 1) != 0)
                    break;
            }
        }
        Cursor = Stop;
    }
}

BOOLEAN
LxMmHandleFault(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG Access)
{
    ULONG64 Page = LX_ALIGN_DOWN(Address, LX_PAGE_SIZE);
    PLX_THREAD Thread = LxCurrentThread();
    LONG Result;

    Process = Process->Mm;
    LxMmLock(Process);
    Result = LxMmPopulate(Process, Page, Page + LX_PAGE_SIZE, Access, TRUE);
    LxMmUnlock(Process);

    if (Result < 0)
    {
        if ((Result == -LX_ENXIO) && (Thread != NULL))
            Thread->FaultBus = TRUE;
        return FALSE;
    }
    if ((Result > 0) || (Thread == NULL) || (Thread->FaultAddress != Page))
    {
        if (Thread != NULL)
        {
            Thread->FaultAddress = Page;
            Thread->FaultRetries = 0;
        }
        return TRUE;
    }
    return (++Thread->FaultRetries < 8);
}

BOOLEAN
LxMmFaultIn(
    _In_ ULONG64 Address,
    _In_ SIZE_T Length,
    _In_ BOOLEAN Write)
{
    PLX_THREAD Thread = LxCurrentThread();
    ULONG64 Start, End;
    LONG Result;

    if ((Thread == NULL) || (Length == 0) ||
        (KeGetCurrentIrql() != PASSIVE_LEVEL) || KeAreAllApcsDisabled())
    {
        return FALSE;
    }

    Start = LX_ALIGN_DOWN(Address, LX_PAGE_SIZE);
    End = LX_ALIGN_UP(Address + Length, LX_PAGE_SIZE);
    if (End <= Start)
        return FALSE;

    LxMmLock(Thread->Process->Mm);
    Result = LxMmPopulate(Thread->Process->Mm, Start, End, Write ? LX_PROT_WRITE : LX_PROT_READ, FALSE);
    LxMmUnlock(Thread->Process->Mm);
    return (Result > 0);
}

BOOLEAN
LxMmSharedKey(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _Out_ PVOID *Object,
    _Out_ PULONG64 Offset)
{
    PFILE_OBJECT FileObject;
    BOOLEAN Found = FALSE;
    PLX_VMA Vma;

    Process = Process->Mm;
    LxMmLock(Process);
    Vma = LxVmaFind(Process, Address);
    if ((Vma != NULL) && (Vma->Flags & LX_VMA_VIEW) &&
        (!LxVmaIsCow(Vma) || !((Vma->Prot & LX_PROT_WRITE) || (Vma->Flags & LX_VMA_DIRTY))))
    {
        if ((Vma->Section != NULL) && !LxVmaIsCow(Vma))
        {
            *Object = Vma->Section;
            *Offset = Vma->Offset + (Address - Vma->Start);
            Found = TRUE;
        }
        else if ((Vma->File != NULL) && (Vma->File->Handle != NULL) &&
                 NT_SUCCESS(ObReferenceObjectByHandle(Vma->File->Handle, 0, *IoFileObjectType, KernelMode,
                                                      (PVOID *)&FileObject, NULL)))
        {
            *Object = (FileObject->SectionObjectPointer != NULL) ? (PVOID)FileObject->SectionObjectPointer :
                                                                   FileObject->FsContext;
            *Offset = Vma->Offset + (Address - Vma->Start);
            ObDereferenceObject(FileObject);
            Found = TRUE;
        }
    }
    LxMmUnlock(Process);
    return Found;
}

LONG
LxMmZero(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 End)
{
    ULONG64 Page = LX_ALIGN_DOWN(Address, LX_PAGE_SIZE);
    PLX_VMA Vma;
    LONG Error;

    Process = Process->Mm;
    if (End <= Address)
        return 0;

    LxMmLock(Process);
    Error = LxMmPopulate(Process, Page, End, LX_PROT_READ, FALSE);
    Vma = LxVmaFind(Process, Page);
    if ((Error >= 0) && (Vma != NULL))
    {
        if (LxVmaIsCow(Vma))
        {
            Vma->Flags |= LX_VMA_DIRTY;
            LxMmViewAllow(Process, Vma, PAGE_WRITECOPY);
        }
        LxVmProtectRange(Page, End, LxVmaIsCow(Vma) ? PAGE_WRITECOPY : PAGE_READWRITE);
        Error = (LONG)LxZeroUser(Address, (SIZE_T)(End - Address));
        LxVmProtectRange(Page, End, LxVmaNtProtect(Vma));
    }
    LxMmUnlock(Process);
    return (Error < 0) ? Error : 0;
}

static
LONG
LxMmUnmapRange(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    LONG Error = LxVmaRemove(Process, Start, End);

    if (Error == 0)
    {
        LxVmDecommitRange(Start, End);
        LxVmReleaseRange(Process, Start, End);
    }
    return Error;
}

static
BOOLEAN
LxVmIsFree(
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    MEMORY_BASIC_INFORMATION Information;

    return NT_SUCCESS(LxVmQuery(Start, &Information)) &&
           (Information.State == MEM_FREE) &&
           ((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize >= End);
}

static
LONG
LxMmMapView(
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Prot,
    _In_ BOOLEAN Fixed,
    _In_opt_ PLX_FILE File,
    _In_ ULONG64 Offset,
    _Out_ PULONG64 Result,
    _Out_ PVOID *SectionObject,
    _Out_ PULONG64 GrowBase)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    FILE_STANDARD_INFORMATION Standard;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER MaximumSize, SectionOffset;
    ULONG64 Delta = Offset & (LX_ALLOC_GRANULARITY - 1);
    BOOLEAN Writable = (File == NULL) || ((File->Flags & LX_O_ACCMODE) == LX_O_RDWR);
    BOOLEAN Grow = FALSE;
    SIZE_T ViewSize = (SIZE_T)(Length + Delta);
    PVOID Base = NULL;
    HANDLE Section;
    NTSTATUS Status;

    *SectionObject = NULL;
    *GrowBase = 0;
    if (Prot & LX_PROT_EXEC)
        return 1;
    if ((Prot & LX_PROT_WRITE) && !Writable)
        return -LX_EACCES;

    if (File != NULL)
    {
        if (File->Handle == NULL)
            return 2;
        Status = ZwQueryInformationFile(File->Handle, &Iosb, &Standard, sizeof(Standard), FileStandardInformation);
        if (!NT_SUCCESS(Status) || (Standard.EndOfFile.QuadPart < 0))
        {
            DPRINT1("Linux shared mapping cannot query the file size: 0x%08lx\n", Status);
            return 3;
        }
        Grow = (Offset + Length > LX_ALIGN_UP((ULONG64)Standard.EndOfFile.QuadPart, LX_PAGE_SIZE));
        ViewSize = ((ULONG64)Standard.EndOfFile.QuadPart > Offset - Delta) ?
                   (SIZE_T)min(Length + Delta, (ULONG64)Standard.EndOfFile.QuadPart - (Offset - Delta)) : 0;
    }

    if (Fixed)
    {
        if ((Address - Delta) & (LX_ALLOC_GRANULARITY - 1))
            return 4;
        if (!LxVmIsFree(Address - Delta, Address + Length))
            return 5;
        Base = (PVOID)(ULONG_PTR)(Address - Delta);
    }
    else if ((Address >= LX_MMAP_MIN_ADDR + Delta) &&
             !((Address - Delta) & (LX_ALLOC_GRANULARITY - 1)) &&
             LxVmIsFree(Address - Delta, Address + Length))
    {
        Base = (PVOID)(ULONG_PTR)(Address - Delta);
    }

    if (Grow)
    {
        ULONG64 Region = LX_ALIGN_UP(Length + Delta, LX_ALLOC_GRANULARITY), Tail;

        if (Base == NULL)
        {
            ULONG64 Probe = 0;

            if (!NT_SUCCESS(LxVmAllocate(&Probe, Region, MEM_RESERVE, PAGE_NOACCESS)))
                return -LX_ENOMEM;
            LxVmFree(Probe, 0, MEM_RELEASE);
            Base = (PVOID)(ULONG_PTR)Probe;
        }
        if (ViewSize != 0)
        {
            InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
            Status = ZwCreateSection(&Section, SECTION_ALL_ACCESS, &ObjectAttributes, NULL,
                                     Writable ? PAGE_READWRITE : PAGE_READONLY, SEC_COMMIT, File->Handle);
            if (!NT_SUCCESS(Status))
                return 6;
            SectionOffset.QuadPart = (LONG64)(Offset - Delta);
            Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &Base, 0, 0, &SectionOffset, &ViewSize, ViewShare,
                                        0, (Prot & LX_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
            ZwClose(Section);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Linux shared mapping cannot map a view at %p: 0x%08lx\n", Base, Status);
                return 7;
            }
            if ((Prot & (LX_PROT_READ | LX_PROT_WRITE)) == LX_PROT_NONE)
                LxVmProtect((ULONG64)(ULONG_PTR)Base, ViewSize, PAGE_NOACCESS);
        }
        Tail = LX_ALIGN_UP((ULONG64)(ULONG_PTR)Base + ViewSize, LX_ALLOC_GRANULARITY);
        if ((Tail < (ULONG64)(ULONG_PTR)Base + Region) &&
            !NT_SUCCESS(LxVmAllocate(&Tail, (ULONG64)(ULONG_PTR)Base + Region - Tail, MEM_RESERVE, PAGE_NOACCESS)))
        {
            if (ViewSize != 0)
                ZwUnmapViewOfSection(NtCurrentProcess(), Base);
            return -LX_ENOMEM;
        }
        *Result = (ULONG64)(ULONG_PTR)Base + Delta;
        *GrowBase = (ULONG64)(ULONG_PTR)Base;
        return 0;
    }

    MaximumSize.QuadPart = (LONG64)Length;
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Section,
                             SECTION_ALL_ACCESS,
                             &ObjectAttributes,
                             (File == NULL) ? &MaximumSize : NULL,
                             Writable ? PAGE_READWRITE : PAGE_READONLY,
                             SEC_COMMIT,
                             (File != NULL) ? File->Handle : NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Linux shared mapping cannot create a section: 0x%08lx\n", Status);
        return 6;
    }

    SectionOffset.QuadPart = (LONG64)(Offset - Delta);
    Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &Base, 0, 0, &SectionOffset, &ViewSize, ViewShare, 0,
                                (Prot & LX_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    if (!NT_SUCCESS(Status) && !Fixed && (Base != NULL))
    {
        Base = NULL;
        Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &Base, 0, 0, &SectionOffset, &ViewSize, ViewShare, 0,
                                    (Prot & LX_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    }
    if (NT_SUCCESS(Status) && (File == NULL) &&
        !NT_SUCCESS(ObReferenceObjectByHandle(Section, 0, NULL, KernelMode, SectionObject, NULL)))
    {
        *SectionObject = NULL;
    }
    ZwClose(Section);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Linux shared mapping cannot map a view at %p: 0x%08lx\n", Base, Status);
        return 7;
    }

    if ((Prot & (LX_PROT_READ | LX_PROT_WRITE)) == LX_PROT_NONE)
        LxVmProtect((ULONG64)(ULONG_PTR)Base, Length + Delta, PAGE_NOACCESS);
    *Result = (ULONG64)(ULONG_PTR)Base + Delta;
    return 0;
}

LONG64
LxMmMapSection(
    _In_ PLX_PROCESS Process,
    _In_ PVOID Section,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Prot,
    _In_ BOOLEAN Remap)
{
    LARGE_INTEGER SectionOffset;
    SIZE_T ViewSize = (SIZE_T)Length;
    PVOID Base = (PVOID)(ULONG_PTR)Address;
    HANDLE Handle;
    NTSTATUS Status;
    LONG Error = 0;

    Process = Process->Mm;
    if (Address & (LX_ALLOC_GRANULARITY - 1))
        return -LX_EINVAL;
    Status = ObOpenObjectByPointer(Section, OBJ_KERNEL_HANDLE, NULL, SECTION_MAP_READ | SECTION_MAP_WRITE,
                                   NULL, KernelMode, &Handle);
    if (!NT_SUCCESS(Status))
        return -LX_ENOMEM;

    LxMmLock(Process);
    if ((Address != 0) && LxVmaOverlaps(Process, Address, Address + Length))
        Error = Remap ? LxMmUnmapRange(Process, Address, Address + Length) : -LX_EINVAL;
    if ((Error == 0) && (Address != 0) && !LxVmIsFree(Address, Address + Length))
        Error = -LX_EINVAL;
    if (Error == 0)
    {
        SectionOffset.QuadPart = 0;
        Status = ZwMapViewOfSection(Handle, NtCurrentProcess(), &Base, 0, 0, &SectionOffset, &ViewSize, ViewShare, 0,
                                    (Prot & LX_PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
        if (!NT_SUCCESS(Status))
            Error = -LX_ENOMEM;
    }
    if (Error == 0)
    {
        Error = LxVmaInsert(Process, (ULONG64)(ULONG_PTR)Base, (ULONG64)(ULONG_PTR)Base + Length, Prot,
                            LX_MAP_SHARED | LX_VMA_VIEW | LX_VMA_SHM, NULL, 0, Section);
        if (Error != 0)
            ZwUnmapViewOfSection(NtCurrentProcess(), Base);
    }
    LxMmUnlock(Process);
    ZwClose(Handle);
    return (Error != 0) ? Error : (LONG64)(ULONG_PTR)Base;
}

LONG
LxMmDetachSection(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address)
{
    PLX_VMA Vma;
    PLIST_ENTRY Entry;
    ULONG64 End;
    LONG Error = -LX_EINVAL;

    Process = Process->Mm;
    LxMmLock(Process);
    Vma = LxVmaFind(Process, Address);
    if ((Vma != NULL) && (Vma->Start == Address) && (Vma->Flags & LX_VMA_SHM) && (Vma->Offset == 0))
    {
        End = Vma->End;
        for (Entry = Vma->Link.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
        {
            PLX_VMA Next = CONTAINING_RECORD(Entry, LX_VMA, Link);

            if ((Next->Start != End) || (Next->Section != Vma->Section) || !(Next->Flags & LX_VMA_SHM))
                break;
            End = Next->End;
        }
        Error = LxMmUnmapRange(Process, Address, End);
    }
    LxMmUnlock(Process);
    return Error;
}

static
LONG
LxMmMapCow(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Prot,
    _In_ ULONG Flags,
    _In_ PLX_FILE File,
    _In_ ULONG64 Offset,
    _Out_ PULONG64 Result)
{
    BOOLEAN Fixed = (Flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) != 0;
    ULONG64 Delta = Offset & (LX_ALLOC_GRANULARITY - 1), Base = 0, ViewEnd, Probe;
    FILE_STANDARD_INFORMATION Standard;
    IO_STATUS_BLOCK Iosb;
    PVOID Section;
    NTSTATUS Status;
    LONG Error;

    if ((File->Handle == NULL) || (File->Type != LX_S_IFREG) || ((File->Flags & LX_O_ACCMODE) == LX_O_WRONLY))
        return 1;
    Status = ZwQueryInformationFile(File->Handle, &Iosb, &Standard, sizeof(Standard), FileStandardInformation);
    if (!NT_SUCCESS(Status) || (Standard.EndOfFile.QuadPart <= 0) || ((ULONG64)Standard.EndOfFile.QuadPart <= Offset))
        return 1;

    if (Fixed)
    {
        if ((Address - Delta) & (LX_ALLOC_GRANULARITY - 1))
            return 1;
        Base = Address - Delta;
    }
    else
    {
        if ((Address != 0) && !((Address - Delta) & (LX_ALLOC_GRANULARITY - 1)) &&
            (Address - Delta >= LX_MMAP_MIN_ADDR) && (Address + Length > Address) &&
            !LxVmaOverlaps(Process, Address - Delta, LX_ALIGN_UP(Address + Length, LX_ALLOC_GRANULARITY)) &&
            LxVmIsFree(Address - Delta, LX_ALIGN_UP(Address + Length, LX_ALLOC_GRANULARITY)))
        {
            Base = Address - Delta;
        }
        else
        {
            Probe = 0;
            if (!NT_SUCCESS(LxVmAllocate(&Probe, LX_ALIGN_UP(Delta + Length, LX_ALLOC_GRANULARITY), MEM_RESERVE,
                                         PAGE_NOACCESS)))
            {
                return -LX_ENOMEM;
            }
            LxVmFree(Probe, 0, MEM_RELEASE);
            Base = Probe;
        }
        Address = Base + Delta;
    }
    ViewEnd = Address + min(Length, LX_ALIGN_UP((ULONG64)Standard.EndOfFile.QuadPart, LX_PAGE_SIZE) - Offset);

    Status = LxMmCreateCowSection(File, &Section);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Linux file mapping of '%s' is a private copy: 0x%08lx\n", File->Path, Status);
        return 1;
    }

    if (Fixed)
    {
        Error = LxVmaRemove(Process, Address, Address + Length);
        if (Error == 0)
            Error = LxMmClearForView(Process, Base, LX_ALIGN_UP(ViewEnd, LX_ALLOC_GRANULARITY));
        if (Error != 0)
        {
            ObDereferenceObject(Section);
            return (Error < 0) ? Error : 1;
        }
    }

    Status = LxMmMapCowView(Section, Base, Offset - Delta, ViewEnd - Base, LxCowViewMaximum(Prot), LxCowProtToNt(Prot));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Linux file mapping of '%s' cannot map a view at %I64x: 0x%08lx\n", File->Path, Base, Status);
        ObDereferenceObject(Section);
        return 1;
    }
    if (Delta != 0)
        LxVmProtect(Base, Delta, PAGE_NOACCESS);
    if (Address + Length > LX_ALIGN_UP(ViewEnd, LX_ALLOC_GRANULARITY))
        LxVmPlace(Process, LX_ALIGN_UP(ViewEnd, LX_ALLOC_GRANULARITY), Address + Length, LX_PLACE_FRESH | LX_PLACE_SPLIT);

    Error = LxVmaInsertView(Process, Address, Address + Length, Prot,
                            LX_MAP_PRIVATE | LX_VMA_VIEW | (Flags & (LX_VMA_HEAP | LX_VMA_STACK)), File, Offset, Section,
                            Base, Offset - Delta, 0);
    ObDereferenceObject(Section);
    if (Error != 0)
    {
        ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Base);
        return Error;
    }
    *Result = Address;
    return 0;
}

LONG64
LxMmMap(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Prot,
    _In_ ULONG Flags,
    _In_opt_ PLX_FILE File,
    _In_ ULONG64 Offset)
{
    BOOLEAN Fixed = (Flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) != 0;
    ULONG VmaFlags = Flags & (LX_MAP_SHARED | LX_MAP_PRIVATE | LX_VMA_HEAP | LX_VMA_STACK | LX_VMA_NOMAYWRITE);
    PLX_PROCESS Caller = Process;
    BOOLEAN Fresh = FALSE;
    ULONG64 Base = 0, GrowBase = 0;
    PVOID Section = NULL;
    LONG Error = 0;
    ULONG Lock;

    Process = Process->Mm;
    if ((Length == 0) || (Offset & (LX_PAGE_SIZE - 1)))
        return -LX_EINVAL;
    if (Fixed && (Address & (LX_PAGE_SIZE - 1)))
        return -LX_EINVAL;

    Length = LX_ALIGN_UP(Length, LX_PAGE_SIZE);
    if ((Length == 0) || (Fixed && (Address + Length < Address)))
        return -LX_ENOMEM;
    if (Fixed && (Address < LX_MMAP_MIN_ADDR))
        return -LX_EPERM;
    if ((Flags & LX_MAP_LOCKED) && !LxMmCanLock(Caller))
        return -LX_EPERM;

    LxMmLock(Process);
    Lock = Process->MmLockDefault | ((Flags & LX_MAP_LOCKED) ? LX_VMA_LOCKED : 0);
    if ((Lock != 0) && !LxMmLockAllowed(Caller, Process, Length))
    {
        LxMmUnlock(Process);
        return -LX_EAGAIN;
    }
    VmaFlags |= Lock;

    if ((VmaFlags & LX_MAP_SHARED) &&
        !((Flags & LX_MAP_FIXED_NOREPLACE) && LxVmaOverlaps(Process, Address, Address + Length)))
    {
        if (Fixed)
            Error = LxMmUnmapRange(Process, Address, Address + Length);
        if (Error == 0)
            Error = LxMmMapView(Address, Length, Prot, Fixed, File, Offset, &Base, &Section, &GrowBase);
        if (Error == 0)
        {
            Error = LxVmaInsertView(Process, Base, Base + Length, Prot, VmaFlags | LX_VMA_VIEW, File, Offset, Section,
                                    GrowBase, (GrowBase != 0) ? (Offset - (Base - GrowBase)) : 0,
                                    (GrowBase != 0) ? (Base + Length) : 0);
            if (Section != NULL)
                ObDereferenceObject(Section);
            if (Error != 0)
                ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)LX_ALIGN_DOWN(Base, LX_ALLOC_GRANULARITY));
            else if (Lock == LX_VMA_LOCKED)
                LxMmLockPopulate(Process, Base, Base + Length);
            LxMmUnlock(Process);
            return (Error != 0) ? Error : (LONG64)Base;
        }
        if (Error < 0)
        {
            LxMmUnlock(Process);
            return Error;
        }
        DPRINT1("Linux shared mapping of '%s' in process %lu is a private copy (reason %ld)\n",
                (File != NULL) ? File->Path : "anonymous memory", Process->Pid, Error);
        Error = 0;
        Base = 0;
    }
    else if ((File != NULL) && !(Prot & LX_PROT_WRITE) &&
             !((Flags & LX_MAP_FIXED_NOREPLACE) && LxVmaOverlaps(Process, Address, Address + Length)))
    {
        Error = LxMmMapCow(Process, Address, Length, Prot, Flags, File, Offset, &Base);
        if ((Error == 0) && (Lock != 0))
        {
            LxVmaSetLock(Process, Base, Base + Length, Lock);
            if (Lock == LX_VMA_LOCKED)
                LxMmLockPopulate(Process, Base, Base + Length);
        }
        if (Error <= 0)
        {
            LxMmUnlock(Process);
            return (Error != 0) ? Error : (LONG64)Base;
        }
        Error = 0;
        Base = 0;
    }

    if (Fixed)
    {
        if ((Flags & LX_MAP_FIXED_NOREPLACE) && LxVmaOverlaps(Process, Address, Address + Length))
            Error = -LX_EEXIST;
        else
            Error = LxVmaRemove(Process, Address, Address + Length);
        if (Error == 0)
            Error = LxVmPlace(Process, Address, Address + Length, LX_PLACE_FRESH | LX_PLACE_SPLIT);
        Base = Address;
    }
    else
    {
        if (Address != 0)
        {
            ULONG64 Hint = LX_ALIGN_UP(Address, LX_PAGE_SIZE);

            if ((Hint >= LX_MMAP_MIN_ADDR) && (Hint + Length > Hint) &&
                !LxVmaOverlaps(Process, Hint, Hint + Length) &&
                (LxVmPlace(Process, Hint, Hint + Length, LX_PLACE_FRESH) == 0))
            {
                Base = Hint;
            }
        }
        if (Base == 0)
        {
            if (NT_SUCCESS(LxVmAllocate(&Base,
                                        LX_ALIGN_UP(Length, LX_ALLOC_GRANULARITY),
                                        MEM_RESERVE | ((Flags & LX_VMA_STACK) ? MEM_TOP_DOWN : 0),
                                        PAGE_NOACCESS)))
            {
                Fresh = TRUE;
            }
            else
            {
                Error = -LX_ENOMEM;
            }
        }
    }

    if (Error == 0)
    {
        Error = LxVmaInsert(Process, Base, Base + Length, Prot, VmaFlags, File, Offset, NULL);
        if ((Error != 0) && Fresh)
            LxVmFree(Base, 0, MEM_RELEASE);
    }
    if ((Error == 0) && (File != NULL))
    {
        Error = LxMmFillPrivateFile(Process, Base, Base + Length);
        if (Error != 0)
            LxMmUnmapRange(Process, Base, Base + Length);
    }
    if (Error != 0)
    {
        LxMmUnlock(Process);
        return Error;
    }

    if (Lock == LX_VMA_LOCKED)
        LxMmLockPopulate(Process, Base, Base + Length);
    else if ((Flags & LX_MAP_POPULATE) && (Prot != LX_PROT_NONE))
        LxMmPopulate(Process, Base, Base + Length, LX_PROT_READ, FALSE);

    LxMmUnlock(Process);
    return (LONG64)Base;
}

LONG64
LxMmUnmap(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length)
{
    ULONG64 End;
    LONG Error;

    Process = Process->Mm;
    if ((Address & (LX_PAGE_SIZE - 1)) || (Length == 0))
        return -LX_EINVAL;

    Length = LX_ALIGN_UP(Length, LX_PAGE_SIZE);
    End = Address + Length;
    if ((Length == 0) || (End < Address))
        return -LX_EINVAL;

    LxMmLock(Process);
    Error = LxMmUnmapRange(Process, Address, End);
    LxMmUnlock(Process);
    return Error;
}

LONG64
LxMmProtect(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Prot)
{
    MEMORY_BASIC_INFORMATION Information;
    PLIST_ENTRY Entry;
    ULONG64 Cursor, End;
    BOOLEAN Upgraded;
    LONG Error;

    Process = Process->Mm;
    if (Address & (LX_PAGE_SIZE - 1))
        return -LX_EINVAL;
    if (Length == 0)
        return 0;

    End = Address + LX_ALIGN_UP(Length, LX_PAGE_SIZE);
    if (End <= Address)
        return -LX_ENOMEM;

    LxMmLock(Process);
    Error = LxVmaCovers(Process, Address, End) ? 0 : -LX_ENOMEM;
    for (Entry = Process->Vmas.Flink; (Error == 0) && (Prot & LX_PROT_WRITE) && (Entry != &Process->Vmas);
         Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if ((Vma->Start < End) && (Vma->End > Address) && (Vma->Flags & LX_VMA_NOMAYWRITE))
            Error = -LX_EACCES;
    }
    if (Error == 0)
        Error = LxVmaSetProt(Process, Address, End, Prot);

    for (Entry = Process->Vmas.Flink; (Error == 0) && (Entry != &Process->Vmas); Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);
        ULONG NtProtect;

        if (Vma->Start >= End)
            break;
        if (Vma->End <= Address)
            continue;
        if (LxVmaIsCow(Vma) && (Prot & LX_PROT_WRITE))
            Vma->Flags |= LX_VMA_DIRTY;
        NtProtect = LxVmaNtProtect(Vma);
        Upgraded = FALSE;

        for (Cursor = max(Vma->Start, Address); (Error == 0) && (Cursor < min(Vma->End, End));)
        {
            ULONG64 RegionEnd;
            NTSTATUS Status;

            if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
                break;
            RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, min(Vma->End, End));
            if ((Information.State == MEM_COMMIT) && LxVmaBacked(Vma, &Information))
            {
                if (LxVmaIsCow(Vma) && !Upgraded &&
                    ((LxViewAccess(Information.AllocationProtect) & LxViewAccess(NtProtect)) != LxViewAccess(NtProtect)))
                {
                    Upgraded = TRUE;
                    Error = LxMmUpgradeView(Process, Vma->ViewBase);
                    continue;
                }
                Status = LxVmProtect(Cursor, RegionEnd - Cursor, NtProtect);
                if (!NT_SUCCESS(Status))
                    Error = -LxStatusToErrno(Status);
                else if (Prot & LX_PROT_EXEC)
                    LxVmFlushCode(Cursor, RegionEnd - Cursor);
            }
            Cursor = RegionEnd;
        }
    }
    if (Error == 0)
        LxVmaMerge(Process, Address, End);
    LxMmUnlock(Process);
    return Error;
}

VOID
LxMmRelease(
    _In_ PLX_PROCESS Process)
{
    while (!IsListEmpty(&Process->Vmas))
        LxVmaFree(CONTAINING_RECORD(RemoveHeadList(&Process->Vmas), LX_VMA, Link));
}

LONG
LxMmFork(
    _In_ PLX_PROCESS Child,
    _In_ PLX_PROCESS Parent)
{
    PLIST_ENTRY Entry;

    Parent = Parent->Mm;
    for (Entry = Parent->Vmas.Flink; Entry != &Parent->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);
        PLX_VMA Copy = LxAlloc(sizeof(*Copy));

        if (Copy == NULL)
        {
            LxMmRelease(Child);
            return -LX_ENOMEM;
        }
        Copy->Start = Vma->Start;
        Copy->End = Vma->End;
        Copy->Prot = Vma->Prot;
        Copy->Flags = Vma->Flags & ~LX_VMA_LOCK_MASK;
        Copy->File = Vma->File;
        Copy->Offset = Vma->Offset;
        Copy->Section = Vma->Section;
        Copy->ViewBase = Vma->ViewBase;
        Copy->ViewOffset = Vma->ViewOffset;
        Copy->MapEnd = Vma->MapEnd;
        if (Copy->File != NULL)
            LxFileReference(Copy->File);
        if (Copy->Section != NULL)
            ObReferenceObject(Copy->Section);
        if ((Copy->Flags & LX_VMA_SHM) && (Copy->Section != NULL))
            LxIpcShmVmaOpen(Copy->Section);
        InsertTailList(&Child->Vmas, &Copy->Link);
    }
    return 0;
}

VOID
LxMmReset(
    _In_ PLX_PROCESS Process)
{
    MEMORY_BASIC_INFORMATION Information;
    PLIST_ENTRY Entry;
    ULONG64 Cursor = 0;

    LxMmLock(Process);
    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (LxVmaIsCow(Vma))
        {
            if (Vma->ViewBase != 0)
                ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)(ULONG_PTR)Vma->ViewBase);
        }
        else if (!(Vma->Flags & LX_VMA_VIEW) && !(Vma->Start & (LX_ALLOC_GRANULARITY - 1)))
        {
            LxVmFree(Vma->Start, 0, MEM_RELEASE);
        }
    }

    while (NT_SUCCESS(LxVmQuery(Cursor, &Information)))
    {
        ULONG64 Next = (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize;

        if ((Information.State != MEM_FREE) && (Information.Type == MEM_PRIVATE))
            LxVmFree((ULONG64)(ULONG_PTR)Information.AllocationBase, 0, MEM_RELEASE);
        else if ((Information.State != MEM_FREE) && (Information.Type == MEM_MAPPED))
            ZwUnmapViewOfSection(NtCurrentProcess(), Information.AllocationBase);
        if (Next <= Cursor)
            break;
        Cursor = Next;
    }

    LxMmRelease(Process);
    Process->BrkBase = 0;
    Process->BrkCurrent = 0;
    Process->BrkCommitted = 0;
    Process->BrkLimit = 0;
    Process->BrkReserved = FALSE;
    Process->MmLockDefault = 0;
    LxMmUnlock(Process);
}

LX_SYSCALL(LxSysMmap)
{
    ULONG Flags = (ULONG)Call->Args[3] & ~(LX_VMA_VIEW | LX_VMA_HEAP | LX_VMA_STACK | LX_VMA_NOMAYWRITE);
    ULONG Type = Flags & LX_MAP_TYPE;
    ULONG Prot = (ULONG)Call->Args[2];
    ULONG Access = 0;
    PLX_FILE File = NULL;
    BOOLEAN Zero = FALSE;
    LONG64 Result = 0;

    if (Call->Args[5] & (LX_PAGE_SIZE - 1))
        return -LX_EINVAL;
    if (!(Flags & LX_MAP_ANONYMOUS))
    {
        File = LxFdGet(Call->Process, (LONG)Call->Args[4]);
        if (File == NULL)
            return -LX_EBADF;
        Access = File->Flags & LX_O_ACCMODE;
    }

    if ((Call->Args[1] == 0) ||
        ((Type != LX_MAP_SHARED) && (Type != LX_MAP_PRIVATE) && (Type != LX_MAP_SHARED_VALIDATE)))
    {
        Result = -LX_EINVAL;
    }
    else if ((Type == LX_MAP_SHARED_VALIDATE) &&
             (Flags & ~(LX_MAP_TYPE | LX_MAP_FIXED | LX_MAP_ANONYMOUS | LX_MAP_GROWSDOWN | LX_MAP_DENYWRITE |
                        LX_MAP_EXECUTABLE | LX_MAP_LOCKED | LX_MAP_NORESERVE | LX_MAP_POPULATE | LX_MAP_NONBLOCK |
                        LX_MAP_STACK | LX_MAP_HUGETLB | LX_MAP_FIXED_NOREPLACE | LX_MAP_UNINITIALIZED |
                        LX_MAP_HUGE_MASK)))
    {
        Result = -LX_EOPNOTSUPP;
    }
    else if ((File != NULL) &&
             ((Access == LX_O_WRONLY) || ((Type != LX_MAP_PRIVATE) && (Prot & LX_PROT_WRITE) && (Access != LX_O_RDWR))))
    {
        Result = -LX_EACCES;
    }
    else if ((File != NULL) && (((File->Handle == NULL) && (File->Data == NULL)) || File->Directory || File->NtPipe))
    {
        Zero = (File->Type == LX_S_IFCHR) && (File->Rdev == ((1 << 8) | 5));
        if (!Zero)
            Result = -LX_ENODEV;
    }

    if (Result == 0)
    {
        if (Type == LX_MAP_SHARED_VALIDATE)
            Flags = (Flags & ~LX_MAP_TYPE) | LX_MAP_SHARED;
        if ((File != NULL) && (Type != LX_MAP_PRIVATE) && (Access != LX_O_RDWR))
            Flags |= LX_VMA_NOMAYWRITE;
        Result = LxMmMap(Call->Process, Call->Args[0], Call->Args[1], Prot, Flags, Zero ? NULL : File, Call->Args[5]);
    }
    if (File != NULL)
        LxFileDereference(File);
    return Result;
}

LX_SYSCALL(LxSysMunmap)
{
    return LxMmUnmap(Call->Process, Call->Args[0], Call->Args[1]);
}

LX_SYSCALL(LxSysMprotect)
{
    return LxMmProtect(Call->Process, Call->Args[0], Call->Args[1], (ULONG)Call->Args[2]);
}

LX_SYSCALL(LxSysPkeyAlloc)
{
    if ((Call->Args[0] != 0) || (Call->Args[1] & ~(ULONG64)LX_PKEY_ACCESS_MASK))
        return -LX_EINVAL;
    return -LX_ENOSPC;
}

LX_SYSCALL(LxSysPkeyFree)
{
    UNREFERENCED_PARAMETER(Call);
    return -LX_EINVAL;
}

LX_SYSCALL(LxSysPkeyMprotect)
{
    if ((LONG)Call->Args[3] != -1)
    {
        if (Call->Args[0] & (LX_PAGE_SIZE - 1))
            return -LX_EINVAL;
        if (Call->Args[1] == 0)
            return 0;
        if (Call->Args[0] + LX_ALIGN_UP(Call->Args[1], LX_PAGE_SIZE) <= Call->Args[0])
            return -LX_ENOMEM;
        return -LX_EINVAL;
    }
    return LxMmProtect(Call->Process, Call->Args[0], Call->Args[1], (ULONG)Call->Args[2]);
}

LONG
LxMmPrepareRemote(
    _In_ PLX_PROCESS Process,
    _In_ PEPROCESS EProcess,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ BOOLEAN Write)
{
    KAPC_STATE ApcState;
    LONG Result;

    Process = Process->Mm;
    KeStackAttachProcess((PKPROCESS)EProcess, &ApcState);
    LxMmLock(Process);
    Result = LxMmPopulate(Process, LX_ALIGN_DOWN(Address, LX_PAGE_SIZE), LX_ALIGN_UP(Address + Length, LX_PAGE_SIZE),
                          Write ? LX_PROT_WRITE : LX_PROT_READ, FALSE);
    LxMmUnlock(Process);
    KeUnstackDetachProcess(&ApcState);
    return (Result == -LX_ENXIO) ? -LX_EFAULT : ((Result < 0) ? Result : 0);
}

LX_SYSCALL(LxSysMadvise)
{
    PLX_PROCESS Process = Call->Process->Mm;
    ULONG64 Address = Call->Args[0];
    ULONG64 End = Address + LX_ALIGN_UP(Call->Args[1], LX_PAGE_SIZE);
    ULONG Advice = (ULONG)Call->Args[2];
    PLIST_ENTRY Entry;
    LONG64 Result = 0;

    if ((Address & (LX_PAGE_SIZE - 1)) || (End < Address))
        return -LX_EINVAL;
    if (End == Address)
        return 0;

    LxMmLock(Process);
    if ((Advice == LX_MADV_DONTNEED) && (LxMmLockedIn(Process, Address, End) != 0))
    {
        Result = -LX_EINVAL;
    }
    else if (!LxVmaCovers(Process, Address, End))
    {
        Result = -LX_ENOMEM;
    }
    else if ((Advice == LX_MADV_DONTNEED) || (Advice == LX_MADV_FREE))
    {
        for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
        {
            PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

            if (Vma->Start >= End)
                break;
            if ((Vma->End <= Address) || (Vma->Flags & LX_MAP_SHARED))
                continue;
            if ((Advice == LX_MADV_FREE) && (Vma->File != NULL))
                continue;
            if (LxVmaIsCow(Vma))
            {
                ULONG64 From = max(Vma->Start, Address), To = min(Vma->End, End);

                if (Vma->ViewBase != 0)
                    To = min(To, LxVmAllocationEnd(Vma->ViewBase));
                if ((Vma->Flags & LX_VMA_DIRTY) && (Vma->ViewBase != 0) && (From < To))
                {
                    LxVmProtect(From, To - From, PAGE_WRITECOPY);
                    LxMmFill(From, To - From, Vma->File, Vma->Offset + (From - Vma->Start));
                    LxVmProtect(From, To - From, LxCowProtToNt(Vma->Prot));
                }
                continue;
            }
            LxVmDecommitRange(max(Vma->Start, Address), min(Vma->End, End));
            if ((Vma->File != NULL) && !(Vma->Flags & LX_VMA_VIEW))
                LxMmPopulateRun(Vma, max(Vma->Start, Address), min(Vma->End, End));
        }
    }
    LxMmUnlock(Process);
    return Result;
}

LX_SYSCALL(LxSysMsync)
{
    PLX_PROCESS Process = Call->Process->Mm;
    ULONG64 Address = Call->Args[0];
    ULONG64 End = Address + LX_ALIGN_UP(Call->Args[1], LX_PAGE_SIZE);
    ULONG Flags = (ULONG)Call->Args[2];
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 Cursor;
    LONG64 Result = 0;

    if ((Address & (LX_PAGE_SIZE - 1)) || (End < Address) ||
        (Flags & ~(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC)) ||
        ((Flags & LX_MS_ASYNC) && (Flags & LX_MS_SYNC)))
    {
        return -LX_EINVAL;
    }
    if (End == Address)
        return 0;

    LxMmLock(Process);
    if ((Flags & LX_MS_INVALIDATE) && (LxMmLockedIn(Process, Address, End) != 0))
        Result = -LX_EBUSY;
    else if (!LxVmaCovers(Process, Address, End))
        Result = -LX_ENOMEM;
    for (Cursor = Address; (Result == 0) && (Flags & LX_MS_SYNC) && (Cursor < End);)
    {
        ULONG64 RegionEnd;

        if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            break;
        RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, End);
        if ((Information.State == MEM_COMMIT) && (Information.Type == MEM_MAPPED))
        {
            PVOID Base = (PVOID)(ULONG_PTR)Cursor;
            SIZE_T Size = (SIZE_T)(RegionEnd - Cursor);
            IO_STATUS_BLOCK Iosb;

            if (!NT_SUCCESS(ZwFlushVirtualMemory(NtCurrentProcess(), &Base, &Size, &Iosb)))
                Result = -LX_EIO;
        }
        Cursor = RegionEnd;
    }
    LxMmUnlock(Process);
    return Result;
}

static
LONG64
LxMmLockRange(
    _In_ PLX_PROCESS Caller,
    _In_ ULONG64 Address,
    _In_ ULONG64 Length,
    _In_ ULONG Lock)
{
    PLX_PROCESS Process = Caller->Mm;
    ULONG64 Start = LX_ALIGN_DOWN(Address, LX_PAGE_SIZE);
    ULONG64 End = Start + LX_ALIGN_UP(Length + (Address - Start), LX_PAGE_SIZE);
    LONG Error;

    if ((Lock != 0) && !LxMmCanLock(Caller))
        return -LX_EPERM;
    if (End < Start)
        return -LX_EINVAL;
    if (End == Start)
        return 0;

    LxMmLock(Process);
    if (!LxVmaCovers(Process, Start, End))
        Error = -LX_ENOMEM;
    else if ((Lock != 0) && !LxMmLockAllowed(Caller, Process, (End - Start) - LxMmLockedIn(Process, Start, End)))
        Error = -LX_ENOMEM;
    else
        Error = LxVmaSetLock(Process, Start, End, Lock);
    if ((Error == 0) && (Lock == LX_VMA_LOCKED))
        LxMmLockPopulate(Process, Start, End);
    LxMmUnlock(Process);
    return Error;
}

static
VOID
LxMmSetLockAll(
    _In_ PLX_PROCESS Process,
    _In_ ULONG Lock)
{
    PLIST_ENTRY Entry;

    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        Vma->Flags = (Vma->Flags & ~LX_VMA_LOCK_MASK) | Lock;
    }
    LxVmaMerge(Process, 0, MAXULONG64);
}

LX_SYSCALL(LxSysMlock)
{
    return LxMmLockRange(Call->Process, Call->Args[0], Call->Args[1], LX_VMA_LOCKED);
}

LX_SYSCALL(LxSysMlock2)
{
    ULONG Flags = (ULONG)Call->Args[2];

    if (Flags & ~LX_MLOCK_ONFAULT)
        return -LX_EINVAL;
    return LxMmLockRange(Call->Process, Call->Args[0], Call->Args[1],
                         LX_VMA_LOCKED | ((Flags & LX_MLOCK_ONFAULT) ? LX_VMA_LOCKONFAULT : 0));
}

LX_SYSCALL(LxSysMunlock)
{
    return LxMmLockRange(Call->Process, Call->Args[0], Call->Args[1], 0);
}

LX_SYSCALL(LxSysMlockall)
{
    PLX_PROCESS Process = Call->Process->Mm;
    ULONG Flags = (ULONG)Call->Args[0];
    ULONG Lock = LX_VMA_LOCKED | ((Flags & LX_MCL_ONFAULT) ? LX_VMA_LOCKONFAULT : 0);
    PLIST_ENTRY Entry;
    ULONG64 Total = 0;

    if (!(Flags & (LX_MCL_CURRENT | LX_MCL_FUTURE)) || (Flags & ~(LX_MCL_CURRENT | LX_MCL_FUTURE | LX_MCL_ONFAULT)))
        return -LX_EINVAL;
    if (!LxMmCanLock(Call->Process))
        return -LX_EPERM;

    LxMmLock(Process);
    if (Flags & LX_MCL_CURRENT)
    {
        for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
        {
            PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

            Total += Vma->End - Vma->Start;
        }
        if (!LxMmLockAllowed(Call->Process, Process, Total - LxMmLockedIn(Process, 0, MAXULONG64)))
        {
            LxMmUnlock(Process);
            return -LX_ENOMEM;
        }
    }
    Process->MmLockDefault = (Flags & LX_MCL_FUTURE) ? Lock : 0;
    if (Flags & LX_MCL_CURRENT)
    {
        LxMmSetLockAll(Process, Lock);
        if (Lock == LX_VMA_LOCKED)
            LxMmLockPopulate(Process, 0, MAXULONG64);
    }
    LxMmUnlock(Process);
    return 0;
}

LX_SYSCALL(LxSysMunlockall)
{
    PLX_PROCESS Process = Call->Process->Mm;

    LxMmLock(Process);
    Process->MmLockDefault = 0;
    LxMmSetLockAll(Process, 0);
    LxMmUnlock(Process);
    return 0;
}

ULONG64
LxMmLockedBytes(
    _In_ PLX_PROCESS Process)
{
    ULONG64 Bytes;

    Process = Process->Mm;
    LxMmLock(Process);
    Bytes = LxMmLockedIn(Process, 0, MAXULONG64);
    LxMmUnlock(Process);
    return Bytes;
}

typedef struct _LX_WS_EX_ENTRY
{
    PVOID VirtualAddress;
    ULONG_PTR Attributes;
} LX_WS_EX_ENTRY;

static
VOID
LxMmResidentRun(
    _In_ HANDLE ProcessHandle,
    _In_ PLX_VMA Vma,
    _In_ ULONG64 Start,
    _In_ ULONG Pages,
    _Out_writes_(Pages) PUCHAR Vector)
{
    LX_WS_EX_ENTRY Entries[64];
    MEMORY_BASIC_INFORMATION Information;
    ULONG Index;

    if ((Vma->Flags & LX_MAP_SHARED) && (Vma->File == NULL) && (Pages <= RTL_NUMBER_OF(Entries)))
    {
        for (Index = 0; Index < Pages; Index++)
        {
            Entries[Index].VirtualAddress = (PVOID)(ULONG_PTR)(Start + Index * LX_PAGE_SIZE);
            Entries[Index].Attributes = 0;
        }
        if (NT_SUCCESS(ZwQueryVirtualMemory(ProcessHandle, NULL, MemoryWorkingSetExList, Entries,
                                            Pages * sizeof(Entries[0]), NULL)))
        {
            for (Index = 0; Index < Pages; Index++)
                Vector[Index] = (UCHAR)(Entries[Index].Attributes & 1);
            return;
        }
    }

    for (Index = 0; Index < Pages;)
    {
        ULONG64 Address = Start + Index * LX_PAGE_SIZE, RegionEnd = Address + LX_PAGE_SIZE;
        UCHAR Value = 0;

        if (NT_SUCCESS(ZwQueryVirtualMemory(ProcessHandle, (PVOID)(ULONG_PTR)Address, MemoryBasicInformation,
                                            &Information, sizeof(Information), NULL)))
        {
            Value = (Information.State == MEM_COMMIT) ? 1 : 0;
            RegionEnd = max(RegionEnd, (ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize);
        }
        for (; (Index < Pages) && (Start + Index * LX_PAGE_SIZE < RegionEnd); Index++)
            Vector[Index] = Value;
    }
}

LONG64
LxMmMincore(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address,
    _In_ ULONG64 End,
    _In_ ULONG64 Output)
{
    UCHAR Vector[64];
    LONG64 Result = 0;

    if (Address == End)
        return 0;

    Process = Process->Mm;
    LxMmLock(Process);
    if (!LxVmaCovers(Process, Address, End))
        Result = -LX_ENOMEM;
    while ((Result == 0) && (Address < End))
    {
        PLX_VMA Vma = LxVmaFind(Process, Address);
        ULONG Pages = (ULONG)min((min(Vma->End, End) - Address) / LX_PAGE_SIZE, sizeof(Vector));

        LxMmResidentRun(NtCurrentProcess(), Vma, Address, Pages, Vector);
        Result = LxCopyToUser(Output, Vector, Pages);
        Output += Pages;
        Address += Pages * LX_PAGE_SIZE;
    }
    LxMmUnlock(Process);
    return Result;
}

ULONG64
LxMmVmaResident(
    _In_ HANDLE ProcessHandle,
    _In_ PLX_VMA Vma)
{
    UCHAR Vector[64];
    ULONG64 Address, Bytes = 0;
    ULONG Pages, Index;

    for (Address = Vma->Start; Address < Vma->End; Address += Pages * LX_PAGE_SIZE)
    {
        Pages = (ULONG)min((Vma->End - Address) / LX_PAGE_SIZE, sizeof(Vector));
        LxMmResidentRun(ProcessHandle, Vma, Address, Pages, Vector);
        for (Index = 0; Index < Pages; Index++)
            Bytes += Vector[Index] ? LX_PAGE_SIZE : 0;
    }
    return Bytes;
}

LX_SYSCALL(LxSysMremap)
{
    PLX_PROCESS Process = Call->Process->Mm;
    ULONG64 Old = Call->Args[0];
    ULONG64 OldLength = LX_ALIGN_UP(Call->Args[1], LX_PAGE_SIZE);
    ULONG64 NewLength = LX_ALIGN_UP(Call->Args[2], LX_PAGE_SIZE);
    ULONG Flags = (ULONG)Call->Args[3];
    MEMORY_BASIC_INFORMATION Information;
    ULONG64 New = 0, Cursor, Offset;
    ULONG Prot, VmaFlags, Lock;
    PLX_FILE File;
    PLX_VMA Vma;
    LONG64 Result;

    if ((Old & (LX_PAGE_SIZE - 1)) || (OldLength == 0) || (NewLength == 0) || (Flags & ~LX_MREMAP_MAYMOVE))
        return -LX_EINVAL;
    if ((Old + OldLength < Old) || (Old + NewLength < Old))
        return -LX_EINVAL;

    LxMmLock(Process);
    Vma = LxVmaFind(Process, Old);
    if ((Vma == NULL) || (Old + OldLength > Vma->End))
    {
        LxMmUnlock(Process);
        return -LX_EFAULT;
    }

    if (NewLength <= OldLength)
    {
        if (NewLength < OldLength)
            LxMmUnmap(Process, Old + NewLength, OldLength - NewLength);
        LxMmUnlock(Process);
        return (LONG64)Old;
    }

    Lock = Vma->Flags & LX_VMA_LOCK_MASK;
    if ((Lock != 0) && !LxMmLockAllowed(Call->Process, Process, NewLength - OldLength))
    {
        LxMmUnlock(Process);
        return -LX_EAGAIN;
    }

    if (LxVmaIsCow(Vma))
    {
        LIST_ENTRY Saved;
        BOOLEAN Dirty = (Vma->Flags & LX_VMA_DIRTY) != 0;

        if (!(Flags & LX_MREMAP_MAYMOVE))
        {
            LxMmUnlock(Process);
            return -LX_ENOMEM;
        }
        Prot = Vma->Prot;
        File = Vma->File;
        Offset = Vma->Offset + (Old - Vma->Start);
        LxFileReference(File);
        InitializeListHead(&Saved);
        Result = Dirty ? LxMmCopySave(&Saved, Old, min(Old + OldLength, Vma->End), Prot, LxCowProtToNt(Prot)) : 0;
        if (Result == 0)
            Result = LxMmMap(Process, 0, NewLength, Prot, LX_MAP_PRIVATE, File, Offset);
        if ((Result >= 0) && !IsListEmpty(&Saved))
        {
            PLX_MM_COPY Copy = CONTAINING_RECORD(Saved.Flink, LX_MM_COPY, Link);
            PLX_VMA Moved = LxVmaFind(Process, (ULONG64)Result);

            Copy->Start = (ULONG64)Result;
            Copy->End = (ULONG64)Result + OldLength;
            if ((Moved != NULL) && LxVmaIsCow(Moved))
            {
                Moved->Flags |= LX_VMA_DIRTY;
                LxMmViewAllow(Process, Moved, PAGE_WRITECOPY);
            }
            LxMmCopyRestore(&Saved, (Moved == NULL) || !LxVmaIsCow(Moved));
        }
        while (!IsListEmpty(&Saved))
        {
            PLX_MM_COPY Copy = CONTAINING_RECORD(RemoveHeadList(&Saved), LX_MM_COPY, Link);

            LxFreePaged(Copy->Data);
            LxFree(Copy);
        }
        if (Result >= 0)
        {
            LxMmUnmap(Process, Old, OldLength);
            if (Lock != 0)
                LxVmaSetLock(Process, (ULONG64)Result, (ULONG64)Result + NewLength, Lock);
            if (Lock == LX_VMA_LOCKED)
                LxMmLockPopulate(Process, (ULONG64)Result, (ULONG64)Result + NewLength);
        }
        LxFileDereference(File);
        LxMmUnlock(Process);
        return Result;
    }
    if (Vma->Flags & LX_VMA_VIEW)
    {
        LxMmUnlock(Process);
        return -LX_ENOMEM;
    }

    Prot = Vma->Prot;
    VmaFlags = Vma->Flags;
    File = Vma->File;
    Offset = (File != NULL) ? Vma->Offset + (Old - Vma->Start) : 0;
    if (File != NULL)
        LxFileReference(File);

    if (!LxVmaOverlaps(Process, Old + OldLength, Old + NewLength) &&
        (LxVmPlace(Process, Old + OldLength, Old + NewLength, LX_PLACE_FRESH) == 0))
    {
        Result = LxVmaInsert(Process, Old + OldLength, Old + NewLength, Prot, VmaFlags, File,
                             (File != NULL) ? Offset + OldLength : 0, NULL);
        if ((Result == 0) && (File != NULL))
            Result = LxMmFillPrivateFile(Process, Old + OldLength, Old + NewLength);
        if (Result == 0)
            Result = (LONG64)Old;
    }
    else if (!(Flags & LX_MREMAP_MAYMOVE))
    {
        Result = -LX_ENOMEM;
    }
    else if (!NT_SUCCESS(LxVmAllocate(&New, LX_ALIGN_UP(NewLength, LX_ALLOC_GRANULARITY), MEM_RESERVE, PAGE_NOACCESS)))
    {
        Result = -LX_ENOMEM;
    }
    else
    {
        Result = LxVmaInsert(Process, New, New + NewLength, Prot, VmaFlags, File, Offset, NULL);
        if (Result != 0)
            LxVmFree(New, 0, MEM_RELEASE);

        for (Cursor = Old; (Result == 0) && (Cursor < Old + OldLength);)
        {
            ULONG64 RegionEnd, Target, Commit;

            if (!NT_SUCCESS(LxVmQuery(Cursor, &Information)))
            {
                Result = -LX_EFAULT;
                break;
            }
            RegionEnd = min((ULONG64)(ULONG_PTR)Information.BaseAddress + Information.RegionSize, Old + OldLength);
            if (Information.State == MEM_COMMIT)
            {
                Target = New + (Cursor - Old);
                Commit = Target;
                if (!NT_SUCCESS(LxVmAllocate(&Commit, RegionEnd - Cursor, MEM_COMMIT, PAGE_READWRITE)))
                {
                    Result = -LX_ENOMEM;
                    break;
                }
                if ((Information.Protect & 0xFF) == PAGE_NOACCESS)
                    LxVmProtect(Cursor, RegionEnd - Cursor, PAGE_READONLY);
                _SEH2_TRY
                {
                    RtlCopyMemory((PVOID)(ULONG_PTR)Target, (PVOID)(ULONG_PTR)Cursor, (SIZE_T)(RegionEnd - Cursor));
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    Result = -LX_EFAULT;
                }
                _SEH2_END;
                if (Result != 0)
                    break;
                if (LxProtToNt(Prot) != PAGE_READWRITE)
                    LxVmProtect(Target, RegionEnd - Cursor, LxProtToNt(Prot));
                if (Prot & LX_PROT_EXEC)
                    LxVmFlushCode(Target, RegionEnd - Cursor);
            }
            Cursor = RegionEnd;
        }

        if ((Result == 0) && (File != NULL))
            Result = LxMmFillPrivateFile(Process, New + OldLength, New + NewLength);
        if (Result == 0)
        {
            LxMmUnmap(Process, Old, OldLength);
            Result = (LONG64)New;
        }
        else if (New != 0)
        {
            LxMmUnmap(Process, New, NewLength);
        }
    }

    if ((Result >= 0) && (Lock == LX_VMA_LOCKED))
        LxMmLockPopulate(Process, (ULONG64)Result + OldLength, (ULONG64)Result + NewLength);
    if (File != NULL)
        LxFileDereference(File);
    LxMmUnlock(Process);
    return Result;
}

BOOLEAN
LxMmHasVmaAbove(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Address)
{
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    Process = Process->Mm;
    LxMmLock(Process);
    for (Entry = Process->Vmas.Flink; (Entry != &Process->Vmas) && !Found; Entry = Entry->Flink)
        Found = CONTAINING_RECORD(Entry, LX_VMA, Link)->End > Address;
    LxMmUnlock(Process);
    return Found;
}

BOOLEAN
LxMmMapsPath(
    _In_ PLX_PROCESS Process,
    _In_opt_ PCSTR Path)
{
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    Process = Process->Mm;
    if (Path == NULL)
        return FALSE;
    LxMmLock(Process);
    for (Entry = Process->Vmas.Flink; (Entry != &Process->Vmas) && !Found; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        Found = (Vma->File != NULL) && (Vma->File->Path != NULL) && (strcmp(Vma->File->Path, Path) == 0);
    }
    LxMmUnlock(Process);
    return Found;
}

PLX_FILE
LxMmFileAt(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    PLIST_ENTRY Entry;
    PLX_FILE File = NULL;

    Process = Process->Mm;
    LxMmLock(Process);
    for (Entry = Process->Vmas.Flink; Entry != &Process->Vmas; Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if ((Vma->Start == Start) && (Vma->End == End) && (Vma->File != NULL))
        {
            File = Vma->File;
            LxFileReference(File);
            break;
        }
    }
    LxMmUnlock(Process);
    return File;
}

ULONG
LxMmFileRanges(
    _In_ PLX_PROCESS Process,
    _Out_writes_(Max * 2) PULONG64 Ranges,
    _In_ ULONG Max)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    Process = Process->Mm;
    LxMmLock(Process);
    for (Entry = Process->Vmas.Flink; (Entry != &Process->Vmas) && (Count < Max); Entry = Entry->Flink)
    {
        PLX_VMA Vma = CONTAINING_RECORD(Entry, LX_VMA, Link);

        if (Vma->File == NULL)
            continue;
        Ranges[Count * 2] = Vma->Start;
        Ranges[Count * 2 + 1] = Vma->End;
        Count++;
    }
    LxMmUnlock(Process);
    return Count;
}

VOID
LxMmSetBrk(
    _In_ PLX_PROCESS Process,
    _In_ ULONG64 StartBrk,
    _In_ ULONG64 Brk)
{
    ULONG64 Base;

    Process = Process->Mm;
    LxMmLock(Process);
    if ((StartBrk != Process->BrkBase) || (Brk != Process->BrkCurrent))
    {
        Process->BrkBase = StartBrk;
        Process->BrkCurrent = Brk;
        Process->BrkCommitted = LX_ALIGN_UP(Brk, LX_PAGE_SIZE);
        Process->BrkLimit = Process->BrkCommitted;
        Process->BrkReserved = TRUE;
        Base = Process->BrkCommitted;
        if (NT_SUCCESS(LxVmAllocate(&Base, LX_BRK_RESERVE, MEM_RESERVE, PAGE_NOACCESS)) && (Base == Process->BrkCommitted))
            Process->BrkLimit = Base + LX_BRK_RESERVE;
    }
    LxMmUnlock(Process);
}

LX_SYSCALL(LxSysBrk)
{
    PLX_PROCESS Process = Call->Process->Mm;
    ULONG64 Requested = Call->Args[0];
    ULONG64 NewCommitted, Result;
    NTSTATUS Status;

    LxMmLock(Process);

    if (!Process->BrkReserved && (Process->BrkBase != 0))
    {
        ULONG64 Base = Process->BrkBase;

        Process->BrkReserved = TRUE;
        Status = LxVmAllocate(&Base, LX_BRK_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
        if (NT_SUCCESS(Status) && (Base == Process->BrkBase))
            Process->BrkLimit = Process->BrkBase + LX_BRK_RESERVE;
        else
            Process->BrkLimit = Process->BrkBase;
    }

    Result = Process->BrkCurrent;
    if ((Requested < Process->BrkBase) || (Requested > Process->BrkLimit))
    {
        LxMmUnlock(Process);
        return (LONG64)Result;
    }

    NewCommitted = LX_ALIGN_UP(Requested, LX_PAGE_SIZE);
    if (NewCommitted > Process->BrkCommitted)
    {
        if (LxVmaOverlaps(Process, Process->BrkCommitted, NewCommitted) ||
            ((Process->MmLockDefault != 0) &&
             !LxMmLockAllowed(Call->Process, Process, NewCommitted - Process->BrkCommitted)) ||
            (LxVmaInsert(Process, Process->BrkCommitted, NewCommitted, LX_PROT_READ | LX_PROT_WRITE,
                         LX_MAP_PRIVATE | LX_VMA_HEAP | Process->MmLockDefault, NULL, 0, NULL) != 0))
        {
            LxMmUnlock(Process);
            return (LONG64)Result;
        }
        if (Process->MmLockDefault == LX_VMA_LOCKED)
            LxMmLockPopulate(Process, Process->BrkCommitted, NewCommitted);
    }
    else if (NewCommitted < Process->BrkCommitted)
    {
        LxVmaRemove(Process, NewCommitted, Process->BrkCommitted);
        LxVmDecommitRange(NewCommitted, Process->BrkCommitted);
    }

    Process->BrkCommitted = NewCommitted;
    Process->BrkCurrent = Requested;
    LxMmUnlock(Process);
    return (LONG64)Requested;
}
