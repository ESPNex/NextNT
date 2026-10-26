/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WPD parameter collections and their serialized form
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"

#define WPD_MAX_DEPTH 8

static const GUID ClsidValues = { 0x0c15d503, 0xd017, 0x47ce, { 0x90, 0x16, 0x7b, 0x3f, 0x97, 0x87, 0x21, 0xcc } };
static const GUID ClsidKeys = { 0xde2d022d, 0x2480, 0x43be, { 0x97, 0xf0, 0xd1, 0xfa, 0x2c, 0xf9, 0x8f, 0x4f } };
static const GUID ClsidVariants = { 0x08a99e2f, 0x6d6d, 0x4b80, { 0xaf, 0x5a, 0xba, 0xf2, 0xbc, 0xbe, 0x4c, 0xb9 } };
static const GUID ClsidValuesList = { 0x3882134d, 0x14cf, 0x4220, { 0x9c, 0xb4, 0x43, 0x5f, 0x86, 0xd8, 0x3f, 0x60 } };

typedef struct _WPD_READER
{
    const UCHAR *Data;
    ULONG Size;
    ULONG Offset;
} WPD_READER, *PWPD_READER;

static
SIZE_T
WpdElementSize(
    _In_ WPD_COLLECTION_KIND Kind)
{
    switch (Kind)
    {
        case WpdValues:
            return sizeof(WPD_ENTRY);
        case WpdKeys:
            return sizeof(PROPERTYKEY);
        case WpdVariants:
            return sizeof(WPD_VALUE);
        default:
            return sizeof(PWPD_COLLECTION);
    }
}

PWPD_COLLECTION
WpdCollectionCreate(
    _In_ WPD_COLLECTION_KIND Kind)
{
    PWPD_COLLECTION Collection;

    Collection = ExAllocatePoolZero(PagedPool, sizeof(*Collection), WPDMTP_TAG);
    if (Collection)
        Collection->Kind = Kind;
    return Collection;
}

VOID
WpdValueClear(
    _Inout_ PWPD_VALUE Value)
{
    switch (Value->Vt)
    {
        case WPD_VT_LPWSTR:
            if (Value->String.Buffer)
                ExFreePoolWithTag(Value->String.Buffer, WPDMTP_TAG);
            break;
        case WPD_VT_VECTOR | WPD_VT_UI1:
            if (Value->Blob.Buffer)
                ExFreePoolWithTag(Value->Blob.Buffer, WPDMTP_TAG);
            break;
        case WPD_VT_UNKNOWN:
            WpdCollectionFree(Value->Collection);
            break;
    }
    RtlZeroMemory(Value, sizeof(*Value));
}

VOID
WpdCollectionFree(
    _In_opt_ PWPD_COLLECTION Collection)
{
    ULONG Index;

    if (!Collection)
        return;
    for (Index = 0; Index < Collection->Count; Index++)
    {
        switch (Collection->Kind)
        {
            case WpdValues:
                WpdValueClear(&Collection->Entries[Index].Value);
                break;
            case WpdVariants:
                WpdValueClear(&Collection->Items[Index]);
                break;
            case WpdValuesList:
                WpdCollectionFree(Collection->Lists[Index]);
                break;
            default:
                break;
        }
    }
    if (Collection->Data)
        ExFreePoolWithTag(Collection->Data, WPDMTP_TAG);
    ExFreePoolWithTag(Collection, WPDMTP_TAG);
}

static
NTSTATUS
WpdReserve(
    _Inout_ PWPD_COLLECTION Collection,
    _In_ ULONG Count)
{
    SIZE_T Element = WpdElementSize(Collection->Kind);
    ULONG Capacity;
    PVOID Data;

    if (Count <= Collection->Capacity)
        return STATUS_SUCCESS;
    Capacity = Collection->Capacity ? Collection->Capacity * 2 : 8;
    if (Capacity < Count)
        Capacity = Count;
    if (Capacity > MAXULONG / Element)
        return STATUS_INSUFFICIENT_RESOURCES;
    Data = ExAllocatePoolZero(PagedPool, Capacity * Element, WPDMTP_TAG);
    if (!Data)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Collection->Data)
    {
        RtlCopyMemory(Data, Collection->Data, Collection->Count * Element);
        ExFreePoolWithTag(Collection->Data, WPDMTP_TAG);
    }
    Collection->Data = Data;
    Collection->Capacity = Capacity;
    return STATUS_SUCCESS;
}

