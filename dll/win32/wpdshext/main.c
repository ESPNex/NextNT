/*
 * PROJECT:     LiberNT Portable Devices shell extension
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Class factory and registration
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdshext.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpdshext);

HINSTANCE wpdshext_instance;
LONG wpdshext_locks;

const CLSID CLSID_WpdShellDelegateFolder = { 0x35786d3c, 0xb075, 0x49b9, { 0x88, 0xdd, 0x02, 0x98, 0x76, 0xe1, 0x1c, 0x01 } };
const CLSID CLSID_WpdShellFolder = { 0xd6791a63, 0xe7e2, 0x4fee, { 0xbf, 0x52, 0x5d, 0xed, 0x8e, 0x86, 0xe9, 0xb8 } };

static const WCHAR delegate_key[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\DelegateFolders\\"
    L"{35786D3C-B075-49b9-88DD-029876E11C01}";

typedef struct wpd_factory
{
    IClassFactory IClassFactory_iface;
    const CLSID *clsid;
} wpd_factory;

static inline wpd_factory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, wpd_factory, IClassFactory_iface);
}

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory))
    {
        *out = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface)
{
    InterlockedIncrement(&wpdshext_locks);
    return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
    InterlockedDecrement(&wpdshext_locks);
    return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    return wpd_folder_create(impl_from_IClassFactory(iface)->clsid, iid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
    if (lock)
        InterlockedIncrement(&wpdshext_locks);
    else
        InterlockedDecrement(&wpdshext_locks);
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateInstance,
    factory_LockServer,
};

static wpd_factory delegate_factory = { { &factory_vtbl }, &CLSID_WpdShellDelegateFolder };
static wpd_factory folder_factory = { { &factory_vtbl }, &CLSID_WpdShellFolder };

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        wpdshext_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (IsEqualCLSID(clsid, &CLSID_WpdShellDelegateFolder))
        return IClassFactory_QueryInterface(&delegate_factory.IClassFactory_iface, iid, out);
    if (IsEqualCLSID(clsid, &CLSID_WpdShellFolder))
        return IClassFactory_QueryInterface(&folder_factory.IClassFactory_iface, iid, out);
    FIXME("unknown class %s\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (wpdshext_locks)
        return S_FALSE;
    wpd_free_devices();
    return S_OK;
}

static LSTATUS set_string(HKEY key, const WCHAR *name, DWORD type, const WCHAR *value)
{
    return RegSetValueExW(key, name, 0, type, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
}

static LSTATUS register_class(const WCHAR *clsid, const WCHAR *name, int localized, DWORD attributes, DWORD value_flags)
{
    WCHAR path[128], text[64];
    HKEY key, sub;
    LSTATUS status;
    DWORD value;

    wsprintfW(path, L"CLSID\\%s", clsid);
    status = RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if (status)
        return status;
    if (name)
        set_string(key, NULL, REG_SZ, name);
    set_string(key, L"InfoTip", REG_EXPAND_SZ, L"@%SystemRoot%\\system32\\wpdshext.dll,-512");
    wsprintfW(text, L"@%%SystemRoot%%\\system32\\wpdshext.dll,-%d", localized);
    set_string(key, L"LocalizedString", REG_EXPAND_SZ, text);
    status = RegCreateKeyExW(key, L"InProcServer32", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL);
    if (!status)
    {
        set_string(sub, NULL, REG_EXPAND_SZ, L"%SystemRoot%\\system32\\wpdshext.dll");
        set_string(sub, L"ThreadingModel", REG_SZ, L"Both");
        RegCloseKey(sub);
    }
    if (attributes && !status)
    {
        status = RegCreateKeyExW(key, L"ShellFolder", 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL);
        if (!status)
        {
            value = attributes;
            RegSetValueExW(sub, L"Attributes", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
            value = value_flags;
            RegSetValueExW(sub, L"FolderValueFlags", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
            RegCloseKey(sub);
        }
    }
    RegCloseKey(key);
    return status;
}

HRESULT WINAPI DllRegisterServer(void)
{
    LSTATUS status;
    HKEY key;

    status = register_class(L"{35786D3C-B075-49b9-88DD-029876E11C01}", L"Portable Devices", IDS_PORTABLE_DEVICES,
                            0xa0000004, 0x10);
    if (!status)
        status = register_class(L"{D6791A63-E7E2-4fee-BF52-5DED8E86E9B8}", NULL, IDS_PORTABLE_DEVICES_FOLDER, 0, 0);
    if (!status)
        status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, delegate_key, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if (!status)
    {
        status = set_string(key, NULL, REG_SZ, L"Portable Devices");
        RegCloseKey(key);
    }
    return HRESULT_FROM_WIN32(status);
}

HRESULT WINAPI DllUnregisterServer(void)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, delegate_key);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{35786D3C-B075-49b9-88DD-029876E11C01}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{D6791A63-E7E2-4fee-BF52-5DED8E86E9B8}");
    return S_OK;
}
