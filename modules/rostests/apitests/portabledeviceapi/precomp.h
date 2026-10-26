/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared declarations for the Windows Portable Devices API tests
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define COBJMACROS
#include <apitest.h>
#include <stdio.h>
#include <winioctl.h>
#include <objbase.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <portabledevice.h>
#include <wpdmtpextensions.h>

#define WPD_TEST_WAIT_SECONDS 90

typedef struct _WPD_TEST_DEVICE
{
    IPortableDevice *Device;
    IPortableDeviceContent *Content;
    IPortableDeviceProperties *Properties;
    IPortableDeviceResources *Resources;
    WCHAR PnPId[512];
} WPD_TEST_DEVICE;

void WpdFormatKey(REFPROPERTYKEY Key, char *Buffer, size_t Size);
void WpdFormatGuid(REFGUID Guid, char *Buffer, size_t Size);
void WpdDumpPropVariant(const char *Prefix, const PROPVARIANT *Value, int Depth);
void WpdDumpValues(const char *Prefix, IPortableDeviceValues *Values, int Depth);
void WpdDumpKeys(const char *Prefix, IPortableDeviceKeyCollection *Keys);
void WpdDumpPropVariants(const char *Prefix, IPortableDevicePropVariantCollection *Collection, int Depth);
void WpdDumpBytes(const char *Prefix, const BYTE *Data, DWORD Size, DWORD Limit);
ULONG WpdHash(const BYTE *Data, SIZE_T Size, ULONG Hash);

BOOL WpdWaitForDevice(IPortableDeviceManager **Manager, WCHAR *PnPId, DWORD PnPIdChars);
HRESULT WpdOpenTestDevice(WPD_TEST_DEVICE *Test);
void WpdCloseTestDevice(WPD_TEST_DEVICE *Test);
HRESULT WpdCreateValues(IPortableDeviceValues **Values);
HRESULT WpdFindStorage(WPD_TEST_DEVICE *Test, WCHAR *StorageId, DWORD StorageIdChars);
HRESULT WpdReadResource(WPD_TEST_DEVICE *Test, LPCWSTR ObjectId, BYTE **Data, SIZE_T *Size);

void WpdTraceIoctlsStart(void);
void WpdTraceIoctlsHookModules(void);
