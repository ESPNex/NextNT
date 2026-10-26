/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows Portable Devices driver for USB MTP devices: definitions
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <devpropdef.h>
#include <devpkey.h>

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif

#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif

#ifndef DECLSPEC_HIDDEN
#define DECLSPEC_HIDDEN
#endif

#ifndef __MIDL_CONST
#define __MIDL_CONST const
#endif

typedef ULONG DWORD;

typedef struct _tagpropertykey
{
    GUID fmtid;
    DWORD pid;
} PROPERTYKEY;

#define REFPROPERTYKEY const PROPERTYKEY *

#include <portabledevice.h>
#include <wpdmtpextensions.h>

#define WPDMTP_TAG 'pMdW'

#define WPD_S_OK                    ((HRESULT)0x00000000L)
#define WPD_S_FALSE                 ((HRESULT)0x00000001L)
#define WPD_E_NOTIMPL               ((HRESULT)0x80004001L)
#define WPD_E_NOINTERFACE           ((HRESULT)0x80004002L)
#define WPD_E_FAIL                  ((HRESULT)0x80004005L)
#define WPD_E_UNEXPECTED            ((HRESULT)0x8000FFFFL)
#define WPD_E_ACCESSDENIED          ((HRESULT)0x80070005L)
#define WPD_E_OUTOFMEMORY           ((HRESULT)0x8007000EL)
#define WPD_E_INVALIDARG            ((HRESULT)0x80070057L)
#define WPD_E_GEN_FAILURE           ((HRESULT)0x8007001FL)
#define WPD_E_NOT_SUPPORTED         ((HRESULT)0x80070032L)
#define WPD_E_INSUFFICIENT_BUFFER   ((HRESULT)0x8007007AL)
#define WPD_E_BUSY                  ((HRESULT)0x800700AAL)
#define WPD_E_NOT_FOUND             ((HRESULT)0x80070490L)
#define WPD_E_UNKNOWN_PROPERTY      ((HRESULT)0x80070648L)
#define WPD_E_CANCELLED             ((HRESULT)0x800704C7L)
#define WPD_E_DEVICE_NOT_CONNECTED  ((HRESULT)0x8007048FL)
#define WPD_E_SEM_TIMEOUT           ((HRESULT)0x80070079L)
#define WPD_E_MTP(Response)         ((HRESULT)(0x80040000L | (Response)))

#define WPD_VT_EMPTY     0
#define WPD_VT_NULL      1
#define WPD_VT_I2        2
#define WPD_VT_I4        3
#define WPD_VT_R4        4
#define WPD_VT_R8        5
#define WPD_VT_DATE      7
#define WPD_VT_BSTR      8
#define WPD_VT_ERROR     10
#define WPD_VT_BOOL      11
#define WPD_VT_UNKNOWN   13
#define WPD_VT_I1        16
#define WPD_VT_UI1       17
#define WPD_VT_UI2       18
#define WPD_VT_UI4       19
#define WPD_VT_I8        20
#define WPD_VT_UI8       21
#define WPD_VT_INT       22
#define WPD_VT_UINT      23
#define WPD_VT_LPWSTR    31
#define WPD_VT_FILETIME  64
#define WPD_VT_CLSID     72
#define WPD_VT_VECTOR    0x1000

typedef enum _WPD_COLLECTION_KIND
{
    WpdValues,
    WpdKeys,
    WpdVariants,
    WpdValuesList
} WPD_COLLECTION_KIND;

typedef struct _WPD_COLLECTION WPD_COLLECTION, *PWPD_COLLECTION;

typedef struct _WPD_VALUE
{
    USHORT Vt;
    union
    {
        UCHAR Ui1;
        USHORT Ui2;
        SHORT Bool;
        ULONG Ui4;
        LONG I4;
        ULONGLONG Ui8;
        LONGLONG I8;
        GUID Guid;
        struct
        {
            PWCHAR Buffer;
            ULONG Bytes;
        } String;
        struct
        {
            PUCHAR Buffer;
            ULONG Size;
        } Blob;
        PWPD_COLLECTION Collection;
    };
} WPD_VALUE, *PWPD_VALUE;

