/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests windows that hide or destroy themselves while they are deactivated
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

#define NESTED_CAP 6

#define NESTED_AFTER_DEFAULT  0x1
#define NESTED_NO_NC_DEFAULT  0x2

typedef enum _NESTED_TRIGGER
{
    TriggerSetActiveWindow,
    TriggerHideActive,
    TriggerDestroyActive
} NESTED_TRIGGER;

typedef enum _NESTED_ACTION
{
    ActionShowWindowHide,
    ActionSetWindowPosHide,
    ActionDestroy
} NESTED_ACTION;

typedef struct _NESTED_CASE
{
    PCSTR Name;
    NESTED_TRIGGER Trigger;
    UINT Message;
    NESTED_ACTION Action;
    BOOL Owned;
    ULONG Flags;
    LONG Inactive;
    LONG NcInactive;
    LONG Nest;
    LONG TargetActivated;
    BOOL TargetActive;
} NESTED_CASE;

static const NESTED_CASE g_Cases[] =
{
    { "SetActiveWindow, ShowWindow(SW_HIDE) on WM_ACTIVATE",
      TriggerSetActiveWindow, WM_ACTIVATE, ActionShowWindowHide, FALSE, 0, 1, 1, 1, 1, TRUE },
    { "SetActiveWindow, SetWindowPos(SWP_HIDEWINDOW) on WM_ACTIVATE",
      TriggerSetActiveWindow, WM_ACTIVATE, ActionSetWindowPosHide, FALSE, 0, 1, 1, 1, 1, TRUE },
    { "SetActiveWindow, ShowWindow(SW_HIDE) on WM_NCACTIVATE",
      TriggerSetActiveWindow, WM_NCACTIVATE, ActionShowWindowHide, FALSE, 0, 2, 2, 2, 1, TRUE },
    { "SetActiveWindow, ShowWindow(SW_HIDE) after DefWindowProc on WM_NCACTIVATE",
      TriggerSetActiveWindow, WM_NCACTIVATE, ActionShowWindowHide, FALSE, NESTED_AFTER_DEFAULT, 2, 2, 2, 1, TRUE },
    { "SetActiveWindow, ShowWindow(SW_HIDE) on WM_ACTIVATE without DefWindowProc on WM_NCACTIVATE",
      TriggerSetActiveWindow, WM_ACTIVATE, ActionShowWindowHide, FALSE, NESTED_NO_NC_DEFAULT, 1, 1, 1, 1, TRUE },
    { "SetActiveWindow, DestroyWindow on WM_ACTIVATE",
      TriggerSetActiveWindow, WM_ACTIVATE, ActionDestroy, FALSE, 0, 1, 1, 1, 0, FALSE },
    { "SetActiveWindow, DestroyWindow on WM_NCACTIVATE",
      TriggerSetActiveWindow, WM_NCACTIVATE, ActionDestroy, FALSE, 0, 1, NESTED_CAP + 1, NESTED_CAP + 1, 1, FALSE },
    { "SetActiveWindow, DestroyWindow after DefWindowProc on WM_NCACTIVATE",
      TriggerSetActiveWindow, WM_NCACTIVATE, ActionDestroy, FALSE, NESTED_AFTER_DEFAULT, 1, NESTED_CAP + 1, NESTED_CAP + 1, 1, FALSE },
    { "ShowWindow(SW_HIDE), DestroyWindow on WM_ACTIVATE",
      TriggerHideActive, WM_ACTIVATE, ActionDestroy, FALSE, 0, 1, 1, 1, 0, FALSE },
    { "SetActiveWindow, owned popup DestroyWindow on WM_ACTIVATE",
      TriggerSetActiveWindow, WM_ACTIVATE, ActionDestroy, TRUE, 0, 2, 2, 2, 0, FALSE },
    { "ShowWindow(SW_HIDE), owned popup DestroyWindow on WM_ACTIVATE",
      TriggerHideActive, WM_ACTIVATE, ActionDestroy, TRUE, 0, 1, 1, 1, 0, FALSE },
    { "DestroyWindow, DestroyWindow on WM_ACTIVATE",
      TriggerDestroyActive, WM_ACTIVATE, ActionDestroy, FALSE, 0, 1, 1, 1, 0, FALSE },
    { "DestroyWindow, owned popup DestroyWindow on WM_ACTIVATE",
      TriggerDestroyActive, WM_ACTIVATE, ActionDestroy, TRUE, 0, 1, 1, 1, 0, FALSE },
};

static const NESTED_CASE *g_Case;
static HWND g_A, g_B, g_Owner;
static LONG g_Nest, g_MaxNest, g_Inactive, g_NcInactive, g_BActivated, g_OwnerActivated;
static LONG g_BDeactivated, g_OwnerDeactivated;
static BOOL g_Capped;
static HWND g_ActiveAtInactive, g_ForegroundAtInactive, g_OtherAtInactive, g_OtherAtLastInactive;

