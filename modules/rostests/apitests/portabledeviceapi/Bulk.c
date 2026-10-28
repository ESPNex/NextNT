/*
 * PROJECT:     NextNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records the bulk property operations of a WPD device
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

typedef struct _BULK_SINK
{
    IPortableDevicePropertiesBulkCallback IPortableDevicePropertiesBulkCallback_iface;
    LONG Ref;
    HANDLE Done;
    ULONG Progress;
    ULONG Objects;
} BULK_SINK;

static BULK_SINK *
SinkFromIface(IPortableDevicePropertiesBulkCallback *Iface)
{
    return CONTAINING_RECORD(Iface, BULK_SINK, IPortableDevicePropertiesBulkCallback_iface);
}

static HRESULT WINAPI
Sink_QueryInterface(IPortableDevicePropertiesBulkCallback *Iface, REFIID Iid, void **Out)
{
    if (IsEqualIID(Iid, &IID_IUnknown) || IsEqualIID(Iid, &IID_IPortableDevicePropertiesBulkCallback))
    {
        *Out = Iface;
        IPortableDevicePropertiesBulkCallback_AddRef(Iface);
        return S_OK;
    }
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Sink_AddRef(IPortableDevicePropertiesBulkCallback *Iface)
{
    return InterlockedIncrement(&SinkFromIface(Iface)->Ref);
}

static ULONG WINAPI
Sink_Release(IPortableDevicePropertiesBulkCallback *Iface)
{
    return InterlockedDecrement(&SinkFromIface(Iface)->Ref);
}

static HRESULT WINAPI
Sink_OnStart(IPortableDevicePropertiesBulkCallback *Iface, REFGUID Context)
{
    char Text[64];

    WpdFormatGuid(Context, Text, sizeof(Text));
    trace("bulk OnStart %s\n", Text);
    return S_OK;
}

static HRESULT WINAPI
Sink_OnProgress(IPortableDevicePropertiesBulkCallback *Iface, REFGUID Context, IPortableDeviceValuesCollection *Results)
{
    BULK_SINK *Sink = SinkFromIface(Iface);
    IPortableDeviceValues *Values;
    DWORD Count = 0, Index;
    char Prefix[32];

    IPortableDeviceValuesCollection_GetCount(Results, &Count);
    trace("bulk OnProgress %lu count %lu\n", Sink->Progress, Count);
    for (Index = 0; Index < Count; Index++)
    {
        if (FAILED(IPortableDeviceValuesCollection_GetAt(Results, Index, &Values)))
            continue;
        _snprintf(Prefix, sizeof(Prefix), "  [%lu] ", Sink->Objects + Index);
        Prefix[sizeof(Prefix) - 1] = 0;
        WpdDumpValues(Prefix, Values, 0);
        IPortableDeviceValues_Release(Values);
    }
    Sink->Objects += Count;
    Sink->Progress++;
    return S_OK;
}

static HRESULT WINAPI
Sink_OnEnd(IPortableDevicePropertiesBulkCallback *Iface, REFGUID Context, HRESULT Status)
{
    BULK_SINK *Sink = SinkFromIface(Iface);

    trace("bulk OnEnd hr 0x%08lX progress %lu objects %lu\n", Status, Sink->Progress, Sink->Objects);
    SetEvent(Sink->Done);
    return S_OK;
}

static IPortableDevicePropertiesBulkCallbackVtbl SinkVtbl =
{
    Sink_QueryInterface,
    Sink_AddRef,
    Sink_Release,
    Sink_OnStart,
    Sink_OnProgress,
    Sink_OnEnd,
};

static void
RunQueued(IPortableDevicePropertiesBulk *Bulk, BULK_SINK *Sink, const char *Name, HRESULT Queued, REFGUID Context)
{
    char Text[64];
    HRESULT hr;
    DWORD Wait;

    WpdFormatGuid(Context, Text, sizeof(Text));
    trace("%s: queue hr 0x%08lX context %s\n", Name, Queued, Text);
    if (FAILED(Queued))
        return;
    Sink->Progress = Sink->Objects = 0;
    ResetEvent(Sink->Done);
    hr = IPortableDevicePropertiesBulk_Start(Bulk, Context);
    trace("%s: Start hr 0x%08lX\n", Name, hr);
    if (FAILED(hr))
        return;
    Wait = WaitForSingleObject(Sink->Done, 20000);
    trace("%s: wait %lu\n", Name, Wait);
}

static HRESULT
CollectIds(WPD_TEST_DEVICE *Test, LPCWSTR Parent, IPortableDevicePropVariantCollection *Ids)
{
    IEnumPortableDeviceObjectIDs *Enum;
    LPWSTR Children[16];
    PROPVARIANT Value;
    ULONG Fetched, Index;
    HRESULT hr;

    hr = IPortableDeviceContent_EnumObjects(Test->Content, 0, Parent, NULL, &Enum);
    if (FAILED(hr))
        return hr;
    for (;;)
    {
        Fetched = 0;
        hr = IEnumPortableDeviceObjectIDs_Next(Enum, ARRAYSIZE(Children), Children, &Fetched);
        for (Index = 0; Index < Fetched; Index++)
        {
            PropVariantInit(&Value);
            Value.vt = VT_LPWSTR;
            Value.pwszVal = Children[Index];
            IPortableDevicePropVariantCollection_Add(Ids, &Value);
            CoTaskMemFree(Children[Index]);
        }
        if (hr != S_OK || !Fetched)
            break;
    }
    IEnumPortableDeviceObjectIDs_Release(Enum);
    return S_OK;
}

START_TEST(Bulk)
{
    IPortableDevicePropertiesBulk *Bulk;
    IPortableDevicePropVariantCollection *Ids;
    IPortableDeviceKeyCollection *Keys;
    IPortableDeviceValuesCollection *Writes;
    IPortableDeviceValues *Write;
    WPD_TEST_DEVICE Test;
    BULK_SINK Sink;
    WCHAR Storage[64];
    GUID Context;
    HRESULT hr;

    WpdTraceIoctlsStart();
    winetest_mute_threshold = 0x7fffffff;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(WpdOpenTestDevice(&Test)) || FAILED(WpdFindStorage(&Test, Storage, ARRAYSIZE(Storage))))
    {
        WpdCloseTestDevice(&Test);
        CoUninitialize();
        return;
    }
    hr = IPortableDeviceProperties_QueryInterface(Test.Properties, &IID_IPortableDevicePropertiesBulk, (void **)&Bulk);
    trace("QI(IPortableDevicePropertiesBulk) hr 0x%08lX\n", hr);
    if (FAILED(hr))
    {
        WpdCloseTestDevice(&Test);
        CoUninitialize();
        return;
    }
    ZeroMemory(&Sink, sizeof(Sink));
    Sink.IPortableDevicePropertiesBulkCallback_iface.lpVtbl = &SinkVtbl;
    Sink.Ref = 1;
    Sink.Done = CreateEventW(NULL, TRUE, FALSE, NULL);

    CoCreateInstance(&CLSID_PortableDevicePropVariantCollection, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IPortableDevicePropVariantCollection, (void **)&Ids);
    CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IPortableDeviceKeyCollection, (void **)&Keys);
    CollectIds(&Test, Storage, Ids);
    IPortableDeviceKeyCollection_Add(Keys, &WPD_OBJECT_NAME);
    IPortableDeviceKeyCollection_Add(Keys, &WPD_OBJECT_SIZE);
    IPortableDeviceKeyCollection_Add(Keys, &WPD_MEDIA_ARTIST);

    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectList(Bulk, Ids, Keys,
                                                                  &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    RunQueued(Bulk, &Sink, "list keys", hr, &Context);
    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectList(Bulk, Ids, NULL,
                                                                  &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    RunQueued(Bulk, &Sink, "list all", hr, &Context);
    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectFormat(Bulk, &WPD_OBJECT_FORMAT_MP3, Storage, 0xFFFFFFFF, Keys,
                                                                    &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    RunQueued(Bulk, &Sink, "format mp3 deep", hr, &Context);
    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectFormat(Bulk, &WPD_OBJECT_FORMAT_MP3, Storage, 1, NULL,
                                                                    &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    RunQueued(Bulk, &Sink, "format mp3 depth1", hr, &Context);
    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectFormat(Bulk, &WPD_OBJECT_FORMAT_ALL, L"", 0, Keys,
                                                                    &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    RunQueued(Bulk, &Sink, "format all root", hr, &Context);

    hr = IPortableDevicePropertiesBulk_QueueGetValuesByObjectList(Bulk, Ids, Keys,
                                                                  &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
    trace("cancel: queue hr 0x%08lX\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IPortableDevicePropertiesBulk_Cancel(Bulk, &Context);
        trace("cancel: Cancel before Start hr 0x%08lX\n", hr);
        hr = IPortableDevicePropertiesBulk_Start(Bulk, &Context);
        trace("cancel: Start after Cancel hr 0x%08lX\n", hr);
    }
    ZeroMemory(&Context, sizeof(Context));
    hr = IPortableDevicePropertiesBulk_Start(Bulk, &Context);
    trace("Start(null context) hr 0x%08lX\n", hr);

    if (SUCCEEDED(CoCreateInstance(&CLSID_PortableDeviceValuesCollection, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IPortableDeviceValuesCollection, (void **)&Writes)))
    {
        if (SUCCEEDED(WpdCreateValues(&Write)))
        {
            PROPVARIANT Value;

            PropVariantInit(&Value);
            if (SUCCEEDED(IPortableDevicePropVariantCollection_GetAt(Ids, 0, &Value)) && Value.vt == VT_LPWSTR)
                IPortableDeviceValues_SetStringValue(Write, &WPD_OBJECT_ID, Value.pwszVal);
            PropVariantClear(&Value);
            IPortableDeviceValues_SetUnsignedIntegerValue(Write, &WPD_OBJECT_SIZE, 1);
            IPortableDeviceValuesCollection_Add(Writes, Write);
            IPortableDeviceValues_Release(Write);
        }
        hr = IPortableDevicePropertiesBulk_QueueSetValuesByObjectList(Bulk, Writes,
                                                                      &Sink.IPortableDevicePropertiesBulkCallback_iface, &Context);
        RunQueued(Bulk, &Sink, "set list", hr, &Context);
        IPortableDeviceValuesCollection_Release(Writes);
    }

    IPortableDeviceKeyCollection_Release(Keys);
    IPortableDevicePropVariantCollection_Release(Ids);
    IPortableDevicePropertiesBulk_Release(Bulk);
    CloseHandle(Sink.Done);
    WpdCloseTestDevice(&Test);
    CoUninitialize();
}
