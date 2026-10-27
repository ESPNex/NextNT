/*
 * PROJECT:     LiberNT Windows Runtime Types
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
