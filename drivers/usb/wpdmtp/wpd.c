/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WPD commands: dispatch, capabilities, enumeration, properties and events
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"
#include "wpdcmd.h"

#define NDEBUG
#include <debug.h>

static const GUID WpdPrivateCategory = { 0xB520E0C2, 0x41DF, 0x4EBD, { 0x93, 0xE7, 0x5B, 0x99, 0xC1, 0x01, 0x93, 0x2F } };
static const PROPERTYKEY WpdPrivateCommand11 = { { 0xB520E0C2, 0x41DF, 0x4EBD, { 0x93, 0xE7, 0x5B, 0x99, 0xC1, 0x01, 0x93, 0x2F } }, 11 };
static const PROPERTYKEY WpdPrivateCommand12 = { { 0xB520E0C2, 0x41DF, 0x4EBD, { 0x93, 0xE7, 0x5B, 0x99, 0xC1, 0x01, 0x93, 0x2F } }, 12 };
static const PROPERTYKEY WpdPrivateResult1002 = { { 0xB520E0C2, 0x41DF, 0x4EBD, { 0x93, 0xE7, 0x5B, 0x99, 0xC1, 0x01, 0x93, 0x2F } }, 1002 };

static const PROPERTYKEY *const SupportedCommands[] =
{
    &WPD_COMMAND_OBJECT_ENUMERATION_START_FIND,
    &WPD_COMMAND_OBJECT_ENUMERATION_FIND_NEXT,
    &WPD_COMMAND_OBJECT_ENUMERATION_END_FIND,
    &WPD_COMMAND_OBJECT_PROPERTIES_GET_SUPPORTED,
    &WPD_COMMAND_OBJECT_PROPERTIES_GET,
    &WPD_COMMAND_OBJECT_PROPERTIES_GET_ALL,
    &WPD_COMMAND_OBJECT_PROPERTIES_GET_ATTRIBUTES,
    &WPD_COMMAND_OBJECT_RESOURCES_GET_SUPPORTED,
    &WPD_COMMAND_OBJECT_RESOURCES_GET_ATTRIBUTES,
    &WPD_COMMAND_OBJECT_RESOURCES_OPEN,
    &WPD_COMMAND_OBJECT_RESOURCES_READ,
    &WPD_COMMAND_OBJECT_RESOURCES_CLOSE,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_COMMANDS,
    &WPD_COMMAND_CAPABILITIES_GET_COMMAND_OPTIONS,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FUNCTIONAL_CATEGORIES,
    &WPD_COMMAND_CAPABILITIES_GET_FUNCTIONAL_OBJECTS,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_CONTENT_TYPES,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FORMATS,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_FORMAT_PROPERTIES,
    &WPD_COMMAND_CAPABILITIES_GET_FIXED_PROPERTY_ATTRIBUTES,
    &WPD_COMMAND_CAPABILITIES_GET_SUPPORTED_EVENTS,
    &WPD_COMMAND_CAPABILITIES_GET_EVENT_OPTIONS,
    &WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES,
    &WPD_COMMAND_MTP_EXT_GET_VENDOR_EXTENSION_DESCRIPTION,
    &WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE,
    &WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ,
    &WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE,
    &WPD_COMMAND_MTP_EXT_READ_DATA,
    &WPD_COMMAND_MTP_EXT_WRITE_DATA,
    &WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER,
    &WpdPrivateCommand11,
    &WpdPrivateCommand12,
};

static const PROPERTYKEY *const ManagementCommands[] =
{
    &WPD_COMMAND_OBJECT_MANAGEMENT_DELETE_OBJECTS,
    &WPD_COMMAND_OBJECT_MANAGEMENT_CREATE_OBJECT_WITH_PROPERTIES_ONLY,
    &WPD_COMMAND_OBJECT_MANAGEMENT_CREATE_OBJECT_WITH_PROPERTIES_AND_DATA,
    &WPD_COMMAND_OBJECT_MANAGEMENT_WRITE_OBJECT_DATA,
    &WPD_COMMAND_OBJECT_MANAGEMENT_COMMIT_OBJECT,
    &WPD_COMMAND_OBJECT_MANAGEMENT_REVERT_OBJECT,
};

static const PROPERTYKEY *const BulkCommands[] =
{
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_START,
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_NEXT,
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_LIST_END,
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_START,
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_NEXT,
    &WPD_COMMAND_OBJECT_PROPERTIES_BULK_GET_VALUES_BY_OBJECT_FORMAT_END,
};

static const PROPERTYKEY *const DeviceKeys[] =
{
    &WPD_OBJECT_ID, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID, &WPD_OBJECT_PERSISTENT_UNIQUE_ID,
    &WPD_OBJECT_PARENT_ID, &WPD_OBJECT_FORMAT, &WPD_OBJECT_CONTENT_TYPE, &WPD_FUNCTIONAL_OBJECT_CATEGORY,
    &WPD_OBJECT_CAN_DELETE, &WPD_DEVICE_FIRMWARE_VERSION, &WPD_DEVICE_PROTOCOL, &WPD_DEVICE_MANUFACTURER,
    &WPD_DEVICE_MODEL, &WPD_OBJECT_NAME, &WPD_DEVICE_SUPPORTS_NON_CONSUMABLE, &WPD_DEVICE_SERIAL_NUMBER,
    &WPD_DEVICE_TYPE, &WPD_DEVICE_TRANSPORT, &WPD_DEVICE_POWER_LEVEL, &WPD_DEVICE_POWER_SOURCE,
    &WPD_DEVICE_FRIENDLY_NAME,
};

static const PROPERTYKEY *const StorageKeys[] =
{
    &WPD_OBJECT_ID, &WPD_OBJECT_PARENT_ID, &WPD_OBJECT_FORMAT, &WPD_OBJECT_CAN_DELETE, &WPD_OBJECT_CONTENT_TYPE,
    &WPD_FUNCTIONAL_OBJECT_CATEGORY, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID, &WPD_STORAGE_TYPE,
    &WPD_STORAGE_FILE_SYSTEM_TYPE, &WPD_STORAGE_ACCESS_CAPABILITY, &WPD_STORAGE_CAPACITY,
    &WPD_STORAGE_FREE_SPACE_IN_BYTES, &WPD_STORAGE_FREE_SPACE_IN_OBJECTS, &WPD_STORAGE_DESCRIPTION,
    &WPD_OBJECT_NAME, &WPD_STORAGE_SERIAL_NUMBER, &WPD_OBJECT_PERSISTENT_UNIQUE_ID,
};

static const PROPERTYKEY *const StorageValueOrder[] =
{
    &WPD_STORAGE_TYPE, &WPD_STORAGE_FILE_SYSTEM_TYPE, &WPD_STORAGE_ACCESS_CAPABILITY, &WPD_STORAGE_CAPACITY,
    &WPD_STORAGE_FREE_SPACE_IN_BYTES, &WPD_STORAGE_FREE_SPACE_IN_OBJECTS, &WPD_STORAGE_DESCRIPTION,
    &WPD_OBJECT_PERSISTENT_UNIQUE_ID, &WPD_OBJECT_NAME, &WPD_STORAGE_SERIAL_NUMBER, &WPD_OBJECT_ID,
    &WPD_OBJECT_PARENT_ID, &WPD_OBJECT_FORMAT, &WPD_OBJECT_CAN_DELETE, &WPD_OBJECT_CONTENT_TYPE,
    &WPD_FUNCTIONAL_OBJECT_CATEGORY, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID,
};

static const WPD_PROP_MAP ObjectProps[] =
{
    { &WPD_OBJECT_ID, 0, TRUE, WpdConvCore, FALSE },
    { &WPD_OBJECT_PARENT_ID, MTP_PROP_PARENT_OBJECT, TRUE, WpdConvCore, FALSE },
    { &WPD_OBJECT_FORMAT, MTP_PROP_OBJECT_FORMAT, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_CONTENT_TYPE, MTP_PROP_OBJECT_FORMAT, FALSE, WpdConvCore, TRUE },
    { &WPD_OBJECT_NAME, MTP_PROP_NAME, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID, MTP_PROP_STORAGE_ID, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_PERSISTENT_UNIQUE_ID, MTP_PROP_PERSISTENT_UID, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_SIZE, MTP_PROP_OBJECT_SIZE, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_ORIGINAL_FILE_NAME, MTP_PROP_OBJECT_FILE_NAME, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_DATE_CREATED, MTP_PROP_DATE_CREATED, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_DATE_MODIFIED, MTP_PROP_DATE_MODIFIED, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_ISHIDDEN, MTP_PROP_HIDDEN, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_ISSYSTEM, MTP_PROP_SYSTEM_OBJECT, FALSE, WpdConvCore, FALSE },
    { &WPD_OBJECT_CAN_DELETE, MTP_PROP_PROTECTION_STATUS, FALSE, WpdConvCore, TRUE },
    { &WPD_OBJECT_NON_CONSUMABLE, MTP_PROP_NON_CONSUMABLE, FALSE, WpdConvCore, FALSE },
    { &WPD_DEVICE_MODEL, 0xDC45, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_ARTIST, 0xDC46, FALSE, WpdConvString, FALSE },
    { &WPD_OBJECT_DATE_AUTHORED, 0xDC47, FALSE, WpdConvDate, FALSE },
    { &WPD_MEDIA_DESCRIPTION, 0xDC48, FALSE, WpdConvString, FALSE },
    { &WPD_OBJECT_LANGUAGE_LOCALE, 0xDC4A, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_COPYRIGHT, 0xDC4B, FALSE, WpdConvString, FALSE },
    { &WPD_VIDEO_RECORDEDTV_STATION_NAME, 0xDC4C, FALSE, WpdConvString, FALSE },
    { &WPD_DEVICE_SERIAL_NUMBER, 0xDC51, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_WIDTH, 0xDC87, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_HEIGHT, 0xDC88, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_DURATION, 0xDC89, FALSE, WpdConvUi8, FALSE },
    { &WPD_MEDIA_USER_EFFECTIVE_RATING, 0xDC8A, FALSE, WpdConvUi4, FALSE },
    { &WPD_MUSIC_TRACK, 0xDC8B, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_GENRE, 0xDC8C, FALSE, WpdConvString, FALSE },
    { &WPD_VIDEO_CREDITS, 0xDC8D, FALSE, WpdConvString, FALSE },
    { &WPD_MUSIC_LYRICS, 0xDC8E, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_SUBSCRIPTION_CONTENT_ID, 0xDC8F, FALSE, WpdConvString, FALSE },
    { &WPD_VIDEO_AUTHOR, 0xDC90, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_USE_COUNT, 0xDC91, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_SKIP_COUNT, 0xDC92, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_LAST_ACCESSED_TIME, 0xDC93, FALSE, WpdConvDate, FALSE },
    { &WPD_MEDIA_PARENTAL_RATING, 0xDC94, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_META_GENRE, 0xDC95, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_COMPOSER, 0xDC96, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_EFFECTIVE_RATING, 0xDC97, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_SUB_TITLE, 0xDC98, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_RELEASE_DATE, 0xDC99, FALSE, WpdConvDate, FALSE },
    { &WPD_MUSIC_ALBUM, 0xDC9A, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_ALBUM_ARTIST, 0xDC9B, FALSE, WpdConvString, FALSE },
    { &WPD_MUSIC_MOOD, 0xDC9C, FALSE, WpdConvString, FALSE },
    { &WPD_OBJECT_IS_DRM_PROTECTED, 0xDC9D, FALSE, WpdConvBool, FALSE },
    { &WPD_MEDIA_SUB_DESCRIPTION, 0xDC9E, FALSE, WpdConvString, FALSE },
    { &WPD_MEDIA_TOTAL_BITRATE, 0xDE91, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_BITRATE_TYPE, 0xDE92, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_SAMPLE_RATE, 0xDE93, FALSE, WpdConvUi4, FALSE },
    { &WPD_AUDIO_CHANNEL_COUNT, 0xDE94, FALSE, WpdConvChannels, FALSE },
    { &WPD_AUDIO_BIT_DEPTH, 0xDE95, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_SCAN_TYPE, 0xDE97, FALSE, WpdConvUi4, FALSE },
    { &WPD_AUDIO_FORMAT_CODE, 0xDE99, FALSE, WpdConvUi4, FALSE },
    { &WPD_AUDIO_BITRATE, 0xDE9A, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_FOURCC_CODE, 0xDE9B, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_BITRATE, 0xDE9C, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_FRAMERATE, 0xDE9D, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_KEY_FRAME_DISTANCE, 0xDE9E, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_BUFFER_SIZE, 0xDE9F, FALSE, WpdConvUi4, FALSE },
    { &WPD_VIDEO_QUALITY_SETTING, 0xDEA0, FALSE, WpdConvUi4, FALSE },
    { &WPD_MEDIA_ENCODING_PROFILE, 0xDEA1, FALSE, WpdConvString, FALSE },
};

static const ULONG ChannelCountBits[] =
{
    0x00000000, 0x3F800000, 0x40000000, 0x40066666, 0x40400000, 0x40466666, 0x40800000, 0x40833333,
    0x40A00000, 0x40A33333, 0x40C00000, 0x40C33333, 0x40E00000, 0x40E33333, 0x41000000, 0x4101999A,
    0x41100000, 0x4111999A,
};

BOOLEAN
WpdIsCommand(
    _In_ const WPD_COMMAND *Command,
    _In_ REFPROPERTYKEY Key)
{
    return RtlEqualMemory(&Command->Category, &Key->fmtid, sizeof(GUID)) && Command->Id == Key->pid;
}

HRESULT
WpdNtToHresult(
    _In_ NTSTATUS Status)
{
    if (Status == STATUS_INSUFFICIENT_RESOURCES)
        return WPD_E_OUTOFMEMORY;
    if (Status == STATUS_INVALID_PARAMETER)
        return WPD_E_INVALIDARG;
    return NT_SUCCESS(Status) ? WPD_S_OK : WPD_E_GEN_FAILURE;
}

