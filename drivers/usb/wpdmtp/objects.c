/*
 * PROJECT:     NextNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     MTP device, storage and object information cache
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"

#define NDEBUG
#include <debug.h>

static const GUID FormatBase = { 0x00000000, 0xAE6C, 0x4804, { 0x98, 0xBA, 0xC5, 0x7B, 0x46, 0x96, 0x5F, 0xE7 } };

VOID
WpdMtpFormatGuid(
    _In_ USHORT Format,
    _Out_ GUID *Guid)
{
    *Guid = FormatBase;
    Guid->Data1 = (ULONG)Format << 16;
}

BOOLEAN
WpdMtpFormatFromGuid(
    _In_ const GUID *Guid,
    _Out_ PUSHORT Format)
{
    *Format = 0;
    if ((Guid->Data1 & 0xFFFF) || Guid->Data2 != FormatBase.Data2 || Guid->Data3 != FormatBase.Data3 ||
        !RtlEqualMemory(Guid->Data4, FormatBase.Data4, sizeof(Guid->Data4)))
    {
        return FALSE;
    }
    *Format = (USHORT)(Guid->Data1 >> 16);
    return TRUE;
}

const GUID *
WpdMtpContentType(
    _In_ USHORT Format)
{
    switch (Format)
    {
        case MTP_FORMAT_ASSOCIATION:
            return &WPD_CONTENT_TYPE_FOLDER;
        case 0x3004:
        case 0x3005:
        case 0xBA83:
        case 0xBA84:
        case 0xBA85:
            return &WPD_CONTENT_TYPE_DOCUMENT;
        case 0x3007:
        case 0x3008:
        case 0x3009:
        case 0xB901:
        case 0xB902:
        case 0xB903:
        case 0xB904:
        case 0xB906:
        case 0xB907:
        case 0xB908:
            return &WPD_CONTENT_TYPE_AUDIO;
        case 0x300A:
        case 0x300B:
        case 0x300C:
        case 0xB980:
        case 0xB981:
        case 0xB982:
        case 0xB983:
        case 0xB984:
            return &WPD_CONTENT_TYPE_VIDEO;
        case 0xBA05:
        case 0xBA10:
        case 0xBA11:
        case 0xBA12:
        case 0xBA13:
        case 0xBA14:
        case 0xBA15:
            return &WPD_CONTENT_TYPE_PLAYLIST;
        case 0xBA03:
            return &WPD_CONTENT_TYPE_AUDIO_ALBUM;
        case 0xBA01:
            return &WPD_CONTENT_TYPE_MIXED_CONTENT_ALBUM;
        case 0xBA81:
        case 0xBB82:
            return &WPD_CONTENT_TYPE_CONTACT;
        default:
            if ((Format & 0xF800) == 0x3800 || (Format & 0xFF80) == 0xB880)
                return &WPD_CONTENT_TYPE_IMAGE;
            return &WPD_CONTENT_TYPE_UNSPECIFIED;
    }
}

USHORT
WpdMtpFormatForContentType(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ const GUID *ContentType)
{
    ULONG Index;

    if (RtlEqualMemory(ContentType, &WPD_CONTENT_TYPE_FOLDER, sizeof(GUID)))
        return MTP_FORMAT_ASSOCIATION;
    for (Index = 0; Index < Dev->FormatCount; Index++)
    {
        if (Dev->Formats[Index].Code != MTP_FORMAT_ASSOCIATION &&
            RtlEqualMemory(WpdMtpContentType(Dev->Formats[Index].Code), ContentType, sizeof(GUID)))
        {
            return Dev->Formats[Index].Code;
        }
    }
    return MTP_FORMAT_UNDEFINED;
}

static
NTSTATUS
WpdMtpQuery(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code,
    _In_ ULONG ParamCount,
    _In_ ULONG P1,
    _In_ ULONG P2,
    _In_ ULONG P3,
    _Out_ PMTP_OPERATION Op)
{
    NTSTATUS Status;

    RtlZeroMemory(Op, sizeof(*Op));
    Op->Code = Code;
    Op->ParamCount = ParamCount;
    Op->Params[0] = P1;
    Op->Params[1] = P2;
    Op->Params[2] = P3;
    Op->DataIn = TRUE;
    Status = WpdMtpTransaction(Dev, Op);
    if (NT_SUCCESS(Status) && Op->ResponseCode != MTP_RC_OK)
        Status = STATUS_UNSUCCESSFUL;
    return Status;
}

static
USHORT
WpdMtpKnownDevicePropType(
    _In_ USHORT Code)
{
    switch (Code)
    {
        case MTP_DEVICE_PROP_FRIENDLY_NAME:
            return MTP_TYPE_STRING;
        case MTP_DEVICE_PROP_BATTERY_LEVEL:
            return MTP_TYPE_UINT8;
        case MTP_DEVICE_PROP_PERCEIVED_TYPE:
            return MTP_TYPE_UINT32;
        default:
            return 0;
    }
}

PMTP_DEVICE_PROP
WpdMtpGetDeviceProp(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code,
    _In_ BOOLEAN Refresh)
{
    PMTP_DEVICE_PROP Prop = NULL;
    MTP_OPERATION Op;
    MTP_DATASET Set;
    ULONG Index;

    if (!Dev->DevicePropInfo)
        return NULL;
    for (Index = 0; Index < Dev->DevicePropCount; Index++)
    {
        if (Dev->DeviceProps[Index] == Code)
        {
            Prop = &Dev->DevicePropInfo[Index];
            break;
        }
    }
    if (!Prop)
        return NULL;
    if (Prop->Valid && !Refresh)
        return Prop;

    if (WpdMtpSupportsOperation(Dev, MTP_OP_GET_DEVICE_PROP_DESC))
    {
        if (!NT_SUCCESS(WpdMtpQuery(Dev, MTP_OP_GET_DEVICE_PROP_DESC, 1, Code, 0, 0, &Op)))
            return Prop->Valid ? Prop : NULL;
        MtpFreeValue(&Prop->Default);
        MtpFreeValue(&Prop->Current);
        MtpFreeForm(&Prop->Form);
        RtlZeroMemory(&Set, sizeof(Set));
        Set.Data = Op.InData;
        Set.Size = Op.InLength;
        Prop->Code = MtpReadU16(&Set);
        Prop->DataType = MtpReadU16(&Set);
        Prop->GetSet = MtpReadU8(&Set);
        MtpReadValue(&Set, Prop->DataType, &Prop->Default);
        MtpReadValue(&Set, Prop->DataType, &Prop->Current);
        MtpReadForm(&Set, Prop->DataType, &Prop->Form);
        Prop->Valid = Prop->Code == Code && Prop->Current.Valid;
        ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
        return Prop->Valid ? Prop : NULL;
    }

    if (!WpdMtpKnownDevicePropType(Code) ||
        !NT_SUCCESS(WpdMtpQuery(Dev, MTP_OP_GET_DEVICE_PROP_VALUE, 1, Code, 0, 0, &Op)))
    {
        return Prop->Valid ? Prop : NULL;
    }
    MtpFreeValue(&Prop->Current);
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    Prop->Code = Code;
    Prop->DataType = WpdMtpKnownDevicePropType(Code);
    MtpReadValue(&Set, Prop->DataType, &Prop->Current);
    Prop->Valid = Prop->Current.Valid;
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    return Prop->Valid ? Prop : NULL;
}

static
VOID
WpdMtpLoadDeviceProps(
    _In_ PWPDMTP_DEVICE Dev)
{
    PMTP_DEVICE_PROP Prop;
    ULONG Index;

    Dev->PerceivedType = 2;
    Dev->PowerLevelValid = FALSE;
    if (!Dev->DevicePropCount)
        return;
    Dev->DevicePropInfo = ExAllocatePoolZero(PagedPool, Dev->DevicePropCount * sizeof(MTP_DEVICE_PROP), WPDMTP_TAG);
    if (!Dev->DevicePropInfo)
        return;
    for (Index = 0; Index < Dev->DevicePropCount; Index++)
        Dev->DevicePropInfo[Index].Code = Dev->DeviceProps[Index];

    Prop = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_FRIENDLY_NAME, TRUE);
    if (Prop && Prop->DataType == MTP_TYPE_STRING && Prop->Current.String && Prop->Current.String[0])
        Dev->FriendlyName = WpdMtpDuplicateString(Prop->Current.String);
    Prop = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_PERCEIVED_TYPE, TRUE);
    if (Prop && Prop->DataType != MTP_TYPE_STRING)
        Dev->PerceivedType = (ULONG)MtpValueInteger(&Prop->Current);
    Prop = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_BATTERY_LEVEL, TRUE);
    if (Prop && Prop->DataType != MTP_TYPE_STRING)
    {
        Dev->PowerLevel = (ULONG)MtpValueInteger(&Prop->Current);
        Dev->PowerLevelValid = TRUE;
    }
}

NTSTATUS
WpdMtpLoadFormatProps(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PMTP_FORMAT Format)
{
    MTP_OPERATION Op;
    MTP_DATASET Set;
    NTSTATUS Status;

    if (Format->PropsLoaded)
        return STATUS_SUCCESS;
    Format->PropsLoaded = TRUE;
    if (!WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROPS_SUPPORTED))
        return STATUS_SUCCESS;
    Status = WpdMtpQuery(Dev, MTP_OP_GET_OBJECT_PROPS_SUPPORTED, 1, Format->Code, 0, 0, &Op);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    Format->Props = MtpReadU16Array(&Set, &Format->PropCount);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    return STATUS_SUCCESS;
}

PMTP_FORMAT
WpdMtpFindFormat(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code)
{
    ULONG Index;

    for (Index = 0; Index < Dev->FormatCount; Index++)
    {
        if (Dev->Formats[Index].Code == Code)
        {
            WpdMtpLoadFormatProps(Dev, &Dev->Formats[Index]);
            return &Dev->Formats[Index];
        }
    }
    return NULL;
}

BOOLEAN
WpdMtpFormatHasProp(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _In_ USHORT Prop)
{
    PMTP_FORMAT Entry = WpdMtpFindFormat(Dev, Format);
    ULONG Index;

    if (!Entry)
        Entry = WpdMtpFindFormat(Dev, MTP_FORMAT_UNDEFINED);
    if (!Entry)
        return FALSE;
    for (Index = 0; Index < Entry->PropCount; Index++)
    {
        if (Entry->Props[Index] == Prop)
            return TRUE;
    }
    return FALSE;
}

PMTP_PROP_DESC
WpdMtpGetPropDesc(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _In_ USHORT Prop)
{
    PLIST_ENTRY Entry;
    PMTP_PROP_DESC Desc;
    MTP_OPERATION Op;
    MTP_DATASET Set;

    for (Entry = Dev->PropDescs.Flink; Entry != &Dev->PropDescs; Entry = Entry->Flink)
    {
        Desc = CONTAINING_RECORD(Entry, MTP_PROP_DESC, Link);
        if (Desc->Format == Format && Desc->Code == Prop)
            return Desc->Valid ? Desc : NULL;
    }
    Desc = ExAllocatePoolZero(PagedPool, sizeof(*Desc), WPDMTP_TAG);
    if (!Desc)
        return NULL;
    Desc->Format = Format;
    Desc->Code = Prop;
    InsertTailList(&Dev->PropDescs, &Desc->Link);
    if (!WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROP_DESC) ||
        !NT_SUCCESS(WpdMtpQuery(Dev, MTP_OP_GET_OBJECT_PROP_DESC, 2, Prop, Format, 0, &Op)))
    {
        return NULL;
    }
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    MtpReadU16(&Set);
    Desc->DataType = MtpReadU16(&Set);
    Desc->GetSet = MtpReadU8(&Set);
    MtpReadValue(&Set, Desc->DataType, &Desc->Default);
    MtpReadU32(&Set);
    Desc->Valid = !Set.Overrun;
    MtpReadForm(&Set, Desc->DataType, &Desc->Form);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    return Desc->Valid ? Desc : NULL;
}

NTSTATUS
WpdMtpLoadDevice(
    _In_ PWPDMTP_DEVICE Dev)
{
    MTP_OPERATION Op;
    MTP_DATASET Set;
    PUSHORT Formats;
    ULONG Index, Count;
    NTSTATUS Status;

    Status = WpdMtpOpenSession(Dev);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = WpdMtpQuery(Dev, MTP_OP_GET_DEVICE_INFO, 0, 0, 0, 0, &Op);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WPDMTP: GetDeviceInfo failed 0x%08lx response 0x%04x\n", Status, Op.ResponseCode);
        return NT_SUCCESS(Status) ? STATUS_DEVICE_PROTOCOL_ERROR : Status;
    }

    WpdMtpFreeDeviceInfo(Dev);
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    Dev->StandardVersion = MtpReadU16(&Set);
    Dev->VendorExtensionId = MtpReadU32(&Set);
    Dev->VendorExtensionVersion = MtpReadU16(&Set);
    Dev->VendorExtensionDesc = MtpReadString(&Set);
    MtpReadU16(&Set);
    Dev->Operations = MtpReadU16Array(&Set, &Dev->OperationCount);
    Dev->Events = MtpReadU16Array(&Set, &Dev->EventCount);
    Dev->DeviceProps = MtpReadU16Array(&Set, &Dev->DevicePropCount);
    Formats = MtpReadU16Array(&Set, &Count);
    if (Formats)
        ExFreePoolWithTag(Formats, WPDMTP_TAG);
    Formats = MtpReadU16Array(&Set, &Count);
    Dev->Manufacturer = MtpReadString(&Set);
    Dev->Model = MtpReadString(&Set);
    Dev->DeviceVersion = MtpReadString(&Set);
    Dev->SerialNumber = MtpReadString(&Set);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    if (Set.Overrun || !Formats)
    {
        if (Formats)
            ExFreePoolWithTag(Formats, WPDMTP_TAG);
        DPRINT1("WPDMTP: DeviceInfo dataset is truncated\n");
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    Dev->Formats = ExAllocatePoolZero(PagedPool, max(Count, 1) * sizeof(MTP_FORMAT), WPDMTP_TAG);
    if (!Dev->Formats)
    {
        ExFreePoolWithTag(Formats, WPDMTP_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (Index = 0; Index < Count; Index++)
        Dev->Formats[Index].Code = Formats[Index];
    Dev->FormatCount = Count;
    ExFreePoolWithTag(Formats, WPDMTP_TAG);

    DPRINT1("WPDMTP: \"%S\" \"%S\" version \"%S\", %lu operations, %lu formats\n",
            Dev->Manufacturer, Dev->Model, Dev->DeviceVersion, Dev->OperationCount, Dev->FormatCount);
    for (Index = 0; Index < Dev->FormatCount; Index++)
        WpdMtpLoadFormatProps(Dev, &Dev->Formats[Index]);
    WpdMtpLoadDeviceProps(Dev);
    return WpdMtpLoadStorages(Dev);
}

static
VOID
WpdMtpFreeStorage(
    _In_ PMTP_STORAGE Storage)
{
    if (Storage->Description)
        ExFreePoolWithTag(Storage->Description, WPDMTP_TAG);
    if (Storage->VolumeIdentifier)
        ExFreePoolWithTag(Storage->VolumeIdentifier, WPDMTP_TAG);
    ExFreePoolWithTag(Storage, WPDMTP_TAG);
}

NTSTATUS
WpdMtpLoadStorages(
    _In_ PWPDMTP_DEVICE Dev)
{
    MTP_OPERATION Op, InfoOp;
    MTP_DATASET Set;
    PULONG Ids;
    ULONG Count, Index;
    PMTP_STORAGE Storage;
    NTSTATUS Status;

    while (!IsListEmpty(&Dev->Storages))
        WpdMtpFreeStorage(CONTAINING_RECORD(RemoveHeadList(&Dev->Storages), MTP_STORAGE, Link));
    Dev->StoragesLoaded = FALSE;

    Status = WpdMtpQuery(Dev, MTP_OP_GET_STORAGE_IDS, 0, 0, 0, 0, &Op);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    Ids = MtpReadU32Array(&Set, &Count);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    if (!Ids)
        return STATUS_DEVICE_PROTOCOL_ERROR;

    for (Index = 0; Index < Count; Index++)
    {
        if (!(Ids[Index] & 0xFFFF))
            continue;
        if (!NT_SUCCESS(WpdMtpQuery(Dev, MTP_OP_GET_STORAGE_INFO, 1, Ids[Index], 0, 0, &InfoOp)))
            continue;
        Storage = ExAllocatePoolZero(PagedPool, sizeof(*Storage), WPDMTP_TAG);
        if (Storage)
        {
            RtlZeroMemory(&Set, sizeof(Set));
            Set.Data = InfoOp.InData;
            Set.Size = InfoOp.InLength;
            Storage->Id = Ids[Index];
            Storage->StorageType = MtpReadU16(&Set);
            Storage->FilesystemType = MtpReadU16(&Set);
            Storage->AccessCapability = MtpReadU16(&Set);
            Storage->MaxCapacity = MtpReadU64(&Set);
            Storage->FreeSpaceInBytes = MtpReadU64(&Set);
            Storage->FreeSpaceInObjects = MtpReadU32(&Set);
            Storage->Description = MtpReadString(&Set);
            Storage->VolumeIdentifier = MtpReadString(&Set);
            InsertTailList(&Dev->Storages, &Storage->Link);
        }
        ExFreePoolWithTag(InfoOp.InData, WPDMTP_TAG);
    }
    ExFreePoolWithTag(Ids, WPDMTP_TAG);
    Dev->StoragesLoaded = TRUE;
    return STATUS_SUCCESS;
}

PMTP_STORAGE
WpdMtpFindStorage(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Id)
{
    PLIST_ENTRY Entry;

    if (!Dev->StoragesLoaded)
        WpdMtpLoadStorages(Dev);
    for (Entry = Dev->Storages.Flink; Entry != &Dev->Storages; Entry = Entry->Flink)
    {
        PMTP_STORAGE Storage = CONTAINING_RECORD(Entry, MTP_STORAGE, Link);

        if (Storage->Id == Id)
            return Storage;
    }
    return NULL;
}

static
VOID
WpdMtpFreeObject(
    _In_ PMTP_OBJECT Object)
{
    PWCHAR *Strings[] = { &Object->FileName, &Object->Name, &Object->DateCreated, &Object->DateModified };
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(Strings); Index++)
    {
        if (*Strings[Index])
            ExFreePoolWithTag(*Strings[Index], WPDMTP_TAG);
    }
    for (Index = 0; Index < Object->PropCount; Index++)
        MtpFreeValue(&Object->Props[Index].Value);
    if (Object->Props)
        ExFreePoolWithTag(Object->Props, WPDMTP_TAG);
    ExFreePoolWithTag(Object, WPDMTP_TAG);
}

VOID
WpdMtpForgetObject(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Handle)
{
    PLIST_ENTRY Entry;

    for (Entry = Dev->Objects.Flink; Entry != &Dev->Objects; Entry = Entry->Flink)
    {
        PMTP_OBJECT Object = CONTAINING_RECORD(Entry, MTP_OBJECT, Link);

        if (Object->Handle == Handle)
        {
            RemoveEntryList(&Object->Link);
            WpdMtpFreeObject(Object);
            return;
        }
    }
}

VOID
WpdMtpForgetAll(
    _In_ PWPDMTP_DEVICE Dev)
{
    while (!IsListEmpty(&Dev->Objects))
        WpdMtpFreeObject(CONTAINING_RECORD(RemoveHeadList(&Dev->Objects), MTP_OBJECT, Link));
}

static
NTSTATUS
WpdMtpLoadObjectInfo(
    _In_ PWPDMTP_DEVICE Dev,
    _Inout_ PMTP_OBJECT Object,
    _Out_ PUSHORT Response)
{
    MTP_OPERATION Op;
    MTP_DATASET Set;
    ULONG Size;
    NTSTATUS Status;

    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_GET_OBJECT_INFO;
    Op.ParamCount = 1;
    Op.Params[0] = Object->Handle;
    Op.DataIn = TRUE;
    Status = WpdMtpTransaction(Dev, &Op);
    *Response = Op.ResponseCode;
    if (!NT_SUCCESS(Status) || Op.ResponseCode != MTP_RC_OK)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    Object->StorageId = MtpReadU32(&Set);
    Object->Format = MtpReadU16(&Set);
    Object->ProtectionStatus = MtpReadU16(&Set);
    Size = MtpReadU32(&Set);
    Object->Size = Size;
    MtpReadU16(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    Object->Parent = MtpReadU32(&Set);
    MtpReadU16(&Set);
    MtpReadU32(&Set);
    MtpReadU32(&Set);
    Object->FileName = MtpReadString(&Set);
    Object->DateCreated = MtpReadString(&Set);
    Object->DateModified = MtpReadString(&Set);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    if (!Object->FileName)
        return STATUS_DEVICE_PROTOCOL_ERROR;
    Object->InfoLoaded = TRUE;
    if (Size == MAXULONG)
        Object->Size = MAXULONGLONG;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WpdMtpGetPropValue(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Handle,
    _In_ USHORT Prop,
    _Out_ PMTP_OPERATION Op)
{
    RtlZeroMemory(Op, sizeof(*Op));
    Op->Code = MTP_OP_GET_OBJECT_PROP_VALUE;
    Op->ParamCount = 2;
    Op->Params[0] = Handle;
    Op->Params[1] = Prop;
    Op->DataIn = TRUE;
    return WpdMtpTransaction(Dev, Op);
}

const MTP_VALUE *
WpdMtpObjectProp(
    _In_ PMTP_OBJECT Object,
    _In_ USHORT Code)
{
    ULONG Index;

    for (Index = 0; Index < Object->PropCount; Index++)
    {
        if (Object->Props[Index].Code == Code)
            return Object->Props[Index].Value.Valid ? &Object->Props[Index].Value : NULL;
    }
    return NULL;
}

static
VOID
WpdMtpReplaceString(
    _Inout_ PWCHAR *Target,
    _In_opt_ PCWSTR Value)
{
    PWCHAR Copy;

    if (!Value)
        return;
    Copy = WpdMtpDuplicateString(Value);
    if (!Copy)
        return;
    if (*Target)
        ExFreePoolWithTag(*Target, WPDMTP_TAG);
    *Target = Copy;
}

static
VOID
WpdMtpApplyProp(
    _Inout_ PMTP_OBJECT Object,
    _In_ USHORT Prop,
    _In_ USHORT DataType,
    _Inout_ PMTP_DATASET Set)
{
    PMTP_OBJECT_PROP Props;
    MTP_VALUE Value;
    ULONG Index;

    if (!MtpReadValue(Set, DataType, &Value))
    {
        MtpFreeValue(&Value);
        return;
    }
    for (Index = 0; Index < Object->PropCount && Object->Props[Index].Code != Prop; Index++);
    if (Index == Object->PropCount)
    {
        Props = ExAllocatePoolZero(PagedPool, (Object->PropCount + 1) * sizeof(MTP_OBJECT_PROP), WPDMTP_TAG);
        if (!Props)
        {
            MtpFreeValue(&Value);
            return;
        }
        if (Object->Props)
        {
            RtlCopyMemory(Props, Object->Props, Object->PropCount * sizeof(MTP_OBJECT_PROP));
            ExFreePoolWithTag(Object->Props, WPDMTP_TAG);
        }
        Object->Props = Props;
        Object->Props[Index].Code = Prop;
        Object->PropCount++;
    }
    else
    {
        MtpFreeValue(&Object->Props[Index].Value);
    }
    Object->Props[Index].Value = Value;

    switch (Prop)
    {
        case MTP_PROP_STORAGE_ID:
            Object->StorageId = (ULONG)MtpValueInteger(&Value);
            break;
        case MTP_PROP_OBJECT_FORMAT:
            Object->Format = (USHORT)MtpValueInteger(&Value);
            break;
        case MTP_PROP_PROTECTION_STATUS:
            Object->ProtectionStatus = (USHORT)MtpValueInteger(&Value);
            break;
        case MTP_PROP_OBJECT_SIZE:
            Object->Size = (ULONGLONG)MtpValueInteger(&Value);
            break;
        case MTP_PROP_PARENT_OBJECT:
            Object->Parent = (ULONG)MtpValueInteger(&Value);
            break;
        case MTP_PROP_HIDDEN:
            Object->Hidden = MtpValueInteger(&Value) != 0;
            break;
        case MTP_PROP_SYSTEM_OBJECT:
            Object->System = MtpValueInteger(&Value) != 0;
            break;
        case MTP_PROP_NON_CONSUMABLE:
            Object->NonConsumable = MtpValueInteger(&Value) != 0;
            break;
        case MTP_PROP_PERSISTENT_UID:
            if (DataType == MTP_TYPE_UINT128 || DataType == MTP_TYPE_INT128)
            {
                RtlCopyMemory(Object->Puid, Value.Raw, sizeof(Object->Puid));
                Object->PuidValid = TRUE;
            }
            break;
        case MTP_PROP_NAME:
            WpdMtpReplaceString(&Object->Name, Value.String);
            break;
        case MTP_PROP_OBJECT_FILE_NAME:
            WpdMtpReplaceString(&Object->FileName, Value.String);
            break;
        case MTP_PROP_DATE_CREATED:
            WpdMtpReplaceString(&Object->DateCreated, Value.String);
            break;
        case MTP_PROP_DATE_MODIFIED:
            WpdMtpReplaceString(&Object->DateModified, Value.String);
            break;
    }
}

static
NTSTATUS
WpdMtpLoadObjectProps(
    _In_ PWPDMTP_DEVICE Dev,
    _Inout_ PMTP_OBJECT Object)
{
    PMTP_FORMAT Format;
    PMTP_PROP_DESC Desc;
    MTP_OPERATION Op;
    MTP_DATASET Set;
    ULONG Index, Count;
    NTSTATUS Status;

    Object->PropsLoaded = TRUE;
    if (WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROP_LIST))
    {
        RtlZeroMemory(&Op, sizeof(Op));
        Op.Code = MTP_OP_GET_OBJECT_PROP_LIST;
        Op.ParamCount = 5;
        Op.Params[0] = Object->Handle;
        Op.Params[2] = MAXULONG;
        Op.DataIn = TRUE;
        Status = WpdMtpTransaction(Dev, &Op);
        if (NT_SUCCESS(Status) && Op.ResponseCode == MTP_RC_OK)
        {
            RtlZeroMemory(&Set, sizeof(Set));
            Set.Data = Op.InData;
            Set.Size = Op.InLength;
            Count = MtpReadU32(&Set);
            for (Index = 0; Index < Count && !Set.Overrun; Index++)
            {
                ULONG Handle = MtpReadU32(&Set);
                USHORT Prop = MtpReadU16(&Set);
                USHORT Type = MtpReadU16(&Set);
                ULONG Before = Set.Offset;

                if (Handle == Object->Handle)
                {
                    WpdMtpApplyProp(Object, Prop, Type, &Set);
                    if (Set.Offset == Before)
                        MtpSkipValue(&Set, Type);
                }
                else
                {
                    MtpSkipValue(&Set, Type);
                }
            }
            ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
            return STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(Status))
            return Status;
    }
    if (!WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROP_VALUE))
        return STATUS_SUCCESS;
    Format = WpdMtpFindFormat(Dev, Object->Format);
    if (!Format)
        Format = WpdMtpFindFormat(Dev, MTP_FORMAT_UNDEFINED);
    for (Index = 0; Format && Index < Format->PropCount; Index++)
    {
        USHORT Prop = Format->Props[Index];

        if (Prop == MTP_PROP_STORAGE_ID || Prop == MTP_PROP_OBJECT_FORMAT || Prop == MTP_PROP_PARENT_OBJECT)
            continue;
        Desc = WpdMtpGetPropDesc(Dev, Object->Format, Prop);
        if (!Desc)
            continue;
        Status = WpdMtpGetPropValue(Dev, Object->Handle, Prop, &Op);
        if (!NT_SUCCESS(Status))
            return Status;
        if (Op.ResponseCode != MTP_RC_OK)
            continue;
        RtlZeroMemory(&Set, sizeof(Set));
        Set.Data = Op.InData;
        Set.Size = Op.InLength;
        WpdMtpApplyProp(Object, Prop, Desc->DataType, &Set);
        ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    }
    return STATUS_SUCCESS;
}

HRESULT
WpdMtpGetObject(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Handle,
    _In_ BOOLEAN Properties,
    _Out_ PMTP_OBJECT *Result)
{
    PLIST_ENTRY Entry;
    PMTP_OBJECT Object = NULL;
    USHORT Response = 0;
    NTSTATUS Status;

    *Result = NULL;
    for (Entry = Dev->Objects.Flink; Entry != &Dev->Objects; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, MTP_OBJECT, Link)->Handle == Handle)
        {
            Object = CONTAINING_RECORD(Entry, MTP_OBJECT, Link);
            break;
        }
    }
    if (!Object)
    {
        Object = ExAllocatePoolZero(PagedPool, sizeof(*Object), WPDMTP_TAG);
        if (!Object)
            return WPD_E_OUTOFMEMORY;
        Object->Handle = Handle;
        Status = WpdMtpLoadObjectInfo(Dev, Object, &Response);
        if (!NT_SUCCESS(Status))
        {
            WpdMtpFreeObject(Object);
            return WpdMtpResultToHresult(Status == STATUS_UNSUCCESSFUL ? STATUS_SUCCESS : Status, Response);
        }
        InsertHeadList(&Dev->Objects, &Object->Link);
    }
    if (Properties && !Object->PropsLoaded)
    {
        Status = WpdMtpLoadObjectProps(Dev, Object);
        if (!NT_SUCCESS(Status))
            return WpdMtpResultToHresult(Status, 0);
    }
    *Result = Object;
    return WPD_S_OK;
}

HRESULT
WpdMtpGetChildren(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG StorageId,
    _In_ ULONG Parent,
    _Out_ PULONG *Handles,
    _Out_ PULONG Count)
{
    MTP_OPERATION Op;
    MTP_DATASET Set;
    NTSTATUS Status;

    *Handles = NULL;
    *Count = 0;
    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_GET_OBJECT_HANDLES;
    Op.ParamCount = 3;
    Op.Params[0] = StorageId;
    Op.Params[1] = 0;
    Op.Params[2] = Parent;
    Op.DataIn = TRUE;
    Status = WpdMtpTransaction(Dev, &Op);
    if (!NT_SUCCESS(Status) || Op.ResponseCode != MTP_RC_OK)
        return WpdMtpResultToHresult(Status, Op.ResponseCode);
    RtlZeroMemory(&Set, sizeof(Set));
    Set.Data = Op.InData;
    Set.Size = Op.InLength;
    *Handles = MtpReadU32Array(&Set, Count);
    ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    return *Handles ? WPD_S_OK : WPD_E_GEN_FAILURE;
}

VOID
WpdMtpFreeCache(
    _In_ PWPDMTP_DEVICE Dev)
{
    WpdMtpForgetAll(Dev);
    while (!IsListEmpty(&Dev->Storages))
        WpdMtpFreeStorage(CONTAINING_RECORD(RemoveHeadList(&Dev->Storages), MTP_STORAGE, Link));
    while (!IsListEmpty(&Dev->PropDescs))
    {
        PMTP_PROP_DESC Desc = CONTAINING_RECORD(RemoveHeadList(&Dev->PropDescs), MTP_PROP_DESC, Link);

        MtpFreeValue(&Desc->Default);
        MtpFreeForm(&Desc->Form);
        ExFreePoolWithTag(Desc, WPDMTP_TAG);
    }
    Dev->StoragesLoaded = FALSE;
    WpdMtpFreeDeviceInfo(Dev);
}
