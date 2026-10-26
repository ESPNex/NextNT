/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WPD commands: resources, object management and MTP vendor operations
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"
#include "wpdcmd.h"

#define NDEBUG
#include <debug.h>

#define WPDMTP_MAX_READ        (8 * 1024 * 1024)
#define WPDMTP_RESULT_OVERHEAD 1024

static
BOOLEAN
WpdIsDefaultResource(
    _In_ PWPD_COLLECTION Params)
{
    PROPERTYKEY Key;

    return WpdValuesGetKey(Params, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_KEYS, &Key) &&
           WpdKeyEqual(&Key, &WPD_RESOURCE_DEFAULT);
}

static
ULONG
WpdReadLimit(
    _In_ ULONG Requested,
    _In_ ULONG OutputLimit)
{
    ULONG Limit = OutputLimit > WPDMTP_RESULT_OVERHEAD ? OutputLimit - WPDMTP_RESULT_OVERHEAD : 0;

    return min(min(Requested, Limit), WPDMTP_MAX_READ);
}

static
HRESULT
WpdReadPartial(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_CONTEXT Context,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Read)
{
    MTP_OPERATION Op;
    NTSTATUS Status;

    *Read = 0;
    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_GET_PARTIAL_OBJECT;
    Op.ParamCount = 3;
    Op.Params[0] = Context->Handle;
    Op.Params[1] = (ULONG)Context->Offset;
    Op.Params[2] = Length;
    Op.DataIn = TRUE;
    Status = WpdMtpTransaction(Dev, &Op);
    if (!NT_SUCCESS(Status) || Op.ResponseCode != MTP_RC_OK)
        return WpdMtpResultToHresult(Status, Op.ResponseCode);
    *Read = min(Op.InLength, Length);
    if (*Read)
        RtlCopyMemory(Buffer, Op.InData, *Read);
    if (Op.InData)
        ExFreePoolWithTag(Op.InData, WPDMTP_TAG);
    return WPD_S_OK;
}

static
HRESULT
WpdReadStream(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_CONTEXT Context,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Read)
{
    ULONGLONG Total;
    USHORT Response;
    NTSTATUS Status;

    *Read = 0;
    if (!Context->Streaming)
    {
        Status = WpdMtpBeginDataIn(Dev, MTP_OP_GET_OBJECT, 1, &Context->Handle, &Total);
        if (Status == STATUS_END_OF_FILE)
        {
            WpdMtpEndTransaction(Dev, &Response, NULL, NULL);
            return WpdMtpResultToHresult(STATUS_SUCCESS, Response == MTP_RC_OK ? MTP_RC_GENERAL_ERROR : Response);
        }
        if (!NT_SUCCESS(Status))
            return WpdMtpResultToHresult(Status, 0);
        Context->Streaming = TRUE;
        Dev->ActiveTransfer = Context;
    }
    Status = WpdMtpReadData(Dev, Buffer, Length, Read);
    if (!NT_SUCCESS(Status))
    {
        Context->Streaming = FALSE;
        Dev->ActiveTransfer = NULL;
        return WpdMtpResultToHresult(Status, 0);
    }
    if (!Dev->DataInOpen)
    {
        Status = WpdMtpEndTransaction(Dev, &Response, NULL, NULL);
        Context->Streaming = FALSE;
        Context->Total = Context->Offset + *Read;
        Dev->ActiveTransfer = NULL;
        if (!NT_SUCCESS(Status) || Response != MTP_RC_OK)
            return WpdMtpResultToHresult(Status, Response);
    }
    return WPD_S_OK;
}