VOID
WpdMakeContextName(
    _Out_writes_(Chars) PWCHAR Buffer,
    _In_ SIZE_T Chars,
    _In_ PCWSTR Prefix)
{
    GUID Guid;

    if (!NT_SUCCESS(ExUuidCreate(&Guid)))
        RtlZeroMemory(&Guid, sizeof(Guid));
    RtlStringCchPrintfW(Buffer, Chars, L"%s{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Prefix,
                        Guid.Data1, Guid.Data2, Guid.Data3, Guid.Data4[0], Guid.Data4[1], Guid.Data4[2],
                        Guid.Data4[3], Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]);
}

PWPDMTP_CONTEXT
WpdFindContext(
    _In_ PWPDMTP_DEVICE Dev,
    _In_opt_ PCWSTR Name,
    _In_ WPDMTP_CONTEXT_TYPE Type)
{
    PLIST_ENTRY Entry;

    if (!Name)
        return NULL;
    for (Entry = Dev->Contexts.Flink; Entry != &Dev->Contexts; Entry = Entry->Flink)
    {
        PWPDMTP_CONTEXT Context = CONTAINING_RECORD(Entry, WPDMTP_CONTEXT, Link);

        if (Context->Type == Type && !wcscmp(Context->Name, Name))
            return Context;
    }
    return NULL;
}

PWPDMTP_CONTEXT
WpdCreateContext(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ WPDMTP_CONTEXT_TYPE Type,
    _In_ PCWSTR Prefix)
{
    PWPDMTP_CONTEXT Context;

    Context = ExAllocatePoolZero(PagedPool, sizeof(*Context), WPDMTP_TAG);
    if (!Context)
        return NULL;
    Context->Type = Type;
    Context->Owner = File;
    WpdMakeContextName(Context->Name, ARRAYSIZE(Context->Name), Prefix);
    InsertTailList(&Dev->Contexts, &Context->Link);
    return Context;
}

VOID
WpdFreeContext(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_CONTEXT Context)
{
    if (Dev->ActiveTransfer == Context)
    {
        WpdMtpAbortData(Dev);
        Dev->ActiveTransfer = NULL;
    }
    RemoveEntryList(&Context->Link);
    if (Context->Handles)
        ExFreePoolWithTag(Context->Handles, WPDMTP_TAG);
    WpdCollectionFree(Context->Properties);
    ExFreePoolWithTag(Context, WPDMTP_TAG);
}

BOOLEAN
WpdParseObjectId(
    _In_opt_ PCWSTR Id,
    _Out_ PWPD_OBJECT_REF Ref)
{
    ULONG Value = 0;
    PCWSTR Cursor;

    RtlZeroMemory(Ref, sizeof(*Ref));
    if (!Id || !Id[0])
        return FALSE;
    if (!wcscmp(Id, WPD_DEVICE_OBJECT_ID))
    {
        Ref->Kind = WpdRefDevice;
        return TRUE;
    }
    if ((Id[0] != L's' && Id[0] != L'o') || !Id[1] || wcslen(Id) > 9)
        return FALSE;
    for (Cursor = Id + 1; *Cursor; Cursor++)
    {
        WCHAR c = *Cursor;

        if (c >= L'0' && c <= L'9')
            Value = Value * 16 + (c - L'0');
        else if (c >= L'A' && c <= L'F')
            Value = Value * 16 + (c - L'A' + 10);
        else if (c >= L'a' && c <= L'f')
            Value = Value * 16 + (c - L'a' + 10);
        else
            return FALSE;
    }
    Ref->Kind = Id[0] == L's' ? WpdRefStorage : WpdRefObject;
    Ref->Value = Value;
    return TRUE;
}

VOID
WpdFormatObjectId(
    _Out_writes_(16) PWCHAR Buffer,
    _In_ BOOLEAN Storage,
    _In_ ULONG Value)
{
    RtlStringCchPrintfW(Buffer, 16, L"%c%lX", Storage ? L's' : L'o', Value);
}

static
VOID
WpdParentId(
    _In_ PMTP_OBJECT Object,
    _Out_writes_(16) PWCHAR Buffer)
{
    if (!Object->Parent || Object->Parent == MTP_ROOT_PARENT)
        WpdFormatObjectId(Buffer, TRUE, Object->StorageId);
    else
        WpdFormatObjectId(Buffer, FALSE, Object->Parent);
}

static
VOID
WpdFormatPuid(
    _In_reads_bytes_(16) const UCHAR *Puid,
    _Out_writes_(40) PWCHAR Buffer)
{
    GUID Guid;

    RtlCopyMemory(&Guid, Puid, sizeof(Guid));
    RtlStringCchPrintfW(Buffer, 40, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                        Guid.Data1, Guid.Data2, Guid.Data3, Guid.Data4[0], Guid.Data4[1], Guid.Data4[2],
                        Guid.Data4[3], Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]);
}

static
VOID
WpdStoragePuid(
    _In_ PMTP_STORAGE Storage,
    _Out_writes_(Chars) PWCHAR Buffer,
    _In_ SIZE_T Chars)
{
    RtlStringCchPrintfW(Buffer, Chars, L"SID-{%lX,%s,%I64u}", Storage->Id,
                        Storage->VolumeIdentifier ? Storage->VolumeIdentifier : L"", Storage->MaxCapacity);
}

static
LONG
WpdDaysFromCivil(
    _In_ LONG Year,
    _In_ ULONG Month,
    _In_ ULONG Day)
{
    LONG Era;
    ULONG YearOfEra, DayOfYear, DayOfEra;

    Year -= Month <= 2;
    Era = (Year >= 0 ? Year : Year - 399) / 400;
    YearOfEra = (ULONG)(Year - Era * 400);
    DayOfYear = (153 * (Month > 2 ? Month - 3 : Month + 9) + 2) / 5 + Day - 1;
    DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
    return Era * 146097 + (LONG)DayOfEra - 719468;
}

static
ULONGLONG
WpdSecondsToDateBits(
    _In_ ULONGLONG Seconds)
{
    ULONGLONG Hi = Seconds / 86400, Lo = 0, Rem = Seconds % 86400, Mantissa;
    ULONG Bit, Top, Shift;
    BOOLEAN Round, Sticky;
    LONG Exponent;

    if (!Seconds)
        return 0;
    for (Bit = 0; Bit < 64; Bit++)
    {
        Rem <<= 1;
        Lo <<= 1;
        if (Rem >= 86400)
        {
            Rem -= 86400;
            Lo |= 1;
        }
    }
    Sticky = Rem != 0;
    if (Hi)
    {
        for (Top = 63; !(Hi >> Top); Top--);
        Top += 64;
    }
    else
    {
        for (Top = 63; !(Lo >> Top); Top--);
    }
    Exponent = (LONG)Top - 64;
    Shift = Top - 52;
    if (Shift >= 64)
        Mantissa = (Hi >> (Shift - 64));
    else if (Shift)
        Mantissa = (Lo >> Shift) | (Shift ? (Hi << (64 - Shift)) : 0);
    else
        Mantissa = Lo;
    if (!Shift)
    {
        Round = FALSE;
    }
    else
    {
        ULONG RoundBit = Shift - 1;

        Round = RoundBit >= 64 ? (Hi >> (RoundBit - 64)) & 1 : (Lo >> RoundBit) & 1;
        if (RoundBit >= 64)
            Sticky |= (Lo != 0) || (RoundBit > 64 && (Hi & ((1ULL << (RoundBit - 64)) - 1)));
        else if (RoundBit)
            Sticky |= (Lo & ((1ULL << RoundBit) - 1)) != 0;
    }
    if (Round && (Sticky || (Mantissa & 1)))
    {
        Mantissa++;
        if (Mantissa >> 53)
        {
            Mantissa >>= 1;
            Exponent++;
        }
    }
    return ((ULONGLONG)(Exponent + 1023) << 52) | (Mantissa & ((1ULL << 52) - 1));
}

static
BOOLEAN
WpdParseMtpDate(
    _In_opt_ PCWSTR Text,
    _Out_ PULONGLONG Bits)
{
    ULONG Fields[6] = { 0 }, Widths[6] = { 4, 2, 2, 2, 2, 2 }, Index, Digit;
    PCWSTR Cursor = Text;
    LONG Days;

    *Bits = 0;
    if (!Text || wcslen(Text) < 15)
        return FALSE;
    for (Index = 0; Index < 6; Index++)
    {
        if (Index == 3)
        {
            if (*Cursor != L'T')
                return FALSE;
            Cursor++;
        }
        for (Digit = 0; Digit < Widths[Index]; Digit++, Cursor++)
        {
            if (*Cursor < L'0' || *Cursor > L'9')
                return FALSE;
            Fields[Index] = Fields[Index] * 10 + (*Cursor - L'0');
        }
    }
    if (Fields[0] < 1900 || !Fields[1] || Fields[1] > 12 || !Fields[2] || Fields[2] > 31 ||
        Fields[3] > 23 || Fields[4] > 59 || Fields[5] > 60)
    {
        return FALSE;
    }
    Days = WpdDaysFromCivil(Fields[0], Fields[1], Fields[2]) - WpdDaysFromCivil(1899, 12, 30);
    *Bits = WpdSecondsToDateBits((ULONGLONG)Days * 86400 + Fields[3] * 3600 + Fields[4] * 60 + Fields[5]);
    return TRUE;
}

static
NTSTATUS
WpdSetDate(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ ULONGLONG Bits)
{
    WPD_VALUE Item = { 0 };

    Item.Vt = WPD_VT_DATE;
    Item.Ui8 = Bits;
    return WpdValuesSet(Values, Key, &Item);
}

static
BOOLEAN
WpdObjectHasKey(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _In_ const WPD_PROP_MAP *Map)
{
    if (!Map->Prop || Map->Prop == MTP_PROP_OBJECT_FORMAT || Map->Prop == MTP_PROP_NAME ||
        Map->Prop == MTP_PROP_STORAGE_ID || Map->Prop == MTP_PROP_PARENT_OBJECT)
    {
        return TRUE;
    }
    return WpdMtpFormatHasProp(Dev, Format, Map->Prop);
}

static
BOOLEAN
WpdPropClaimed(
    _In_ USHORT Prop)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(ObjectProps); Index++)
    {
        if (ObjectProps[Index].Prop == Prop && !ObjectProps[Index].Derived)
            return TRUE;
    }
    return FALSE;
}

static
VOID
WpdVendorKey(
    _In_ BOOLEAN Device,
    _In_ USHORT Prop,
    _Out_ PROPERTYKEY *Key)
{
    Key->fmtid = Device ? WPD_PROPERTIES_MTP_VENDOR_EXTENDED_DEVICE_PROPS : WPD_PROPERTIES_MTP_VENDOR_EXTENDED_OBJECT_PROPS;
    Key->pid = Prop;
}

static
BOOLEAN
WpdVendorProp(
    _In_ BOOLEAN Device,
    _In_ REFPROPERTYKEY Key,
    _Out_ PUSHORT Prop)
{
    const GUID *Fmtid = Device ? &WPD_PROPERTIES_MTP_VENDOR_EXTENDED_DEVICE_PROPS : &WPD_PROPERTIES_MTP_VENDOR_EXTENDED_OBJECT_PROPS;

    *Prop = 0;
    if (!RtlEqualMemory(&Key->fmtid, Fmtid, sizeof(GUID)) || !Key->pid || Key->pid > MAXUSHORT)
        return FALSE;
    *Prop = (USHORT)Key->pid;
    return TRUE;
}

static
NTSTATUS
WpdObjectKeys(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _In_opt_ PMTP_OBJECT Object,
    _Inout_ PWPD_COLLECTION Keys)
{
    PMTP_FORMAT Entry;
    PROPERTYKEY Key;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    for (Index = 0; Index < ARRAYSIZE(ObjectProps) && NT_SUCCESS(Status); Index++)
    {
        if (!WpdObjectHasKey(Dev, Format, &ObjectProps[Index]))
            continue;
        if (Object && ObjectProps[Index].Key == &WPD_OBJECT_PERSISTENT_UNIQUE_ID && !Object->PuidValid)
            continue;
        Status = WpdKeysAdd(Keys, ObjectProps[Index].Key);
    }
    Entry = WpdMtpFindFormat(Dev, Format);
    if (!Entry)
        Entry = WpdMtpFindFormat(Dev, MTP_FORMAT_UNDEFINED);
    for (Index = 0; Entry && Index < Entry->PropCount && NT_SUCCESS(Status); Index++)
    {
        if (WpdPropClaimed(Entry->Props[Index]))
            continue;
        WpdVendorKey(FALSE, Entry->Props[Index], &Key);
        Status = WpdKeysAdd(Keys, &Key);
    }
    return Status;
}

NTSTATUS
WpdFormatKeys(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _Inout_ PWPD_COLLECTION Keys)
{
    return WpdObjectKeys(Dev, Format, NULL, Keys);
}

const WPD_PROP_MAP *
WpdFindPropMap(
    _In_ REFPROPERTYKEY Key)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(ObjectProps); Index++)
    {
        if (WpdKeyEqual(ObjectProps[Index].Key, Key))
            return &ObjectProps[Index];
    }
    return NULL;
}

