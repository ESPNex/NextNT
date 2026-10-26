/*
 * PROJECT:     LiberNT Portable Devices shell extension
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Device connections, item IDs and enumeration
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdshext.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpdshext);

static CRITICAL_SECTION devices_cs;
static CRITICAL_SECTION_DEBUG devices_cs_debug =
{
    0, 0, &devices_cs,
    { &devices_cs_debug.ProcessLocksList, &devices_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": devices_cs") }
};
static CRITICAL_SECTION devices_cs = { &devices_cs_debug, -1, 0, 0, 0, 0 };
static wpd_device *devices;

static WCHAR *strdup_heap(const WCHAR *string)
{
    WCHAR *copy;
    SIZE_T size;

    if (!string)
        return NULL;
    size = (lstrlenW(string) + 1) * sizeof(WCHAR);
    copy = HeapAlloc(GetProcessHeap(), 0, size);
    if (copy)
        memcpy(copy, string, size);
    return copy;
}

static void release_device(wpd_device *entry)
{
    if (entry->resources)
        IPortableDeviceResources_Release(entry->resources);
    if (entry->properties)
        IPortableDeviceProperties_Release(entry->properties);
    if (entry->content)
        IPortableDeviceContent_Release(entry->content);
    if (entry->device)
    {
        IPortableDevice_Close(entry->device);
        IPortableDevice_Release(entry->device);
    }
    HeapFree(GetProcessHeap(), 0, entry->pnp_id);
    HeapFree(GetProcessHeap(), 0, entry);
}

HRESULT wpd_get_device(const WCHAR *pnp_id, wpd_device **out)
{
    IPortableDeviceValues *client = NULL;
    wpd_device *entry;
    HRESULT hr;

    *out = NULL;
    EnterCriticalSection(&devices_cs);
    for (entry = devices; entry; entry = entry->next)
    {
        if (!lstrcmpiW(entry->pnp_id, pnp_id))
        {
            *out = entry;
            LeaveCriticalSection(&devices_cs);
            return S_OK;
        }
    }
    entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
    if (!entry)
    {
        LeaveCriticalSection(&devices_cs);
        return E_OUTOFMEMORY;
    }
    entry->pnp_id = strdup_heap(pnp_id);
    hr = entry->pnp_id ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_PortableDeviceFTM, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDevice,
                              (void **)&entry->device);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceValues,
                              (void **)&client);
    if (SUCCEEDED(hr))
    {
        IPortableDeviceValues_SetStringValue(client, &WPD_CLIENT_NAME, L"Windows Portable Devices Shell");
        IPortableDeviceValues_SetUnsignedIntegerValue(client, &WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ | GENERIC_WRITE);
        hr = IPortableDevice_Open(entry->device, pnp_id, client);
        IPortableDeviceValues_Release(client);
    }
    if (SUCCEEDED(hr))
        hr = IPortableDevice_Content(entry->device, &entry->content);
    if (SUCCEEDED(hr))
        hr = IPortableDeviceContent_Properties(entry->content, &entry->properties);
    if (SUCCEEDED(hr))
        hr = IPortableDeviceContent_Transfer(entry->content, &entry->resources);
    if (FAILED(hr))
    {
        WARN("device %s not opened, hr 0x%08lx\n", debugstr_w(pnp_id), hr);
        release_device(entry);
        LeaveCriticalSection(&devices_cs);
        return hr;
    }
    entry->next = devices;
    devices = entry;
    *out = entry;
    LeaveCriticalSection(&devices_cs);
    return S_OK;
}

void wpd_free_devices(void)
{
    wpd_device *entry;

    EnterCriticalSection(&devices_cs);
    while ((entry = devices))
    {
        devices = entry->next;
        release_device(entry);
    }
    LeaveCriticalSection(&devices_cs);
}

const wpd_item *wpd_item_from_pidl(PCUIDLIST_RELATIVE pidl)
{
    const BYTE *data;
    DWORD signature;
    WORD cb;

    if (!pidl)
        return NULL;
    cb = pidl->mkid.cb;
    data = (const BYTE *)pidl;
    if (cb >= sizeof(WORD) + FIELD_OFFSET(wpd_item, strings))
    {
        memcpy(&signature, data + sizeof(WORD), sizeof(signature));
        if (signature == WPD_ITEM_SIGNATURE)
            return (const wpd_item *)(data + sizeof(WORD));
    }
    if (cb >= FIELD_OFFSET(DELEGATEITEMID, rgb) + FIELD_OFFSET(wpd_item, strings))
    {
        memcpy(&signature, data + FIELD_OFFSET(DELEGATEITEMID, rgb), sizeof(signature));
        if (signature == WPD_ITEM_SIGNATURE)
            return (const wpd_item *)(data + FIELD_OFFSET(DELEGATEITEMID, rgb));
    }
    return NULL;
}

const WCHAR *wpd_item_string(const wpd_item *item, int index)
{
    return item->strings + item->offsets[index];
}

BOOL wpd_item_is_container(const wpd_item *item)
{
    return item->kind != WPD_KIND_FILE;
}

PITEMID_CHILD wpd_create_item(IMalloc *malloc, const wpd_item_info *info)
{
    wpd_item header;
    WORD chars = 0, length[WPD_STR_COUNT];
    SIZE_T size;
    BYTE *buffer, *data;
    int i;

    memset(&header, 0, sizeof(header));
    for (i = 0; i < WPD_STR_COUNT; i++)
    {
        length[i] = (WORD)min(lstrlenW(info->strings[i] ? info->strings[i] : L""), 2000) + 1;
        header.offsets[i] = chars;
        chars += length[i];
    }
    size = FIELD_OFFSET(wpd_item, strings) + chars * sizeof(WCHAR);
    if (size > 0xF000)
        return NULL;
    header.signature = WPD_ITEM_SIGNATURE;
    header.kind = info->kind;
    header.type = info->type;
    header.flags = info->flags;
    header.size = info->size;
    header.capacity = info->capacity;
    header.free = info->free;
    header.created = info->created;
    header.modified = info->modified;
    header.authored = info->authored;
    header.duration = info->duration;
    header.track = info->track;
    header.width = info->width;
    header.height = info->height;
    header.chars = chars;

    if (malloc)
    {
        buffer = IMalloc_Alloc(malloc, size);
        if (!buffer)
            return NULL;
        data = buffer + FIELD_OFFSET(DELEGATEITEMID, rgb);
    }
    else
    {
        buffer = CoTaskMemAlloc(sizeof(WORD) + size + sizeof(WORD));
        if (!buffer)
            return NULL;
        memset(buffer, 0, sizeof(WORD) + size + sizeof(WORD));
        *(WORD *)buffer = (WORD)(sizeof(WORD) + size);
        data = buffer + sizeof(WORD);
    }
    memcpy(data, &header, FIELD_OFFSET(wpd_item, strings));
    for (i = 0; i < WPD_STR_COUNT; i++)
    {
        WCHAR *target = (WCHAR *)(data + FIELD_OFFSET(wpd_item, strings)) + header.offsets[i];
        if (length[i] > 1)
            memcpy(target, info->strings[i], (length[i] - 1) * sizeof(WCHAR));
        target[length[i] - 1] = 0;
    }
    return (PITEMID_CHILD)buffer;
}

static void variant_time(const PROPVARIANT *value, FILETIME *time)
{
    SYSTEMTIME st;
    FILETIME local;

    memset(time, 0, sizeof(*time));
    if (value->vt == VT_DATE && VariantTimeToSystemTime(value->date, &st) &&
        SystemTimeToFileTime(&st, &local))
    {
        LocalFileTimeToFileTime(&local, time);
    }
}

int wpd_device_icon(BYTE type)
{
    static const int icons[] =
    {
        IDI_DEVICE_GENERIC, IDI_DEVICE_CAMERA, IDI_DEVICE_MEDIA_PLAYER, IDI_DEVICE_PHONE,
        IDI_DEVICE_VIDEO, IDI_DEVICE_PIM, IDI_DEVICE_AUDIO_RECORDER,
    };

    return type < ARRAY_SIZE(icons) ? icons[type] : IDI_DEVICE_GENERIC;
}

static HRESULT get_values(wpd_device *device, const WCHAR *object_id, const PROPERTYKEY *const *keys, DWORD count,
                          IPortableDeviceValues **values)
{
    IPortableDeviceKeyCollection *collection;
    DWORD i;
    HRESULT hr;

    *values = NULL;
    hr = CoCreateInstance(&CLSID_PortableDeviceKeyCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDeviceKeyCollection, (void **)&collection);
    if (FAILED(hr))
        return hr;
    for (i = 0; i < count; i++)
        IPortableDeviceKeyCollection_Add(collection, keys[i]);
    hr = IPortableDeviceProperties_GetValues(device->properties, object_id, collection, values);
    IPortableDeviceKeyCollection_Release(collection);
    return hr;
}

static WCHAR *get_string(IPortableDeviceValues *values, REFPROPERTYKEY key)
{
    LPWSTR value = NULL;

    if (FAILED(IPortableDeviceValues_GetStringValue(values, key, &value)))
        return NULL;
    return value;
}

HRESULT wpd_object_item(wpd_folder *folder, wpd_device *device, const WCHAR *object_id, PITEMID_CHILD *item)
{
    static const PROPERTYKEY *const keys[] =
    {
        &WPD_OBJECT_NAME, &WPD_OBJECT_ORIGINAL_FILE_NAME, &WPD_OBJECT_CONTENT_TYPE, &WPD_OBJECT_SIZE,
        &WPD_OBJECT_DATE_CREATED, &WPD_OBJECT_DATE_MODIFIED, &WPD_OBJECT_PERSISTENT_UNIQUE_ID,
        &WPD_FUNCTIONAL_OBJECT_CATEGORY, &WPD_STORAGE_CAPACITY, &WPD_STORAGE_FREE_SPACE_IN_BYTES,
        &WPD_STORAGE_FILE_SYSTEM_TYPE, &WPD_OBJECT_FORMAT, &WPD_OBJECT_IS_DRM_PROTECTED, &WPD_MUSIC_TRACK,
        &WPD_MEDIA_ARTIST, &WPD_MUSIC_ALBUM, &WPD_MEDIA_GENRE, &WPD_MEDIA_DURATION, &WPD_OBJECT_DATE_AUTHORED,
        &WPD_MEDIA_WIDTH, &WPD_MEDIA_HEIGHT,
    };
    IPortableDeviceValues *values;
    wpd_item_info info;
    WCHAR *name, *file, *puid, *fs, *artist, *album, *genre, guid_text[39];
    PROPVARIANT value;
    GUID guid, format = WPD_OBJECT_FORMAT_UNSPECIFIED;
    BOOL protected = FALSE;
    GUID content = GUID_NULL, category = GUID_NULL;
    HRESULT hr;

    *item = NULL;
    hr = get_values(device, object_id, keys, ARRAY_SIZE(keys), &values);
    if (FAILED(hr))
        return hr;
    memset(&info, 0, sizeof(info));
    IPortableDeviceValues_GetGuidValue(values, &WPD_OBJECT_CONTENT_TYPE, &content);
    IPortableDeviceValues_GetGuidValue(values, &WPD_FUNCTIONAL_OBJECT_CATEGORY, &category);
    if (IsEqualGUID(&content, &WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT))
    {
        if (!IsEqualGUID(&category, &WPD_FUNCTIONAL_CATEGORY_STORAGE))
        {
            IPortableDeviceValues_Release(values);
            return S_FALSE;
        }
        info.kind = WPD_KIND_STORAGE;
    }
    else
    {
        info.kind = IsEqualGUID(&content, &WPD_CONTENT_TYPE_FOLDER) ? WPD_KIND_FOLDER : WPD_KIND_FILE;
    }
    IPortableDeviceValues_GetUnsignedLargeIntegerValue(values, &WPD_OBJECT_SIZE, &info.size);
    IPortableDeviceValues_GetUnsignedLargeIntegerValue(values, &WPD_STORAGE_CAPACITY, &info.capacity);
    IPortableDeviceValues_GetUnsignedLargeIntegerValue(values, &WPD_STORAGE_FREE_SPACE_IN_BYTES, &info.free);
    PropVariantInit(&value);
    if (SUCCEEDED(IPortableDeviceValues_GetValue(values, &WPD_OBJECT_DATE_CREATED, &value)) && value.vt == VT_DATE)
    {
        variant_time(&value, &info.created);
        info.flags |= WPD_ITEM_HAS_CREATED;
    }
    PropVariantClear(&value);
    if (SUCCEEDED(IPortableDeviceValues_GetValue(values, &WPD_OBJECT_DATE_MODIFIED, &value)))
        variant_time(&value, &info.modified);
    PropVariantClear(&value);
    name = get_string(values, &WPD_OBJECT_NAME);
    file = get_string(values, &WPD_OBJECT_ORIGINAL_FILE_NAME);
    puid = get_string(values, &WPD_OBJECT_PERSISTENT_UNIQUE_ID);
    fs = get_string(values, &WPD_STORAGE_FILE_SYSTEM_TYPE);
    artist = get_string(values, &WPD_MEDIA_ARTIST);
    album = get_string(values, &WPD_MUSIC_ALBUM);
    genre = get_string(values, &WPD_MEDIA_GENRE);
    IPortableDeviceValues_GetGuidValue(values, &WPD_OBJECT_FORMAT, &format);
    IPortableDeviceValues_GetBoolValue(values, &WPD_OBJECT_IS_DRM_PROTECTED, &protected);
    if (SUCCEEDED(IPortableDeviceValues_GetUnsignedIntegerValue(values, &WPD_MUSIC_TRACK, &info.track)))
        info.flags |= WPD_ITEM_HAS_TRACK;
    if (SUCCEEDED(IPortableDeviceValues_GetUnsignedLargeIntegerValue(values, &WPD_MEDIA_DURATION, &info.duration)))
        info.flags |= WPD_ITEM_HAS_DURATION;
    if (SUCCEEDED(IPortableDeviceValues_GetUnsignedIntegerValue(values, &WPD_MEDIA_WIDTH, &info.width)) &&
        SUCCEEDED(IPortableDeviceValues_GetUnsignedIntegerValue(values, &WPD_MEDIA_HEIGHT, &info.height)))
    {
        info.flags |= WPD_ITEM_HAS_DIMENSIONS;
    }
    PropVariantInit(&value);
    if (SUCCEEDED(IPortableDeviceValues_GetValue(values, &WPD_OBJECT_DATE_AUTHORED, &value)) && value.vt == VT_DATE)
    {
        variant_time(&value, &info.authored);
        info.flags |= WPD_ITEM_HAS_AUTHORED;
    }
    PropVariantClear(&value);
    IPortableDeviceValues_Release(values);

    if (info.kind != WPD_KIND_STORAGE)
        info.flags |= WPD_ITEM_CAN_RENAME;
    if (protected)
        info.flags |= WPD_ITEM_PROTECTED;
    if (IsEqualGUID(&format, &WPD_OBJECT_FORMAT_UNSPECIFIED))
        info.flags |= WPD_ITEM_UNSPECIFIED_FORMAT;
    if (IsEqualGUID(&content, &WPD_CONTENT_TYPE_AUDIO))
        info.flags |= WPD_ITEM_AUDIO;
    if (IsEqualGUID(&content, &WPD_CONTENT_TYPE_AUDIO) || IsEqualGUID(&content, &WPD_CONTENT_TYPE_VIDEO) ||
        IsEqualGUID(&content, &WPD_CONTENT_TYPE_IMAGE))
    {
        info.flags |= WPD_ITEM_MEDIA;
    }
    if (artist)
        info.flags |= WPD_ITEM_HAS_ARTIST;
    if (album)
        info.flags |= WPD_ITEM_HAS_ALBUM;
    if (genre && (info.flags & WPD_ITEM_AUDIO))
        info.flags |= WPD_ITEM_HAS_GENRE;
    info.strings[WPD_STR_ID] = object_id;
    info.strings[WPD_STR_PARSE] = puid ? puid : object_id;
    if (puid && lstrlenW(puid) == 38 && puid[0] == '{' && SUCCEEDED(IIDFromString(puid, &guid)) &&
        StringFromGUID2(&guid, guid_text, ARRAY_SIZE(guid_text)))
    {
        info.strings[WPD_STR_PARSE] = guid_text;
    }
    info.strings[WPD_STR_NAME] = name ? name : (file ? file : object_id);
    info.strings[WPD_STR_FILE] = file ? file : (name ? name : object_id);
    info.strings[WPD_STR_EXTRA] = fs;
    info.strings[WPD_STR_ARTIST] = artist;
    info.strings[WPD_STR_ALBUM] = album;
    info.strings[WPD_STR_GENRE] = genre;
    *item = wpd_create_item(folder->kind ? NULL : folder->malloc, &info);
    CoTaskMemFree(name);
    CoTaskMemFree(file);
    CoTaskMemFree(puid);
    CoTaskMemFree(fs);
    CoTaskMemFree(artist);
    CoTaskMemFree(album);
    CoTaskMemFree(genre);
    return *item ? S_OK : E_OUTOFMEMORY;
}

HRESULT wpd_device_item(wpd_folder *folder, const WCHAR *pnp_id, PITEMID_CHILD *item)
{
    IPortableDeviceManager *manager;
    IPortableDeviceValues *attributes;
    wpd_device *device;
    wpd_item_info info;
    WCHAR name[260];
    DWORD chars = ARRAY_SIZE(name), type = 0, size = sizeof(type), kind = 0;
    BOOL writable = FALSE;
    HRESULT hr;

    *item = NULL;
    hr = CoCreateInstance(&CLSID_PortableDeviceManager, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceManager,
                          (void **)&manager);
    if (FAILED(hr))
        return hr;
    name[0] = 0;
    if (FAILED(IPortableDeviceManager_GetDeviceFriendlyName(manager, pnp_id, name, &chars)))
        name[0] = 0;
    if (FAILED(IPortableDeviceManager_GetDeviceProperty(manager, pnp_id, PORTABLE_DEVICE_TYPE, (BYTE *)&type, &size, &kind)) ||
        kind != REG_DWORD)
    {
        type = 0;
    }
    IPortableDeviceManager_Release(manager);

    if (SUCCEEDED(wpd_get_device(pnp_id, &device)) &&
        SUCCEEDED(IPortableDeviceProperties_GetPropertyAttributes(device->properties, WPD_DEVICE_OBJECT_ID,
                                                                  &WPD_DEVICE_FRIENDLY_NAME, &attributes)))
    {
        IPortableDeviceValues_GetBoolValue(attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_WRITE, &writable);
        IPortableDeviceValues_Release(attributes);
    }

    memset(&info, 0, sizeof(info));
    info.kind = WPD_KIND_DEVICE;
    info.type = (BYTE)min(type, 0xFF);
    info.flags = writable ? WPD_ITEM_CAN_RENAME : 0;
    info.strings[WPD_STR_ID] = pnp_id;
    info.strings[WPD_STR_PARSE] = pnp_id;
    info.strings[WPD_STR_NAME] = name[0] ? name : pnp_id;
    info.strings[WPD_STR_FILE] = name[0] ? name : pnp_id;
    *item = wpd_create_item(folder->malloc, &info);
    return *item ? S_OK : E_OUTOFMEMORY;
}

void wpd_free_items(PITEMID_CHILD *items, DWORD count)
{
    DWORD i;

    for (i = 0; i < count; i++)
        ILFree((LPITEMIDLIST)items[i]);
    CoTaskMemFree(items);
}

static HRESULT add_item(PITEMID_CHILD **items, DWORD *count, DWORD *capacity, PITEMID_CHILD item)
{
    PITEMID_CHILD *grown;

    if (*count == *capacity)
    {
        *capacity = *capacity ? *capacity * 2 : 16;
        grown = CoTaskMemRealloc(*items, *capacity * sizeof(**items));
        if (!grown)
        {
            ILFree((LPITEMIDLIST)item);
            return E_OUTOFMEMORY;
        }
        *items = grown;
    }
    (*items)[(*count)++] = item;
    return S_OK;
}

HRESULT wpd_enum_children(wpd_folder *folder, DWORD flags, PITEMID_CHILD **items, DWORD *count)
{
    IEnumPortableDeviceObjectIDs *enumerator;
    IPortableDeviceManager *manager;
    PITEMID_CHILD item;
    wpd_device *device;
    LPWSTR ids[32], *pnp_ids;
    DWORD capacity = 0, total = 0, i;
    ULONG fetched;
    HRESULT hr;

    *items = NULL;
    *count = 0;
    if (!folder->kind)
    {
        if (!(flags & SHCONTF_FOLDERS))
            return S_OK;
        hr = CoCreateInstance(&CLSID_PortableDeviceManager, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceManager,
                              (void **)&manager);
        if (FAILED(hr))
            return hr;
        IPortableDeviceManager_RefreshDeviceList(manager);
        hr = IPortableDeviceManager_GetDevices(manager, NULL, &total);
        pnp_ids = SUCCEEDED(hr) && total ? CoTaskMemAlloc(total * sizeof(*pnp_ids)) : NULL;
        if (pnp_ids && FAILED(IPortableDeviceManager_GetDevices(manager, pnp_ids, &total)))
            total = 0;
        IPortableDeviceManager_Release(manager);
        for (i = 0; pnp_ids && i < total; i++)
        {
            if (SUCCEEDED(wpd_device_item(folder, pnp_ids[i], &item)))
                add_item(items, count, &capacity, item);
            CoTaskMemFree(pnp_ids[i]);
        }
        CoTaskMemFree(pnp_ids);
        return S_OK;
    }

    hr = wpd_get_device(folder->pnp_id, &device);
    if (FAILED(hr))
        return hr;
    hr = IPortableDeviceContent_EnumObjects(device->content, 0, folder->object_id, NULL, &enumerator);
    if (FAILED(hr))
        return hr;
    for (;;)
    {
        fetched = 0;
        hr = IEnumPortableDeviceObjectIDs_Next(enumerator, ARRAY_SIZE(ids), ids, &fetched);
        for (i = 0; i < fetched; i++)
        {
            if (wpd_object_item(folder, device, ids[i], &item) == S_OK)
            {
                const wpd_item *data = wpd_item_from_pidl(item);
                BOOL container = wpd_item_is_container(data);

                if ((container && (flags & SHCONTF_FOLDERS)) || (!container && (flags & SHCONTF_NONFOLDERS)))
                    add_item(items, count, &capacity, item);
                else
                    ILFree((LPITEMIDLIST)item);
            }
            CoTaskMemFree(ids[i]);
        }
        if (hr != S_OK || !fetched)
            break;
    }
    IEnumPortableDeviceObjectIDs_Release(enumerator);
    return S_OK;
}

typedef struct wpd_enum
{
    IEnumIDList IEnumIDList_iface;
    LONG refcount;
    PITEMID_CHILD *items;
    DWORD count;
    DWORD next;
} wpd_enum;

static inline wpd_enum *impl_from_IEnumIDList(IEnumIDList *iface)
{
    return CONTAINING_RECORD(iface, wpd_enum, IEnumIDList_iface);
}

static HRESULT WINAPI enum_QueryInterface(IEnumIDList *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumIDList))
    {
        *out = iface;
        IEnumIDList_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef(IEnumIDList *iface)
{
    return InterlockedIncrement(&impl_from_IEnumIDList(iface)->refcount);
}

static ULONG WINAPI enum_Release(IEnumIDList *iface)
{
    wpd_enum *enumerator = impl_from_IEnumIDList(iface);
    ULONG refcount = InterlockedDecrement(&enumerator->refcount);

    if (!refcount)
    {
        wpd_free_items(enumerator->items, enumerator->count);
        HeapFree(GetProcessHeap(), 0, enumerator);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static HRESULT WINAPI enum_Next(IEnumIDList *iface, ULONG count, PITEMID_CHILD *items, ULONG *fetched)
{
    wpd_enum *enumerator = impl_from_IEnumIDList(iface);
    ULONG done = 0;

    if (!items || (count > 1 && !fetched))
        return E_INVALIDARG;
    while (done < count && enumerator->next < enumerator->count)
    {
        items[done] = (PITEMID_CHILD)ILClone((LPCITEMIDLIST)enumerator->items[enumerator->next]);
        if (!items[done])
            break;
        enumerator->next++;
        done++;
    }
    if (fetched)
        *fetched = done;
    return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumIDList *iface, ULONG count)
{
    wpd_enum *enumerator = impl_from_IEnumIDList(iface);
    DWORD left = enumerator->count - enumerator->next;

    enumerator->next += min(count, left);
    return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Reset(IEnumIDList *iface)
{
    impl_from_IEnumIDList(iface)->next = 0;
    return S_OK;
}

static HRESULT WINAPI enum_Clone(IEnumIDList *iface, IEnumIDList **out)
{
    *out = NULL;
    return E_NOTIMPL;
}

static const IEnumIDListVtbl enum_vtbl =
{
    enum_QueryInterface,
    enum_AddRef,
    enum_Release,
    enum_Next,
    enum_Skip,
    enum_Reset,
    enum_Clone,
};

HRESULT wpd_enum_create(PITEMID_CHILD *items, DWORD count, IEnumIDList **out)
{
    wpd_enum *enumerator;

    enumerator = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*enumerator));
    if (!enumerator)
    {
        wpd_free_items(items, count);
        return E_OUTOFMEMORY;
    }
    enumerator->IEnumIDList_iface.lpVtbl = &enum_vtbl;
    enumerator->refcount = 1;
    enumerator->items = items;
    enumerator->count = count;
    InterlockedIncrement(&wpdshext_locks);
    *out = &enumerator->IEnumIDList_iface;
    return S_OK;
}
