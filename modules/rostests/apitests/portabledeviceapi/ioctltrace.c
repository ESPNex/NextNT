/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records the device I/O control requests the WPD API sends from this process
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <tlhelp32.h>

typedef BOOL (WINAPI *PFN_DEVICE_IO_CONTROL)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HANDLE (WINAPI *PFN_CREATE_FILE_W)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static HANDLE TraceFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION TraceLock;
static PFN_DEVICE_IO_CONTROL RealDeviceIoControl;
static PFN_CREATE_FILE_W RealCreateFileW;
static LONG TraceSequence;

static void
TraceWrite(const char *Text)
{
    DWORD Written;

    if (TraceFile != INVALID_HANDLE_VALUE)
        WriteFile(TraceFile, Text, (DWORD)strlen(Text), &Written, NULL);
}

static void
TraceBytes(const BYTE *Data, DWORD Size, DWORD Limit)
{
    char Line[16 + 3 * 32 + 2];
    DWORD i, n, Count = min(Size, Limit);

    for (i = 0; i < Count; i += 32)
    {
        int Used = sprintf(Line, "  %04lX:", i);
        for (n = 0; n < 32 && i + n < Count; n++)
            Used += sprintf(Line + Used, " %02X", Data[i + n]);
        strcpy(Line + Used, "\n");
        TraceWrite(Line);
    }
    if (Count < Size)
    {
        sprintf(Line, "  ... %lu more\n", Size - Count);
        TraceWrite(Line);
    }
}

static BOOL WINAPI
HookDeviceIoControl(HANDLE Handle, DWORD Code, LPVOID In, DWORD InSize, LPVOID Out, DWORD OutSize,
                    LPDWORD Returned, LPOVERLAPPED Overlapped)
{
    BOOL Result;
    DWORD Error, Bytes = 0;
    BOOL Pending = FALSE;
    char Line[256];
    LONG Sequence = InterlockedIncrement(&TraceSequence);

    EnterCriticalSection(&TraceLock);
    sprintf(Line, "#%ld tid %lu ioctl h %p code 0x%08lX in %lu out %lu ov %d\n",
            Sequence, GetCurrentThreadId(), Handle, Code, InSize, OutSize, Overlapped != NULL);
    TraceWrite(Line);
    if (In && InSize)
        TraceBytes(In, InSize, 0x2000);
    LeaveCriticalSection(&TraceLock);

    Result = RealDeviceIoControl(Handle, Code, In, InSize, Out, OutSize, Returned, Overlapped);
    Error = GetLastError();
    if (Result)
    {
        Bytes = Returned ? *Returned : 0;
    }
    else if (Error == ERROR_IO_PENDING && Overlapped)
    {
        HANDLE Event = (HANDLE)((ULONG_PTR)Overlapped->hEvent & ~(ULONG_PTR)1);

        Pending = TRUE;
        if (Event && WaitForSingleObject(Event, 10000) == WAIT_OBJECT_0)
        {
            if (GetOverlappedResult(Handle, Overlapped, &Bytes, FALSE))
                Error = ERROR_SUCCESS;
            else
                Error = GetLastError();
            SetEvent(Event);
        }
        else
        {
            Error = WAIT_TIMEOUT;
        }
    }

    EnterCriticalSection(&TraceLock);
    sprintf(Line, "#%ld result %d pending %d error %lu bytes %lu\n", Sequence, Result, Pending, Error, Bytes);
    TraceWrite(Line);
    if (Out && Bytes)
        TraceBytes(Out, Bytes, 0x2000);
    LeaveCriticalSection(&TraceLock);

    SetLastError(Pending ? ERROR_IO_PENDING : Error);
    return Result;
}

static HANDLE WINAPI
HookCreateFileW(LPCWSTR Name, DWORD Access, DWORD Share, LPSECURITY_ATTRIBUTES Security,
                DWORD Disposition, DWORD Flags, HANDLE Template)
{
    HANDLE Handle;
    DWORD Error;
    char Line[1200];

    Handle = RealCreateFileW(Name, Access, Share, Security, Disposition, Flags, Template);
    Error = GetLastError();
    EnterCriticalSection(&TraceLock);
    _snprintf(Line, sizeof(Line), "createfile \"%ls\" access 0x%08lX share 0x%lX disp %lu flags 0x%08lX -> %p error %lu\n",
              Name ? Name : L"(null)", Access, Share, Disposition, Flags, Handle, Error);
    Line[sizeof(Line) - 1] = 0;
    TraceWrite(Line);
    LeaveCriticalSection(&TraceLock);
    SetLastError(Error);
    return Handle;
}