typedef struct _WPD_ENTRY
{
    PROPERTYKEY Key;
    WPD_VALUE Value;
} WPD_ENTRY, *PWPD_ENTRY;

struct _WPD_COLLECTION
{
    WPD_COLLECTION_KIND Kind;
    ULONG Count;
    ULONG Capacity;
    union
    {
        PWPD_ENTRY Entries;
        PROPERTYKEY *Keys;
        PWPD_VALUE Items;
        PWPD_COLLECTION *Lists;
        PVOID Data;
    };
};

#define MTP_CONTAINER_COMMAND   1
#define MTP_CONTAINER_DATA      2
#define MTP_CONTAINER_RESPONSE  3
#define MTP_CONTAINER_EVENT     4

#define MTP_OP_GET_DEVICE_INFO              0x1001
#define MTP_OP_OPEN_SESSION                 0x1002
#define MTP_OP_CLOSE_SESSION                0x1003
#define MTP_OP_GET_STORAGE_IDS              0x1004
#define MTP_OP_GET_STORAGE_INFO             0x1005
#define MTP_OP_GET_NUM_OBJECTS              0x1006
#define MTP_OP_GET_OBJECT_HANDLES           0x1007
#define MTP_OP_GET_OBJECT_INFO              0x1008
#define MTP_OP_GET_OBJECT                   0x1009
#define MTP_OP_DELETE_OBJECT                0x100B
#define MTP_OP_SEND_OBJECT_INFO             0x100C
#define MTP_OP_SEND_OBJECT                  0x100D
#define MTP_OP_FORMAT_STORE                 0x100F
#define MTP_OP_GET_DEVICE_PROP_DESC         0x1014
#define MTP_OP_GET_DEVICE_PROP_VALUE        0x1015
#define MTP_OP_SET_DEVICE_PROP_VALUE        0x1016
#define MTP_OP_MOVE_OBJECT                  0x1019
#define MTP_OP_COPY_OBJECT                  0x101A
#define MTP_OP_GET_PARTIAL_OBJECT           0x101B
#define MTP_OP_GET_OBJECT_PROPS_SUPPORTED   0x9801
#define MTP_OP_GET_OBJECT_PROP_DESC         0x9802
#define MTP_OP_GET_OBJECT_PROP_VALUE        0x9803
#define MTP_OP_SET_OBJECT_PROP_VALUE        0x9804
#define MTP_OP_GET_OBJECT_PROP_LIST         0x9805

#define MTP_RC_OK                           0x2001
#define MTP_RC_GENERAL_ERROR                0x2002
#define MTP_RC_SESSION_NOT_OPEN             0x2003
#define MTP_RC_OPERATION_NOT_SUPPORTED      0x2005
#define MTP_RC_INVALID_OBJECT_HANDLE        0x2009
#define MTP_RC_DEVICE_BUSY                  0x2019
#define MTP_RC_SESSION_ALREADY_OPEN         0x201E

#define MTP_EVENT_OBJECT_ADDED              0x4002
#define MTP_EVENT_OBJECT_REMOVED            0x4003
#define MTP_EVENT_STORE_ADDED               0x4004
#define MTP_EVENT_STORE_REMOVED             0x4005
#define MTP_EVENT_DEVICE_PROP_CHANGED       0x4006
#define MTP_EVENT_OBJECT_INFO_CHANGED       0x4007
#define MTP_EVENT_DEVICE_INFO_CHANGED       0x4008
#define MTP_EVENT_STORAGE_INFO_CHANGED      0x400C
#define MTP_EVENT_OBJECT_PROP_CHANGED       0xC801

