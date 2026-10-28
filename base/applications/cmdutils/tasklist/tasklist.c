/*
 * PROJECT:     ReactOS Tasklist Command
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Displays a list of currently running processes on the computer.
 * COPYRIGHT:   Copyright 2021 He Yang <1160386205@qq.com>
 */

#include "tasklist.h"

// the strings in OptionList are the command-line options.
// should always correspond with the defines below, in sequence (except OPTION_INVALID)
static PCWSTR OptionList[] = { L"?", L"nh", L"fi" };

#define OPTION_INVALID    -1
#define OPTION_HELP       0
#define OPTION_NOHEADER    1
#define OPTION_FILTER     2

typedef enum _FILTER_FIELD
{
    FilterImageName,
    FilterPid,
    FilterSession,
    FilterMemUsage
} FILTER_FIELD;

typedef enum _FILTER_OPERATOR
{
    FilterEqual,
    FilterNotEqual,
    FilterGreater,
    FilterLess,
    FilterGreaterEqual,
    FilterLessEqual
} FILTER_OPERATOR;

typedef struct _TASK_FILTER
{
    FILTER_FIELD Field;
    FILTER_OPERATOR Operator;
    BOOL Prefix;
    ULONGLONG Number;
    WCHAR Text[MAX_PATH];
} TASK_FILTER, *PTASK_FILTER;

// the max string length PrintResString can handle
#define RES_STR_MAXLEN 64

// Print split line
VOID PrintSplitLine(UINT Length)
{
    for (; Length; Length--)
    {
        ConPuts(StdOut, L"=");
    }
}

// Print spaces
VOID PrintSpace(UINT Length)
{
    ConPrintf(StdOut, L"%*ls", (INT)Length, L"");
}

// Print a string.
// if bAlignLeft == TRUE then aligned to left, otherwise aligned to right
// MaxWidth is the width for printing.
VOID PrintString(LPCWSTR String, UINT MaxWidth, BOOL bAlignLeft)
{
    ConPrintf(StdOut, bAlignLeft ? L"%-*.*ls" : L"%*.*ls", MaxWidth, MaxWidth, String);
}

// Print a string from resource
// if bAlignLeft == TRUE then aligned to left, otherwise aligned to right
// MaxWidth is the width for printing.
// The string WILL be truncated if it's longer than RES_STR_MAXLEN
VOID PrintResString(UINT uID, UINT MaxWidth, BOOL bAlignLeft)
{
    WCHAR StringBuffer[RES_STR_MAXLEN];
    LoadStringW(NULL, uID, StringBuffer, _countof(StringBuffer));
    PrintString(StringBuffer, MaxWidth, bAlignLeft);
}

// Print a number, aligned to right.
// MaxWidth is the width for printing.
// the number WILL NOT be truncated if it's longer than MaxWidth
VOID PrintNum(LONGLONG Number, UINT MaxWidth)
{
    ConPrintf(StdOut, L"%*lld", MaxWidth, Number);
}

// Print memory size using KB as unit, with comma-separated number, aligned to right.
// MaxWidth is the width for printing.
// the number WILL be truncated if it's longer than MaxWidth
BOOL PrintMemory(SIZE_T MemorySizeByte, UINT MaxWidth)
{
    SIZE_T MemorySize = MemorySizeByte >> 10;

    WCHAR NumberString[27] = { 0 }; // length 26 is enough to display ULLONG_MAX in decimal with comma, one more for zero-terminated.
    C_ASSERT(sizeof(SIZE_T) <= 8);

    PWCHAR pNumberStr = NumberString;

    // calculate the length
    UINT PrintLength = 0;
    SIZE_T Tmp = MemorySize;
    UINT Mod = 1;

    do
    {
        Tmp /= 10;
        PrintLength++;
        Mod *= 10;
    } while (Tmp);

    for (UINT i = PrintLength; i; i--)
    {
        Mod /= 10;
        *pNumberStr = L'0' + (MemorySize / Mod);
        MemorySize %= Mod;
        pNumberStr++;

        if (i != 1 && i % 3 == 1)
        {
            *pNumberStr = L',';
            pNumberStr++;
        }
    }

    WCHAR FormatStr[RES_STR_MAXLEN];
    LoadStringW(NULL, IDS_MEMORY_STR, FormatStr, _countof(FormatStr));

    WCHAR String[RES_STR_MAXLEN + _countof(NumberString)] = { 0 };
    StringCchPrintfW(String, _countof(String), FormatStr, NumberString);
    PrintString(String, MaxWidth, FALSE);

    return TRUE;
}

