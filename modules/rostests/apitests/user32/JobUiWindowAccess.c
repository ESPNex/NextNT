/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for window access from a process in a job with UI restrictions
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

#include <strsafe.h>

#define ACCESS_ISWINDOW      0x01
#define ACCESS_CLIENTRECT    0x02
#define ACCESS_WINDOWRECT    0x04
#define ACCESS_WINDOWTHREAD  0x08
#define ACCESS_GETDC         0x10
#define ACCESS_POSTMESSAGE   0x20
#define ACCESS_USER32        (ACCESS_ISWINDOW | ACCESS_CLIENTRECT | ACCESS_WINDOWRECT | ACCESS_WINDOWTHREAD)
#define ACCESS_KERNEL        (ACCESS_GETDC | ACCESS_POSTMESSAGE)
#define ACCESS_ALL           (ACCESS_USER32 | ACCESS_KERNEL)

#define VALIDATE_HANDLES_FLAG 0x20000000

static
DWORD
RunChild(
    _In_ HWND hWnd,
    _In_ BOOL ClearValidateFlag)
{
    DWORD Access = 0, ProcessId = 0;
    RECT Rect;
    HDC hDC;

    if (ClearValidateFlag)
    {
        PDWORD UserFlags;

        IsGUIThread(TRUE);
        UserFlags = (PDWORD)((PBYTE)NtCurrentTeb() + (sizeof(PVOID) == 8 ? 0x800 : 0x6cc) +
                             2 * sizeof(ULONG_PTR) + 3 * sizeof(DWORD));
        *UserFlags &= ~VALIDATE_HANDLES_FLAG;
    }

    if (IsWindow(hWnd))
        Access |= ACCESS_ISWINDOW;
    if (GetClientRect(hWnd, &Rect))
        Access |= ACCESS_CLIENTRECT;
    if (GetWindowRect(hWnd, &Rect))
        Access |= ACCESS_WINDOWRECT;
    if (GetWindowThreadProcessId(hWnd, &ProcessId) != 0)
        Access |= ACCESS_WINDOWTHREAD;
    hDC = GetDC(hWnd);
    if (hDC != NULL)
    {
        Access |= ACCESS_GETDC;
        ReleaseDC(hWnd, hDC);
    }
    if (PostMessageW(hWnd, WM_NULL, 0, 0))
        Access |= ACCESS_POSTMESSAGE;

    return Access;
}

static
void
TestAccess(
    _In_ HWND hWnd,
    _In_ DWORD Limits,
    _In_ BOOL ClearValidateFlag,
    _In_ DWORD Expected)
{
    WCHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 64];
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    HANDLE hJob;
    DWORD ExitCode = 0xDEADBEEF;

    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    Info.UIRestrictionsClass = Limits;
    ok(SetInformationJobObject(hJob, JobObjectBasicUIRestrictions, &Info, sizeof(Info)),
       "SetInformationJobObject(%lx) failed with %lu\n", Limits, GetLastError());

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" JobUiWindowAccess child %Ix %d",
                    FileName, (ULONG_PTR)hWnd, ClearValidateFlag);

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        CloseHandle(hJob);
        return;
    }

    if (!AssignProcessToJobObject(hJob, ProcessInfo.hProcess))
    {
        skip("AssignProcessToJobObject failed with %lu\n", GetLastError());
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    else
    {
        ResumeThread(ProcessInfo.hThread);
        while (MsgWaitForMultipleObjects(1, &ProcessInfo.hProcess, FALSE, 15000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
        {
            MSG Msg;
            while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessageW(&Msg);
        }
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(ExitCode == Expected, "Limits %lx, clear %d: access %lx, expected %lx\n",
           Limits, ClearValidateFlag, ExitCode, Expected);
    }

    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(hJob);
}

START_TEST(JobUiWindowAccess)
{
    char **Arguments;
    int Count = winetest_get_mainargs(&Arguments);
    WNDCLASSW Class = {0};
    HWND hWnd;
    BOOL IsWow64 = FALSE;

    if (Count >= 5 && !strcmp(Arguments[2], "child"))
        ExitProcess(RunChild((HWND)(ULONG_PTR)_strtoui64(Arguments[3], NULL, 16), atoi(Arguments[4])));

    Class.lpfnWndProc = DefWindowProcW;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"JobUiWindowAccess";
    RegisterClassW(&Class);
    hWnd = CreateWindowExW(0, L"JobUiWindowAccess", L"JobUiWindowAccess", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           40, 40, 300, 200, NULL, NULL, Class.hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowEx failed with %lu\n", GetLastError());
    if (hWnd == NULL)
        return;

    TestAccess(hWnd, 0, FALSE, ACCESS_ALL);
    TestAccess(hWnd, JOB_OBJECT_UILIMIT_READCLIPBOARD, FALSE, ACCESS_KERNEL);
    TestAccess(hWnd, JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD, FALSE, ACCESS_KERNEL);
    TestAccess(hWnd, JOB_OBJECT_UILIMIT_HANDLES, FALSE, 0);

    IsWow64Process(GetCurrentProcess(), &IsWow64);
    if (IsWow64)
    {
        skip("The user flags of a WoW64 thread are not at the native offset\n");
    }
    else
    {
        TestAccess(hWnd, JOB_OBJECT_UILIMIT_READCLIPBOARD, TRUE, ACCESS_ALL);
        TestAccess(hWnd, JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD, TRUE, ACCESS_ALL);
        TestAccess(hWnd, JOB_OBJECT_UILIMIT_HANDLES, TRUE, ACCESS_USER32);
    }

    DestroyWindow(hWnd);
}
