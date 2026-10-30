/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            lib/ntdll/rtl/process.c
 * PURPOSE:         Process functions
 * PROGRAMMER:      Ariadne (ariadne@xs4all.nl)
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES *******************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* RtlGetDeviceFamilyInfoEnum is implemented independently of the SDK target. */
#ifndef UNIFIEDBUILDREVISION_KEY
#define UNIFIEDBUILDREVISION_KEY L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"
#define UNIFIEDBUILDREVISION_VALUE L"UBR"
#define UNIFIEDBUILDREVISION_MIN 0x00000000
#define DEVICEFAMILYDEVICEFORM_KEY L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM"
#define DEVICEFAMILYDEVICEFORM_VALUE L"DeviceForm"
#define DEVICEFAMILYINFOENUM_DESKTOP 0x00000003
#define DEVICEFAMILYINFOENUM_SERVER 0x00000009
#define DEVICEFAMILYDEVICEFORM_UNKNOWN 0x00000000
#define DEVICEFAMILYDEVICEFORM_MAX 0x0000002F
#endif

/* FUNCTIONS ******************************************************************/

static signed char g_ReportProductType = 0;

/* HACK: ReactOS specific changes, see bug-reports CORE-6611 and CORE-4620 (aka. #5003) */
static VOID NTAPI
SetRosSpecificInfo(IN OUT PRTL_OSVERSIONINFOEXW VersionInformation)
{
    CHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION kvpInfo = (PVOID)Buffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE hKey;
    ULONG Length;
    NTSTATUS Status;
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ReactOS\\Settings\\Version");
    UNICODE_STRING ValName = RTL_CONSTANT_STRING(L"ReportAsWorkstation");

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_OPENIF | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    /* Don't change anything if the key doesn't exist */
    Status = (g_ReportProductType == 0) ? NtOpenKey(&hKey, KEY_READ, &ObjectAttributes) : STATUS_CANCELLED;
    if (NT_SUCCESS(Status))
    {
        /* Get the value from the registry and make sure it's a 32-bit value */
        Status = NtQueryValueKey(hKey,
                                 &ValName,
                                 KeyValuePartialInformation,
                                 kvpInfo,
                                 sizeof(Buffer),
                                 &Length);
        if (NT_SUCCESS(Status) &&
            (kvpInfo->Type == REG_DWORD) &&
            (kvpInfo->DataLength == sizeof(ULONG)))
        {
            BOOLEAN IsWorkstation = SharedUserData->NtProductType == NtProductWinNt;
            BOOLEAN ReportAsWorkstation = (*(PULONG)kvpInfo->Data) != 0;
            if (IsWorkstation != ReportAsWorkstation)
            {
                g_ReportProductType = ReportAsWorkstation ? VER_NT_WORKSTATION : VER_NT_SERVER;
            }
        }

        /* Close the handle */
        NtClose(hKey);
    }

    if (g_ReportProductType > 0)
    {
        VersionInformation->wProductType = g_ReportProductType;
        DPRINT("We modified the reported OS product type from %d to %d\n",
                SharedUserData->NtProductType, VersionInformation->wProductType);
    }
    else
    {
        g_ReportProductType = -1; /* No override, caller gets the real value */
    }
}

/**********************************************************************
 * NAME                         EXPORTED
 *  RtlGetNtProductType
 *
 * DESCRIPTION
 *  Retrieves the OS product type.
 *
 * ARGUMENTS
 *  ProductType Pointer to the product type variable.
 *
 * RETURN VALUE
 *  TRUE if successful, otherwise FALSE
 *
 * NOTE
 *  ProductType can be one of the following values:
 *    1 Workstation (WinNT)
 *    2 Server (LanmanNT)
 *    3 Advanced Server (ServerNT)
 *
 * REVISIONS
 *  2000-08-10 ekohl
 *
 * @implemented
 */
