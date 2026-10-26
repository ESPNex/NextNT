/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the WpdSerializer buffer format
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static const PROPERTYKEY KeyA = { { 0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } }, 2 };
static const PROPERTYKEY KeyB = { { 0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } }, 3 };
static const GUID GuidC = { 0xAABBCCDD, 0xEEFF, 0x0011, { 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 } };

static void
DumpSerialized(IWpdSerializer *Serializer, const char *Name, IPortableDeviceValues *Values)
{
    BYTE *Buffer = NULL;
    DWORD Size = 0, Serialized = 0, Written;
    HRESULT hr;
    IPortableDeviceValues *Back;
    BYTE Small[8];

    hr = IWpdSerializer_GetSerializedSize(Serializer, Values, &Serialized);
    trace("%s: GetSerializedSize hr 0x%08lX size %lu\n", Name, hr, Serialized);
    hr = IWpdSerializer_GetBufferFromIPortableDeviceValues(Serializer, Values, &Buffer, &Size);
    trace("%s: GetBufferFromIPortableDeviceValues hr 0x%08lX size %lu\n", Name, hr, Size);
    ok(hr == S_OK, "%s: hr 0x%08lX\n", Name, hr);
    if (FAILED(hr))
        return;
    ok(Size == Serialized, "%s: size %lu serialized %lu\n", Name, Size, Serialized);
    WpdDumpBytes("  ", Buffer, Size, 1024);

    Written = 0xdead;
    hr = IWpdSerializer_WriteIPortableDeviceValuesToBuffer(Serializer, sizeof(Small), Values, Small, &Written);
    trace("%s: WriteIPortableDeviceValuesToBuffer(8) hr 0x%08lX written %lu\n", Name, hr, Written);

    Back = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, Size, &Back);
    ok(hr == S_OK, "%s: round trip hr 0x%08lX\n", Name, hr);
    if (Back)
    {
        WpdDumpValues("  back ", Back, 0);
        IPortableDeviceValues_Release(Back);
    }
    Back = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, Size - 1, &Back);
    trace("%s: truncated hr 0x%08lX values %p\n", Name, hr, Back);
    if (Back)
        IPortableDeviceValues_Release(Back);
    CoTaskMemFree(Buffer);
}


static void
DumpOneValue(IWpdSerializer *Serializer, const char *Name, const PROPVARIANT *Value)
{
    IPortableDeviceValues *Values;
    HRESULT hr;

    if (FAILED(WpdCreateValues(&Values)))
        return;
    hr = IPortableDeviceValues_SetValue(Values, &KeyA, Value);
    trace("%s: SetValue hr 0x%08lX\n", Name, hr);
    if (SUCCEEDED(hr))
        DumpSerialized(Serializer, Name, Values);
    IPortableDeviceValues_Release(Values);
}