#define MTP_FORMAT_UNDEFINED                0x3000
#define MTP_FORMAT_ASSOCIATION              0x3001

#define MTP_PROP_STORAGE_ID                 0xDC01
#define MTP_PROP_OBJECT_FORMAT              0xDC02
#define MTP_PROP_PROTECTION_STATUS          0xDC03
#define MTP_PROP_OBJECT_SIZE                0xDC04
#define MTP_PROP_OBJECT_FILE_NAME           0xDC07
#define MTP_PROP_DATE_CREATED               0xDC08
#define MTP_PROP_DATE_MODIFIED              0xDC09
#define MTP_PROP_PARENT_OBJECT              0xDC0B
#define MTP_PROP_HIDDEN                     0xDC0D
#define MTP_PROP_SYSTEM_OBJECT              0xDC0E
#define MTP_PROP_PERSISTENT_UID             0xDC41
#define MTP_PROP_NAME                       0xDC44
#define MTP_PROP_NON_CONSUMABLE             0xDC4F

#define MTP_DEVICE_PROP_BATTERY_LEVEL       0x5001
#define MTP_DEVICE_PROP_FRIENDLY_NAME       0xD402
#define MTP_DEVICE_PROP_PERCEIVED_TYPE      0xD407

#define MTP_TYPE_INT8      0x0001
#define MTP_TYPE_UINT8     0x0002
#define MTP_TYPE_INT16     0x0003
#define MTP_TYPE_UINT16    0x0004
#define MTP_TYPE_INT32     0x0005
#define MTP_TYPE_UINT32    0x0006
#define MTP_TYPE_INT64     0x0007
#define MTP_TYPE_UINT64    0x0008
#define MTP_TYPE_INT128    0x0009
#define MTP_TYPE_UINT128   0x000A
#define MTP_TYPE_ARRAY     0x4000
#define MTP_TYPE_AUINT16   0x4004
#define MTP_TYPE_STRING    0xFFFF

#define MTP_STORAGE_ALL             0xFFFFFFFF
#define MTP_ROOT_PARENT             0xFFFFFFFF
#define MTP_MAX_PARAMS              5

#define USB_STILL_IMAGE_CANCEL_REQUEST          0x64
#define USB_STILL_IMAGE_DEVICE_RESET_REQUEST    0x66
#define USB_STILL_IMAGE_GET_DEVICE_STATUS       0x67

#define WPDMTP_TRANSFER_CHUNK       (256 * 1024)
#define WPDMTP_OPTIMAL_TRANSFER     (256 * 1024)
#define WPDMTP_MAX_STRING_BYTES     510

#pragma pack(push, 1)
typedef struct _MTP_CONTAINER_HEADER
{
    ULONG Length;
    USHORT Type;
    USHORT Code;
    ULONG TransactionId;
} MTP_CONTAINER_HEADER, *PMTP_CONTAINER_HEADER;
#pragma pack(pop)

typedef struct _MTP_DATASET
{
    PUCHAR Data;
    ULONG Size;
    ULONG Offset;
    BOOLEAN Overrun;
} MTP_DATASET, *PMTP_DATASET;

typedef struct _MTP_OPERATION
{
    USHORT Code;
    ULONG ParamCount;
    ULONG Params[MTP_MAX_PARAMS];
    PVOID DataOut;
    ULONG DataOutLength;
    BOOLEAN DataIn;
    PUCHAR InData;
    ULONG InLength;
    USHORT ResponseCode;
    ULONG ResponseParamCount;
    ULONG ResponseParams[MTP_MAX_PARAMS];
} MTP_OPERATION, *PMTP_OPERATION;

typedef struct _MTP_VALUE
{
    USHORT DataType;
    BOOLEAN Valid;
    UCHAR Raw[16];
    PWCHAR String;
} MTP_VALUE, *PMTP_VALUE;

#define MTP_FORM_NONE           0
#define MTP_FORM_RANGE          1
#define MTP_FORM_ENUMERATION    2