static
BOOLEAN
WpdConvert(
    _In_ UCHAR Conv,
    _In_opt_ const MTP_VALUE *Value,
    _Out_ PWPD_VALUE Item)
{
    LONGLONG Integer;
    ULONGLONG Bits;

    RtlZeroMemory(Item, sizeof(*Item));
    if (!Value || !Value->Valid)
        return FALSE;
    if (Value->DataType == MTP_TYPE_STRING || Value->DataType == MTP_TYPE_AUINT16)
    {
        if (Conv == WpdConvDate)
        {
            if (!WpdParseMtpDate(Value->String, &Bits))
                return FALSE;
            Item->Vt = WPD_VT_DATE;
            Item->Ui8 = Bits;
            return TRUE;
        }
        if (Conv != WpdConvString && Conv != WpdConvRaw)
            return FALSE;
        return NT_SUCCESS(WpdMakeString(Item, Value->String));
    }
    if (Value->DataType == MTP_TYPE_INT128 || Value->DataType == MTP_TYPE_UINT128)
        return FALSE;
    Integer = MtpValueInteger(Value);
    switch (Conv)
    {
        case WpdConvUi4:
            Item->Vt = WPD_VT_UI4;
            Item->Ui4 = (ULONG)Integer;
            return TRUE;
        case WpdConvUi8:
            Item->Vt = WPD_VT_UI8;
            Item->Ui8 = (ULONGLONG)Integer;
            return TRUE;
        case WpdConvBool:
            Item->Vt = WPD_VT_BOOL;
            Item->Bool = Integer ? -1 : 0;
            return TRUE;
        case WpdConvChannels:
            if ((ULONGLONG)Integer >= ARRAYSIZE(ChannelCountBits))
                return FALSE;
            Item->Vt = WPD_VT_R4;
            Item->Ui4 = ChannelCountBits[Integer];
            return TRUE;
        case WpdConvRaw:
            switch (Value->DataType)
            {
                case MTP_TYPE_INT8:
                    Item->Vt = WPD_VT_I1;
                    Item->Ui1 = (UCHAR)Integer;
                    return TRUE;
                case MTP_TYPE_UINT8:
                    Item->Vt = WPD_VT_UI1;
                    Item->Ui1 = (UCHAR)Integer;
                    return TRUE;
                case MTP_TYPE_INT16:
                    Item->Vt = WPD_VT_I2;
                    Item->Ui2 = (USHORT)Integer;
                    return TRUE;
                case MTP_TYPE_UINT16:
                    Item->Vt = WPD_VT_UI2;
                    Item->Ui2 = (USHORT)Integer;
                    return TRUE;
                case MTP_TYPE_INT32:
                    Item->Vt = WPD_VT_I4;
                    Item->I4 = (LONG)Integer;
                    return TRUE;
                case MTP_TYPE_UINT32:
                    Item->Vt = WPD_VT_UI4;
                    Item->Ui4 = (ULONG)Integer;
                    return TRUE;
                case MTP_TYPE_INT64:
                    Item->Vt = WPD_VT_I8;
                    Item->I8 = Integer;
                    return TRUE;
                case MTP_TYPE_UINT64:
                    Item->Vt = WPD_VT_UI8;
                    Item->Ui8 = (ULONGLONG)Integer;
                    return TRUE;
                default:
                    return FALSE;
            }
        default:
            return FALSE;
    }
}

static
NTSTATUS
WpdSetConverted(
    _Inout_ PWPD_COLLECTION Values,
    _In_ REFPROPERTYKEY Key,
    _In_ UCHAR Conv,
    _In_opt_ const MTP_VALUE *Value)
{
    WPD_VALUE Item;

    if (!WpdConvert(Conv, Value, &Item))
        return WpdValuesSetError(Values, Key, WPD_E_NOT_FOUND);
    return WpdValuesSet(Values, Key, &Item);
}

static
NTSTATUS
WpdObjectValue(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PMTP_OBJECT Object,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Values)
{
    const WPD_PROP_MAP *Map;
    WCHAR Text[48];
    GUID Guid;
    ULONGLONG Bits;
    USHORT Prop;

    if (WpdKeyEqual(Key, &WPD_OBJECT_ID))
    {
        WpdFormatObjectId(Text, FALSE, Object->Handle);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_PARENT_ID))
    {
        WpdParentId(Object, Text);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_FORMAT))
    {
        WpdMtpFormatGuid(Object->Format, &Guid);
        return WpdValuesSetGuid(Values, Key, &Guid);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTENT_TYPE))
        return WpdValuesSetGuid(Values, Key, WpdMtpContentType(Object->Format));
    if (WpdKeyEqual(Key, &WPD_OBJECT_NAME))
        return WpdValuesSetString(Values, Key, Object->Name ? Object->Name : Object->FileName);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID))
    {
        WpdFormatObjectId(Text, TRUE, Object->StorageId);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_PERSISTENT_UNIQUE_ID) && Object->PuidValid)
    {
        WpdFormatPuid(Object->Puid, Text);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_SIZE))
        return WpdValuesSetUi8(Values, Key, Object->Size);
    if (WpdKeyEqual(Key, &WPD_OBJECT_ORIGINAL_FILE_NAME))
        return WpdValuesSetString(Values, Key, Object->FileName);
    if (WpdKeyEqual(Key, &WPD_OBJECT_DATE_CREATED) && WpdParseMtpDate(Object->DateCreated, &Bits))
        return WpdSetDate(Values, Key, Bits);
    if (WpdKeyEqual(Key, &WPD_OBJECT_DATE_MODIFIED) && WpdParseMtpDate(Object->DateModified, &Bits))
        return WpdSetDate(Values, Key, Bits);
    if (WpdKeyEqual(Key, &WPD_OBJECT_ISHIDDEN))
        return WpdValuesSetBool(Values, Key, Object->Hidden);
    if (WpdKeyEqual(Key, &WPD_OBJECT_ISSYSTEM))
        return WpdValuesSetBool(Values, Key, Object->System);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CAN_DELETE))
        return WpdValuesSetBool(Values, Key, Object->ProtectionStatus == 0);
    if (WpdKeyEqual(Key, &WPD_OBJECT_NON_CONSUMABLE))
        return WpdValuesSetBool(Values, Key, Object->NonConsumable);
    Map = WpdFindPropMap(Key);
    if (Map && Map->Conv != WpdConvCore)
        return WpdSetConverted(Values, Key, Map->Conv, WpdMtpObjectProp(Object, Map->Prop));
    if (WpdVendorProp(FALSE, Key, &Prop))
        return WpdSetConverted(Values, Key, WpdConvRaw, WpdMtpObjectProp(Object, Prop));
    return WpdValuesSetError(Values, Key, WPD_E_NOT_FOUND);
}

static
PCWSTR
WpdFilesystemName(
    _In_ USHORT Type)
{
    switch (Type)
    {
        case 0x0001:
            return L"Generic flat";
        case 0x0002:
            return L"Generic hierarchical";
        case 0x0003:
            return L"DCF";
        default:
            return L"Undefined";
    }
}

static
NTSTATUS
WpdDeviceValue(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Values)
{
    PMTP_DEVICE_PROP Prop;
    WCHAR Text[32];
    ULONG Index;
    USHORT Code;
    BOOLEAN NonConsumable = FALSE;

    if (WpdKeyEqual(Key, &WPD_OBJECT_ID) || WpdKeyEqual(Key, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID) ||
        WpdKeyEqual(Key, &WPD_OBJECT_PERSISTENT_UNIQUE_ID))
    {
        return WpdValuesSetString(Values, Key, WPD_DEVICE_OBJECT_ID);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_PARENT_ID))
        return WpdValuesSetString(Values, Key, L"");
    if (WpdKeyEqual(Key, &WPD_OBJECT_FORMAT))
        return WpdValuesSetGuid(Values, Key, &WPD_OBJECT_FORMAT_PROPERTIES_ONLY);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTENT_TYPE))
        return WpdValuesSetGuid(Values, Key, &WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
    if (WpdKeyEqual(Key, &WPD_FUNCTIONAL_OBJECT_CATEGORY))
        return WpdValuesSetGuid(Values, Key, &WPD_FUNCTIONAL_CATEGORY_DEVICE);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CAN_DELETE))
        return WpdValuesSetBool(Values, Key, FALSE);
    if (WpdKeyEqual(Key, &WPD_DEVICE_FIRMWARE_VERSION))
        return WpdValuesSetString(Values, Key, Dev->DeviceVersion);
    if (WpdKeyEqual(Key, &WPD_DEVICE_PROTOCOL))
    {
        RtlStringCchPrintfW(Text, ARRAYSIZE(Text), L"MTP: %u.%02u",
                            Dev->VendorExtensionVersion / 100, Dev->VendorExtensionVersion % 100);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_DEVICE_MANUFACTURER))
        return WpdValuesSetString(Values, Key, Dev->Manufacturer);
    if (WpdKeyEqual(Key, &WPD_DEVICE_MODEL))
        return WpdValuesSetString(Values, Key, Dev->Model);
    if (WpdKeyEqual(Key, &WPD_OBJECT_NAME))
        return WpdValuesSetString(Values, Key, Dev->Model);
    if (WpdKeyEqual(Key, &WPD_DEVICE_FRIENDLY_NAME))
        return WpdValuesSetString(Values, Key, Dev->FriendlyName ? Dev->FriendlyName : Dev->Model);
    if (WpdKeyEqual(Key, &WPD_DEVICE_SUPPORTS_NON_CONSUMABLE))
    {
        for (Index = 0; Index < Dev->FormatCount && !NonConsumable; Index++)
            NonConsumable = WpdMtpFormatHasProp(Dev, Dev->Formats[Index].Code, MTP_PROP_NON_CONSUMABLE);
        return WpdValuesSetBool(Values, Key, NonConsumable);
    }
    if (WpdKeyEqual(Key, &WPD_DEVICE_SERIAL_NUMBER))
        return WpdValuesSetString(Values, Key, Dev->SerialNumber);
    if (WpdKeyEqual(Key, &WPD_DEVICE_TYPE))
        return WpdValuesSetUi4(Values, Key, Dev->PerceivedType);
    if (WpdKeyEqual(Key, &WPD_DEVICE_TRANSPORT))
        return WpdValuesSetUi4(Values, Key, WPD_DEVICE_TRANSPORT_USB);
    if (WpdKeyEqual(Key, &WPD_DEVICE_POWER_LEVEL) && Dev->PowerLevelValid)
    {
        Prop = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_BATTERY_LEVEL, TRUE);
        if (Prop && Prop->DataType != MTP_TYPE_STRING)
            Dev->PowerLevel = (ULONG)MtpValueInteger(&Prop->Current);
        return WpdValuesSetUi4(Values, Key, Dev->PowerLevel);
    }
    if (WpdKeyEqual(Key, &WPD_DEVICE_POWER_SOURCE))
        return WpdValuesSetUi4(Values, Key, WPD_POWER_SOURCE_BATTERY);
    if (WpdVendorProp(TRUE, Key, &Code))
    {
        Prop = WpdMtpGetDeviceProp(Dev, Code, FALSE);
        return WpdSetConverted(Values, Key, WpdConvRaw, Prop ? &Prop->Current : NULL);
    }
    return WpdValuesSetError(Values, Key, WPD_E_NOT_FOUND);
}

static
NTSTATUS
WpdStorageValue(
    _In_ PMTP_STORAGE Storage,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Values)
{
    WCHAR Text[96];

    if (WpdKeyEqual(Key, &WPD_OBJECT_ID) || WpdKeyEqual(Key, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID))
    {
        WpdFormatObjectId(Text, TRUE, Storage->Id);
        return WpdValuesSetString(Values, Key, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_PARENT_ID))
        return WpdValuesSetString(Values, Key, WPD_DEVICE_OBJECT_ID);
    if (WpdKeyEqual(Key, &WPD_OBJECT_FORMAT))
        return WpdValuesSetGuid(Values, Key, &WPD_OBJECT_FORMAT_PROPERTIES_ONLY);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CAN_DELETE))
        return WpdValuesSetBool(Values, Key, FALSE);
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTENT_TYPE))
        return WpdValuesSetGuid(Values, Key, &WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
    if (WpdKeyEqual(Key, &WPD_FUNCTIONAL_OBJECT_CATEGORY))
        return WpdValuesSetGuid(Values, Key, &WPD_FUNCTIONAL_CATEGORY_STORAGE);
    if (WpdKeyEqual(Key, &WPD_STORAGE_TYPE))
        return WpdValuesSetUi4(Values, Key, Storage->StorageType);
    if (WpdKeyEqual(Key, &WPD_STORAGE_FILE_SYSTEM_TYPE))
        return WpdValuesSetString(Values, Key, WpdFilesystemName(Storage->FilesystemType));
    if (WpdKeyEqual(Key, &WPD_STORAGE_ACCESS_CAPABILITY))
        return WpdValuesSetUi4(Values, Key, Storage->AccessCapability);
    if (WpdKeyEqual(Key, &WPD_STORAGE_CAPACITY))
        return WpdValuesSetUi8(Values, Key, Storage->MaxCapacity);
    if (WpdKeyEqual(Key, &WPD_STORAGE_FREE_SPACE_IN_BYTES))
        return WpdValuesSetUi8(Values, Key, Storage->FreeSpaceInBytes);
    if (WpdKeyEqual(Key, &WPD_STORAGE_FREE_SPACE_IN_OBJECTS))
        return WpdValuesSetUi8(Values, Key, Storage->FreeSpaceInObjects);
    if (WpdKeyEqual(Key, &WPD_STORAGE_DESCRIPTION) || WpdKeyEqual(Key, &WPD_OBJECT_NAME))
        return WpdValuesSetString(Values, Key, Storage->Description);
    if (WpdKeyEqual(Key, &WPD_STORAGE_SERIAL_NUMBER))
        return WpdValuesSetString(Values, Key, Storage->VolumeIdentifier);
    if (WpdKeyEqual(Key, &WPD_OBJECT_PERSISTENT_UNIQUE_ID))
    {
        WpdStoragePuid(Storage, Text, ARRAYSIZE(Text));
        return WpdValuesSetString(Values, Key, Text);
    }
    return WpdValuesSetError(Values, Key, WPD_E_NOT_FOUND);
}

HRESULT
WpdResolveObject(
    _In_ PWPDMTP_DEVICE Dev,
    _In_opt_ PCWSTR Id,
    _Out_ PWPD_OBJECT_REF Ref)
{
    HRESULT hr;

    if (!WpdParseObjectId(Id, Ref))
        return WPD_E_INVALIDARG;
    switch (Ref->Kind)
    {
        case WpdRefStorage:
            Ref->Storage = WpdMtpFindStorage(Dev, Ref->Value);
            return Ref->Storage ? WPD_S_OK : WPD_E_MTP(0x2008);
        case WpdRefObject:
            hr = WpdMtpGetObject(Dev, Ref->Value, TRUE, &Ref->Object);
            return hr;
        default:
            return WPD_S_OK;
    }
}