static PCSTR
WindowName(HWND hwnd)
{
    if (hwnd == NULL)
        return "none";
    if (hwnd == g_A)
        return "A";
    if (hwnd == g_B)
        return "B";
    if (hwnd == g_Owner)
        return "owner";
    return "other";
}

static VOID
DoAction(HWND hwnd)
{
    if (g_Nest > NESTED_CAP)
    {
        g_Capped = TRUE;
        return;
    }

    switch (g_Case->Action)
    {
        case ActionShowWindowHide:
            ShowWindow(hwnd, SW_HIDE);
            break;
        case ActionSetWindowPosHide:
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
            break;
        case ActionDestroy:
            DestroyWindow(hwnd);
            break;
    }
}

static LRESULT
DefaultResult(HWND hwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    if (!IsWindow(hwnd))
        return (Message == WM_NCACTIVATE);
    return DefWindowProcW(hwnd, Message, wParam, lParam);
}

static LRESULT CALLBACK
NestedActivationProc(HWND hwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    BOOL React = FALSE;
    LRESULT Result;

    if (g_Case == NULL)
        return DefWindowProcW(hwnd, Message, wParam, lParam);

    if (hwnd == g_A)
    {
        if (Message == WM_NCACTIVATE && !wParam)
        {
            g_NcInactive++;
            React = (g_Case->Message == WM_NCACTIVATE);
            if (!React && (g_Case->Flags & NESTED_NO_NC_DEFAULT))
                return TRUE;
        }
        else if (Message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
        {
            if (g_Inactive++ == 0)
            {
                g_ActiveAtInactive = GetActiveWindow();
                g_ForegroundAtInactive = GetForegroundWindow();
                g_OtherAtInactive = (HWND)lParam;
            }
            g_OtherAtLastInactive = (HWND)lParam;
            React = (g_Case->Message == WM_ACTIVATE);
        }
    }
    else if (Message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)
    {
        if (hwnd == g_B)
            g_BActivated++;
        else if (hwnd == g_Owner)
            g_OwnerActivated++;
    }
    else if (Message == WM_ACTIVATE)
    {
        if (hwnd == g_B)
            g_BDeactivated++;
        else if (hwnd == g_Owner)
            g_OwnerDeactivated++;
    }

    if (!React)
        return DefWindowProcW(hwnd, Message, wParam, lParam);

    if (++g_Nest > g_MaxNest)
        g_MaxNest = g_Nest;
    if (g_Case->Flags & NESTED_AFTER_DEFAULT)
    {
        Result = DefaultResult(hwnd, Message, wParam, lParam);
        DoAction(hwnd);
    }
    else
    {
        DoAction(hwnd);
        Result = DefaultResult(hwnd, Message, wParam, lParam);
    }
    g_Nest--;
    return Result;
}

static VOID
FlushMessages(VOID)
{
    MSG Msg;

    while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
}

static VOID
RunCase(const NESTED_CASE *Case)
{
    BOOL Foreground;
    HWND Active, Target;
    LONG TargetActivated, TargetDeactivated;
    LONG_PTR TriggerResult;

    g_B = CreateWindowExW(0, L"NestedActivation", L"B", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          40, 40, 300, 200, NULL, NULL, NULL, NULL);
    g_Owner = NULL;
    if (Case->Owned)
    {
        g_Owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"NestedActivation", L"Owner",
                                  WS_POPUP | WS_BORDER | WS_VISIBLE,
                                  360, 40, 260, 260, NULL, NULL, NULL, NULL);
    }
    g_A = CreateWindowExW(WS_EX_TOOLWINDOW, L"NestedActivation", L"A",
                          WS_POPUP | WS_BORDER | WS_VISIBLE,
                          380, 80, 160, 160, g_Owner, NULL, NULL, NULL);
    ok(g_B != NULL && g_A != NULL && (!Case->Owned || g_Owner != NULL),
       "%s: CreateWindowExW failed, error %lu\n", Case->Name, GetLastError());
    if (g_B == NULL || g_A == NULL || (Case->Owned && g_Owner == NULL))
        goto Cleanup;

    SetForegroundWindow(g_A);
    SetActiveWindow(g_A);
    FlushMessages();
    Foreground = (GetForegroundWindow() == g_A);
    ok(GetActiveWindow() == g_A, "%s: setup active window is %s, expected A\n",
       Case->Name, WindowName(GetActiveWindow()));
    if (GetActiveWindow() != g_A)
        goto Cleanup;

    g_Nest = g_MaxNest = g_Inactive = g_NcInactive = g_BActivated = g_OwnerActivated = 0;
    g_BDeactivated = g_OwnerDeactivated = 0;
    g_Capped = FALSE;
    g_ActiveAtInactive = g_ForegroundAtInactive = g_OtherAtInactive = g_OtherAtLastInactive = NULL;
    g_Case = Case;

    if (Case->Trigger == TriggerSetActiveWindow)
        TriggerResult = (LONG_PTR)SetActiveWindow(g_B);
    else if (Case->Trigger == TriggerHideActive)
        TriggerResult = ShowWindow(g_A, SW_HIDE);
    else
        TriggerResult = DestroyWindow(g_A);
    FlushMessages();

    g_Case = NULL;
    Active = GetActiveWindow();
    Target = (Case->Trigger != TriggerSetActiveWindow && Case->Owned) ? g_Owner : g_B;
    TargetActivated = (Target == g_B) ? g_BActivated : g_OwnerActivated;
    TargetDeactivated = (Target == g_B) ? g_BDeactivated : g_OwnerDeactivated;

    trace("%s: foreground %d, WA_INACTIVE %ld, WM_NCACTIVATE(FALSE) %ld, nesting %ld%s, "
          "active during WA_INACTIVE %s, foreground %s, lParam %s, last lParam %s, B activated %ld, "
          "owner activated %ld, target WA_INACTIVE %ld, trigger returned %s, final active %s, A alive %d, A visible %d\n",
          Case->Name, Foreground, g_Inactive, g_NcInactive, g_MaxNest, g_Capped ? " (capped)" : "",
          WindowName(g_ActiveAtInactive), WindowName(g_ForegroundAtInactive), WindowName(g_OtherAtInactive),
          WindowName(g_OtherAtLastInactive), g_BActivated, g_OwnerActivated, TargetDeactivated,
          Case->Trigger == TriggerSetActiveWindow ? WindowName((HWND)TriggerResult) : (TriggerResult ? "TRUE" : "FALSE"),
          WindowName(Active),
          IsWindow(g_A), IsWindow(g_A) && IsWindowVisible(g_A));

    ok(g_MaxNest == Case->Nest, "%s: deactivation nesting %ld, expected %ld\n",
       Case->Name, g_MaxNest, Case->Nest);
    ok(g_Capped == (Case->Nest > NESTED_CAP), "%s: nesting cap reached %d, expected %d\n",
       Case->Name, g_Capped, Case->Nest > NESTED_CAP);
    ok(g_Inactive == Case->Inactive, "%s: A got WA_INACTIVE %ld times, expected %ld\n",
       Case->Name, g_Inactive, Case->Inactive);
    ok(g_NcInactive == Case->NcInactive, "%s: A got WM_NCACTIVATE(FALSE) %ld times, expected %ld\n",
       Case->Name, g_NcInactive, Case->NcInactive);
    ok(g_ActiveAtInactive == g_A, "%s: active window during WA_INACTIVE is %s, expected A\n",
       Case->Name, WindowName(g_ActiveAtInactive));
    ok(g_OtherAtInactive == Target, "%s: WA_INACTIVE names %s, expected %s\n",
       Case->Name, WindowName(g_OtherAtInactive), WindowName(Target));
    ok(TargetActivated == Case->TargetActivated, "%s: %s got WA_ACTIVE %ld times, expected %ld\n",
       Case->Name, WindowName(Target), TargetActivated, Case->TargetActivated);
    ok(Active == (Case->TargetActive ? Target : NULL), "%s: final active window is %s, expected %s\n",
       Case->Name, WindowName(Active), Case->TargetActive ? WindowName(Target) : "none");
    ok(TargetDeactivated == 0, "%s: %s got WA_INACTIVE %ld times, expected 0\n",
       Case->Name, WindowName(Target), TargetDeactivated);
    if (Case->Trigger == TriggerSetActiveWindow)
    {
        ok((HWND)TriggerResult == (Case->TargetActive ? g_A : NULL), "%s: SetActiveWindow returned %s, expected %s\n",
           Case->Name, WindowName((HWND)TriggerResult), Case->TargetActive ? "A" : "none");
    }
    else
    {
        ok(TriggerResult != 0, "%s: trigger returned 0, expected nonzero\n", Case->Name);
    }
    ok(!IsWindow(g_A) || !IsWindowVisible(g_A), "%s: A is still visible\n", Case->Name);

Cleanup:
    g_Case = NULL;
    if (g_A != NULL && IsWindow(g_A))
        DestroyWindow(g_A);
    if (g_Owner != NULL)
        DestroyWindow(g_Owner);
    if (g_B != NULL)
        DestroyWindow(g_B);
    FlushMessages();
    g_A = g_B = g_Owner = NULL;
}

START_TEST(NestedActivation)
{
    WNDCLASSW Class = { 0 };
    ULONG i;

    Class.lpfnWndProc = NestedActivationProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    Class.lpszClassName = L"NestedActivation";
    if (!RegisterClassW(&Class))
    {
        skip("RegisterClassW failed, error %lu\n", GetLastError());
        return;
    }

    for (i = 0; i < ARRAYSIZE(g_Cases); i++)
        RunCase(&g_Cases[i]);

    UnregisterClassW(L"NestedActivation", GetModuleHandleW(NULL));
}