BOOLEAN
WpdKeyEqual(
    _In_ REFPROPERTYKEY Left,
    _In_ REFPROPERTYKEY Right)
{
    return Left->pid == Right->pid && RtlEqualMemory(&Left->fmtid, &Right->fmtid, sizeof(GUID));
}

NTSTATUS
WpdValueCopy(
    _Out_ PWPD_VALUE Destination,
    _In_ const WPD_VALUE *Source)
{
    ULONG Index;
    NTSTATUS Status;

    *Destination = *Source;
    switch (Source->Vt)
    {
        case WPD_VT_LPWSTR:
            Destination->String.Buffer = ExAllocatePoolWithTag(PagedPool, Source->String.Bytes, WPDMTP_TAG);
            if (!Destination->String.Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            RtlCopyMemory(Destination->String.Buffer, Source->String.Buffer, Source->String.Bytes);
            break;
        case WPD_VT_VECTOR | WPD_VT_UI1:
            Destination->Blob.Buffer = ExAllocatePoolWithTag(PagedPool, max(Source->Blob.Size, 1), WPDMTP_TAG);
            if (!Destination->Blob.Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            RtlCopyMemory(Destination->Blob.Buffer, Source->Blob.Buffer, Source->Blob.Size);
            break;
        case WPD_VT_UNKNOWN:
        {
            PWPD_COLLECTION From = Source->Collection, To;

            To = WpdCollectionCreate(From->Kind);
            Destination->Collection = To;
            if (!To)
                return STATUS_INSUFFICIENT_RESOURCES;
            Status = WpdReserve(To, From->Count);
            if (!NT_SUCCESS(Status))
                return Status;
            for (Index = 0; Index < From->Count; Index++)
            {
                switch (From->Kind)
                {
                    case WpdValues:
                        To->Entries[Index].Key = From->Entries[Index].Key;
                        Status = WpdValueCopy(&To->Entries[Index].Value, &From->Entries[Index].Value);
                        break;
                    case WpdKeys:
                        To->Keys[Index] = From->Keys[Index];
                        Status = STATUS_SUCCESS;
                        break;
                    case WpdVariants:
                        Status = WpdValueCopy(&To->Items[Index], &From->Items[Index]);
                        break;
                    default:
                    {
                        WPD_VALUE Inner, Copy;

                        Inner.Vt = WPD_VT_UNKNOWN;
                        Inner.Collection = From->Lists[Index];
                        Status = WpdValueCopy(&Copy, &Inner);
                        To->Lists[Index] = Copy.Collection;
                        break;
                    }
                }
                To->Count = Index + 1;
                if (!NT_SUCCESS(Status))
                    return Status;
            }
            break;
        }
    }
    return STATUS_SUCCESS;
}

PWPD_VALUE
WpdValuesFind(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key)
{
    ULONG Index;

    if (!Values || Values->Kind != WpdValues)
        return NULL;
    for (Index = 0; Index < Values->Count; Index++)
    {
        if (WpdKeyEqual(&Values->Entries[Index].Key, Key))
            return &Values->Entries[Index].Value;
    }
    return NULL;
}

NTSTATUS
WpdValuesSet(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_VALUE Value)
{
    PWPD_VALUE Existing;
    NTSTATUS Status;

    Existing = WpdValuesFind(Values, Key);
    if (Existing)
    {
        WpdValueClear(Existing);
        *Existing = *Value;
        RtlZeroMemory(Value, sizeof(*Value));
        return STATUS_SUCCESS;
    }
    Status = WpdReserve(Values, Values->Count + 1);
    if (!NT_SUCCESS(Status))
    {
        WpdValueClear(Value);
        return Status;
    }
    Values->Entries[Values->Count].Key = *Key;
    Values->Entries[Values->Count].Value = *Value;
    Values->Count++;
    RtlZeroMemory(Value, sizeof(*Value));
    return STATUS_SUCCESS;
}

NTSTATUS
WpdValuesSetUi4(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ ULONG Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_UI4;
    Item.Ui4 = Value;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetUi8(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ ULONGLONG Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_UI8;
    Item.Ui8 = Value;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetI8(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ LONGLONG Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_I8;
    Item.I8 = Value;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetBool(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ BOOLEAN Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_BOOL;
    Item.Bool = Value ? -1 : 0;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetError(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ HRESULT Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_ERROR;
    Item.I4 = Value;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetGuid(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ const GUID *Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_CLSID;
    Item.Guid = *Value;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdMakeString(
    _Out_ PWPD_VALUE Item,
    _In_opt_ PCWSTR Value)
{
    SIZE_T Chars = wcslen(Value ? Value : L"") + 1;

    RtlZeroMemory(Item, sizeof(*Item));
    if (Chars > MAXULONG / sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;
    Item->String.Buffer = ExAllocatePoolWithTag(PagedPool, Chars * sizeof(WCHAR), WPDMTP_TAG);
    if (!Item->String.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Item->String.Buffer, Value ? Value : L"", Chars * sizeof(WCHAR));
    Item->String.Bytes = (ULONG)(Chars * sizeof(WCHAR));
    Item->Vt = WPD_VT_LPWSTR;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdValuesSetString(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ PCWSTR Value)
{
    WPD_VALUE Item;
    NTSTATUS Status;

    Status = WpdMakeString(&Item, Value);
    if (!NT_SUCCESS(Status))
        return Status;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetBuffer(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ ULONG Size)
{
    WPD_VALUE Item = { 0 };

    Item.Blob.Buffer = ExAllocatePoolWithTag(PagedPool, max(Size, 1), WPDMTP_TAG);
    if (!Item.Blob.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Size)
        RtlCopyMemory(Item.Blob.Buffer, Data, Size);
    Item.Blob.Size = Size;
    Item.Vt = WPD_VT_VECTOR | WPD_VT_UI1;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetCollection(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ PWPD_COLLECTION Collection)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_UNKNOWN;
    Item.Collection = Collection;
    return WpdValuesSet(Values, Key, &Item);
}

NTSTATUS
WpdValuesSetKey(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ REFPROPERTYKEY Value)
{
    PWPD_COLLECTION Keys;
    NTSTATUS Status;

    Keys = WpdCollectionCreate(WpdKeys);
    if (!Keys)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = WpdKeysAdd(Keys, Value);
    if (!NT_SUCCESS(Status))
    {
        WpdCollectionFree(Keys);
        return Status;
    }
    return WpdValuesSetCollection(Values, Key, Keys);
}

BOOLEAN
WpdValuesGetUi4(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PULONG Value)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    *Value = 0;
    if (!Item)
        return FALSE;
    switch (Item->Vt)
    {
        case WPD_VT_UI4:
        case WPD_VT_I4:
        case WPD_VT_UINT:
        case WPD_VT_INT:
        case WPD_VT_ERROR:
            *Value = Item->Ui4;
            return TRUE;
        case WPD_VT_UI2:
            *Value = Item->Ui2;
            return TRUE;
        case WPD_VT_UI1:
            *Value = Item->Ui1;
            return TRUE;
        case WPD_VT_UI8:
        case WPD_VT_I8:
            *Value = (ULONG)Item->Ui8;
            return Item->Ui8 <= MAXULONG;
        default:
            return FALSE;
    }
}

BOOLEAN
WpdValuesGetUi8(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PULONGLONG Value)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);
    ULONG Small;

    *Value = 0;
    if (!Item)
        return FALSE;
    if (Item->Vt == WPD_VT_UI8 || Item->Vt == WPD_VT_I8)
    {
        *Value = Item->Ui8;
        return TRUE;
    }
    if (WpdValuesGetUi4(Values, Key, &Small))
    {
        *Value = Small;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN
WpdValuesGetI8(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PLONGLONG Value)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    *Value = 0;
    if (!Item)
        return FALSE;
    if (Item->Vt == WPD_VT_I8 || Item->Vt == WPD_VT_UI8)
    {
        *Value = Item->I8;
        return TRUE;
    }
    if (Item->Vt == WPD_VT_I4 || Item->Vt == WPD_VT_INT)
    {
        *Value = Item->I4;
        return TRUE;
    }
    if (Item->Vt == WPD_VT_UI4 || Item->Vt == WPD_VT_UINT)
    {
        *Value = Item->Ui4;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN
WpdValuesGetBool(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PBOOLEAN Value)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    *Value = FALSE;
    if (!Item || Item->Vt != WPD_VT_BOOL)
        return FALSE;
    *Value = Item->Bool != 0;
    return TRUE;
}

BOOLEAN
WpdValuesGetGuid(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ GUID *Value)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    RtlZeroMemory(Value, sizeof(*Value));
    if (!Item || Item->Vt != WPD_VT_CLSID)
        return FALSE;
    *Value = Item->Guid;
    return TRUE;
}

PCWSTR
WpdValuesGetString(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    if (!Item || Item->Vt != WPD_VT_LPWSTR)
        return NULL;
    return Item->String.Buffer;
}

BOOLEAN
WpdValuesGetBuffer(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PUCHAR *Data,
    _Out_ PULONG Size)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    *Data = NULL;
    *Size = 0;
    if (!Item || Item->Vt != (WPD_VT_VECTOR | WPD_VT_UI1))
        return FALSE;
    *Data = Item->Blob.Buffer;
    *Size = Item->Blob.Size;
    return TRUE;
}

PWPD_COLLECTION
WpdValuesGetCollection(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ WPD_COLLECTION_KIND Kind)
{
    PWPD_VALUE Item = WpdValuesFind(Values, Key);

    if (!Item || Item->Vt != WPD_VT_UNKNOWN || !Item->Collection || Item->Collection->Kind != Kind)
        return NULL;
    return Item->Collection;
}

BOOLEAN
WpdValuesGetKey(
    _In_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _Out_ PROPERTYKEY *Value)
{
    PWPD_COLLECTION Keys = WpdValuesGetCollection(Values, Key, WpdKeys);

    RtlZeroMemory(Value, sizeof(*Value));
    if (!Keys || !Keys->Count)
        return FALSE;
    *Value = Keys->Keys[0];
    return TRUE;
}

NTSTATUS
WpdKeysAdd(
    _Inout_ PWPD_COLLECTION Keys,
    _In_ REFPROPERTYKEY Key)
{
    NTSTATUS Status;

    Status = WpdReserve(Keys, Keys->Count + 1);
    if (!NT_SUCCESS(Status))
        return Status;
    Keys->Keys[Keys->Count++] = *Key;
    return STATUS_SUCCESS;
}

BOOLEAN
WpdKeysContain(
    _In_ PWPD_COLLECTION Keys,
    _In_ REFPROPERTYKEY Key)
{
    ULONG Index;

    for (Index = 0; Index < Keys->Count; Index++)
    {
        if (WpdKeyEqual(&Keys->Keys[Index], Key))
            return TRUE;
    }
    return FALSE;
}

NTSTATUS
WpdVariantsAdd(
    _Inout_ PWPD_COLLECTION Variants,
    _Inout_ PWPD_VALUE Value)
{
    NTSTATUS Status;

    Status = WpdReserve(Variants, Variants->Count + 1);
    if (!NT_SUCCESS(Status))
    {
        WpdValueClear(Value);
        return Status;
    }
    Variants->Items[Variants->Count++] = *Value;
    RtlZeroMemory(Value, sizeof(*Value));
    return STATUS_SUCCESS;
}

NTSTATUS
WpdListsAdd(
    _Inout_ PWPD_COLLECTION List,
    _In_ PWPD_COLLECTION Values)
{
    NTSTATUS Status;

    Status = WpdReserve(List, List->Count + 1);
    if (!NT_SUCCESS(Status))
    {
        WpdCollectionFree(Values);
        return Status;
    }
    List->Lists[List->Count++] = Values;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdVariantsAddString(
    _Inout_ PWPD_COLLECTION Variants,
    _In_ PCWSTR Value)
{
    WPD_VALUE Item;
    NTSTATUS Status;

    Status = WpdMakeString(&Item, Value);
    if (!NT_SUCCESS(Status))
        return Status;
    return WpdVariantsAdd(Variants, &Item);
}

NTSTATUS
WpdVariantsAddGuid(
    _Inout_ PWPD_COLLECTION Variants,
    _In_ const GUID *Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_CLSID;
    Item.Guid = *Value;
    return WpdVariantsAdd(Variants, &Item);
}

NTSTATUS
WpdVariantsAddError(
    _Inout_ PWPD_COLLECTION Variants,
    _In_ HRESULT Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_ERROR;
    Item.I4 = Value;
    return WpdVariantsAdd(Variants, &Item);
}

NTSTATUS
WpdVariantsAddUi4(
    _Inout_ PWPD_COLLECTION Variants,
    _In_ ULONG Value)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_UI4;
    Item.Ui4 = Value;
    return WpdVariantsAdd(Variants, &Item);
}

static
BOOLEAN
ReadBytes(
    _Inout_ PWPD_READER Reader,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (Reader->Size - Reader->Offset < Length)
        return FALSE;
    RtlCopyMemory(Buffer, Reader->Data + Reader->Offset, Length);
    Reader->Offset += Length;
    return TRUE;
}

static
BOOLEAN
ReadU32(
    _Inout_ PWPD_READER Reader,
    _Out_ PULONG Value)
{
    return ReadBytes(Reader, Value, sizeof(*Value));
}

static NTSTATUS ReadValue(_Inout_ PWPD_READER Reader, _Out_ PWPD_VALUE Value, _In_ ULONG Depth);

static
NTSTATUS
ReadCollectionBody(
    _Inout_ PWPD_READER Reader,
    _In_ const GUID *Clsid,
    _Out_ PWPD_COLLECTION *Result,
    _In_ ULONG Depth)
{
    WPD_COLLECTION_KIND Kind;
    PWPD_COLLECTION Collection;
    ULONG Count, Index;
    NTSTATUS Status;

    *Result = NULL;
    if (Depth > WPD_MAX_DEPTH)
        return STATUS_INVALID_PARAMETER;
    if (RtlEqualMemory(Clsid, &ClsidValues, sizeof(GUID)))
        Kind = WpdValues;
    else if (RtlEqualMemory(Clsid, &ClsidKeys, sizeof(GUID)))
        Kind = WpdKeys;
    else if (RtlEqualMemory(Clsid, &ClsidVariants, sizeof(GUID)))
        Kind = WpdVariants;
    else if (RtlEqualMemory(Clsid, &ClsidValuesList, sizeof(GUID)))
        Kind = WpdValuesList;
    else
        return STATUS_INVALID_PARAMETER;
    if (!ReadU32(Reader, &Count))
        return STATUS_INVALID_PARAMETER;
    if (Count > (Reader->Size - Reader->Offset) / 4)
        return STATUS_INVALID_PARAMETER;

    Collection = WpdCollectionCreate(Kind);
    if (!Collection)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = WpdReserve(Collection, Count);
    for (Index = 0; NT_SUCCESS(Status) && Index < Count; Index++)
    {
        switch (Kind)
        {
            case WpdValues:
                if (!ReadBytes(Reader, &Collection->Entries[Index].Key, sizeof(PROPERTYKEY)))
                {
                    Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                Status = ReadValue(Reader, &Collection->Entries[Index].Value, Depth + 1);
                break;
            case WpdKeys:
                if (!ReadBytes(Reader, &Collection->Keys[Index], sizeof(PROPERTYKEY)))
                    Status = STATUS_INVALID_PARAMETER;
                break;
            case WpdVariants:
                Status = ReadValue(Reader, &Collection->Items[Index], Depth + 1);
                break;
            default:
            {
                GUID Inner;

                if (!ReadBytes(Reader, &Inner, sizeof(Inner)) || !RtlEqualMemory(&Inner, &ClsidValues, sizeof(GUID)))
                {
                    Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                Status = ReadCollectionBody(Reader, &Inner, &Collection->Lists[Index], Depth + 1);
                break;
            }
        }
        if (NT_SUCCESS(Status))
            Collection->Count = Index + 1;
    }
    if (!NT_SUCCESS(Status))
    {
        WpdCollectionFree(Collection);
        return Status;
    }
    *Result = Collection;
    return STATUS_SUCCESS;
}

static
NTSTATUS
ReadValue(
    _Inout_ PWPD_READER Reader,
    _Out_ PWPD_VALUE Value,
    _In_ ULONG Depth)
{
    ULONG Vt, Length;
    GUID Clsid;

    RtlZeroMemory(Value, sizeof(*Value));
    if (!ReadU32(Reader, &Vt))
        return STATUS_INVALID_PARAMETER;
    switch (Vt)
    {
        case WPD_VT_EMPTY:
        case WPD_VT_NULL:
            break;
        case WPD_VT_I1:
        case WPD_VT_UI1:
            if (!ReadBytes(Reader, &Value->Ui1, 1))
                return STATUS_INVALID_PARAMETER;
            break;
        case WPD_VT_I2:
        case WPD_VT_UI2:
        case WPD_VT_BOOL:
            if (!ReadBytes(Reader, &Value->Ui2, 2))
                return STATUS_INVALID_PARAMETER;
            break;
        case WPD_VT_I4:
        case WPD_VT_UI4:
        case WPD_VT_INT:
        case WPD_VT_UINT:
        case WPD_VT_ERROR:
        case WPD_VT_R4:
            if (!ReadBytes(Reader, &Value->Ui4, 4))
                return STATUS_INVALID_PARAMETER;
            break;
        case WPD_VT_I8:
        case WPD_VT_UI8:
        case WPD_VT_R8:
        case WPD_VT_DATE:
        case WPD_VT_FILETIME:
            if (!ReadBytes(Reader, &Value->Ui8, 8))
                return STATUS_INVALID_PARAMETER;
            break;
        case WPD_VT_CLSID:
            if (!ReadBytes(Reader, &Value->Guid, sizeof(GUID)))
                return STATUS_INVALID_PARAMETER;
            break;
        case WPD_VT_LPWSTR:
            if (!ReadU32(Reader, &Length) || Length < sizeof(WCHAR) || (Length & 1) ||
                Reader->Size - Reader->Offset < Length)
            {
                return STATUS_INVALID_PARAMETER;
            }
            Value->String.Buffer = ExAllocatePoolWithTag(PagedPool, Length, WPDMTP_TAG);
            if (!Value->String.Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            ReadBytes(Reader, Value->String.Buffer, Length);
            Value->String.Buffer[Length / sizeof(WCHAR) - 1] = UNICODE_NULL;
            Value->String.Bytes = (ULONG)((wcslen(Value->String.Buffer) + 1) * sizeof(WCHAR));
            break;
        case WPD_VT_VECTOR | WPD_VT_UI1:
            if (!ReadU32(Reader, &Length) || Reader->Size - Reader->Offset < Length)
                return STATUS_INVALID_PARAMETER;
            Value->Blob.Buffer = ExAllocatePoolWithTag(PagedPool, max(Length, 1), WPDMTP_TAG);
            if (!Value->Blob.Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            ReadBytes(Reader, Value->Blob.Buffer, Length);
            Value->Blob.Size = Length;
            break;
        case WPD_VT_UNKNOWN:
        {
            NTSTATUS Status;

            if (!ReadBytes(Reader, &Clsid, sizeof(Clsid)))
                return STATUS_INVALID_PARAMETER;
            Status = ReadCollectionBody(Reader, &Clsid, &Value->Collection, Depth);
            if (!NT_SUCCESS(Status))
                return Status;
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }
    Value->Vt = (USHORT)Vt;
    return STATUS_SUCCESS;
}

NTSTATUS
WpdDeserialize(
    _In_reads_bytes_(Size) const UCHAR *Buffer,
    _In_ ULONG Size,
    _Out_ PWPD_COLLECTION *Values)
{
    WPD_READER Reader;
    ULONG Vt;
    GUID Clsid;

    *Values = NULL;
    Reader.Data = Buffer;
    Reader.Size = Size;
    Reader.Offset = 0;
    if (!ReadU32(&Reader, &Vt) || Vt != WPD_VT_UNKNOWN ||
        !ReadBytes(&Reader, &Clsid, sizeof(Clsid)) || !RtlEqualMemory(&Clsid, &ClsidValues, sizeof(GUID)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    return ReadCollectionBody(&Reader, &Clsid, Values, 0);
}

static ULONG CollectionBodySize(_In_ PWPD_COLLECTION Collection);

static
ULONG
ValueSize(
    _In_ const WPD_VALUE *Value)
{
    switch (Value->Vt)
    {
        case WPD_VT_EMPTY:
        case WPD_VT_NULL:
            return 4;
        case WPD_VT_I1:
        case WPD_VT_UI1:
            return 5;
        case WPD_VT_I2:
        case WPD_VT_UI2:
        case WPD_VT_BOOL:
            return 6;
        case WPD_VT_I8:
        case WPD_VT_UI8:
        case WPD_VT_R8:
        case WPD_VT_DATE:
        case WPD_VT_FILETIME:
            return 12;
        case WPD_VT_CLSID:
            return 4 + sizeof(GUID);
        case WPD_VT_LPWSTR:
            return 8 + Value->String.Bytes;
        case WPD_VT_VECTOR | WPD_VT_UI1:
            return 8 + Value->Blob.Size;
        case WPD_VT_UNKNOWN:
            return 4 + CollectionBodySize(Value->Collection);
        default:
            return 8;
    }
}

static
ULONG
CollectionBodySize(
    _In_ PWPD_COLLECTION Collection)
{
    ULONG Size = sizeof(GUID) + 4, Index;

    for (Index = 0; Index < Collection->Count; Index++)
    {
        switch (Collection->Kind)
        {
            case WpdValues:
                if (Collection->Entries[Index].Value.Vt != WPD_VT_EMPTY)
                    Size += sizeof(PROPERTYKEY) + ValueSize(&Collection->Entries[Index].Value);
                break;
            case WpdKeys:
                Size += sizeof(PROPERTYKEY);
                break;
            case WpdVariants:
                Size += ValueSize(&Collection->Items[Index]);
                break;
            default:
                Size += CollectionBodySize(Collection->Lists[Index]);
                break;
        }
    }
    return Size;
}

ULONG
WpdSerializedSize(
    _In_ PWPD_COLLECTION Values)
{
    return 4 + CollectionBodySize(Values);
}

static
PUCHAR
PutU32(
    _Inout_ PUCHAR Cursor,
    _In_ ULONG Value)
{
    RtlCopyMemory(Cursor, &Value, 4);
    return Cursor + 4;
}

static PUCHAR WriteCollectionBody(_Inout_ PUCHAR Cursor, _In_ PWPD_COLLECTION Collection);

static
PUCHAR
WriteValue(
    _Inout_ PUCHAR Cursor,
    _In_ const WPD_VALUE *Value)
{
    Cursor = PutU32(Cursor, Value->Vt);
    switch (Value->Vt)
    {
        case WPD_VT_EMPTY:
        case WPD_VT_NULL:
            break;
        case WPD_VT_I1:
        case WPD_VT_UI1:
            *Cursor++ = Value->Ui1;
            break;
        case WPD_VT_I2:
        case WPD_VT_UI2:
        case WPD_VT_BOOL:
            RtlCopyMemory(Cursor, &Value->Ui2, 2);
            Cursor += 2;
            break;
        case WPD_VT_I8:
        case WPD_VT_UI8:
        case WPD_VT_R8:
        case WPD_VT_DATE:
        case WPD_VT_FILETIME:
            RtlCopyMemory(Cursor, &Value->Ui8, 8);
            Cursor += 8;
            break;
        case WPD_VT_CLSID:
            RtlCopyMemory(Cursor, &Value->Guid, sizeof(GUID));
            Cursor += sizeof(GUID);
            break;
        case WPD_VT_LPWSTR:
            Cursor = PutU32(Cursor, Value->String.Bytes);
            RtlCopyMemory(Cursor, Value->String.Buffer, Value->String.Bytes);
            Cursor += Value->String.Bytes;
            break;
        case WPD_VT_VECTOR | WPD_VT_UI1:
            Cursor = PutU32(Cursor, Value->Blob.Size);
            if (Value->Blob.Size)
                RtlCopyMemory(Cursor, Value->Blob.Buffer, Value->Blob.Size);
            Cursor += Value->Blob.Size;
            break;
        case WPD_VT_UNKNOWN:
            Cursor = WriteCollectionBody(Cursor, Value->Collection);
            break;
        default:
            Cursor = PutU32(Cursor, Value->Ui4);
            break;
    }
    return Cursor;
}

static
PUCHAR
WriteCollectionBody(
    _Inout_ PUCHAR Cursor,
    _In_ PWPD_COLLECTION Collection)
{
    const GUID *Clsid;
    ULONG Index, Count = 0;

    switch (Collection->Kind)
    {
        case WpdValues:
            Clsid = &ClsidValues;
            for (Index = 0; Index < Collection->Count; Index++)
            {
                if (Collection->Entries[Index].Value.Vt != WPD_VT_EMPTY)
                    Count++;
            }
            break;
        case WpdKeys:
            Clsid = &ClsidKeys;
            Count = Collection->Count;
            break;
        case WpdVariants:
            Clsid = &ClsidVariants;
            Count = Collection->Count;
            break;
        default:
            Clsid = &ClsidValuesList;
            Count = Collection->Count;
            break;
    }
    RtlCopyMemory(Cursor, Clsid, sizeof(GUID));
    Cursor = PutU32(Cursor + sizeof(GUID), Count);
    for (Index = 0; Index < Collection->Count; Index++)
    {
        switch (Collection->Kind)
        {
            case WpdValues:
                if (Collection->Entries[Index].Value.Vt == WPD_VT_EMPTY)
                    break;
                RtlCopyMemory(Cursor, &Collection->Entries[Index].Key, sizeof(PROPERTYKEY));
                Cursor = WriteValue(Cursor + sizeof(PROPERTYKEY), &Collection->Entries[Index].Value);
                break;
            case WpdKeys:
                RtlCopyMemory(Cursor, &Collection->Keys[Index], sizeof(PROPERTYKEY));
                Cursor += sizeof(PROPERTYKEY);
                break;
            case WpdVariants:
                Cursor = WriteValue(Cursor, &Collection->Items[Index]);
                break;
            default:
                Cursor = WriteCollectionBody(Cursor, Collection->Lists[Index]);
                break;
        }
    }
    return Cursor;
}

VOID
WpdSerialize(
    _In_ PWPD_COLLECTION Values,
    _Out_writes_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size)
{
    UNREFERENCED_PARAMETER(Size);
    WriteCollectionBody(PutU32(Buffer, WPD_VT_UNKNOWN), Values);
}