HRESULT
WpdResources(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _In_ ULONG OutputLimit,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPDMTP_CONTEXT Context;
    PWPD_COLLECTION Collection;
    WPD_OBJECT_REF Ref;
    GUID Format;
    ULONG Requested, Read = 0;
    PUCHAR Buffer;
    HRESULT hr;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Id == WPD_COMMAND_OBJECT_RESOURCES_GET_SUPPORTED.pid ||
        Id == WPD_COMMAND_OBJECT_RESOURCES_GET_ATTRIBUTES.pid ||
        Id == WPD_COMMAND_OBJECT_RESOURCES_OPEN.pid)
    {
        hr = WpdResolveObject(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_RESOURCES_OBJECT_ID), &Ref);
        if (FAILED(hr))
            return hr;
        if (Id == WPD_COMMAND_OBJECT_RESOURCES_GET_SUPPORTED.pid)
        {
            Collection = WpdCollectionCreate(WpdKeys);
            if (!Collection)
                return WPD_E_OUTOFMEMORY;
            if (Ref.Kind == WpdRefObject)
                Status = WpdKeysAdd(Collection, &WPD_RESOURCE_DEFAULT);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_KEYS, Collection);
            else
                WpdCollectionFree(Collection);
            return WpdNtToHresult(Status);
        }
        if (Ref.Kind != WpdRefObject || !WpdIsDefaultResource(Params))
            return WPD_E_INVALIDARG;
        if (Id == WPD_COMMAND_OBJECT_RESOURCES_GET_ATTRIBUTES.pid)
        {
            Collection = WpdCollectionCreate(WpdValues);
            if (!Collection)
                return WPD_E_OUTOFMEMORY;
            WpdMtpFormatGuid(Ref.Object->Format, &Format);
            Status = WpdValuesSetUi8(Collection, &WPD_RESOURCE_ATTRIBUTE_TOTAL_SIZE, Ref.Object->Size);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetGuid(Collection, &WPD_RESOURCE_ATTRIBUTE_FORMAT, &Format);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetBool(Collection, &WPD_RESOURCE_ATTRIBUTE_CAN_READ, TRUE);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetBool(Collection, &WPD_RESOURCE_ATTRIBUTE_CAN_WRITE, FALSE);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetBool(Collection, &WPD_RESOURCE_ATTRIBUTE_CAN_DELETE, FALSE);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetUi4(Collection, &WPD_RESOURCE_ATTRIBUTE_OPTIMAL_READ_BUFFER_SIZE, WPDMTP_OPTIMAL_TRANSFER);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_RESOURCES_RESOURCE_ATTRIBUTES, Collection);
            else
                WpdCollectionFree(Collection);
            return WpdNtToHresult(Status);
        }
        if (Ref.Object->Format == MTP_FORMAT_ASSOCIATION)
            return WPD_E_INVALIDARG;
        if (WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_RESOURCES_ACCESS_MODE, &Requested) && (Requested & 3))
            return WPD_E_ACCESSDENIED;
        Context = WpdCreateContext(Dev, File, WpdContextRead, L"CExistingObjectContext::CObjectReadContext");
        if (!Context)
            return WPD_E_OUTOFMEMORY;
        Context->Handle = Ref.Object->Handle;
        Context->Total = Ref.Object->Size;
        Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, Context->Name);
        if (NT_SUCCESS(Status))
            Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_OBJECT_RESOURCES_OPTIMAL_TRANSFER_BUFFER_SIZE,
                                     (ULONG)min(Context->Total, WPDMTP_OPTIMAL_TRANSFER));
        if (NT_SUCCESS(Status))
            Status = WpdValuesSetBool(Results, &WPD_PROPERTY_OBJECT_RESOURCES_SUPPORTS_UNITS, FALSE);
        if (!NT_SUCCESS(Status))
            WpdFreeContext(Dev, Context);
        return WpdNtToHresult(Status);
    }

    Context = WpdFindContext(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT), WpdContextRead);
    if (!Context)
        return WPD_E_NOT_FOUND;

    switch (Id)
    {
        case 5:
            if (!WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_TO_READ, &Requested))
                return WPD_E_INVALIDARG;
            Requested = WpdReadLimit(Requested, OutputLimit);
            if (Context->Total != MAXULONGLONG)
                Requested = (ULONG)min(Requested, Context->Total - min(Context->Offset, Context->Total));
            Buffer = ExAllocatePoolWithTag(PagedPool, max(Requested, 1), WPDMTP_TAG);
            if (!Buffer)
                return WPD_E_OUTOFMEMORY;
            hr = WPD_S_OK;
            if (Requested)
            {
                if (!Context->Streaming && Context->Offset + Requested <= MAXULONG &&
                    WpdMtpSupportsOperation(Dev, MTP_OP_GET_PARTIAL_OBJECT))
                {
                    hr = WpdReadPartial(Dev, Context, Buffer, Requested, &Read);
                }
                else
                {
                    hr = WpdReadStream(Dev, Context, Buffer, Requested, &Read);
                }
            }
            if (SUCCEEDED(hr))
            {
                Context->Offset += Read;
                Status = WpdValuesSetBuffer(Results, &WPD_PROPERTY_OBJECT_RESOURCES_DATA, Buffer, Read);
                if (NT_SUCCESS(Status))
                    Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_OBJECT_RESOURCES_NUM_BYTES_READ, Read);
                if (NT_SUCCESS(Status))
                    Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, Context->Name);
                hr = WpdNtToHresult(Status);
            }
            ExFreePoolWithTag(Buffer, WPDMTP_TAG);
            return hr;
        case 7:
            WpdFreeContext(Dev, Context);
            return WPD_S_OK;
        case 6:
        case 10:
        case 12:
            WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, Context->Name);
            return WPD_E_ACCESSDENIED;
        default:
            return WPD_E_NOTIMPL;
    }
}

