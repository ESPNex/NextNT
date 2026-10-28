/*
 * PROJECT:     NextNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for EnumerateTraceGuidsEx
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

static const GUID ManifestGuid = {0x5b0a8d3e,0x2f6c,0x4c1d,{0x9e,0x41,0x7a,0x33,0x0c,0x62,0xd8,0x15}};
static const GUID ClassicGuid = {0x5b0a8d3e,0x2f6c,0x4c1d,{0x9e,0x41,0x7a,0x33,0x0c,0x62,0xd8,0x16}};
static const GUID UnusedGuid = {0x5b0a8d3e,0x2f6c,0x4c1d,{0x9e,0x41,0x7a,0x33,0x0c,0x62,0xd8,0x17}};

static ULONG WINAPI ClassicCallback(WMIDPREQUESTCODE Code, PVOID Context, ULONG *Size, PVOID Buffer)
{
    return ERROR_SUCCESS;
}

static BOOL ListContains(TRACE_QUERY_INFO_CLASS Class, const GUID *Guid)
{
    GUID *List;
    ULONG Length = 0, Index;
    ULONG Status;
    BOOL Found = FALSE;

    Status = EnumerateTraceGuidsEx(Class, NULL, 0, NULL, 0, &Length);
    ok(Status == ERROR_INSUFFICIENT_BUFFER || (Status == ERROR_SUCCESS && !Length),
       "Class %d: status %lu, length %lu\n", Class, Status, Length);
    ok(!(Length % sizeof(GUID)), "Class %d: length %lu\n", Class, Length);
    if (Status != ERROR_INSUFFICIENT_BUFFER || !Length) return FALSE;

    Length += 16 * sizeof(GUID);
    List = HeapAlloc(GetProcessHeap(), 0, Length);
    if (!List) return FALSE;
    Status = EnumerateTraceGuidsEx(Class, NULL, 0, List, Length, &Length);
    ok(Status == ERROR_SUCCESS, "Class %d: status %lu\n", Class, Status);
    if (Status == ERROR_SUCCESS)
    {
        for (Index = 0; Index < Length / sizeof(GUID); Index++)
        {
            if (IsEqualGUID(&List[Index], Guid)) Found = TRUE;
        }
    }
    HeapFree(GetProcessHeap(), 0, List);
    return Found;
}

static void CheckInfo(const GUID *Guid, ULONG ExpectedFlags)
{
    TRACE_GUID_INFO *Info;
    TRACE_PROVIDER_INSTANCE_INFO *Instance;
    ULONG Length = 0, Index, Found = 0;
    ULONG Status;

    Status = EnumerateTraceGuidsEx(TraceGuidQueryInfo, (PVOID)Guid, sizeof(GUID), NULL, 0, &Length);
    ok_long(Status, ERROR_INSUFFICIENT_BUFFER);
    ok(Length >= sizeof(TRACE_GUID_INFO) + sizeof(TRACE_PROVIDER_INSTANCE_INFO), "Length %lu\n", Length);
    if (Status != ERROR_INSUFFICIENT_BUFFER) return;

    Info = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    if (!Info) return;
    Status = EnumerateTraceGuidsEx(TraceGuidQueryInfo, (PVOID)Guid, sizeof(GUID), Info, Length, &Length);
    ok_long(Status, ERROR_SUCCESS);
    if (Status == ERROR_SUCCESS)
    {
        ok(Info->InstanceCount >= 1, "InstanceCount %lu\n", Info->InstanceCount);
        Instance = (TRACE_PROVIDER_INSTANCE_INFO *)(Info + 1);
        for (Index = 0; Index < Info->InstanceCount; Index++)
        {
            if (Instance->Pid == GetCurrentProcessId())
            {
                Found++;
                ok(Instance->EnableCount == 0, "EnableCount %lu\n", Instance->EnableCount);
                ok(Instance->Flags == ExpectedFlags, "Flags 0x%lx\n", Instance->Flags);
            }
            if (!Instance->NextOffset) break;
            Instance = (TRACE_PROVIDER_INSTANCE_INFO *)((PUCHAR)Instance + Instance->NextOffset);
        }
        ok(Index + 1 == Info->InstanceCount, "Walked %lu of %lu instances\n", Index + 1, Info->InstanceCount);
        ok(Found == 1, "Found %lu instances for this process\n", Found);
    }
    HeapFree(GetProcessHeap(), 0, Info);
}

START_TEST(EnumerateTraceGuidsEx)
{
    REGHANDLE RegHandle;
    TRACEHANDLE ClassicHandle;
    ULONG Length;
    ULONG Status;
    GUID Buffer;

    Status = EventRegister(&ManifestGuid, NULL, NULL, &RegHandle);
    ok_long(Status, ERROR_SUCCESS);
    if (Status != ERROR_SUCCESS) return;

    ok(ListContains(TraceGuidQueryList, &ManifestGuid), "Provider missing from the list\n");
    ok(ListContains(TraceGuidQueryProcess, &ManifestGuid), "Provider missing from the process list\n");
    CheckInfo(&ManifestGuid, 0);

    Status = RegisterTraceGuidsW(ClassicCallback, NULL, &ClassicGuid, 0, NULL, NULL, NULL, &ClassicHandle);
    ok_long(Status, ERROR_SUCCESS);
    if (Status == ERROR_SUCCESS)
    {
        ok(ListContains(TraceGuidQueryProcess, &ClassicGuid), "Classic provider missing\n");
        CheckInfo(&ClassicGuid, TRACE_PROVIDER_FLAG_LEGACY);
        ok_long(UnregisterTraceGuids(ClassicHandle), ERROR_SUCCESS);
    }

    Status = EnumerateTraceGuidsEx(TraceGuidQueryList, NULL, 0, NULL, sizeof(GUID), &Length);
    ok_long(Status, ERROR_INSUFFICIENT_BUFFER);
    ok(Length >= sizeof(GUID) && !(Length % sizeof(GUID)), "Length %lu\n", Length);

    ok_long(EventUnregister(RegHandle), ERROR_SUCCESS);
    ok(!ListContains(TraceGuidQueryProcess, &ManifestGuid), "Unregistered provider still listed\n");

    Length = 0xdeadbeef;
    Status = EnumerateTraceGuidsEx(TraceGuidQueryInfo, (PVOID)&UnusedGuid, sizeof(GUID), NULL, 0, &Length);
    ok_long(Status, ERROR_WMI_GUID_NOT_FOUND);
    ok(Length == 0, "Length %lu\n", Length);

    Length = 0xdeadbeef;
    Status = EnumerateTraceGuidsEx(TraceGuidQueryInfo, NULL, 0, NULL, 0, &Length);
    ok_long(Status, ERROR_INVALID_PARAMETER);
    Status = EnumerateTraceGuidsEx(TraceGuidQueryInfo, &Buffer, sizeof(GUID) - 1, NULL, 0, &Length);
    ok_long(Status, ERROR_INVALID_PARAMETER);
}
