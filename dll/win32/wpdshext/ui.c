/*
 * PROJECT:     LiberNT Portable Devices shell extension
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Icons, context menus, copy out, copy in and delete
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdshext.h"

WINE_DEFAULT_DEBUG_CHANNEL(wpdshext);

typedef struct wpd_icon
{
    IExtractIconW IExtractIconW_iface;
    LONG refcount;
    BYTE kind;
    BYTE type;
} wpd_icon;

static inline wpd_icon *impl_from_IExtractIconW(IExtractIconW *iface)
{
    return CONTAINING_RECORD(iface, wpd_icon, IExtractIconW_iface);
}

static HRESULT WINAPI icon_QueryInterface(IExtractIconW *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IExtractIconW))
    {
        *out = iface;
        IExtractIconW_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI icon_AddRef(IExtractIconW *iface)
{
    return InterlockedIncrement(&impl_from_IExtractIconW(iface)->refcount);
}

static ULONG WINAPI icon_Release(IExtractIconW *iface)
{
    wpd_icon *icon = impl_from_IExtractIconW(iface);
    ULONG refcount = InterlockedDecrement(&icon->refcount);

    if (!refcount)
    {
        HeapFree(GetProcessHeap(), 0, icon);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static HRESULT WINAPI icon_GetIconLocation(IExtractIconW *iface, UINT flags, LPWSTR file, UINT chars, int *index, UINT *out_flags)
{
    wpd_icon *icon = impl_from_IExtractIconW(iface);

    if (!file || !index || !out_flags)
        return E_INVALIDARG;
    *out_flags = 0;
    if (icon->kind == WPD_KIND_FOLDER)
    {
        lstrcpynW(file, L"shell32.dll", chars);
        *index = (flags & GIL_OPENICON) ? -5 : -4;
        return S_OK;
    }
    if (!GetModuleFileNameW(wpdshext_instance, file, chars))
        return E_FAIL;
    *index = -(icon->kind == WPD_KIND_STORAGE ? IDI_STORAGE : wpd_device_icon(icon->type));
    return S_OK;
}

static HRESULT WINAPI icon_Extract(IExtractIconW *iface, LPCWSTR file, UINT index, HICON *large_icon, HICON *small_icon, UINT sizes)
{
    return S_FALSE;
}

static const IExtractIconWVtbl icon_vtbl =
{
    icon_QueryInterface,
    icon_AddRef,
    icon_Release,
    icon_GetIconLocation,
    icon_Extract,
};

HRESULT wpd_extract_icon_create(const wpd_item *item, REFIID iid, void **out)
{
    wpd_icon *icon;
    HRESULT hr;

    *out = NULL;
    if (item->kind == WPD_KIND_FILE)
        return SHCreateFileExtractIconW(wpd_item_string(item, WPD_STR_FILE), FILE_ATTRIBUTE_NORMAL, iid, out);
    icon = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*icon));
    if (!icon)
        return E_OUTOFMEMORY;
    icon->IExtractIconW_iface.lpVtbl = &icon_vtbl;
    icon->refcount = 1;
    icon->kind = item->kind;
    icon->type = item->type;
    InterlockedIncrement(&wpdshext_locks);
    hr = IExtractIconW_QueryInterface(&icon->IExtractIconW_iface, iid, out);
    IExtractIconW_Release(&icon->IExtractIconW_iface);
    return hr;
}

static HRESULT items_from_data(IDataObject *data, PIDLIST_ABSOLUTE *parent, PCUITEMID_CHILD **items, UINT *count,
                               STGMEDIUM *medium)
{
    FORMATETC format = { (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLISTW), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    CIDA *cida;
    UINT i;

    *items = NULL;
    *count = 0;
    *parent = NULL;
    if (!data || FAILED(IDataObject_GetData(data, &format, medium)))
        return E_INVALIDARG;
    cida = GlobalLock(medium->hGlobal);
    if (!cida || !cida->cidl)
    {
        if (cida)
            GlobalUnlock(medium->hGlobal);
        ReleaseStgMedium(medium);
        return E_INVALIDARG;
    }
    *items = CoTaskMemAlloc(cida->cidl * sizeof(**items));
    if (!*items)
    {
        GlobalUnlock(medium->hGlobal);
        ReleaseStgMedium(medium);
        return E_OUTOFMEMORY;
    }
    *parent = (PIDLIST_ABSOLUTE)((BYTE *)cida + cida->aoffset[0]);
    for (i = 0; i < cida->cidl; i++)
        (*items)[i] = (PCUITEMID_CHILD)((BYTE *)cida + cida->aoffset[i + 1]);
    *count = cida->cidl;
    return S_OK;
}

HRESULT wpd_delete_items(wpd_folder *folder, UINT count, PCUITEMID_CHILD_ARRAY items)
{
    IPortableDevicePropVariantCollection *ids;
    PROPVARIANT value;
    PIDLIST_ABSOLUTE absolute;
    wpd_device *device;
    HRESULT hr;
    UINT i;

    if (!folder->pnp_id)
        return E_ACCESSDENIED;
    hr = wpd_get_device(folder->pnp_id, &device);
    if (FAILED(hr))
        return hr;
    hr = CoCreateInstance(&CLSID_PortableDevicePropVariantCollection, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IPortableDevicePropVariantCollection, (void **)&ids);
    if (FAILED(hr))
        return hr;
    for (i = 0; i < count; i++)
    {
        const wpd_item *item = wpd_item_from_pidl(items[i]);

        if (!item || item->kind == WPD_KIND_DEVICE || item->kind == WPD_KIND_STORAGE)
            continue;
        PropVariantInit(&value);
        value.vt = VT_LPWSTR;
        value.pwszVal = (LPWSTR)wpd_item_string(item, WPD_STR_ID);
        IPortableDevicePropVariantCollection_Add(ids, &value);
    }
    hr = IPortableDeviceContent_Delete(device->content, PORTABLE_DEVICE_DELETE_WITH_RECURSION, ids, NULL);
    IPortableDevicePropVariantCollection_Release(ids);
    for (i = 0; i < count; i++)
    {
        const wpd_item *item = wpd_item_from_pidl(items[i]);

        absolute = item ? ILCombine(folder->pidl, (LPCITEMIDLIST)items[i]) : NULL;
        if (absolute)
            SHChangeNotify(wpd_item_is_container(item) ? SHCNE_RMDIR : SHCNE_DELETE, SHCNF_IDLIST, absolute, NULL);
        ILFree(absolute);
    }
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, folder->pidl, NULL);
    return hr;
}

HRESULT CALLBACK wpd_menu_callback(IShellFolder *iface, HWND hwnd, IDataObject *data, UINT msg, WPARAM wparam, LPARAM lparam)
{
    wpd_folder *folder = wpd_folder_from_shellfolder(iface);
    PCUITEMID_CHILD *items;
    PIDLIST_ABSOLUTE parent;
    STGMEDIUM medium;
    WCHAR format[256], text[300];
    UINT count;
    HRESULT hr;

    switch (msg)
    {
        case DFM_MERGECONTEXTMENU:
            return S_OK;
        case DFM_INVOKECOMMAND:
            if (wparam == DFM_CMD_PROPERTIES)
                return S_OK;
            if (wparam != DFM_CMD_DELETE || !folder)
                return S_FALSE;
            hr = items_from_data(data, &parent, &items, &count, &medium);
            if (FAILED(hr))
                return hr;
            LoadStringW(wpdshext_instance, count == 1 ? IDS_CONFIRM_DELETE_ONE : IDS_CONFIRM_DELETE_MANY, format,
                        ARRAY_SIZE(format));
            wsprintfW(text, format, count);
            LoadStringW(wpdshext_instance, IDS_CONFIRM_DELETE_TITLE, format, ARRAY_SIZE(format));
            if (!hwnd || MessageBoxW(hwnd, text, format, MB_YESNO | MB_ICONWARNING) == IDYES)
                hr = wpd_delete_items(folder, count, items);
            else
                hr = S_OK;
            CoTaskMemFree(items);
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            return FAILED(hr) ? hr : S_OK;
        case DFM_GETDEFSTATICID:
            return S_FALSE;
        default:
            return E_NOTIMPL;
    }
}

typedef struct wpd_entry
{
    WCHAR path[MAX_PATH];
    WCHAR *id;
    BOOL directory;
    ULONGLONG size;
    FILETIME modified;
} wpd_entry;

typedef struct wpd_data
{
    IDataObject IDataObject_iface;
    LONG refcount;
    wpd_folder *folder;
    PITEMID_CHILD *items;
    UINT count;
    wpd_entry *entries;
    UINT entry_count;
    UINT entry_capacity;
    BOOL expanded;
    DWORD drop_effect;
    CLIPFORMAT cf_idlist;
    CLIPFORMAT cf_descriptor;
    CLIPFORMAT cf_contents;
    CLIPFORMAT cf_drop_effect;
    CLIPFORMAT cf_storage_attributes;
    CLIPFORMAT cf_nse;
    CLIPFORMAT cf_pnp_path;
    CLIPFORMAT cf_storage_puid;
} wpd_data;

typedef struct wpd_folder_stream
{
    IStream IStream_iface;
    LONG refcount;
} wpd_folder_stream;

static inline wpd_folder_stream *impl_from_IStream(IStream *iface)
{
    return CONTAINING_RECORD(iface, wpd_folder_stream, IStream_iface);
}

static HRESULT WINAPI stream_QueryInterface(IStream *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ISequentialStream) || IsEqualIID(iid, &IID_IStream))
    {
        *out = iface;
        IStream_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IStream *iface)
{
    return InterlockedIncrement(&impl_from_IStream(iface)->refcount);
}

static ULONG WINAPI stream_Release(IStream *iface)
{
    wpd_folder_stream *stream = impl_from_IStream(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    if (!refcount)
    {
        HeapFree(GetProcessHeap(), 0, stream);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static HRESULT WINAPI stream_Read(IStream *iface, void *buffer, ULONG size, ULONG *read)
{
    if (read)
        *read = 0;
    return E_INVALIDARG;
}

static HRESULT WINAPI stream_Write(IStream *iface, const void *buffer, ULONG size, ULONG *written)
{
    if (written)
        *written = 0;
    return E_INVALIDARG;
}

static HRESULT WINAPI stream_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_SetSize(IStream *iface, ULARGE_INTEGER size)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_CopyTo(IStream *iface, IStream *target, ULARGE_INTEGER size, ULARGE_INTEGER *read,
                                    ULARGE_INTEGER *written)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_Commit(IStream *iface, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_Revert(IStream *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_Stat(IStream *iface, STATSTG *stat, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_Clone(IStream *iface, IStream **out)
{
    return E_NOTIMPL;
}

static const IStreamVtbl folder_stream_vtbl =
{
    stream_QueryInterface,
    stream_AddRef,
    stream_Release,
    stream_Read,
    stream_Write,
    stream_Seek,
    stream_SetSize,
    stream_CopyTo,
    stream_Commit,
    stream_Revert,
    stream_LockRegion,
    stream_UnlockRegion,
    stream_Stat,
    stream_Clone,
};

static HRESULT folder_stream_create(IStream **out)
{
    wpd_folder_stream *stream;

    *out = NULL;
    stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream));
    if (!stream)
        return E_OUTOFMEMORY;
    stream->IStream_iface.lpVtbl = &folder_stream_vtbl;
    stream->refcount = 1;
    InterlockedIncrement(&wpdshext_locks);
    *out = &stream->IStream_iface;
    return S_OK;
}

static HGLOBAL make_dword(DWORD value)
{
    HGLOBAL global = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));

    if (global)
    {
        *(DWORD *)GlobalLock(global) = value;
        GlobalUnlock(global);
    }
    return global;
}

static HGLOBAL make_string(const WCHAR *value)
{
    SIZE_T size = lstrlenW(value) * sizeof(WCHAR);
    HGLOBAL global = GlobalAlloc(GHND | GMEM_SHARE, size);

    if (global)
    {
        memcpy(GlobalLock(global), value, size);
        GlobalUnlock(global);
    }
    return global;
}

static DWORD storage_attributes(const wpd_data *data)
{
    UINT i;

    for (i = 0; i < data->count; i++)
    {
        const wpd_item *item = wpd_item_from_pidl(data->items[i]);

        if (!item || !wpd_item_is_container(item))
            return 0xF8;
    }
    return data->count ? 0xFB : 0xF8;
}

static inline wpd_data *impl_from_IDataObject(IDataObject *iface)
{
    return CONTAINING_RECORD(iface, wpd_data, IDataObject_iface);
}

static WCHAR *strdup_heap(const WCHAR *string)
{
    SIZE_T size = (lstrlenW(string) + 1) * sizeof(WCHAR);
    WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, size);

    if (copy)
        memcpy(copy, string, size);
    return copy;
}

static HRESULT add_entry(wpd_data *data, const WCHAR *relative, const wpd_item *item)
{
    wpd_entry *entries;
    wpd_entry *entry;

    if (data->entry_count == data->entry_capacity)
    {
        data->entry_capacity = data->entry_capacity ? data->entry_capacity * 2 : 16;
        entries = data->entries ?
                  HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, data->entries, data->entry_capacity * sizeof(*entries)) :
                  HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, data->entry_capacity * sizeof(*entries));
        if (!entries)
            return E_OUTOFMEMORY;
        data->entries = entries;
    }
    entry = &data->entries[data->entry_count];
    lstrcpynW(entry->path, relative, ARRAY_SIZE(entry->path));
    entry->id = strdup_heap(wpd_item_string(item, WPD_STR_ID));
    if (!entry->id)
        return E_OUTOFMEMORY;
    entry->directory = wpd_item_is_container(item);
    entry->size = item->size;
    entry->modified = item->modified;
    data->entry_count++;
    return S_OK;
}

static HRESULT expand_item(wpd_data *data, const WCHAR *prefix, const wpd_item *item, int depth)
{
    wpd_folder scan;
    PITEMID_CHILD *children;
    WCHAR relative[MAX_PATH];
    DWORD count, i;
    HRESULT hr;

    if (depth > 32)
        return S_OK;
    if (prefix[0])
    {
        lstrcpynW(relative, prefix, ARRAY_SIZE(relative));
        PathAppendW(relative, wpd_item_string(item, WPD_STR_FILE));
    }
    else
    {
        lstrcpynW(relative, wpd_item_string(item, WPD_STR_FILE), ARRAY_SIZE(relative));
    }
    hr = add_entry(data, relative, item);
    if (FAILED(hr) || !wpd_item_is_container(item))
        return hr;
    memset(&scan, 0, sizeof(scan));
    scan.kind = item->kind;
    scan.pnp_id = data->folder->pnp_id;
    scan.object_id = (WCHAR *)wpd_item_string(item, WPD_STR_ID);
    hr = wpd_enum_children(&scan, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &children, &count);
    if (FAILED(hr))
        return hr;
    for (i = 0; i < count && SUCCEEDED(hr); i++)
        hr = expand_item(data, relative, wpd_item_from_pidl(children[i]), depth + 1);
    wpd_free_items(children, count);
    return hr;
}

static HRESULT expand(wpd_data *data)
{
    HRESULT hr = S_OK;
    UINT i;

    if (data->expanded)
        return S_OK;
    for (i = 0; i < data->count && SUCCEEDED(hr); i++)
    {
        const wpd_item *item = wpd_item_from_pidl(data->items[i]);

        if (item && item->kind != WPD_KIND_DEVICE)
            hr = expand_item(data, L"", item, 0);
    }
    data->expanded = SUCCEEDED(hr);
    return hr;
}

static HGLOBAL make_idlist(wpd_data *data)
{
    SIZE_T size, offset;
    UINT i;
    CIDA *cida;
    HGLOBAL global;

    size = FIELD_OFFSET(CIDA, aoffset[data->count + 1]) + ILGetSize(data->folder->pidl);
    for (i = 0; i < data->count; i++)
        size += ILGetSize((LPCITEMIDLIST)data->items[i]);
    global = GlobalAlloc(GHND | GMEM_SHARE, size);
    if (!global)
        return NULL;
    cida = GlobalLock(global);
    cida->cidl = data->count;
    offset = FIELD_OFFSET(CIDA, aoffset[data->count + 1]);
    cida->aoffset[0] = (UINT)offset;
    memcpy((BYTE *)cida + offset, data->folder->pidl, ILGetSize(data->folder->pidl));
    offset += ILGetSize(data->folder->pidl);
    for (i = 0; i < data->count; i++)
    {
        cida->aoffset[i + 1] = (UINT)offset;
        memcpy((BYTE *)cida + offset, data->items[i], ILGetSize((LPCITEMIDLIST)data->items[i]));
        offset += ILGetSize((LPCITEMIDLIST)data->items[i]);
    }
    GlobalUnlock(global);
    return global;
}

static HGLOBAL make_descriptor(wpd_data *data)
{
    FILEGROUPDESCRIPTORW *group;
    HGLOBAL global;
    UINT i;

    global = GlobalAlloc(GHND | GMEM_SHARE, FIELD_OFFSET(FILEGROUPDESCRIPTORW, fgd[max(data->entry_count, 1)]));
    if (!global)
        return NULL;
    group = GlobalLock(global);
    group->cItems = data->entry_count;
    for (i = 0; i < data->entry_count; i++)
    {
        FILEDESCRIPTORW *descriptor = &group->fgd[i];

        descriptor->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
        descriptor->dwFileAttributes = data->entries[i].directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!data->entries[i].directory)
        {
            descriptor->dwFlags |= FD_FILESIZE;
            descriptor->nFileSizeLow = (DWORD)data->entries[i].size;
            descriptor->nFileSizeHigh = (DWORD)(data->entries[i].size >> 32);
        }
        if (data->entries[i].modified.dwLowDateTime || data->entries[i].modified.dwHighDateTime)
        {
            descriptor->dwFlags |= FD_WRITESTIME;
            descriptor->ftLastWriteTime = data->entries[i].modified;
        }
        lstrcpynW(descriptor->cFileName, data->entries[i].path, ARRAY_SIZE(descriptor->cFileName));
    }
    GlobalUnlock(global);
    return global;
}

static HRESULT WINAPI data_QueryInterface(IDataObject *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject))
    {
        *out = iface;
        IDataObject_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_AddRef(IDataObject *iface)
{
    return InterlockedIncrement(&impl_from_IDataObject(iface)->refcount);
}

static ULONG WINAPI data_Release(IDataObject *iface)
{
    wpd_data *data = impl_from_IDataObject(iface);
    ULONG refcount = InterlockedDecrement(&data->refcount);
    UINT i;

    if (!refcount)
    {
        for (i = 0; i < data->entry_count; i++)
            HeapFree(GetProcessHeap(), 0, data->entries[i].id);
        HeapFree(GetProcessHeap(), 0, data->entries);
        wpd_free_items(data->items, data->count);
        IShellFolder2_Release(&data->folder->IShellFolder2_iface);
        HeapFree(GetProcessHeap(), 0, data);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static HRESULT WINAPI data_GetData(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
    wpd_data *data = impl_from_IDataObject(iface);
    wpd_device *device;
    DWORD optimal = 0;
    HRESULT hr;

    if (!format || !medium)
        return E_INVALIDARG;
    memset(medium, 0, sizeof(*medium));
    if (format->cfFormat == data->cf_idlist && (format->tymed & TYMED_HGLOBAL))
    {
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = make_idlist(data);
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    if ((format->cfFormat == data->cf_drop_effect || format->cfFormat == data->cf_storage_attributes ||
         format->cfFormat == data->cf_nse || format->cfFormat == data->cf_pnp_path ||
         (format->cfFormat == data->cf_storage_puid && data->folder->storage_puid)) &&
        (format->tymed & TYMED_HGLOBAL))
    {
        medium->tymed = TYMED_HGLOBAL;
        if (format->cfFormat == data->cf_drop_effect)
            medium->hGlobal = make_dword(data->drop_effect);
        else if (format->cfFormat == data->cf_storage_attributes)
            medium->hGlobal = make_dword(storage_attributes(data));
        else if (format->cfFormat == data->cf_nse)
            medium->hGlobal = make_dword(1);
        else if (format->cfFormat == data->cf_pnp_path)
            medium->hGlobal = make_string(data->folder->pnp_id);
        else
            medium->hGlobal = make_string(data->folder->storage_puid);
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    if (format->cfFormat == data->cf_descriptor && (format->tymed & TYMED_HGLOBAL))
    {
        hr = expand(data);
        if (FAILED(hr))
            return hr;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = make_descriptor(data);
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    if (format->cfFormat == data->cf_contents && (format->tymed & TYMED_ISTREAM))
    {
        hr = expand(data);
        if (FAILED(hr))
            return hr;
        if (format->lindex < 0 || (UINT)format->lindex >= data->entry_count)
            return DV_E_LINDEX;
        if (data->entries[format->lindex].directory)
        {
            hr = folder_stream_create(&medium->pstm);
            if (SUCCEEDED(hr))
                medium->tymed = TYMED_ISTREAM;
            return hr;
        }
        hr = wpd_get_device(data->folder->pnp_id, &device);
        if (FAILED(hr))
            return hr;
        hr = IPortableDeviceResources_GetStream(device->resources, data->entries[format->lindex].id, &WPD_RESOURCE_DEFAULT,
                                                STGM_READ, &optimal, &medium->pstm);
        if (FAILED(hr))
            return hr;
        medium->tymed = TYMED_ISTREAM;
        return S_OK;
    }
    return DV_E_FORMATETC;
}

static HRESULT WINAPI data_GetDataHere(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI data_QueryGetData(IDataObject *iface, FORMATETC *format)
{
    wpd_data *data = impl_from_IDataObject(iface);

    if (!format)
        return E_INVALIDARG;
    if (format->cfFormat == data->cf_contents)
        return (format->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
    if (format->cfFormat == data->cf_idlist || format->cfFormat == data->cf_descriptor ||
        format->cfFormat == data->cf_drop_effect || format->cfFormat == data->cf_storage_attributes ||
        format->cfFormat == data->cf_nse || format->cfFormat == data->cf_pnp_path ||
        (format->cfFormat == data->cf_storage_puid && data->folder->storage_puid))
    {
        return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    }
    return DV_E_FORMATETC;
}

static HRESULT WINAPI data_GetCanonicalFormatEtc(IDataObject *iface, FORMATETC *in, FORMATETC *out)
{
    if (out)
        out->ptd = NULL;
    return DATA_S_SAMEFORMATETC;
}

static HRESULT WINAPI data_SetData(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium, BOOL release)
{
    wpd_data *data = impl_from_IDataObject(iface);
    DWORD *value;

    if (!format || !medium)
        return E_INVALIDARG;
    if (format->cfFormat == data->cf_drop_effect && medium->tymed == TYMED_HGLOBAL)
    {
        value = GlobalLock(medium->hGlobal);
        if (value)
        {
            data->drop_effect = *value;
            GlobalUnlock(medium->hGlobal);
        }
    }
    if (release)
        ReleaseStgMedium(medium);
    return S_OK;
}

static HRESULT WINAPI data_EnumFormatEtc(IDataObject *iface, DWORD direction, IEnumFORMATETC **out)
{
    wpd_data *data = impl_from_IDataObject(iface);
    FORMATETC *formats;
    UINT count = 0, i;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (direction != DATADIR_GET)
        return E_NOTIMPL;
    formats = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (data->count + 7) * sizeof(*formats));
    if (!formats)
        return E_OUTOFMEMORY;
    formats[count++].cfFormat = data->cf_idlist;
    for (i = 0; i < max(data->count, 1); i++)
    {
        formats[count].cfFormat = data->cf_contents;
        formats[count++].tymed = TYMED_ISTREAM;
    }
    formats[count++].cfFormat = data->cf_descriptor;
    formats[count++].cfFormat = data->cf_storage_attributes;
    formats[count++].cfFormat = data->cf_drop_effect;
    formats[count++].cfFormat = data->cf_nse;
    formats[count++].cfFormat = data->cf_pnp_path;
    if (data->folder->storage_puid)
        formats[count++].cfFormat = data->cf_storage_puid;
    for (i = 0; i < count; i++)
    {
        formats[i].dwAspect = DVASPECT_CONTENT;
        formats[i].lindex = -1;
        if (!formats[i].tymed)
            formats[i].tymed = TYMED_HGLOBAL;
    }
    hr = SHCreateStdEnumFmtEtc(count, formats, out);
    HeapFree(GetProcessHeap(), 0, formats);
    return hr;
}

static HRESULT WINAPI data_DAdvise(IDataObject *iface, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_DUnadvise(IDataObject *iface, DWORD connection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_EnumDAdvise(IDataObject *iface, IEnumSTATDATA **out)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl data_vtbl =
{
    data_QueryInterface,
    data_AddRef,
    data_Release,
    data_GetData,
    data_GetDataHere,
    data_QueryGetData,
    data_GetCanonicalFormatEtc,
    data_SetData,
    data_EnumFormatEtc,
    data_DAdvise,
    data_DUnadvise,
    data_EnumDAdvise,
};

HRESULT wpd_data_object_create(wpd_folder *folder, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID iid, void **out)
{
    wpd_data *data;
    HRESULT hr;
    UINT i;

    *out = NULL;
    data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data));
    if (!data)
        return E_OUTOFMEMORY;
    data->items = CoTaskMemAlloc(count * sizeof(*data->items));
    if (!data->items)
    {
        HeapFree(GetProcessHeap(), 0, data);
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        data->items[i] = (PITEMID_CHILD)ILClone((LPCITEMIDLIST)items[i]);
        if (!data->items[i])
        {
            wpd_free_items(data->items, i);
            HeapFree(GetProcessHeap(), 0, data);
            return E_OUTOFMEMORY;
        }
    }
    data->IDataObject_iface.lpVtbl = &data_vtbl;
    data->refcount = 1;
    data->folder = folder;
    data->count = count;
    data->drop_effect = DROPEFFECT_COPY;
    data->cf_idlist = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLISTW);
    data->cf_descriptor = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    data->cf_contents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTSW);
    data->cf_drop_effect = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECTW);
    data->cf_storage_attributes = (CLIPFORMAT)RegisterClipboardFormatW(L"WPD Storage Attributes");
    data->cf_nse = (CLIPFORMAT)RegisterClipboardFormatW(L"WPD NSE");
    data->cf_pnp_path = (CLIPFORMAT)RegisterClipboardFormatW(L"WPD NSE PnPDevicePath");
    data->cf_storage_puid = (CLIPFORMAT)RegisterClipboardFormatW(L"WPD NSE StoragePUID");
    IShellFolder2_AddRef(&folder->IShellFolder2_iface);
    InterlockedIncrement(&wpdshext_locks);
    hr = IDataObject_QueryInterface(&data->IDataObject_iface, iid, out);
    IDataObject_Release(&data->IDataObject_iface);
    return hr;
}

typedef struct wpd_drop
{
    IDropTarget IDropTarget_iface;
    LONG refcount;
    wpd_folder *folder;
    BOOL accept;
} wpd_drop;

static inline wpd_drop *impl_from_IDropTarget(IDropTarget *iface)
{
    return CONTAINING_RECORD(iface, wpd_drop, IDropTarget_iface);
}

static HRESULT WINAPI drop_QueryInterface(IDropTarget *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropTarget))
    {
        *out = iface;
        IDropTarget_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI drop_AddRef(IDropTarget *iface)
{
    return InterlockedIncrement(&impl_from_IDropTarget(iface)->refcount);
}

static ULONG WINAPI drop_Release(IDropTarget *iface)
{
    wpd_drop *drop = impl_from_IDropTarget(iface);
    ULONG refcount = InterlockedDecrement(&drop->refcount);

    if (!refcount)
    {
        IShellFolder2_Release(&drop->folder->IShellFolder2_iface);
        HeapFree(GetProcessHeap(), 0, drop);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static BOOL has_files(IDataObject *data)
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

    return data && IDataObject_QueryGetData(data, &format) == S_OK;
}

static HRESULT WINAPI drop_DragEnter(IDropTarget *iface, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    wpd_drop *drop = impl_from_IDropTarget(iface);

    drop->accept = has_files(data);
    *effect = drop->accept ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT WINAPI drop_DragOver(IDropTarget *iface, DWORD keys, POINTL point, DWORD *effect)
{
    *effect = impl_from_IDropTarget(iface)->accept ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT WINAPI drop_DragLeave(IDropTarget *iface)
{
    impl_from_IDropTarget(iface)->accept = FALSE;
    return S_OK;
}

static void file_format(const WCHAR *name, GUID *content, GUID *format)
{
    static const struct
    {
        const WCHAR *extension;
        const GUID *content;
        const GUID *format;
    } map[] =
    {
        { L".mp3", &WPD_CONTENT_TYPE_AUDIO, &WPD_OBJECT_FORMAT_MP3 },
        { L".wma", &WPD_CONTENT_TYPE_AUDIO, &WPD_OBJECT_FORMAT_WMA },
        { L".wav", &WPD_CONTENT_TYPE_AUDIO, &WPD_OBJECT_FORMAT_WAVE },
        { L".jpg", &WPD_CONTENT_TYPE_IMAGE, &WPD_OBJECT_FORMAT_EXIF },
        { L".jpeg", &WPD_CONTENT_TYPE_IMAGE, &WPD_OBJECT_FORMAT_EXIF },
        { L".png", &WPD_CONTENT_TYPE_IMAGE, &WPD_OBJECT_FORMAT_PNG },
        { L".bmp", &WPD_CONTENT_TYPE_IMAGE, &WPD_OBJECT_FORMAT_BMP },
        { L".gif", &WPD_CONTENT_TYPE_IMAGE, &WPD_OBJECT_FORMAT_GIF },
        { L".mp4", &WPD_CONTENT_TYPE_VIDEO, &WPD_OBJECT_FORMAT_MP4 },
        { L".avi", &WPD_CONTENT_TYPE_VIDEO, &WPD_OBJECT_FORMAT_AVI },
        { L".wmv", &WPD_CONTENT_TYPE_VIDEO, &WPD_OBJECT_FORMAT_WMV },
        { L".txt", &WPD_CONTENT_TYPE_DOCUMENT, &WPD_OBJECT_FORMAT_TEXT },
    };
    const WCHAR *extension = PathFindExtensionW(name);
    UINT i;

    *content = WPD_CONTENT_TYPE_GENERIC_FILE;
    *format = WPD_OBJECT_FORMAT_UNSPECIFIED;
    for (i = 0; i < ARRAY_SIZE(map); i++)
    {
        if (!lstrcmpiW(extension, map[i].extension))
        {
            *content = *map[i].content;
            *format = *map[i].format;
            return;
        }
    }
}

static HRESULT upload(wpd_device *device, const WCHAR *parent, const WCHAR *path, int depth)
{
    IPortableDeviceValues *values;
    IStream *stream = NULL;
    WIN32_FIND_DATAW find;
    WCHAR child[MAX_PATH], *created = NULL;
    const WCHAR *name = PathFindFileNameW(path);
    LARGE_INTEGER size;
    GUID content, format;
    HANDLE file, search;
    DWORD optimal = 0, read, attributes;
    BYTE *buffer;
    HRESULT hr;

    if (depth > 32)
        return S_OK;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(GetLastError());
    hr = CoCreateInstance(&CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceValues,
                          (void **)&values);
    if (FAILED(hr))
        return hr;
    IPortableDeviceValues_SetStringValue(values, &WPD_OBJECT_PARENT_ID, parent);
    IPortableDeviceValues_SetStringValue(values, &WPD_OBJECT_NAME, name);
    IPortableDeviceValues_SetStringValue(values, &WPD_OBJECT_ORIGINAL_FILE_NAME, name);
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        IPortableDeviceValues_SetGuidValue(values, &WPD_OBJECT_CONTENT_TYPE, &WPD_CONTENT_TYPE_FOLDER);
        hr = IPortableDeviceContent_CreateObjectWithPropertiesOnly(device->content, values, &created);
        IPortableDeviceValues_Release(values);
        if (FAILED(hr))
            return hr;
        lstrcpynW(child, path, ARRAY_SIZE(child));
        PathAppendW(child, L"*");
        search = FindFirstFileW(child, &find);
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!lstrcmpW(find.cFileName, L".") || !lstrcmpW(find.cFileName, L".."))
                    continue;
                lstrcpynW(child, path, ARRAY_SIZE(child));
                PathAppendW(child, find.cFileName);
                hr = upload(device, created, child, depth + 1);
            }
            while (SUCCEEDED(hr) && FindNextFileW(search, &find));
            FindClose(search);
        }
        CoTaskMemFree(created);
        return hr;
    }

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        IPortableDeviceValues_Release(values);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    size.QuadPart = 0;
    GetFileSizeEx(file, &size);
    file_format(name, &content, &format);
    IPortableDeviceValues_SetUnsignedLargeIntegerValue(values, &WPD_OBJECT_SIZE, size.QuadPart);
    IPortableDeviceValues_SetGuidValue(values, &WPD_OBJECT_CONTENT_TYPE, &content);
    IPortableDeviceValues_SetGuidValue(values, &WPD_OBJECT_FORMAT, &format);
    hr = IPortableDeviceContent_CreateObjectWithPropertiesAndData(device->content, values, &stream, &optimal, NULL);
    IPortableDeviceValues_Release(values);
    if (SUCCEEDED(hr))
    {
        if (!optimal)
            optimal = 0x40000;
        buffer = HeapAlloc(GetProcessHeap(), 0, optimal);
        hr = buffer ? S_OK : E_OUTOFMEMORY;
        while (SUCCEEDED(hr) && ReadFile(file, buffer, optimal, &read, NULL) && read)
        {
            ULONG written = 0;

            hr = IStream_Write(stream, buffer, read, &written);
            if (SUCCEEDED(hr) && written != read)
                hr = E_FAIL;
        }
        HeapFree(GetProcessHeap(), 0, buffer);
        if (SUCCEEDED(hr))
            hr = IStream_Commit(stream, STGC_DEFAULT);
        else
            IStream_Revert(stream);
        IStream_Release(stream);
    }
    CloseHandle(file);
    return hr;
}

static HRESULT WINAPI drop_Drop(IDropTarget *iface, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    wpd_drop *drop = impl_from_IDropTarget(iface);
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    WCHAR path[MAX_PATH];
    wpd_device *device;
    HDROP files;
    UINT count, i;
    HRESULT hr;

    drop->accept = FALSE;
    *effect = DROPEFFECT_NONE;
    hr = wpd_get_device(drop->folder->pnp_id, &device);
    if (FAILED(hr))
        return hr;
    hr = IDataObject_GetData(data, &format, &medium);
    if (FAILED(hr))
        return hr;
    files = GlobalLock(medium.hGlobal);
    count = files ? DragQueryFileW(files, 0xFFFFFFFF, NULL, 0) : 0;
    for (i = 0; i < count && SUCCEEDED(hr); i++)
    {
        if (DragQueryFileW(files, i, path, ARRAY_SIZE(path)))
            hr = upload(device, drop->folder->object_id, path, 0);
    }
    if (files)
        GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, drop->folder->pidl, NULL);
    if (SUCCEEDED(hr))
        *effect = DROPEFFECT_COPY;
    else
        WARN("upload to %s failed, hr 0x%08lx\n", debugstr_w(drop->folder->object_id), hr);
    return hr;
}

static const IDropTargetVtbl drop_vtbl =
{
    drop_QueryInterface,
    drop_AddRef,
    drop_Release,
    drop_DragEnter,
    drop_DragOver,
    drop_DragLeave,
    drop_Drop,
};

HRESULT wpd_drop_target_create(wpd_folder *folder, REFIID iid, void **out)
{
    wpd_drop *drop;
    HRESULT hr;

    *out = NULL;
    drop = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*drop));
    if (!drop)
        return E_OUTOFMEMORY;
    drop->IDropTarget_iface.lpVtbl = &drop_vtbl;
    drop->refcount = 1;
    drop->folder = folder;
    IShellFolder2_AddRef(&folder->IShellFolder2_iface);
    InterlockedIncrement(&wpdshext_locks);
    hr = IDropTarget_QueryInterface(&drop->IDropTarget_iface, iid, out);
    IDropTarget_Release(&drop->IDropTarget_iface);
    return hr;
}