static
HRESULT
WpdSendObjectInfo(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Properties,
    _In_ BOOLEAN PropertiesOnly,
    _Out_ PULONG Handle,
    _Out_ PULONGLONG Size)
{
    WPD_OBJECT_REF Parent;
    MTP_OPERATION Op;
    GUID Guid;
    PCWSTR Name;
    USHORT Format = MTP_FORMAT_UNDEFINED;
    ULONG StorageId, ParentHandle;
    PUCHAR Data, Cursor;
    HRESULT hr;
    NTSTATUS Status;

    *Handle = 0;
    *Size = 0;
    hr = WpdResolveObject(Dev, WpdValuesGetString(Properties, &WPD_OBJECT_PARENT_ID), &Parent);
    if (FAILED(hr))
        return hr;
    if (Parent.Kind == WpdRefDevice)
        return WPD_E_INVALIDARG;
    if (Parent.Kind == WpdRefStorage)
    {
        StorageId = Parent.Value;
        ParentHandle = MTP_ROOT_PARENT;
    }
    else
    {
        if (Parent.Object->Format != MTP_FORMAT_ASSOCIATION)
            return WPD_E_INVALIDARG;
        StorageId = Parent.Object->StorageId;
        ParentHandle = Parent.Object->Handle;
    }
    Name = WpdValuesGetString(Properties, &WPD_OBJECT_ORIGINAL_FILE_NAME);
    if (!Name || !Name[0])
        Name = WpdValuesGetString(Properties, &WPD_OBJECT_NAME);
    if (!Name || !Name[0])
        return WPD_E_INVALIDARG;

    if (WpdValuesGetGuid(Properties, &WPD_OBJECT_FORMAT, &Guid) && WpdMtpFormatFromGuid(&Guid, &Format))
    {
        if (Format == MTP_FORMAT_ASSOCIATION && !PropertiesOnly)
            return WPD_E_INVALIDARG;
    }
    else if (WpdValuesGetGuid(Properties, &WPD_OBJECT_CONTENT_TYPE, &Guid))
    {
        Format = WpdMtpFormatForContentType(Dev, &Guid);
    }
    if (PropertiesOnly)
    {
        if (!WpdValuesGetGuid(Properties, &WPD_OBJECT_CONTENT_TYPE, &Guid) ||
            !RtlEqualMemory(&Guid, &WPD_CONTENT_TYPE_FOLDER, sizeof(GUID)))
        {
            if (Format != MTP_FORMAT_ASSOCIATION)
                return WPD_E_INVALIDARG;
        }
        Format = MTP_FORMAT_ASSOCIATION;
    }
    else if (!WpdValuesGetUi8(Properties, &WPD_OBJECT_SIZE, Size))
    {
        return WPD_E_INVALIDARG;
    }

    Data = ExAllocatePoolZero(PagedPool, 64 + MtpStringSize(Name), WPDMTP_TAG);
    if (!Data)
        return WPD_E_OUTOFMEMORY;
    Cursor = MtpWriteU32(Data, 0);
    Cursor = MtpWriteU16(Cursor, Format);
    Cursor = MtpWriteU16(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, *Size > MAXULONG ? MAXULONG : (ULONG)*Size);
    Cursor = MtpWriteU16(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU16(Cursor, Format == MTP_FORMAT_ASSOCIATION ? 1 : 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteU32(Cursor, 0);
    Cursor = MtpWriteString(Cursor, Name);
    Cursor = MtpWriteString(Cursor, NULL);
    Cursor = MtpWriteString(Cursor, NULL);
    Cursor = MtpWriteString(Cursor, NULL);

    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_SEND_OBJECT_INFO;
    Op.ParamCount = 2;
    Op.Params[0] = StorageId;
    Op.Params[1] = ParentHandle;
    Op.DataOut = Data;
    Op.DataOutLength = (ULONG)(Cursor - Data);
    Status = WpdMtpTransaction(Dev, &Op);
    ExFreePoolWithTag(Data, WPDMTP_TAG);
    if (!NT_SUCCESS(Status) || Op.ResponseCode != MTP_RC_OK)
        return WpdMtpResultToHresult(Status, Op.ResponseCode);
    if (Op.ResponseParamCount < 3)
        return WPD_E_GEN_FAILURE;
    *Handle = Op.ResponseParams[2];
    return WPD_S_OK;
}

static
HRESULT
WpdDeleteHandle(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Handle)
{
    MTP_OPERATION Op;
    NTSTATUS Status;

    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_DELETE_OBJECT;
    Op.ParamCount = 2;
    Op.Params[0] = Handle;
    Status = WpdMtpTransaction(Dev, &Op);
    WpdMtpForgetObject(Dev, Handle);
    return WpdMtpResultToHresult(Status, Op.ResponseCode);
}

static
HRESULT
WpdDeleteObject(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PMTP_OBJECT Object)
{
    MTP_OBJECT Snapshot;
    ULONG Handle = Object->Handle;
    HRESULT hr;

    RtlZeroMemory(&Snapshot, sizeof(Snapshot));
    Snapshot.Handle = Object->Handle;
    Snapshot.StorageId = Object->StorageId;
    Snapshot.Parent = Object->Parent;
    Snapshot.Format = Object->Format;
    Snapshot.PuidValid = Object->PuidValid;
    RtlCopyMemory(Snapshot.Puid, Object->Puid, sizeof(Snapshot.Puid));
    hr = WpdDeleteHandle(Dev, Handle);
    if (SUCCEEDED(hr))
        WpdMtpPostObjectEvent(Dev, WpdObjectEventRemoved, &Snapshot, NULL);
    else if (Snapshot.Format == MTP_FORMAT_ASSOCIATION && hr == WPD_E_MTP(0x2012))
        WpdMtpPostHandleEvent(Dev, WpdObjectEventHierarchy, Handle, NULL);
    return hr;
}

VOID
WpdUploadRevert(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_CONTEXT Context)
{
    if (Dev->ActiveTransfer == Context)
    {
        WpdMtpAbortData(Dev);
        Dev->ActiveTransfer = NULL;
    }
    if (Context->Handle)
        WpdDeleteHandle(Dev, Context->Handle);
    Context->Handle = 0;
}

static
HRESULT
WpdDeleteObjects(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Ids, Outcome;
    WPD_OBJECT_REF Ref;
    ULONG Options = 0, Index, Count;
    PULONG Children;
    HRESULT hr;
    BOOLEAN Partial = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    Ids = WpdValuesGetCollection(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_IDS, WpdVariants);
    if (!Ids)
        return WPD_E_INVALIDARG;
    WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_DELETE_OPTIONS, &Options);
    Outcome = WpdCollectionCreate(WpdVariants);
    if (!Outcome)
        return WPD_E_OUTOFMEMORY;
    for (Index = 0; Index < Ids->Count && NT_SUCCESS(Status); Index++)
    {
        PCWSTR Id = Ids->Items[Index].Vt == WPD_VT_LPWSTR ? Ids->Items[Index].String.Buffer : NULL;

        hr = WpdResolveObject(Dev, Id, &Ref);
        if (SUCCEEDED(hr) && Ref.Kind != WpdRefObject)
            hr = WPD_E_INVALIDARG;
        if (SUCCEEDED(hr) && Ref.Object->Format == MTP_FORMAT_ASSOCIATION &&
            Options != PORTABLE_DEVICE_DELETE_WITH_RECURSION)
        {
            hr = WpdMtpGetChildren(Dev, Ref.Object->StorageId, Ref.Object->Handle, &Children, &Count);
            if (SUCCEEDED(hr))
            {
                ExFreePoolWithTag(Children, WPDMTP_TAG);
                if (Count)
                    hr = (HRESULT)0x80070091L;
            }
        }
        if (SUCCEEDED(hr))
            hr = WpdDeleteObject(Dev, Ref.Object);
        if (FAILED(hr))
            Partial = TRUE;
        Status = WpdVariantsAddError(Outcome, hr);
    }
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_DELETE_RESULTS, Outcome);
    else
        WpdCollectionFree(Outcome);
    if (!NT_SUCCESS(Status))
        return WpdNtToHresult(Status);
    return Partial ? WPD_S_FALSE : WPD_S_OK;
}

