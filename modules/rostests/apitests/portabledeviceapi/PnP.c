/*
 * PROJECT:     NextNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records the Plug and Play state of the Windows Portable Devices
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <devpkey.h>

typedef BOOL (WINAPI *PFN_GET_PROPERTY_KEYS)(HDEVINFO, PSP_DEVINFO_DATA, DEVPROPKEY *, DWORD, PDWORD, DWORD);
typedef BOOL (WINAPI *PFN_GET_PROPERTY)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, DWORD, PDWORD, DWORD);
typedef BOOL (WINAPI *PFN_GET_INTERFACE_PROPERTY_KEYS)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, DEVPROPKEY *, DWORD, PDWORD, DWORD);
typedef BOOL (WINAPI *PFN_GET_INTERFACE_PROPERTY)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, DWORD, PDWORD, DWORD);

static void
DumpDevProp(const char *Prefix, const DEVPROPKEY *Key, DEVPROPTYPE Type, const BYTE *Data, DWORD Size)
{
    char Guid[64];

    WpdFormatGuid(&Key->fmtid, Guid, sizeof(Guid));
    switch (Type)
    {
        case DEVPROP_TYPE_STRING:
            trace("%s%s.%lu string \"%ls\"\n", Prefix, Guid, Key->pid, (const WCHAR *)Data);
            break;
        case DEVPROP_TYPE_STRING_LIST:
        {
            const WCHAR *p;
            trace("%s%s.%lu string-list\n", Prefix, Guid, Key->pid);
            for (p = (const WCHAR *)Data; *p; p += lstrlenW(p) + 1)
                trace("%s    \"%ls\"\n", Prefix, p);
            break;
        }
        case DEVPROP_TYPE_UINT32:
            trace("%s%s.%lu uint32 0x%lX\n", Prefix, Guid, Key->pid, *(const ULONG *)Data);
            break;
        case DEVPROP_TYPE_BOOLEAN:
            trace("%s%s.%lu bool %d\n", Prefix, Guid, Key->pid, *(const BYTE *)Data);
            break;
        case DEVPROP_TYPE_GUID:
        {
            char Value[64];
            WpdFormatGuid((const GUID *)Data, Value, sizeof(Value));
            trace("%s%s.%lu guid %s\n", Prefix, Guid, Key->pid, Value);
            break;
        }
        default:
            trace("%s%s.%lu type 0x%lX size %lu\n", Prefix, Guid, Key->pid, Type, Size);
            WpdDumpBytes(Prefix, Data, Size, 64);
            break;
    }
}

static void
DumpRegistryKey(const char *Prefix, HKEY Key)
{
    WCHAR Name[256];
    BYTE Data[2048];
    DWORD i, NameChars, Size, Type;

    for (i = 0;; i++)
    {
        NameChars = ARRAYSIZE(Name);
        Size = sizeof(Data) - sizeof(WCHAR) * 2;
        ZeroMemory(Data, sizeof(Data));
        if (RegEnumValueW(Key, i, Name, &NameChars, NULL, &Type, Data, &Size) != ERROR_SUCCESS)
            break;
        if (Type == REG_SZ || Type == REG_EXPAND_SZ)
            trace("%s\"%ls\" sz \"%ls\"\n", Prefix, Name, (WCHAR *)Data);
        else if (Type == REG_DWORD)
            trace("%s\"%ls\" dword 0x%lX\n", Prefix, Name, *(DWORD *)Data);
        else if (Type == REG_MULTI_SZ)
        {
            const WCHAR *p;
            trace("%s\"%ls\" multi\n", Prefix, Name);
            for (p = (WCHAR *)Data; *p; p += lstrlenW(p) + 1)
                trace("%s    \"%ls\"\n", Prefix, p);
        }
        else
        {
            trace("%s\"%ls\" type %lu size %lu\n", Prefix, Name, Type, Size);
            WpdDumpBytes(Prefix, Data, Size, 64);
        }
    }
    for (i = 0;; i++)
    {
        HKEY Sub;
        char SubPrefix[128];

        NameChars = ARRAYSIZE(Name);
        if (RegEnumKeyExW(Key, i, Name, &NameChars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        trace("%s[%ls]\n", Prefix, Name);
        if (RegOpenKeyExW(Key, Name, 0, KEY_READ, &Sub) == ERROR_SUCCESS)
        {
            _snprintf(SubPrefix, sizeof(SubPrefix), "%s  ", Prefix);
            SubPrefix[sizeof(SubPrefix) - 1] = 0;
            DumpRegistryKey(SubPrefix, Sub);
            RegCloseKey(Sub);
        }
    }
}

static void
DumpDeviceClasses(const WCHAR *InstanceId)
{
    HKEY Classes, Class, Interface;
    WCHAR ClassName[64], InterfaceName[512], Instance[512];
    DWORD i, j, Chars, Size;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses", 0, KEY_READ, &Classes))
        return;
    for (i = 0;; i++)
    {
        Chars = ARRAYSIZE(ClassName);
        if (RegEnumKeyExW(Classes, i, ClassName, &Chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(Classes, ClassName, 0, KEY_READ, &Class))
            continue;
        for (j = 0;; j++)
        {
            Chars = ARRAYSIZE(InterfaceName);
            if (RegEnumKeyExW(Class, j, InterfaceName, &Chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExW(Class, InterfaceName, 0, KEY_READ, &Interface))
                continue;
            Size = sizeof(Instance);
            ZeroMemory(Instance, sizeof(Instance));
            if (!RegQueryValueExW(Interface, L"DeviceInstance", NULL, NULL, (BYTE *)Instance, &Size) &&
                !lstrcmpiW(Instance, InstanceId))
            {
                trace("  interface class %ls %ls\n", ClassName, InterfaceName);
                DumpRegistryKey("    ", Interface);
            }
            RegCloseKey(Interface);
        }
        RegCloseKey(Class);
    }
    RegCloseKey(Classes);
}

static void
CopyInf(const WCHAR *InfName)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH];

    if (GetFileAttributesW(L"logs") == INVALID_FILE_ATTRIBUTES)
        return;
    GetWindowsDirectoryW(Source, ARRAYSIZE(Source));
    lstrcatW(Source, L"\\INF\\");
    lstrcatW(Source, InfName);
    lstrcpyW(Target, L"logs\\");
    lstrcatW(Target, InfName);
    lstrcatW(Target, L".txt");
    trace("copy %ls -> %d\n", Source, CopyFileW(Source, Target, FALSE));
}

static void
DumpDevice(HDEVINFO Set, SP_DEVINFO_DATA *Info)
{
    static const struct { DWORD Property; const char *Name; } Properties[] =
    {
        { SPDRP_DEVICEDESC, "DEVICEDESC" }, { SPDRP_HARDWAREID, "HARDWAREID" },
        { SPDRP_COMPATIBLEIDS, "COMPATIBLEIDS" }, { SPDRP_SERVICE, "SERVICE" },
        { SPDRP_CLASS, "CLASS" }, { SPDRP_CLASSGUID, "CLASSGUID" }, { SPDRP_DRIVER, "DRIVER" },
        { SPDRP_CONFIGFLAGS, "CONFIGFLAGS" }, { SPDRP_MFG, "MFG" }, { SPDRP_FRIENDLYNAME, "FRIENDLYNAME" },
        { SPDRP_LOCATION_INFORMATION, "LOCATION_INFORMATION" },
        { SPDRP_PHYSICAL_DEVICE_OBJECT_NAME, "PDO" }, { SPDRP_CAPABILITIES, "CAPABILITIES" },
        { SPDRP_UPPERFILTERS, "UPPERFILTERS" }, { SPDRP_LOWERFILTERS, "LOWERFILTERS" },
        { SPDRP_ENUMERATOR_NAME, "ENUMERATOR_NAME" }, { SPDRP_DEVTYPE, "DEVTYPE" },
        { SPDRP_CHARACTERISTICS, "CHARACTERISTICS" }, { SPDRP_REMOVAL_POLICY, "REMOVAL_POLICY" },
        { SPDRP_INSTALL_STATE, "INSTALL_STATE" }, { SPDRP_LOCATION_PATHS, "LOCATION_PATHS" },
    };
    BYTE Data[4096];
    WCHAR Instance[512];
    DWORD i, Type, Size, Status, Problem;
    HKEY Key;
    DEVINST Parent;
    PFN_GET_PROPERTY_KEYS GetKeys;
    PFN_GET_PROPERTY GetProperty;
    HMODULE SetupApi = GetModuleHandleW(L"setupapi.dll");

    SetupDiGetDeviceInstanceIdW(Set, Info, Instance, ARRAYSIZE(Instance), NULL);
    trace("device %ls\n", Instance);
    if (CM_Get_DevNode_Status(&Status, &Problem, Info->DevInst, 0) == CR_SUCCESS)
        trace("  status 0x%lX problem %lu\n", Status, Problem);

    for (i = 0; i < ARRAYSIZE(Properties); i++)
    {
        ZeroMemory(Data, sizeof(Data));
        if (!SetupDiGetDeviceRegistryPropertyW(Set, Info, Properties[i].Property, &Type, Data, sizeof(Data) - 4, &Size))
            continue;
        if (Type == REG_SZ)
            trace("  %s \"%ls\"\n", Properties[i].Name, (WCHAR *)Data);
        else if (Type == REG_MULTI_SZ)
        {
            const WCHAR *p;
            trace("  %s\n", Properties[i].Name);
            for (p = (WCHAR *)Data; *p; p += lstrlenW(p) + 1)
                trace("      \"%ls\"\n", p);
        }
        else if (Type == REG_DWORD)
            trace("  %s 0x%lX\n", Properties[i].Name, *(DWORD *)Data);
        else
            trace("  %s type %lu size %lu\n", Properties[i].Name, Type, Size);
    }

    GetKeys = (PFN_GET_PROPERTY_KEYS)GetProcAddress(SetupApi, "SetupDiGetDevicePropertyKeys");
    GetProperty = (PFN_GET_PROPERTY)GetProcAddress(SetupApi, "SetupDiGetDevicePropertyW");
    if (GetKeys && GetProperty)
    {
        DEVPROPKEY Keys[256];
        DWORD Count = 0;

        if (GetKeys(Set, Info, Keys, ARRAYSIZE(Keys), &Count, 0))
        {
            trace("  devprops %lu\n", Count);
            for (i = 0; i < Count; i++)
            {
                DEVPROPTYPE PropType;
                ZeroMemory(Data, sizeof(Data));
                if (GetProperty(Set, Info, &Keys[i], &PropType, Data, sizeof(Data) - 4, &Size, 0))
                    DumpDevProp("    ", &Keys[i], PropType, Data, Size);
            }
        }
    }

    Key = SetupDiOpenDevRegKey(Set, Info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (Key != INVALID_HANDLE_VALUE)
    {
        trace("  device key\n");
        DumpRegistryKey("    ", Key);
        RegCloseKey(Key);
    }
    Key = SetupDiOpenDevRegKey(Set, Info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (Key != INVALID_HANDLE_VALUE)
    {
        WCHAR Inf[MAX_PATH];

        trace("  driver key\n");
        DumpRegistryKey("    ", Key);
        Size = sizeof(Inf);
        ZeroMemory(Inf, sizeof(Inf));
        if (!RegQueryValueExW(Key, L"InfPath", NULL, NULL, (BYTE *)Inf, &Size))
            CopyInf(Inf);
        RegCloseKey(Key);
    }

    DumpDeviceClasses(Instance);

    if (CM_Get_Parent(&Parent, Info->DevInst, 0) == CR_SUCCESS &&
        CM_Get_Device_IDW(Parent, Instance, ARRAYSIZE(Instance), 0) == CR_SUCCESS)
    {
        trace("  parent %ls\n", Instance);
    }
}

static void
DumpInterfaces(const GUID *InterfaceGuid, const char *Name)
{
    HDEVINFO Set;
    SP_DEVICE_INTERFACE_DATA Interface;
    SP_DEVINFO_DATA Info;
    BYTE Buffer[2048];
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)Buffer;
    DWORD i, j;
    HKEY Key;
    PFN_GET_INTERFACE_PROPERTY_KEYS GetKeys;
    PFN_GET_INTERFACE_PROPERTY GetProperty;
    HMODULE SetupApi = GetModuleHandleW(L"setupapi.dll");

    GetKeys = (PFN_GET_INTERFACE_PROPERTY_KEYS)GetProcAddress(SetupApi, "SetupDiGetDeviceInterfacePropertyKeys");
    GetProperty = (PFN_GET_INTERFACE_PROPERTY)GetProcAddress(SetupApi, "SetupDiGetDeviceInterfacePropertyW");

    Set = SetupDiGetClassDevsW(InterfaceGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiGetClassDevs(%s) error %lu\n", Name, GetLastError());
    if (Set == INVALID_HANDLE_VALUE)
        return;
    for (i = 0;; i++)
    {
        Interface.cbSize = sizeof(Interface);
        if (!SetupDiEnumDeviceInterfaces(Set, NULL, InterfaceGuid, i, &Interface))
            break;
        Detail->cbSize = sizeof(*Detail);
        Info.cbSize = sizeof(Info);
        if (!SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, sizeof(Buffer), NULL, &Info))
            continue;
        trace("%s interface %lu \"%ls\" flags 0x%lX\n", Name, i, Detail->DevicePath, Interface.Flags);
        Key = SetupDiOpenDeviceInterfaceRegKey(Set, &Interface, 0, KEY_READ);
        if (Key != INVALID_HANDLE_VALUE)
        {
            trace("  interface key\n");
            DumpRegistryKey("    ", Key);
            RegCloseKey(Key);
        }
        if (GetKeys && GetProperty)
        {
            DEVPROPKEY Keys[128];
            DWORD Count = 0, Size;

            if (GetKeys(Set, &Interface, Keys, ARRAYSIZE(Keys), &Count, 0))
            {
                trace("  interface devprops %lu\n", Count);
                for (j = 0; j < Count; j++)
                {
                    DEVPROPTYPE PropType;
                    BYTE Data[2048];
                    ZeroMemory(Data, sizeof(Data));
                    if (GetProperty(Set, &Interface, &Keys[j], &PropType, Data, sizeof(Data) - 4, &Size, 0))
                        DumpDevProp("    ", &Keys[j], PropType, Data, Size);
                }
            }
        }
        DumpDevice(Set, &Info);
    }
    SetupDiDestroyDeviceInfoList(Set);
}

static void
DumpClassKey(void)
{
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{EEC5AD98-8080-425F-922A-DABF3DE3F69A}",
                      0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        trace("class key WPD\n");
        DumpRegistryKey("  ", Key);
        RegCloseKey(Key);
    }
    else
    {
        trace("class key WPD missing\n");
    }
}

START_TEST(PnP)
{
    IPortableDeviceManager *Manager;
    WCHAR PnPId[512];

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (WpdWaitForDevice(&Manager, PnPId, ARRAYSIZE(PnPId)))
    {
        IPortableDeviceManager_Release(Manager);
        DumpInterfaces(&GUID_DEVINTERFACE_WPD, "WPD");
        DumpInterfaces(&GUID_DEVINTERFACE_WPD_PRIVATE, "WPD_PRIVATE");
        DumpInterfaces(&GUID_DEVINTERFACE_WPD_SERVICE, "WPD_SERVICE");
        DumpClassKey();
    }
    CoUninitialize();
}
