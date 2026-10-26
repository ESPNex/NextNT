/*
 * PROJECT:     LiberNT Portable Devices class installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Names and registers installed WPD devices after what the device reports
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <initguid.h>
#include <portabledevice.h>

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(wpd_ci);

static BOOL get_interface_path(HDEVINFO set, PSP_DEVINFO_DATA info, WCHAR *path, DWORD chars)
{
    SP_DEVICE_INTERFACE_DATA iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    WCHAR instance[MAX_DEVICE_ID_LEN];
    HDEVINFO interfaces;
    DWORD size = 0;
    BOOL ret = FALSE;

    if (!SetupDiGetDeviceInstanceIdW(set, info, instance, ARRAYSIZE(instance), NULL))
        return FALSE;
    interfaces = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_WPD, instance, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (interfaces == INVALID_HANDLE_VALUE)
        return FALSE;
    iface_data.cbSize = sizeof(iface_data);
    if (SetupDiEnumDeviceInterfaces(interfaces, NULL, &GUID_DEVINTERFACE_WPD, 0, &iface_data))
    {
        SetupDiGetDeviceInterfaceDetailW(interfaces, &iface_data, NULL, 0, &size, NULL);
        detail = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
        if (detail)
        {
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(interfaces, &iface_data, detail, size, NULL, NULL) &&
                lstrlenW(detail->DevicePath) < (int)chars)
            {
                lstrcpyW(path, detail->DevicePath);
                ret = TRUE;
            }
            HeapFree(GetProcessHeap(), 0, detail);
        }
    }
    SetupDiDestroyDeviceInfoList(interfaces);
    return ret;
}

static void set_string(HDEVINFO set, PSP_DEVINFO_DATA info, DWORD property, IPortableDeviceValues *values,
                       REFPROPERTYKEY key)
{
    LPWSTR value = NULL;

    if (FAILED(IPortableDeviceValues_GetStringValue(values, key, &value)) || !value)
        return;
    if (value[0] && !SetupDiSetDeviceRegistryPropertyW(set, info, property, (const BYTE *)value,
                                                       (lstrlenW(value) + 1) * sizeof(WCHAR)))
    {
        WARN("property %lu not set, error %lu\n", property, GetLastError());
    }
    CoTaskMemFree(value);
}

static const GUID *const autoplay_types[] =
{
    &WPD_CONTENT_TYPE_IMAGE, &WPD_CONTENT_TYPE_AUDIO, &WPD_CONTENT_TYPE_VIDEO, &WPD_CONTENT_TYPE_DOCUMENT,
    &WPD_CONTENT_TYPE_UNSPECIFIED, &WPD_CONTENT_TYPE_FOLDER, &WPD_CONTENT_TYPE_PLAYLIST,
    &WPD_CONTENT_TYPE_AUDIO_ALBUM, &WPD_CONTENT_TYPE_IMAGE_ALBUM, &WPD_CONTENT_TYPE_VIDEO_ALBUM,
    &WPD_CONTENT_TYPE_MIXED_CONTENT_ALBUM, &WPD_CONTENT_TYPE_CONTACT, &WPD_CONTENT_TYPE_CONTACT_GROUP,
    &WPD_CONTENT_TYPE_CALENDAR, &WPD_CONTENT_TYPE_TASK, &WPD_CONTENT_TYPE_MEMO, &WPD_CONTENT_TYPE_EMAIL,
    &WPD_CONTENT_TYPE_PROGRAM, &WPD_CONTENT_TYPE_NETWORK_ASSOCIATION, &WPD_CONTENT_TYPE_CERTIFICATE,
    &WPD_CONTENT_TYPE_WIRELESS_PROFILE, &WPD_CONTENT_TYPE_MEDIA_CAST, &WPD_CONTENT_TYPE_SECTION,
    &WPD_CONTENT_TYPE_GENERIC_FILE, &WPD_CONTENT_TYPE_GENERIC_MESSAGE, &WPD_CONTENT_TYPE_TELEVISION,
};

static const WCHAR autoplay_root[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers\\DeviceHandlers\\";
static const WCHAR wpd_interface[] = L"{6ac27878-a6fa-4155-ba85-f98f491d4f33}";
static const WCHAR wmdm_provider[] = L"{77F7F122-20B0-4117-A2FB-059D1FC88256}";

static int device_icon(DWORD type)
{
    static const int icons[] = { -701, -702, -704, -703, -705, -706, -707 };

    return type < ARRAYSIZE(icons) ? icons[type] : 0;
}

static BOOL has_content_type(IPortableDevicePropVariantCollection *types, const GUID *type)
{
    PROPVARIANT value;
    DWORD count = 0, i;
    BOOL found = FALSE;

    IPortableDevicePropVariantCollection_GetCount(types, &count);
    for (i = 0; i < count && !found; i++)
    {
        PropVariantInit(&value);
        if (SUCCEEDED(IPortableDevicePropVariantCollection_GetAt(types, i, &value)) && value.vt == VT_CLSID)
            found = IsEqualGUID(value.puuid, type);
        PropVariantClear(&value);
    }
    return found;
}

static void register_autoplay(const WCHAR *handler, IPortableDevicePropVariantCollection *types)
{
    WCHAR path[MAX_PATH], name[64], guid[40];
    HKEY key;
    DWORD i;

    if (!types)
        return;
    lstrcpyW(path, autoplay_root);
    lstrcatW(path, handler);
    lstrcatW(path, L"\\EventHandlers\\DeviceArrival");
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
        return;
    for (i = 0; i < ARRAYSIZE(autoplay_types); i++)
    {
        if (!has_content_type(types, autoplay_types[i]))
            continue;
        StringFromGUID2(autoplay_types[i], guid, ARRAYSIZE(guid));
        _snwprintf(name, ARRAYSIZE(name), L"WPD\\Sink\\%s", guid);
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)wpd_interface, sizeof(wpd_interface));
        _snwprintf(name, ARRAYSIZE(name), L"WPD\\Source\\%s", guid);
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)wpd_interface, sizeof(wpd_interface));
    }
    RegCloseKey(key);
}

static void register_device(HDEVINFO set, PSP_DEVINFO_DATA info, IPortableDevice *device, IPortableDeviceValues *values)
{
    IPortableDeviceCapabilities *caps = NULL;
    IPortableDevicePropVariantCollection *types = NULL, *formats = NULL;
    WCHAR icons[MAX_PATH], container[64], handler[96];
    LPWSTR label = NULL;
    DWORD type = 0, legacy = 0, size, kind, value, i;
    BOOL images = FALSE;
    HKEY key;
    int icon;

    key = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE)
        return;
    if (SUCCEEDED(IPortableDevice_Capabilities(device, &caps)))
    {
        IPortableDeviceCapabilities_GetSupportedContentTypes(caps, &WPD_FUNCTIONAL_CATEGORY_ALL, &types);
        if (SUCCEEDED(IPortableDeviceCapabilities_GetSupportedFormats(caps, &WPD_CONTENT_TYPE_IMAGE, &formats)))
        {
            value = 0;
            IPortableDevicePropVariantCollection_GetCount(formats, &value);
            images = value != 0;
            IPortableDevicePropVariantCollection_Release(formats);
        }
    }

    IPortableDeviceValues_GetUnsignedIntegerValue(values, &WPD_DEVICE_TYPE, &type);
    RegSetValueExW(key, PORTABLE_DEVICE_TYPE, 0, REG_DWORD, (const BYTE *)&type, sizeof(type));
    icon = device_icon(type);
    if (icon)
    {
        size = _snwprintf(icons, ARRAYSIZE(icons) - 1, L"%%SystemRoot%%\\system32\\wpdshext.dll,%d", icon);
        icons[size + 1] = 0;
        RegSetValueExW(key, PORTABLE_DEVICE_ICON, 0, REG_MULTI_SZ, (const BYTE *)icons, (size + 2) * sizeof(WCHAR));
    }

    size = sizeof(legacy);
    if (!RegQueryValueExW(key, L"EnableLegacySupport", NULL, &kind, (BYTE *)&legacy, &size) &&
        kind == REG_DWORD && legacy == 0xFFFFFFFF)
    {
        legacy = 2 | (images ? 1 : 0);
        RegSetValueExW(key, L"EnableLegacySupport", 0, REG_DWORD, (const BYTE *)&legacy, sizeof(legacy));
    }
    if (legacy != 0xFFFFFFFF && (legacy & 2))
    {
        RegSetValueExW(key, L"WMDMSPCLSID", 0, REG_SZ, (const BYTE *)wmdm_provider, sizeof(wmdm_provider));
        value = 1;
        RegSetValueExW(key, L"UseExtendedWmdm", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    }

    if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_BASE_CONTAINERID, NULL, (BYTE *)container,
                                          sizeof(container), NULL))
    {
        for (i = 0; container[i]; i++)
            container[i] = towupper(container[i]);
        _snwprintf(handler, ARRAYSIZE(handler), L"WpdDeviceHandler_%s", container);
        RegSetValueExW(key, L"DeviceHandlers", 0, REG_SZ, (const BYTE *)handler, (lstrlenW(handler) + 1) * sizeof(WCHAR));
        register_autoplay(handler, types);
    }

    if (FAILED(IPortableDeviceValues_GetStringValue(values, &WPD_DEVICE_FRIENDLY_NAME, &label)) || !label || !label[0])
    {
        CoTaskMemFree(label);
        label = NULL;
        IPortableDeviceValues_GetStringValue(values, &WPD_DEVICE_MODEL, &label);
    }
    if (label)
        RegSetValueExW(key, L"Label", 0, REG_SZ, (const BYTE *)label, (lstrlenW(label) + 1) * sizeof(WCHAR));
    CoTaskMemFree(label);
    if (types)
        IPortableDevicePropVariantCollection_Release(types);
    if (caps)
        IPortableDeviceCapabilities_Release(caps);
    RegCloseKey(key);
}

static void name_device(HDEVINFO set, PSP_DEVINFO_DATA info)
{
    IPortableDeviceValues *client = NULL, *values = NULL;
    IPortableDeviceKeyCollection *keys = NULL;
    IPortableDeviceProperties *properties = NULL;
    IPortableDeviceContent *content = NULL;
    IPortableDevice *device = NULL;
    WCHAR path[MAX_PATH];
    HRESULT hr, init;

    if (!get_interface_path(set, info, path, ARRAYSIZE(path)))
        return;
    init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_PortableDeviceFTM, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDevice, (void **)&device);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceValues, (void **)&client);
    if (SUCCEEDED(hr))
    {
        IPortableDeviceValues_SetStringValue(client, &WPD_CLIENT_NAME, L"WPD class installer");
        IPortableDeviceValues_SetUnsignedIntegerValue(client, &WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ);
        hr = IPortableDevice_Open(device, path, client);
    }
    if (SUCCEEDED(hr))
        hr = IPortableDevice_Content(device, &content);
    if (SUCCEEDED(hr))
        hr = IPortableDeviceContent_Properties(content, &properties);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IPortableDeviceKeyCollection, (void **)&keys);
    if (SUCCEEDED(hr))
    {
        IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_MODEL);
        IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_MANUFACTURER);
        IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_FRIENDLY_NAME);
        IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_TYPE);
        hr = IPortableDeviceProperties_GetValues(properties, WPD_DEVICE_OBJECT_ID, keys, &values);
    }
    if (SUCCEEDED(hr))
    {
        set_string(set, info, SPDRP_DEVICEDESC, values, &WPD_DEVICE_MODEL);
        set_string(set, info, SPDRP_MFG, values, &WPD_DEVICE_MANUFACTURER);
        set_string(set, info, SPDRP_FRIENDLYNAME, values, &WPD_DEVICE_FRIENDLY_NAME);
        register_device(set, info, device, values);
    }
    else
    {
        WARN("device strings not read from %s, hr 0x%08lx\n", debugstr_w(path), hr);
    }
    if (values)
        IPortableDeviceValues_Release(values);
    if (keys)
        IPortableDeviceKeyCollection_Release(keys);
    if (properties)
        IPortableDeviceProperties_Release(properties);
    if (content)
        IPortableDeviceContent_Release(content);
    if (client)
        IPortableDeviceValues_Release(client);
    if (device)
    {
        IPortableDevice_Close(device);
        IPortableDevice_Release(device);
    }
    if (SUCCEEDED(init))
        CoUninitialize();
}

DWORD WINAPI WpdClassInstaller(DI_FUNCTION function, HDEVINFO set, PSP_DEVINFO_DATA info)
{
    TRACE("%u %p %p\n", function, set, info);

    if (function != DIF_INSTALLDEVICE || !info)
        return ERROR_DI_DO_DEFAULT;
    if (!SetupDiInstallDevice(set, info))
        return GetLastError();
    name_device(set, info);
    return NO_ERROR;
}