VOID PrintHeader(VOID)
{
    PrintResString(IDS_HEADER_IMAGENAME, COLUMNWIDTH_IMAGENAME, TRUE);
    PrintSpace(1);
    PrintResString(IDS_HEADER_PID,       COLUMNWIDTH_PID,       FALSE);
    PrintSpace(1);
    PrintResString(IDS_HEADER_SESSION,   COLUMNWIDTH_SESSION,   FALSE);
    PrintSpace(1);
    PrintResString(IDS_HEADER_MEMUSAGE,  COLUMNWIDTH_MEMUSAGE,  FALSE);

    ConPuts(StdOut, L"\n");

    PrintSplitLine(COLUMNWIDTH_IMAGENAME);
    PrintSpace(1);
    PrintSplitLine(COLUMNWIDTH_PID);
    PrintSpace(1);
    PrintSplitLine(COLUMNWIDTH_SESSION);
    PrintSpace(1);
    PrintSplitLine(COLUMNWIDTH_MEMUSAGE);

    ConPuts(StdOut, L"\n");
}

static LPCWSTR SkipSpaces(LPCWSTR String)
{
    while (*String == L' ' || *String == L'\t')
        String++;
    return String;
}

static LPCWSTR TakeToken(LPCWSTR String, LPWSTR Token, SIZE_T TokenLength)
{
    SIZE_T Length = 0;

    String = SkipSpaces(String);
    while (*String && *String != L' ' && *String != L'\t')
    {
        if (Length + 1 >= TokenLength)
            return NULL;
        Token[Length++] = *String++;
    }
    Token[Length] = UNICODE_NULL;
    return Length ? String : NULL;
}

static BOOL ParseFilter(LPCWSTR String, PTASK_FILTER Filter)
{
    static const struct { LPCWSTR Name; FILTER_FIELD Field; BOOL Numeric; } Fields[] =
    {
        { L"IMAGENAME", FilterImageName, FALSE },
        { L"PID", FilterPid, TRUE },
        { L"SESSION", FilterSession, TRUE },
        { L"MEMUSAGE", FilterMemUsage, TRUE },
    };
    static const struct { LPCWSTR Name; FILTER_OPERATOR Operator; } Operators[] =
    {
        { L"eq", FilterEqual },
        { L"ne", FilterNotEqual },
        { L"gt", FilterGreater },
        { L"lt", FilterLess },
        { L"ge", FilterGreaterEqual },
        { L"le", FilterLessEqual },
    };
    WCHAR Name[32], Operator[8];
    SIZE_T Length, i;
    BOOL Numeric = FALSE, Found = FALSE;

    String = TakeToken(String, Name, _countof(Name));
    if (!String)
        return FALSE;
    String = TakeToken(String, Operator, _countof(Operator));
    if (!String)
        return FALSE;

    for (i = 0; i < _countof(Fields); i++)
    {
        if (!_wcsicmp(Name, Fields[i].Name))
        {
            Filter->Field = Fields[i].Field;
            Numeric = Fields[i].Numeric;
            Found = TRUE;
            break;
        }
    }
    if (!Found)
        return FALSE;

    Found = FALSE;
    for (i = 0; i < _countof(Operators); i++)
    {
        if (!_wcsicmp(Operator, Operators[i].Name))
        {
            Filter->Operator = Operators[i].Operator;
            Found = TRUE;
            break;
        }
    }
    if (!Found)
        return FALSE;
    if (!Numeric && Filter->Operator != FilterEqual && Filter->Operator != FilterNotEqual)
        return FALSE;

    String = SkipSpaces(String);
    Length = wcslen(String);
    while (Length && (String[Length - 1] == L' ' || String[Length - 1] == L'\t'))
        Length--;
    if (!Length || Length >= _countof(Filter->Text))
        return FALSE;
    RtlCopyMemory(Filter->Text, String, Length * sizeof(WCHAR));
    Filter->Text[Length] = UNICODE_NULL;

    if (Numeric)
    {
        Filter->Number = 0;
        for (i = 0; i < Length; i++)
        {
            if (Filter->Text[i] < L'0' || Filter->Text[i] > L'9')
                return FALSE;
            if (Filter->Number > (MAXULONGLONG - 9) / 10)
                return FALSE;
            Filter->Number = Filter->Number * 10 + (Filter->Text[i] - L'0');
        }
        Filter->Prefix = FALSE;
    }
    else
    {
        Filter->Prefix = (Filter->Text[Length - 1] == L'*');
        if (Filter->Prefix)
            Filter->Text[Length - 1] = UNICODE_NULL;
        if (wcschr(Filter->Text, L'*'))
            return FALSE;
    }
    return TRUE;
}

