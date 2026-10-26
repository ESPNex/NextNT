/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     MTP transactions over the USB still image class transport
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"

#define NDEBUG
#include <debug.h>

VOID
WpdMtpLock(
    _In_ PWPDMTP_DEVICE Dev)
{
    KeWaitForSingleObject(&Dev->Lock, Executive, KernelMode, FALSE, NULL);
}

VOID
WpdMtpUnlock(
    _In_ PWPDMTP_DEVICE Dev)
{
    KeSetEvent(&Dev->Lock, IO_NO_INCREMENT, FALSE);
}

UCHAR
MtpReadU8(
    _Inout_ PMTP_DATASET Set)
{
    UCHAR Value = 0;

    MtpReadBytes(Set, &Value, sizeof(Value));
    return Value;
}

USHORT
MtpReadU16(
    _Inout_ PMTP_DATASET Set)
{
    USHORT Value = 0;

    MtpReadBytes(Set, &Value, sizeof(Value));
    return Value;
}

ULONG
MtpReadU32(
    _Inout_ PMTP_DATASET Set)
{
    ULONG Value = 0;

    MtpReadBytes(Set, &Value, sizeof(Value));
    return Value;
}

ULONGLONG
MtpReadU64(
    _Inout_ PMTP_DATASET Set)
{
    ULONGLONG Value = 0;

    MtpReadBytes(Set, &Value, sizeof(Value));
    return Value;
}

VOID
MtpReadBytes(
    _Inout_ PMTP_DATASET Set,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (Set->Overrun || Set->Size - Set->Offset < Length)
    {
        Set->Overrun = TRUE;
        RtlZeroMemory(Buffer, Length);
        return;
    }
    RtlCopyMemory(Buffer, Set->Data + Set->Offset, Length);
    Set->Offset += Length;
}

PWCHAR
MtpReadString(
    _Inout_ PMTP_DATASET Set)
{
    UCHAR Chars = MtpReadU8(Set);
    PWCHAR String;

    String = ExAllocatePoolZero(PagedPool, (Chars + 1) * sizeof(WCHAR), WPDMTP_TAG);
    if (!String)
    {
        Set->Overrun = TRUE;
        return NULL;
    }
    MtpReadBytes(Set, String, Chars * sizeof(WCHAR));
    String[Chars] = UNICODE_NULL;
    return String;
}

PUSHORT
MtpReadU16Array(
    _Inout_ PMTP_DATASET Set,
    _Out_ PULONG Count)
{
    ULONG Elements = MtpReadU32(Set);
    PUSHORT Array;

    *Count = 0;
    if (Set->Overrun || Elements > (Set->Size - Set->Offset) / sizeof(USHORT))
    {
        Set->Overrun = TRUE;
        return NULL;
    }
    Array = ExAllocatePoolZero(PagedPool, max(Elements, 1) * sizeof(USHORT), WPDMTP_TAG);
    if (!Array)
    {
        Set->Overrun = TRUE;
        return NULL;
    }
    MtpReadBytes(Set, Array, Elements * sizeof(USHORT));
    *Count = Elements;
    return Array;
}

PULONG
MtpReadU32Array(
    _Inout_ PMTP_DATASET Set,
    _Out_ PULONG Count)
{
    ULONG Elements = MtpReadU32(Set);
    PULONG Array;

    *Count = 0;
    if (Set->Overrun || Elements > (Set->Size - Set->Offset) / sizeof(ULONG))
    {
        Set->Overrun = TRUE;
        return NULL;
    }
    Array = ExAllocatePoolZero(PagedPool, max(Elements, 1) * sizeof(ULONG), WPDMTP_TAG);
    if (!Array)
    {
        Set->Overrun = TRUE;
        return NULL;
    }
    MtpReadBytes(Set, Array, Elements * sizeof(ULONG));
    *Count = Elements;
    return Array;
}

