/*
 * IPortableDevice implementation
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpd);

#define WPD_OUTPUT_SLACK (1024 * 1024)
#define WPD_OUTPUT_MAX   (64 * 1024 * 1024)

struct event_sink
{
    struct event_sink *next;
    struct portable_device *device;
    IPortableDeviceEventCallback *callback;
    HCMNOTIFICATION notification;
    WCHAR cookie[40];
};

WCHAR *strdupW_co(const WCHAR *string)
{
    SIZE_T size = (lstrlenW(string) + 1) * sizeof(WCHAR);
    WCHAR *copy = CoTaskMemAlloc(size);

    if (copy)
        memcpy(copy, string, size);
    return copy;
}

HRESULT create_values(IPortableDeviceValues **values)
{
    return CoCreateInstance(&CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER,
                            &IID_IPortableDeviceValues, (void **)values);
}

HRESULT create_key_collection(IPortableDeviceKeyCollection **keys)
{
    return CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                            &IID_IPortableDeviceKeyCollection, (void **)keys);
}

HRESULT create_propvariant_collection(IPortableDevicePropVariantCollection **collection)
{
    return CoCreateInstance(&CLSID_PortableDevicePropVariantCollection, NULL, CLSCTX_INPROC_SERVER,
                            &IID_IPortableDevicePropVariantCollection, (void **)collection);
}

HRESULT results_get_unknown(IPortableDeviceValues *results, REFPROPERTYKEY key, REFIID iid, void **out)
{
    IUnknown *unknown;
    HRESULT hr;

    *out = NULL;
    hr = IPortableDeviceValues_GetIUnknownValue(results, key, &unknown);
    if (FAILED(hr))
        return hr;
    hr = IUnknown_QueryInterface(unknown, iid, out);
    IUnknown_Release(unknown);
    return hr;
}

HRESULT device_check_open(struct portable_device *device)
{
    return device->handle != INVALID_HANDLE_VALUE ? S_OK : E_WPD_DEVICE_NOT_OPEN;
}

static HRESULT device_ioctl(struct portable_device *device, DWORD code, BYTE *input, DWORD input_size,
                            IPortableDeviceValues **results)
{
    IWpdSerializer *serializer;
    OVERLAPPED overlapped;
    DWORD output_size = input_size + WPD_OUTPUT_SLACK, returned = 0, error;
    BYTE *output;
    HRESULT hr;

    *results = NULL;
    hr = CoCreateInstance(&CLSID_WpdSerializer, NULL, CLSCTX_INPROC_SERVER, &IID_IWpdSerializer, (void **)&serializer);
    if (FAILED(hr))
        return hr;
    for (;;)
    {
        output = HeapAlloc(GetProcessHeap(), 0, output_size);
        if (!output)
        {
            hr = E_OUTOFMEMORY;
            break;
        }
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            HeapFree(GetProcessHeap(), 0, output);
            break;
        }
        error = ERROR_SUCCESS;
        if (!DeviceIoControl(device->handle, code, input, input_size, output, output_size, &returned, &overlapped))
        {
            error = GetLastError();
            if (error == ERROR_IO_PENDING)
                error = GetOverlappedResult(device->handle, &overlapped, &returned, TRUE) ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(overlapped.hEvent);
        if (error == ERROR_INSUFFICIENT_BUFFER && output_size < WPD_OUTPUT_MAX)
        {
            HeapFree(GetProcessHeap(), 0, output);
            output_size *= 4;
            continue;
        }
        if (error != ERROR_SUCCESS)
            hr = HRESULT_FROM_WIN32(error);
        else
            hr = IWpdSerializer_GetIPortableDeviceValuesFromBuffer(serializer, output, returned, results);
        HeapFree(GetProcessHeap(), 0, output);
        break;
    }
    IWpdSerializer_Release(serializer);
    return hr;
}

HRESULT device_send(struct portable_device *device, IPortableDeviceValues *params, IPortableDeviceValues **results)
{
    IWpdSerializer *serializer;
    IPortableDeviceValues *copy;
    PROPERTYKEY key;
    PROPVARIANT value;
    DWORD count = 0, i, size, code, access;
    BYTE *buffer;
    HRESULT hr;

    *results = NULL;
    hr = device_check_open(device);
    if (FAILED(hr))
        return hr;
    hr = create_values(&copy);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetCount(params, &count);
    for (i = 0; i < count && SUCCEEDED(hr); i++)
    {
        PropVariantInit(&value);
        hr = IPortableDeviceValues_GetAt(params, i, &key, &value);
        if (SUCCEEDED(hr))
            hr = IPortableDeviceValues_SetValue(copy, &key, &value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr) && device->client_context)
        hr = IPortableDeviceValues_SetStringValue(copy, &WPD_PROPERTY_COMMON_CLIENT_INFORMATION_CONTEXT, device->client_context);
    if (FAILED(hr))
    {
        IPortableDeviceValues_Release(copy);
        return hr;
    }

    code = (device->desired_access & GENERIC_WRITE) ? IOCTL_WPD_MESSAGE_READWRITE_ACCESS : IOCTL_WPD_MESSAGE_READ_ACCESS;
    if (SUCCEEDED(IPortableDeviceValues_GetUnsignedIntegerValue(params, &WPD_API_OPTION_IOCTL_ACCESS, &access)))
        code = (access & FILE_WRITE_ACCESS) ? IOCTL_WPD_MESSAGE_READWRITE_ACCESS : IOCTL_WPD_MESSAGE_READ_ACCESS;

    hr = CoCreateInstance(&CLSID_WpdSerializer, NULL, CLSCTX_INPROC_SERVER, &IID_IWpdSerializer, (void **)&serializer);
    if (SUCCEEDED(hr))
    {
        hr = IWpdSerializer_GetBufferFromIPortableDeviceValues(serializer, copy, &buffer, &size);
        IWpdSerializer_Release(serializer);
        if (SUCCEEDED(hr))
        {
            hr = device_ioctl(device, code, buffer, size, results);
            CoTaskMemFree(buffer);
        }
    }
    IPortableDeviceValues_Release(copy);
    return hr;
}

HRESULT device_command(struct portable_device *device, REFPROPERTYKEY command, IPortableDeviceValues *params,
                       IPortableDeviceValues **results)
{
    IPortableDeviceValues *values = params, *out = NULL;
    HRESULT hr, command_hr;

    if (results)
        *results = NULL;
    if (!values)
    {
        hr = create_values(&values);
        if (FAILED(hr))
            return hr;
    }
    else
    {
        IPortableDeviceValues_AddRef(values);
    }
    hr = IPortableDeviceValues_SetGuidValue(values, &WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &command->fmtid);
    if (SUCCEEDED(hr))
        hr = IPortableDeviceValues_SetUnsignedIntegerValue(values, &WPD_PROPERTY_COMMON_COMMAND_ID, command->pid);
    if (SUCCEEDED(hr))
        hr = device_send(device, values, &out);
    IPortableDeviceValues_Release(values);
    if (FAILED(hr))
        return hr;
    if (FAILED(IPortableDeviceValues_GetErrorValue(out, &WPD_PROPERTY_COMMON_HRESULT, &command_hr)))
        command_hr = E_UNEXPECTED;
    if (results && SUCCEEDED(command_hr))
        *results = out;
    else
        IPortableDeviceValues_Release(out);
    return command_hr;
}

static HRESULT WINAPI device_QueryInterface(IPortableDevice *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPortableDevice))
        return E_NOINTERFACE;
    *out = iface;
    IPortableDevice_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI device_AddRef(IPortableDevice *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDevice(iface)->refcount);
}

static HRESULT WINAPI device_Close(IPortableDevice *iface);

static ULONG WINAPI device_Release(IPortableDevice *iface)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    ULONG refcount = InterlockedDecrement(&device->refcount);

    if (!refcount)
    {
        device_Close(iface);
        DeleteCriticalSection(&device->cs);
        HeapFree(GetProcessHeap(), 0, device);
    }
    return refcount;
}

static HRESULT WINAPI device_Open(IPortableDevice *iface, LPCWSTR pnp_id, IPortableDeviceValues *client_info)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    IPortableDeviceValues *params, *results;
    IPortableDeviceKeyCollection *keys;
    DWORD access = GENERIC_READ | GENERIC_WRITE, share = FILE_SHARE_READ | FILE_SHARE_WRITE, qos = 0;
    LPWSTR context = NULL;
    BOOL option;
    HANDLE handle;
    HRESULT hr;

    TRACE("%p %s %p\n", iface, debugstr_w(pnp_id), client_info);
    if (!pnp_id || !client_info)
        return E_POINTER;
    EnterCriticalSection(&device->cs);
    if (device->handle != INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&device->cs);
        return E_WPD_DEVICE_ALREADY_OPENED;
    }
    IPortableDeviceValues_GetUnsignedIntegerValue(client_info, &WPD_CLIENT_DESIRED_ACCESS, &access);
    IPortableDeviceValues_GetUnsignedIntegerValue(client_info, &WPD_CLIENT_SHARE_MODE, &share);
    IPortableDeviceValues_GetUnsignedIntegerValue(client_info, &WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, &qos);
    handle = CreateFileW(pnp_id, access, share, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | qos, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        LeaveCriticalSection(&device->cs);
        WARN("cannot open %s: 0x%08lx\n", debugstr_w(pnp_id), hr);
        return hr;
    }
    device->handle = handle;
    device->desired_access = access;
    device->pnp_id = strdupW_co(pnp_id);

    hr = create_values(&params);
    if (SUCCEEDED(hr))
    {
        hr = IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_COMMON_CLIENT_INFORMATION, client_info);
        if (SUCCEEDED(hr))
            hr = device_command(device, &WPD_COMMAND_COMMON_SAVE_CLIENT_INFORMATION, params, &results);
        IPortableDeviceValues_Release(params);
        if (SUCCEEDED(hr))
        {
            if (SUCCEEDED(IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_COMMON_CLIENT_INFORMATION_CONTEXT, &context)))
            {
                device->client_context = strdupW_co(context);
                CoTaskMemFree(context);
            }
            IPortableDeviceValues_Release(results);
        }
    }
    if (FAILED(hr))
    {
        LeaveCriticalSection(&device->cs);
        device_Close(iface);
        return hr;
    }

    if (SUCCEEDED(create_values(&params)))
    {
        if (SUCCEEDED(create_key_collection(&keys)))
        {
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_MANUFACTURER);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_MODEL);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_PROTOCOL);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_FIRMWARE_VERSION);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_TYPE);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_TRANSPORT);
            IPortableDeviceKeyCollection_Add(keys, &WPD_DEVICE_SERIAL_NUMBER);
            IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, WPD_DEVICE_OBJECT_ID);
            IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, keys);
            IPortableDeviceKeyCollection_Release(keys);
            if (SUCCEEDED(device_command(device, &WPD_COMMAND_OBJECT_PROPERTIES_GET, params, &results)))
                IPortableDeviceValues_Release(results);
        }
        IPortableDeviceValues_Release(params);
    }
    if (SUCCEEDED(device_command(device, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_COMMANDS, NULL, &results)))
        IPortableDeviceValues_Release(results);
    if (SUCCEEDED(create_values(&params)))
    {
        IPortableDeviceValues_SetKeyValue(params, &WPD_PROPERTY_CAPABILITIES_COMMAND, &WPD_COMMAND_OBJECT_RESOURCES_READ);
        if (SUCCEEDED(device_command(device, &WPD_COMMAND_CAPABILITIES_GET_COMMAND_OPTIONS, params, &results)))
        {
            IPortableDeviceValues *options;

            if (SUCCEEDED(IPortableDeviceValues_GetIPortableDeviceValuesValue(results, &WPD_PROPERTY_CAPABILITIES_COMMAND_OPTIONS, &options)))
            {
                if (SUCCEEDED(IPortableDeviceValues_GetBoolValue(options, &WPD_OPTION_OBJECT_RESOURCES_NO_INPUT_BUFFER_ON_READ, &option)))
                    device->no_input_buffer_on_read = option;
                IPortableDeviceValues_Release(options);
            }
            IPortableDeviceValues_Release(results);
        }
        IPortableDeviceValues_Release(params);
    }
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT WINAPI device_SendCommand(IPortableDevice *iface, const DWORD flags, IPortableDeviceValues *params,
                                         IPortableDeviceValues **results)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);

    TRACE("%p %#lx %p %p\n", iface, flags, params, results);
    if (!params || !results)
        return E_POINTER;
    return device_send(device, params, results);
}

static HRESULT WINAPI device_Content(IPortableDevice *iface, IPortableDeviceContent **content)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    HRESULT hr;

    if (!content)
        return E_POINTER;
    *content = NULL;
    hr = device_check_open(device);
    if (FAILED(hr))
        return hr;
    return content_create(device, content);
}

static HRESULT WINAPI device_Capabilities(IPortableDevice *iface, IPortableDeviceCapabilities **capabilities)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    HRESULT hr;

    if (!capabilities)
        return E_POINTER;
    *capabilities = NULL;
    hr = device_check_open(device);
    if (FAILED(hr))
        return hr;
    return capabilities_create(device, capabilities);
}

static HRESULT WINAPI device_Cancel(IPortableDevice *iface)
{
    return device_check_open(impl_from_IPortableDevice(iface));
}

static void sink_free(struct event_sink *sink)
{
    if (sink->notification)
        CM_Unregister_Notification(sink->notification);
    IPortableDeviceEventCallback_Release(sink->callback);
    HeapFree(GetProcessHeap(), 0, sink);
}

static HRESULT WINAPI device_Close(IPortableDevice *iface)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    struct event_sink *sink;

    EnterCriticalSection(&device->cs);
    while ((sink = device->sinks))
    {
        device->sinks = sink->next;
        LeaveCriticalSection(&device->cs);
        sink_free(sink);
        EnterCriticalSection(&device->cs);
    }
    if (device->handle != INVALID_HANDLE_VALUE)
        CloseHandle(device->handle);
    device->handle = INVALID_HANDLE_VALUE;
    CoTaskMemFree(device->pnp_id);
    device->pnp_id = NULL;
    CoTaskMemFree(device->client_context);
    device->client_context = NULL;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static DWORD CALLBACK device_event_callback(HCMNOTIFICATION notification, void *context, CM_NOTIFY_ACTION action,
                                            CM_NOTIFY_EVENT_DATA *data, DWORD size)
{
    struct event_sink *sink = context;
    IWpdSerializer *serializer;
    IPortableDeviceValues *values;
    DWORD offset;

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT ||
        !IsEqualGUID(&data->u.DeviceHandle.EventGuid, &WPD_EVENT_NOTIFICATION))
    {
        return ERROR_SUCCESS;
    }
    offset = FIELD_OFFSET(CM_NOTIFY_EVENT_DATA, u.DeviceHandle.Data);
    if (size < offset || data->u.DeviceHandle.DataSize > size - offset)
        return ERROR_SUCCESS;
    if (FAILED(CoCreateInstance(&CLSID_WpdSerializer, NULL, CLSCTX_INPROC_SERVER, &IID_IWpdSerializer, (void **)&serializer)))
        return ERROR_SUCCESS;
    if (SUCCEEDED(IWpdSerializer_GetIPortableDeviceValuesFromBuffer(serializer, data->u.DeviceHandle.Data,
                                                                    data->u.DeviceHandle.DataSize, &values)))
    {
        if (sink->device->pnp_id)
            IPortableDeviceValues_SetStringValue(values, &WPD_EVENT_PARAMETER_PNP_DEVICE_ID, sink->device->pnp_id);
        IPortableDeviceEventCallback_OnEvent(sink->callback, values);
        IPortableDeviceValues_Release(values);
    }
    IWpdSerializer_Release(serializer);
    return ERROR_SUCCESS;
}

static HRESULT WINAPI device_Advise(IPortableDevice *iface, const DWORD flags, IPortableDeviceEventCallback *callback,
                                    IPortableDeviceValues *params, LPWSTR *cookie)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    CM_NOTIFY_FILTER filter;
    struct event_sink *sink;
    CONFIGRET cr;
    GUID guid;
    HRESULT hr;

    TRACE("%p %#lx %p %p %p\n", iface, flags, callback, params, cookie);
    if (!callback || !cookie)
        return E_POINTER;
    *cookie = NULL;
    hr = device_check_open(device);
    if (FAILED(hr))
        return hr;
    sink = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sink));
    if (!sink)
        return E_OUTOFMEMORY;
    sink->device = device;
    sink->callback = callback;
    IPortableDeviceEventCallback_AddRef(callback);
    CoCreateGuid(&guid);
    StringFromGUID2(&guid, sink->cookie, ARRAY_SIZE(sink->cookie));

    memset(&filter, 0, sizeof(filter));
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = device->handle;
    cr = CM_Register_Notification(&filter, sink, device_event_callback, &sink->notification);
    if (cr != CR_SUCCESS)
    {
        WARN("CM_Register_Notification failed %lu\n", cr);
        sink->notification = NULL;
    }
    *cookie = strdupW_co(sink->cookie);
    if (!*cookie)
    {
        sink_free(sink);
        return E_OUTOFMEMORY;
    }
    EnterCriticalSection(&device->cs);
    sink->next = device->sinks;
    device->sinks = sink;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT WINAPI device_Unadvise(IPortableDevice *iface, LPCWSTR cookie)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    struct event_sink **link, *sink = NULL;

    if (!cookie)
        return E_POINTER;
    EnterCriticalSection(&device->cs);
    for (link = &device->sinks; *link; link = &(*link)->next)
    {
        if (!lstrcmpiW((*link)->cookie, cookie))
        {
            sink = *link;
            *link = sink->next;
            break;
        }
    }
    LeaveCriticalSection(&device->cs);
    if (!sink)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    sink_free(sink);
    return S_OK;
}

static HRESULT WINAPI device_GetPnPDeviceID(IPortableDevice *iface, LPWSTR *pnp_id)
{
    struct portable_device *device = impl_from_IPortableDevice(iface);
    HRESULT hr;

    if (!pnp_id)
        return E_POINTER;
    *pnp_id = NULL;
    hr = device_check_open(device);
    if (FAILED(hr))
        return hr;
    *pnp_id = strdupW_co(device->pnp_id);
    return *pnp_id ? S_OK : E_OUTOFMEMORY;
}

static const IPortableDeviceVtbl device_vtbl =
{
    device_QueryInterface,
    device_AddRef,
    device_Release,
    device_Open,
    device_SendCommand,
    device_Content,
    device_Capabilities,
    device_Cancel,
    device_Close,
    device_Advise,
    device_Unadvise,
    device_GetPnPDeviceID,
};

HRESULT portable_device_create(REFIID iid, void **out)
{
    struct portable_device *device;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    device = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;
    device->IPortableDevice_iface.lpVtbl = &device_vtbl;
    device->refcount = 1;
    device->handle = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&device->cs);
    hr = IPortableDevice_QueryInterface(&device->IPortableDevice_iface, iid, out);
    IPortableDevice_Release(&device->IPortableDevice_iface);
    return hr;
}