static void
PatchSlot(ULONG_PTR *Slot, ULONG_PTR Value)
{
    DWORD Old;

    if (*Slot == Value)
        return;
    if (VirtualProtect(Slot, sizeof(*Slot), PAGE_READWRITE, &Old))
    {
        *Slot = Value;
        VirtualProtect(Slot, sizeof(*Slot), Old, &Old);
    }
}

static void
PatchByName(BYTE *Base, ULONG_PTR *Names, ULONG_PTR *Slots)
{
    for (; *Names; Names++, Slots++)
    {
        PIMAGE_IMPORT_BY_NAME Import;

        if (IMAGE_SNAP_BY_ORDINAL(*Names))
            continue;
        Import = (PIMAGE_IMPORT_BY_NAME)(Base + *Names);
        if (!strcmp((const char *)Import->Name, "DeviceIoControl"))
            PatchSlot(Slots, (ULONG_PTR)HookDeviceIoControl);
        else if (!strcmp((const char *)Import->Name, "CreateFileW"))
            PatchSlot(Slots, (ULONG_PTR)HookCreateFileW);
    }
}

static void
HookModule(HMODULE Module)
{
    BYTE *Base = (BYTE *)Module;
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    PIMAGE_DATA_DIRECTORY Directory = Nt->OptionalHeader.DataDirectory;
    PIMAGE_IMPORT_DESCRIPTOR Import;
    PIMAGE_DELAYLOAD_DESCRIPTOR Delay;

    if (Directory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
    {
        for (Import = (PIMAGE_IMPORT_DESCRIPTOR)(Base + Directory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
             Import->Name;
             Import++)
        {
            if (!Import->OriginalFirstThunk)
                continue;
            PatchByName(Base, (ULONG_PTR *)(Base + Import->OriginalFirstThunk), (ULONG_PTR *)(Base + Import->FirstThunk));
        }
    }
    if (Directory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
    {
        for (Delay = (PIMAGE_DELAYLOAD_DESCRIPTOR)(Base + Directory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
             Delay->DllNameRVA;
             Delay++)
        {
            if (!Delay->ImportNameTableRVA)
                continue;
            PatchByName(Base, (ULONG_PTR *)(Base + Delay->ImportNameTableRVA), (ULONG_PTR *)(Base + Delay->ImportAddressTableRVA));
        }
    }
}

void
WpdTraceIoctlsStart(void)
{
    char Path[MAX_PATH];
    HMODULE Kernel;

    if (TraceFile != INVALID_HANDLE_VALUE)
        return;
    if (!GetEnvironmentVariableA("WPD_IOCTL_LOG", Path, sizeof(Path)))
        return;
    Kernel = GetModuleHandleA("kernelbase.dll");
    if (!Kernel)
        Kernel = GetModuleHandleA("kernel32.dll");
    RealDeviceIoControl = (PFN_DEVICE_IO_CONTROL)GetProcAddress(Kernel, "DeviceIoControl");
    RealCreateFileW = (PFN_CREATE_FILE_W)GetProcAddress(Kernel, "CreateFileW");
    if (!RealDeviceIoControl || !RealCreateFileW)
        return;
    InitializeCriticalSection(&TraceLock);
    TraceFile = CreateFileA(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (TraceFile != INVALID_HANDLE_VALUE)
        SetFilePointer(TraceFile, 0, NULL, FILE_END);
}

void
WpdTraceIoctlsHookModules(void)
{
    HANDLE Snapshot;
    MODULEENTRY32W Entry;
    HMODULE Self = GetModuleHandleW(NULL);

    if (TraceFile == INVALID_HANDLE_VALUE)
        return;
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;
    Entry.dwSize = sizeof(Entry);
    if (Module32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (Entry.hModule == Self ||
                !lstrcmpiW(Entry.szModule, L"kernel32.dll") ||
                !lstrcmpiW(Entry.szModule, L"kernelbase.dll") ||
                !lstrcmpiW(Entry.szModule, L"ntdll.dll"))
            {
                continue;
            }
            EnterCriticalSection(&TraceLock);
            HookModule(Entry.hModule);
            LeaveCriticalSection(&TraceLock);
        } while (Module32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
}
