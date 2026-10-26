/*
 * PROJECT:     LiberNT Portable Devices shell extension
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Private declarations of the Portable Devices shell folders
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define COBJMACROS

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stgprop.h>
#include <propkey.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <portabledevice.h>
#include <wine/debug.h>

#include "resource.h"

#define WPD_ITEM_SIGNATURE 0x49445057

enum
{
    WPD_KIND_DEVICE = 1,
    WPD_KIND_STORAGE,
    WPD_KIND_FOLDER,
    WPD_KIND_FILE
};

enum
{
    WPD_STR_ID,
    WPD_STR_PARSE,
    WPD_STR_NAME,
    WPD_STR_FILE,
    WPD_STR_EXTRA,
    WPD_STR_ARTIST,
    WPD_STR_ALBUM,
    WPD_STR_GENRE,
    WPD_STR_COUNT
};

#define WPD_ITEM_CAN_RENAME 0x0001
#define WPD_ITEM_PROTECTED  0x0002
#define WPD_ITEM_UNSPECIFIED_FORMAT 0x0004
#define WPD_ITEM_AUDIO      0x0008
#define WPD_ITEM_HAS_TRACK  0x0010
#define WPD_ITEM_MEDIA      0x0020
#define WPD_ITEM_HAS_DURATION 0x0040
#define WPD_ITEM_HAS_AUTHORED 0x0080
#define WPD_ITEM_HAS_DIMENSIONS 0x0100
#define WPD_ITEM_HAS_ARTIST 0x0200
#define WPD_ITEM_HAS_ALBUM  0x0400
#define WPD_ITEM_HAS_GENRE  0x0800

#pragma pack(push, 1)
typedef struct wpd_item
{
    DWORD signature;
    BYTE kind;
    BYTE type;
    WORD flags;
    ULONGLONG size;
    ULONGLONG capacity;
    ULONGLONG free;
    FILETIME created;
    FILETIME modified;
    FILETIME authored;
    ULONGLONG duration;
    DWORD track;
    DWORD width;
    DWORD height;
    WORD offsets[WPD_STR_COUNT];
    WORD chars;
    WCHAR strings[1];
} wpd_item;
#pragma pack(pop)

typedef struct wpd_item_info
{
    BYTE kind;
    BYTE type;
    WORD flags;
    ULONGLONG size;
    ULONGLONG capacity;
    ULONGLONG free;
    FILETIME created;
    FILETIME modified;
    FILETIME authored;
    ULONGLONG duration;
    DWORD track;
    DWORD width;
    DWORD height;
    const WCHAR *strings[WPD_STR_COUNT];
} wpd_item_info;

typedef struct wpd_device
{
    struct wpd_device *next;
    WCHAR *pnp_id;
    IPortableDevice *device;
    IPortableDeviceContent *content;
    IPortableDeviceProperties *properties;
    IPortableDeviceResources *resources;
} wpd_device;

typedef struct wpd_folder
{
    IShellFolder2 IShellFolder2_iface;
    IPersistFolder2 IPersistFolder2_iface;
    IDelegateFolder IDelegateFolder_iface;
    LONG refcount;
    CLSID clsid;
    PIDLIST_ABSOLUTE pidl;
    IMalloc *malloc;
    BYTE kind;
    BYTE device_type;
    WCHAR *pnp_id;
    WCHAR *object_id;
} wpd_folder;

extern HINSTANCE wpdshext_instance;
extern LONG wpdshext_locks;
extern const CLSID CLSID_WpdShellDelegateFolder;
extern const CLSID CLSID_WpdShellFolder;

HRESULT wpd_get_device(const WCHAR *pnp_id, wpd_device **device);
void wpd_free_devices(void);
const wpd_item *wpd_item_from_pidl(PCUIDLIST_RELATIVE pidl);
const WCHAR *wpd_item_string(const wpd_item *item, int index);
PITEMID_CHILD wpd_create_item(IMalloc *malloc, const wpd_item_info *info);
HRESULT wpd_enum_children(wpd_folder *folder, DWORD flags, PITEMID_CHILD **items, DWORD *count);
HRESULT wpd_object_item(wpd_folder *folder, wpd_device *device, const WCHAR *object_id, PITEMID_CHILD *item);
HRESULT wpd_device_item(wpd_folder *folder, const WCHAR *pnp_id, PITEMID_CHILD *item);
HRESULT wpd_enum_create(PITEMID_CHILD *items, DWORD count, IEnumIDList **out);
void wpd_free_items(PITEMID_CHILD *items, DWORD count);
BOOL wpd_item_is_container(const wpd_item *item);
int wpd_device_icon(BYTE type);

HRESULT wpd_folder_create(REFCLSID clsid, REFIID iid, void **out);
HRESULT wpd_folder_child(wpd_folder *parent, PCUITEMID_CHILD pidl, wpd_folder **child);
wpd_folder *wpd_folder_from_shellfolder(IShellFolder *iface);
void wpd_display_name(const wpd_item *item, WCHAR *buffer, DWORD chars, BOOL editing);

HRESULT wpd_extract_icon_create(const wpd_item *item, REFIID iid, void **out);
HRESULT wpd_data_object_create(wpd_folder *folder, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID iid, void **out);
HRESULT wpd_drop_target_create(wpd_folder *folder, REFIID iid, void **out);
HRESULT CALLBACK wpd_menu_callback(IShellFolder *folder, HWND hwnd, IDataObject *data, UINT msg, WPARAM wparam, LPARAM lparam);
HRESULT wpd_delete_items(wpd_folder *folder, UINT count, PCUITEMID_CHILD_ARRAY items);
