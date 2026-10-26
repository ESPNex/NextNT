/*
 * PROJECT:     LiberNT Linux Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Huge page pool and hugetlb page stores backed by large-page sections
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "lxcore.h"

#define NDEBUG
#include <debug.h>

static FAST_MUTEX LxHugeLock;
static PVOID *LxHugeFree;
static ULONG LxHugeFreeCount;
static ULONG LxHugeFreeCapacity;
static ULONG64 LxHugeTotal;
static ULONG64 LxHugeSurplus;

VOID
LxHugeInitialize(VOID)
{
    ExInitializeFastMutex(&LxHugeLock);
}

ULONG64
LxHugePageSize(VOID)
{
    return SharedUserData->LargePageMinimum;
}

static
PVOID
LxHugeCreatePage(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER Size;
    PVOID Section = NULL;
    HANDLE Handle;
    NTSTATUS Status;

    Size.QuadPart = (LONG64)LxHugePageSize();
    if (Size.QuadPart == 0)
        return NULL;
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Handle, SECTION_ALL_ACCESS, &ObjectAttributes, &Size, PAGE_READWRITE,
                             SEC_COMMIT | SEC_LARGE_PAGES, NULL);
    if (!NT_SUCCESS(Status))
        return NULL;
    Status = ObReferenceObjectByHandle(Handle, SECTION_ALL_ACCESS, NULL, KernelMode, &Section, NULL);
    ZwClose(Handle);
    return NT_SUCCESS(Status) ? Section : NULL;
}

static
BOOLEAN
LxHugePushFree(
    _In_ PVOID Section)
{
    if (LxHugeFreeCount == LxHugeFreeCapacity)
    {
        ULONG Capacity = LxHugeFreeCapacity ? LxHugeFreeCapacity * 2 : 16;
        PVOID *Pages = LxAlloc(Capacity * sizeof(*Pages));

        if (Pages == NULL)
            return FALSE;
        if (LxHugeFree != NULL)
        {
            RtlCopyMemory(Pages, LxHugeFree, LxHugeFreeCount * sizeof(*Pages));
            LxFree(LxHugeFree);
        }
        LxHugeFree = Pages;
        LxHugeFreeCapacity = Capacity;
    }
    LxHugeFree[LxHugeFreeCount++] = Section;
    return TRUE;
}

ULONG64
LxHugeQueryPool(
    _Out_opt_ PULONG64 Free,
    _Out_opt_ PULONG64 Surplus)
{
    ULONG64 Total;

    ExAcquireFastMutex(&LxHugeLock);
    Total = LxHugeTotal;
    if (Free != NULL)
        *Free = LxHugeFreeCount;
    if (Surplus != NULL)
        *Surplus = LxHugeSurplus;
    ExReleaseFastMutex(&LxHugeLock);
    return Total;
}

VOID
LxHugeSetPool(
    _In_ ULONG64 Count)
{
    for (;;)
    {
        PVOID Section;
        BOOLEAN Need;

        ExAcquireFastMutex(&LxHugeLock);
        Need = (LxHugeTotal < Count);
        ExReleaseFastMutex(&LxHugeLock);
        if (!Need || ((Section = LxHugeCreatePage()) == NULL))
            break;
        ExAcquireFastMutex(&LxHugeLock);
        if ((LxHugeTotal < Count) && LxHugePushFree(Section))
        {
            LxHugeTotal++;
            Section = NULL;
        }
        ExReleaseFastMutex(&LxHugeLock);
        if (Section != NULL)
        {
            ObDereferenceObject(Section);
            break;
        }
    }
    for (;;)
    {
        PVOID Section = NULL;

        ExAcquireFastMutex(&LxHugeLock);
        if ((LxHugeTotal > Count) && (LxHugeFreeCount != 0))
        {
            Section = LxHugeFree[--LxHugeFreeCount];
            LxHugeTotal--;
        }
        ExReleaseFastMutex(&LxHugeLock);
        if (Section == NULL)
            break;
        ObDereferenceObject(Section);
    }
}

static
VOID
LxHugeRelease(
    _In_ PVOID Section)
{
    BOOLEAN Refill;

    ObDereferenceObject(Section);
    ExAcquireFastMutex(&LxHugeLock);
    Refill = (LxHugeSurplus == 0);
    if (!Refill)
        LxHugeSurplus--;
    ExReleaseFastMutex(&LxHugeLock);
    if (!Refill)
        return;
    Section = LxHugeCreatePage();
    ExAcquireFastMutex(&LxHugeLock);
    if ((Section != NULL) && LxHugePushFree(Section))
        Section = NULL;
    else
        LxHugeTotal--;
    ExReleaseFastMutex(&LxHugeLock);
    if (Section != NULL)
        ObDereferenceObject(Section);
}

LONG
LxHugeStoreReserve(
    _Inout_ PLX_HUGE_STORE Store,
    _In_ ULONG64 First,
    _In_ ULONG64 Count,
    _In_ BOOLEAN Surplus)
{
    ULONG64 Index, Missing = 0;
    PVOID *Pages = NULL;

    if ((First + Count < First) || (First + Count > MAXULONG))
        return -LX_ENOMEM;
    if (First + Count > Store->Count)
    {
        Pages = LxAlloc((SIZE_T)(First + Count) * sizeof(*Pages));
        if (Pages == NULL)
            return -LX_ENOMEM;
    }

    ExAcquireFastMutex(&LxHugeLock);
    if ((Pages != NULL) && (First + Count > Store->Count))
    {
        if (Store->Pages != NULL)
            RtlCopyMemory(Pages, Store->Pages, Store->Count * sizeof(*Pages));
        LxFree(Store->Pages);
        Store->Pages = Pages;
        Store->Count = (ULONG)(First + Count);
        Pages = NULL;
    }
    for (Index = First; Index < First + Count; Index++)
    {
        if (Store->Pages[Index] == NULL)
            Missing++;
    }
    if (!Surplus && (Missing > LxHugeFreeCount))
    {
        ExReleaseFastMutex(&LxHugeLock);
        LxFree(Pages);
        return -LX_ENOMEM;
    }
    for (Index = First; (Index < First + Count) && (LxHugeFreeCount != 0); Index++)
    {
        if (Store->Pages[Index] == NULL)
            Store->Pages[Index] = LxHugeFree[--LxHugeFreeCount];
    }
    ExReleaseFastMutex(&LxHugeLock);
    LxFree(Pages);

    for (Index = First; Index < First + Count; Index++)
    {
        PVOID Section;

        if (Store->Pages[Index] != NULL)
            continue;
        Section = LxHugeCreatePage();
        if (Section == NULL)
            return -LX_ENOMEM;
        ExAcquireFastMutex(&LxHugeLock);
        if (Store->Pages[Index] == NULL)
        {
            Store->Pages[Index] = Section;
            LxHugeSurplus++;
            Section = NULL;
        }
        ExReleaseFastMutex(&LxHugeLock);
        if (Section != NULL)
            ObDereferenceObject(Section);
    }
    return 0;
}

VOID
LxHugeStoreTruncate(
    _Inout_ PLX_HUGE_STORE Store,
    _In_ ULONG64 Count)
{
    for (;;)
    {
        PVOID Section = NULL;
        ULONG64 Index;

        ExAcquireFastMutex(&LxHugeLock);
        for (Index = Count; Index < Store->Count; Index++)
        {
            if (Store->Pages[Index] != NULL)
            {
                Section = Store->Pages[Index];
                Store->Pages[Index] = NULL;
                break;
            }
        }
        if (Section == NULL)
        {
            if (Count < Store->Count)
                Store->Count = (ULONG)Count;
            if ((Store->Count == 0) && (Store->Pages != NULL))
            {
                LxFree(Store->Pages);
                Store->Pages = NULL;
            }
        }
        ExReleaseFastMutex(&LxHugeLock);
        if (Section == NULL)
            break;
        LxHugeRelease(Section);
    }
}

PVOID
LxHugeStorePage(
    _In_ PLX_HUGE_STORE Store,
    _In_ ULONG64 Index)
{
    PVOID Section = NULL;

    ExAcquireFastMutex(&LxHugeLock);
    if ((Index < Store->Count) && (Store->Pages[Index] != NULL))
    {
        Section = Store->Pages[Index];
        ObReferenceObject(Section);
    }
    ExReleaseFastMutex(&LxHugeLock);
    return Section;
}

NTSTATUS
LxHugeMapPage(
    _In_ PVOID Section,
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *Base,
    _In_ ULONG Protect)
{
    LARGE_INTEGER Offset;
    SIZE_T ViewSize = (SIZE_T)LxHugePageSize();
    HANDLE Handle;
    NTSTATUS Status;

    Offset.QuadPart = 0;
    Status = ObOpenObjectByPointer(Section, OBJ_KERNEL_HANDLE, NULL, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                                   NULL, KernelMode, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwMapViewOfSection(Handle, ProcessHandle, Base, 0, 0, &Offset, &ViewSize, ViewShare, MEM_LARGE_PAGES,
                                Protect);
    ZwClose(Handle);
    return Status;
}

NTSTATUS
LxHugeCopyPage(
    _In_ PVOID From,
    _In_ PVOID To)
{
    PVOID Source = NULL, Target = NULL;
    NTSTATUS Status;

    Status = LxHugeMapPage(From, NtCurrentProcess(), &Source, PAGE_READONLY);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = LxHugeMapPage(To, NtCurrentProcess(), &Target, PAGE_READWRITE);
    if (NT_SUCCESS(Status))
    {
        _SEH2_TRY
        {
            RtlCopyMemory(Target, Source, (SIZE_T)LxHugePageSize());
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        ZwUnmapViewOfSection(NtCurrentProcess(), Target);
    }
    ZwUnmapViewOfSection(NtCurrentProcess(), Source);
    return Status;
}

LONG64
LxHugeStoreRead(
    _In_ PLX_HUGE_STORE Store,
    _In_ ULONG64 Offset,
    _In_ ULONG64 Buffer,
    _In_ ULONG64 Length)
{
    ULONG64 PageSize = LxHugePageSize(), Done = 0;

    while (Done < Length)
    {
        ULONG64 Index = (Offset + Done) / PageSize, Within = (Offset + Done) % PageSize;
        ULONG64 Chunk = min(Length - Done, PageSize - Within);
        PVOID Section = LxHugeStorePage(Store, Index), View = NULL;
        LONG Error = 0;

        if (Section == NULL)
        {
            Error = LxZeroUser(Buffer + Done, Chunk);
        }
        else
        {
            if (!NT_SUCCESS(LxHugeMapPage(Section, NtCurrentProcess(), &View, PAGE_READONLY)))
                Error = -LX_ENOMEM;
            else
            {
                Error = LxCopyToUser(Buffer + Done, (PUCHAR)View + Within, (SIZE_T)Chunk);
                ZwUnmapViewOfSection(NtCurrentProcess(), View);
            }
            ObDereferenceObject(Section);
        }
        if (Error != 0)
            return (Done != 0) ? (LONG64)Done : Error;
        Done += Chunk;
    }
    return (LONG64)Done;
}

static const LX_FILE_OPS LxHugeFileOps =
{
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

PLX_FILE
LxHugeFileCreate(
    _In_ PCSTR Path)
{
    PLX_FILE File = LxFileAlloc(&LxHugeFileOps, LX_O_RDWR, LX_S_IFREG);

    if (File == NULL)
        return NULL;
    File->Path = LxStrDup(Path);
    File->Mode = 0600;
    File->Memfd = LxAlloc(sizeof(*File->Memfd));
    if (File->Memfd != NULL)
    {
        File->Memfd->RefCount = 1;
        File->Memfd->Seals = LX_F_SEAL_SEAL;
        File->Memfd->Huge = LxAlloc(sizeof(*File->Memfd->Huge));
    }
    if ((File->Path == NULL) || (File->Memfd == NULL) || (File->Memfd->Huge == NULL))
    {
        LxFileDereference(File);
        return NULL;
    }
    return File;
}