static
NTSTATUS
WpdSupportedKeys(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_OBJECT_REF Ref,
    _Inout_ PWPD_COLLECTION Keys)
{
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (Ref->Kind)
    {
        case WpdRefDevice:
            for (Index = 0; Index < ARRAYSIZE(DeviceKeys) && NT_SUCCESS(Status); Index++)
            {
                if (DeviceKeys[Index] == &WPD_DEVICE_POWER_LEVEL && !Dev->PowerLevelValid)
                    continue;
                Status = WpdKeysAdd(Keys, DeviceKeys[Index]);
            }
            for (Index = 0; Index < Dev->DevicePropCount && NT_SUCCESS(Status); Index++)
            {
                PROPERTYKEY Key;

                if (Dev->DeviceProps[Index] == MTP_DEVICE_PROP_FRIENDLY_NAME ||
                    Dev->DeviceProps[Index] == MTP_DEVICE_PROP_BATTERY_LEVEL ||
                    !WpdMtpGetDeviceProp(Dev, Dev->DeviceProps[Index], FALSE))
                {
                    continue;
                }
                WpdVendorKey(TRUE, Dev->DeviceProps[Index], &Key);
                Status = WpdKeysAdd(Keys, &Key);
            }
            return Status;
        case WpdRefStorage:
            for (Index = 0; Index < ARRAYSIZE(StorageKeys) && NT_SUCCESS(Status); Index++)
                Status = WpdKeysAdd(Keys, StorageKeys[Index]);
            return Status;
        default:
            return WpdObjectKeys(Dev, Ref->Object->Format, Ref->Object, Keys);
    }
}

static
NTSTATUS
WpdRefValue(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_OBJECT_REF Ref,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Values)
{
    switch (Ref->Kind)
    {
        case WpdRefDevice:
            return WpdDeviceValue(Dev, Key, Values);
        case WpdRefStorage:
            return WpdStorageValue(Ref->Storage, Key, Values);
        default:
            return WpdObjectValue(Dev, Ref->Object, Key, Values);
    }
}

static
HRESULT
WpdPropertiesGetSupported(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    WPD_OBJECT_REF Ref;
    PWPD_COLLECTION Keys;
    HRESULT hr;
    NTSTATUS Status;

    hr = WpdResolveObject(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID), &Ref);
    if (FAILED(hr))
        return hr;
    Keys = WpdCollectionCreate(WpdKeys);
    if (!Keys)
        return WPD_E_OUTOFMEMORY;
    Status = WpdSupportedKeys(Dev, &Ref, Keys);
    if (NT_SUCCESS(Status))
        return WpdNtToHresult(WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, Keys));
    WpdCollectionFree(Keys);
    return WpdNtToHresult(Status);
}

static
HRESULT
WpdPropertiesGet(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params,
    _In_ BOOLEAN All,
    _Inout_ PWPD_COLLECTION Results)
{
    WPD_OBJECT_REF Ref;
    PWPD_COLLECTION Keys, Requested = NULL, Values;
    HRESULT hr;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;
    BOOLEAN Partial = FALSE;

    if (!All)
    {
        Requested = WpdValuesGetCollection(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, WpdKeys);
        if (!Requested)
            return WPD_E_INVALIDARG;
    }
    hr = WpdResolveObject(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID), &Ref);
    if (FAILED(hr))
        return hr;
    Keys = WpdCollectionCreate(WpdKeys);
    Values = WpdCollectionCreate(WpdValues);
    if (!Keys || !Values)
    {
        WpdCollectionFree(Keys);
        WpdCollectionFree(Values);
        return WPD_E_OUTOFMEMORY;
    }
    Status = WpdSupportedKeys(Dev, &Ref, Keys);
    if (All && NT_SUCCESS(Status))
    {
        if (Ref.Kind == WpdRefStorage)
        {
            for (Index = 0; Index < ARRAYSIZE(StorageValueOrder) && NT_SUCCESS(Status); Index++)
                Status = WpdRefValue(Dev, &Ref, StorageValueOrder[Index], Values);
        }
        else
        {
            for (Index = 0; Index < Keys->Count && NT_SUCCESS(Status); Index++)
                Status = WpdRefValue(Dev, &Ref, &Keys->Keys[Index], Values);
        }
    }
    else if (NT_SUCCESS(Status))
    {
        if (Ref.Kind == WpdRefObject && !WpdKeysContain(Requested, &WPD_OBJECT_ID))
            Status = WpdRefValue(Dev, &Ref, &WPD_OBJECT_ID, Values);
        for (Index = 0; Index < Requested->Count && NT_SUCCESS(Status); Index++)
        {
            if (WpdKeysContain(Keys, &Requested->Keys[Index]))
            {
                Status = WpdRefValue(Dev, &Ref, &Requested->Keys[Index], Values);
            }
            else
            {
                Status = WpdValuesSetError(Values, &Requested->Keys[Index],
                                           Ref.Kind == WpdRefStorage ? WPD_E_NOT_SUPPORTED : WPD_E_NOT_FOUND);
                Partial = TRUE;
            }
        }
    }
    WpdCollectionFree(Keys);
    for (Index = 0; Index < Values->Count; Index++)
    {
        if (Values->Entries[Index].Value.Vt == WPD_VT_ERROR)
            Partial = TRUE;
    }
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_VALUES, Values);
    else
        WpdCollectionFree(Values);
    if (!NT_SUCCESS(Status))
        return WpdNtToHresult(Status);
    return Partial ? WPD_S_FALSE : WPD_S_OK;
}

static
NTSTATUS
WpdCoreDefault(
    _In_ PMTP_PROP_DESC Desc,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Attributes)
{
    const MTP_VALUE *Default = &Desc->Default;
    WCHAR Text[48];
    GUID Guid;
    ULONGLONG Bits;

    if (!Default->Valid)
        return STATUS_SUCCESS;
    if (WpdKeyEqual(Key, &WPD_OBJECT_FORMAT))
    {
        WpdMtpFormatGuid((USHORT)MtpValueInteger(Default), &Guid);
        return WpdValuesSetGuid(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, &Guid);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTENT_TYPE))
        return WpdValuesSetGuid(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, WpdMtpContentType((USHORT)MtpValueInteger(Default)));
    if (WpdKeyEqual(Key, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID))
    {
        WpdFormatObjectId(Text, TRUE, (ULONG)MtpValueInteger(Default));
        return WpdValuesSetString(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_PERSISTENT_UNIQUE_ID))
    {
        WpdFormatPuid(Default->Raw, Text);
        return WpdValuesSetString(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, Text);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_SIZE))
        return WpdValuesSetUi8(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, (ULONGLONG)MtpValueInteger(Default));
    if (WpdKeyEqual(Key, &WPD_OBJECT_ISHIDDEN) || WpdKeyEqual(Key, &WPD_OBJECT_ISSYSTEM) ||
        WpdKeyEqual(Key, &WPD_OBJECT_NON_CONSUMABLE))
    {
        return WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, MtpValueInteger(Default) != 0);
    }
    if (WpdKeyEqual(Key, &WPD_OBJECT_CAN_DELETE))
        return WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, MtpValueInteger(Default) == 0);
    if (WpdKeyEqual(Key, &WPD_OBJECT_DATE_CREATED) || WpdKeyEqual(Key, &WPD_OBJECT_DATE_MODIFIED))
    {
        if (Default->DataType == MTP_TYPE_STRING && WpdParseMtpDate(Default->String, &Bits))
            return WpdSetDate(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, Bits);
        return WpdValuesSetError(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, WPD_E_NOT_FOUND);
    }
    if (Default->DataType == MTP_TYPE_STRING && Default->String)
        return WpdValuesSetString(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, Default->String);
    return STATUS_SUCCESS;
}

static
NTSTATUS
WpdFormAttributes(
    _In_ UCHAR Conv,
    _In_ BOOLEAN CanDelete,
    _In_ const MTP_FORM *Form,
    _Inout_ PWPD_COLLECTION Attributes)
{
    static const PROPERTYKEY *const RangeKeys[] =
    {
        &WPD_PROPERTY_ATTRIBUTE_RANGE_MIN, &WPD_PROPERTY_ATTRIBUTE_RANGE_MAX, &WPD_PROPERTY_ATTRIBUTE_RANGE_STEP,
    };
    PWPD_COLLECTION Elements;
    WPD_VALUE Item;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Form->Flag == MTP_FORM_RANGE && !CanDelete)
    {
        for (Index = 0; Index < ARRAYSIZE(RangeKeys) && NT_SUCCESS(Status); Index++)
        {
            if (WpdConvert(Conv, &Form->Range[Index], &Item))
                Status = WpdValuesSet(Attributes, RangeKeys[Index], &Item);
        }
        return Status;
    }
    if (Form->Flag != MTP_FORM_ENUMERATION)
        return STATUS_SUCCESS;
    Elements = WpdCollectionCreate(WpdVariants);
    if (!Elements)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (Index = 0; Index < Form->EnumCount && NT_SUCCESS(Status); Index++)
    {
        if (CanDelete)
        {
            RtlZeroMemory(&Item, sizeof(Item));
            Item.Vt = WPD_VT_BOOL;
            Item.Bool = MtpValueInteger(&Form->Enum[Index]) == 0 ? -1 : 0;
            Status = WpdVariantsAdd(Elements, &Item);
        }
        else if (WpdConvert(Conv, &Form->Enum[Index], &Item))
        {
            Status = WpdVariantsAdd(Elements, &Item);
        }
    }
    if (NT_SUCCESS(Status))
        return WpdValuesSetCollection(Attributes, &WPD_PROPERTY_ATTRIBUTE_ENUMERATION_ELEMENTS, Elements);
    WpdCollectionFree(Elements);
    return Status;
}

static
ULONG
WpdFormFlag(
    _In_ const MTP_FORM *Form)
{
    if (Form->Flag == MTP_FORM_RANGE)
        return WPD_PROPERTY_ATTRIBUTE_FORM_RANGE;
    if (Form->Flag == MTP_FORM_ENUMERATION)
        return WPD_PROPERTY_ATTRIBUTE_FORM_ENUMERATION;
    return WPD_PROPERTY_ATTRIBUTE_FORM_UNSPECIFIED;
}

NTSTATUS
WpdObjectAttributes(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USHORT Format,
    _In_ REFPROPERTYKEY Key,
    _In_ BOOLEAN WithDefault,
    _Inout_ PWPD_COLLECTION Attributes)
{
    const WPD_PROP_MAP *Map = WpdFindPropMap(Key);
    PMTP_PROP_DESC Desc = NULL;
    UCHAR Conv = WpdConvRaw;
    USHORT Prop = 0;
    BOOLEAN Fast = TRUE, CanDelete = WpdKeyEqual(Key, &WPD_OBJECT_CAN_DELETE), UseForm;
    WPD_VALUE Item;
    NTSTATUS Status;

    if (Map)
    {
        Prop = Map->Prop;
        Conv = Map->Conv;
        Fast = Map->Fast;
    }
    else if (WpdVendorProp(FALSE, Key, &Prop))
    {
        Fast = FALSE;
    }
    if (Prop && !Fast)
        Desc = WpdMtpGetPropDesc(Dev, Format, Prop);
    UseForm = Desc && (Conv != WpdConvCore || CanDelete);
    Status = WpdValuesSetUi4(Attributes, &WPD_PROPERTY_ATTRIBUTE_FORM,
                             UseForm ? WpdFormFlag(&Desc->Form) : WPD_PROPERTY_ATTRIBUTE_FORM_UNSPECIFIED);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_READ, TRUE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_WRITE, Desc && Desc->GetSet != 0 && !(Map && Map->Derived));
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_DELETE, FALSE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_FAST_PROPERTY, Fast);
    if (NT_SUCCESS(Status) && WithDefault && Desc)
    {
        if (Conv == WpdConvCore)
        {
            Status = WpdCoreDefault(Desc, Key, Attributes);
        }
        else if (WpdConvert(Conv, &Desc->Default, &Item))
        {
            Status = WpdValuesSet(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, &Item);
        }
        else if (Conv == WpdConvDate)
        {
            Status = WpdValuesSetError(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, WPD_E_NOT_FOUND);
        }
    }
    if (NT_SUCCESS(Status) && Desc && Desc->DataType == MTP_TYPE_STRING)
        Status = WpdValuesSetUi4(Attributes, &WPD_PROPERTY_ATTRIBUTE_MAX_SIZE, WPDMTP_MAX_STRING_BYTES);
    if (NT_SUCCESS(Status) && Desc && (Desc->DataType == MTP_TYPE_UINT128 || Desc->DataType == MTP_TYPE_INT128))
        Status = WpdValuesSetUi4(Attributes, &WPD_PROPERTY_ATTRIBUTE_MAX_SIZE, 16);
    if (NT_SUCCESS(Status) && UseForm)
        Status = WpdFormAttributes(Conv, CanDelete, &Desc->Form, Attributes);
    return Status;
}

static
NTSTATUS
WpdFixedAttributes(
    _Inout_ PWPD_COLLECTION Attributes)
{
    NTSTATUS Status;

    Status = WpdValuesSetUi4(Attributes, &WPD_PROPERTY_ATTRIBUTE_FORM, WPD_PROPERTY_ATTRIBUTE_FORM_UNSPECIFIED);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_READ, TRUE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_WRITE, FALSE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_DELETE, FALSE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_FAST_PROPERTY, TRUE);
    return Status;
}