HRESULT
WpdManagement(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Properties;
    PWPDMTP_CONTEXT Context;
    WCHAR Text[16];
    ULONG Handle, Written = 0, Length;
    ULONGLONG Size;
    PUCHAR Data;
    USHORT Response;
    HRESULT hr;
    NTSTATUS Status;

    switch (Id)
    {
        case 2:
        case 3:
            Properties = WpdValuesGetCollection(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CREATION_PROPERTIES, WpdValues);
            if (!Properties)
                return WPD_E_INVALIDARG;
            hr = WpdSendObjectInfo(Dev, Properties, Id == 2, &Handle, &Size);
            if (FAILED(hr))
                return hr;
            if (Id == 2)
            {
                WpdMtpPostHandleEvent(Dev, WpdObjectEventAdded, Handle, NULL);
                WpdFormatObjectId(Text, FALSE, Handle);
                return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_ID, Text));
            }
            Context = WpdCreateContext(Dev, File, WpdContextUpload, L"CNewObjectContext::CObjectUploadContext");
            if (!Context)
            {
                WpdDeleteHandle(Dev, Handle);
                return WPD_E_OUTOFMEMORY;
            }
            Context->Handle = Handle;
            Context->Total = Size;
            Status = WpdMtpBeginDataOut(Dev, MTP_OP_SEND_OBJECT, 0, NULL, Size);
            if (!NT_SUCCESS(Status))
            {
                WpdUploadRevert(Dev, Context);
                WpdFreeContext(Dev, Context);
                return WpdMtpResultToHresult(Status, 0);
            }
            Dev->ActiveTransfer = Context;
            Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, Context->Name);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OPTIMAL_TRANSFER_BUFFER_SIZE,
                                         WPDMTP_OPTIMAL_TRANSFER);
            return WpdNtToHresult(Status);
        case 7:
            return WpdDeleteObjects(Dev, Params, Results);
    }

    Context = WpdFindContext(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT), WpdContextUpload);
    if (!Context)
        return WPD_E_NOT_FOUND;
    switch (Id)
    {
        case 4:
            if (!WpdValuesGetBuffer(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_DATA, &Data, &Length))
                return WPD_E_INVALIDARG;
            if (WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_NUM_BYTES_TO_WRITE, &Written))
                Length = min(Length, Written);
            if (Context->Offset + Length > Context->Total || Dev->ActiveTransfer != Context)
                return WPD_E_INVALIDARG;
            Status = WpdMtpWriteData(Dev, Data, Length);
            if (!NT_SUCCESS(Status))
            {
                Dev->ActiveTransfer = NULL;
                Context->Failed = TRUE;
                return WpdMtpResultToHresult(Status, 0);
            }
            Context->Offset += Length;
            Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_NUM_BYTES_WRITTEN, Length);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT, Context->Name);
            return WpdNtToHresult(Status);
        case 5:
            if (Context->Failed || Context->Offset != Context->Total || Dev->DataOutOpen)
            {
                hr = Context->Failed ? WPD_E_GEN_FAILURE : WPD_E_UNEXPECTED;
                WpdUploadRevert(Dev, Context);
                WpdFreeContext(Dev, Context);
                return hr;
            }
            Status = WpdMtpEndTransaction(Dev, &Response, NULL, NULL);
            Dev->ActiveTransfer = NULL;
            hr = WpdMtpResultToHresult(Status, Response);
            Handle = Context->Handle;
            if (FAILED(hr))
            {
                WpdUploadRevert(Dev, Context);
                WpdFreeContext(Dev, Context);
                return hr;
            }
            Context->Handle = 0;
            WpdMtpPostHandleEvent(Dev, WpdObjectEventAdded, Handle, Context->Name);
            WpdFreeContext(Dev, Context);
            WpdFormatObjectId(Text, FALSE, Handle);
            return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_MANAGEMENT_OBJECT_ID, Text));
        case 6:
            WpdUploadRevert(Dev, Context);
            WpdFreeContext(Dev, Context);
            return WPD_S_OK;
        default:
            return WPD_E_NOTIMPL;
    }
}