static BOOL CompareNumber(ULONGLONG Value, PTASK_FILTER Filter)
{
    switch (Filter->Operator)
    {
        case FilterEqual:        return Value == Filter->Number;
        case FilterNotEqual:     return Value != Filter->Number;
        case FilterGreater:      return Value > Filter->Number;
        case FilterLess:         return Value < Filter->Number;
        case FilterGreaterEqual: return Value >= Filter->Number;
        case FilterLessEqual:    return Value <= Filter->Number;
    }
    return FALSE;
}

static BOOL MatchFilters(PSYSTEM_PROCESS_INFORMATION pSPI, LPCWSTR ImageName, PTASK_FILTER Filters, UINT FilterCount)
{
    for (UINT i = 0; i < FilterCount; i++)
    {
        PTASK_FILTER Filter = &Filters[i];
        BOOL Match;

        switch (Filter->Field)
        {
            case FilterImageName:
                Match = Filter->Prefix ? !_wcsnicmp(ImageName, Filter->Text, wcslen(Filter->Text))
                                       : !_wcsicmp(ImageName, Filter->Text);
                if (Filter->Operator == FilterNotEqual)
                    Match = !Match;
                break;
            case FilterPid:
                Match = CompareNumber((ULONG_PTR)pSPI->UniqueProcessId, Filter);
                break;
            case FilterSession:
                Match = CompareNumber(pSPI->SessionId, Filter);
                break;
            case FilterMemUsage:
                Match = CompareNumber(pSPI->WorkingSetSize >> 10, Filter);
                break;
            default:
                Match = FALSE;
                break;
        }
        if (!Match)
            return FALSE;
    }
    return TRUE;
}

BOOL EnumProcessAndPrint(BOOL bNoHeader, PTASK_FILTER Filters, UINT FilterCount)
{
    // Call NtQuerySystemInformation for the process information
    ULONG ProcessInfoBufferLength = 0;
    ULONG ResultLength = 0;
    PBYTE ProcessInfoBuffer = NULL;

    // Get the buffer size we need
    NTSTATUS Status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &ResultLength);

    // New process/thread might appear before we call for the actual data.
    // Try to avoid this by retrying several times.
    for (UINT Retry = 0; Retry < NT_SYSTEM_QUERY_MAX_RETRY; Retry++)
    {
        // (Re)allocate buffer
        ProcessInfoBufferLength = ResultLength;
        ResultLength = 0;
        if (ProcessInfoBuffer)
        {
            PBYTE NewProcessInfoBuffer = HeapReAlloc(GetProcessHeap(), 0,
                                                     ProcessInfoBuffer,
                                                     ProcessInfoBufferLength);
            if (NewProcessInfoBuffer)
            {
                ProcessInfoBuffer = NewProcessInfoBuffer;
            }
            else
            {
                // out of memory
                ConResMsgPrintf(StdErr, 0, IDS_OUT_OF_MEMORY);
                HeapFree(GetProcessHeap(), 0, ProcessInfoBuffer);
                return FALSE;
            }
        }
        else
        {
            ProcessInfoBuffer = HeapAlloc(GetProcessHeap(), 0, ProcessInfoBufferLength);
            if (!ProcessInfoBuffer)
            {
                // out of memory
                ConResMsgPrintf(StdErr, 0, IDS_OUT_OF_MEMORY);
                return FALSE;
            }
        }

        // Query information
        Status = NtQuerySystemInformation(SystemProcessInformation,
                                          ProcessInfoBuffer,
                                          ProcessInfoBufferLength,
                                          &ResultLength);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
        {
            break;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        // tried NT_SYSTEM_QUERY_MAX_RETRY times, or failed with some other reason
        ConResMsgPrintf(StdErr, 0, IDS_ENUM_FAILED);
        HeapFree(GetProcessHeap(), 0, ProcessInfoBuffer);
        return FALSE;
    }

    BOOL bPrinted = FALSE;
    PSYSTEM_PROCESS_INFORMATION pSPI;
    pSPI = (PSYSTEM_PROCESS_INFORMATION)ProcessInfoBuffer;
    while (pSPI)
    {
        LPCWSTR ImageName = pSPI->UniqueProcessId ? pSPI->ImageName.Buffer : L"System Idle Process";

        if (!MatchFilters(pSPI, ImageName ? ImageName : L"", Filters, FilterCount))
        {
            if (pSPI->NextEntryOffset == 0)
                break;
            pSPI = (PSYSTEM_PROCESS_INFORMATION)((LPBYTE)pSPI + pSPI->NextEntryOffset);
            continue;
        }

        if (!bPrinted)
        {
            ConPuts(StdOut, L"\n");
            if (!bNoHeader)
                PrintHeader();
            bPrinted = TRUE;
        }

        PrintString(ImageName, COLUMNWIDTH_IMAGENAME, TRUE);
        PrintSpace(1);
        PrintNum((LONGLONG)(INT_PTR)pSPI->UniqueProcessId, COLUMNWIDTH_PID);
        PrintSpace(1);
        PrintNum((ULONGLONG)pSPI->SessionId, COLUMNWIDTH_SESSION);
        PrintSpace(1);
        PrintMemory(pSPI->WorkingSetSize, COLUMNWIDTH_MEMUSAGE);

        ConPuts(StdOut, L"\n");

        if (pSPI->NextEntryOffset == 0)
            break;
        pSPI = (PSYSTEM_PROCESS_INFORMATION)((LPBYTE)pSPI + pSPI->NextEntryOffset);
    }

    if (!bPrinted)
        ConResMsgPrintf(StdOut, 0, IDS_NO_MATCHING_TASKS);

    HeapFree(GetProcessHeap(), 0, ProcessInfoBuffer);
    return TRUE;
}

