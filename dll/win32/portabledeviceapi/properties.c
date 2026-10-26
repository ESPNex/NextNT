/*
 * IPortableDeviceProperties and IPortableDeviceCapabilities implementation
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "private.h"

struct bulk_op
{
    struct bulk_op *next;
    struct properties *owner;
    GUID context;
    WCHAR *driver_context;
    const PROPERTYKEY *next_command;
    const PROPERTYKEY *end_command;
    IPortableDevicePropertiesBulkCallback *callback;
    BOOL started;
    BOOL cancelled;
};

struct properties
{
    IPortableDeviceProperties IPortableDeviceProperties_iface;
    IPortableDevicePropertiesBulk IPortableDevicePropertiesBulk_iface;
    LONG refcount;
    struct portable_device *device;
    CRITICAL_SECTION cs;
    struct bulk_op *ops;
};

struct capabilities
{
    IPortableDeviceCapabilities IPortableDeviceCapabilities_iface;
    LONG refcount;
    struct portable_device *device;
};

static inline struct properties *impl_from_IPortableDeviceProperties(IPortableDeviceProperties *iface)
{
    return CONTAINING_RECORD(iface, struct properties, IPortableDeviceProperties_iface);
}

static inline struct capabilities *impl_from_IPortableDeviceCapabilities(IPortableDeviceCapabilities *iface)
{
    return CONTAINING_RECORD(iface, struct capabilities, IPortableDeviceCapabilities_iface);
}

static HRESULT query(struct portable_device *device, REFPROPERTYKEY command, IPortableDeviceValues *params,
                     REFPROPERTYKEY result, REFIID iid, void **out)
{
    IPortableDeviceValues *results;
    HRESULT hr, get_hr;

    *out = NULL;
    hr = device_command(device, command, params, &results);
    if (FAILED(hr))
        return hr;
    get_hr = results_get_unknown(results, result, iid, out);
    IPortableDeviceValues_Release(results);
    return FAILED(get_hr) ? get_hr : hr;
}

static HRESULT object_params(LPCWSTR object_id, REFPROPERTYKEY id_key, IPortableDeviceValues **params)
{
    HRESULT hr;

    if (!object_id)
        return E_POINTER;
    hr = create_values(params);
    if (SUCCEEDED(hr))
        hr = IPortableDeviceValues_SetStringValue(*params, id_key, object_id);
    return hr;
}

static HRESULT WINAPI properties_QueryInterface(IPortableDeviceProperties *iface, REFIID iid, void **out)
{
    struct properties *properties = impl_from_IPortableDeviceProperties(iface);

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPortableDeviceProperties))
        *out = &properties->IPortableDeviceProperties_iface;
    else if (IsEqualIID(iid, &IID_IPortableDevicePropertiesBulk))
        *out = &properties->IPortableDevicePropertiesBulk_iface;
    else
        return E_NOINTERFACE;
    IPortableDeviceProperties_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI properties_AddRef(IPortableDeviceProperties *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDeviceProperties(iface)->refcount);
}

static ULONG WINAPI properties_Release(IPortableDeviceProperties *iface)
{
    struct properties *properties = impl_from_IPortableDeviceProperties(iface);
    ULONG refcount = InterlockedDecrement(&properties->refcount);

    if (!refcount)
    {
        while (properties->ops)
        {
            struct bulk_op *op = properties->ops;

            properties->ops = op->next;
            IPortableDevicePropertiesBulkCallback_Release(op->callback);
            CoTaskMemFree(op->driver_context);
            HeapFree(GetProcessHeap(), 0, op);
        }
        DeleteCriticalSection(&properties->cs);
        IPortableDevice_Release(&properties->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, properties);
    }
    return refcount;
}

static HRESULT WINAPI properties_GetSupportedProperties(IPortableDeviceProperties *iface, LPCWSTR object_id,
                                                        IPortableDeviceKeyCollection **keys)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!keys)
        return E_POINTER;
    *keys = NULL;
    hr = object_params(object_id, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, &params);
    if (FAILED(hr))
        return hr;
    hr = query(impl_from_IPortableDeviceProperties(iface)->device, &WPD_COMMAND_OBJECT_PROPERTIES_GET_SUPPORTED, params,
               &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, &IID_IPortableDeviceKeyCollection, (void **)keys);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI properties_GetPropertyAttributes(IPortableDeviceProperties *iface, LPCWSTR object_id,
                                                       REFPROPERTYKEY key, IPortableDeviceValues **attributes)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!key || !attributes)
        return E_POINTER;
    *attributes = NULL;
    hr = object_params(object_id, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, &params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetKeyValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, key);
    hr = query(impl_from_IPortableDeviceProperties(iface)->device, &WPD_COMMAND_OBJECT_PROPERTIES_GET_ATTRIBUTES, params,
               &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_ATTRIBUTES, &IID_IPortableDeviceValues, (void **)attributes);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI properties_GetValues(IPortableDeviceProperties *iface, LPCWSTR object_id,
                                           IPortableDeviceKeyCollection *keys, IPortableDeviceValues **values)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!values)
        return E_POINTER;
    *values = NULL;
    hr = object_params(object_id, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, &params);
    if (FAILED(hr))
        return hr;
    if (keys)
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, keys);
    hr = query(impl_from_IPortableDeviceProperties(iface)->device,
               keys ? &WPD_COMMAND_OBJECT_PROPERTIES_GET : &WPD_COMMAND_OBJECT_PROPERTIES_GET_ALL, params,
               &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_VALUES, &IID_IPortableDeviceValues, (void **)values);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI properties_SetValues(IPortableDeviceProperties *iface, LPCWSTR object_id,
                                           IPortableDeviceValues *values, IPortableDeviceValues **results)
{
    IPortableDeviceValues *params, *out;
    HRESULT hr;

    if (!values || !results)
        return E_POINTER;
    *results = NULL;
    hr = object_params(object_id, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, &params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceValuesValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_VALUES, values);
    hr = device_command(impl_from_IPortableDeviceProperties(iface)->device, &WPD_COMMAND_OBJECT_PROPERTIES_SET, params, &out);
    IPortableDeviceValues_Release(params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_GetIPortableDeviceValuesValue(out, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_WRITE_RESULTS, results);
    IPortableDeviceValues_Release(out);
    return hr;
}

static HRESULT WINAPI properties_Delete(IPortableDeviceProperties *iface, LPCWSTR object_id, IPortableDeviceKeyCollection *keys)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!keys)
        return E_POINTER;
    hr = object_params(object_id, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID, &params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, keys);
    hr = device_command(impl_from_IPortableDeviceProperties(iface)->device, &WPD_COMMAND_OBJECT_PROPERTIES_DELETE, params, NULL);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI properties_Cancel(IPortableDeviceProperties *iface)
{
    return S_OK;
}

static const IPortableDevicePropertiesVtbl properties_vtbl =
{
    properties_QueryInterface,
    properties_AddRef,
    properties_Release,
    properties_GetSupportedProperties,
    properties_GetPropertyAttributes,
    properties_GetValues,
    properties_SetValues,
    properties_Delete,
    properties_Cancel,
};

static inline struct properties *impl_from_IPortableDevicePropertiesBulk(IPortableDevicePropertiesBulk *iface)
{
    return CONTAINING_RECORD(iface, struct properties, IPortableDevicePropertiesBulk_iface);
}

static HRESULT WINAPI bulk_QueryInterface(IPortableDevicePropertiesBulk *iface, REFIID iid, void **out)
{
    return properties_QueryInterface(&impl_from_IPortableDevicePropertiesBulk(iface)->IPortableDeviceProperties_iface, iid, out);
}

static ULONG WINAPI bulk_AddRef(IPortableDevicePropertiesBulk *iface)
{
    return properties_AddRef(&impl_from_IPortableDevicePropertiesBulk(iface)->IPortableDeviceProperties_iface);
}

static ULONG WINAPI bulk_Release(IPortableDevicePropertiesBulk *iface)
{
    return properties_Release(&impl_from_IPortableDevicePropertiesBulk(iface)->IPortableDeviceProperties_iface);
}

static struct bulk_op *bulk_take(struct properties *properties, REFGUID context, BOOL start)
{
    struct bulk_op **link, *op = NULL;

    EnterCriticalSection(&properties->cs);
    for (link = &properties->ops; *link; link = &(*link)->next)
    {
        if (IsEqualGUID(&(*link)->context, context) && !(*link)->started)
        {
            op = *link;
            if (start)
                op->started = TRUE;
            else
                *link = op->next;
            break;
        }
    }
    LeaveCriticalSection(&properties->cs);
    return op;
}

static void bulk_remove(struct properties *properties, struct bulk_op *op)
{
    struct bulk_op **link;

    EnterCriticalSection(&properties->cs);
    for (link = &properties->ops; *link; link = &(*link)->next)
    {
        if (*link == op)
        {
            *link = op->next;
            break;
        }
    }
    LeaveCriticalSection(&properties->cs);
}

static void bulk_end(struct bulk_op *op)
{
    IPortableDeviceValues *params;

    if (SUCCEEDED(create_values(&params)))
    {
        IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT, op->driver_context);
        device_command(op->owner->device, op->end_command, params, NULL);
        IPortableDeviceValues_Release(params);
    }
}

static void bulk_free(struct bulk_op *op)
{
    struct properties *properties = op->owner;

    IPortableDevicePropertiesBulkCallback_Release(op->callback);
    CoTaskMemFree(op->driver_context);
    HeapFree(GetProcessHeap(), 0, op);
    IPortableDeviceProperties_Release(&properties->IPortableDeviceProperties_iface);
}

static DWORD WINAPI bulk_thread(void *arg)
{
    struct bulk_op *op = arg;
    IPortableDeviceValues *params, *results;
    IPortableDeviceValuesCollection *batch;
    DWORD count;
    HRESULT hr = S_OK, init;

    init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!op->cancelled && op->started)
    {
        hr = create_values(&params);
        if (FAILED(hr))
            break;
        IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT, op->driver_context);
        hr = device_command(op->owner->device, op->next_command, params, &results);
        IPortableDeviceValues_Release(params);
        if (FAILED(hr))
            break;
        batch = NULL;
        hr = IPortableDeviceValues_GetIPortableDeviceValuesCollectionValue(results,
                                                                            &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_VALUES, &batch);
        IPortableDeviceValues_Release(results);
        if (FAILED(hr))
            break;
        count = 0;
        IPortableDeviceValuesCollection_GetCount(batch, &count);
        if (count)
            IPortableDevicePropertiesBulkCallback_OnProgress(op->callback, &op->context, batch);
        IPortableDeviceValuesCollection_Release(batch);
        if (!count)
            break;
    }
    bulk_end(op);
    bulk_remove(op->owner, op);
    IPortableDevicePropertiesBulkCallback_OnEnd(op->callback, &op->context, hr);
    bulk_free(op);
    if (SUCCEEDED(init))
        CoUninitialize();
    return 0;
}

static HRESULT bulk_queue(struct properties *properties, REFPROPERTYKEY start, REFPROPERTYKEY next, REFPROPERTYKEY end,
                          IPortableDeviceValues *params, IPortableDevicePropertiesBulkCallback *callback, GUID *context)
{
    IPortableDeviceValues *results;
    struct bulk_op *op;
    LPWSTR name = NULL;
    HRESULT hr;

    memset(context, 0, sizeof(*context));
    hr = device_command(properties->device, start, params, &results);
    if (FAILED(hr))
        return hr;
    hr = IPortableDeviceValues_GetStringValue(results, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT, &name);
    IPortableDeviceValues_Release(results);
    if (FAILED(hr))
        return hr;
    op = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*op));
    if (!op)
    {
        CoTaskMemFree(name);
        return E_OUTOFMEMORY;
    }
    CoCreateGuid(&op->context);
    op->owner = properties;
    op->driver_context = name;
    op->next_command = next;
    op->end_command = end;
    op->callback = callback;
    IPortableDevicePropertiesBulkCallback_AddRef(callback);
    EnterCriticalSection(&properties->cs);
    op->next = properties->ops;
    properties->ops = op;
    LeaveCriticalSection(&properties->cs);
    *context = op->context;
    IPortableDevicePropertiesBulkCallback_OnStart(callback, &op->context);
    return S_OK;
}

static HRESULT WINAPI bulk_QueueGetValuesByObjectList(IPortableDevicePropertiesBulk *iface,
                                                      IPortableDevicePropVariantCollection *object_ids,
                                                      IPortableDeviceKeyCollection *keys,
                                                      IPortableDevicePropertiesBulkCallback *callback, GUID *context)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!object_ids || !callback || !context)
        return E_POINTER;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDevicePropVariantCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_IDS,
                                                                       object_ids);
    if (keys)
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PROPERTY_KEYS, keys);
    hr = bulk_queue(impl_from_IPortableDevicePropertiesBulk(iface),
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_START,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_NEXT,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_END, params, callback, context);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI bulk_QueueGetValuesByObjectFormat(IPortableDevicePropertiesBulk *iface, REFGUID format,
                                                        LPCWSTR parent, const DWORD depth,
                                                        IPortableDeviceKeyCollection *keys,
                                                        IPortableDevicePropertiesBulkCallback *callback, GUID *context)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!format || !parent || !callback || !context)
        return E_POINTER;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetGuidValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_FORMAT, format);
    IPortableDeviceValues_SetUnsignedIntegerValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_DEPTH, depth);
    IPortableDeviceValues_SetStringValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PARENT_OBJECT_ID, parent);
    if (keys)
        IPortableDeviceValues_SetIPortableDeviceKeyCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PROPERTY_KEYS, keys);
    hr = bulk_queue(impl_from_IPortableDevicePropertiesBulk(iface),
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_START,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_NEXT,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_END, params, callback, context);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI bulk_QueueSetValuesByObjectList(IPortableDevicePropertiesBulk *iface,
                                                      IPortableDeviceValuesCollection *object_values,
                                                      IPortableDevicePropertiesBulkCallback *callback, GUID *context)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!object_values || !callback || !context)
        return E_POINTER;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetIPortableDeviceValuesCollectionValue(params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_VALUES,
                                                                  object_values);
    hr = bulk_queue(impl_from_IPortableDevicePropertiesBulk(iface),
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_SET_VALUES_BY_OBJECT_LIST_START,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_SET_VALUES_BY_OBJECT_LIST_NEXT,
                    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_SET_VALUES_BY_OBJECT_LIST_END, params, callback, context);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI bulk_Start(IPortableDevicePropertiesBulk *iface, REFGUID context)
{
    struct properties *properties = impl_from_IPortableDevicePropertiesBulk(iface);
    struct bulk_op *op;
    HANDLE thread;

    if (!context)
        return E_POINTER;
    op = bulk_take(properties, context, TRUE);
    if (!op)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    IPortableDeviceProperties_AddRef(&properties->IPortableDeviceProperties_iface);
    thread = CreateThread(NULL, 0, bulk_thread, op, 0, NULL);
    if (!thread)
    {
        op->started = FALSE;
        IPortableDeviceProperties_Release(&properties->IPortableDeviceProperties_iface);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(thread);
    return S_OK;
}

static HRESULT WINAPI bulk_Cancel(IPortableDevicePropertiesBulk *iface, REFGUID context)
{
    struct properties *properties = impl_from_IPortableDevicePropertiesBulk(iface);
    struct bulk_op *op, *iter;
    HANDLE thread;

    if (!context)
        return E_POINTER;
    op = bulk_take(properties, context, FALSE);
    if (!op)
    {
        EnterCriticalSection(&properties->cs);
        for (iter = properties->ops; iter; iter = iter->next)
        {
            if (IsEqualGUID(&iter->context, context))
                iter->cancelled = TRUE;
        }
        LeaveCriticalSection(&properties->cs);
        return S_OK;
    }
    EnterCriticalSection(&properties->cs);
    op->next = properties->ops;
    properties->ops = op;
    op->started = TRUE;
    op->cancelled = TRUE;
    LeaveCriticalSection(&properties->cs);
    IPortableDeviceProperties_AddRef(&properties->IPortableDeviceProperties_iface);
    thread = CreateThread(NULL, 0, bulk_thread, op, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        bulk_thread(op);
    return S_OK;
}

static const IPortableDevicePropertiesBulkVtbl bulk_vtbl =
{
    bulk_QueryInterface,
    bulk_AddRef,
    bulk_Release,
    bulk_QueueGetValuesByObjectList,
    bulk_QueueGetValuesByObjectFormat,
    bulk_QueueSetValuesByObjectList,
    bulk_Start,
    bulk_Cancel,
};

HRESULT properties_create(struct portable_device *device, IPortableDeviceProperties **out)
{
    struct properties *properties;

    *out = NULL;
    properties = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*properties));
    if (!properties)
        return E_OUTOFMEMORY;
    properties->IPortableDeviceProperties_iface.lpVtbl = &properties_vtbl;
    properties->IPortableDevicePropertiesBulk_iface.lpVtbl = &bulk_vtbl;
    InitializeCriticalSection(&properties->cs);
    properties->refcount = 1;
    properties->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    *out = &properties->IPortableDeviceProperties_iface;
    return S_OK;
}

static HRESULT WINAPI capabilities_QueryInterface(IPortableDeviceCapabilities *iface, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPortableDeviceCapabilities))
        return E_NOINTERFACE;
    *out = iface;
    IPortableDeviceCapabilities_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI capabilities_AddRef(IPortableDeviceCapabilities *iface)
{
    return InterlockedIncrement(&impl_from_IPortableDeviceCapabilities(iface)->refcount);
}

static ULONG WINAPI capabilities_Release(IPortableDeviceCapabilities *iface)
{
    struct capabilities *capabilities = impl_from_IPortableDeviceCapabilities(iface);
    ULONG refcount = InterlockedDecrement(&capabilities->refcount);

    if (!refcount)
    {
        IPortableDevice_Release(&capabilities->device->IPortableDevice_iface);
        HeapFree(GetProcessHeap(), 0, capabilities);
    }
    return refcount;
}

static HRESULT capability_query(IPortableDeviceCapabilities *iface, REFPROPERTYKEY command, REFPROPERTYKEY guid_key,
                                const GUID *guid, REFPROPERTYKEY key_key, const PROPERTYKEY *key,
                                REFPROPERTYKEY result, REFIID iid, void **out)
{
    IPortableDeviceValues *params;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    hr = create_values(&params);
    if (FAILED(hr))
        return hr;
    if (guid_key)
    {
        if (!guid)
        {
            IPortableDeviceValues_Release(params);
            return E_POINTER;
        }
        IPortableDeviceValues_SetGuidValue(params, guid_key, guid);
    }
    if (key_key)
    {
        if (!key)
        {
            IPortableDeviceValues_Release(params);
            return E_POINTER;
        }
        IPortableDeviceValues_SetKeyValue(params, key_key, key);
    }
    hr = query(impl_from_IPortableDeviceCapabilities(iface)->device, command, params, result, iid, out);
    IPortableDeviceValues_Release(params);
    return hr;
}

static HRESULT WINAPI capabilities_GetSupportedCommands(IPortableDeviceCapabilities *iface, IPortableDeviceKeyCollection **commands)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_COMMANDS, NULL, NULL, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_SUPPORTED_COMMANDS, &IID_IPortableDeviceKeyCollection, (void **)commands);
}

static HRESULT WINAPI capabilities_GetCommandOptions(IPortableDeviceCapabilities *iface, REFPROPERTYKEY command,
                                                     IPortableDeviceValues **options)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_COMMAND_OPTIONS, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_COMMAND, command,
                            &WPD_PROPERTY_CAPABILITIES_COMMAND_OPTIONS, &IID_IPortableDeviceValues, (void **)options);
}

static HRESULT WINAPI capabilities_GetFunctionalCategories(IPortableDeviceCapabilities *iface,
                                                           IPortableDevicePropVariantCollection **categories)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FUNCTIONAL_CATEGORIES, NULL, NULL, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORIES, &IID_IPortableDevicePropVariantCollection,
                            (void **)categories);
}

static HRESULT WINAPI capabilities_GetFunctionalObjects(IPortableDeviceCapabilities *iface, REFGUID category,
                                                        IPortableDevicePropVariantCollection **objects)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_FUNCTIONAL_OBJECTS,
                            &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORY, category, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_OBJECTS, &IID_IPortableDevicePropVariantCollection,
                            (void **)objects);
}

static HRESULT WINAPI capabilities_GetSupportedContentTypes(IPortableDeviceCapabilities *iface, REFGUID category,
                                                            IPortableDevicePropVariantCollection **types)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_CONTENT_TYPES,
                            &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORY, category, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_CONTENT_TYPES, &IID_IPortableDevicePropVariantCollection,
                            (void **)types);
}

static HRESULT WINAPI capabilities_GetSupportedFormats(IPortableDeviceCapabilities *iface, REFGUID content_type,
                                                       IPortableDevicePropVariantCollection **formats)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FORMATS,
                            &WPD_PROPERTY_CAPABILITIES_CONTENT_TYPE, content_type, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_FORMATS, &IID_IPortableDevicePropVariantCollection,
                            (void **)formats);
}

static HRESULT WINAPI capabilities_GetSupportedFormatProperties(IPortableDeviceCapabilities *iface, REFGUID format,
                                                                IPortableDeviceKeyCollection **keys)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FORMAT_PROPERTIES,
                            &WPD_PROPERTY_CAPABILITIES_FORMAT, format, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_PROPERTY_KEYS, &IID_IPortableDeviceKeyCollection, (void **)keys);
}

static HRESULT WINAPI capabilities_GetFixedPropertyAttributes(IPortableDeviceCapabilities *iface, REFGUID format,
                                                              REFPROPERTYKEY key, IPortableDeviceValues **attributes)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_FIXED_PROPERTY_ATTRIBUTES,
                            &WPD_PROPERTY_CAPABILITIES_FORMAT, format, &WPD_PROPERTY_CAPABILITIES_PROPERTY_KEYS, key,
                            &WPD_PROPERTY_CAPABILITIES_PROPERTY_ATTRIBUTES, &IID_IPortableDeviceValues, (void **)attributes);
}

static HRESULT WINAPI capabilities_Cancel(IPortableDeviceCapabilities *iface)
{
    return S_OK;
}

static HRESULT WINAPI capabilities_GetSupportedEvents(IPortableDeviceCapabilities *iface,
                                                      IPortableDevicePropVariantCollection **events)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_EVENTS, NULL, NULL, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_SUPPORTED_EVENTS, &IID_IPortableDevicePropVariantCollection,
                            (void **)events);
}

static HRESULT WINAPI capabilities_GetEventOptions(IPortableDeviceCapabilities *iface, REFGUID event,
                                                   IPortableDeviceValues **options)
{
    return capability_query(iface, &WPD_COMMAND_CAPABILITIES_GET_EVENT_OPTIONS,
                            &WPD_PROPERTY_CAPABILITIES_EVENT, event, NULL, NULL,
                            &WPD_PROPERTY_CAPABILITIES_EVENT_OPTIONS, &IID_IPortableDeviceValues, (void **)options);
}

static const IPortableDeviceCapabilitiesVtbl capabilities_vtbl =
{
    capabilities_QueryInterface,
    capabilities_AddRef,
    capabilities_Release,
    capabilities_GetSupportedCommands,
    capabilities_GetCommandOptions,
    capabilities_GetFunctionalCategories,
    capabilities_GetFunctionalObjects,
    capabilities_GetSupportedContentTypes,
    capabilities_GetSupportedFormats,
    capabilities_GetSupportedFormatProperties,
    capabilities_GetFixedPropertyAttributes,
    capabilities_Cancel,
    capabilities_GetSupportedEvents,
    capabilities_GetEventOptions,
};

HRESULT capabilities_create(struct portable_device *device, IPortableDeviceCapabilities **out)
{
    struct capabilities *capabilities;

    *out = NULL;
    capabilities = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*capabilities));
    if (!capabilities)
        return E_OUTOFMEMORY;
    capabilities->IPortableDeviceCapabilities_iface.lpVtbl = &capabilities_vtbl;
    capabilities->refcount = 1;
    capabilities->device = device;
    IPortableDevice_AddRef(&device->IPortableDevice_iface);
    *out = &capabilities->IPortableDeviceCapabilities_iface;
    return S_OK;
}