static
NTSTATUS
WpdResponseResults(
    _In_ USHORT Code,
    _In_ ULONG Count,
    _In_reads_(Count) const ULONG *Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Variants;
    ULONG Index;
    NTSTATUS Status;

    Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, Code);
    if (!NT_SUCCESS(Status))
        return Status;
    Variants = WpdCollectionCreate(WpdVariants);
    if (!Variants)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (Index = 0; Index < Count && NT_SUCCESS(Status); Index++)
        Status = WpdVariantsAddUi4(Variants, Params[Index]);
    if (NT_SUCCESS(Status))
        return WpdValuesSetCollection(Results, &WPD_PROPERTY_MTP_EXT_RESPONSE_PARAMS, Variants);
    WpdCollectionFree(Variants);
    return Status;
}

static
BOOLEAN
WpdVendorParams(
    _In_ PWPD_COLLECTION Params,
    _Out_ PUSHORT Code,
    _Out_ PULONG Count,
    _Out_writes_(MTP_MAX_PARAMS) PULONG Values)
{
    PWPD_COLLECTION Variants;
    ULONG Value, Index;

    *Count = 0;
    if (!WpdValuesGetUi4(Params, &WPD_PROPERTY_MTP_EXT_OPERATION_CODE, &Value) || Value > MAXUSHORT)
        return FALSE;
    *Code = (USHORT)Value;
    Variants = WpdValuesGetCollection(Params, &WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, WpdVariants);
    if (!Variants)
        return TRUE;
    if (Variants->Count > MTP_MAX_PARAMS)
        return FALSE;
    for (Index = 0; Index < Variants->Count; Index++)
    {
        if (Variants->Items[Index].Vt != WPD_VT_UI4)
            return FALSE;
        Values[Index] = Variants->Items[Index].Ui4;
    }
    *Count = Variants->Count;
    return TRUE;
}

