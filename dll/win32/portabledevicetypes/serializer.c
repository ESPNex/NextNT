/*
 * IWpdSerializer implementation
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

#define SERIALIZER_MAX_DEPTH 16

struct wpd_serializer
{
    IWpdSerializer IWpdSerializer_iface;
    LONG refcount;
};

struct writer
{
    BYTE *data;
    DWORD size;
    DWORD offset;
    BOOL overflow;
};

struct reader
{
    const BYTE *data;
    DWORD size;
    DWORD offset;
};

static inline struct wpd_serializer *impl_from_IWpdSerializer(IWpdSerializer *iface)
{
    return CONTAINING_RECORD(iface, struct wpd_serializer, IWpdSerializer_iface);
}

static void put_data(struct writer *writer, const void *data, DWORD size, BOOL soft)
{
    DWORD room;

    if (writer->data && size && !writer->overflow)
    {
        room = writer->offset < writer->size ? writer->size - writer->offset : 0;
        if (room >= size)
            memcpy(writer->data + writer->offset, data, size);
        else if (soft)
            memcpy(writer->data + writer->offset, data, room);
        else
            writer->overflow = TRUE;
    }
    writer->offset += size;
}

static void put_bytes(struct writer *writer, const void *data, DWORD size)
{
    put_data(writer, data, size, FALSE);
}

static void put_soft(struct writer *writer, const void *data, DWORD size)
{
    put_data(writer, data, size, TRUE);
}

static void put_u32(struct writer *writer, DWORD value)
{
    put_bytes(writer, &value, sizeof(value));
}

static void put_soft_u32(struct writer *writer, DWORD value)
{
    put_soft(writer, &value, sizeof(value));
}

static HRESULT write_values(struct writer *writer, IPortableDeviceValues *values, int depth);
static HRESULT write_value(struct writer *writer, const PROPVARIANT *value, int depth);

static HRESULT write_unknown(struct writer *writer, IUnknown *unknown, int depth)
{
    IPortableDeviceValues *values;
    IPortableDeviceKeyCollection *keys;
    IPortableDevicePropVariantCollection *variants;
    IPortableDeviceValuesCollection *list;
    PROPVARIANT item;
    PROPERTYKEY key;
    DWORD count = 0, i;
    HRESULT hr;

    if (!unknown)
        return E_INVALIDARG;
    if (depth > SERIALIZER_MAX_DEPTH)
        return E_INVALIDARG;
    if (SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IPortableDeviceValues, (void **)&values)))
    {
        hr = write_values(writer, values, depth + 1);
        IPortableDeviceValues_Release(values);
        return hr;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IPortableDeviceKeyCollection, (void **)&keys)))
    {
        hr = IPortableDeviceKeyCollection_GetCount(keys, &count);
        if (SUCCEEDED(hr))
        {
            put_soft(writer, &CLSID_PortableDeviceKeyCollection, sizeof(GUID));
            put_soft_u32(writer, count);
            for (i = 0; i < count && SUCCEEDED(hr); i++)
            {
                hr = IPortableDeviceKeyCollection_GetAt(keys, i, &key);
                if (SUCCEEDED(hr))
                    put_bytes(writer, &key, sizeof(key));
            }
        }
        IPortableDeviceKeyCollection_Release(keys);
        return hr;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IPortableDevicePropVariantCollection, (void **)&variants)))
    {
        hr = IPortableDevicePropVariantCollection_GetCount(variants, &count);
        if (SUCCEEDED(hr))
        {
            put_soft(writer, &CLSID_PortableDevicePropVariantCollection, sizeof(GUID));
            put_soft_u32(writer, count);
            for (i = 0; i < count && SUCCEEDED(hr); i++)
            {
                PropVariantInit(&item);
                hr = IPortableDevicePropVariantCollection_GetAt(variants, i, &item);
                if (SUCCEEDED(hr))
                    hr = write_value(writer, &item, depth + 1);
                PropVariantClear(&item);
            }
        }
        IPortableDevicePropVariantCollection_Release(variants);
        return hr;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IPortableDeviceValuesCollection, (void **)&list)))
    {
        hr = IPortableDeviceValuesCollection_GetCount(list, &count);
        if (SUCCEEDED(hr))
        {
            put_soft(writer, &CLSID_PortableDeviceValuesCollection, sizeof(GUID));
            put_soft_u32(writer, count);
            for (i = 0; i < count && SUCCEEDED(hr); i++)
            {
                hr = IPortableDeviceValuesCollection_GetAt(list, i, &values);
                if (SUCCEEDED(hr))
                {
                    hr = write_values(writer, values, depth + 1);
                    IPortableDeviceValues_Release(values);
                }
            }
        }
        IPortableDeviceValuesCollection_Release(list);
        return hr;
    }
    return E_NOINTERFACE;
}

static HRESULT write_value(struct writer *writer, const PROPVARIANT *value, int depth)
{
    DWORD length;

    switch (value->vt)
    {
        case VT_EMPTY:
        case VT_NULL:
            put_soft_u32(writer, value->vt);
            return S_OK;
        case VT_I1:
        case VT_UI1:
            put_soft_u32(writer, value->vt);
            put_bytes(writer, &value->bVal, 1);
            return S_OK;
        case VT_I2:
        case VT_UI2:
        case VT_BOOL:
            put_soft_u32(writer, value->vt);
            put_bytes(writer, &value->uiVal, 2);
            return S_OK;
        case VT_I4:
        case VT_UI4:
        case VT_INT:
        case VT_UINT:
        case VT_ERROR:
        case VT_R4:
            put_soft_u32(writer, value->vt);
            put_bytes(writer, &value->ulVal, 4);
            return S_OK;
        case VT_I8:
        case VT_UI8:
        case VT_R8:
        case VT_DATE:
        case VT_FILETIME:
            put_soft_u32(writer, value->vt);
            put_bytes(writer, &value->uhVal, 8);
            return S_OK;
        case VT_CLSID:
            if (!value->puuid)
                return E_INVALIDARG;
            put_soft_u32(writer, VT_CLSID);
            put_bytes(writer, value->puuid, sizeof(GUID));
            return S_OK;
        case VT_LPWSTR:
        case VT_BSTR:
        {
            const WCHAR *string = value->vt == VT_LPWSTR ? value->pwszVal : value->bstrVal;

            if (!string)
                string = L"";
            length = (DWORD)((lstrlenW(string) + 1) * sizeof(WCHAR));
            put_soft_u32(writer, VT_LPWSTR);
            put_u32(writer, length);
            put_bytes(writer, string, length);
            return S_OK;
        }
        case VT_VECTOR | VT_UI1:
            put_soft_u32(writer, value->vt);
            put_u32(writer, value->caub.cElems);
            put_bytes(writer, value->caub.pElems, value->caub.cElems);
            return S_OK;
        case VT_UNKNOWN:
            put_soft_u32(writer, VT_UNKNOWN);
            return write_unknown(writer, value->punkVal, depth);
        default:
            return E_INVALIDARG;
    }
}

static HRESULT write_values(struct writer *writer, IPortableDeviceValues *values, int depth)
{
    PROPERTYKEY key;
    PROPVARIANT value;
    DWORD count = 0, written = 0, i, count_offset;
    HRESULT hr;

    hr = IPortableDeviceValues_GetCount(values, &count);
    if (FAILED(hr))
        return hr;
    put_soft(writer, &CLSID_PortableDeviceValues, sizeof(GUID));
    count_offset = writer->offset;
    put_soft_u32(writer, 0);
    for (i = 0; i < count && SUCCEEDED(hr); i++)
    {
        PropVariantInit(&value);
        hr = IPortableDeviceValues_GetAt(values, i, &key, &value);
        if (SUCCEEDED(hr) && value.vt != VT_EMPTY)
        {
            put_bytes(writer, &key, sizeof(key));
            hr = write_value(writer, &value, depth);
            written++;
        }
        PropVariantClear(&value);
    }
    if (writer->data && !writer->overflow && count_offset < writer->size)
        memcpy(writer->data + count_offset, &written, min(sizeof(written), writer->size - count_offset));
    return hr;
}

static HRESULT serialize(IPortableDeviceValues *values, BYTE *buffer, DWORD capacity, DWORD *size)
{
    struct writer writer = { buffer, capacity, 0, FALSE };
    HRESULT hr;

    put_soft_u32(&writer, VT_UNKNOWN);
    hr = write_values(&writer, values, 0);
    if (SUCCEEDED(hr) && writer.overflow)
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *size = SUCCEEDED(hr) ? (buffer ? min(writer.offset, capacity) : writer.offset) : 0;
    return hr;
}

static BOOL get_bytes(struct reader *reader, void *data, DWORD size)
{
    if (reader->size - reader->offset < size)
        return FALSE;
    memcpy(data, reader->data + reader->offset, size);
    reader->offset += size;
    return TRUE;
}

static BOOL get_u32(struct reader *reader, DWORD *value)
{
    return get_bytes(reader, value, sizeof(*value));
}

static DWORD get_partial_u32(struct reader *reader)
{
    DWORD value = 0, size = min(sizeof(value), reader->size - reader->offset);

    get_bytes(reader, &value, size);
    return value;
}

static HRESULT read_values_body(struct reader *reader, IPortableDeviceValues **result, int depth);
static HRESULT read_value(struct reader *reader, PROPVARIANT *value, int depth);

static HRESULT read_collection(struct reader *reader, IUnknown **result, int depth)
{
    IPortableDeviceKeyCollection *keys;
    IPortableDevicePropVariantCollection *variants;
    IPortableDeviceValuesCollection *list;
    IPortableDeviceValues *values;
    PROPVARIANT item;
    PROPERTYKEY key;
    GUID clsid;
    DWORD count, i;
    HRESULT hr;

    *result = NULL;
    if (depth > SERIALIZER_MAX_DEPTH || !get_bytes(reader, &clsid, sizeof(clsid)))
        return E_UNEXPECTED;
    if (IsEqualGUID(&clsid, &CLSID_PortableDeviceValues))
    {
        reader->offset -= sizeof(clsid);
        hr = read_values_body(reader, &values, depth + 1);
        if (SUCCEEDED(hr))
            *result = (IUnknown *)values;
        return hr;
    }
    count = get_partial_u32(reader);
    if (IsEqualGUID(&clsid, &CLSID_PortableDeviceKeyCollection))
    {
        hr = portable_device_key_collection_create(&IID_IPortableDeviceKeyCollection, (void **)&keys);
        for (i = 0; SUCCEEDED(hr) && i < count; i++)
        {
            if (!get_bytes(reader, &key, sizeof(key)))
                hr = E_UNEXPECTED;
            else
                hr = IPortableDeviceKeyCollection_Add(keys, &key);
        }
        if (SUCCEEDED(hr))
            *result = (IUnknown *)keys;
        else if (keys)
            IPortableDeviceKeyCollection_Release(keys);
        return hr;
    }
    if (IsEqualGUID(&clsid, &CLSID_PortableDevicePropVariantCollection))
    {
        hr = portable_device_propvariant_collection_create(&IID_IPortableDevicePropVariantCollection, (void **)&variants);
        for (i = 0; SUCCEEDED(hr) && i < count; i++)
        {
            PropVariantInit(&item);
            hr = read_value(reader, &item, depth + 1);
            if (SUCCEEDED(hr))
                hr = IPortableDevicePropVariantCollection_Add(variants, &item);
            PropVariantClear(&item);
        }
        if (SUCCEEDED(hr))
            *result = (IUnknown *)variants;
        else if (variants)
            IPortableDevicePropVariantCollection_Release(variants);
        return hr;
    }
    if (IsEqualGUID(&clsid, &CLSID_PortableDeviceValuesCollection))
    {
        hr = portable_device_values_collection_create(&IID_IPortableDeviceValuesCollection, (void **)&list);
        for (i = 0; SUCCEEDED(hr) && i < count; i++)
        {
            hr = read_values_body(reader, &values, depth + 1);
            if (SUCCEEDED(hr))
            {
                hr = IPortableDeviceValuesCollection_Add(list, values);
                IPortableDeviceValues_Release(values);
            }
        }
        if (SUCCEEDED(hr))
            *result = (IUnknown *)list;
        else if (list)
            IPortableDeviceValuesCollection_Release(list);
        return hr;
    }
    return E_UNEXPECTED;
}

static HRESULT read_value(struct reader *reader, PROPVARIANT *value, int depth)
{
    DWORD vt, length;
    HRESULT hr;

    PropVariantInit(value);
    vt = get_partial_u32(reader);
    switch (vt)
    {
        case VT_EMPTY:
            return E_INVALIDARG;
        case VT_NULL:
            break;
        case VT_I1:
        case VT_UI1:
            if (!get_bytes(reader, &value->bVal, 1))
                return E_UNEXPECTED;
            break;
        case VT_I2:
        case VT_UI2:
        case VT_BOOL:
            if (!get_bytes(reader, &value->uiVal, 2))
                return E_UNEXPECTED;
            break;
        case VT_I4:
        case VT_UI4:
        case VT_INT:
        case VT_UINT:
        case VT_ERROR:
        case VT_R4:
            if (!get_bytes(reader, &value->ulVal, 4))
                return E_UNEXPECTED;
            break;
        case VT_I8:
        case VT_UI8:
        case VT_R8:
        case VT_DATE:
        case VT_FILETIME:
            if (!get_bytes(reader, &value->uhVal, 8))
                return E_UNEXPECTED;
            break;
        case VT_CLSID:
            value->puuid = CoTaskMemAlloc(sizeof(GUID));
            if (!value->puuid)
                return E_OUTOFMEMORY;
            if (!get_bytes(reader, value->puuid, sizeof(GUID)))
            {
                CoTaskMemFree(value->puuid);
                value->puuid = NULL;
                return E_UNEXPECTED;
            }
            break;
        case VT_LPWSTR:
            if (!get_u32(reader, &length) || length < sizeof(WCHAR) || (length & 1) ||
                reader->size - reader->offset < length)
            {
                return E_UNEXPECTED;
            }
            value->pwszVal = CoTaskMemAlloc(length);
            if (!value->pwszVal)
                return E_OUTOFMEMORY;
            get_bytes(reader, value->pwszVal, length);
            value->pwszVal[length / sizeof(WCHAR) - 1] = 0;
            break;
        case VT_VECTOR | VT_UI1:
            if (!get_u32(reader, &length) || reader->size - reader->offset < length)
                return E_UNEXPECTED;
            value->caub.pElems = CoTaskMemAlloc(length ? length : 1);
            if (!value->caub.pElems)
                return E_OUTOFMEMORY;
            get_bytes(reader, value->caub.pElems, length);
            value->caub.cElems = length;
            break;
        case VT_UNKNOWN:
            hr = read_collection(reader, &value->punkVal, depth);
            if (FAILED(hr))
                return hr;
            break;
        default:
            return E_UNEXPECTED;
    }
    value->vt = (VARTYPE)vt;
    return S_OK;
}

static HRESULT read_values_body(struct reader *reader, IPortableDeviceValues **result, int depth)
{
    IPortableDeviceValues *values;
    PROPVARIANT value;
    PROPERTYKEY key;
    GUID clsid;
    DWORD count, i;
    HRESULT hr;

    *result = NULL;
    if (depth > SERIALIZER_MAX_DEPTH || !get_bytes(reader, &clsid, sizeof(clsid)) ||
        !IsEqualGUID(&clsid, &CLSID_PortableDeviceValues))
    {
        return E_UNEXPECTED;
    }
    count = get_partial_u32(reader);
    hr = portable_device_values_create(&IID_IPortableDeviceValues, (void **)&values);
    for (i = 0; SUCCEEDED(hr) && i < count; i++)
    {
        if (!get_bytes(reader, &key, sizeof(key)))
        {
            hr = E_UNEXPECTED;
            break;
        }
        hr = read_value(reader, &value, depth);
        if (SUCCEEDED(hr))
            hr = IPortableDeviceValues_SetValue(values, &key, &value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr))
        *result = values;
    else if (values)
        IPortableDeviceValues_Release(values);
    return hr;
}

static HRESULT WINAPI serializer_QueryInterface(IWpdSerializer *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IWpdSerializer))
        return E_NOINTERFACE;
    *out = iface;
    IWpdSerializer_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI serializer_AddRef(IWpdSerializer *iface)
{
    return InterlockedIncrement(&impl_from_IWpdSerializer(iface)->refcount);
}

static ULONG WINAPI serializer_Release(IWpdSerializer *iface)
{
    struct wpd_serializer *serializer = impl_from_IWpdSerializer(iface);
    ULONG refcount = InterlockedDecrement(&serializer->refcount);

    if (!refcount)
        HeapFree(GetProcessHeap(), 0, serializer);
    return refcount;
}

static HRESULT WINAPI serializer_GetIPortableDeviceValuesFromBuffer(IWpdSerializer *iface, BYTE *buffer,
                                                                    DWORD size, IPortableDeviceValues **params)
{
    struct reader reader = { buffer, size, 0 };
    DWORD vt;

    if (!params)
        return E_POINTER;
    *params = NULL;
    if (!buffer || !size)
        return E_INVALIDARG;
    vt = get_partial_u32(&reader);
    if (vt != VT_UNKNOWN)
        return E_UNEXPECTED;
    return read_values_body(&reader, params, 0);
}

static HRESULT WINAPI serializer_WriteIPortableDeviceValuesToBuffer(IWpdSerializer *iface, DWORD size,
                                                                    IPortableDeviceValues *results, BYTE *buffer,
                                                                    DWORD *written)
{
    DWORD needed;
    HRESULT hr;

    if (!results || !buffer || !written)
        return E_POINTER;
    hr = serialize(results, NULL, 0, &needed);
    if (FAILED(hr))
        return hr;
    hr = serialize(results, buffer, size, &needed);
    if (SUCCEEDED(hr))
        *written = needed;
    return hr;
}

static HRESULT WINAPI serializer_GetBufferFromIPortableDeviceValues(IWpdSerializer *iface,
                                                                    IPortableDeviceValues *source, BYTE **buffer,
                                                                    DWORD *size)
{
    DWORD needed;
    BYTE *data;
    HRESULT hr;

    if (!source || !buffer || !size)
        return E_POINTER;
    *buffer = NULL;
    *size = 0;
    hr = serialize(source, NULL, 0, &needed);
    if (FAILED(hr))
        return hr;
    data = CoTaskMemAlloc(needed);
    if (!data)
        return E_OUTOFMEMORY;
    hr = serialize(source, data, needed, &needed);
    if (FAILED(hr))
    {
        CoTaskMemFree(data);
        return hr;
    }
    *buffer = data;
    *size = needed;
    return S_OK;
}

static HRESULT WINAPI serializer_GetSerializedSize(IWpdSerializer *iface, IPortableDeviceValues *source, DWORD *size)
{
    if (!source || !size)
        return E_POINTER;
    return serialize(source, NULL, 0, size);
}

static const IWpdSerializerVtbl serializer_vtbl =
{
    serializer_QueryInterface,
    serializer_AddRef,
    serializer_Release,
    serializer_GetIPortableDeviceValuesFromBuffer,
    serializer_WriteIPortableDeviceValuesToBuffer,
    serializer_GetBufferFromIPortableDeviceValues,
    serializer_GetSerializedSize,
};

HRESULT wpd_serializer_create(REFIID iid, void **out)
{
    struct wpd_serializer *serializer;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    serializer = HeapAlloc(GetProcessHeap(), 0, sizeof(*serializer));
    if (!serializer)
        return E_OUTOFMEMORY;
    serializer->IWpdSerializer_iface.lpVtbl = &serializer_vtbl;
    serializer->refcount = 1;
    hr = IWpdSerializer_QueryInterface(&serializer->IWpdSerializer_iface, iid, out);
    IWpdSerializer_Release(&serializer->IWpdSerializer_iface);
    return hr;
}