static void
TestMoreTypes(IWpdSerializer *Serializer)
{
    PROPVARIANT Value;
    ULONG Ui4[2] = { 0x11223344, 0x55667788 };
    BYTE Blob[3] = { 9, 8, 7 };
    IPortableDeviceValues *Values, *A, *B;
    IPortableDeviceValuesCollection *Collection;
    HRESULT hr;

    PropVariantInit(&Value);
    DumpOneValue(Serializer, "empty-vt", &Value);
    Value.vt = VT_NULL;
    DumpOneValue(Serializer, "null-vt", &Value);
    Value.vt = VT_I1;
    Value.cVal = -3;
    DumpOneValue(Serializer, "i1", &Value);
    Value.vt = VT_UI1;
    Value.bVal = 0xAB;
    DumpOneValue(Serializer, "ui1", &Value);
    Value.vt = VT_I2;
    Value.iVal = -2;
    DumpOneValue(Serializer, "i2", &Value);
    Value.vt = VT_I8;
    Value.hVal.QuadPart = -5;
    DumpOneValue(Serializer, "i8", &Value);
    Value.vt = VT_R4;
    Value.fltVal = 2.5f;
    DumpOneValue(Serializer, "r4", &Value);
    Value.vt = VT_INT;
    Value.lVal = -7;
    DumpOneValue(Serializer, "int", &Value);
    Value.vt = VT_UINT;
    Value.ulVal = 7;
    DumpOneValue(Serializer, "uint", &Value);
    Value.vt = VT_LPSTR;
    Value.pszVal = (LPSTR)"Ab";
    DumpOneValue(Serializer, "lpstr", &Value);
    Value.vt = VT_BSTR;
    Value.bstrVal = SysAllocString(L"Ab");
    DumpOneValue(Serializer, "bstr", &Value);
    SysFreeString(Value.bstrVal);
    Value.vt = VT_VECTOR | VT_UI4;
    Value.caul.cElems = 2;
    Value.caul.pElems = Ui4;
    DumpOneValue(Serializer, "vector-ui4", &Value);
    Value.vt = VT_BLOB;
    Value.blob.cbSize = sizeof(Blob);
    Value.blob.pBlobData = Blob;
    DumpOneValue(Serializer, "blob", &Value);
    Value.vt = VT_UNKNOWN;
    Value.punkVal = (IUnknown *)Serializer;
    DumpOneValue(Serializer, "unknown-other", &Value);

    if (FAILED(WpdCreateValues(&Values)))
        return;
    hr = CoCreateInstance(&CLSID_PortableDeviceValuesCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDeviceValuesCollection, (void **)&Collection);
    ok(hr == S_OK, "values collection hr 0x%08lX\n", hr);
    if (SUCCEEDED(hr))
    {
        WpdCreateValues(&A);
        WpdCreateValues(&B);
        IPortableDeviceValues_SetUnsignedIntegerValue(A, &KeyA, 1);
        IPortableDeviceValues_SetStringValue(B, &KeyB, L"x");
        IPortableDeviceValuesCollection_Add(Collection, A);
        IPortableDeviceValuesCollection_Add(Collection, B);
        IPortableDeviceValues_Release(A);
        IPortableDeviceValues_Release(B);
        IPortableDeviceValues_SetIPortableDeviceValuesCollectionValue(Values, &KeyA, Collection);
        IPortableDeviceValuesCollection_Release(Collection);
        DumpSerialized(Serializer, "values-collection", Values);
    }
    IPortableDeviceValues_Release(Values);
}

static void
TestKeyValue(void)
{
    IPortableDeviceValues *Values;
    IPortableDeviceKeyCollection *Keys;
    PROPVARIANT Value;
    PROPERTYKEY Key;
    DWORD Count;
    HRESULT hr;
    char Text[96];

    if (FAILED(WpdCreateValues(&Values)))
        return;
    hr = IPortableDeviceValues_SetKeyValue(Values, &KeyA, &KeyB);
    ok(hr == S_OK, "SetKeyValue hr 0x%08lX\n", hr);
    PropVariantInit(&Value);
    hr = IPortableDeviceValues_GetValue(Values, &KeyA, &Value);
    trace("key value: GetValue hr 0x%08lX vt 0x%X\n", hr, Value.vt);
    if (SUCCEEDED(hr) && Value.vt == VT_UNKNOWN && Value.punkVal)
    {
        hr = IUnknown_QueryInterface(Value.punkVal, &IID_IPortableDeviceKeyCollection, (void **)&Keys);
        trace("key value: QI(KeyCollection) hr 0x%08lX\n", hr);
        if (SUCCEEDED(hr))
        {
            Count = 0;
            IPortableDeviceKeyCollection_GetCount(Keys, &Count);
            trace("key value: count %lu\n", Count);
            IPortableDeviceKeyCollection_Release(Keys);
        }
    }
    PropVariantClear(&Value);

    hr = CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDeviceKeyCollection, (void **)&Keys);
    if (SUCCEEDED(hr))
    {
        IPortableDeviceKeyCollection_Add(Keys, &KeyB);
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(Values, &KeyB, Keys);
        ZeroMemory(&Key, sizeof(Key));
        hr = IPortableDeviceValues_GetKeyValue(Values, &KeyB, &Key);
        WpdFormatKey(&Key, Text, sizeof(Text));
        trace("key collection 1: GetKeyValue hr 0x%08lX key %s\n", hr, Text);
        IPortableDeviceKeyCollection_Add(Keys, &KeyA);
        ZeroMemory(&Key, sizeof(Key));
        hr = IPortableDeviceValues_GetKeyValue(Values, &KeyB, &Key);
        WpdFormatKey(&Key, Text, sizeof(Text));
        trace("key collection 2: GetKeyValue hr 0x%08lX key %s\n", hr, Text);
        IPortableDeviceKeyCollection_Clear(Keys);
        ZeroMemory(&Key, sizeof(Key));
        hr = IPortableDeviceValues_GetKeyValue(Values, &KeyB, &Key);
        trace("key collection 0: GetKeyValue hr 0x%08lX\n", hr);
        IPortableDeviceKeyCollection_Release(Keys);
    }
    IPortableDeviceValues_Release(Values);
}