static
PMTP_DEVICE_PROP
WpdDeviceKeyProp(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ REFPROPERTYKEY Key,
    _Out_ PUCHAR Conv)
{
    USHORT Code = 0;

    *Conv = WpdConvRaw;
    if (WpdKeyEqual(Key, &WPD_DEVICE_FRIENDLY_NAME))
    {
        Code = MTP_DEVICE_PROP_FRIENDLY_NAME;
        *Conv = WpdConvString;
    }
    else if (WpdKeyEqual(Key, &WPD_DEVICE_POWER_LEVEL))
    {
        Code = MTP_DEVICE_PROP_BATTERY_LEVEL;
        *Conv = WpdConvUi4;
    }
    else if (!WpdVendorProp(TRUE, Key, &Code))
    {
        return NULL;
    }
    return WpdMtpGetDeviceProp(Dev, Code, FALSE);
}

static
NTSTATUS
WpdDeviceAttributes(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ REFPROPERTYKEY Key,
    _Inout_ PWPD_COLLECTION Attributes)
{
    PMTP_DEVICE_PROP Prop;
    WPD_VALUE Item;
    UCHAR Conv;
    NTSTATUS Status;

    Prop = WpdDeviceKeyProp(Dev, Key, &Conv);
    if (!Prop)
        return WpdFixedAttributes(Attributes);
    Status = WpdValuesSetUi4(Attributes, &WPD_PROPERTY_ATTRIBUTE_FORM, WpdFormFlag(&Prop->Form));
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_READ, TRUE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_WRITE, Prop->GetSet != 0);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_CAN_DELETE, FALSE);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetBool(Attributes, &WPD_PROPERTY_ATTRIBUTE_FAST_PROPERTY, TRUE);
    if (NT_SUCCESS(Status) && WpdConvert(Conv, &Prop->Default, &Item))
        Status = WpdValuesSet(Attributes, &WPD_PROPERTY_ATTRIBUTE_DEFAULT_VALUE, &Item);
    if (NT_SUCCESS(Status))
        Status = WpdFormAttributes(Conv, FALSE, &Prop->Form, Attributes);
    return Status;
}

static
HRESULT
WpdPropertiesGetAttributes(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    WPD_OBJECT_REF Ref;
    PWPD_COLLECTION Keys, Attributes;
    PROPERTYKEY Key;
    HRESULT hr;
    NTSTATUS Status;

    if (!WpdValuesGetKey(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_KEYS, &Key))
        return WPD_E_INVALIDARG;
    hr = WpdResolveObject(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID), &Ref);
    if (FAILED(hr))
        return hr;
    Keys = WpdCollectionCreate(WpdKeys);
    if (!Keys)
        return WPD_E_OUTOFMEMORY;
    Status = WpdSupportedKeys(Dev, &Ref, Keys);
    if (NT_SUCCESS(Status) && !WpdKeysContain(Keys, &Key))
    {
        WpdCollectionFree(Keys);
        return WPD_E_UNKNOWN_PROPERTY;
    }
    WpdCollectionFree(Keys);
    if (!NT_SUCCESS(Status))
        return WpdNtToHresult(Status);
    Attributes = WpdCollectionCreate(WpdValues);
    if (!Attributes)
        return WPD_E_OUTOFMEMORY;
    if (Ref.Kind == WpdRefObject)
        Status = WpdObjectAttributes(Dev, Ref.Object->Format, &Key, TRUE, Attributes);
    else if (Ref.Kind == WpdRefDevice)
        Status = WpdDeviceAttributes(Dev, &Key, Attributes);
    else
        Status = WpdFixedAttributes(Attributes);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_ATTRIBUTES, Attributes);
    else
        WpdCollectionFree(Attributes);
    return WpdNtToHresult(Status);
}

static
BOOLEAN
WpdEncodeWrite(
    _In_ USHORT DataType,
    _In_ const WPD_VALUE *Value,
    _Out_ PUCHAR *Data,
    _Out_ PULONG Length)
{
    ULONGLONG Integer;
    ULONG Size;
    PUCHAR Buffer;

    *Data = NULL;
    *Length = 0;
    if (DataType == MTP_TYPE_STRING)
    {
        if (Value->Vt != WPD_VT_LPWSTR || !Value->String.Buffer)
            return FALSE;
        Buffer = ExAllocatePoolWithTag(PagedPool, MtpStringSize(Value->String.Buffer), WPDMTP_TAG);
        if (!Buffer)
            return FALSE;
        *Length = (ULONG)(MtpWriteString(Buffer, Value->String.Buffer) - Buffer);
        *Data = Buffer;
        return TRUE;
    }
    switch (Value->Vt)
    {
        case WPD_VT_UI1:
        case WPD_VT_I1:
            Integer = Value->Ui1;
            break;
        case WPD_VT_UI2:
        case WPD_VT_I2:
            Integer = Value->Ui2;
            break;
        case WPD_VT_BOOL:
            Integer = Value->Bool ? 1 : 0;
            break;
        case WPD_VT_UI4:
        case WPD_VT_I4:
        case WPD_VT_UINT:
        case WPD_VT_INT:
            Integer = Value->Ui4;
            break;
        case WPD_VT_UI8:
        case WPD_VT_I8:
            Integer = Value->Ui8;
            break;
        default:
            return FALSE;
    }
    switch (DataType)
    {
        case MTP_TYPE_INT8:
        case MTP_TYPE_UINT8:
            Size = 1;
            break;
        case MTP_TYPE_INT16:
        case MTP_TYPE_UINT16:
            Size = 2;
            break;
        case MTP_TYPE_INT32:
        case MTP_TYPE_UINT32:
            Size = 4;
            break;
        case MTP_TYPE_INT64:
        case MTP_TYPE_UINT64:
            Size = 8;
            break;
        default:
            return FALSE;
    }
    Buffer = ExAllocatePoolWithTag(PagedPool, Size, WPDMTP_TAG);
    if (!Buffer)
        return FALSE;
    RtlCopyMemory(Buffer, &Integer, Size);
    *Data = Buffer;
    *Length = Size;
    return TRUE;
}

static
HRESULT
WpdWriteTarget(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_OBJECT_REF Ref,
    _In_ PWPD_ENTRY Entry,
    _Out_ PMTP_OPERATION Op)
{
    const WPD_PROP_MAP *Map;
    PMTP_PROP_DESC Desc;
    PMTP_DEVICE_PROP DeviceProp;
    PUCHAR Data;
    ULONG Length;
    USHORT Prop = 0;

    RtlZeroMemory(Op, sizeof(*Op));
    if (WpdKeyEqual(&Entry->Key, &WPD_OBJECT_PERSISTENT_UNIQUE_ID))
        return WPD_E_UNKNOWN_PROPERTY;
    if (Ref->Kind == WpdRefDevice)
    {
        if (!WpdKeyEqual(&Entry->Key, &WPD_DEVICE_FRIENDLY_NAME) ||
            !WpdMtpSupportsOperation(Dev, MTP_OP_SET_DEVICE_PROP_VALUE))
        {
            return WPD_E_ACCESSDENIED;
        }
        DeviceProp = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_FRIENDLY_NAME, FALSE);
        if (!DeviceProp || !DeviceProp->GetSet || !WpdEncodeWrite(DeviceProp->DataType, &Entry->Value, &Data, &Length))
            return WPD_E_ACCESSDENIED;
        Op->Code = MTP_OP_SET_DEVICE_PROP_VALUE;
        Op->ParamCount = 1;
        Op->Params[0] = MTP_DEVICE_PROP_FRIENDLY_NAME;
        Op->DataOut = Data;
        Op->DataOutLength = Length;
        return WPD_S_OK;
    }
    if (Ref->Kind != WpdRefObject || !WpdMtpSupportsOperation(Dev, MTP_OP_SET_OBJECT_PROP_VALUE))
        return WPD_E_ACCESSDENIED;
    if (WpdKeyEqual(&Entry->Key, &WPD_OBJECT_NAME))
    {
        Prop = WpdMtpFormatHasProp(Dev, Ref->Object->Format, MTP_PROP_NAME) ? MTP_PROP_NAME : MTP_PROP_OBJECT_FILE_NAME;
    }
    else if (WpdKeyEqual(&Entry->Key, &WPD_OBJECT_ORIGINAL_FILE_NAME))
    {
        Prop = MTP_PROP_OBJECT_FILE_NAME;
    }
    else
    {
        Map = WpdFindPropMap(&Entry->Key);
        if (Map && Map->Conv != WpdConvCore && Map->Conv != WpdConvDate && !Map->Derived)
            Prop = Map->Prop;
        else if (!Map)
            WpdVendorProp(FALSE, &Entry->Key, &Prop);
    }
    if (!Prop || !WpdMtpFormatHasProp(Dev, Ref->Object->Format, Prop))
        return WPD_E_ACCESSDENIED;
    Desc = WpdMtpGetPropDesc(Dev, Ref->Object->Format, Prop);
    if (Desc && !Desc->GetSet)
        return WPD_E_ACCESSDENIED;
    if (!WpdEncodeWrite(Desc ? Desc->DataType : MTP_TYPE_STRING, &Entry->Value, &Data, &Length))
        return WPD_E_ACCESSDENIED;
    Op->Code = MTP_OP_SET_OBJECT_PROP_VALUE;
    Op->ParamCount = 2;
    Op->Params[0] = Ref->Object->Handle;
    Op->Params[1] = Prop;
    Op->DataOut = Data;
    Op->DataOutLength = Length;
    return WPD_S_OK;
}

static
HRESULT
WpdPropertiesSet(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Values, WriteResults;
    WPD_OBJECT_REF Ref;
    PMTP_OPERATION Ops;
    ULONG Index;
    HRESULT hr, Item;
    BOOLEAN Partial = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    Values = WpdValuesGetCollection(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_VALUES, WpdValues);
    if (!Values)
        return WPD_E_INVALIDARG;
    hr = WpdResolveObject(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_OBJECT_ID), &Ref);
    if (FAILED(hr))
        return hr;
    WriteResults = WpdCollectionCreate(WpdValues);
    Ops = ExAllocatePoolZero(PagedPool, max(Values->Count, 1) * sizeof(MTP_OPERATION), WPDMTP_TAG);
    if (!WriteResults || !Ops)
    {
        WpdCollectionFree(WriteResults);
        if (Ops)
            ExFreePoolWithTag(Ops, WPDMTP_TAG);
        return WPD_E_OUTOFMEMORY;
    }
    for (Index = 0; Index < Values->Count && NT_SUCCESS(Status); Index++)
    {
        Item = WpdWriteTarget(Dev, &Ref, &Values->Entries[Index], &Ops[Index]);
        if (FAILED(Item))
        {
            Partial = TRUE;
            Status = WpdValuesSetError(WriteResults, &Values->Entries[Index].Key, Item);
        }
    }
    hr = WPD_S_OK;
    for (Index = 0; Index < Values->Count && NT_SUCCESS(Status) && SUCCEEDED(hr); Index++)
    {
        if (!Ops[Index].Code)
            continue;
        Status = WpdMtpTransaction(Dev, &Ops[Index]);
        hr = WpdMtpResultToHresult(Status, Ops[Index].ResponseCode);
        Status = STATUS_SUCCESS;
        if (FAILED(hr))
            break;
        if (Ref.Kind == WpdRefObject)
        {
            WpdMtpPostHandleEvent(Dev, WpdObjectEventUpdated, Ref.Object->Handle, NULL);
        }
        else if (Ops[Index].Code == MTP_OP_SET_DEVICE_PROP_VALUE)
        {
            PMTP_DEVICE_PROP Prop = WpdMtpGetDeviceProp(Dev, MTP_DEVICE_PROP_FRIENDLY_NAME, TRUE);

            if (Dev->FriendlyName)
                ExFreePoolWithTag(Dev->FriendlyName, WPDMTP_TAG);
            Dev->FriendlyName = NULL;
            if (Prop && Prop->Current.String && Prop->Current.String[0])
                Dev->FriendlyName = WpdMtpDuplicateString(Prop->Current.String);
        }
        Status = WpdValuesSetError(WriteResults, &Values->Entries[Index].Key, WPD_S_OK);
    }
    for (Index = 0; Index < Values->Count; Index++)
    {
        if (Ops[Index].DataOut)
            ExFreePoolWithTag(Ops[Index].DataOut, WPDMTP_TAG);
    }
    ExFreePoolWithTag(Ops, WPDMTP_TAG);
    if (FAILED(hr))
    {
        WpdCollectionFree(WriteResults);
        return hr;
    }
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_PROPERTY_WRITE_RESULTS, WriteResults);
    else
        WpdCollectionFree(WriteResults);
    if (!NT_SUCCESS(Status))
        return WpdNtToHresult(Status);
    return Partial ? WPD_S_FALSE : WPD_S_OK;
}

