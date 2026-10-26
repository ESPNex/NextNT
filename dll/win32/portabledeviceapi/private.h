/*
 * Portable Device API private declarations
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdarg.h>
#include <string.h>

#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <winioctl.h>
#include <winreg.h>
#include <objbase.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <portabledevice.h>

#include <wine/debug.h>

struct portable_device
{
    IPortableDevice IPortableDevice_iface;
    LONG refcount;
    CRITICAL_SECTION cs;
    HANDLE handle;
    WCHAR *pnp_id;
    WCHAR *client_context;
    DWORD desired_access;
    BOOL no_input_buffer_on_read;
    struct event_sink *sinks;
};

HRESULT portable_device_create(REFIID iid, void **out);
HRESULT portable_device_manager_create(REFIID iid, void **out);

HRESULT device_check_open(struct portable_device *device);
HRESULT device_send(struct portable_device *device, IPortableDeviceValues *params, IPortableDeviceValues **results);
HRESULT device_command(struct portable_device *device, REFPROPERTYKEY command, IPortableDeviceValues *params,
                       IPortableDeviceValues **results);
HRESULT create_values(IPortableDeviceValues **values);
HRESULT create_key_collection(IPortableDeviceKeyCollection **keys);
HRESULT create_propvariant_collection(IPortableDevicePropVariantCollection **collection);
HRESULT results_get_unknown(IPortableDeviceValues *results, REFPROPERTYKEY key, REFIID iid, void **out);
WCHAR *strdupW_co(const WCHAR *string);

HRESULT content_create(struct portable_device *device, IPortableDeviceContent **content);
HRESULT capabilities_create(struct portable_device *device, IPortableDeviceCapabilities **capabilities);
HRESULT properties_create(struct portable_device *device, IPortableDeviceProperties **properties);
HRESULT resources_create(struct portable_device *device, IPortableDeviceResources **resources);
HRESULT enum_create(struct portable_device *device, const WCHAR *parent, IPortableDeviceValues *filter,
                    IEnumPortableDeviceObjectIDs **out);
HRESULT upload_stream_create(struct portable_device *device, const WCHAR *context, DWORD optimal, IStream **out);
HRESULT read_stream_create(struct portable_device *device, const WCHAR *object_id, const WCHAR *context,
                           IStream **out);

static inline struct portable_device *impl_from_IPortableDevice(IPortableDevice *iface)
{
    return CONTAINING_RECORD(iface, struct portable_device, IPortableDevice_iface);
}
