/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WPD command layer shared definitions
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

typedef struct _WPD_COMMAND
{
    GUID Category;
    ULONG Id;
} WPD_COMMAND, *PWPD_COMMAND;

typedef enum _WPD_REF_KIND
{
    WpdRefDevice,
    WpdRefStorage,
    WpdRefObject
} WPD_REF_KIND;

typedef struct _WPD_OBJECT_REF
{
    WPD_REF_KIND Kind;
    ULONG Value;
    PMTP_STORAGE Storage;
    PMTP_OBJECT Object;
} WPD_OBJECT_REF, *PWPD_OBJECT_REF;

typedef enum _WPD_CONV
{
    WpdConvCore,
    WpdConvString,
    WpdConvDate,
    WpdConvUi4,
    WpdConvUi8,
    WpdConvBool,
    WpdConvChannels,
    WpdConvRaw
} WPD_CONV;

typedef struct _WPD_PROP_MAP
{
    const PROPERTYKEY *Key;
    USHORT Prop;
    BOOLEAN Fast;
    UCHAR Conv;
    BOOLEAN Derived;
} WPD_PROP_MAP;

HRESULT WpdNtToHresult(_In_ NTSTATUS Status);
VOID WpdMakeContextName(_Out_writes_(Chars) PWCHAR Buffer, _In_ SIZE_T Chars, _In_ PCWSTR Prefix);
PWPDMTP_CONTEXT WpdFindContext(_In_ PWPDMTP_DEVICE Dev, _In_opt_ PCWSTR Name, _In_ WPDMTP_CONTEXT_TYPE Type);
PWPDMTP_CONTEXT WpdCreateContext(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File, _In_ WPDMTP_CONTEXT_TYPE Type, _In_ PCWSTR Prefix);
VOID WpdFreeContext(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_CONTEXT Context);
BOOLEAN WpdParseObjectId(_In_opt_ PCWSTR Id, _Out_ PWPD_OBJECT_REF Ref);
VOID WpdFormatObjectId(_Out_writes_(16) PWCHAR Buffer, _In_ BOOLEAN Storage, _In_ ULONG Value);
HRESULT WpdResolveObject(_In_ PWPDMTP_DEVICE Dev, _In_opt_ PCWSTR Id, _Out_ PWPD_OBJECT_REF Ref);

HRESULT WpdResources(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File, _In_ ULONG Id, _In_ PWPD_COLLECTION Params,
                     _In_ ULONG OutputLimit, _Inout_ PWPD_COLLECTION Results);
HRESULT WpdManagement(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File, _In_ ULONG Id, _In_ PWPD_COLLECTION Params,
                      _Inout_ PWPD_COLLECTION Results);
HRESULT WpdVendor(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File, _In_ ULONG Id, _In_ PWPD_COLLECTION Params,
                  _In_ ULONG OutputLimit, _Inout_ PWPD_COLLECTION Results);
VOID WpdUploadRevert(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_CONTEXT Context);