static
NTSTATUS
WpdBulkObjectValues(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PCWSTR Id,
    _In_opt_ PWPD_COLLECTION Keys,
    _In_ BOOLEAN References,
    _Out_ PWPD_COLLECTION *Result)
{
    WPD_OBJECT_REF Ref;
    PWPD_COLLECTION Values, Supported;
    ULONG Index;
    HRESULT hr;
    NTSTATUS Status;

    *Result = NULL;
    Values = WpdCollectionCreate(WpdValues);
    Supported = WpdCollectionCreate(WpdKeys);
    if (!Values || !Supported)
    {
        WpdCollectionFree(Values);
        WpdCollectionFree(Supported);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = WpdValuesSetString(Values, &WPD_OBJECT_ID, Id);
    hr = WpdResolveObject(Dev, Id, &Ref);
    if (SUCCEEDED(hr) && NT_SUCCESS(Status))
        Status = WpdSupportedKeys(Dev, &Ref, Supported);
    if (References && NT_SUCCESS(Status))
    {
        Status = WpdValuesSetError(Values, &WPD_OBJECT_REFERENCES, WPD_E_NOT_FOUND);
        if (NT_SUCCESS(Status))
            Status = SUCCEEDED(hr) ? WpdRefValue(Dev, &Ref, &WPD_OBJECT_PARENT_ID, Values) :
                     WpdValuesSetError(Values, &WPD_OBJECT_PARENT_ID, hr);
    }
    else if (Keys)
    {
        for (Index = 0; Index < Keys->Count && NT_SUCCESS(Status); Index++)
        {
            if (FAILED(hr))
                Status = WpdValuesSetError(Values, &Keys->Keys[Index], hr);
            else if (WpdKeysContain(Supported, &Keys->Keys[Index]))
                Status = WpdRefValue(Dev, &Ref, &Keys->Keys[Index], Values);
            else
                Status = WpdValuesSetError(Values, &Keys->Keys[Index], WPD_E_NOT_FOUND);
        }
    }
    else if (SUCCEEDED(hr))
    {
        for (Index = 0; Index < Supported->Count && NT_SUCCESS(Status); Index++)
            Status = WpdRefValue(Dev, &Ref, &Supported->Keys[Index], Values);
    }
    WpdCollectionFree(Supported);
    if (!NT_SUCCESS(Status))
    {
        WpdCollectionFree(Values);
        return Status;
    }
    *Result = Values;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WpdBulkAddHandle(
    _Inout_ PWPDMTP_CONTEXT Context,
    _In_ ULONG Handle)
{
    PULONG Handles;

    if (Context->Count == Context->Total)
    {
        Handles = ExAllocatePoolZero(PagedPool, (Context->Count + 32) * sizeof(ULONG), WPDMTP_TAG);
        if (!Handles)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (Context->Handles)
        {
            RtlCopyMemory(Handles, Context->Handles, Context->Count * sizeof(ULONG));
            ExFreePoolWithTag(Context->Handles, WPDMTP_TAG);
        }
        Context->Handles = Handles;
        Context->Total = Context->Count + 32;
    }
    Context->Handles[Context->Count++] = Handle;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WpdBulkCollect(
    _In_ PWPDMTP_DEVICE Dev,
    _Inout_ PWPDMTP_CONTEXT Context,
    _In_ ULONG StorageId,
    _In_ ULONG Parent,
    _In_ USHORT Format,
    _In_ ULONG Level,
    _In_ ULONG Depth)
{
    PMTP_OBJECT Object;
    PULONG Children;
    ULONG Count, Index;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Depth != MAXULONG && Level > Depth)
        return STATUS_SUCCESS;
    if (FAILED(WpdMtpGetChildren(Dev, StorageId, Parent, &Children, &Count)))
        return STATUS_SUCCESS;
    for (Index = 0; Index < Count && NT_SUCCESS(Status); Index++)
    {
        if (FAILED(WpdMtpGetObject(Dev, Children[Index], FALSE, &Object)))
            continue;
        if (!Format || Object->Format == Format)
            Status = WpdBulkAddHandle(Context, Object->Handle);
        if (NT_SUCCESS(Status) && Object->Format == MTP_FORMAT_ASSOCIATION && (Depth == MAXULONG || Level < Depth))
            Status = WpdBulkCollect(Dev, Context, StorageId, Children[Index], Format, Level + 1, Depth);
    }
    ExFreePoolWithTag(Children, WPDMTP_TAG);
    return Status;
}

static
HRESULT
WpdBulk(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPDMTP_CONTEXT Context;
    PWPD_COLLECTION Keys, Ids, Batch, Values;
    WPD_VALUE Copy;
    WPD_OBJECT_REF Ref;
    PLIST_ENTRY Entry;
    WCHAR Text[16];
    PCWSTR Parent;
    GUID Guid;
    USHORT Format = 0;
    ULONG Depth = 0, Index, KeyCount;
    PWPD_VALUE Found;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROP_LIST) || Id < 2 || Id > 7)
        return WPD_E_NOT_SUPPORTED;
    if (Id == 2 || Id == 5)
    {
        Context = WpdCreateContext(Dev, File, Id == 2 ? WpdContextBulkList : WpdContextBulkFormat,
                                   Id == 2 ? L"CBulkGetValuesByObjectListContext" : L"CBulkGetValuesByObjectFormatContext");
        if (!Context)
            return WPD_E_OUTOFMEMORY;
        Context->Properties = WpdCollectionCreate(WpdValues);
        if (!Context->Properties)
        {
            WpdFreeContext(Dev, Context);
            return WPD_E_OUTOFMEMORY;
        }
        Found = WpdValuesFind(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PROPERTY_KEYS);
        if (Found && Found->Vt == WPD_VT_UNKNOWN && Found->Collection && Found->Collection->Kind == WpdKeys)
        {
            Status = WpdValueCopy(&Copy, Found);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSet(Context->Properties, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PROPERTY_KEYS, &Copy);
            else
                WpdValueClear(&Copy);
        }
        if (Id == 2 && NT_SUCCESS(Status))
        {
            Found = WpdValuesFind(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_IDS);
            if (!Found || Found->Vt != WPD_VT_UNKNOWN || !Found->Collection || Found->Collection->Kind != WpdVariants)
            {
                WpdFreeContext(Dev, Context);
                return WPD_E_INVALIDARG;
            }
            Status = WpdValueCopy(&Copy, Found);
            if (NT_SUCCESS(Status))
                Status = WpdValuesSet(Context->Properties, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_IDS, &Copy);
            else
                WpdValueClear(&Copy);
        }
        else if (NT_SUCCESS(Status))
        {
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_FORMAT, &Guid))
            {
                WpdFreeContext(Dev, Context);
                return WPD_E_INVALIDARG;
            }
            if (!RtlEqualMemory(&Guid, &WPD_OBJECT_FORMAT_ALL, sizeof(GUID)) && !WpdMtpFormatFromGuid(&Guid, &Format))
            {
                WpdFreeContext(Dev, Context);
                return WPD_E_INVALIDARG;
            }
            WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_DEPTH, &Depth);
            Parent = WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PARENT_OBJECT_ID);
            if (Parent && WpdParseObjectId(Parent[0] ? Parent : WPD_DEVICE_OBJECT_ID, &Ref) &&
                SUCCEEDED(WpdResolveObject(Dev, Parent[0] ? Parent : WPD_DEVICE_OBJECT_ID, &Ref)))
            {
                switch (Ref.Kind)
                {
                    case WpdRefDevice:
                        if (Depth == MAXULONG || Depth >= 2)
                        {
                            WpdMtpFindStorage(Dev, 0);
                            for (Entry = Dev->Storages.Flink; Entry != &Dev->Storages && NT_SUCCESS(Status); Entry = Entry->Flink)
                            {
                                Status = WpdBulkCollect(Dev, Context, CONTAINING_RECORD(Entry, MTP_STORAGE, Link)->Id,
                                                        MTP_ROOT_PARENT, Format, 2, Depth);
                            }
                        }
                        break;
                    case WpdRefStorage:
                        Status = WpdBulkCollect(Dev, Context, Ref.Storage->Id, MTP_ROOT_PARENT, Format, 1, Depth);
                        break;
                    default:
                        if (!Format || Ref.Object->Format == Format)
                            Status = WpdBulkAddHandle(Context, Ref.Object->Handle);
                        if (NT_SUCCESS(Status) && Ref.Object->Format == MTP_FORMAT_ASSOCIATION && Depth)
                            Status = WpdBulkCollect(Dev, Context, Ref.Object->StorageId, Ref.Object->Handle, Format, 1, Depth);
                        break;
                }
            }
        }
        if (!NT_SUCCESS(Status))
        {
            WpdFreeContext(Dev, Context);
            return WpdNtToHresult(Status);
        }
        return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT, Context->Name));
    }

    Context = WpdFindContext(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT),
                             Id <= 4 ? WpdContextBulkList : WpdContextBulkFormat);
    if (!Context)
        return WPD_E_NOT_FOUND;
    if (Id == 4 || Id == 7)
    {
        WpdFreeContext(Dev, Context);
        return WPD_S_OK;
    }

    Batch = WpdCollectionCreate(WpdValuesList);
    if (!Batch)
        return WPD_E_OUTOFMEMORY;
    Keys = WpdValuesGetCollection(Context->Properties, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_PROPERTY_KEYS, WpdKeys);
    if (Id == 3)
    {
        Ids = WpdValuesGetCollection(Context->Properties, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_OBJECT_IDS, WpdVariants);
        for (Index = 0; Ids && !Context->Next && Index < Ids->Count && NT_SUCCESS(Status); Index++)
        {
            if (Ids->Items[Index].Vt != WPD_VT_LPWSTR)
                continue;
            Status = WpdBulkObjectValues(Dev, Ids->Items[Index].String.Buffer, Keys, FALSE, &Values);
            if (NT_SUCCESS(Status))
                Status = WpdListsAdd(Batch, Values);
        }
        Context->Next = 1;
    }
    else
    {
        KeyCount = Keys ? Keys->Count : 1;
        if (Context->Count && Context->Next <= KeyCount)
        {
            PWPD_COLLECTION Single = NULL;

            if (Keys && Context->Next < KeyCount)
            {
                Single = WpdCollectionCreate(WpdKeys);
                if (!Single)
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                else
                    Status = WpdKeysAdd(Single, &Keys->Keys[KeyCount - 1 - Context->Next]);
            }
            for (Index = 0; Index < Context->Count && NT_SUCCESS(Status) && (Single || !Keys); Index++)
            {
                WpdFormatObjectId(Text, FALSE, Context->Handles[Index]);
                Status = WpdBulkObjectValues(Dev, Text, Single, !Keys && Context->Next == KeyCount, &Values);
                if (NT_SUCCESS(Status))
                    Status = WpdListsAdd(Batch, Values);
            }
            WpdCollectionFree(Single);
            Context->Next++;
            if (Keys && Context->Next == KeyCount)
                Context->Next = KeyCount + 1;
        }
    }
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_VALUES, Batch);
    else
        WpdCollectionFree(Batch);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_PROPERTIES_BULK_CONTEXT, Context->Name);
    return WpdNtToHresult(Status);
}

static
NTSTATUS
WpdAddFormatsForType(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ const GUID *ContentType,
    _Inout_ PWPD_COLLECTION Variants)
{
    ULONG Index;
    GUID Guid;
    NTSTATUS Status = STATUS_SUCCESS;

    for (Index = 0; Index < Dev->FormatCount && NT_SUCCESS(Status); Index++)
    {
        if (!RtlEqualMemory(WpdMtpContentType(Dev->Formats[Index].Code), ContentType, sizeof(GUID)) &&
            !RtlEqualMemory(ContentType, &WPD_CONTENT_TYPE_ALL, sizeof(GUID)))
        {
            continue;
        }
        WpdMtpFormatGuid(Dev->Formats[Index].Code, &Guid);
        Status = WpdVariantsAddGuid(Variants, &Guid);
    }
    return Status;
}