static void
TestCraftedBuffers(IWpdSerializer *Serializer)
{
    static const BYTE Header[] = { 0x0D, 0, 0, 0, 0x03, 0xD5, 0x15, 0x0C, 0x17, 0xD0, 0xCE, 0x47, 0x90, 0x16, 0x7B, 0x3F, 0x97, 0x87, 0x21, 0xCC };
    BYTE Buffer[64];
    IPortableDeviceValues *Values;
    HRESULT hr;

    memcpy(Buffer, Header, sizeof(Header));
    memset(Buffer + sizeof(Header), 0, sizeof(Buffer) - sizeof(Header));
    Values = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, 30, &Values);
    trace("crafted trailing: hr 0x%08lX values %p\n", hr, Values);
    if (Values)
        IPortableDeviceValues_Release(Values);
    Buffer[0] = 0x13;
    Values = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, 24, &Values);
    trace("crafted vt: hr 0x%08lX values %p\n", hr, Values);
    if (Values)
        IPortableDeviceValues_Release(Values);
    Buffer[0] = 0x0D;
    Buffer[4] = 0x2D;
    Values = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, 24, &Values);
    trace("crafted clsid: hr 0x%08lX values %p\n", hr, Values);
    if (Values)
        IPortableDeviceValues_Release(Values);
    Buffer[4] = 0x03;
    Buffer[20] = 2;
    Values = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, 24, &Values);
    trace("crafted count: hr 0x%08lX values %p\n", hr, Values);
    if (Values)
        IPortableDeviceValues_Release(Values);
    Values = NULL;
    hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, 0, &Values);
    trace("crafted zero: hr 0x%08lX values %p\n", hr, Values);
    if (Values)
        IPortableDeviceValues_Release(Values);
}