INT GetOptionType(LPCWSTR szOption)
{
    if (szOption[0] != L'/' && szOption[0] != L'-')
    {
        return OPTION_INVALID;
    }
    szOption++;

    for (UINT i = 0; i < _countof(OptionList); i++)
    {
        if (!_wcsicmp(OptionList[i], szOption))
        {
            return i;
        }
    }
    return OPTION_INVALID;
}

BOOL ProcessArguments(INT argc, WCHAR **argv)
{
    BOOL bHasHelp = FALSE, bHasNoHeader = FALSE, bResult;
    PTASK_FILTER Filters;
    UINT FilterCount = 0;

    Filters = HeapAlloc(GetProcessHeap(), 0, max(argc, 1) * sizeof(*Filters));
    if (!Filters)
    {
        ConResMsgPrintf(StdErr, 0, IDS_OUT_OF_MEMORY);
        return FALSE;
    }

    for (INT i = 1; i < argc; i++)
    {
        INT Option = GetOptionType(argv[i]);

        switch (Option)
        {
            case OPTION_HELP:
            {
                if (bHasHelp)
                {
                    // -? already specified
                    ConResMsgPrintf(StdErr, 0, IDS_OPTION_TOO_MUCH, argv[i], 1);
                    ConResMsgPrintf(StdErr, 0, IDS_USAGE);
                    return FALSE;
                }
                bHasHelp = TRUE;
                break;
            }
            case OPTION_NOHEADER:
            {
                if (bHasNoHeader)
                {
                    // -nh already specified
                    ConResMsgPrintf(StdErr, 0, IDS_OPTION_TOO_MUCH, argv[i], 1);
                    ConResMsgPrintf(StdErr, 0, IDS_USAGE);
                    return FALSE;
                }
                bHasNoHeader = TRUE;
                break;
            }
            case OPTION_FILTER:
            {
                if (i + 1 >= argc)
                {
                    ConResMsgPrintf(StdErr, 0, IDS_VALUE_EXPECTED, argv[i]);
                    ConResMsgPrintf(StdErr, 0, IDS_TYPE_HELP);
                    HeapFree(GetProcessHeap(), 0, Filters);
                    return FALSE;
                }
                if (!ParseFilter(argv[++i], &Filters[FilterCount]))
                {
                    ConResMsgPrintf(StdErr, 0, IDS_FILTER_INVALID);
                    HeapFree(GetProcessHeap(), 0, Filters);
                    return FALSE;
                }
                FilterCount++;
                break;
            }
            case OPTION_INVALID:
            default:
            {
                ConResMsgPrintf(StdErr, 0, IDS_INVALID_OPTION);
                ConResMsgPrintf(StdErr, 0, IDS_USAGE);
                HeapFree(GetProcessHeap(), 0, Filters);
                return FALSE;
            }
        }
    }

    if (bHasHelp)
    {
        if (argc > 2) // any arguments other than -? exists
        {
            ConResMsgPrintf(StdErr, 0, IDS_INVALID_SYNTAX);
            ConResMsgPrintf(StdErr, 0, IDS_USAGE);
        }
        else
        {
            ConResMsgPrintf(StdOut, 0, IDS_USAGE);
            ConResMsgPrintf(StdOut, 0, IDS_DESCRIPTION);
        }
        bResult = FALSE;
    }
    else
    {
        EnumProcessAndPrint(bHasNoHeader, Filters, FilterCount);
        bResult = TRUE;
    }
    HeapFree(GetProcessHeap(), 0, Filters);
    return bResult;
}

int wmain(int argc, WCHAR **argv)
{
    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    if (!ProcessArguments(argc, argv))
    {
        return 1;
    }
    return 0;
}