static
ULONG
MtpTypeSize(
    _In_ USHORT DataType)
{
    switch (DataType & ~MTP_TYPE_ARRAY)
    {
        case MTP_TYPE_INT8:
        case MTP_TYPE_UINT8:
            return 1;
        case MTP_TYPE_INT16:
        case MTP_TYPE_UINT16:
            return 2;
        case MTP_TYPE_INT32:
        case MTP_TYPE_UINT32:
            return 4;
        case MTP_TYPE_INT64:
        case MTP_TYPE_UINT64:
            return 8;
        case MTP_TYPE_INT128:
        case MTP_TYPE_UINT128:
            return 16;
        default:
            return 0;
    }
}

VOID
MtpSkipValue(
    _Inout_ PMTP_DATASET Set,
    _In_ USHORT DataType)
{
    ULONG Size, Count;

    if (DataType == MTP_TYPE_STRING)
    {
        Size = MtpReadU8(Set) * sizeof(WCHAR);
    }
    else if (DataType & MTP_TYPE_ARRAY)
    {
        Count = MtpReadU32(Set);
        Size = MtpTypeSize(DataType);
        if (!Size || Count > (Set->Size - Set->Offset) / Size)
        {
            Set->Overrun = TRUE;
            return;
        }
        Size *= Count;
    }
    else
    {
        Size = MtpTypeSize(DataType);
        if (!Size)
        {
            Set->Overrun = TRUE;
            return;
        }
    }
    if (Set->Overrun || Set->Size - Set->Offset < Size)
    {
        Set->Overrun = TRUE;
        return;
    }
    Set->Offset += Size;
}

BOOLEAN
MtpReadValue(
    _Inout_ PMTP_DATASET Set,
    _In_ USHORT DataType,
    _Out_ PMTP_VALUE Value)
{
    ULONG Size, Count, Index;

    RtlZeroMemory(Value, sizeof(*Value));
    Value->DataType = DataType;
    if (DataType == MTP_TYPE_STRING)
    {
        Value->String = MtpReadString(Set);
        Value->Valid = Value->String != NULL && !Set->Overrun;
        return Value->Valid;
    }
    if (DataType == MTP_TYPE_AUINT16)
    {
        Count = MtpReadU32(Set);
        if (Set->Overrun || Count > (Set->Size - Set->Offset) / sizeof(USHORT) || Count > 0x7FFF)
        {
            Set->Overrun = TRUE;
            return FALSE;
        }
        Value->String = ExAllocatePoolZero(PagedPool, (Count + 1) * sizeof(WCHAR), WPDMTP_TAG);
        if (!Value->String)
        {
            Set->Overrun = TRUE;
            return FALSE;
        }
        for (Index = 0; Index < Count; Index++)
            Value->String[Index] = MtpReadU16(Set);
        Value->Valid = !Set->Overrun;
        return Value->Valid;
    }
    Size = MtpTypeSize(DataType);
    if ((DataType & MTP_TYPE_ARRAY) || !Size)
    {
        MtpSkipValue(Set, DataType);
        return FALSE;
    }
    MtpReadBytes(Set, Value->Raw, Size);
    Value->Valid = !Set->Overrun;
    return Value->Valid;
}

VOID
MtpFreeValue(
    _Inout_ PMTP_VALUE Value)
{
    if (Value->String)
        ExFreePoolWithTag(Value->String, WPDMTP_TAG);
    RtlZeroMemory(Value, sizeof(*Value));
}

VOID
MtpReadForm(
    _Inout_ PMTP_DATASET Set,
    _In_ USHORT DataType,
    _Out_ PMTP_FORM Form)
{
    ULONG Index;

    RtlZeroMemory(Form, sizeof(*Form));
    Form->Flag = MtpReadU8(Set);
    if (Set->Overrun)
    {
        Form->Flag = MTP_FORM_NONE;
        return;
    }
    if (Form->Flag == MTP_FORM_RANGE)
    {
        for (Index = 0; Index < ARRAYSIZE(Form->Range); Index++)
            MtpReadValue(Set, DataType, &Form->Range[Index]);
    }
    else if (Form->Flag == MTP_FORM_ENUMERATION)
    {
        Form->EnumCount = MtpReadU16(Set);
        if (Set->Overrun || !Form->EnumCount)
        {
            Form->EnumCount = 0;
            return;
        }
        Form->Enum = ExAllocatePoolZero(PagedPool, Form->EnumCount * sizeof(MTP_VALUE), WPDMTP_TAG);
        if (!Form->Enum)
        {
            Form->EnumCount = 0;
            Set->Overrun = TRUE;
            return;
        }
        for (Index = 0; Index < Form->EnumCount && !Set->Overrun; Index++)
            MtpReadValue(Set, DataType, &Form->Enum[Index]);
    }
}