BOOLEAN NTAPI
RtlGetNtProductType(_Out_ PNT_PRODUCT_TYPE ProductType)
{
    *ProductType = SharedUserData->NtProductType;

    if (g_ReportProductType == 0)
    {
        /* Initialize cached value */
        RTL_OSVERSIONINFOEXW ovi;
        ovi.dwOSVersionInfoSize = sizeof(ovi);
        ovi.wProductType = *ProductType;
        SetRosSpecificInfo(&ovi);
    }

    if (g_ReportProductType > 0)
    {
        *ProductType = g_ReportProductType;
        DPRINT("Overriding RtlGetNtProductType to return %d\n", *ProductType);
    }
    return TRUE;
}

/**********************************************************************
 * NAME                         EXPORTED
 *  RtlGetNtVersionNumbers
 *
 * DESCRIPTION
 *  Get the version numbers of the run time library.
 *
 * ARGUMENTS
 *  pMajorVersion [OUT] Destination for the Major version
 *  pMinorVersion [OUT] Destination for the Minor version
 *  pBuildNumber  [OUT] Destination for the Build version
 *
 * RETURN VALUE
 *  Nothing.
 *
 * NOTES
 *  - Introduced in Windows XP (NT 5.1)
 *  - Since this call didn't exist before XP, we report at least the version
 *    5.1. This fixes the loading of msvcrt.dll as released with XP Home,
 *    which fails in DLLMain() if the major version isn't 5.
 *
 * @implemented
 */
/*
 * Version lie for applications that demand Windows 10.
 *
 * The system reports 11.0 (REACTOS_REPORTED_NT), which is the truth. An
 * application that asks for "10.0 or later" -- the normal check, through its
 * manifest or VerifyVersionInfo -- is satisfied by 11.0 and never reaches this
 * code. An application that insists on *exactly* 10.0 refuses to start on
 * anything newer, and there are a few of those; Windows itself answers them with
 * an app-compat shim, and so does this:
 *
 *   HKLM\System\CurrentControlSet\Control\Session Manager\AppCompat
 *       VersionLie  REG_MULTI_SZ  = foo.exe\0bar.exe\0
 *
 * A name in that list gets 10.0.19045 from every version query of the process;
 * everything else gets the truth. The decision is per process (the image path),
 * so one lying application cannot lower the version for the rest of the system.
 *
 * This deliberately does not go through ntdll.AppCompat/ because the PEB is
 * where the answer already lives: RtlGetVersion reads Peb->OSMajorVersion, so
 * the shim only has to compute a different number for the calls that produce it.
 */
static BOOLEAN g_VersionLieChecked;
static BOOLEAN g_VersionLie;

#define VERSION_LIE_KEY \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\AppCompat"
#define VERSION_LIE_VALUE L"VersionLie"
#define VERSION_LIE_MAJOR 10
#define VERSION_LIE_MINOR 0
#define VERSION_LIE_BUILD 19045

static BOOLEAN NTAPI
IsVersionLieImage(VOID)
{
    PEB *Peb = NtCurrentPeb();
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(VERSION_LIE_KEY);
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(VERSION_LIE_VALUE);
    KEY_VALUE_PARTIAL_INFORMATION *Info;
    UNICODE_STRING ImageName, Entry;
    PWSTR Base;
    USHORT BaseLength, i;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 2048];
    HANDLE Key = NULL;
    ULONG Length = 0;
    NTSTATUS Status;
    BOOLEAN Match = FALSE;

    if (g_VersionLieChecked)
        return g_VersionLie;
    g_VersionLieChecked = TRUE;

    if (!Peb || !Peb->ProcessParameters)
        return FALSE;

    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Info = (KEY_VALUE_PARTIAL_INFORMATION *)Buffer;
    Status = NtQueryValueKey(Key, &ValueName, KeyValuePartialInformation,
                             Info, sizeof(Buffer), &Length);
    NtClose(Key);
    if (!NT_SUCCESS(Status) || Info->Type != REG_MULTI_SZ || Info->DataLength < sizeof(WCHAR))
        return FALSE;

    /* The image name is a full path; the list holds bare file names. */
    ImageName = Peb->ProcessParameters->ImagePathName;
    Base = ImageName.Buffer;
    BaseLength = ImageName.Length;
    for (i = 0; i < ImageName.Length / sizeof(WCHAR); i++)
    {
        if (ImageName.Buffer[i] == (WCHAR)0x5C || ImageName.Buffer[i] == (WCHAR)0x2F)
        {
            Base = ImageName.Buffer + i + 1;
            BaseLength = ImageName.Length - (i + 1) * sizeof(WCHAR);
        }
    }

    /* Walk the multi-string: entries are NUL terminated, the list ends with an
       empty one. */
    Entry.Buffer = (PWSTR)Info->Data;
    Entry.Length = 0;
    for (i = 0; i * sizeof(WCHAR) < Info->DataLength; i++)
    {
        WCHAR c = ((PWSTR)Info->Data)[i];

        if (c == UNICODE_NULL)
        {
            if (Entry.Length != 0 &&
                Entry.Length == BaseLength &&
                _wcsnicmp(Entry.Buffer, Base, BaseLength / sizeof(WCHAR)) == 0)
            {
                Match = TRUE;
                break;
            }
            Entry.Buffer = (PWSTR)Info->Data + i + 1;
            Entry.Length = 0;
        }
        else
        {
            Entry.Length += sizeof(WCHAR);
        }
    }

    g_VersionLie = Match;
    return Match;
}

