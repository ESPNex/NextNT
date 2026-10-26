/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records how IPortableDeviceValues getters treat a stored VT_ERROR value
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

START_TEST(Values)
{
    IPortableDeviceValues *Values;
    PROPVARIANT Value;
    LPWSTR String = NULL;
    GUID Guid;
    ULONG Ulong = 0x55555555;
    ULONGLONG Ulonglong = 0x5555555555555555ULL;
    LONG Long = 0x55555555;
    BOOL Bool = 0x55;
    HRESULT Error = 0x55555555, hr;
    char Text[64];

    winetest_mute_threshold = 0x7fffffff;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = WpdCreateValues(&Values);
    ok(hr == S_OK, "WpdCreateValues hr 0x%08lX\n", hr);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }
    hr = IPortableDeviceValues_SetErrorValue(Values, &WPD_OBJECT_NAME, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    trace("SetErrorValue hr 0x%08lX\n", hr);
    hr = IPortableDeviceValues_GetStringValue(Values, &WPD_OBJECT_NAME, &String);
    trace("GetStringValue hr 0x%08lX %s\n", hr, String ? "string" : "null");
    CoTaskMemFree(String);
    memset(&Guid, 0x55, sizeof(Guid));
    hr = IPortableDeviceValues_GetGuidValue(Values, &WPD_OBJECT_NAME, &Guid);
    WpdFormatGuid(&Guid, Text, sizeof(Text));
    trace("GetGuidValue hr 0x%08lX %s\n", hr, Text);
    hr = IPortableDeviceValues_GetUnsignedIntegerValue(Values, &WPD_OBJECT_NAME, &Ulong);
    trace("GetUnsignedIntegerValue hr 0x%08lX 0x%08lX\n", hr, Ulong);
    hr = IPortableDeviceValues_GetSignedIntegerValue(Values, &WPD_OBJECT_NAME, &Long);
    trace("GetSignedIntegerValue hr 0x%08lX 0x%08lX\n", hr, Long);
    hr = IPortableDeviceValues_GetUnsignedLargeIntegerValue(Values, &WPD_OBJECT_NAME, &Ulonglong);
    trace("GetUnsignedLargeIntegerValue hr 0x%08lX 0x%016I64X\n", hr, Ulonglong);
    hr = IPortableDeviceValues_GetBoolValue(Values, &WPD_OBJECT_NAME, &Bool);
    trace("GetBoolValue hr 0x%08lX 0x%X\n", hr, Bool);
    hr = IPortableDeviceValues_GetErrorValue(Values, &WPD_OBJECT_NAME, &Error);
    trace("GetErrorValue hr 0x%08lX 0x%08lX\n", hr, Error);
    PropVariantInit(&Value);
    hr = IPortableDeviceValues_GetValue(Values, &WPD_OBJECT_NAME, &Value);
    trace("GetValue hr 0x%08lX vt %u scode 0x%08lX\n", hr, Value.vt, Value.vt == VT_ERROR ? Value.scode : 0);
    PropVariantClear(&Value);
    IPortableDeviceValues_Release(Values);
    CoUninitialize();
}