typedef struct _MTP_FORM
{
    UCHAR Flag;
    MTP_VALUE Range[3];
    ULONG EnumCount;
    PMTP_VALUE Enum;
} MTP_FORM, *PMTP_FORM;

typedef struct _MTP_PROP_DESC
{
    LIST_ENTRY Link;
    USHORT Format;
    USHORT Code;
    USHORT DataType;
    UCHAR GetSet;
    BOOLEAN Valid;
    MTP_VALUE Default;
    MTP_FORM Form;
} MTP_PROP_DESC, *PMTP_PROP_DESC;

typedef struct _MTP_DEVICE_PROP
{
    USHORT Code;
    USHORT DataType;
    UCHAR GetSet;
    BOOLEAN Valid;
    MTP_VALUE Default;
    MTP_VALUE Current;
    MTP_FORM Form;
} MTP_DEVICE_PROP, *PMTP_DEVICE_PROP;

typedef struct _MTP_OBJECT_PROP
{
    USHORT Code;
    MTP_VALUE Value;
} MTP_OBJECT_PROP, *PMTP_OBJECT_PROP;

typedef struct _MTP_FORMAT
{
    USHORT Code;
    BOOLEAN PropsLoaded;
    ULONG PropCount;
    PUSHORT Props;
} MTP_FORMAT, *PMTP_FORMAT;

typedef struct _MTP_STORAGE
{
    LIST_ENTRY Link;
    ULONG Id;
    USHORT StorageType;
    USHORT FilesystemType;
    USHORT AccessCapability;
    ULONGLONG MaxCapacity;
    ULONGLONG FreeSpaceInBytes;
    ULONG FreeSpaceInObjects;
    PWCHAR Description;
    PWCHAR VolumeIdentifier;
} MTP_STORAGE, *PMTP_STORAGE;

typedef struct _MTP_OBJECT
{
    LIST_ENTRY Link;
    ULONG Handle;
    ULONG StorageId;
    ULONG Parent;
    USHORT Format;
    USHORT ProtectionStatus;
    ULONGLONG Size;
    PWCHAR FileName;
    PWCHAR Name;
    PWCHAR DateCreated;
    PWCHAR DateModified;
    UCHAR Puid[16];
    BOOLEAN PuidValid;
    BOOLEAN InfoLoaded;
    BOOLEAN PropsLoaded;
    BOOLEAN Hidden;
    BOOLEAN System;
    BOOLEAN NonConsumable;
    ULONG PropCount;
    PMTP_OBJECT_PROP Props;
} MTP_OBJECT, *PMTP_OBJECT;

typedef enum _WPDMTP_OBJECT_EVENT
{
    WpdObjectEventAdded,
    WpdObjectEventUpdated,
    WpdObjectEventHierarchy,
    WpdObjectEventRemoved
} WPDMTP_OBJECT_EVENT;

typedef enum _WPDMTP_CONTEXT_TYPE
{
    WpdContextEnum,
    WpdContextRead,
    WpdContextUpload,
    WpdContextVendor,
    WpdContextBulkList,
    WpdContextBulkFormat
} WPDMTP_CONTEXT_TYPE;

typedef struct _WPDMTP_FILE *PWPDMTP_FILE;

typedef struct _WPDMTP_CONTEXT
{
    LIST_ENTRY Link;
    WPDMTP_CONTEXT_TYPE Type;
    PWPDMTP_FILE Owner;
    WCHAR Name[96];
    ULONG Handle;
    ULONGLONG Offset;
    ULONGLONG Total;
    ULONG Count;
    ULONG Next;
    PULONG Handles;
    BOOLEAN Streaming;
    BOOLEAN Failed;
    BOOLEAN Folder;
    USHORT VendorCode;
    ULONG VendorParamCount;
    ULONG VendorParams[MTP_MAX_PARAMS];
    PWPD_COLLECTION Properties;
} WPDMTP_CONTEXT, *PWPDMTP_CONTEXT;

