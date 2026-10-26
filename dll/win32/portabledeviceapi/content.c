/*
 * IPortableDeviceContent, IEnumPortableDeviceObjectIDs and object upload stream
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpd);

struct content
{
    IPortableDeviceContent2 IPortableDeviceContent2_iface;
    LONG refcount;
    struct portable_device *device;
};

struct object_enum
{
    IEnumPortableDeviceObjectIDs IEnumPortableDeviceObjectIDs_iface;
    LONG refcount;
    struct portable_device *device;
    WCHAR *parent;
    IPortableDeviceValues *filter;
    WCHAR *context;
};

struct upload_stream
{
    IPortableDeviceDataStream IPortableDeviceDataStream_iface;
    LONG refcount;
    struct portable_device *device;
    WCHAR *context;
    WCHAR *object_id;
    DWORD optimal;
    ULONGLONG written;
};

static inline struct content *impl_from_IPortableDeviceContent2(IPortableDeviceContent2 *iface)
{
    return CONTAINING_RECORD(iface, struct content, IPortableDeviceContent2_iface);
}

static inline struct object_enum *impl_from_IEnumPortableDeviceObjectIDs(IEnumPortableDeviceObjectIDs *iface)
{
    return CONTAINING_RECORD(iface, struct object_enum, IEnumPortableDeviceObjectIDs_iface);
}

static inline struct upload_stream *impl_from_upload(IPortableDeviceDataStream *iface)
{
    return CONTAINING_RECORD(iface, struct upload_stream, IPortableDeviceDataStream_iface);
}

static HRESULT enum_start(struct object_enum *object_enum)
{
    IPortableDeviceValues *params, *results;
    LPWSTR context;
    HRESULT hr;

    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_ENUMERATION_PARENT_ID, object_enum->parent);
    if (object_enum->filter)
        IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_ENUMERATION_FILTER, object_enum->filter);
    hr = device_command(object_enum->device, &WPD_COMMAND_OBJECT_ENUMERATION_START_FIND, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    hr = IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT, &context);
    IPortableDeviceValues_Release(results);
    if (SUCCEEDED(hr))
        object_enum->context = context;
    return hr;
}

static void enum_end(struct object_enum *object_enum)
{
    IPortableDeviceValues *params;

    if (!object_enum->context)
        return;
    if (SUCCEEDED(create_values(&params)))
    {
        IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT, object_enum->context);
        device_command(object_enum->device, &WPD_COMMAND_OBJECT_ENUMERATION_END_FIND, params, NULL);
        IPortableDeviceValues_Release(params);
    }
    CoTaskMemFree(object_enum->context);
    object_enum->context = NULL;
}

static HRESULT enum_next(struct object_enum *object_enum, ULONG requested, LPWSTR *ids, ULONG *fetched)
{
    IPortableDeviceValues *params, *results;
    IPortableDevicePropVariantCollection *collection;
    PROPVARIANT value;
    DWORD count = 0, i;
    HRESULT hr, command_hr;

    *fetched = 0;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT, object_enum->context);
    IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_ENUMERATION_NUM_OBJECTS_REQUESTED, requested);
    command_hr = device_command(object_enum->device, &WPD_COMMAND_OBJECT_ENUMERATION_FIND_NEXT, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(command_hr))
        return command_hr;
    hr = results_get_unknown(results, &WPD_PROPERTY_OBJECT_ENUMERATION_OBJECT_IDS,
                             &IID_IPortableDevicePropVariantCollection, (void **)&collection);
    IPortableDeviceValues_Release(results);
    if (FAILED(hr))
        return hr;
    IPortableDevicePropVariantCollection_GetCount(collection, &count);
    for (i = 0; i < count && *fetched < requested; i++)
    {
        PropVariantInit(&value);
        if (SUCCEEDED(IPortableDevicePropVariantCollection_GetAt(collection, i, &value)) && value.vt == VT_LPWSTR)
        {
            if (ids)
            {
                ids[*fetched] = value.pwszVal;
                value.vt = VT_EMPTY;
            }
            (*fetched)++;
        }
        PropVariantClear(&value);
    }
    IPortableDevicePropVariantCollection_Release(collection);
    return *fetched == requested ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_QueryInterface(IEnumPortableDeviceObjectIDs *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IEnumPortableDeviceObjectIDs))
        return E_NOINTERFACE;
    *out = iface;
    IEnumPortableDeviceObjectIDs_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI enum_AddRef(IEnumPortableDeviceObjectIDs *iface)
{
    return InterlockedIncrement(&impl_from_IEnumPortableDeviceObjectIDs(iface)->refcount);
}

static ULONG WINAPI enum_Release(IEnumPortableDeviceObjectIDs *iface)
{
    struct object_enum *object_enum = impl_from_IEnumPortableDeviceObjectIDs(iface);
    ULONG refcount = InterlockedDecrement(&object_enum->refcount);

    if (!refcount)
    {
        enum_end(object_enum);
        if (object_enum->filter)
            IPortableDeviceValues_Release(object_enum->filter);
        CoTaskMemFree(object_enum->parent);
        IPortableDevice_Release(&object_enum->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, object_enum);
    }
    return refcount;
}

static HRESULT WINAPI enum_Next(IEnumPortableDeviceObjectIDs *iface, ULONG count, LPWSTR *ids, ULONG *fetched)
{
    struct object_enum *object_enum = impl_from_IEnumPortableDeviceObjectIDs(iface);
    ULONG got = 0;
    HRESULT hr;

    if (!ids || (!fetched && count > 1))
        return E_POINTER;
    if (fetched)
        *fetched = 0;
    if (!count)
        return S_OK;
    hr = enum_next(object_enum, count, ids, &got);
    if (fetched)
        *fetched = got;
    return hr;
}

static HRESULT WINAPI enum_Skip(IEnumPortableDeviceObjectIDs *iface, ULONG count)
{
    struct object_enum *object_enum = impl_from_IEnumPortableDeviceObjectIDs(iface);
    LPWSTR *ids;
    ULONG got = 0, i;
    HRESULT hr;

    if (!count)
        return S_OK;
    ids = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*ids));
    if (!ids)
        return E_OUTOFMEMORY;
    hr = enum_next(object_enum, count, ids, &got);
    for (i = 0; i < got; i++)
        CoTaskMemFree(ids[i]);
    HeapFree(GetProcessHeap(), 0, ids);
    return hr;
}

static HRESULT WINAPI enum_Reset(IEnumPortableDeviceObjectIDs *iface)
{
    struct object_enum *object_enum = impl_from_IEnumPortableDeviceObjectIDs(iface);

    enum_end(object_enum);
    return enum_start(object_enum);
}

static HRESULT WINAPI enum_Clone(IEnumPortableDeviceObjectIDs *iface, IEnumPortableDeviceObjectIDs **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI enum_Cancel(IEnumPortableDeviceObjectIDs *iface)
{
    return S_OK;
}

static const IEnumPortableDeviceObjectIDsVtbl enum_vtbl =
{
    enum_QueryInterface,
    enum_AddRef,
    enum_Release,
    enum_Next,
    enum_Skip,
    enum_Reset,
    enum_Clone,
    enum_Cancel,
};

HRESULT enum_create(struct portable_device *device, const WCHAR *parent, IPortableDeviceValues *filter,
                    IEnumPortableDeviceObjectIDs **out)
{
    struct object_enum *object_enum;
    HRESULT hr;

    *out = NULL;
    object_enum = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object_enum));
    if (!object_enum)
        return E_OUTOFMEMORY;
    object_enum->IEnumPortableDeviceObjectIDs_iface.lpVtbl = &enum_vtbl;
    object_enum->refcount = 1;
    object_enum->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    object_enum->parent = strdupW_co(parent);
    object_enum->filter = filter;
    if (filter)
        IPortableDeviceValues_AddRef(filter);
    hr = object_enum->parent ? enum_start(object_enum) : E_OUTOFMEMORY;
    if (FAILED(hr))
    {
        IEnumPortableDeviceObjectIDs_Release(&object_enum->IEnumPortableDeviceObjectIDs_iface);
        return hr;
    }
    *out = &object_enum->IEnumPortableDeviceObjectIDs_iface;
    return S_OK;
}

static HRESULT WINAPI upload_QueryInterface(IPortableDeviceDataStream *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IStream) &&
        !IsEqualIID(iid, &IID_ISequentialStream) && !IsEqualIID(iid, &IID_IPortableDeviceDataStream))
    {
        return E_NOINTERFACE;
    }
    *out = iface;
    IPortableDeviceDataStream_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI upload_AddRef(IPortableDeviceDataStream *iface)
{
    return InterlockedIncrement(&impl_from_upload(iface)->refcount);
}

static HRESULT upload_simple(struct upload_stream *stream, REFPROPERTYKEY command, IPortableDeviceValues **results)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, stream->context);
    hr = device_command(stream->device, command, params, results);
    IPortableDeviceValues_Release(params);
    return hr;
}

static ULONG WINAPI upload_Release(IPortableDeviceDataStream *iface)
{
    struct upload_stream *stream = impl_from_upload(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    if (!refcount)
    {
        if (stream->context && !stream->object_id)
            upload_simple(stream, &WPD_COMMAND_OBJECT_MANAGEMENT_REVERT_OBJECT, NULL);
        CoTaskMemFree(stream->context);
        CoTaskMemFree(stream->object_id);
        IPortableDevice_Release(&stream->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, stream);
    }
    return refcount;
}

static HRESULT WINAPI upload_Read(IPortableDeviceDataStream *iface, void *data, ULONG size, ULONG *read)
{
    if (read)
        *read = 0;
    return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI upload_Write(IPortableDeviceDataStream *iface, const void *data, ULONG size, ULONG *written)
{
    struct upload_stream *stream = impl_from_upload(iface);
    IPortableDeviceValues *params, *results;
    ULONG done = 0, chunk, count;
    HRESULT hr = S_OK;

    if (written)
        *written = 0;
    if (!data)
        return STG_E_INVALIDPOINTER;
    if (stream->object_id)
        return E_ACCESSDENIED;
    while (done < size && SUCCEEDED(hr))
    {
        chunk = min(size - done, stream->optimal ? stream->optimal : size - done);
        hr = create_values(&params);
        if (FAILED(hr))
            break;
        IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, stream->context);
        IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_NUM_BYTES_TO_WRITE, chunk);
        IPortableDeviceValues_SetBufferValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_DATA, (BYTE *)data + done, chunk);
        hr = device_command(stream->device, &WPD_COMMAND_OBJECT_MANAGEMENT_WRITE_OBJECT_DATA, params, &results);
        IPortableDeviceValues_Release(params);
        if (FAILED(hr))
            break;
        count = 0;
        IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_NUM_BYTES_WRITTEN, &count);
        IPortableDeviceValues_Release(results);
        done += count;
        stream->written += count;
        if (count != chunk)
            break;
    }
    if (written)
        *written = done;
    return hr;
}

static HRESULT WINAPI upload_Seek(IPortableDeviceDataStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_SetSize(IPortableDeviceDataStream *iface, ULARGE_INTEGER size)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_CopyTo(IPortableDeviceDataStream *iface, IStream *target, ULARGE_INTEGER size,
                                    ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_Commit(IPortableDeviceDataStream *iface, DWORD flags)
{
    struct upload_stream *stream = impl_from_upload(iface);
    IPortableDeviceValues *results;
    LPWSTR object_id;
    HRESULT hr;

    if (stream->object_id)
        return S_OK;
    hr = upload_simple(stream, &WPD_COMMAND_OBJECT_MANAGEMENT_COMMIT_OBJECT, &results);
    if (FAILED(hr))
        return hr;
    if (SUCCEEDED(IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_ID, &object_id)))
        stream->object_id = object_id;
    IPortableDeviceValues_Release(results);
    return S_OK;
}

static HRESULT WINAPI upload_Revert(IPortableDeviceDataStream *iface)
{
    struct upload_stream *stream = impl_from_upload(iface);
    HRESULT hr;

    if (stream->object_id)
        return E_ACCESSDENIED;
    hr = upload_simple(stream, &WPD_COMMAND_OBJECT_MANAGEMENT_REVERT_OBJECT, NULL);
    CoTaskMemFree(stream->context);
    stream->context = NULL;
    return hr;
}

static HRESULT WINAPI upload_LockRegion(IPortableDeviceDataStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_UnlockRegion(IPortableDeviceDataStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_Stat(IPortableDeviceDataStream *iface, STATSTG *stat, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_Clone(IPortableDeviceDataStream *iface, IStream **out)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI upload_GetObjectID(IPortableDeviceDataStream *iface, LPWSTR *object_id)
{
    struct upload_stream *stream = impl_from_upload(iface);

    if (!object_id)
        return E_POINTER;
    *object_id = NULL;
    if (!stream->object_id)
        return E_WPD_OBJECT_NOT_COMMITED;
    *object_id = strdupW_co(stream->object_id);
    return *object_id ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI upload_Cancel(IPortableDeviceDataStream *iface)
{
    return S_OK;
}

static const IPortableDeviceDataStreamVtbl upload_vtbl =
{
    upload_QueryInterface,
    upload_AddRef,
    upload_Release,
    upload_Read,
    upload_Write,
    upload_Seek,
    upload_SetSize,
    upload_CopyTo,
    upload_Commit,
    upload_Revert,
    upload_LockRegion,
    upload_UnlockRegion,
    upload_Stat,
    upload_Clone,
    upload_GetObjectID,
    upload_Cancel,
};

HRESULT upload_stream_create(struct portable_device *device, const WCHAR *context, DWORD optimal, IStream **out)
{
    struct upload_stream *stream;

    *out = NULL;
    stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream));
    if (!stream)
        return E_OUTOFMEMORY;
    stream->IPortableDeviceDataStream_iface.lpVtbl = &upload_vtbl;
    stream->refcount = 1;
    stream->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    stream->context = strdupW_co(context);
    stream->optimal = optimal;
    if (!stream->context)
    {
        IPortableDeviceDataStream_Release(&stream->IPortableDeviceDataStream_iface);
        return E_OUTOFMEMORY;
    }
    *out = (IStream *)&stream->IPortableDeviceDataStream_iface;
    return S_OK;
}

static HRESULT WINAPI content_QueryInterface(IPortableDeviceContent2 *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPortableDeviceContent) &&
        !IsEqualIID(iid, &IID_IPortableDeviceContent2))
    {
        return E_NOINTERFACE;
    }
    *out = iface;
    IPortableDeviceContent2_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI content_AddRef(IPortableDeviceContent2 *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDeviceContent2(iface)->refcount);
}

static ULONG WINAPI content_Release(IPortableDeviceContent2 *iface)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);
    ULONG refcount = InterlockedDecrement(&content->refcount);

    if (!refcount)
    {
        IPortableDevice_Release(&content->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, content);
    }
    return refcount;
}

static HRESULT WINAPI content_EnumObjects(IPortableDeviceContent2 *iface, const DWORD flags, LPCWSTR parent,
                                          IPortableDeviceValues *filter, IEnumPortableDeviceObjectIDs **out)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);

    TRACE("%p %#lx %s %p %p\n", iface, flags, debugstr_w(parent), filter, out);
    if (!parent || !out)
        return E_POINTER;
    return enum_create(content->device, parent, filter, out);
}

static HRESULT WINAPI content_Properties(IPortableDeviceContent2 *iface, IPortableDeviceProperties **properties)
{
    if (!properties)
        return E_POINTER;
    return properties_create(impl_from_IPortableDeviceContent2(iface)->device, properties);
}

static HRESULT WINAPI content_Transfer(IPortableDeviceContent2 *iface, IPortableDeviceResources **resources)
{
    if (!resources)
        return E_POINTER;
    return resources_create(impl_from_IPortableDeviceContent2(iface)->device, resources);
}

static HRESULT WINAPI content_CreateObjectWithPropertiesOnly(IPortableDeviceContent2 *iface, IPortableDeviceValues *values,
                                                             LPWSTR *object_id)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);
    IPortableDeviceValues *params, *results;
    LPWSTR id;
    HRESULT hr;

    if (!values)
        return E_POINTER;
    if (object_id)
        *object_id = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CREATION_PROPERTIES, values);
    hr = device_command(content->device, &WPD_COMMAND_OBJECT_MANAGEMENT_CREATE_OBJECT_WITH_PROPERTIES_ONLY, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    if (object_id && SUCCEEDED(IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_ID, &id)))
        *object_id = id;
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI content_CreateObjectWithPropertiesAndData(IPortableDeviceContent2 *iface, IPortableDeviceValues *values,
                                                                IStream **data, DWORD *optimal, LPWSTR *cookie)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);
    IPortableDeviceValues *params, *results;
    LPWSTR context;
    DWORD size = 0;
    HRESULT hr;

    if (!values || !data)
        return E_POINTER;
    *data = NULL;
    if (cookie)
        *cookie = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CREATION_PROPERTIES, values);
    hr = device_command(content->device, &WPD_COMMAND_OBJECT_MANAGEMENT_CREATE_OBJECT_WITH_PROPERTIES_AND_DATA, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OPTIMAL_TRANSFER_BUFFER_SIZE, &size);
    hr = IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, &context);
    IPortableDeviceValues_Release(results);
    if (FAILED(hr))
        return hr;
    hr = upload_stream_create(content->device, context, size, data);
    if (SUCCEEDED(hr))
    {
        if (optimal)
            *optimal = size;
        if (cookie)
            *cookie = strdupW_co(context);
    }
    CoTaskMemFree(context);
    return hr;
}

static HRESULT collection_command(struct portable_device *device, REFPROPERTYKEY command, DWORD options,
                                  IPortableDevicePropVariantCollection *ids, const WCHAR *destination,
                                  REFPROPERTYKEY result_key, IPortableDevicePropVariantCollection **out)
{
    IPortableDeviceValues *params, *results;
    HRESULT hr;

    if (out)
        *out = NULL;
    if (!ids)
        return E_POINTER;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    if (IsEqualPropertyKey(*command, WPD_COMMAND_OBJECT_MANAGEMENT_DELETE_OBJECTS))
        IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_DELETE_OPTIONS, options);
    IPortableDeviceValues_SetIPortableDevicePropVariantCollectionValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_IDS, ids);
    if (destination)
        IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_DESTINATION_FOLDER_OBJECT_ID, destination);
    hr = device_command(device, command, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    if (out)
        results_get_unknown(results, result_key, &IID_IPortableDevicePropVariantCollection, (void **)out);
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI content_Delete(IPortableDeviceContent2 *iface, const DWORD options,
                                     IPortableDevicePropVariantCollection *ids, IPortableDevicePropVariantCollection **results)
{
    return collection_command(impl_from_IPortableDeviceContent2(iface)->device, &WPD_COMMAND_OBJECT_MANAGEMENT_DELETE_OBJECTS,
                              options, ids, NULL, &WPD_PROPERTY_OBJECT_MANAGEMENT_DELETE_RESULTS, results);
}

static HRESULT WINAPI content_GetObjectIDsFromPersistentUniqueIDs(IPortableDeviceContent2 *iface,
                                                                  IPortableDevicePropVariantCollection *puids,
                                                                  IPortableDevicePropVariantCollection **ids)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);
    IPortableDeviceValues *params, *results;
    HRESULT hr;

    if (!puids || !ids)
        return E_POINTER;
    *ids = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDevicePropVariantCollectionValue(params, &WPD_PROPERTY_COMMON_PERSISTENT_UNIQUE_IDS, puids);
    hr = device_command(content->device, &WPD_COMMAND_COMMON_GET_OBJECT_IDS_FROM_PERSISTENT_UNIQUE_IDS, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    hr = results_get_unknown(results, &WPD_PROPERTY_COMMON_OBJECT_IDS, &IID_IPortableDevicePropVariantCollection, (void **)ids);
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI content_Cancel(IPortableDeviceContent2 *iface)
{
    return S_OK;
}

static HRESULT WINAPI content_Move(IPortableDeviceContent2 *iface, IPortableDevicePropVariantCollection *ids,
                                   LPCWSTR destination, IPortableDevicePropVariantCollection **results)
{
    if (!destination)
        return E_POINTER;
    return collection_command(impl_from_IPortableDeviceContent2(iface)->device, &WPD_COMMAND_OBJECT_MANAGEMENT_MOVE_OBJECTS,
                              0, ids, destination, &WPD_PROPERTY_OBJECT_MANAGEMENT_MOVE_RESULTS, results);
}

static HRESULT WINAPI content_Copy(IPortableDeviceContent2 *iface, IPortableDevicePropVariantCollection *ids,
                                   LPCWSTR destination, IPortableDevicePropVariantCollection **results)
{
    if (!destination)
        return E_POINTER;
    return collection_command(impl_from_IPortableDeviceContent2(iface)->device, &WPD_COMMAND_OBJECT_MANAGEMENT_COPY_OBJECTS,
                              0, ids, destination, &WPD_PROPERTY_OBJECT_MANAGEMENT_COPY_RESULTS, results);
}

static HRESULT WINAPI content_UpdateObjectWithPropertiesAndData(IPortableDeviceContent2 *iface, LPCWSTR object_id,
                                                                IPortableDeviceValues *properties, IStream **data,
                                                                DWORD *optimal)
{
    struct content *content = impl_from_IPortableDeviceContent2(iface);
    IPortableDeviceValues *params, *results;
    LPWSTR context;
    DWORD size = 0;
    HRESULT hr;

    if (!object_id || !properties || !data)
        return E_POINTER;
    *data = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_ID, object_id);
    IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_MANAGEMENT_UPDATE_PROPERTIES, properties);
    hr = device_command(content->device, &WPD_COMMAND_OBJECT_MANAGEMENT_UPDATE_OBJECT_WITH_PROPERTIES_AND_DATA, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OPTIMAL_TRANSFER_BUFFER_SIZE, &size);
    hr = IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, &context);
    IPortableDeviceValues_Release(results);
    if (FAILED(hr))
        return hr;
    hr = upload_stream_create(content->device, context, size, data);
    if (SUCCEEDED(hr) && optimal)
        *optimal = size;
    CoTaskMemFree(context);
    return hr;
}

static const IPortableDeviceContent2Vtbl content_vtbl =
{
    content_QueryInterface,
    content_AddRef,
    content_Release,
    content_EnumObjects,
    content_Properties,
    content_Transfer,
    content_CreateObjectWithPropertiesOnly,
    content_CreateObjectWithPropertiesAndData,
    content_Delete,
    content_GetObjectIDsFromPersistentUniqueIDs,
    content_Cancel,
    content_Move,
    content_Copy,
    content_UpdateObjectWithPropertiesAndData,
};

HRESULT content_create(struct portable_device *device, IPortableDeviceContent **out)
{
    struct content *content;

    *out = NULL;
    content = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*content));
    if (!content)
        return E_OUTOFMEMORY;
    content->IPortableDeviceContent2_iface.lpVtbl = &content_vtbl;
    content->refcount = 1;
    content->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    *out = (IPortableDeviceContent *)&content->IPortableDeviceContent2_iface;
    return S_OK;
}