static
HRESULT
WpdCapabilities(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Collection = NULL;
    const PROPERTYKEY *Key = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    PROPERTYKEY Command;
    GUID Guid;
    USHORT Format;
    ULONG Index, Inner;
    PLIST_ENTRY Entry;
    WCHAR Text[16];
    BOOLEAN Writable = WpdMtpSupportsOperation(Dev, MTP_OP_SEND_OBJECT_INFO) || WpdMtpSupportsOperation(Dev, MTP_OP_DELETE_OBJECT);

    switch (Id)
    {
        case 2:
            Collection = WpdCollectionCreate(WpdKeys);
            Key = &WPD_PROPERTY_CAPABILITIES_SUPPORTED_COMMANDS;
            for (Index = 0; Collection && Index < ARRAYSIZE(SupportedCommands) && NT_SUCCESS(Status); Index++)
                Status = WpdKeysAdd(Collection, SupportedCommands[Index]);
            for (Index = 0; Collection && Writable && Index < ARRAYSIZE(ManagementCommands) && NT_SUCCESS(Status); Index++)
                Status = WpdKeysAdd(Collection, ManagementCommands[Index]);
            if (Collection && NT_SUCCESS(Status) && WpdMtpSupportsOperation(Dev, MTP_OP_SET_OBJECT_PROP_VALUE))
                Status = WpdKeysAdd(Collection, &WPD_COMMAND_OBJECT_PROPERTIES_SET);
            for (Index = 0; Collection && WpdMtpSupportsOperation(Dev, MTP_OP_GET_OBJECT_PROP_LIST) &&
                 Index < ARRAYSIZE(BulkCommands) && NT_SUCCESS(Status); Index++)
            {
                Status = WpdKeysAdd(Collection, BulkCommands[Index]);
            }
            break;
        case 3:
            if (!WpdValuesGetKey(Params, &WPD_PROPERTY_CAPABILITIES_COMMAND, &Command))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdValues);
            Key = &WPD_PROPERTY_CAPABILITIES_COMMAND_OPTIONS;
            if (Collection && WpdKeyEqual(&Command, &WPD_COMMAND_OBJECT_RESOURCES_READ))
                Status = WpdValuesSetBool(Collection, &WPD_OPTION_OBJECT_RESOURCES_NO_INPUT_BUFFER_ON_READ, TRUE);
            else if (Collection && WpdKeyEqual(&Command, &WPD_COMMAND_OBJECT_MANAGEMENT_DELETE_OBJECTS))
                Status = WpdValuesSetBool(Collection, &WPD_OPTION_OBJECT_MANAGEMENT_RECURSIVE_DELETE_SUPPORTED, TRUE);
            break;
        case 4:
            Collection = WpdCollectionCreate(WpdVariants);
            Key = &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORIES;
            if (Collection)
                Status = WpdVariantsAddGuid(Collection, &WPD_FUNCTIONAL_CATEGORY_STORAGE);
            break;
        case 5:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORY, &Guid))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdVariants);
            Key = &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_OBJECTS;
            if (!Collection)
                break;
            if (RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_DEVICE, sizeof(GUID)) ||
                RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_ALL, sizeof(GUID)))
            {
                Status = WpdVariantsAddString(Collection, WPD_DEVICE_OBJECT_ID);
            }
            if (RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_STORAGE, sizeof(GUID)) ||
                RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_ALL, sizeof(GUID)))
            {
                WpdMtpFindStorage(Dev, 0);
                for (Entry = Dev->Storages.Flink; Entry != &Dev->Storages && NT_SUCCESS(Status); Entry = Entry->Flink)
                {
                    WpdFormatObjectId(Text, TRUE, CONTAINING_RECORD(Entry, MTP_STORAGE, Link)->Id);
                    Status = WpdVariantsAddString(Collection, Text);
                }
            }
            break;
        case 6:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_FUNCTIONAL_CATEGORY, &Guid))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdVariants);
            Key = &WPD_PROPERTY_CAPABILITIES_CONTENT_TYPES;
            if (!Collection || (!RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_STORAGE, sizeof(GUID)) &&
                                !RtlEqualMemory(&Guid, &WPD_FUNCTIONAL_CATEGORY_ALL, sizeof(GUID))))
            {
                break;
            }
            for (Index = 0; Index < Dev->FormatCount && NT_SUCCESS(Status); Index++)
            {
                const GUID *Type = WpdMtpContentType(Dev->Formats[Index].Code);
                BOOLEAN Seen = FALSE;

                for (Inner = 0; Inner < Collection->Count && !Seen; Inner++)
                    Seen = RtlEqualMemory(&Collection->Items[Inner].Guid, Type, sizeof(GUID));
                if (!Seen)
                    Status = WpdVariantsAddGuid(Collection, Type);
            }
            break;
        case 7:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_CONTENT_TYPE, &Guid))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdVariants);
            Key = &WPD_PROPERTY_CAPABILITIES_FORMATS;
            if (Collection)
                Status = WpdAddFormatsForType(Dev, &Guid, Collection);
            break;
        case 8:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_FORMAT, &Guid) || !WpdMtpFormatFromGuid(&Guid, &Format))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdKeys);
            Key = &WPD_PROPERTY_CAPABILITIES_PROPERTY_KEYS;
            if (Collection)
                Status = WpdFormatKeys(Dev, Format, Collection);
            break;
        case 9:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_FORMAT, &Guid) || !WpdMtpFormatFromGuid(&Guid, &Format) ||
                !WpdValuesGetKey(Params, &WPD_PROPERTY_CAPABILITIES_PROPERTY_KEYS, &Command))
            {
                return WPD_E_INVALIDARG;
            }
            Collection = WpdCollectionCreate(WpdValues);
            Key = &WPD_PROPERTY_CAPABILITIES_PROPERTY_ATTRIBUTES;
            if (Collection)
                Status = WpdObjectAttributes(Dev, Format, &Command, FALSE, Collection);
            break;
        case 10:
            Collection = WpdCollectionCreate(WpdVariants);
            Key = &WPD_PROPERTY_CAPABILITIES_SUPPORTED_EVENTS;
            if (Collection)
                Status = WpdVariantsAddGuid(Collection, &WPD_EVENT_DEVICE_RESET);
            if (Collection && NT_SUCCESS(Status))
                Status = WpdVariantsAddGuid(Collection, &WPD_EVENT_OBJECT_REMOVED);
            if (Collection && NT_SUCCESS(Status))
                Status = WpdVariantsAddGuid(Collection, &WPD_EVENT_OBJECT_ADDED);
            for (Index = 0; Collection && Index < Dev->EventCount && NT_SUCCESS(Status); Index++)
            {
                if (Dev->Events[Index] == MTP_EVENT_OBJECT_INFO_CHANGED)
                    Status = WpdVariantsAddGuid(Collection, &WPD_EVENT_OBJECT_UPDATED);
            }
            break;
        case 11:
            if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_CAPABILITIES_EVENT, &Guid))
                return WPD_E_INVALIDARG;
            Collection = WpdCollectionCreate(WpdValues);
            Key = &WPD_PROPERTY_CAPABILITIES_EVENT_OPTIONS;
            if (Collection)
                Status = WpdValuesSetBool(Collection, &WPD_EVENT_OPTION_IS_BROADCAST_EVENT, TRUE);
            break;
        default:
            return WPD_E_NOTIMPL;
    }
    if (!Collection)
        return WPD_E_OUTOFMEMORY;
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, Key, Collection);
    else
        WpdCollectionFree(Collection);
    return WpdNtToHresult(Status);
}

static
HRESULT
WpdEnumeration(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPDMTP_CONTEXT Context;
    PWPD_COLLECTION Ids;
    WPD_OBJECT_REF Ref;
    PLIST_ENTRY Entry;
    ULONG Requested, Index;
    WCHAR Text[16];
    NTSTATUS Status = STATUS_SUCCESS;

    if (Id == 2)
    {
        PCWSTR Parent = WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_ENUMERATION_PARENT_ID);

        Context = WpdCreateContext(Dev, File, WpdContextEnum, L"CEnumContext");
        if (!Context)
            return WPD_E_OUTOFMEMORY;
        Context->Failed = !Parent;
        if (Parent && !WpdParseObjectId(Parent[0] ? Parent : WPD_DEVICE_OBJECT_ID, &Ref))
            Context->Failed = TRUE;
        Context->Properties = WpdCollectionCreate(WpdValues);
        if (Context->Properties && Parent)
            WpdValuesSetString(Context->Properties, &WPD_PROPERTY_OBJECT_ENUMERATION_PARENT_ID, Parent[0] ? Parent : WPD_DEVICE_OBJECT_ID);
        return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT, Context->Name));
    }

    Context = WpdFindContext(Dev, WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT), WpdContextEnum);
    if (!Context)
        return WPD_E_NOT_FOUND;
    if (Id == 4)
    {
        WpdFreeContext(Dev, Context);
        return WPD_S_OK;
    }
    if (Id != 3)
        return WPD_E_NOTIMPL;

    if (!WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_ENUMERATION_NUM_OBJECTS_REQUESTED, &Requested))
        return WPD_E_INVALIDARG;
    if (!Context->Handles && !Context->Failed)
    {
        HRESULT hr = WpdResolveObject(Dev, WpdValuesGetString(Context->Properties, &WPD_PROPERTY_OBJECT_ENUMERATION_PARENT_ID), &Ref);

        if (FAILED(hr))
        {
            Context->Failed = TRUE;
        }
        else if (Ref.Kind == WpdRefDevice)
        {
            WpdMtpFindStorage(Dev, 0);
            for (Entry = Dev->Storages.Flink; Entry != &Dev->Storages; Entry = Entry->Flink)
                Context->Count++;
            Context->Handles = ExAllocatePoolZero(PagedPool, max(Context->Count, 1) * sizeof(ULONG), WPDMTP_TAG);
            if (!Context->Handles)
                return WPD_E_OUTOFMEMORY;
            Index = 0;
            for (Entry = Dev->Storages.Flink; Entry != &Dev->Storages; Entry = Entry->Flink)
                Context->Handles[Index++] = CONTAINING_RECORD(Entry, MTP_STORAGE, Link)->Id;
            Context->Folder = TRUE;
        }
        else if (Ref.Kind == WpdRefStorage)
        {
            hr = WpdMtpGetChildren(Dev, Ref.Value, MTP_ROOT_PARENT, &Context->Handles, &Context->Count);
            if (FAILED(hr))
                Context->Failed = TRUE;
        }
        else if (Ref.Object->Format == MTP_FORMAT_ASSOCIATION)
        {
            hr = WpdMtpGetChildren(Dev, Ref.Object->StorageId, Ref.Object->Handle, &Context->Handles, &Context->Count);
            if (FAILED(hr))
                Context->Failed = TRUE;
        }
        else
        {
            Context->Failed = TRUE;
        }
    }

    Ids = WpdCollectionCreate(WpdVariants);
    if (!Ids)
        return WPD_E_OUTOFMEMORY;
    while (!Context->Failed && Context->Next < Context->Count && Ids->Count < Requested && NT_SUCCESS(Status))
    {
        WpdFormatObjectId(Text, Context->Folder, Context->Handles[Context->Next++]);
        Status = WpdVariantsAddString(Ids, Text);
    }
    Requested = Ids->Count == Requested;
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_OBJECT_ENUMERATION_OBJECT_IDS, Ids);
    else
        WpdCollectionFree(Ids);
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetString(Results, &WPD_PROPERTY_OBJECT_ENUMERATION_CONTEXT, Context->Name);
    if (!NT_SUCCESS(Status))
        return WpdNtToHresult(Status);
    return Requested ? WPD_S_OK : WPD_S_FALSE;
}

static
HRESULT
WpdCommon(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params,
    _Inout_ PWPD_COLLECTION Results)
{
    PWPD_COLLECTION Puids, Ids, Info;
    PLIST_ENTRY Entry;
    WCHAR Text[96];
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Id == WPD_COMMAND_COMMON_SAVE_CLIENT_INFORMATION.pid)
    {
        Info = WpdValuesGetCollection(Params, &WPD_PROPERTY_COMMON_CLIENT_INFORMATION, WpdValues);
        if (Info)
            WpdValuesGetUi4(Info, &WPD_CLIENT_DESIRED_ACCESS, &File->DesiredAccess);
        if (!File->HasClient)
        {
            WpdMakeContextName(File->ClientContext, ARRAYSIZE(File->ClientContext), L"");
            File->HasClient = TRUE;
        }
        return WpdNtToHresult(WpdValuesSetString(Results, &WPD_PROPERTY_COMMON_CLIENT_INFORMATION_CONTEXT, File->ClientContext));
    }
    if (Id != WPD_COMMAND_COMMON_GET_OBJECT_IDS_FROM_PERSISTENT_UNIQUE_IDS.pid)
        return WPD_E_NOTIMPL;

    Puids = WpdValuesGetCollection(Params, &WPD_PROPERTY_COMMON_PERSISTENT_UNIQUE_IDS, WpdVariants);
    if (!Puids)
        return WPD_E_INVALIDARG;
    Ids = WpdCollectionCreate(WpdVariants);
    if (!Ids)
        return WPD_E_OUTOFMEMORY;
    WpdMtpFindStorage(Dev, 0);
    for (Index = 0; Index < Puids->Count && NT_SUCCESS(Status); Index++)
    {
        PCWSTR Puid = Puids->Items[Index].Vt == WPD_VT_LPWSTR ? Puids->Items[Index].String.Buffer : NULL;
        PCWSTR Found = L"";

        Text[0] = UNICODE_NULL;
        if (Puid && !wcscmp(Puid, WPD_DEVICE_OBJECT_ID))
            Found = WPD_DEVICE_OBJECT_ID;
        for (Entry = Dev->Storages.Flink; Puid && !Found[0] && Entry != &Dev->Storages; Entry = Entry->Flink)
        {
            PMTP_STORAGE Storage = CONTAINING_RECORD(Entry, MTP_STORAGE, Link);

            WpdStoragePuid(Storage, Text, ARRAYSIZE(Text));
            if (!_wcsicmp(Text, Puid))
            {
                WpdFormatObjectId(Text, TRUE, Storage->Id);
                Found = Text;
            }
        }
        for (Entry = Dev->Objects.Flink; Puid && !Found[0] && Entry != &Dev->Objects; Entry = Entry->Flink)
        {
            PMTP_OBJECT Object = CONTAINING_RECORD(Entry, MTP_OBJECT, Link);

            if (!Object->PuidValid)
                continue;
            WpdFormatPuid(Object->Puid, Text);
            if (!_wcsicmp(Text, Puid))
            {
                WpdFormatObjectId(Text, FALSE, Object->Handle);
                Found = Text;
            }
        }
        Status = WpdVariantsAddString(Ids, Found);
    }
    if (NT_SUCCESS(Status))
        Status = WpdValuesSetCollection(Results, &WPD_PROPERTY_COMMON_OBJECT_IDS, Ids);
    else
        WpdCollectionFree(Ids);
    return WpdNtToHresult(Status);
}

static
BOOLEAN
WpdNeedsWrite(
    _In_ const GUID *Category,
    _In_ ULONG Id,
    _In_ PWPD_COLLECTION Params)
{
    ULONG Mode;

    if (RtlEqualMemory(Category, &WPD_CATEGORY_OBJECT_MANAGEMENT, sizeof(GUID)) ||
        RtlEqualMemory(Category, &WPD_CATEGORY_STORAGE, sizeof(GUID)) ||
        RtlEqualMemory(Category, &WPD_CATEGORY_MTP_EXT_VENDOR_OPERATIONS, sizeof(GUID)))
    {
        return TRUE;
    }
    if (RtlEqualMemory(Category, &WPD_CATEGORY_COMMON, sizeof(GUID)))
        return Id == WPD_COMMAND_COMMON_RESET_DEVICE.pid;
    if (RtlEqualMemory(Category, &WPD_CATEGORY_OBJECT_PROPERTIES, sizeof(GUID)))
        return Id == WPD_COMMAND_OBJECT_PROPERTIES_SET.pid || Id == WPD_COMMAND_OBJECT_PROPERTIES_DELETE.pid;
    if (RtlEqualMemory(Category, &WPD_CATEGORY_OBJECT_PROPERTIES_BULK, sizeof(GUID)))
        return Id >= WPD_COMMAND_OBJECT_PROPERTIES_BULK_SET_VALUES_BY_OBJECT_LIST_START.pid;
    if (RtlEqualMemory(Category, &WPD_CATEGORY_OBJECT_RESOURCES, sizeof(GUID)))
    {
        if (Id == WPD_COMMAND_OBJECT_RESOURCES_OPEN.pid)
            return WpdValuesGetUi4(Params, &WPD_PROPERTY_OBJECT_RESOURCES_ACCESS_MODE, &Mode) && (Mode & 3);
        return Id == WPD_COMMAND_OBJECT_RESOURCES_WRITE.pid || Id == WPD_COMMAND_OBJECT_RESOURCES_DELETE.pid ||
               Id == WPD_COMMAND_OBJECT_RESOURCES_CREATE_RESOURCE.pid || Id == WPD_COMMAND_OBJECT_RESOURCES_REVERT.pid ||
               Id == WPD_COMMAND_OBJECT_RESOURCES_COMMIT.pid;
    }
    return FALSE;
}