START_TEST(Serializer)
{
    IWpdSerializer *Serializer;
    IPortableDeviceValues *Values, *Nested;
    IPortableDeviceKeyCollection *Keys;
    IPortableDevicePropVariantCollection *Collection;
    PROPVARIANT Value;
    BYTE Bytes[5] = { 1, 2, 3, 4, 5 };
    HRESULT hr;

    winetest_mute_threshold = 0x7fffffff;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_WpdSerializer, NULL, CLSCTX_INPROC_SERVER, &IID_IWpdSerializer, (void **)&Serializer);
    ok(hr == S_OK, "CoCreateInstance(WpdSerializer) hr 0x%08lX\n", hr);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }

    hr = WpdCreateValues(&Values);
    ok(hr == S_OK, "values hr 0x%08lX\n", hr);
    DumpSerialized(Serializer, "empty", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetUnsignedIntegerValue(Values, &KeyA, 0x12345678);
    DumpSerialized(Serializer, "ui4", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetStringValue(Values, &KeyA, L"Ab");
    DumpSerialized(Serializer, "lpwstr", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetStringValue(Values, &KeyA, L"");
    DumpSerialized(Serializer, "lpwstr-empty", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetUnsignedLargeIntegerValue(Values, &KeyA, 0x1122334455667788ULL);
    IPortableDeviceValues_SetSignedIntegerValue(Values, &KeyB, -2);
    DumpSerialized(Serializer, "ui8-i4", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetBoolValue(Values, &KeyA, TRUE);
    IPortableDeviceValues_SetErrorValue(Values, &KeyB, E_ACCESSDENIED);
    DumpSerialized(Serializer, "bool-error", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetGuidValue(Values, &KeyA, &GuidC);
    IPortableDeviceValues_SetKeyValue(Values, &KeyB, &KeyA);
    DumpSerialized(Serializer, "guid-key", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetBufferValue(Values, &KeyA, Bytes, sizeof(Bytes));
    DumpSerialized(Serializer, "buffer", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    PropVariantInit(&Value);
    Value.vt = VT_R8;
    Value.dblVal = 1.5;
    IPortableDeviceValues_SetValue(Values, &KeyA, &Value);
    Value.vt = VT_DATE;
    Value.date = 2.25;
    IPortableDeviceValues_SetValue(Values, &KeyB, &Value);
    DumpSerialized(Serializer, "r8-date", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    PropVariantInit(&Value);
    Value.vt = VT_FILETIME;
    Value.filetime.dwLowDateTime = 0x01020304;
    Value.filetime.dwHighDateTime = 0x05060708;
    IPortableDeviceValues_SetValue(Values, &KeyA, &Value);
    Value.vt = VT_UI2;
    Value.uiVal = 0xBEEF;
    IPortableDeviceValues_SetValue(Values, &KeyB, &Value);
    DumpSerialized(Serializer, "filetime-ui2", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    WpdCreateValues(&Nested);
    IPortableDeviceValues_SetUnsignedIntegerValue(Nested, &KeyB, 7);
    IPortableDeviceValues_SetIPortableDeviceValuesValue(Values, &KeyA, Nested);
    IPortableDeviceValues_Release(Nested);
    DumpSerialized(Serializer, "nested-values", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    hr = CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDeviceKeyCollection, (void **)&Keys);
    ok(hr == S_OK, "key collection hr 0x%08lX\n", hr);
    if (SUCCEEDED(hr))
    {
        IPortableDeviceKeyCollection_Add(Keys, &KeyA);
        IPortableDeviceKeyCollection_Add(Keys, &KeyB);
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(Values, &KeyA, Keys);
        IPortableDeviceKeyCollection_Release(Keys);
    }
    DumpSerialized(Serializer, "key-collection", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    hr = CoCreateInstance(&CLSID_PortableDevicePropVariantCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDevicePropVariantCollection, (void **)&Collection);
    ok(hr == S_OK, "propvariant collection hr 0x%08lX\n", hr);
    if (SUCCEEDED(hr))
    {
        PropVariantInit(&Value);
        Value.vt = VT_LPWSTR;
        Value.pwszVal = (LPWSTR)L"o1";
        IPortableDevicePropVariantCollection_Add(Collection, &Value);
        Value.vt = VT_UI4;
        Value.ulVal = 9;
        IPortableDevicePropVariantCollection_Add(Collection, &Value);
        IPortableDeviceValues_SetIPortableDevicePropVariantCollectionValue(Values, &KeyA, Collection);
        IPortableDevicePropVariantCollection_Release(Collection);
    }
    DumpSerialized(Serializer, "propvariant-collection", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetGuidValue(Values, &WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &WPD_CATEGORY_CAPABILITIES);
    IPortableDeviceValues_SetUnsignedIntegerValue(Values, &WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FUNCTIONAL_CATEGORIES.pid);
    DumpSerialized(Serializer, "command", Values);
    IPortableDeviceValues_Release(Values);

    TestMoreTypes(Serializer);
    TestKeyValue();
    TestCraftedBuffers(Serializer);
    IWpdSerializer_Release(Serializer);
    CoUninitialize();
}
