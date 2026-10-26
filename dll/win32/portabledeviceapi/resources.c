/*
 * IPortableDeviceResources and resource stream implementation
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpd);

struct resources
{
    IPortableDeviceResources IPortableDeviceResources_iface;
    LONG refcount;
    struct portable_device *device;
};

struct read_stream
{
    IPortableDeviceDataStream IPortableDeviceDataStream_iface;
    LONG refcount;
    struct portable_device *device;
    WCHAR *context;
    WCHAR *object_id;
};

static inline struct resources *impl_from_IPortableDeviceResources(IPortableDeviceResources *iface)
{
    return CONTAINING_RECORD(iface, struct resources, IPortableDeviceResources_iface);
}

static inline struct read_stream *impl_from_read(IPortableDeviceDataStream *iface)
{
    return CONTAINING_RECORD(iface, struct read_stream, IPortableDeviceDataStream_iface);
}

static HRESULT stream_command(struct read_stream *stream, REFPROPERTYKEY command, IPortableDeviceValues *extra,
                              IPortableDeviceValues **results)
{
    IPortableDeviceValues *params = extra;
    HRESULT hr;

    if (!params)
    {
        hr = create_values(&params);
        if (FAILED(hr))
            return hr;
    }
    else
    {
        IPortableDeviceValues_AddRef(params);
    }
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, stream->context);
    hr = device_command(stream->device, command, params, results);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI read_QueryInterface(IPortableDeviceDataStream *iface, REFIID iid, void **out)
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

static ULONG WINAPI read_AddRef(IPortableDeviceDataStream *iface)
{
    return InterlockedIncrement(&impl_from_read(iface)->refcount);
}

static ULONG WINAPI read_Release(IPortableDeviceDataStream *iface)
{
    struct read_stream *stream = impl_from_read(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    if (!refcount)
    {
        if (stream->context)
            stream_command(stream, &WPD_COMMAND_OBJECT_RESOURCES_CLOSE, NULL, NULL);
        CoTaskMemFree(stream->context);
        CoTaskMemFree(stream->object_id);
        IPortableDevice_Release(&stream->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, stream);
    }
    return refcount;
}

static HRESULT WINAPI read_Read(IPortableDeviceDataStream *iface, void *data, ULONG size, ULONG *read)
{
    struct read_stream *stream = impl_from_read(iface);
    IPortableDeviceValues *params, *results;
    BYTE *buffer;
    DWORD length;
    ULONG done = 0, count;
    HRESULT hr = S_OK;

    if (read)
        *read = 0;
    if (!data)
        return STG_E_INVALIDPOINTER;
    while (done < size)
    {
        hr = create_values(&params);
        if (FAILED(hr))
            break;
        IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_TO_READ, size - done);
        if (!stream->device->no_input_buffer_on_read)
        {
            BYTE dummy = 0;
            IPortableDeviceValues_SetBufferValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_DATA, &dummy, 0);
        }
        hr = stream_command(stream, &WPD_COMMAND_OBJECT_RESOURCES_READ, params, &results);
        IPortableDeviceValues_Release(params);
        if (FAILED(hr))
            break;
        count = 0;
        IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_READ, &count);
        if (count && SUCCEEDED(IPortableDeviceValues_GetBufferValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_DATA, &buffer, &length)))
        {
            count = min(min(count, length), size - done);
            memcpy((BYTE *)data + done, buffer, count);
            CoTaskMemFree(buffer);
        }
        else
        {
            count = 0;
        }
        IPortableDeviceValues_Release(results);
        done += count;
        if (!count)
            break;
    }
    if (read)
        *read = done;
    return done ? S_OK : hr;
}

static HRESULT WINAPI read_Write(IPortableDeviceDataStream *iface, const void *data, ULONG size, ULONG *written)
{
    struct read_stream *stream = impl_from_read(iface);
    IPortableDeviceValues *params, *results;
    ULONG count = 0;
    HRESULT hr;

    if (written)
        *written = 0;
    if (!data)
        return STG_E_INVALIDPOINTER;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_TO_WRITE, size);
    IPortableDeviceValues_SetBufferValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_DATA, (BYTE *)data, size);
    hr = stream_command(stream, &WPD_COMMAND_OBJECT_RESOURCES_WRITE, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_WRITTEN, &count);
    IPortableDeviceValues_Release(results);
    if (written)
        *written = count;
    return hr;
}

static HRESULT WINAPI read_Seek(IPortableDeviceDataStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
    struct read_stream *stream = impl_from_read(iface);
    IPortableDeviceValues *params, *results;
    ULONGLONG offset;
    HRESULT hr;

    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetSignedLargeIntegerValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_SEEK_OFFSET, move.QuadPart);
    IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_SEEK_ORIGIN_FLAG, origin);
    hr = stream_command(stream, &WPD_COMMAND_OBJECT_RESOURCES_SEEK, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    if (position && SUCCEEDED(IPortableDeviceValues_GetUnsignedLargeIntegerValue(results,
                                                                                &WPD_PROPERTY_OBJECT_RESOURCES_POSITION_FROM_START,
                                                                                &offset)))
    {
        position->QuadPart = offset;
    }
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI read_SetSize(IPortableDeviceDataStream *iface, ULARGE_INTEGER size)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_CopyTo(IPortableDeviceDataStream *iface, IStream *target, ULARGE_INTEGER size,
                                  ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_Commit(IPortableDeviceDataStream *iface, DWORD flags)
{
    return stream_command(impl_from_read(iface), &WPD_COMMAND_OBJECT_RESOURCES_COMMIT, NULL, NULL);
}

static HRESULT WINAPI read_Revert(IPortableDeviceDataStream *iface)
{
    return stream_command(impl_from_read(iface), &WPD_COMMAND_OBJECT_RESOURCES_REVERT, NULL, NULL);
}

static HRESULT WINAPI read_LockRegion(IPortableDeviceDataStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_UnlockRegion(IPortableDeviceDataStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_Stat(IPortableDeviceDataStream *iface, STATSTG *stat, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_Clone(IPortableDeviceDataStream *iface, IStream **out)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_GetObjectID(IPortableDeviceDataStream *iface, LPWSTR *object_id)
{
    struct read_stream *stream = impl_from_read(iface);

    if (!object_id)
        return E_POINTER;
    *object_id = strdupW_co(stream->object_id);
    return *object_id ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI read_Cancel(IPortableDeviceDataStream *iface)
{
    return S_OK;
}

static const IPortableDeviceDataStreamVtbl read_vtbl =
{
    read_QueryInterface,
    read_AddRef,
    read_Release,
    read_Read,
    read_Write,
    read_Seek,
    read_SetSize,
    read_CopyTo,
    read_Commit,
    read_Revert,
    read_LockRegion,
    read_UnlockRegion,
    read_Stat,
    read_Clone,
    read_GetObjectID,
    read_Cancel,
};

HRESULT read_stream_create(struct portable_device *device, const WCHAR *object_id, const WCHAR *context, IStream **out)
{
    struct read_stream *stream;

    *out = NULL;
    stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream));
    if (!stream)
        return E_OUTOFMEMORY;
    stream->IPortableDeviceDataStream_iface.lpVtbl = &read_vtbl;
    stream->refcount = 1;
    stream->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    stream->context = strdupW_co(context);
    stream->object_id = strdupW_co(object_id);
    if (!stream->context || !stream->object_id)
    {
        IPortableDeviceDataStream_Release(&stream->IPortableDeviceDataStream_iface);
        return E_OUTOFMEMORY;
    }
    *out = (IStream *)&stream->IPortableDeviceDataStream_iface;
    return S_OK;
}

static HRESULT WINAPI resources_QueryInterface(IPortableDeviceResources *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPortableDeviceResources))
        return E_NOINTERFACE;
    *out = iface;
    IPortableDeviceResources_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI resources_AddRef(IPortableDeviceResources *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDeviceResources(iface)->refcount);
}

static ULONG WINAPI resources_Release(IPortableDeviceResources *iface)
{
    struct resources *resources = impl_from_IPortableDeviceResources(iface);
    ULONG refcount = InterlockedDecrement(&resources->refcount);

    if (!refcount)
    {
        IPortableDevice_Release(&resources->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, resources);
    }
    return refcount;
}

static HRESULT resource_params(LPCWSTR object_id, REFPROPERTYKEY key, IPortableDeviceValues **params)
{
    HRESULT hr;

    if (!object_id)
        return E_POINTER;
    hr = create_values(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(*params, &WPD_PROPERTY_OBJECT_RESOURCES_OBJECT_ID, object_id);
    if (key)
        IPortableDeviceValues_SetKeyValue(*params, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_KEYS, key);
    return S_OK;
}

static HRESULT WINAPI resources_GetSupportedResources(IPortableDeviceResources *iface, LPCWSTR object_id,
                                                      IPortableDeviceKeyCollection **keys)
{
    IPortableDeviceValues *params, *results;
    HRESULT hr;

    if (!keys)
        return E_POINTER;
    *keys = NULL;
    hr = resource_params(object_id, NULL, &params);
    if (FAILED(hr))
        return hr;
    hr = device_command(impl_from_IPortableDeviceResources(iface)->device, &WPD_COMMAND_OBJECT_RESOURCES_GET_SUPPORTED,
                        params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    hr = results_get_unknown(results, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_KEYS, &IID_IPortableDeviceKeyCollection, (void **)keys);
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI resources_GetResourceAttributes(IPortableDeviceResources *iface, LPCWSTR object_id,
                                                      REFPROPERTYKEY key, IPortableDeviceValues **attributes)
{
    IPortableDeviceValues *params, *results;
    HRESULT hr;

    if (!key || !attributes)
        return E_POINTER;
    *attributes = NULL;
    hr = resource_params(object_id, key, &params);
    if (FAILED(hr))
        return hr;
    hr = device_command(impl_from_IPortableDeviceResources(iface)->device, &WPD_COMMAND_OBJECT_RESOURCES_GET_ATTRIBUTES,
                        params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    hr = IPortableDeviceValues_GetIPortableDeviceValuesValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_ATTRIBUTES, attributes);
    IPortableDeviceValues_Release(results);
    return hr;
}

static HRESULT WINAPI resources_GetStream(IPortableDeviceResources *iface, LPCWSTR object_id, REFPROPERTYKEY key,
                                          const DWORD mode, DWORD *optimal, IStream **stream)
{
    struct resources *resources = impl_from_IPortableDeviceResources(iface);
    IPortableDeviceValues *params, *results;
    LPWSTR context;
    DWORD size = 0;
    HRESULT hr;

    TRACE("%p %s %#lx\n", iface, debugstr_w(object_id), mode);
    if (!key || !stream)
        return E_POINTER;
    *stream = NULL;
    hr = resource_params(object_id, key, &params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_ACCESS_MODE, mode);
    hr = device_command(resources->device, &WPD_COMMAND_OBJECT_RESOURCES_OPEN, params, &results);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetUnsignedIntegerValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_OPTIMAL_TRANSFER_BUFFER_SIZE, &size);
    hr = IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, &context);
    IPortableDeviceValues_Release(results);
    if (FAILED(hr))
        return hr;
    hr = read_stream_create(resources->device, object_id, context, stream);
    CoTaskMemFree(context);
    if (SUCCEEDED(hr) && optimal)
        *optimal = size;
    return hr;
}

static HRESULT WINAPI resources_Delete(IPortableDeviceResources *iface, LPCWSTR object_id, IPortableDeviceKeyCollection *keys)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!keys)
        return E_POINTER;
    hr = resource_params(object_id, NULL, &params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_KEYS, keys);
    hr = device_command(impl_from_IPortableDeviceResources(iface)->device, &WPD_COMMAND_OBJECT_RESOURCES_DELETE, params, NULL);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI resources_Cancel(IPortableDeviceResources *iface)
{
    return S_OK;
}

static HRESULT WINAPI resources_CreateResource(IPortableDeviceResources *iface, IPortableDeviceValues *attributes,
                                               IStream **data, DWORD *optimal, LPWSTR *cookie)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!attributes || !data)
        return E_POINTER;
    *data = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_ATTRIBUTES, attributes);
    hr = device_command(impl_from_IPortableDeviceResources(iface)->device, &WPD_COMMAND_OBJECT_RESOURCES_CREATE_RESOURCE,
                        params, NULL);
    IPortableDeviceValues_Release(params);
    return FAILED(hr) ? hr : E_NOTIMPL;
}

static const IPortableDeviceResourcesVtbl resources_vtbl =
{
    resources_QueryInterface,
    resources_AddRef,
    resources_Release,
    resources_GetSupportedResources,
    resources_GetResourceAttributes,
    resources_GetStream,
    resources_Delete,
    resources_Cancel,
    resources_CreateResource,
};

HRESULT resources_create(struct portable_device *device, IPortableDeviceResources **out)
{
    struct resources *resources;

    *out = NULL;
    resources = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*resources));
    if (!resources)
        return E_OUTOFMEMORY;
    resources->IPortableDeviceResources_iface.lpVtbl = &resources_vtbl;
    resources->refcount = 1;
    resources->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    *out = &resources->IPortableDeviceResources_iface;
    return S_OK;
}
