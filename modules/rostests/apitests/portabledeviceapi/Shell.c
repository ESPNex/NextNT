/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records how the shell shows WPD devices under This PC
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <shlobj.h>
#include <shlwapi.h>

static void
DumpName(IShellFolder *Folder, PCUITEMID_CHILD Item, SHGDNF Flags, const char *Name, const char *Prefix)
{
    STRRET Ret;
    WCHAR Text[1024];
    HRESULT hr;

    hr = IShellFolder_GetDisplayNameOf(Folder, Item, Flags, &Ret);
    if (SUCCEEDED(hr))
        hr = StrRetToBufW(&Ret, Item, Text, ARRAYSIZE(Text));
    trace("%s%s hr 0x%08lX \"%ls\"\n", Prefix, Name, hr, SUCCEEDED(hr) ? Text : L"");
}

static void
DumpItem(IShellFolder *Folder, PCUITEMID_CHILD Item, const char *Prefix)
{
    IShellFolder2 *Folder2;
    SFGAOF Attributes = SFGAO_CAPABILITYMASK | SFGAO_DISPLAYATTRMASK | SFGAO_CONTENTSMASK |
                        SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_FILESYSANCESTOR | SFGAO_STORAGE |
                        SFGAO_STORAGEANCESTOR | SFGAO_STREAM | SFGAO_REMOVABLE | SFGAO_BROWSABLE |
                        SFGAO_NONENUMERATED | SFGAO_NEWCONTENT | SFGAO_VALIDATE;
    SHELLDETAILS Details;
    WCHAR Text[512];
    UINT Column;
    HRESULT hr;

    DumpName(Folder, Item, SHGDN_NORMAL, "normal", Prefix);
    DumpName(Folder, Item, SHGDN_INFOLDER, "infolder", Prefix);
    DumpName(Folder, Item, SHGDN_FORPARSING, "parsing", Prefix);
    DumpName(Folder, Item, SHGDN_INFOLDER | SHGDN_FORPARSING, "infolder-parsing", Prefix);
    DumpName(Folder, Item, SHGDN_INFOLDER | SHGDN_FOREDITING, "editing", Prefix);
    hr = IShellFolder_GetAttributesOf(Folder, 1, &Item, &Attributes);
    trace("%sattributes hr 0x%08lX 0x%08lX\n", Prefix, hr, Attributes);
    trace("%spidl size %u\n", Prefix, ILGetSize((PCUIDLIST_RELATIVE)Item));
    if (SUCCEEDED(IShellFolder_QueryInterface(Folder, &IID_IShellFolder2, (void **)&Folder2)))
    {
        for (Column = 0; Column < 64; Column++)
        {
            hr = IShellFolder2_GetDetailsOf(Folder2, Item, Column, &Details);
            if (FAILED(hr))
            {
                trace("%scolumn %u hr 0x%08lX\n", Prefix, Column, hr);
                if (hr == E_INVALIDARG)
                    break;
                continue;
            }
            Text[0] = 0;
            StrRetToBufW(&Details.str, Item, Text, ARRAYSIZE(Text));
            trace("%scolumn %u \"%ls\"\n", Prefix, Column, Text);
        }
        IShellFolder2_Release(Folder2);
    }
}

static void
DumpColumns(IShellFolder *Folder, const char *Prefix)
{
    IShellFolder2 *Folder2;
    SHELLDETAILS Details;
    SHCOLSTATEF State;
    SHCOLUMNID Scid;
    WCHAR Text[256];
    char Fmtid[64];
    UINT Column;
    HRESULT hr, StateHr, ScidHr;

    if (FAILED(IShellFolder_QueryInterface(Folder, &IID_IShellFolder2, (void **)&Folder2)))
        return;
    for (Column = 0; Column < 64; Column++)
    {
        hr = IShellFolder2_GetDetailsOf(Folder2, NULL, Column, &Details);
        if (FAILED(hr))
        {
            trace("%sheader %u hr 0x%08lX\n", Prefix, Column, hr);
            break;
        }
        Text[0] = 0;
        StrRetToBufW(&Details.str, NULL, Text, ARRAYSIZE(Text));
        State = 0;
        StateHr = IShellFolder2_GetDefaultColumnState(Folder2, Column, &State);
        memset(&Scid, 0, sizeof(Scid));
        ScidHr = IShellFolder2_MapColumnToSCID(Folder2, Column, &Scid);
        WpdFormatGuid(&Scid.fmtid, Fmtid, sizeof(Fmtid));
        trace("%sheader %u \"%ls\" fmt %d state hr 0x%08lX 0x%08lX scid hr 0x%08lX %s %lu\n", Prefix, Column, Text,
              Details.fmt, StateHr, State, ScidHr, Fmtid, Scid.pid);
    }
    IShellFolder2_Release(Folder2);
}