VOID
MtpFreeForm(
    _Inout_ PMTP_FORM Form)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(Form->Range); Index++)
        MtpFreeValue(&Form->Range[Index]);
    for (Index = 0; Index < Form->EnumCount; Index++)
        MtpFreeValue(&Form->Enum[Index]);
    if (Form->Enum)
        ExFreePoolWithTag(Form->Enum, WPDMTP_TAG);
    RtlZeroMemory(Form, sizeof(*Form));
}

LONGLONG
MtpValueInteger(
    _In_ const MTP_VALUE *Value)
{
    switch (Value->DataType)
    {
        case MTP_TYPE_INT8:
            return *(const CHAR *)Value->Raw;
        case MTP_TYPE_UINT8:
            return *(const UCHAR *)Value->Raw;
        case MTP_TYPE_INT16:
            return *(const SHORT *)Value->Raw;
        case MTP_TYPE_UINT16:
            return *(const USHORT *)Value->Raw;
        case MTP_TYPE_INT32:
            return *(const LONG *)Value->Raw;
        case MTP_TYPE_UINT32:
            return *(const ULONG *)Value->Raw;
        case MTP_TYPE_INT64:
        case MTP_TYPE_UINT64:
        case MTP_TYPE_INT128:
        case MTP_TYPE_UINT128:
            return *(const LONGLONG *)Value->Raw;
        default:
            return 0;
    }
}

ULONG
MtpStringSize(
    _In_opt_ PCWSTR String)
{
    SIZE_T Chars = String ? wcslen(String) : 0;

    if (!Chars)
        return 1;
    return 1 + (ULONG)(min(Chars, 254) + 1) * sizeof(WCHAR);
}

PUCHAR
MtpWriteU8(
    _Inout_ PUCHAR Cursor,
    _In_ UCHAR Value)
{
    *Cursor = Value;
    return Cursor + 1;
}

PUCHAR
MtpWriteU16(
    _Inout_ PUCHAR Cursor,
    _In_ USHORT Value)
{
    RtlCopyMemory(Cursor, &Value, sizeof(Value));
    return Cursor + sizeof(Value);
}

PUCHAR
MtpWriteU32(
    _Inout_ PUCHAR Cursor,
    _In_ ULONG Value)
{
    RtlCopyMemory(Cursor, &Value, sizeof(Value));
    return Cursor + sizeof(Value);
}

PUCHAR
MtpWriteString(
    _Inout_ PUCHAR Cursor,
    _In_opt_ PCWSTR String)
{
    SIZE_T Chars = String ? wcslen(String) : 0;
    UCHAR Count;

    if (!Chars)
        return MtpWriteU8(Cursor, 0);
    Count = (UCHAR)(min(Chars, 254) + 1);
    Cursor = MtpWriteU8(Cursor, Count);
    RtlCopyMemory(Cursor, String, (Count - 1) * sizeof(WCHAR));
    Cursor += (Count - 1) * sizeof(WCHAR);
    return MtpWriteU16(Cursor, 0);
}

PWCHAR
WpdMtpDuplicateString(
    _In_opt_ PCWSTR String)
{
    SIZE_T Bytes = ((String ? wcslen(String) : 0) + 1) * sizeof(WCHAR);
    PWCHAR Copy;

    Copy = ExAllocatePoolWithTag(PagedPool, Bytes, WPDMTP_TAG);
    if (Copy)
        RtlCopyMemory(Copy, String ? String : L"", Bytes);
    return Copy;
}