typedef struct _WPDMTP_FILE
{
    WCHAR ClientContext[40];
    BOOLEAN HasClient;
    ULONG DesiredAccess;
} WPDMTP_FILE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WPDMTP_FILE, WpdMtpGetFile)

typedef struct _WPDMTP_DEVICE
{
    WDFDEVICE Device;
    PDEVICE_OBJECT LowerDeviceObject;
    WDFQUEUE PendingQueue;
    WDFWORKITEM EventItem;
    KEVENT Lock;
    KSPIN_LOCK EventLock;

    USHORT VendorId;
    USHORT ProductId;
    UCHAR InterfaceNumber;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;
    USBD_PIPE_HANDLE BulkIn;
    USBD_PIPE_HANDLE BulkOut;
    USBD_PIPE_HANDLE InterruptIn;
    ULONG BulkInPacketSize;
    ULONG BulkOutPacketSize;
    ULONG InterruptPacketSize;
    PUCHAR TransferBuffer;

    PIRP EventIrp;
    PURB EventUrb;
    PUCHAR EventBuffer;
    volatile LONG EventPending;
    volatile BOOLEAN Stopping;
    KEVENT EventIdle;
    LIST_ENTRY EventQueue;

    BOOLEAN SessionOpen;
    ULONG SessionId;
    ULONG TransactionId;
    BOOLEAN DataInOpen;
    BOOLEAN ResponseBuffered;
    BOOLEAN ResponsePending;
    ULONGLONG DataInRemaining;
    ULONG InBuffered;
    ULONG InOffset;
    BOOLEAN DataOutOpen;
    ULONGLONG DataOutRemaining;
    ULONGLONG DataOutTotal;
    ULONG DataOutTransfered;
    PWPDMTP_CONTEXT ActiveTransfer;

    USHORT StandardVersion;
    ULONG VendorExtensionId;
    USHORT VendorExtensionVersion;
    PWCHAR VendorExtensionDesc;
    ULONG OperationCount;
    PUSHORT Operations;
    ULONG EventCount;
    PUSHORT Events;
    ULONG DevicePropCount;
    PUSHORT DeviceProps;
    PMTP_DEVICE_PROP DevicePropInfo;
    ULONG FormatCount;
    PMTP_FORMAT Formats;
    PWCHAR Manufacturer;
    PWCHAR Model;
    PWCHAR DeviceVersion;
    PWCHAR SerialNumber;
    PWCHAR FriendlyName;
    ULONG PerceivedType;
    BOOLEAN PowerLevelValid;
    ULONG PowerLevel;

    LIST_ENTRY Storages;
    LIST_ENTRY Objects;
    LIST_ENTRY PropDescs;
    LIST_ENTRY Contexts;
    BOOLEAN StoragesLoaded;
    UNICODE_STRING InterfaceName;
} WPDMTP_DEVICE, *PWPDMTP_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WPDMTP_DEVICE, WpdMtpGetDevice)

typedef struct _WPDMTP_EVENT
{
    LIST_ENTRY Link;
    USHORT Code;
    ULONG ParamCount;
    ULONG Params[3];
} WPDMTP_EVENT, *PWPDMTP_EVENT;