HRESULT
WpdVendor(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _In_ ULONG OutputLimit,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Variants;
    PWPDMTP_CONTEXT Context;
    MTP_OPERATION Op;
    ULONG Index, Count, Values[MTP_MAX_PARAMS], Length, Done;
    ULONG ResponseParams[MTP_MAX_PARAMS], ResponseCount;
    ULONGLONG Total;
    USHORT Code, Response;
    PUCHAR Data;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (Id)
    {
        case 11:
            Variants = WpdCollectionCreate(WpdVariants);
            if (!Variants)
                return WPD_E_OUTOFMEMORY;
            for (Index = 0; Index < Dev->OperationCount && NT_SUCCESS(Status); Index++)
            {
                if (Dev->Operations[Index] >= 0x9000 && Dev->Operations[Index] <= 0x97FF)
                    Status = WpdVariantsAddUi4(Variants, Dev->Operations[Index]);
            }
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_MTP_EXT_VENDOR_OPERATION_CODES, Variants);
            else
                WpdCollectionFree(Variants);
            return WpdNtToHresult(Status);
        case 18:
            return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_MTP_EXT_VENDOR_EXTENSION_DESCRIPTION,
                                                     Dev->VendorExtensionDesc));
        case 12:
            if (!WpdVendorParams(Params, &Code, &Count, Values))
                return WPD_E_INVALIDARG;
            RtlZeroMemory(&Op, sizeof(Op));
            Op.Code = Code;
            Op.ParamCount = Count;
            RtlCopyMemory(Op.Params, Values, Count * sizeof(ULONG));
            Status = WpdMtpTransaction(Dev, &Op);
            if (!NT_SUCCESS(Status))
                return WpdMtpResultToHresult(Status, 0);
            return WpdNtToHresult(WpdResponseResults(Op.ResponseCode, Op.ResponseParamCount, Op.ResponseParams, Results));
        case 13:
        case 14:
            if (!WpdVendorParams(Params, &Code, &Count, Values))
                return WPD_E_INVALIDARG;
            if (Id == 14 && !WpdValuesGetUi8(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, &Total))
                return WPD_E_INVALIDARG;
            Context = WpdCreateContext(Dev, File, WpdContextVendor, L"CVendorTransferContext");
            if (!Context)
                return WPD_E_OUTOFMEMORY;
            if (Id == 13)
            {
                Status = WpdMtpBeginDataIn(Dev, Code, Count, Values, &Total);
                if (Status == STATUS_END_OF_FILE)
                {
                    Status = STATUS_SUCCESS;
                    Total = 0;
                    Context->Folder = TRUE;
                }
            }
            else
            {
                Status = WpdMtpBeginDataOut(Dev, Code, Count, Values, Total);
            }
            if (!NT_SUCCESS(Status))
            {
                WpdFreeContext(Dev, Context);
                return WpdMtpResultToHresult(Status, 0);
            }
            Context->Total = Total;
            Dev->ActiveTransfer = Context;
            Status = WpdValuesSetString(Results, &WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, Context->Name);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetUi8(Results, &WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, Total);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, WPDMTP_OPTIMAL_TRANSFER);
            return WpdNtToHresult(Status);
    }

    Context = WpdFindContext(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT), WpdContextVendor);
    if (!Context)
        return WPD_E_NOT_FOUND;
    switch (Id)
    {
        case 15:
            if (!WpdValuesGetUi4(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, &Length))
                return WPD_E_INVALIDARG;
            Length = WpdReadLimit(Length, OutputLimit);
            Data = ExAllocatePoolWithTag(PagedPool, max(Length, 1), WPDMTP_TAG);
            if (!Data)
                return WPD_E_OUTOFMEMORY;
            Done = 0;
            if (!Context->Folder)
                Status = WpdMtpReadData(Dev, Data, Length, &Done);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetBuffer(Results, &WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, Data, Done);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSetUi4(Results, &WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, Done);
            ExFreePoolWithTag(Data, WPDMTP_TAG);
            return NT_SUCCESS(Status) ? WPD_S_OK : WpdMtpResultToHresult(Status, 0);
        case 16:
            if (!WpdValuesGetBuffer(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &Data, &Length))
                return WPD_E_INVALIDARG;
            if (WpdValuesGetUi4(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, &Done))
                Length = min(Length, Done);
            Status = WpdMtpWriteData(Dev, Data, Length);
            if (!NT_SUCCESS(Status))
                return WpdMtpResultToHresult(Status, 0);
            return WpdNtToHresult(WpdValuesSetUi4(Results, &WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN, Length));
        case 17:
            Status = WpdMtpEndTransaction(Dev, &Response, ResponseParams, &ResponseCount);
            Dev->ActiveTransfer = NULL;
            WpdFreeContext(Dev, Context);
            if (!NT_SUCCESS(Status))
                return WpdMtpResultToHresult(Status, 0);
            return WpdNtToHresult(WpdResponseResults(Response, ResponseCount, ResponseParams, Results));
        default:
            return WPD_E_NOTIMPL;
    }
}
