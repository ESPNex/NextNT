/*
 * PROJECT:     NextNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records serializer edge cases, private commands, USB IDs and property writes of a WPD device
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static const PROPERTYKEY SweepKeyA = { { 0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } }, 2 };
static const PROPERTYKEY SweepKeyB = { { 0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } }, 3 };
static const GUID PrivateCategory = { 0xB520E0C2, 0x41DF, 0x4EBD, { 0x93, 0xE7, 0x5B, 0x99, 0xC1, 0x01, 0x93, 0x2F } };

static void
SweepValues(IWpdSerializer *Serializer, const char *Name, IPortableDeviceValues *Values)
{
    IPortableDeviceValues *Back;
    BYTE *Buffer = NULL, *Out;
    DWORD Size = 0, Len, Written, Count;
    HRESULT hr;

    hr = IWpdSerializer_GetBufferFromIPortableDeviceValues(Serializer, Values, &Buffer, &Size);
    trace("sweep %s size %lu hr 0x%08lX\n", Name, Size, hr);
    if (FAILED(hr))
        return;
    for (Len = 0; Len <= Size; Len++)
    {
        Back = NULL;
        hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(Serializer, Buffer, Len, &Back);
        Count = 0xdead;
        if (Back)
        {
            IPortableDeviceValues_GetCount(Back, &Count);
            IPortableDeviceValues_Release(Back);
        }
        trace("sweep %s read %lu hr 0x%08lX count %lu\n", Name, Len, hr, Count);
    }
    Out = HeapAlloc(GetProcessHeap(), 0, Size + 4);
    if (Out)
    {
        for (Len = 0; Len <= Size + 2; Len++)
        {
            memset(Out, 0xCC, Size + 4);
            Written = 0xdead;
            hr = IWpdSerializer_WriteIPortableDeviceValuesToBuffer(Serializer, Len, Values, Out, &Written);
            trace("sweep %s write %lu hr 0x%08lX written %lu match %d tail 0x%02X\n", Name, Len, hr, Written,
                  Written <= Size && !memcmp(Out, Buffer, Written), Out[Len]);
        }
        HeapFree(GetProcessHeap(), 0, Out);
    }
    CoTaskMemFree(Buffer);
}

static void
TestSerializerSweep(void)
{
    IWpdSerializer *Serializer;
    IPortableDeviceValues *Values, *Nested;
    IPortableDeviceKeyCollection *Keys;
    IPortableDevicePropVariantCollection *Collection;
    PROPVARIANT Value;
    BYTE Bytes[3] = { 1, 2, 3 };
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_WpdSerializer, NULL, CLSCTX_INPROC_SERVER, &IID_IWpdSerializer, (void **)&Serializer);
    ok(hr == S_OK, "CoCreateInstance(WpdSerializer) hr 0x%08lX\n", hr);
    if (FAILED(hr))
        return;

    WpdCreateValues(&Values);
    SweepValues(Serializer, "empty", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetUnsignedIntegerValue(Values, &SweepKeyA, 0x12345678);
    SweepValues(Serializer, "ui4", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetStringValue(Values, &SweepKeyA, L"Ab");
    SweepValues(Serializer, "lpwstr", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetStringValue(Values, &SweepKeyA, L"");
    SweepValues(Serializer, "lpwstr-empty", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    PropVariantInit(&Value);
    Value.vt = VT_NULL;
    IPortableDeviceValues_SetValue(Values, &SweepKeyA, &Value);
    SweepValues(Serializer, "null", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetBufferValue(Values, &SweepKeyA, Bytes, sizeof(Bytes));
    SweepValues(Serializer, "buffer", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    IPortableDeviceValues_SetGuidValue(Values, &SweepKeyA, &PrivateCategory);
    IPortableDeviceValues_SetUnsignedIntegerValue(Values, &SweepKeyB, 5);
    SweepValues(Serializer, "guid-ui4", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    WpdCreateValues(&Nested);
    IPortableDeviceValues_SetUnsignedIntegerValue(Nested, &SweepKeyB, 7);
    IPortableDeviceValues_SetIPortableDeviceValuesValue(Values, &SweepKeyA, Nested);
    IPortableDeviceValues_Release(Nested);
    SweepValues(Serializer, "nested", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    WpdCreateValues(&Nested);
    IPortableDeviceValues_SetIPortableDeviceValuesValue(Values, &SweepKeyA, Nested);
    IPortableDeviceValues_Release(Nested);
    SweepValues(Serializer, "nested-empty", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    if (SUCCEEDED(CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IPortableDeviceKeyCollection, (void **)&Keys)))
    {
        IPortableDeviceKeyCollection_Add(Keys, &SweepKeyA);
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(Values, &SweepKeyB, Keys);
        IPortableDeviceKeyCollection_Release(Keys);
    }
    SweepValues(Serializer, "keys", Values);
    IPortableDeviceValues_Release(Values);

    WpdCreateValues(&Values);
    if (SUCCEEDED(CoCreateInstance(&CLSID_PortableDevicePropVariantCollection, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IPortableDevicePropVariantCollection, (void **)&Collection)))
    {
        PropVariantInit(&Value);
        Value.vt = VT_UI4;
        Value.ulVal = 9;
        IPortableDevicePropVariantCollection_Add(Collection, &Value);
        IPortableDeviceValues_SetIPortableDevicePropVariantCollectionValue(Values, &SweepKeyB, Collection);
        IPortableDevicePropVariantCollection_Release(Collection);
    }
    SweepValues(Serializer, "propvariants", Values);
    IPortableDeviceValues_Release(Values);

    IWpdSerializer_Release(Serializer);
}

static void
DumpMultiSz(const char *Prefix, const WCHAR *List)
{
    for (; *List; List += lstrlenW(List) + 1)
        trace("%s\"%ls\"\n", Prefix, List);
}

static void
TestUsbIds(void)
{
    HDEVINFO Set;
    SP_DEVINFO_DATA Info;
    WCHAR Id[512], Data[2048];
    DWORD i, Type;

    Set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiGetClassDevs(USB) error %lu\n", GetLastError());
    if (Set == INVALID_HANDLE_VALUE)
        return;
    for (i = 0;; i++)
    {
        Info.cbSize = sizeof(Info);
        if (!SetupDiEnumDeviceInfo(Set, i, &Info))
            break;
        if (!SetupDiGetDeviceInstanceIdW(Set, &Info, Id, ARRAYSIZE(Id), NULL))
            continue;
        trace("usb device \"%ls\"\n", Id);
        ZeroMemory(Data, sizeof(Data));
        if (SetupDiGetDeviceRegistryPropertyW(Set, &Info, SPDRP_HARDWAREID, &Type, (BYTE *)Data, sizeof(Data) - 4, NULL))
            DumpMultiSz("  hardware ", Data);
        ZeroMemory(Data, sizeof(Data));
        if (SetupDiGetDeviceRegistryPropertyW(Set, &Info, SPDRP_COMPATIBLEIDS, &Type, (BYTE *)Data, sizeof(Data) - 4, NULL))
            DumpMultiSz("  compatible ", Data);
        ZeroMemory(Data, sizeof(Data));
        if (SetupDiGetDeviceRegistryPropertyW(Set, &Info, SPDRP_SERVICE, &Type, (BYTE *)Data, sizeof(Data) - 4, NULL))
            trace("  service \"%ls\"\n", Data);
    }
    SetupDiDestroyDeviceInfoList(Set);
}

static void
SendPrivate(WPD_TEST_DEVICE *Test, DWORD Id, LPCWSTR ObjectId)
{
    IPortableDeviceValues *Params, *Results;
    HRESULT hr;

    if (FAILED(WpdCreateValues(&Params)))
        return;
    IPortableDeviceValues_SetGuidValue(Params, &WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &PrivateCategory);
    IPortableDeviceValues_SetUnsignedIntegerValue(Params, &WPD_PROPERTY_COMMON_COMMAND_ID, Id);
    if (ObjectId)
        IPortableDeviceValues_SetStringValue(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, ObjectId);
    Results = NULL;
    hr = IPortableDevice_SendCommand(Test->Device, 0, Params, &Results);
    trace("private .%lu object \"%ls\" hr 0x%08lX\n", Id, ObjectId ? ObjectId : L"(none)", hr);
    if (Results)
    {
        WpdDumpValues("  ", Results, 0);
        IPortableDeviceValues_Release(Results);
    }
    IPortableDeviceValues_Release(Params);
}

static HRESULT
FindChild(WPD_TEST_DEVICE *Test, LPCWSTR Parent, LPCWSTR FileName, WCHAR *ObjectId, DWORD Chars)
{
    IEnumPortableDeviceObjectIDs *Enum;
    IPortableDeviceKeyCollection *Keys;
    IPortableDeviceValues *Values;
    LPWSTR Ids[16], Name;
    ULONG Fetched, i;
    HRESULT hr, Result = E_FAIL;

    hr = CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDeviceKeyCollection, (void **)&Keys);
    if (FAILED(hr))
        return hr;
    IPortableDeviceKeyCollection_Add(Keys, &WPD_OBJECT_ORIGINAL_FILE_NAME);
    hr = IPortableDeviceContent_EnumObjects(Test->Content, 0, Parent, NULL, &Enum);
    if (FAILED(hr))
    {
        IPortableDeviceKeyCollection_Release(Keys);
        return hr;
    }
    for (;;)
    {
        Fetched = 0;
        hr = IEnumPortableDeviceObjectIDs_Next(Enum, ARRAYSIZE(Ids), Ids, &Fetched);
        for (i = 0; i < Fetched; i++)
        {
            if (FAILED(Result) &&
                SUCCEEDED(IPortableDeviceProperties_GetValues(Test->Properties, Ids[i], Keys, &Values)))
            {
                if (SUCCEEDED(IPortableDeviceValues_GetStringValue(Values, &WPD_OBJECT_ORIGINAL_FILE_NAME, &Name)))
                {
                    if (!lstrcmpiW(Name, FileName))
                    {
                        lstrcpynW(ObjectId, Ids[i], Chars);
                        Result = S_OK;
                    }
                    CoTaskMemFree(Name);
                }
                IPortableDeviceValues_Release(Values);
            }
            CoTaskMemFree(Ids[i]);
        }
        if (hr != S_OK || !Fetched)
            break;
    }
    IEnumPortableDeviceObjectIDs_Release(Enum);
    IPortableDeviceKeyCollection_Release(Keys);
    return Result;
}

static void
DumpNames(WPD_TEST_DEVICE *Test, LPCWSTR ObjectId)
{
    IPortableDeviceKeyCollection *Keys;
    IPortableDeviceValues *Values;
    HRESULT hr;

    if (FAILED(CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IPortableDeviceKeyCollection, (void **)&Keys)))
        return;
    IPortableDeviceKeyCollection_Add(Keys, &WPD_OBJECT_NAME);
    IPortableDeviceKeyCollection_Add(Keys, &WPD_OBJECT_ORIGINAL_FILE_NAME);
    IPortableDeviceKeyCollection_Add(Keys, &WPD_DEVICE_FRIENDLY_NAME);
    hr = IPortableDeviceProperties_GetValues(Test->Properties, ObjectId, Keys, &Values);
    trace("names of \"%ls\" hr 0x%08lX\n", ObjectId, hr);
    if (SUCCEEDED(hr))
    {
        WpdDumpValues("  ", Values, 0);
        IPortableDeviceValues_Release(Values);
    }
    IPortableDeviceKeyCollection_Release(Keys);
}

static void
SetOne(WPD_TEST_DEVICE *Test, LPCWSTR ObjectId, REFPROPERTYKEY Key, LPCWSTR Text, const char *Name)
{
    IPortableDeviceValues *Values, *Results;
    HRESULT hr;

    if (FAILED(WpdCreateValues(&Values)))
        return;
    IPortableDeviceValues_SetStringValue(Values, Key, Text);
    Results = NULL;
    hr = IPortableDeviceProperties_SetValues(Test->Properties, ObjectId, Values, &Results);
    trace("SetValues(%ls, %s = \"%ls\") hr 0x%08lX\n", ObjectId, Name, Text, hr);
    if (Results)
    {
        WpdDumpValues("  ", Results, 0);
        IPortableDeviceValues_Release(Results);
    }
    IPortableDeviceValues_Release(Values);
    DumpNames(Test, ObjectId);
}

static void
TestWrites(WPD_TEST_DEVICE *Test)
{
    IPortableDeviceValues *Values, *Results;
    WCHAR Storage[64], Object[64];
    HRESULT hr;

    if (FAILED(WpdFindStorage(Test, Storage, ARRAYSIZE(Storage))))
    {
        skip("no storage\n");
        return;
    }
    hr = FindChild(Test, Storage, L"notes.txt", Object, ARRAYSIZE(Object));
    trace("notes.txt hr 0x%08lX \"%ls\"\n", hr, SUCCEEDED(hr) ? Object : L"");
    if (SUCCEEDED(hr))
    {
        DumpNames(Test, Object);
        SetOne(Test, Object, &WPD_OBJECT_ORIGINAL_FILE_NAME, L"renamed.txt", "ORIGINAL_FILE_NAME");
        SetOne(Test, Object, &WPD_OBJECT_NAME, L"renamed2", "NAME");
        SetOne(Test, Object, &WPD_OBJECT_NAME, L"", "NAME");
        SetOne(Test, Object, &WPD_OBJECT_ORIGINAL_FILE_NAME, L"song.mp3", "ORIGINAL_FILE_NAME");

        if (SUCCEEDED(WpdCreateValues(&Values)))
        {
            IPortableDeviceValues_SetStringValue(Values, &WPD_OBJECT_ORIGINAL_FILE_NAME, L"both.txt");
            IPortableDeviceValues_SetStringValue(Values, &WPD_OBJECT_PERSISTENT_UNIQUE_ID, L"x");
            IPortableDeviceValues_SetUnsignedIntegerValue(Values, &WPD_OBJECT_SIZE, 1);
            Results = NULL;
            hr = IPortableDeviceProperties_SetValues(Test->Properties, Object, Values, &Results);
            trace("SetValues(%ls, name+puid+size) hr 0x%08lX\n", Object, hr);
            if (Results)
            {
                WpdDumpValues("  ", Results, 0);
                IPortableDeviceValues_Release(Results);
            }
            IPortableDeviceValues_Release(Values);
            DumpNames(Test, Object);
        }
    }
    hr = FindChild(Test, Storage, L"Music", Object, ARRAYSIZE(Object));
    trace("Music hr 0x%08lX \"%ls\"\n", hr, SUCCEEDED(hr) ? Object : L"");
    if (SUCCEEDED(hr))
    {
        SetOne(Test, Object, &WPD_OBJECT_ORIGINAL_FILE_NAME, L"Tunes", "ORIGINAL_FILE_NAME");
        SetOne(Test, Object, &WPD_OBJECT_ORIGINAL_FILE_NAME, L"Music", "ORIGINAL_FILE_NAME");
    }
    SetOne(Test, Storage, &WPD_OBJECT_NAME, L"Store", "NAME");
    DumpNames(Test, WPD_DEVICE_OBJECT_ID);
    SetOne(Test, WPD_DEVICE_OBJECT_ID, &WPD_DEVICE_FRIENDLY_NAME, L"Renamed Rich", "DEVICE_FRIENDLY_NAME");
    SetOne(Test, WPD_DEVICE_OBJECT_ID, &WPD_DEVICE_FRIENDLY_NAME, L"QEMU Rich MTP", "DEVICE_FRIENDLY_NAME");
}

START_TEST(Probe)
{
    WPD_TEST_DEVICE Test;

    WpdTraceIoctlsStart();
    winetest_mute_threshold = 0x7fffffff;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    TestSerializerSweep();
    TestUsbIds();
    if (SUCCEEDED(WpdOpenTestDevice(&Test)))
    {
        SendPrivate(&Test, 11, NULL);
        SendPrivate(&Test, 12, NULL);
        SendPrivate(&Test, 11, WPD_DEVICE_OBJECT_ID);
        SendPrivate(&Test, 12, WPD_DEVICE_OBJECT_ID);
        SendPrivate(&Test, 13, NULL);
        TestWrites(&Test);
    }
    WpdCloseTestDevice(&Test);
    CoUninitialize();
}