PWPD_COLLECTION WpdCollectionCreate(_In_ WPD_COLLECTION_KIND Kind);
VOID WpdCollectionFree(_In_opt_ PWPD_COLLECTION Collection);
VOID WpdValueClear(_Inout_ PWPD_VALUE Value);
NTSTATUS WpdValueCopy(_Out_ PWPD_VALUE Destination, _In_ const WPD_VALUE *Source);
NTSTATUS WpdValuesSet(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Inout_ PWPD_VALUE Value);
PWPD_VALUE WpdValuesFind(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key);
NTSTATUS WpdValuesSetUi4(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ ULONG Value);
NTSTATUS WpdValuesSetUi8(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ ULONGLONG Value);
NTSTATUS WpdValuesSetI8(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ LONGLONG Value);
NTSTATUS WpdValuesSetBool(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ BOOLEAN Value);
NTSTATUS WpdValuesSetError(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ HRESULT Value);
NTSTATUS WpdValuesSetGuid(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ const GUID *Value);
NTSTATUS WpdValuesSetString(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ PCWSTR Value);
NTSTATUS WpdMakeString(_Out_ PWPD_VALUE Item, _In_opt_ PCWSTR Value);
NTSTATUS WpdValuesSetBuffer(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_reads_bytes_(Size) const VOID *Data, _In_ ULONG Size);
NTSTATUS WpdValuesSetCollection(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ PWPD_COLLECTION Collection);
NTSTATUS WpdValuesSetKey(_Inout_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ REFPROPERTYKEY Value);
BOOLEAN WpdValuesGetUi4(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PULONG Value);
BOOLEAN WpdValuesGetUi8(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PULONGLONG Value);
BOOLEAN WpdValuesGetI8(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PLONGLONG Value);
BOOLEAN WpdValuesGetBool(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PBOOLEAN Value);
BOOLEAN WpdValuesGetGuid(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ GUID *Value);
PCWSTR WpdValuesGetString(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key);
BOOLEAN WpdValuesGetBuffer(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PUCHAR *Data, _Out_ PULONG Size);
PWPD_COLLECTION WpdValuesGetCollection(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _In_ WPD_COLLECTION_KIND Kind);
BOOLEAN WpdValuesGetKey(_In_ PWPD_COLLECTION Values, _In_ REFPROPERTYKEY Key, _Out_ PROPERTYKEY *Value);
NTSTATUS WpdKeysAdd(_Inout_ PWPD_COLLECTION Keys, _In_ REFPROPERTYKEY Key);
BOOLEAN WpdKeysContain(_In_ PWPD_COLLECTION Keys, _In_ REFPROPERTYKEY Key);
NTSTATUS WpdVariantsAdd(_Inout_ PWPD_COLLECTION Variants, _Inout_ PWPD_VALUE Value);
NTSTATUS WpdListsAdd(_Inout_ PWPD_COLLECTION List, _In_ PWPD_COLLECTION Values);
NTSTATUS WpdVariantsAddString(_Inout_ PWPD_COLLECTION Variants, _In_ PCWSTR Value);
NTSTATUS WpdVariantsAddGuid(_Inout_ PWPD_COLLECTION Variants, _In_ const GUID *Value);
NTSTATUS WpdVariantsAddError(_Inout_ PWPD_COLLECTION Variants, _In_ HRESULT Value);
NTSTATUS WpdVariantsAddUi4(_Inout_ PWPD_COLLECTION Variants, _In_ ULONG Value);
BOOLEAN WpdKeyEqual(_In_ REFPROPERTYKEY Left, _In_ REFPROPERTYKEY Right);
NTSTATUS WpdDeserialize(_In_reads_bytes_(Size) const UCHAR *Buffer, _In_ ULONG Size, _Out_ PWPD_COLLECTION *Values);
ULONG WpdSerializedSize(_In_ PWPD_COLLECTION Values);
VOID WpdSerialize(_In_ PWPD_COLLECTION Values, _Out_writes_bytes_(Size) PUCHAR Buffer, _In_ ULONG Size);