BOOLEAN
WpdMtpSupportsOperation(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code)
{
    ULONG Index;

    for (Index = 0; Index < Dev->OperationCount; Index++)
    {
        if (Dev->Operations[Index] == Code)
            return TRUE;
    }
    return FALSE;
}

BOOLEAN
WpdMtpSupportsDeviceProp(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code)
{
    ULONG Index;

    for (Index = 0; Index < Dev->DevicePropCount; Index++)
    {
        if (Dev->DeviceProps[Index] == Code)
            return TRUE;
    }
    return FALSE;
}

HRESULT
WpdMtpResultToHresult(
    _In_ NTSTATUS Status,
    _In_ USHORT ResponseCode)
{
    if (Status == STATUS_DEVICE_NOT_CONNECTED || Status == STATUS_NO_SUCH_DEVICE)
        return WPD_E_DEVICE_NOT_CONNECTED;
    if (Status == STATUS_CANCELLED)
        return WPD_E_CANCELLED;
    if (Status == STATUS_INSUFFICIENT_RESOURCES)
        return WPD_E_OUTOFMEMORY;
    if (!NT_SUCCESS(Status))
        return WPD_E_GEN_FAILURE;
    if (ResponseCode == MTP_RC_OK)
        return WPD_S_OK;
    return WPD_E_MTP(ResponseCode);
}

