/*
 * PROJECT:     NextNT Windows Runtime Types
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows Runtime metadata file resolution and API information queries
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "private.h"
#include "rometadata.h"

WINE_DEFAULT_DEBUG_CHANNEL(wintypes);

static BOOL metadata_get_directory(WCHAR *dir, UINT size)
{
    static const WCHAR winmetadata[] = L"\\WinMetadata";
    UINT len = GetSystemDirectoryW(dir, size);

    if (!len || len + ARRAY_SIZE(winmetadata) > size)
        return FALSE;

    memcpy(dir + len, winmetadata, sizeof(winmetadata));
    return TRUE;
}

static HRESULT metadata_open_scope(IMetaDataDispenserEx *dispenser, const WCHAR *path, IMetaDataImport **import)
{
    IMetaDataDispenserEx *own = NULL;
    HRESULT hr;

    if (!dispenser)
    {
        hr = MetaDataGetDispenser(&CLSID_CorMetaDataDispenser, &IID_IMetaDataDispenserEx, (void **)&own);
        if (FAILED(hr))
            return hr;
        dispenser = own;
    }

    hr = IMetaDataDispenserEx_OpenScope(dispenser, path, 0, &IID_IMetaDataImport, (IUnknown **)import);
    if (own)
        IMetaDataDispenserEx_Release(own);
    return hr;
}

static BOOL metadata_scope_has_namespace(IMetaDataImport *import, const WCHAR *name, UINT32 length)
{
    WCHAR type_name[MAX_PATH];
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    ULONG count, written, flags;
    mdToken extends;
    BOOL found = FALSE;

    while (!found && IMetaDataImport_EnumTypeDefs(import, &henum, &type_def, 1, &count) == S_OK && count)
    {
        if (FAILED(IMetaDataImport_GetTypeDefProps(import, type_def, type_name, ARRAY_SIZE(type_name), &written,
                                                   &flags, &extends)))
            continue;
        if (wcslen(type_name) > length && !wcsncmp(type_name, name, length) && type_name[length] == '.')
            found = TRUE;
    }

    if (henum)
        IMetaDataImport_CloseEnum(import, henum);
    return found;
}

static HRESULT metadata_resolve(const WCHAR *name, UINT32 length, IMetaDataDispenserEx *dispenser, WCHAR *path,
                                UINT path_size, IMetaDataImport **import, mdTypeDef *type_def)
{
    WCHAR dir[MAX_PATH], candidate[MAX_PATH];
    BOOL is_namespace = FALSE;
    UINT32 prefix = length;

    *import = NULL;
    *type_def = mdTypeDefNil;

    if (wcspbrk(name, L"\\/:*?\"<>|"))
        return RO_E_METADATA_NAME_NOT_FOUND;
    if (!metadata_get_directory(dir, ARRAY_SIZE(dir)))
        return E_UNEXPECTED;

    for (;;)
    {
        IMetaDataImport *scope;
        mdTypeDef token;
        int written;

        written = _snwprintf(candidate, ARRAY_SIZE(candidate), L"%s\\%.*s.winmd", dir, (int)prefix, name);
        if (written > 0 && written < ARRAY_SIZE(candidate) &&
            GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES &&
            SUCCEEDED(metadata_open_scope(dispenser, candidate, &scope)))
        {
            if (IMetaDataImport_FindTypeDefByName(scope, name, 0, &token) == S_OK)
            {
                if (path)
                    lstrcpynW(path, candidate, path_size);
                *import = scope;
                *type_def = token;
                return S_OK;
            }

            if (!is_namespace)
                is_namespace = metadata_scope_has_namespace(scope, name, length);
            IMetaDataImport_Release(scope);
        }

        while (prefix && name[--prefix] != '.');
        if (!prefix)
            break;
    }

    return is_namespace ? RO_E_METADATA_NAME_IS_NAMESPACE : RO_E_METADATA_NAME_NOT_FOUND;
}

static IMetaDataImport *metadata_open_type(HSTRING type_name, mdTypeDef *type_def)
{
    IMetaDataImport *scope;
    const WCHAR *buffer;
    UINT32 length;

    buffer = WindowsGetStringRawBuffer(type_name, &length);
    if (!length || wcslen(buffer) != length)
        return NULL;
    if (metadata_resolve(buffer, length, NULL, NULL, 0, &scope, type_def) != S_OK)
        return NULL;
    return scope;
}

static const WCHAR *metadata_member_name(HSTRING member_name)
{
    const WCHAR *buffer;
    UINT32 length;

    buffer = WindowsGetStringRawBuffer(member_name, &length);
    if (!length || wcslen(buffer) != length)
        return NULL;
    return buffer;
}

static BOOL metadata_sig_uncompress(const COR_SIGNATURE *sig, ULONG len, ULONG *pos, ULONG *value)
{
    BYTE first;

    if (*pos >= len)
        return FALSE;

    first = sig[*pos];
    if (!(first & 0x80))
    {
        *value = first;
        *pos += 1;
        return TRUE;
    }
    if ((first & 0xc0) == 0x80)
    {
        if (*pos + 2 > len)
            return FALSE;
        *value = ((first & 0x3f) << 8) | sig[*pos + 1];
        *pos += 2;
        return TRUE;
    }
    if ((first & 0xe0) == 0xc0)
    {
        if (*pos + 4 > len)
            return FALSE;
        *value = ((ULONG)(first & 0x1f) << 24) | (sig[*pos + 1] << 16) | (sig[*pos + 2] << 8) | sig[*pos + 3];
        *pos += 4;
        return TRUE;
    }
    return FALSE;
}

static BOOL metadata_sig_param_count(const COR_SIGNATURE *sig, ULONG len, ULONG *count)
{
    ULONG pos = 1, generic_count;

    if (!sig || !len)
        return FALSE;
    if ((sig[0] & IMAGE_CEE_CS_CALLCONV_GENERIC) && !metadata_sig_uncompress(sig, len, &pos, &generic_count))
        return FALSE;
    return metadata_sig_uncompress(sig, len, &pos, count);
}

BOOLEAN metadata_type_present(HSTRING type_name)
{
    IMetaDataImport *scope;
    mdTypeDef type_def;

    if (!(scope = metadata_open_type(type_name, &type_def)))
        return FALSE;
    IMetaDataImport_Release(scope);
    return TRUE;
}

BOOLEAN metadata_method_present(HSTRING type_name, HSTRING method_name, BOOL check_arity, UINT32 arity)
{
    const WCHAR *name = metadata_member_name(method_name);
    const COR_SIGNATURE *sig;
    IMetaDataImport *scope;
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    mdMethodDef method;
    ULONG count, sig_len, params;
    BOOLEAN found = FALSE;

    if (!name || !(scope = metadata_open_type(type_name, &type_def)))
        return FALSE;

    while (!found && IMetaDataImport_EnumMethodsWithName(scope, &henum, type_def, name, &method, 1, &count) == S_OK &&
           count)
    {
        if (!check_arity)
            found = TRUE;
        else if (SUCCEEDED(IMetaDataImport_GetMethodProps(scope, method, NULL, NULL, 0, NULL, NULL, &sig, &sig_len,
                                                          NULL, NULL)) &&
                 metadata_sig_param_count(sig, sig_len, &params) && params == arity)
            found = TRUE;
    }

    if (henum)
        IMetaDataImport_CloseEnum(scope, henum);
    IMetaDataImport_Release(scope);
    return found;
}

BOOLEAN metadata_event_present(HSTRING type_name, HSTRING event_name)
{
    const WCHAR *name = metadata_member_name(event_name);
    WCHAR event_buffer[MAX_PATH];
    IMetaDataImport *scope;
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    mdEvent event;
    ULONG count, written;
    BOOLEAN found = FALSE;

    if (!name || !(scope = metadata_open_type(type_name, &type_def)))
        return FALSE;

    while (!found && IMetaDataImport_EnumEvents(scope, &henum, type_def, &event, 1, &count) == S_OK && count)
    {
        if (SUCCEEDED(IMetaDataImport_GetEventProps(scope, event, NULL, event_buffer, ARRAY_SIZE(event_buffer),
                                                    &written, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL)) &&
            !wcscmp(event_buffer, name))
            found = TRUE;
    }

    if (henum)
        IMetaDataImport_CloseEnum(scope, henum);
    IMetaDataImport_Release(scope);
    return found;
}

BOOLEAN metadata_property_present(HSTRING type_name, HSTRING property_name, BOOL need_getter, BOOL need_setter)
{
    const WCHAR *name = metadata_member_name(property_name);
    WCHAR property_buffer[MAX_PATH];
    mdMethodDef getter, setter;
    IMetaDataImport *scope;
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    mdProperty property;
    ULONG count, written;
    BOOLEAN found = FALSE;

    if (!name || !(scope = metadata_open_type(type_name, &type_def)))
        return FALSE;

    while (!found && IMetaDataImport_EnumProperties(scope, &henum, type_def, &property, 1, &count) == S_OK && count)
    {
        if (FAILED(IMetaDataImport_GetPropertyProps(scope, property, NULL, property_buffer,
                                                    ARRAY_SIZE(property_buffer), &written, NULL, NULL, NULL, NULL,
                                                    NULL, NULL, &setter, &getter, NULL, 0, NULL)))
            continue;
        if (wcscmp(property_buffer, name))
            continue;
        if (need_getter && IsNilToken(getter))
            continue;
        if (need_setter && IsNilToken(setter))
            continue;
        found = TRUE;
    }

    if (henum)
        IMetaDataImport_CloseEnum(scope, henum);
    IMetaDataImport_Release(scope);
    return found;
}

BOOLEAN metadata_enum_value_present(HSTRING type_name, HSTRING value_name)
{
    const WCHAR *name = metadata_member_name(value_name);
    IMetaDataImport *scope;
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    mdFieldDef field;
    ULONG count, flags;
    BOOLEAN found = FALSE;

    if (!name || !(scope = metadata_open_type(type_name, &type_def)))
        return FALSE;

    while (!found && IMetaDataImport_EnumFieldsWithName(scope, &henum, type_def, name, &field, 1, &count) == S_OK &&
           count)
    {
        if (SUCCEEDED(IMetaDataImport_GetFieldProps(scope, field, NULL, NULL, 0, NULL, &flags, NULL, NULL, NULL, NULL,
                                                    NULL)) &&
            (flags & fdLiteral))
            found = TRUE;
    }

    if (henum)
        IMetaDataImport_CloseEnum(scope, henum);
    IMetaDataImport_Release(scope);
    return found;
}

BOOLEAN metadata_contract_present(HSTRING contract_name, UINT16 major, UINT16 minor)
{
    IMetaDataImport *scope;
    mdTypeDef type_def;
    const BYTE *data;
    ULONG len;
    UINT32 version;
    BOOLEAN present = FALSE;

    if (!(scope = metadata_open_type(contract_name, &type_def)))
        return FALSE;

    if (IMetaDataImport_GetCustomAttributeByName(scope, type_def, L"Windows.Foundation.Metadata.ContractVersionAttribute",
                                                 &data, &len) == S_OK &&
        data && len >= 6 && data[0] == 1 && data[1] == 0)
    {
        memcpy(&version, data + 2, sizeof(version));
        present = (version >> 16) > major || ((version >> 16) == major && (version & 0xffff) >= minor);
    }

    IMetaDataImport_Release(scope);
    return present;
}

HRESULT WINAPI RoGetMetaDataFile(const HSTRING name, IMetaDataDispenserEx *dispenser, HSTRING *path,
                                 IMetaDataImport2 **import, mdTypeDef *type_def)
{
    WCHAR file[MAX_PATH];
    IMetaDataImport *scope;
    const WCHAR *buffer, *dot;
    mdTypeDef token;
    UINT32 length;
    HRESULT hr;

    TRACE("name %s, dispenser %p, path %p, import %p, type_def %p.\n", debugstr_hstring(name), dispenser, path,
          import, type_def);

    if (path)
        *path = NULL;
    if (import)
        *import = NULL;
    if (type_def)
        *type_def = mdTypeDefNil;

    buffer = WindowsGetStringRawBuffer(name, &length);
    if (!length || wcslen(buffer) != length)
        return E_INVALIDARG;
    if ((dot = wcsrchr(buffer, '.')) && (dot == buffer || !dot[1]))
        return E_INVALIDARG;
    if (!import != !type_def)
        return E_INVALIDARG;

    hr = metadata_resolve(buffer, length, dispenser, file, ARRAY_SIZE(file), &scope, &token);
    if (hr != S_OK)
        return hr;

    if (path && FAILED(hr = WindowsCreateString(file, wcslen(file), path)))
    {
        IMetaDataImport_Release(scope);
        return hr;
    }

    if (import)
    {
        hr = IMetaDataImport_QueryInterface(scope, &IID_IMetaDataImport2, (void **)import);
        if (FAILED(hr))
        {
            if (path)
            {
                WindowsDeleteString(*path);
                *path = NULL;
            }
            IMetaDataImport_Release(scope);
            return hr;
        }
        *type_def = token;
    }

    IMetaDataImport_Release(scope);
    return S_OK;
}

struct string_list
{
    HSTRING *items;
    DWORD count;
    DWORD capacity;
};

static BOOL string_list_contains(const struct string_list *list, const WCHAR *str, UINT32 length)
{
    DWORD i;

    for (i = 0; i < list->count; i++)
    {
        UINT32 item_length;
        const WCHAR *item = WindowsGetStringRawBuffer(list->items[i], &item_length);

        if (item_length == length && !wcsncmp(item, str, length))
            return TRUE;
    }
    return FALSE;
}

static HRESULT string_list_add(struct string_list *list, const WCHAR *str, UINT32 length)
{
    HSTRING *items;
    HRESULT hr;

    if (string_list_contains(list, str, length))
        return S_OK;

    if (list->count == list->capacity)
    {
        DWORD capacity = list->capacity ? list->capacity * 2 : 8;

        if (!(items = CoTaskMemRealloc(list->items, capacity * sizeof(*items))))
            return E_OUTOFMEMORY;
        list->items = items;
        list->capacity = capacity;
    }

    if (FAILED(hr = WindowsCreateString(str, length, &list->items[list->count])))
        return hr;
    list->count++;
    return S_OK;
}

static void string_list_free(struct string_list *list)
{
    DWORD i;

    for (i = 0; i < list->count; i++)
        WindowsDeleteString(list->items[i]);
    CoTaskMemFree(list->items);
    memset(list, 0, sizeof(*list));
}

static BOOL namespace_is_prefix(const WCHAR *prefix, UINT32 prefix_length, const WCHAR *name, UINT32 length)
{
    return prefix_length <= length && !wcsnicmp(prefix, name, prefix_length) &&
           (prefix_length == length || name[prefix_length] == '.');
}

static HRESULT metadata_collect_children(const WCHAR *path, const WCHAR *name, UINT32 length,
                                         struct string_list *children)
{
    WCHAR type_name[MAX_PATH];
    IMetaDataImport *scope;
    HCORENUM henum = NULL;
    mdTypeDef type_def;
    ULONG count, written, flags;
    mdToken extends;
    HRESULT hr = S_OK;

    if (FAILED(metadata_open_scope(NULL, path, &scope)))
        return S_OK;

    while (SUCCEEDED(hr) && IMetaDataImport_EnumTypeDefs(scope, &henum, &type_def, 1, &count) == S_OK && count)
    {
        const WCHAR *child, *end, *last_dot;

        if (FAILED(IMetaDataImport_GetTypeDefProps(scope, type_def, type_name, ARRAY_SIZE(type_name), &written,
                                                   &flags, &extends)))
            continue;
        if (!(last_dot = wcsrchr(type_name, '.')))
            continue;

        if (!length)
            child = type_name;
        else if ((UINT32)(last_dot - type_name) > length && !wcsncmp(type_name, name, length) &&
                 type_name[length] == '.')
            child = type_name + length + 1;
        else
            continue;

        if (!(end = wcschr(child, '.')) || end > last_dot)
            end = last_dot;
        if (end > child && end <= last_dot)
            hr = string_list_add(children, child, end - child);
    }

    if (henum)
        IMetaDataImport_CloseEnum(scope, henum);
    IMetaDataImport_Release(scope);
    return hr;
}

static HRESULT metadata_scan_directory(const WCHAR *dir, const WCHAR *name, UINT32 length,
                                       struct string_list *paths, struct string_list *children)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    HRESULT hr = S_OK;

    if (_snwprintf(pattern, ARRAY_SIZE(pattern), L"%s\\*.winmd", dir) < 0)
        return S_OK;
    pattern[ARRAY_SIZE(pattern) - 1] = 0;

    if ((find = FindFirstFileW(pattern, &data)) == INVALID_HANDLE_VALUE)
        return S_OK;

    do
    {
        UINT32 stem_length = wcslen(data.cFileName);
        BOOL candidate, descendant;
        int written;

        if (stem_length <= 6 || data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        stem_length -= 6;

        candidate = length && namespace_is_prefix(data.cFileName, stem_length, name, length);
        descendant = !length || namespace_is_prefix(name, length, data.cFileName, stem_length);
        if (!candidate && !descendant)
            continue;

        written = _snwprintf(path, ARRAY_SIZE(path), L"%s\\%s", dir, data.cFileName);
        if (written <= 0 || written >= ARRAY_SIZE(path))
            continue;

        if (candidate && paths && FAILED(hr = string_list_add(paths, path, written)))
            break;
        if (children && FAILED(hr = metadata_collect_children(path, name, length, children)))
            break;
    } while (FindNextFileW(find, &data));

    FindClose(find);
    return hr;
}

HRESULT metadata_resolve_namespace(HSTRING name, HSTRING windows_dir, DWORD package_count,
                                   const HSTRING *package_dirs, DWORD *paths_count, HSTRING **paths,
                                   DWORD *children_count, HSTRING **children)
{
    struct string_list path_list = {0}, child_list = {0};
    WCHAR system_dir[MAX_PATH];
    const WCHAR *buffer, *dir;
    UINT32 length, dir_length;
    HRESULT hr = S_OK;
    DWORD i;

    if (paths_count)
        *paths_count = 0;
    if (paths)
        *paths = NULL;
    if (children_count)
        *children_count = 0;
    if (children)
        *children = NULL;

    if (!paths && !children)
        return E_INVALIDARG;

    buffer = WindowsGetStringRawBuffer(name, &length);
    if (wcslen(buffer) != length)
        return E_INVALIDARG;
    if (!length && (!children || paths))
        return E_INVALIDARG;

    dir = WindowsGetStringRawBuffer(windows_dir, &dir_length);
    if (!dir_length)
    {
        if (!metadata_get_directory(system_dir, ARRAY_SIZE(system_dir)))
            return E_UNEXPECTED;
        dir = system_dir;
    }

    hr = metadata_scan_directory(dir, buffer, length, paths ? &path_list : NULL, children ? &child_list : NULL);
    for (i = 0; SUCCEEDED(hr) && package_dirs && i < package_count; i++)
    {
        const WCHAR *package_dir = WindowsGetStringRawBuffer(package_dirs[i], &dir_length);

        if (dir_length)
            hr = metadata_scan_directory(package_dir, buffer, length, paths ? &path_list : NULL,
                                         children ? &child_list : NULL);
    }

    if (FAILED(hr))
    {
        string_list_free(&path_list);
        string_list_free(&child_list);
        return hr;
    }

    if (!path_list.count && !child_list.count)
    {
        string_list_free(&path_list);
        string_list_free(&child_list);
        return RO_E_METADATA_NAME_NOT_FOUND;
    }

    if (paths)
    {
        *paths = path_list.items;
        if (paths_count)
            *paths_count = path_list.count;
    }
    else
        string_list_free(&path_list);

    if (children)
    {
        *children = child_list.items;
        if (children_count)
            *children_count = child_list.count;
    }
    else
        string_list_free(&child_list);

    return S_OK;
}