NTSTATUS WpdMtpUsbConfigure(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpUsbUnconfigure(_In_ PWPDMTP_DEVICE Dev);
NTSTATUS WpdMtpUsbBulk(_In_ PWPDMTP_DEVICE Dev, _In_ BOOLEAN In, _Inout_updates_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _Out_ PULONG Transferred);
NTSTATUS WpdMtpUsbResetPipe(_In_ PWPDMTP_DEVICE Dev, _In_ USBD_PIPE_HANDLE Pipe);
NTSTATUS WpdMtpUsbClassRequest(_In_ PWPDMTP_DEVICE Dev, _In_ BOOLEAN In, _In_ UCHAR Request, _Inout_updates_bytes_opt_(Length) PVOID Buffer, _In_ USHORT Length);
NTSTATUS WpdMtpUsbStartEvents(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpUsbStopEvents(_In_ PWPDMTP_DEVICE Dev);
EVT_WDF_WORKITEM WpdMtpEventWork;

VOID WpdMtpLock(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpUnlock(_In_ PWPDMTP_DEVICE Dev);
NTSTATUS WpdMtpTransaction(_In_ PWPDMTP_DEVICE Dev, _Inout_ PMTP_OPERATION Op);
NTSTATUS WpdMtpBeginDataIn(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code, _In_ ULONG ParamCount, _In_reads_(ParamCount) const ULONG *Params, _Out_ PULONGLONG Total);
NTSTATUS WpdMtpReadData(_In_ PWPDMTP_DEVICE Dev, _Out_writes_bytes_(Length) PUCHAR Buffer, _In_ ULONG Length, _Out_ PULONG Read);
NTSTATUS WpdMtpBeginDataOut(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code, _In_ ULONG ParamCount, _In_reads_(ParamCount) const ULONG *Params, _In_ ULONGLONG Total);
NTSTATUS WpdMtpWriteData(_In_ PWPDMTP_DEVICE Dev, _In_reads_bytes_(Length) const UCHAR *Buffer, _In_ ULONG Length);
NTSTATUS WpdMtpEndTransaction(_In_ PWPDMTP_DEVICE Dev, _Out_ PUSHORT ResponseCode, _Out_opt_ PULONG ResponseParams, _Out_opt_ PULONG ResponseParamCount);
NTSTATUS WpdMtpAbortData(_In_ PWPDMTP_DEVICE Dev);
NTSTATUS WpdMtpOpenSession(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpCloseSession(_In_ PWPDMTP_DEVICE Dev);
BOOLEAN WpdMtpSupportsOperation(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code);
BOOLEAN WpdMtpSupportsDeviceProp(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code);
HRESULT WpdMtpResultToHresult(_In_ NTSTATUS Status, _In_ USHORT ResponseCode);
VOID WpdMtpFreeDeviceInfo(_In_ PWPDMTP_DEVICE Dev);

UCHAR MtpReadU8(_Inout_ PMTP_DATASET Set);
USHORT MtpReadU16(_Inout_ PMTP_DATASET Set);
ULONG MtpReadU32(_Inout_ PMTP_DATASET Set);
ULONGLONG MtpReadU64(_Inout_ PMTP_DATASET Set);
VOID MtpReadBytes(_Inout_ PMTP_DATASET Set, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);
PWCHAR MtpReadString(_Inout_ PMTP_DATASET Set);
PUSHORT MtpReadU16Array(_Inout_ PMTP_DATASET Set, _Out_ PULONG Count);
PULONG MtpReadU32Array(_Inout_ PMTP_DATASET Set, _Out_ PULONG Count);
VOID MtpSkipValue(_Inout_ PMTP_DATASET Set, _In_ USHORT DataType);
ULONG MtpStringSize(_In_opt_ PCWSTR String);
PUCHAR MtpWriteU8(_Inout_ PUCHAR Cursor, _In_ UCHAR Value);
PUCHAR MtpWriteU16(_Inout_ PUCHAR Cursor, _In_ USHORT Value);
PUCHAR MtpWriteU32(_Inout_ PUCHAR Cursor, _In_ ULONG Value);
PUCHAR MtpWriteString(_Inout_ PUCHAR Cursor, _In_opt_ PCWSTR String);
PWCHAR WpdMtpDuplicateString(_In_opt_ PCWSTR String);
BOOLEAN MtpReadValue(_Inout_ PMTP_DATASET Set, _In_ USHORT DataType, _Out_ PMTP_VALUE Value);
VOID MtpFreeValue(_Inout_ PMTP_VALUE Value);
VOID MtpReadForm(_Inout_ PMTP_DATASET Set, _In_ USHORT DataType, _Out_ PMTP_FORM Form);
VOID MtpFreeForm(_Inout_ PMTP_FORM Form);
LONGLONG MtpValueInteger(_In_ const MTP_VALUE *Value);

NTSTATUS WpdMtpLoadDevice(_In_ PWPDMTP_DEVICE Dev);
NTSTATUS WpdMtpLoadStorages(_In_ PWPDMTP_DEVICE Dev);
PMTP_STORAGE WpdMtpFindStorage(_In_ PWPDMTP_DEVICE Dev, _In_ ULONG Id);
HRESULT WpdMtpGetObject(_In_ PWPDMTP_DEVICE Dev, _In_ ULONG Handle, _In_ BOOLEAN Properties, _Out_ PMTP_OBJECT *Object);
VOID WpdMtpForgetObject(_In_ PWPDMTP_DEVICE Dev, _In_ ULONG Handle);
VOID WpdMtpForgetAll(_In_ PWPDMTP_DEVICE Dev);
HRESULT WpdMtpGetChildren(_In_ PWPDMTP_DEVICE Dev, _In_ ULONG StorageId, _In_ ULONG Parent, _Out_ PULONG *Handles, _Out_ PULONG Count);
PMTP_FORMAT WpdMtpFindFormat(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code);
NTSTATUS WpdMtpLoadFormatProps(_In_ PWPDMTP_DEVICE Dev, _In_ PMTP_FORMAT Format);
BOOLEAN WpdMtpFormatHasProp(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Format, _In_ USHORT Prop);
PMTP_PROP_DESC WpdMtpGetPropDesc(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Format, _In_ USHORT Prop);
PMTP_DEVICE_PROP WpdMtpGetDeviceProp(_In_ PWPDMTP_DEVICE Dev, _In_ USHORT Code, _In_ BOOLEAN Refresh);
const MTP_VALUE *WpdMtpObjectProp(_In_ PMTP_OBJECT Object, _In_ USHORT Code);
VOID WpdMtpFreeCache(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpFormatGuid(_In_ USHORT Format, _Out_ GUID *Guid);
BOOLEAN WpdMtpFormatFromGuid(_In_ const GUID *Guid, _Out_ PUSHORT Format);
const GUID *WpdMtpContentType(_In_ USHORT Format);
USHORT WpdMtpFormatForContentType(_In_ PWPDMTP_DEVICE Dev, _In_ const GUID *ContentType);

NTSTATUS WpdMtpDispatch(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File, _In_ BOOLEAN WriteAccess,
                        _In_ PWPD_COLLECTION Params, _In_ ULONG OutputLimit, _Out_ PWPD_COLLECTION *Results);
BOOLEAN WpdMtpRequestBlocked(_In_ PWPDMTP_DEVICE Dev, _In_ PWPD_COLLECTION Params);
VOID WpdMtpPostObjectEvent(_In_ PWPDMTP_DEVICE Dev, _In_ WPDMTP_OBJECT_EVENT Kind, _In_ PMTP_OBJECT Object, _In_opt_ PCWSTR Cookie);
VOID WpdMtpPostHandleEvent(_In_ PWPDMTP_DEVICE Dev, _In_ WPDMTP_OBJECT_EVENT Kind, _In_ ULONG Handle, _In_opt_ PCWSTR Cookie);
VOID WpdMtpReleaseFile(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_FILE File);
VOID WpdMtpReleaseAllContexts(_In_ PWPDMTP_DEVICE Dev);
VOID WpdMtpProcessEvent(_In_ PWPDMTP_DEVICE Dev, _In_ PWPDMTP_EVENT Event);