static
NTSTATUS
WpdMtpSendCommand(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code,
    _In_ ULONG ParamCount,
    _In_reads_(ParamCount) const ULONG *Params)
{
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->TransferBuffer;
    ULONG Length = sizeof(*Header) + ParamCount * sizeof(ULONG), Sent;
    NTSTATUS Status;

    if (Dev->DataInOpen || Dev->DataOutOpen || Dev->ResponseBuffered || Dev->ResponsePending)
    {
        Status = WpdMtpAbortData(Dev);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    Header->Length = Length;
    Header->Type = MTP_CONTAINER_COMMAND;
    Header->Code = Code;
    Header->TransactionId = Code == MTP_OP_OPEN_SESSION ? 0 : ++Dev->TransactionId;
    if (ParamCount)
        RtlCopyMemory(Header + 1, Params, ParamCount * sizeof(ULONG));
    Status = WpdMtpUsbBulk(Dev, FALSE, Dev->TransferBuffer, Length, &Sent);
    if (NT_SUCCESS(Status) && Sent != Length)
        Status = STATUS_DEVICE_DATA_ERROR;
    if (!NT_SUCCESS(Status))
        DPRINT1("WPDMTP: command 0x%04x not sent 0x%08lx\n", Code, Status);
    return Status;
}

static
NTSTATUS
WpdMtpReadResponse(
    _In_ PWPDMTP_DEVICE Dev,
    _Out_ PUSHORT ResponseCode,
    _Out_opt_ PULONG ResponseParams,
    _Out_opt_ PULONG ResponseParamCount)
{
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->TransferBuffer;
    ULONG Read, Count, Attempt;
    NTSTATUS Status;

    *ResponseCode = 0;
    if (ResponseParamCount)
        *ResponseParamCount = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = WpdMtpUsbBulk(Dev, TRUE, Dev->TransferBuffer, max(Dev->BulkInPacketSize, 64), &Read);
        if (!NT_SUCCESS(Status))
            return Status;
        if (Read)
            break;
    }
    if (Read < sizeof(*Header) || Header->Type != MTP_CONTAINER_RESPONSE)
    {
        DPRINT1("WPDMTP: bad response container (%lu bytes, type %u)\n", Read, Read >= 6 ? Header->Type : 0);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    *ResponseCode = Header->Code;
    Count = min((min(Read, Header->Length) - sizeof(*Header)) / sizeof(ULONG), MTP_MAX_PARAMS);
    if (ResponseParams && Count)
        RtlCopyMemory(ResponseParams, Header + 1, Count * sizeof(ULONG));
    if (ResponseParamCount)
        *ResponseParamCount = Count;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdMtpBeginDataIn(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code,
    _In_ ULONG ParamCount,
    _In_reads_(ParamCount) const ULONG *Params,
    _Out_ PULONGLONG Total)
{
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->TransferBuffer;
    ULONG Read;
    NTSTATUS Status;

    *Total = 0;
    Status = WpdMtpSendCommand(Dev, Code, ParamCount, Params);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = WpdMtpUsbBulk(Dev, TRUE, Dev->TransferBuffer, Dev->BulkInPacketSize, &Read);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Read >= sizeof(*Header) && Header->Type == MTP_CONTAINER_RESPONSE)
    {
        Dev->ResponseBuffered = TRUE;
        Dev->InBuffered = Read;
        return STATUS_END_OF_FILE;
    }
    if (Read < sizeof(*Header) || Header->Type != MTP_CONTAINER_DATA || Header->Length < sizeof(*Header))
        return STATUS_DEVICE_PROTOCOL_ERROR;

    *Total = Header->Length == MAXULONG ? MAXULONGLONG : Header->Length - sizeof(*Header);
    Dev->DataInOpen = TRUE;
    Dev->DataInRemaining = *Total;
    Dev->InOffset = sizeof(*Header);
    Dev->InBuffered = Read - sizeof(*Header);
    if (*Total != MAXULONGLONG && Dev->InBuffered > *Total)
        Dev->InBuffered = (ULONG)*Total;
    if (*Total == MAXULONGLONG && Read < Dev->BulkInPacketSize)
        Dev->DataInRemaining = Dev->InBuffered;
    if (!Dev->DataInRemaining)
        Dev->DataInOpen = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdMtpReadData(
    _In_ PWPDMTP_DEVICE Dev,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Read)
{
    ULONG Copied = 0, Chunk, Got;
    BOOLEAN Unknown;
    NTSTATUS Status;

    *Read = 0;
    while (Copied < Length && Dev->DataInOpen && Dev->DataInRemaining)
    {
        Unknown = Dev->DataInRemaining == MAXULONGLONG;
        if (Dev->InBuffered)
        {
            Chunk = min(Dev->InBuffered, Length - Copied);
            RtlCopyMemory(Buffer + Copied, Dev->TransferBuffer + Dev->InOffset, Chunk);
            Dev->InOffset += Chunk;
            Dev->InBuffered -= Chunk;
            Copied += Chunk;
            if (!Unknown)
                Dev->DataInRemaining -= Chunk;
            continue;
        }
        if (Unknown)
            Chunk = WPDMTP_TRANSFER_CHUNK;
        else
            Chunk = (ULONG)min(WPDMTP_TRANSFER_CHUNK, (Dev->DataInRemaining + Dev->BulkInPacketSize - 1) / Dev->BulkInPacketSize * Dev->BulkInPacketSize);
        Status = WpdMtpUsbBulk(Dev, TRUE, Dev->TransferBuffer, Chunk, &Got);
        if (!NT_SUCCESS(Status))
        {
            Dev->DataInOpen = FALSE;
            Dev->DataInRemaining = 0;
            *Read = Copied;
            return Status;
        }
        Dev->InOffset = 0;
        Dev->InBuffered = Got;
        if (Unknown && Got < Chunk)
            Dev->DataInRemaining = Got;
        else if (!Unknown && Got > Dev->DataInRemaining)
            Dev->InBuffered = (ULONG)Dev->DataInRemaining;
        if (!Got)
            Dev->DataInRemaining = 0;
    }
    if (!Dev->DataInRemaining)
    {
        Dev->DataInOpen = FALSE;
        Dev->InBuffered = 0;
    }
    *Read = Copied;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdMtpBeginDataOut(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Code,
    _In_ ULONG ParamCount,
    _In_reads_(ParamCount) const ULONG *Params,
    _In_ ULONGLONG Total)
{
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->TransferBuffer;
    NTSTATUS Status;

    Status = WpdMtpSendCommand(Dev, Code, ParamCount, Params);
    if (!NT_SUCCESS(Status))
        return Status;
    Header->Length = Total + sizeof(*Header) > MAXULONG ? MAXULONG : (ULONG)(Total + sizeof(*Header));
    Header->Type = MTP_CONTAINER_DATA;
    Header->Code = Code;
    Header->TransactionId = Dev->TransactionId;
    Dev->DataOutOpen = TRUE;
    Dev->DataOutRemaining = Total;
    Dev->DataOutTotal = Total + sizeof(*Header);
    Dev->DataOutTransfered = sizeof(*Header);
    if (!Total)
        return WpdMtpWriteData(Dev, NULL, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
WpdMtpFlushOut(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ BOOLEAN Final)
{
    ULONG Sent, Length = Dev->DataOutTransfered;
    NTSTATUS Status;

    if (!Length)
        return STATUS_SUCCESS;
    Status = WpdMtpUsbBulk(Dev, FALSE, Dev->TransferBuffer, Length, &Sent);
    Dev->DataOutTransfered = 0;
    if (NT_SUCCESS(Status) && Sent != Length)
        Status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(Status) && Final && !(Dev->DataOutTotal % Dev->BulkOutPacketSize))
        Status = WpdMtpUsbBulk(Dev, FALSE, Dev->TransferBuffer, 0, &Sent);
    return Status;
}

NTSTATUS
WpdMtpWriteData(
    _In_ PWPDMTP_DEVICE Dev,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    ULONG Chunk, Done = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Dev->DataOutOpen || Length > Dev->DataOutRemaining)
        return STATUS_INVALID_DEVICE_STATE;
    while (Done < Length)
    {
        Chunk = min(Length - Done, WPDMTP_TRANSFER_CHUNK - Dev->DataOutTransfered);
        RtlCopyMemory(Dev->TransferBuffer + Dev->DataOutTransfered, Buffer + Done, Chunk);
        Dev->DataOutTransfered += Chunk;
        Dev->DataOutRemaining -= Chunk;
        Done += Chunk;
        if (Dev->DataOutTransfered == WPDMTP_TRANSFER_CHUNK && Dev->DataOutRemaining)
        {
            Status = WpdMtpFlushOut(Dev, FALSE);
            if (!NT_SUCCESS(Status))
                break;
        }
    }
    if (NT_SUCCESS(Status) && !Dev->DataOutRemaining)
    {
        Status = WpdMtpFlushOut(Dev, TRUE);
        Dev->DataOutOpen = FALSE;
        Dev->ResponsePending = NT_SUCCESS(Status);
    }
    if (!NT_SUCCESS(Status))
        Dev->DataOutOpen = FALSE;
    return Status;
}

NTSTATUS
WpdMtpEndTransaction(
    _In_ PWPDMTP_DEVICE Dev,
    _Out_ PUSHORT ResponseCode,
    _Out_opt_ PULONG ResponseParams,
    _Out_opt_ PULONG ResponseParamCount)
{
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->TransferBuffer;
    UCHAR Discard[512];
    ULONG Read, Count;

    *ResponseCode = 0;
    if (ResponseParamCount)
        *ResponseParamCount = 0;
    while (Dev->DataInOpen)
    {
        if (!NT_SUCCESS(WpdMtpReadData(Dev, Discard, sizeof(Discard), &Read)) || !Read)
            break;
    }
    Dev->DataInOpen = FALSE;
    Dev->InBuffered = 0;
    if (Dev->DataOutOpen)
        return STATUS_INVALID_DEVICE_STATE;
    Dev->ResponsePending = FALSE;
    if (Dev->ResponseBuffered)
    {
        Dev->ResponseBuffered = FALSE;
        *ResponseCode = Header->Code;
        Count = min((min(Header->Length, 32) - sizeof(*Header)) / sizeof(ULONG), MTP_MAX_PARAMS);
        if (ResponseParams && Count)
            RtlCopyMemory(ResponseParams, Header + 1, Count * sizeof(ULONG));
        if (ResponseParamCount)
            *ResponseParamCount = Count;
        return STATUS_SUCCESS;
    }
    return WpdMtpReadResponse(Dev, ResponseCode, ResponseParams, ResponseParamCount);
}

NTSTATUS
WpdMtpAbortData(
    _In_ PWPDMTP_DEVICE Dev)
{
    UCHAR Cancel[6];
    USHORT Code;
    ULONG Status = 0;
    UCHAR DeviceStatus[4];
    ULONG Attempt;

    if ((Dev->DataInOpen && Dev->DataInRemaining <= WPDMTP_TRANSFER_CHUNK) ||
        (!Dev->DataInOpen && (Dev->ResponseBuffered || Dev->ResponsePending)))
    {
        return WpdMtpEndTransaction(Dev, &Code, NULL, NULL);
    }
    if (!Dev->DataOutOpen && !Dev->DataInOpen)
        return STATUS_SUCCESS;

    Dev->DataOutOpen = FALSE;
    Dev->DataInOpen = FALSE;
    Dev->DataInRemaining = 0;
    Dev->InBuffered = 0;
    Dev->ResponseBuffered = FALSE;
    Dev->ResponsePending = FALSE;
    *(PUSHORT)Cancel = 0x4001;
    RtlCopyMemory(Cancel + 2, &Dev->TransactionId, sizeof(ULONG));
    WpdMtpUsbClassRequest(Dev, FALSE, USB_STILL_IMAGE_CANCEL_REQUEST, Cancel, sizeof(Cancel));
    for (Attempt = 0; Attempt < 10; Attempt++)
    {
        RtlZeroMemory(DeviceStatus, sizeof(DeviceStatus));
        if (!NT_SUCCESS(WpdMtpUsbClassRequest(Dev, TRUE, USB_STILL_IMAGE_GET_DEVICE_STATUS,
                                              DeviceStatus, sizeof(DeviceStatus))))
        {
            break;
        }
        RtlCopyMemory(&Status, DeviceStatus + 2, sizeof(USHORT));
        if (Status != MTP_RC_DEVICE_BUSY)
            break;
    }
    WpdMtpUsbResetPipe(Dev, Dev->BulkOut);
    WpdMtpUsbResetPipe(Dev, Dev->BulkIn);
    Dev->DataOutTransfered = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdMtpTransaction(
    _In_ PWPDMTP_DEVICE Dev,
    _Inout_ PMTP_OPERATION Op)
{
    ULONGLONG Total;
    ULONG Read;
    NTSTATUS Status;

    Op->ResponseCode = 0;
    Op->ResponseParamCount = 0;
    Op->InData = NULL;
    Op->InLength = 0;

    if (Op->DataOut)
    {
        Status = WpdMtpBeginDataOut(Dev, Op->Code, Op->ParamCount, Op->Params, Op->DataOutLength);
        if (NT_SUCCESS(Status) && Op->DataOutLength)
            Status = WpdMtpWriteData(Dev, Op->DataOut, Op->DataOutLength);
    }
    else if (Op->DataIn)
    {
        Status = WpdMtpBeginDataIn(Dev, Op->Code, Op->ParamCount, Op->Params, &Total);
        if (Status == STATUS_END_OF_FILE)
        {
            Status = STATUS_SUCCESS;
        }
        else if (NT_SUCCESS(Status))
        {
            if (Total > 64 * 1024 * 1024)
            {
                WpdMtpAbortData(Dev);
                return STATUS_DEVICE_DATA_ERROR;
            }
            Op->InData = ExAllocatePoolWithTag(PagedPool, (SIZE_T)max(Total, 1), WPDMTP_TAG);
            if (!Op->InData)
            {
                WpdMtpAbortData(Dev);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Status = WpdMtpReadData(Dev, Op->InData, (ULONG)Total, &Read);
            Op->InLength = Read;
        }
    }
    else
    {
        Status = WpdMtpSendCommand(Dev, Op->Code, Op->ParamCount, Op->Params);
    }
    if (NT_SUCCESS(Status))
        Status = WpdMtpEndTransaction(Dev, &Op->ResponseCode, Op->ResponseParams, &Op->ResponseParamCount);
    if (!NT_SUCCESS(Status) || Op->ResponseCode != MTP_RC_OK)
    {
        DPRINT("WPDMTP: operation 0x%04x status 0x%08lx response 0x%04x\n", Op->Code, Status, Op->ResponseCode);
        if (Op->InData)
            ExFreePoolWithTag(Op->InData, WPDMTP_TAG);
        Op->InData = NULL;
        Op->InLength = 0;
    }
    return Status;
}

NTSTATUS
WpdMtpOpenSession(
    _In_ PWPDMTP_DEVICE Dev)
{
    MTP_OPERATION Op;
    NTSTATUS Status;

    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_OPEN_SESSION;
    Op.ParamCount = 1;
    Op.Params[0] = 1;
    Dev->TransactionId = 0;
    Status = WpdMtpTransaction(Dev, &Op);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Op.ResponseCode == MTP_RC_SESSION_ALREADY_OPEN)
    {
        RtlZeroMemory(&Op, sizeof(Op));
        Op.Code = MTP_OP_CLOSE_SESSION;
        WpdMtpTransaction(Dev, &Op);
        RtlZeroMemory(&Op, sizeof(Op));
        Op.Code = MTP_OP_OPEN_SESSION;
        Op.ParamCount = 1;
        Op.Params[0] = 1;
        Dev->TransactionId = 0;
        Status = WpdMtpTransaction(Dev, &Op);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    if (Op.ResponseCode != MTP_RC_OK)
    {
        DPRINT1("WPDMTP: OpenSession failed with response 0x%04x\n", Op.ResponseCode);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    Dev->SessionOpen = TRUE;
    Dev->SessionId = 1;
    return STATUS_SUCCESS;
}

VOID
WpdMtpCloseSession(
    _In_ PWPDMTP_DEVICE Dev)
{
    MTP_OPERATION Op;

    if (!Dev->SessionOpen)
        return;
    RtlZeroMemory(&Op, sizeof(Op));
    Op.Code = MTP_OP_CLOSE_SESSION;
    WpdMtpTransaction(Dev, &Op);
    Dev->SessionOpen = FALSE;
}

VOID
WpdMtpFreeDeviceInfo(
    _In_ PWPDMTP_DEVICE Dev)
{
    PVOID *Fields[] =
    {
        (PVOID *)&Dev->VendorExtensionDesc, (PVOID *)&Dev->Operations, (PVOID *)&Dev->Events,
        (PVOID *)&Dev->DeviceProps, (PVOID *)&Dev->Manufacturer, (PVOID *)&Dev->Model,
        (PVOID *)&Dev->DeviceVersion, (PVOID *)&Dev->SerialNumber, (PVOID *)&Dev->FriendlyName,
    };
    ULONG Index;

    if (Dev->DevicePropInfo)
    {
        for (Index = 0; Index < Dev->DevicePropCount; Index++)
        {
            MtpFreeValue(&Dev->DevicePropInfo[Index].Default);
            MtpFreeValue(&Dev->DevicePropInfo[Index].Current);
            MtpFreeForm(&Dev->DevicePropInfo[Index].Form);
        }
        ExFreePoolWithTag(Dev->DevicePropInfo, WPDMTP_TAG);
        Dev->DevicePropInfo = NULL;
    }

    for (Index = 0; Index < ARRAYSIZE(Fields); Index++)
    {
        if (*Fields[Index])
            ExFreePoolWithTag(*Fields[Index], WPDMTP_TAG);
        *Fields[Index] = NULL;
    }
    if (Dev->Formats)
    {
        for (Index = 0; Index < Dev->FormatCount; Index++)
        {
            if (Dev->Formats[Index].Props)
                ExFreePoolWithTag(Dev->Formats[Index].Props, WPDMTP_TAG);
        }
        ExFreePoolWithTag(Dev->Formats, WPDMTP_TAG);
    }
    Dev->Formats = NULL;
    Dev->FormatCount = Dev->OperationCount = Dev->EventCount = Dev->DevicePropCount = 0;
}
