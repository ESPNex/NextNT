/*
 * IPortableDeviceManager implementation
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

struct device_manager
{
    IPortableDeviceManager IPortableDeviceManager_iface;
    LONG refcount;
    CRITICAL_SECTION cs;
    WCHAR **ids;
    DWORD count;
    WCHAR **private_ids;
    DWORD private_count;
};

static inline struct device_manager *impl_from_IPortableDeviceManager(IPortableDeviceManager *iface)
{
    return CONTAINING_RECORD(iface, struct device_manager, IPortableDeviceManager_iface);
}

static void free_list(WCHAR **ids, DWORD count)
{
    DWORD i;

    for (i = 0; i < count; i++)
        HeapFree(GetProcessHeap(), 0, ids[i]);
    HeapFree(GetProcessHeap(), 0, ids);
}

static HRESULT build_list(const GUID *interface_guid, WCHAR ***ids, DWORD *count)
{
    SP_DEVICE_INTERFACE_DATA iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    WCHAR **list = NULL, **grown;
    DWORD index, size, used = 0, capacity = 0;
    HDEVINFO set;

    *ids = NULL;
    *count = 0;
    set = SetupDiGetClassDevsW(interface_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    for (index = 0;; index++)
    {
        iface_data.cbSize = sizeof(iface_data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, interface_guid, index, &iface_data))
            break;
        size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface_data, NULL, 0, &size, NULL);
        if (!size)
            continue;
        detail = HeapAlloc(GetProcessHeap(), 0, size);
        if (!detail)
            break;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface_data, detail, size, NULL, NULL))
        {
            if (used == capacity)
            {
                capacity = capacity ? capacity * 2 : 4;
                grown = list ? HeapReAlloc(GetProcessHeap(), 0, list, capacity * sizeof(*list))
                             : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*list));
                if (!grown)
                {
                    HeapFree(GetProcessHeap(), 0, detail);
                    break;
                }
                list = grown;
            }
            list[used] = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(detail->DevicePath) + 1) * sizeof(WCHAR));
            if (list[used])
            {
                lstrcpyW(list[used], detail->DevicePath);
                _wcslwr(list[used]);
                used++;
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    *ids = list;
    *count = used;
    return S_OK;
}

static HRESULT copy_list(WCHAR **source, DWORD source_count, LPWSTR *ids, DWORD *count)
{
    DWORD i, n;

    if (!count)
        return E_POINTER;
    if (!ids)
    {
        *count = source_count;
        return S_OK;
    }
    n = min(*count, source_count);
    for (i = 0; i < n; i++)
    {
        ids[i] = strdupW_co(source[i]);
        if (!ids[i])
        {
            while (i)
                CoTaskMemFree(ids[--i]);
            return E_OUTOFMEMORY;
        }
    }
    *count = n;
    return n < source_count ? S_FALSE : S_OK;
}

static BOOL open_device(LPCWSTR pnp_id, HDEVINFO *set, SP_DEVINFO_DATA *info)
{
    SP_DEVICE_INTERFACE_DATA iface_data;

    *set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (*set == INVALID_HANDLE_VALUE)
        return FALSE;
    iface_data.cbSize = sizeof(iface_data);
    info->cbSize = sizeof(*info);
    if (SetupDiOpenDeviceInterfaceW(*set, pnp_id, 0, &iface_data))
    {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        DWORD size = 0;
        BOOL found = FALSE;

        SetupDiGetDeviceInterfaceDetailW(*set, &iface_data, NULL, 0, &size, NULL);
        detail = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
        if (detail)
        {
            detail->cbSize = sizeof(*detail);
            found = SetupDiGetDeviceInterfaceDetailW(*set, &iface_data, detail, size, NULL, info);
            HeapFree(GetProcessHeap(), 0, detail);
        }
        if (found)
            return TRUE;
    }
    SetupDiDestroyDeviceInfoList(*set);
    *set = INVALID_HANDLE_VALUE;
    return FALSE;
}

static HRESULT get_string(LPCWSTR pnp_id, DWORD first, DWORD second, WCHAR *buffer, DWORD *chars)
{
    WCHAR value[512];
    SP_DEVINFO_DATA info;
    HDEVINFO set;
    DWORD type, length;
    BOOL found;

    if (!pnp_id || !chars)
        return E_POINTER;
    if (!open_device(pnp_id, &set, &info))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    found = SetupDiGetDeviceRegistryPropertyW(set, &info, first, &type, (BYTE *)value, sizeof(value), NULL);
    if (!found && second != MAXDWORD)
        found = SetupDiGetDeviceRegistryPropertyW(set, &info, second, &type, (BYTE *)value, sizeof(value), NULL);
    SetupDiDestroyDeviceInfoList(set);
    if (!found)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    value[ARRAY_SIZE(value) - 1] = 0;
    length = lstrlenW(value) + 1;
    if (!buffer)
    {
        *chars = length;
        return S_OK;
    }
    if (*chars < length)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(buffer, value, length * sizeof(WCHAR));
    *chars = length;
    return S_OK;
}

static HRESULT WINAPI manager_QueryInterface(IPortableDeviceManager *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPortableDeviceManager))
        return E_NOINTERFACE;
    *out = iface;
    IPortableDeviceManager_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI manager_AddRef(IPortableDeviceManager *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDeviceManager(iface)->refcount);
}

static ULONG WINAPI manager_Release(IPortableDeviceManager *iface)
{
    struct device_manager *manager = impl_from_IPortableDeviceManager(iface);
    ULONG refcount = InterlockedDecrement(&manager->refcount);

    if (!refcount)
    {
        free_list(manager->ids, manager->count);
        free_list(manager->private_ids, manager->private_count);
        DeleteCriticalSection(&manager->cs);
        HeapFree(GetProcessHeap(), 0, manager);
    }
    return refcount;
}

static HRESULT WINAPI manager_GetDevices(IPortableDeviceManager *iface, LPWSTR *ids, DWORD *count)
{
    struct device_manager *manager = impl_from_IPortableDeviceManager(iface);
    HRESULT hr;

    EnterCriticalSection(&manager->cs);
    hr = copy_list(manager->ids, manager->count, ids, count);
    LeaveCriticalSection(&manager->cs);
    return hr;
}

static HRESULT WINAPI manager_RefreshDeviceList(IPortableDeviceManager *iface)
{
    struct device_manager *manager = impl_from_IPortableDeviceManager(iface);
    WCHAR **ids, **private_ids;
    DWORD count, private_count;
    HRESULT hr;

    hr = build_list(&GUID_DEVINTERFACE_WPD, &ids, &count);
    if (FAILED(hr))
        return hr;
    hr = build_list(&GUID_DEVINTERFACE_WPD_PRIVATE, &private_ids, &private_count);
    if (FAILED(hr))
    {
        free_list(ids, count);
        return hr;
    }
    EnterCriticalSection(&manager->cs);
    free_list(manager->ids, manager->count);
    free_list(manager->private_ids, manager->private_count);
    manager->ids = ids;
    manager->count = count;
    manager->private_ids = private_ids;
    manager->private_count = private_count;
    LeaveCriticalSection(&manager->cs);
    return S_OK;
}

static HRESULT WINAPI manager_GetDeviceFriendlyName(IPortableDeviceManager *iface, LPCWSTR pnp_id, WCHAR *name, DWORD *chars)
{
    return get_string(pnp_id, SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC, name, chars);
}

static HRESULT WINAPI manager_GetDeviceDescription(IPortableDeviceManager *iface, LPCWSTR pnp_id, WCHAR *description, DWORD *chars)
{
    return get_string(pnp_id, SPDRP_DEVICEDESC, MAXDWORD, description, chars);
}

static HRESULT WINAPI manager_GetDeviceManufacturer(IPortableDeviceManager *iface, LPCWSTR pnp_id, WCHAR *manufacturer, DWORD *chars)
{
    return get_string(pnp_id, SPDRP_MFG, MAXDWORD, manufacturer, chars);
}

static HRESULT WINAPI manager_GetDeviceProperty(IPortableDeviceManager *iface, LPCWSTR pnp_id, LPCWSTR name,
                                                BYTE *data, DWORD *size, DWORD *type)
{
    WCHAR instance[MAX_DEVICE_ID_LEN], path[MAX_DEVICE_ID_LEN + 64];
    SP_DEVINFO_DATA info;
    HDEVINFO set;
    HKEY key;
    LONG error = ERROR_FILE_NOT_FOUND;

    if (!pnp_id || !name || !size || !type)
        return E_POINTER;
    if (!open_device(pnp_id, &set, &info))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (key != INVALID_HANDLE_VALUE)
    {
        error = RegQueryValueExW(key, name, NULL, type, data, size);
        RegCloseKey(key);
    }
    if (error == ERROR_FILE_NOT_FOUND &&
        SetupDiGetDeviceInstanceIdW(set, &info, instance, ARRAY_SIZE(instance), NULL))
    {
        lstrcpyW(path, L"SYSTEM\\CurrentControlSet\\Enum\\");
        lstrcatW(path, instance);
        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key))
        {
            error = RegQueryValueExW(key, name, NULL, type, data, size);
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return error ? HRESULT_FROM_WIN32(error) : S_OK;
}

static HRESULT WINAPI manager_GetPrivateDevices(IPortableDeviceManager *iface, LPWSTR *ids, DWORD *count)
{
    struct device_manager *manager = impl_from_IPortableDeviceManager(iface);
    HRESULT hr;

    EnterCriticalSection(&manager->cs);
    hr = copy_list(manager->private_ids, manager->private_count, ids, count);
    LeaveCriticalSection(&manager->cs);
    return hr;
}

static const IPortableDeviceManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    manager_GetDevices,
    manager_RefreshDeviceList,
    manager_GetDeviceFriendlyName,
    manager_GetDeviceDescription,
    manager_GetDeviceManufacturer,
    manager_GetDeviceProperty,
    manager_GetPrivateDevices,
};

HRESULT portable_device_manager_create(REFIID iid, void **out)
{
    struct device_manager *manager;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    manager = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*manager));
    if (!manager)
        return E_OUTOFMEMORY;
    manager->IPortableDeviceManager_iface.lpVtbl = &manager_vtbl;
    manager->refcount = 1;
    InitializeCriticalSection(&manager->cs);
    manager_RefreshDeviceList(&manager->IPortableDeviceManager_iface);
    hr = IPortableDeviceManager_QueryInterface(&manager->IPortableDeviceManager_iface, iid, out);
    IPortableDeviceManager_Release(&manager->IPortableDeviceManager_iface);
    return hr;
}
