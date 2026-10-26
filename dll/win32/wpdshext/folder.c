/*
 * PROJECT:     LiberNT Portable Devices shell extension
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Portable Devices shell folders
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdshext.h"

#define ATTR_DEVICE  (SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_REMOVABLE | SFGAO_STORAGEANCESTOR | \
                      SFGAO_HASPROPSHEET | SFGAO_CANLINK)
#define ATTR_STORAGE (SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_REMOVABLE | SFGAO_STORAGEANCESTOR | \
                      SFGAO_HASPROPSHEET | SFGAO_STORAGE)
#define ATTR_FOLDER  (SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_STORAGEANCESTOR | SFGAO_DROPTARGET | \
                      SFGAO_HASPROPSHEET | SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_STORAGE | \
                      SFGAO_CANMOVE | SFGAO_CANCOPY)
#define ATTR_FILE    (SFGAO_STREAM | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET | SFGAO_CANDELETE | \
                      SFGAO_CANRENAME | SFGAO_CANMOVE | SFGAO_CANCOPY)

enum
{
    VALUE_NAME,
    VALUE_TYPE,
    VALUE_SIZE,
    VALUE_FREE,
    VALUE_PERCENT,
    VALUE_NONE,
    VALUE_MODIFIED,
    VALUE_CREATED,
    VALUE_PROTECTED,
    VALUE_SUPPORTED,
    VALUE_TRACK,
    VALUE_TITLE,
    VALUE_ARTIST,
    VALUE_ALBUM,
    VALUE_YEAR,
    VALUE_GENRE,
    VALUE_DURATION,
    VALUE_AUTHORED,
    VALUE_DIMENSIONS,
};

typedef struct wpd_column
{
    UINT title;
    const PROPERTYKEY *key;
    SHCOLSTATEF state;
    SHCOLSTATEF music_state;
    int fmt;
    BYTE value;
} wpd_column;

static const PROPERTYKEY scid_supported =
    { { 0x0025cc14, 0x5009, 0x4803, { 0x8b, 0x50, 0xf1, 0x0f, 0x68, 0xb7, 0x6a, 0x36 } }, 3 };

#define STR_ON   (SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT)
#define INT_ON   (SHCOLSTATE_TYPE_INT | SHCOLSTATE_ONBYDEFAULT)
#define STR_SLOW (SHCOLSTATE_TYPE_STR | SHCOLSTATE_SLOW)
#define INT_SLOW (SHCOLSTATE_TYPE_INT | SHCOLSTATE_SLOW)

static const wpd_column root_columns[] =
{
    { IDS_COLUMN_NAME, &PKEY_ItemNameDisplay, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_NAME },
    { IDS_COLUMN_TYPE, &PKEY_ItemTypeText, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_TYPE },
};

static const wpd_column storage_columns[] =
{
    { IDS_COLUMN_NAME, &PKEY_ItemNameDisplay, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_NAME },
    { IDS_COLUMN_TYPE, &PKEY_ItemTypeText, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_TYPE },
    { IDS_COLUMN_TOTAL, &PKEY_Size, INT_ON, INT_ON, LVCFMT_RIGHT, VALUE_SIZE },
    { IDS_COLUMN_FREE, &PKEY_FreeSpace, INT_ON | SHCOLSTATE_SLOW, INT_ON | SHCOLSTATE_SLOW, LVCFMT_RIGHT, VALUE_FREE },
    { IDS_COLUMN_PERCENT_FULL, &PKEY_PercentFull, INT_SLOW, INT_SLOW, LVCFMT_LEFT, VALUE_PERCENT },
    { IDS_COLUMN_AVAILABLE, &PKEY_Computer_DecoratedFreeSpace, STR_SLOW | SHCOLSTATE_HIDDEN,
      STR_SLOW | SHCOLSTATE_HIDDEN, LVCFMT_LEFT, VALUE_NONE },
};

static const wpd_column object_columns[] =
{
    { IDS_COLUMN_NAME, &PKEY_ItemNameDisplay, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_NAME },
    { IDS_COLUMN_TYPE, &PKEY_ItemTypeText, STR_ON, STR_ON, LVCFMT_LEFT, VALUE_TYPE },
    { IDS_COLUMN_SIZE, &PKEY_Size, INT_ON, INT_ON, LVCFMT_RIGHT, VALUE_SIZE },
    { IDS_COLUMN_MODIFIED, &PKEY_DateModified, STR_ON, SHCOLSTATE_TYPE_STR, LVCFMT_LEFT, VALUE_MODIFIED },
    { IDS_COLUMN_CREATED, &PKEY_DateCreated, STR_ON, SHCOLSTATE_TYPE_STR, LVCFMT_LEFT, VALUE_CREATED },
    { IDS_COLUMN_PROTECTED, &PKEY_DRM_IsProtected, SHCOLSTATE_TYPE_STR, SHCOLSTATE_TYPE_STR, LVCFMT_LEFT, VALUE_PROTECTED },
    { IDS_COLUMN_SUPPORTED, &scid_supported, SHCOLSTATE_TYPE_STR, SHCOLSTATE_TYPE_STR, LVCFMT_LEFT, VALUE_SUPPORTED },
    { IDS_COLUMN_TRACK, &PKEY_Music_TrackNumber, INT_SLOW, INT_SLOW | SHCOLSTATE_ONBYDEFAULT, LVCFMT_RIGHT, VALUE_TRACK },
    { IDS_COLUMN_TITLE, &PKEY_Title, STR_SLOW, STR_SLOW, LVCFMT_LEFT, VALUE_TITLE },
    { IDS_COLUMN_ARTIST, &PKEY_Music_Artist, STR_SLOW, STR_SLOW | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, VALUE_ARTIST },
    { IDS_COLUMN_ALBUM, &PKEY_Music_AlbumTitle, STR_SLOW, STR_SLOW | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, VALUE_ALBUM },
    { IDS_COLUMN_YEAR, &PKEY_Media_Year, INT_SLOW, INT_SLOW | SHCOLSTATE_ONBYDEFAULT, LVCFMT_RIGHT, VALUE_YEAR },
    { IDS_COLUMN_GENRE, &PKEY_Music_Genre, STR_SLOW, STR_SLOW, LVCFMT_LEFT, VALUE_GENRE },
    { IDS_COLUMN_DURATION, &WPD_MEDIA_DURATION, SHCOLSTATE_TYPE_STR, STR_ON, LVCFMT_LEFT, VALUE_DURATION },
    { IDS_COLUMN_DATE_AUTHORED, &WPD_OBJECT_DATE_AUTHORED, SHCOLSTATE_TYPE_STR, SHCOLSTATE_TYPE_STR, LVCFMT_LEFT, VALUE_AUTHORED },
    { IDS_COLUMN_DIMENSIONS, &PKEY_Image_Dimensions, STR_SLOW, STR_SLOW, LVCFMT_LEFT, VALUE_DIMENSIONS },
};

static const wpd_column *folder_columns(const wpd_folder *folder, UINT *count)
{
    if (!folder->kind)
    {
        *count = ARRAY_SIZE(root_columns);
        return root_columns;
    }
    if (folder->kind == WPD_KIND_DEVICE)
    {
        *count = ARRAY_SIZE(storage_columns);
        return storage_columns;
    }
    *count = ARRAY_SIZE(object_columns);
    return object_columns;
}

static const wpd_column *folder_column(const wpd_folder *folder, UINT index)
{
    UINT count;
    const wpd_column *columns = folder_columns(folder, &count);

    return index < count ? &columns[index] : NULL;
}

static const IShellFolder2Vtbl folder_vtbl;
static const IPersistFolder2Vtbl persist_vtbl;
static const IDelegateFolderVtbl delegate_vtbl;

static inline wpd_folder *impl_from_IShellFolder2(IShellFolder2 *iface)
{
    return CONTAINING_RECORD(iface, wpd_folder, IShellFolder2_iface);
}

static inline wpd_folder *impl_from_IPersistFolder2(IPersistFolder2 *iface)
{
    return CONTAINING_RECORD(iface, wpd_folder, IPersistFolder2_iface);
}

static inline wpd_folder *impl_from_IDelegateFolder(IDelegateFolder *iface)
{
    return CONTAINING_RECORD(iface, wpd_folder, IDelegateFolder_iface);
}

wpd_folder *wpd_folder_from_shellfolder(IShellFolder *iface)
{
    if (!iface || iface->lpVtbl != (const IShellFolderVtbl *)&folder_vtbl)
        return NULL;
    return impl_from_IShellFolder2((IShellFolder2 *)iface);
}

static WCHAR *strdup_co(const WCHAR *string)
{
    WCHAR *copy;
    SIZE_T size;

    if (!string)
        return NULL;
    size = (lstrlenW(string) + 1) * sizeof(WCHAR);
    copy = CoTaskMemAlloc(size);
    if (copy)
        memcpy(copy, string, size);
    return copy;
}

static wpd_folder *folder_alloc(REFCLSID clsid)
{
    wpd_folder *folder;

    folder = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*folder));
    if (!folder)
        return NULL;
    folder->IShellFolder2_iface.lpVtbl = &folder_vtbl;
    folder->IPersistFolder2_iface.lpVtbl = &persist_vtbl;
    folder->IDelegateFolder_iface.lpVtbl = &delegate_vtbl;
    folder->refcount = 1;
    folder->clsid = *clsid;
    InterlockedIncrement(&wpdshext_locks);
    return folder;
}

HRESULT wpd_folder_create(REFCLSID clsid, REFIID iid, void **out)
{
    wpd_folder *folder = folder_alloc(clsid);
    HRESULT hr;

    *out = NULL;
    if (!folder)
        return E_OUTOFMEMORY;
    hr = IShellFolder2_QueryInterface(&folder->IShellFolder2_iface, iid, out);
    IShellFolder2_Release(&folder->IShellFolder2_iface);
    return hr;
}

HRESULT wpd_folder_child(wpd_folder *parent, PCUITEMID_CHILD pidl, wpd_folder **out)
{
    const wpd_item *item = wpd_item_from_pidl(pidl);
    wpd_folder *child;
    LPITEMIDLIST first;

    *out = NULL;
    if (!item || !wpd_item_is_container(item))
        return E_INVALIDARG;
    child = folder_alloc(&parent->clsid);
    if (!child)
        return E_OUTOFMEMORY;
    child->kind = item->kind;
    child->device_type = item->kind == WPD_KIND_DEVICE ? item->type : parent->device_type;
    if (item->kind == WPD_KIND_DEVICE)
    {
        child->pnp_id = strdup_co(wpd_item_string(item, WPD_STR_ID));
        child->object_id = strdup_co(WPD_DEVICE_OBJECT_ID);
    }
    else
    {
        child->pnp_id = strdup_co(parent->pnp_id);
        child->object_id = strdup_co(wpd_item_string(item, WPD_STR_ID));
    }
    first = ILCloneFirst((LPCITEMIDLIST)pidl);
    child->pidl = first ? ILCombine(parent->pidl, first) : NULL;
    ILFree(first);
    if (!child->pnp_id || !child->object_id || !child->pidl)
    {
        IShellFolder2_Release(&child->IShellFolder2_iface);
        return E_OUTOFMEMORY;
    }
    *out = child;
    return S_OK;
}

static BOOL hide_extension(void)
{
    DWORD value = 1, size = sizeof(value);

    if (SHGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                    L"HideFileExt", NULL, &value, &size))
    {
        value = 1;
    }
    return value != 0;
}

static BOOL known_extension(const WCHAR *extension)
{
    HKEY key;

    if (!extension || !extension[0] || extension[1] == 0)
        return FALSE;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, extension, 0, KEY_QUERY_VALUE, &key))
        return FALSE;
    RegCloseKey(key);
    return TRUE;
}

void wpd_display_name(const wpd_item *item, WCHAR *buffer, DWORD chars, BOOL editing)
{
    const WCHAR *name = wpd_item_string(item, WPD_STR_NAME), *extension, *separator;
    DWORD length;

    separator = StrRChrW(name, NULL, L'\\');
    if (separator)
        name = separator + 1;
    lstrcpynW(buffer, name, chars);
    if (item->kind != WPD_KIND_FILE || !hide_extension())
        return;
    extension = PathFindExtensionW(name);
    if (!extension || extension == name || !known_extension(extension))
        return;
    length = (DWORD)(extension - name);
    if (length < chars)
        buffer[length] = 0;
}

static HRESULT WINAPI folder_QueryInterface(IShellFolder2 *iface, REFIID iid, void **out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);

    if (!out)
        return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IShellFolder) || IsEqualIID(iid, &IID_IShellFolder2))
        *out = &folder->IShellFolder2_iface;
    else if (IsEqualIID(iid, &IID_IPersist) || IsEqualIID(iid, &IID_IPersistFolder) ||
             IsEqualIID(iid, &IID_IPersistFolder2))
        *out = &folder->IPersistFolder2_iface;
    else if (IsEqualIID(iid, &IID_IDelegateFolder) && !folder->kind)
        *out = &folder->IDelegateFolder_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI folder_AddRef(IShellFolder2 *iface)
{
    return InterlockedIncrement(&impl_from_IShellFolder2(iface)->refcount);
}

static ULONG WINAPI folder_Release(IShellFolder2 *iface)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    ULONG refcount = InterlockedDecrement(&folder->refcount);

    if (!refcount)
    {
        if (folder->malloc)
            IMalloc_Release(folder->malloc);
        ILFree(folder->pidl);
        CoTaskMemFree(folder->pnp_id);
        CoTaskMemFree(folder->object_id);
        HeapFree(GetProcessHeap(), 0, folder);
        InterlockedDecrement(&wpdshext_locks);
    }
    return refcount;
}

static HRESULT find_child(wpd_folder *folder, const WCHAR *name, DWORD length, PITEMID_CHILD *out)
{
    PITEMID_CHILD *items;
    DWORD count, i;
    HRESULT hr;
    WCHAR display[MAX_PATH];

    *out = NULL;
    hr = wpd_enum_children(folder, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &items, &count);
    if (FAILED(hr))
        return hr;
    for (i = 0; i < count && !*out; i++)
    {
        const wpd_item *item = wpd_item_from_pidl(items[i]);
        const WCHAR *parse = wpd_item_string(item, WPD_STR_PARSE);

        wpd_display_name(item, display, ARRAY_SIZE(display), FALSE);
        if ((lstrlenW(parse) == (int)length && !StrCmpNIW(parse, name, length)) ||
            (lstrlenW(wpd_item_string(item, WPD_STR_NAME)) == (int)length &&
             !StrCmpNIW(wpd_item_string(item, WPD_STR_NAME), name, length)) ||
            (lstrlenW(display) == (int)length && !StrCmpNIW(display, name, length)))
        {
            *out = items[i];
            items[i] = NULL;
        }
    }
    wpd_free_items(items, count);
    return *out ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

static HRESULT WINAPI folder_ParseDisplayName(IShellFolder2 *iface, HWND hwnd, LPBC ctx, LPOLESTR name,
                                              ULONG *eaten, PIDLIST_RELATIVE *pidl, ULONG *attributes)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface), *child;
    PITEMID_CHILD item;
    PIDLIST_RELATIVE rest = NULL;
    const WCHAR *end;
    DWORD length;
    HRESULT hr;

    if (!name || !pidl)
        return E_INVALIDARG;
    *pidl = NULL;
    if (!folder->kind && !wcsncmp(name, L"\\\\?\\", 4))
    {
        end = wcschr(name, L'}');
        end = end ? end + 1 : name + lstrlenW(name);
    }
    else
    {
        end = wcschr(name, L'\\');
        if (!end)
            end = name + lstrlenW(name);
    }
    length = (DWORD)(end - name);
    if (!length)
        return E_INVALIDARG;
    hr = find_child(folder, name, length, &item);
    if (FAILED(hr))
        return hr;
    if (*end == L'\\' && end[1])
    {
        hr = wpd_folder_child(folder, item, &child);
        if (SUCCEEDED(hr))
        {
            hr = IShellFolder2_ParseDisplayName(&child->IShellFolder2_iface, hwnd, ctx, (LPOLESTR)end + 1, NULL,
                                                &rest, attributes);
            IShellFolder2_Release(&child->IShellFolder2_iface);
        }
        if (SUCCEEDED(hr))
        {
            *pidl = ILCombine((LPCITEMIDLIST)item, rest);
            ILFree(rest);
            hr = *pidl ? S_OK : E_OUTOFMEMORY;
        }
        ILFree((LPITEMIDLIST)item);
    }
    else
    {
        *pidl = (PIDLIST_RELATIVE)item;
        if (attributes && *attributes)
            IShellFolder2_GetAttributesOf(iface, 1, (PCUITEMID_CHILD_ARRAY)pidl, attributes);
    }
    if (eaten && SUCCEEDED(hr))
        *eaten = lstrlenW(name);
    return hr;
}

static HRESULT WINAPI folder_EnumObjects(IShellFolder2 *iface, HWND hwnd, SHCONTF flags, IEnumIDList **out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    PITEMID_CHILD *items;
    DWORD count;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    hr = wpd_enum_children(folder, flags, &items, &count);
    if (FAILED(hr))
        return hr;
    return wpd_enum_create(items, count, out);
}

static HRESULT WINAPI folder_BindToObject(IShellFolder2 *iface, PCUIDLIST_RELATIVE pidl, LPBC ctx, REFIID iid, void **out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface), *child;
    PCUIDLIST_RELATIVE rest;
    HRESULT hr;

    if (!pidl || !out)
        return E_INVALIDARG;
    *out = NULL;
    hr = wpd_folder_child(folder, (PCUITEMID_CHILD)pidl, &child);
    if (FAILED(hr))
        return hr;
    rest = (PCUIDLIST_RELATIVE)ILGetNext((LPCITEMIDLIST)pidl);
    if (rest && rest->mkid.cb)
        hr = IShellFolder2_BindToObject(&child->IShellFolder2_iface, rest, ctx, iid, out);
    else
        hr = IShellFolder2_QueryInterface(&child->IShellFolder2_iface, iid, out);
    IShellFolder2_Release(&child->IShellFolder2_iface);
    return hr;
}

static HRESULT WINAPI folder_BindToStorage(IShellFolder2 *iface, PCUIDLIST_RELATIVE pidl, LPBC ctx, REFIID iid, void **out)
{
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static int compare_values(ULONGLONG a, ULONGLONG b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}

static HRESULT WINAPI folder_CompareIDs(IShellFolder2 *iface, LPARAM param, PCUIDLIST_RELATIVE pidl1,
                                        PCUIDLIST_RELATIVE pidl2)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface), *child;
    const wpd_item *item1 = wpd_item_from_pidl(pidl1), *item2 = wpd_item_from_pidl(pidl2);
    const wpd_column *column;
    PCUIDLIST_RELATIVE next1, next2;
    WCHAR type1[MAX_PATH], type2[MAX_PATH];
    int result = 0;
    HRESULT hr;

    if (!item1 || !item2)
        return E_INVALIDARG;
    if (wpd_item_is_container(item1) != wpd_item_is_container(item2))
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(wpd_item_is_container(item1) ? -1 : 1));
    column = folder_column(folder, LOWORD(param));
    switch (column ? column->value : VALUE_NAME)
    {
        case VALUE_SIZE:
            result = item1->kind == WPD_KIND_STORAGE ? compare_values(item1->capacity, item2->capacity) :
                     compare_values(item1->size, item2->size);
            break;
        case VALUE_FREE:
            result = compare_values(item1->free, item2->free);
            break;
        case VALUE_PERCENT:
            result = compare_values(item1->capacity - item1->free, item2->capacity - item2->free);
            break;
        case VALUE_CREATED:
            result = CompareFileTime(&item1->created, &item2->created);
            break;
        case VALUE_MODIFIED:
            result = CompareFileTime(&item1->modified, &item2->modified);
            break;
        case VALUE_TRACK:
            result = compare_values(item1->track, item2->track);
            break;
        case VALUE_DURATION:
            result = compare_values(item1->duration, item2->duration);
            break;
        case VALUE_AUTHORED:
            result = CompareFileTime(&item1->authored, &item2->authored);
            break;
        case VALUE_GENRE:
            result = lstrcmpiW(wpd_item_string(item1, WPD_STR_GENRE), wpd_item_string(item2, WPD_STR_GENRE));
            break;
        case VALUE_ARTIST:
            result = lstrcmpiW(wpd_item_string(item1, WPD_STR_ARTIST), wpd_item_string(item2, WPD_STR_ARTIST));
            break;
        case VALUE_ALBUM:
            result = lstrcmpiW(wpd_item_string(item1, WPD_STR_ALBUM), wpd_item_string(item2, WPD_STR_ALBUM));
            break;
        case VALUE_TYPE:
            lstrcpynW(type1, PathFindExtensionW(wpd_item_string(item1, WPD_STR_FILE)), ARRAY_SIZE(type1));
            lstrcpynW(type2, PathFindExtensionW(wpd_item_string(item2, WPD_STR_FILE)), ARRAY_SIZE(type2));
            result = lstrcmpiW(type1, type2);
            break;
    }
    if (!result)
        result = StrCmpLogicalW(wpd_item_string(item1, WPD_STR_NAME), wpd_item_string(item2, WPD_STR_NAME));
    if (!result)
        result = lstrcmpW(wpd_item_string(item1, WPD_STR_PARSE), wpd_item_string(item2, WPD_STR_PARSE));
    if (result)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(result < 0 ? -1 : 1));
    next1 = (PCUIDLIST_RELATIVE)ILGetNext((LPCITEMIDLIST)pidl1);
    next2 = (PCUIDLIST_RELATIVE)ILGetNext((LPCITEMIDLIST)pidl2);
    if ((!next1 || !next1->mkid.cb) && (!next2 || !next2->mkid.cb))
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    if (!next1 || !next1->mkid.cb)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)-1);
    if (!next2 || !next2->mkid.cb)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
    hr = wpd_folder_child(folder, (PCUITEMID_CHILD)pidl1, &child);
    if (FAILED(hr))
        return hr;
    hr = IShellFolder2_CompareIDs(&child->IShellFolder2_iface, param, next1, next2);
    IShellFolder2_Release(&child->IShellFolder2_iface);
    return hr;
}

static HRESULT WINAPI folder_CreateViewObject(IShellFolder2 *iface, HWND hwnd, REFIID iid, void **out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    SFV_CREATE create;

    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IShellView))
    {
        memset(&create, 0, sizeof(create));
        create.cbSize = sizeof(create);
        create.pshf = (IShellFolder *)&folder->IShellFolder2_iface;
        return SHCreateShellFolderView(&create, (IShellView **)out);
    }
    if (IsEqualIID(iid, &IID_IDropTarget) && (folder->kind == WPD_KIND_STORAGE || folder->kind == WPD_KIND_FOLDER))
        return wpd_drop_target_create(folder, iid, out);
    if (IsEqualIID(iid, &IID_IContextMenu))
        return CDefFolderMenu_Create2(folder->pidl, hwnd, 0, NULL, (IShellFolder *)&folder->IShellFolder2_iface,
                                      wpd_menu_callback, 0, NULL, (IContextMenu **)out);
    return E_NOINTERFACE;
}

static HRESULT WINAPI folder_GetAttributesOf(IShellFolder2 *iface, UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF *flags)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    DWORD attributes = ~0u;
    UINT i;

    if (!flags)
        return E_INVALIDARG;
    if (!count)
    {
        *flags &= SFGAO_FOLDER | SFGAO_HASSUBFOLDER | (folder->kind >= WPD_KIND_STORAGE ? SFGAO_DROPTARGET : 0);
        return S_OK;
    }
    for (i = 0; i < count; i++)
    {
        const wpd_item *item = wpd_item_from_pidl(items[i]);
        DWORD value;

        if (!item)
            return E_INVALIDARG;
        switch (item->kind)
        {
            case WPD_KIND_DEVICE:
                value = ATTR_DEVICE | ((item->flags & WPD_ITEM_CAN_RENAME) ? SFGAO_CANRENAME : 0);
                break;
            case WPD_KIND_STORAGE:
                value = ATTR_STORAGE;
                break;
            case WPD_KIND_FOLDER:
                value = ATTR_FOLDER;
                break;
            default:
                value = ATTR_FILE;
                break;
        }
        attributes &= value;
    }
    *flags &= attributes;
    return S_OK;
}

static HRESULT WINAPI folder_GetUIObjectOf(IShellFolder2 *iface, HWND hwnd, UINT count, PCUITEMID_CHILD_ARRAY items,
                                           REFIID iid, UINT *reserved, void **out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface), *child;
    const wpd_item *item;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (!count || !items)
        return E_INVALIDARG;
    item = wpd_item_from_pidl(items[0]);
    if (!item)
        return E_INVALIDARG;
    if ((IsEqualIID(iid, &IID_IExtractIconW) || IsEqualIID(iid, &IID_IExtractIconA)) && count == 1)
        return wpd_extract_icon_create(item, iid, out);
    if (IsEqualIID(iid, &IID_IContextMenu))
        return CDefFolderMenu_Create2(folder->pidl, hwnd, count, items, (IShellFolder *)&folder->IShellFolder2_iface,
                                      wpd_menu_callback, 0, NULL, (IContextMenu **)out);
    if (IsEqualIID(iid, &IID_IDataObject))
        return wpd_data_object_create(folder, count, items, iid, out);
    if (IsEqualIID(iid, &IID_IDropTarget) && count == 1 && wpd_item_is_container(item))
    {
        hr = wpd_folder_child(folder, items[0], &child);
        if (FAILED(hr))
            return hr;
        hr = IShellFolder2_CreateViewObject(&child->IShellFolder2_iface, hwnd, iid, out);
        IShellFolder2_Release(&child->IShellFolder2_iface);
        return hr;
    }
    return E_NOINTERFACE;
}

static HRESULT absolute_parse_name(wpd_folder *folder, const WCHAR *relative, STRRET *ret)
{
    WCHAR *parent = NULL, *result;
    DWORD length;
    HRESULT hr;

    hr = SHGetNameFromIDList(folder->pidl, SIGDN_DESKTOPABSOLUTEPARSING, &parent);
    if (FAILED(hr))
        return hr;
    length = lstrlenW(parent) + 1 + lstrlenW(relative) + 1;
    result = CoTaskMemAlloc(length * sizeof(WCHAR));
    if (result)
    {
        lstrcpyW(result, parent);
        lstrcatW(result, L"\\");
        lstrcatW(result, relative);
    }
    CoTaskMemFree(parent);
    if (!result)
        return E_OUTOFMEMORY;
    ret->uType = STRRET_WSTR;
    ret->pOleStr = result;
    return S_OK;
}

static HRESULT WINAPI folder_GetDisplayNameOf(IShellFolder2 *iface, PCUITEMID_CHILD pidl, SHGDNF flags, STRRET *ret)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface), *child;
    const wpd_item *item = wpd_item_from_pidl(pidl);
    PCUIDLIST_RELATIVE rest;
    WCHAR name[MAX_PATH];
    HRESULT hr;

    if (!item || !ret)
        return E_INVALIDARG;
    rest = (PCUIDLIST_RELATIVE)ILGetNext((LPCITEMIDLIST)pidl);
    if (rest && rest->mkid.cb)
    {
        hr = wpd_folder_child(folder, pidl, &child);
        if (FAILED(hr))
            return hr;
        hr = IShellFolder2_GetDisplayNameOf(&child->IShellFolder2_iface, (PCUITEMID_CHILD)rest, flags, ret);
        IShellFolder2_Release(&child->IShellFolder2_iface);
        return hr;
    }
    if (flags & SHGDN_FORPARSING)
    {
        if (flags & SHGDN_INFOLDER)
        {
            ret->uType = STRRET_WSTR;
            ret->pOleStr = strdup_co(wpd_item_string(item, WPD_STR_PARSE));
            return ret->pOleStr ? S_OK : E_OUTOFMEMORY;
        }
        return absolute_parse_name(folder, wpd_item_string(item, WPD_STR_PARSE), ret);
    }
    wpd_display_name(item, name, ARRAY_SIZE(name), (flags & SHGDN_FOREDITING) != 0);
    ret->uType = STRRET_WSTR;
    ret->pOleStr = strdup_co(name);
    return ret->pOleStr ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI folder_SetNameOf(IShellFolder2 *iface, HWND hwnd, PCUITEMID_CHILD pidl, LPCOLESTR name,
                                       SHGDNF flags, PITEMID_CHILD *out)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    const wpd_item *item = wpd_item_from_pidl(pidl);
    IPortableDeviceValues *values = NULL, *results = NULL;
    PIDLIST_ABSOLUTE old_pidl, new_pidl;
    const WCHAR *extension, *current;
    WCHAR full[MAX_PATH];
    wpd_device *device;
    PITEMID_CHILD item_new = NULL;
    HRESULT hr;

    if (out)
        *out = NULL;
    if (!item || !name || !name[0])
        return E_INVALIDARG;
    if (!(item->flags & WPD_ITEM_CAN_RENAME))
        return E_ACCESSDENIED;
    hr = wpd_get_device(item->kind == WPD_KIND_DEVICE ? wpd_item_string(item, WPD_STR_ID) : folder->pnp_id, &device);
    if (FAILED(hr))
        return hr;
    hr = CoCreateInstance(&CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER, &IID_IPortableDeviceValues,
                          (void **)&values);
    if (FAILED(hr))
        return hr;
    lstrcpynW(full, name, ARRAY_SIZE(full));
    if (item->kind == WPD_KIND_FILE)
    {
        current = wpd_item_string(item, WPD_STR_FILE);
        extension = PathFindExtensionW(current);
        if (*extension && lstrcmpiW(PathFindExtensionW(name), extension) && hide_extension() && known_extension(extension))
            StrCatBuffW(full, extension, ARRAY_SIZE(full));
    }
    if (item->kind == WPD_KIND_DEVICE)
    {
        IPortableDeviceValues_SetStringValue(values, &WPD_DEVICE_FRIENDLY_NAME, full);
        hr = IPortableDeviceProperties_SetValues(device->properties, WPD_DEVICE_OBJECT_ID, values, &results);
    }
    else
    {
        IPortableDeviceValues_SetStringValue(values, &WPD_OBJECT_ORIGINAL_FILE_NAME, full);
        hr = IPortableDeviceProperties_SetValues(device->properties, wpd_item_string(item, WPD_STR_ID), values, &results);
    }
    IPortableDeviceValues_Release(values);
    if (results)
    {
        HRESULT item_hr = S_OK;

        IPortableDeviceValues_GetErrorValue(results, item->kind == WPD_KIND_DEVICE ? &WPD_DEVICE_FRIENDLY_NAME :
                                            &WPD_OBJECT_ORIGINAL_FILE_NAME, &item_hr);
        IPortableDeviceValues_Release(results);
        if (FAILED(item_hr))
            hr = item_hr;
    }
    if (FAILED(hr))
        return hr;
    if (item->kind == WPD_KIND_DEVICE)
        hr = wpd_device_item(folder, wpd_item_string(item, WPD_STR_ID), &item_new);
    else
        hr = wpd_object_item(folder, device, wpd_item_string(item, WPD_STR_ID), &item_new);
    if (hr != S_OK)
        return FAILED(hr) ? hr : E_FAIL;
    old_pidl = ILCombine(folder->pidl, (LPCITEMIDLIST)pidl);
    new_pidl = ILCombine(folder->pidl, (LPCITEMIDLIST)item_new);
    if (old_pidl && new_pidl)
        SHChangeNotify(wpd_item_is_container(item) ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM, SHCNF_IDLIST,
                       old_pidl, new_pidl);
    ILFree(old_pidl);
    ILFree(new_pidl);
    if (out)
        *out = item_new;
    else
        ILFree((LPITEMIDLIST)item_new);
    return S_OK;
}

static HRESULT WINAPI folder_GetDefaultSearchGUID(IShellFolder2 *iface, GUID *guid)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_EnumSearches(IShellFolder2 *iface, IEnumExtraSearch **out)
{
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_GetDefaultColumn(IShellFolder2 *iface, DWORD reserved, ULONG *sort, ULONG *display)
{
    if (sort)
        *sort = 0;
    if (display)
        *display = 0;
    return S_OK;
}

static HRESULT WINAPI folder_GetDefaultColumnState(IShellFolder2 *iface, UINT column, SHCOLSTATEF *flags)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    const wpd_column *entry = folder_column(folder, column);

    if (!flags || !entry)
        return E_INVALIDARG;
    *flags = folder->device_type == WPD_DEVICE_TYPE_MEDIA_PLAYER ? entry->music_state : entry->state;
    return S_OK;
}

static HRESULT WINAPI folder_GetDetailsEx(IShellFolder2 *iface, PCUITEMID_CHILD pidl, const SHCOLUMNID *column, VARIANT *value)
{
    return E_NOTIMPL;
}

static void type_name(const wpd_item *item, WCHAR *buffer, DWORD chars)
{
    static const UINT types[] = { IDS_TYPE_GENERIC, IDS_TYPE_CAMERA, IDS_TYPE_MEDIA_PLAYER, IDS_TYPE_PHONE };
    SHFILEINFOW info;

    buffer[0] = 0;
    switch (item->kind)
    {
        case WPD_KIND_DEVICE:
            LoadStringW(wpdshext_instance, item->type < ARRAY_SIZE(types) ? types[item->type] : IDS_TYPE_GENERIC,
                        buffer, chars);
            break;
        case WPD_KIND_STORAGE:
            lstrcpynW(buffer, wpd_item_string(item, WPD_STR_EXTRA), chars);
            break;
        default:
            memset(&info, 0, sizeof(info));
            if (SHGetFileInfoW(wpd_item_string(item, WPD_STR_FILE),
                               item->kind == WPD_KIND_FOLDER ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                               &info, sizeof(info), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME))
            {
                lstrcpynW(buffer, info.szTypeName, chars);
            }
            break;
    }
}

static void format_time(const FILETIME *time, WCHAR *buffer, DWORD chars)
{
    FILETIME local;
    SYSTEMTIME st;
    int length;

    buffer[0] = 0;
    if (!time->dwLowDateTime && !time->dwHighDateTime)
        return;
    if (!FileTimeToLocalFileTime(time, &local) || !FileTimeToSystemTime(&local, &st))
        return;
    length = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, buffer, chars);
    if (length > 0 && (DWORD)length < chars)
    {
        buffer[length - 1] = L' ';
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, buffer + length, chars - length);
    }
}

static void title_text(const wpd_item *item, WCHAR *buffer, DWORD chars)
{
    const WCHAR *name = wpd_item_string(item, WPD_STR_NAME), *extension = PathFindExtensionW(name);
    DWORD length = (DWORD)(extension - name);

    lstrcpynW(buffer, name, chars);
    if (extension != name && length < chars)
        buffer[length] = 0;
}

static HRESULT detail_text(const wpd_column *column, const wpd_item *item, WCHAR *text, DWORD chars)
{
    static const HRESULT missing = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ULONGLONG seconds;

    text[0] = 0;
    switch (column->value)
    {
        case VALUE_NAME:
            wpd_display_name(item, text, chars, FALSE);
            break;
        case VALUE_TYPE:
            type_name(item, text, chars);
            break;
        case VALUE_SIZE:
            if (item->kind == WPD_KIND_STORAGE)
                StrFormatByteSizeW(item->capacity, text, chars);
            else if (item->kind == WPD_KIND_FILE)
                StrFormatByteSizeW(item->size, text, chars);
            break;
        case VALUE_FREE:
            if (item->kind == WPD_KIND_STORAGE)
                StrFormatByteSizeW(item->free, text, chars);
            break;
        case VALUE_PERCENT:
            if (item->kind == WPD_KIND_STORAGE && item->capacity && item->free <= item->capacity)
                wsprintfW(text, L"%u%%", (UINT)((item->capacity - item->free) * 100 / item->capacity));
            break;
        case VALUE_MODIFIED:
            format_time(&item->modified, text, chars);
            break;
        case VALUE_CREATED:
            format_time(&item->created, text, chars);
            break;
        case VALUE_PROTECTED:
            if (item->flags & WPD_ITEM_PROTECTED)
                LoadStringW(wpdshext_instance, IDS_YES, text, chars);
            break;
        case VALUE_SUPPORTED:
            if (item->kind == WPD_KIND_FILE)
                LoadStringW(wpdshext_instance, (item->flags & WPD_ITEM_UNSPECIFIED_FORMAT) ? IDS_NO : IDS_YES, text, chars);
            break;
        case VALUE_TRACK:
            if (!(item->flags & WPD_ITEM_HAS_TRACK))
                return missing;
            wsprintfW(text, L"%lu", item->track);
            break;
        case VALUE_TITLE:
            if (item->kind != WPD_KIND_FILE || !(item->flags & WPD_ITEM_MEDIA))
                return missing;
            title_text(item, text, chars);
            break;
        case VALUE_ARTIST:
            if (!(item->flags & WPD_ITEM_HAS_ARTIST))
                return missing;
            lstrcpynW(text, wpd_item_string(item, WPD_STR_ARTIST), chars);
            break;
        case VALUE_ALBUM:
            if (!(item->flags & WPD_ITEM_HAS_ALBUM))
                return missing;
            lstrcpynW(text, wpd_item_string(item, WPD_STR_ALBUM), chars);
            break;
        case VALUE_GENRE:
            if (!(item->flags & WPD_ITEM_HAS_GENRE))
                return missing;
            lstrcpynW(text, wpd_item_string(item, WPD_STR_GENRE), chars);
            break;
        case VALUE_DURATION:
            if (!(item->flags & WPD_ITEM_HAS_DURATION))
                return missing;
            seconds = item->duration / 1000;
            wsprintfW(text, L"%02u:%02u:%02u", (UINT)(seconds / 3600), (UINT)(seconds / 60 % 60), (UINT)(seconds % 60));
            break;
        case VALUE_AUTHORED:
            if (!(item->flags & WPD_ITEM_HAS_AUTHORED))
                return missing;
            format_time(&item->authored, text, chars);
            break;
        case VALUE_DIMENSIONS:
            if (!(item->flags & WPD_ITEM_HAS_DIMENSIONS))
                return missing;
            wsprintfW(text, L"%lu X %lu", item->width, item->height);
            break;
        case VALUE_YEAR:
            return missing;
    }
    return S_OK;
}

static HRESULT WINAPI folder_GetDetailsOf(IShellFolder2 *iface, PCUITEMID_CHILD pidl, UINT column, SHELLDETAILS *details)
{
    wpd_folder *folder = impl_from_IShellFolder2(iface);
    const wpd_column *entry = folder_column(folder, column);
    const wpd_item *item;
    WCHAR text[MAX_PATH];
    HRESULT hr;

    if (!details)
        return E_INVALIDARG;
    if (!entry)
        return !folder->kind && pidl ? E_FAIL : E_INVALIDARG;
    text[0] = 0;
    if (!pidl)
    {
        details->fmt = entry->fmt;
        details->cxChar = entry->value == VALUE_NAME ? 25 : entry->value == VALUE_TYPE ? 18 : 15;
        LoadStringW(wpdshext_instance, entry->title, text, ARRAY_SIZE(text));
    }
    else
    {
        item = wpd_item_from_pidl(pidl);
        if (!item)
            return E_INVALIDARG;
        hr = detail_text(entry, item, text, ARRAY_SIZE(text));
        if (FAILED(hr))
            return hr;
        details->fmt = LVCFMT_LEFT;
        details->cxChar = 0;
    }
    details->str.uType = STRRET_WSTR;
    details->str.pOleStr = strdup_co(text);
    return details->str.pOleStr ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI folder_MapColumnToSCID(IShellFolder2 *iface, UINT column, SHCOLUMNID *scid)
{
    const wpd_column *entry = folder_column(impl_from_IShellFolder2(iface), column);

    if (!scid || !entry)
        return E_INVALIDARG;
    scid->fmtid = entry->key->fmtid;
    scid->pid = entry->key->pid;
    return S_OK;
}

static const IShellFolder2Vtbl folder_vtbl =
{
    folder_QueryInterface,
    folder_AddRef,
    folder_Release,
    folder_ParseDisplayName,
    folder_EnumObjects,
    folder_BindToObject,
    folder_BindToStorage,
    folder_CompareIDs,
    folder_CreateViewObject,
    folder_GetAttributesOf,
    folder_GetUIObjectOf,
    folder_GetDisplayNameOf,
    folder_SetNameOf,
    folder_GetDefaultSearchGUID,
    folder_EnumSearches,
    folder_GetDefaultColumn,
    folder_GetDefaultColumnState,
    folder_GetDetailsEx,
    folder_GetDetailsOf,
    folder_MapColumnToSCID,
};

static HRESULT WINAPI persist_QueryInterface(IPersistFolder2 *iface, REFIID iid, void **out)
{
    return folder_QueryInterface(&impl_from_IPersistFolder2(iface)->IShellFolder2_iface, iid, out);
}

static ULONG WINAPI persist_AddRef(IPersistFolder2 *iface)
{
    return folder_AddRef(&impl_from_IPersistFolder2(iface)->IShellFolder2_iface);
}

static ULONG WINAPI persist_Release(IPersistFolder2 *iface)
{
    return folder_Release(&impl_from_IPersistFolder2(iface)->IShellFolder2_iface);
}

static HRESULT WINAPI persist_GetClassID(IPersistFolder2 *iface, CLSID *clsid)
{
    if (!clsid)
        return E_POINTER;
    *clsid = impl_from_IPersistFolder2(iface)->clsid;
    return S_OK;
}

static HRESULT WINAPI persist_Initialize(IPersistFolder2 *iface, PCIDLIST_ABSOLUTE pidl)
{
    wpd_folder *folder = impl_from_IPersistFolder2(iface);
    PIDLIST_ABSOLUTE copy = ILClone(pidl);

    if (!copy)
        return E_OUTOFMEMORY;
    ILFree(folder->pidl);
    folder->pidl = copy;
    return S_OK;
}

static HRESULT WINAPI persist_GetCurFolder(IPersistFolder2 *iface, PIDLIST_ABSOLUTE *pidl)
{
    wpd_folder *folder = impl_from_IPersistFolder2(iface);

    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(folder->pidl);
    return *pidl ? S_OK : E_OUTOFMEMORY;
}

static const IPersistFolder2Vtbl persist_vtbl =
{
    persist_QueryInterface,
    persist_AddRef,
    persist_Release,
    persist_GetClassID,
    persist_Initialize,
    persist_GetCurFolder,
};

static HRESULT WINAPI delegate_QueryInterface(IDelegateFolder *iface, REFIID iid, void **out)
{
    return folder_QueryInterface(&impl_from_IDelegateFolder(iface)->IShellFolder2_iface, iid, out);
}

static ULONG WINAPI delegate_AddRef(IDelegateFolder *iface)
{
    return folder_AddRef(&impl_from_IDelegateFolder(iface)->IShellFolder2_iface);
}

static ULONG WINAPI delegate_Release(IDelegateFolder *iface)
{
    return folder_Release(&impl_from_IDelegateFolder(iface)->IShellFolder2_iface);
}

static HRESULT WINAPI delegate_SetItemAlloc(IDelegateFolder *iface, IMalloc *malloc)
{
    wpd_folder *folder = impl_from_IDelegateFolder(iface);

    if (malloc)
        IMalloc_AddRef(malloc);
    if (folder->malloc)
        IMalloc_Release(folder->malloc);
    folder->malloc = malloc;
    return S_OK;
}

static const IDelegateFolderVtbl delegate_vtbl =
{
    delegate_QueryInterface,
    delegate_AddRef,
    delegate_Release,
    delegate_SetItemAlloc,
};