static void
DumpFolder(IShellFolder *Folder, int Depth, BOOL DevicesOnly)
{
    IEnumIDList *Enum;
    PITEMID_CHILD Item;
    IShellFolder *Child;
    STRRET Ret;
    WCHAR Parsing[1024], Lower[1024];
    char Prefix[32];
    ULONG Fetched, Count = 0;
    HRESULT hr;

    memset(Prefix, ' ', sizeof(Prefix));
    Prefix[min(2 * Depth + 2, (int)sizeof(Prefix) - 1)] = 0;
    hr = IShellFolder_EnumObjects(Folder, NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &Enum);
    trace("%sEnumObjects hr 0x%08lX\n", Prefix, hr);
    if (hr != S_OK)
        return;
    if (!DevicesOnly)
        DumpColumns(Folder, Prefix);
    while (Count < 64 && IEnumIDList_Next(Enum, 1, &Item, &Fetched) == S_OK && Fetched)
    {
        Count++;
        Parsing[0] = 0;
        if (SUCCEEDED(IShellFolder_GetDisplayNameOf(Folder, Item, SHGDN_FORPARSING, &Ret)))
            StrRetToBufW(&Ret, Item, Parsing, ARRAYSIZE(Parsing));
        lstrcpynW(Lower, Parsing, ARRAYSIZE(Lower));
        _wcslwr(Lower);
        if (!DevicesOnly || wcsstr(Lower, L"usb#vid_46f4") || wcsstr(Lower, L"wpd"))
        {
            trace("%sitem %lu\n", Prefix, Count);
            DumpItem(Folder, Item, Prefix);
            if (Depth < 4 && SUCCEEDED(IShellFolder_BindToObject(Folder, Item, NULL, &IID_IShellFolder, (void **)&Child)))
            {
                SFGAOF Attributes = SFGAO_FOLDER;
                if (SUCCEEDED(IShellFolder_GetAttributesOf(Folder, 1, (PCUITEMID_CHILD_ARRAY)&Item, &Attributes)) &&
                    (Attributes & SFGAO_FOLDER))
                    DumpFolder(Child, Depth + 1, FALSE);
                IShellFolder_Release(Child);
            }
        }
        else
        {
            trace("%sother \"%ls\"\n", Prefix, Parsing);
        }
        ILFree(Item);
    }
    IEnumIDList_Release(Enum);
}

START_TEST(Shell)
{
    IShellFolder *Desktop, *Computer;
    PIDLIST_RELATIVE Pidl;
    HRESULT hr;

    winetest_mute_threshold = 0x7fffffff;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = SHGetDesktopFolder(&Desktop);
    ok(hr == S_OK, "SHGetDesktopFolder hr 0x%08lX\n", hr);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }
    hr = IShellFolder_ParseDisplayName(Desktop, NULL, NULL, (LPWSTR)L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", NULL, &Pidl, NULL);
    ok(hr == S_OK, "ParseDisplayName(This PC) hr 0x%08lX\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IShellFolder_BindToObject(Desktop, Pidl, NULL, &IID_IShellFolder, (void **)&Computer);
        ok(hr == S_OK, "BindToObject(This PC) hr 0x%08lX\n", hr);
        if (SUCCEEDED(hr))
        {
            DumpFolder(Computer, 0, TRUE);
            IShellFolder_Release(Computer);
        }
        ILFree(Pidl);
    }
    IShellFolder_Release(Desktop);
    CoUninitialize();
}