BOOLEAN
WpdMtpRequestBlocked(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Params)
{
    PCWSTR Context;

    if (!Dev->ActiveTransfer)
        return FALSE;
    Context = WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_MANAGEMENT_CONTEXT);
    if (!Context)
        Context = WpdValuesGetString(Params, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT);
    if (!Context)
        Context = WpdValuesGetString(Params, &WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT);
    return !Context || wcscmp(Context, Dev->ActiveTransfer->Name) != 0;
}

NTSTATUS
WpdMtpDispatch(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File,
    _In_ BOOLEAN WriteAccess,
    _In_ PWPD_COLLECTION Params,
    _In_ ULONG OutputLimit,
    _Out_ PWPD_COLLECTION *Results)
{
    WPD_COMMAND Command;
    PWPD_COLLECTION Out;
    HRESULT hr;

    *Results = NULL;
    Out = WpdCollectionCreate(WpdValues);
    if (!Out)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!WpdValuesGetGuid(Params, &WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &Command.Category) ||
        !WpdValuesGetUi4(Params, &WPD_PROPERTY_COMMON_COMMAND_ID, &Command.Id))
    {
        hr = WPD_E_INVALIDARG;
    }
    else if (!WriteAccess && WpdNeedsWrite(&Command.Category, Command.Id, Params))
    {
        hr = WPD_E_ACCESSDENIED;
    }
    else if (!Dev->SessionOpen)
    {
        hr = WPD_E_DEVICE_NOT_CONNECTED;
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_COMMON, sizeof(GUID)))
    {
        hr = WpdCommon(Dev, File, Command.Id, Params, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_CAPABILITIES, sizeof(GUID)))
    {
        hr = WpdCapabilities(Dev, Command.Id, Params, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_OBJECT_ENUMERATION, sizeof(GUID)))
    {
        hr = WpdEnumeration(Dev, File, Command.Id, Params, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_OBJECT_PROPERTIES, sizeof(GUID)))
    {
        switch (Command.Id)
        {
            case 2:
                hr = WpdPropertiesGetSupported(Dev, Params, Out);
                break;
            case 3:
                hr = WpdPropertiesGetAttributes(Dev, Params, Out);
                break;
            case 4:
                hr = WpdPropertiesGet(Dev, Params, FALSE, Out);
                break;
            case 5:
                hr = WpdPropertiesSet(Dev, Params, Out);
                break;
            case 6:
                hr = WpdPropertiesGet(Dev, Params, TRUE, Out);
                break;
            default:
                hr = WPD_E_NOTIMPL;
                break;
        }
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_OBJECT_RESOURCES, sizeof(GUID)))
    {
        hr = WpdResources(Dev, File, Command.Id, Params, OutputLimit, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_OBJECT_MANAGEMENT, sizeof(GUID)))
    {
        hr = WpdManagement(Dev, File, Command.Id, Params, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_MTP_EXT_VENDOR_OPERATIONS, sizeof(GUID)))
    {
        hr = WpdVendor(Dev, File, Command.Id, Params, OutputLimit, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WPD_CATEGORY_OBJECT_PROPERTIES_BULK, sizeof(GUID)))
    {
        hr = WpdBulk(Dev, File, Command.Id, Params, Out);
    }
    else if (RtlEqualMemory(&Command.Category, &WpdPrivateCategory, sizeof(GUID)))
    {
        if (Command.Id == WpdPrivateCommand11.pid)
            hr = WPD_E_NOT_FOUND;
        else if (Command.Id == WpdPrivateCommand12.pid)
            hr = WpdNtToHresult(WpdValuesSetUi4(Out, &WpdPrivateResult1002, 1));
        else
            hr = WPD_E_NOTIMPL;
    }
    else
    {
        hr = WPD_E_NOTIMPL;
    }
    if (FAILED(hr))
    {
        PWPD_VALUE Context = WpdValuesFind(Out, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT);
        WPD_VALUE Saved = { 0 };

        if (Context)
            WpdValueCopy(&Saved, Context);
        WpdCollectionFree(Out);
        Out = WpdCollectionCreate(WpdValues);
        if (!Out)
        {
            WpdValueClear(&Saved);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (Saved.Vt)
            WpdValuesSet(Out, &WPD_PROPERTY_OBJECT_RESOURCES_CONTEXT, &Saved);
    }
    if (!NT_SUCCESS(WpdValuesSetError(Out, &WPD_PROPERTY_COMMON_HRESULT, hr)))
    {
        WpdCollectionFree(Out);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *Results = Out;
    return STATUS_SUCCESS;
}

VOID
WpdMtpReleaseFile(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_FILE File)
{
    PLIST_ENTRY Entry, Next;

    for (Entry = Dev->Contexts.Flink; Entry != &Dev->Contexts; Entry = Next)
    {
        PWPDMTP_CONTEXT Context = CONTAINING_RECORD(Entry, WPDMTP_CONTEXT, Link);

        Next = Entry->Flink;
        if (Context->Owner == File)
        {
            if (Context->Type == WpdContextUpload && Context->Handle && Dev->ActiveTransfer == Context)
                WpdUploadRevert(Dev, Context);
            WpdFreeContext(Dev, Context);
        }
    }
}

VOID
WpdMtpReleaseAllContexts(
    _In_ PWPDMTP_DEVICE Dev)
{
    while (!IsListEmpty(&Dev->Contexts))
        WpdFreeContext(Dev, CONTAINING_RECORD(Dev->Contexts.Flink, WPDMTP_CONTEXT, Link));
    Dev->ActiveTransfer = NULL;
}

static
VOID
WpdPostEvent(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPD_COLLECTION Values)
{
    PTARGET_DEVICE_CUSTOM_NOTIFICATION Notification;
    ULONG Size = WpdSerializedSize(Values), Total;
    NTSTATUS Status;

    Total = FIELD_OFFSET(TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer) + Size;
    Notification = ExAllocatePoolZero(PagedPool, Total, WPDMTP_TAG);
    if (!Notification)
        return;
    Notification->Version = 1;
    Notification->Size = (USHORT)Total;
    Notification->Event = WPD_EVENT_NOTIFICATION;
    Notification->NameBufferOffset = -1;
    WpdSerialize(Values, Notification->CustomDataBuffer, Size);
    Status = IoReportTargetDeviceChangeAsynchronous(WdfDeviceWdmGetPhysicalDevice(Dev->Device), Notification, NULL, NULL);
    if (!NT_SUCCESS(Status))
        DPRINT1("WPDMTP: event notification failed 0x%08lx\n", Status);
    ExFreePoolWithTag(Notification, WPDMTP_TAG);
}

static
NTSTATUS
WpdEventObjectIds(
    _In_ PMTP_OBJECT Object,
    _Inout_ PWPD_COLLECTION Values,
    _In_ BOOLEAN Puid)
{
    WCHAR Text[48];
    NTSTATUS Status = STATUS_SUCCESS;

    if (Puid && Object->PuidValid)
    {
        WpdFormatPuid(Object->Puid, Text);
        Status = WpdValuesSetString(Values, &WPD_OBJECT_PERSISTENT_UNIQUE_ID, Text);
    }
    return Status;
}

static
VOID
WpdBuildObjectEvent(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ const GUID *EventId,
    _In_ PMTP_OBJECT Object,
    _In_opt_ PCWSTR Cookie,
    _In_ BOOLEAN Brief,
    _In_ BOOLEAN Hierarchy,
    _Inout_ PWPD_COLLECTION Values)
{
    WCHAR Text[16];

    WpdFormatObjectId(Text, FALSE, Object->Handle);
    WpdValuesSetString(Values, &WPD_OBJECT_ID, Text);
    if (Brief)
    {
        WpdValuesSetGuid(Values, &WPD_EVENT_PARAMETER_EVENT_ID, EventId);
        if (Hierarchy)
            WpdValuesSetBool(Values, &WPD_EVENT_PARAMETER_CHILD_HIERARCHY_CHANGED, TRUE);
        WpdEventObjectIds(Object, Values, TRUE);
        WpdObjectValue(Dev, Object, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID, Values);
        WpdObjectValue(Dev, Object, &WPD_OBJECT_PARENT_ID, Values);
        return;
    }
    WpdEventObjectIds(Object, Values, TRUE);
    WpdObjectValue(Dev, Object, &WPD_OBJECT_NAME, Values);
    WpdObjectValue(Dev, Object, &WPD_OBJECT_CONTENT_TYPE, Values);
    WpdValuesSetError(Values, &WPD_FUNCTIONAL_OBJECT_CATEGORY, WPD_E_NOT_FOUND);
    WpdObjectValue(Dev, Object, &WPD_OBJECT_ORIGINAL_FILE_NAME, Values);
    WpdObjectValue(Dev, Object, &WPD_OBJECT_PARENT_ID, Values);
    WpdObjectValue(Dev, Object, &WPD_OBJECT_CONTAINER_FUNCTIONAL_OBJECT_ID, Values);
    WpdValuesSetGuid(Values, &WPD_EVENT_PARAMETER_EVENT_ID, EventId);
    if (Cookie)
        WpdValuesSetString(Values, &WPD_EVENT_PARAMETER_OBJECT_CREATION_COOKIE, Cookie);
}

VOID
WpdMtpPostObjectEvent(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ WPDMTP_OBJECT_EVENT Kind,
    _In_ PMTP_OBJECT Object,
    _In_opt_ PCWSTR Cookie)
{
    PWPD_COLLECTION Values;

    Values = WpdCollectionCreate(WpdValues);
    if (!Values)
        return;
    switch (Kind)
    {
        case WpdObjectEventAdded:
            WpdBuildObjectEvent(Dev, &WPD_EVENT_OBJECT_ADDED, Object, Cookie, FALSE, FALSE, Values);
            break;
        case WpdObjectEventUpdated:
            WpdBuildObjectEvent(Dev, &WPD_EVENT_OBJECT_UPDATED, Object, NULL, FALSE, FALSE, Values);
            break;
        case WpdObjectEventHierarchy:
            WpdBuildObjectEvent(Dev, &WPD_EVENT_OBJECT_UPDATED, Object, NULL, TRUE, TRUE, Values);
            break;
        case WpdObjectEventRemoved:
            WpdBuildObjectEvent(Dev, &WPD_EVENT_OBJECT_REMOVED, Object, NULL, TRUE, FALSE, Values);
            break;
    }
    WpdPostEvent(Dev, Values);
    WpdCollectionFree(Values);
}

VOID
WpdMtpPostHandleEvent(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ WPDMTP_OBJECT_EVENT Kind,
    _In_ ULONG Handle,
    _In_opt_ PCWSTR Cookie)
{
    PMTP_OBJECT Object;

    WpdMtpForgetObject(Dev, Handle);
    if (SUCCEEDED(WpdMtpGetObject(Dev, Handle, TRUE, &Object)))
        WpdMtpPostObjectEvent(Dev, Kind, Object, Cookie);
}

VOID
WpdMtpProcessEvent(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ PWPDMTP_EVENT Event)
{
    PWPD_COLLECTION Values;
    PMTP_OBJECT Object;
    WCHAR Text[48];
    const GUID *EventId = NULL;
    BOOLEAN Storage = FALSE;

    DPRINT1("WPDMTP: event 0x%04x parameter 0x%lx\n", Event->Code, Event->ParamCount ? Event->Params[0] : 0);
    WpdMtpLock(Dev);
    if (!Dev->SessionOpen || Dev->ActiveTransfer)
    {
        WpdMtpUnlock(Dev);
        return;
    }
    switch (Event->Code)
    {
        case MTP_EVENT_OBJECT_ADDED:
            WpdMtpPostHandleEvent(Dev, WpdObjectEventAdded, Event->Params[0], NULL);
            WpdMtpUnlock(Dev);
            return;
        case MTP_EVENT_OBJECT_REMOVED:
            if (SUCCEEDED(WpdMtpGetObject(Dev, Event->Params[0], FALSE, &Object)))
                WpdMtpPostObjectEvent(Dev, WpdObjectEventRemoved, Object, NULL);
            WpdMtpForgetObject(Dev, Event->Params[0]);
            WpdMtpUnlock(Dev);
            return;
        case MTP_EVENT_OBJECT_INFO_CHANGED:
        case MTP_EVENT_OBJECT_PROP_CHANGED:
            WpdMtpPostHandleEvent(Dev, WpdObjectEventUpdated, Event->Params[0], NULL);
            WpdMtpUnlock(Dev);
            return;
    }
    Values = WpdCollectionCreate(WpdValues);
    if (!Values)
    {
        WpdMtpUnlock(Dev);
        return;
    }
    switch (Event->Code)
    {
        case MTP_EVENT_STORE_ADDED:
            EventId = &WPD_EVENT_OBJECT_ADDED;
            Storage = TRUE;
            WpdMtpLoadStorages(Dev);
            WpdValuesSetString(Values, &WPD_OBJECT_PARENT_ID, WPD_DEVICE_OBJECT_ID);
            break;
        case MTP_EVENT_STORE_REMOVED:
            EventId = &WPD_EVENT_OBJECT_REMOVED;
            Storage = TRUE;
            WpdMtpForgetAll(Dev);
            WpdMtpLoadStorages(Dev);
            break;
        case MTP_EVENT_STORAGE_INFO_CHANGED:
            WpdMtpLoadStorages(Dev);
            break;
        case MTP_EVENT_DEVICE_INFO_CHANGED:
        case MTP_EVENT_DEVICE_PROP_CHANGED:
            break;
    }
    if (EventId)
    {
        WpdValuesSetGuid(Values, &WPD_EVENT_PARAMETER_EVENT_ID, EventId);
        WpdFormatObjectId(Text, Storage, Event->Params[0]);
        WpdValuesSetString(Values, &WPD_OBJECT_ID, Text);
        WpdPostEvent(Dev, Values);
    }
    WpdCollectionFree(Values);
    WpdMtpUnlock(Dev);
}