VOID NTAPI
RtlGetNtVersionNumbers(OUT PULONG pMajorVersion,
                       OUT PULONG pMinorVersion,
                       OUT PULONG pBuildNumber)
{
    PPEB pPeb = NtCurrentPeb();
    BOOLEAN Lie = IsVersionLieImage();

    if (Lie)
    {
        if (pMajorVersion) *pMajorVersion = VERSION_LIE_MAJOR;
        if (pMinorVersion) *pMinorVersion = VERSION_LIE_MINOR;
        if (pBuildNumber) *pBuildNumber = (0xF0000000 | VERSION_LIE_BUILD);
        return;
    }

    if (pMajorVersion)
    {
        *pMajorVersion = pPeb->OSMajorVersion < 5 ? 5 : pPeb->OSMajorVersion;
    }

    if (pMinorVersion)
    {
        if ( (pPeb->OSMajorVersion  < 5) ||
            ((pPeb->OSMajorVersion == 5) && (pPeb->OSMinorVersion < 1)) )
            *pMinorVersion = 1;
        else
            *pMinorVersion = pPeb->OSMinorVersion;
    }

    if (pBuildNumber)
    {
        /* Windows really does this! */
        *pBuildNumber = (0xF0000000 | pPeb->OSBuildNumber);
    }
}

/*
 * @implemented
 * @note User-mode version of RtlGetVersion in ntoskrnl/rtl/misc.c
 */
NTSTATUS NTAPI
RtlGetVersion(IN OUT PRTL_OSVERSIONINFOW lpVersionInformation)
{
    SIZE_T Length;
    PPEB Peb = NtCurrentPeb();

    if (IsVersionLieImage())
    {
        lpVersionInformation->dwMajorVersion = VERSION_LIE_MAJOR;
        lpVersionInformation->dwMinorVersion = VERSION_LIE_MINOR;
        lpVersionInformation->dwBuildNumber = VERSION_LIE_BUILD;
    }
    else
    {
        lpVersionInformation->dwMajorVersion = Peb->OSMajorVersion;
        lpVersionInformation->dwMinorVersion = Peb->OSMinorVersion;
        lpVersionInformation->dwBuildNumber = Peb->OSBuildNumber;
    }
    lpVersionInformation->dwPlatformId = Peb->OSPlatformId;
    RtlZeroMemory(lpVersionInformation->szCSDVersion, sizeof(lpVersionInformation->szCSDVersion));

    /* If we have a CSD version string, initialized by Application Compatibility... */
    if (Peb->CSDVersion.Length && Peb->CSDVersion.Buffer && Peb->CSDVersion.Buffer[0] != UNICODE_NULL)
    {
        /* ... copy it... */
        Length = min(wcslen(Peb->CSDVersion.Buffer), ARRAYSIZE(lpVersionInformation->szCSDVersion) - 1);
        wcsncpy(lpVersionInformation->szCSDVersion, Peb->CSDVersion.Buffer, Length);
    }
    else
    {
        /* ... otherwise we just null-terminate it */
        Length = 0;
    }

    /* Always null-terminate the user CSD version string */
    lpVersionInformation->szCSDVersion[Length] = UNICODE_NULL;

    if (lpVersionInformation->dwOSVersionInfoSize == sizeof(RTL_OSVERSIONINFOEXW))
    {
        PRTL_OSVERSIONINFOEXW InfoEx = (PRTL_OSVERSIONINFOEXW)lpVersionInformation;
        InfoEx->wServicePackMajor = (Peb->OSCSDVersion >> 8) & 0xFF;
        InfoEx->wServicePackMinor = Peb->OSCSDVersion & 0xFF;
        InfoEx->wSuiteMask = SharedUserData->SuiteMask & 0xFFFF;
        if (InfoEx->wSuiteMask & VER_SUITE_SINGLEUSERTS)
            InfoEx->wSuiteMask &= ~VER_SUITE_TERMINAL;
        InfoEx->wProductType = SharedUserData->NtProductType;
        InfoEx->wReserved = 0;

        /* HACK: ReactOS specific changes, see bug-reports CORE-6611 and CORE-4620 (aka. #5003) */
        SetRosSpecificInfo(InfoEx);
    }

    return STATUS_SUCCESS;
}

static ULONG
RtlpQueryDeviceFamilyValue(
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName,
    _In_ ULONG DefaultValue)
{
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        ULONG_PTR Alignment;
        UCHAR Bytes[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } Buffer;
    PKEY_VALUE_PARTIAL_INFORMATION kvpInfo = (PVOID)Buffer.Bytes;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValName;
    HANDLE hKey;
    ULONG Length;
    ULONG Value = DefaultValue;
    NTSTATUS Status;

    RtlInitUnicodeString(&KeyName, KeyPath);
    RtlInitUnicodeString(&ValName, ValueName);

    InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenKey(&hKey, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Value;

    Status = NtQueryValueKey(hKey, &ValName, KeyValuePartialInformation, kvpInfo, sizeof(Buffer), &Length);
    if (NT_SUCCESS(Status) &&
        (kvpInfo->Type == REG_DWORD) &&
        (kvpInfo->DataLength == sizeof(ULONG)))
    {
        Value = *(PULONG)kvpInfo->Data;
    }

    NtClose(hKey);
    return Value;
}

VOID
NTAPI
RtlGetDeviceFamilyInfoEnum(
    _Out_opt_ PULONGLONG pullUAPInfo,
    _Out_opt_ PULONG pulDeviceFamily,
    _Out_opt_ PULONG pulDeviceForm)
{
    PPEB Peb = NtCurrentPeb();

    if (pullUAPInfo != NULL)
    {
        ULONG Ubr = RtlpQueryDeviceFamilyValue(UNIFIEDBUILDREVISION_KEY, UNIFIEDBUILDREVISION_VALUE, UNIFIEDBUILDREVISION_MIN);

        *pullUAPInfo = ((ULONGLONG)(Peb->OSMajorVersion & 0xFFFF) << 48) |
                       ((ULONGLONG)(Peb->OSMinorVersion & 0xFFFF) << 32) |
                       ((ULONGLONG)(Peb->OSBuildNumber & 0xFFFF) << 16) |
                       ((ULONGLONG)(Ubr & 0xFFFF));
    }

    if (pulDeviceFamily != NULL)
    {
        *pulDeviceFamily = (SharedUserData->NtProductType == NtProductWinNt) ?
                           DEVICEFAMILYINFOENUM_DESKTOP :
                           DEVICEFAMILYINFOENUM_SERVER;
    }

    if (pulDeviceForm != NULL)
    {
        ULONG DeviceForm = RtlpQueryDeviceFamilyValue(DEVICEFAMILYDEVICEFORM_KEY, DEVICEFAMILYDEVICEFORM_VALUE, DEVICEFAMILYDEVICEFORM_UNKNOWN);

        if (DeviceForm > DEVICEFAMILYDEVICEFORM_MAX)
            DeviceForm = DEVICEFAMILYDEVICEFORM_UNKNOWN;

        *pulDeviceForm = DeviceForm;
    }
}

/* EOF */
