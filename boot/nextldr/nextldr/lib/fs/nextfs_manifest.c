/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextldr/lib/fs/nextfs_manifest.c
 * PURPOSE:     BOOT.MAN parser
 * LICENSE:     GPL-3.0-or-later
 *
 * No libc, no allocation: the buffer is walked once, and every string that ends
 * up in the result is a copy, because the caller's buffer is usually the boot
 * loader's arena and gets reused for the next file it reads.
 */

#include <fs/nextfs_manifest.h>

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

static u32 NfmanLength(const char *Text)
{
    u32 Length = 0;

    while (Text[Length] != 0)
        Length++;

    return Length;
}

static int NfmanIsSpace(char Character)
{
    return Character == ' ' || Character == '\t';
}

static char NfmanLower(char Character)
{
    if (Character >= 'A' && Character <= 'Z')
        return (char)(Character - 'A' + 'a');
    return Character;
}

/* Case-insensitive comparison of a counted token against a terminated word. */
static int NfmanTokenIs(const char *Token, u32 TokenLength, const char *Word)
{
    u32 Index = 0;

    for (; Index < TokenLength; Index++)
    {
        if (Word[Index] == 0 || NfmanLower(Token[Index]) != NfmanLower(Word[Index]))
            return 0;
    }

    return Word[Index] == 0;
}

static void NfmanCopyLower(char *Destination, u32 DestinationSize, const char *Source, u32 Length)
{
    u32 Index;

    if (Length >= DestinationSize)
        Length = DestinationSize - 1;

    for (Index = 0; Index < Length; Index++)
        Destination[Index] = NfmanLower(Source[Index]);
    Destination[Length] = 0;
}

static int NfmanHexDigit(char Character)
{
    if (Character >= '0' && Character <= '9')
        return Character - '0';
    if (Character >= 'a' && Character <= 'f')
        return Character - 'a' + 10;
    if (Character >= 'A' && Character <= 'F')
        return Character - 'A' + 10;
    return -1;
}

/* Decimal, or hex after a 0x. A nonsense number loses that number only; the
   count of skipped lines is what tells the person at the screen. */
static int NfmanNumber(const char *Text, u32 Length, u64 *Value)
{
    u32 Index = 0;
    u64 Result = 0;
    int Digit, Base = 10;

    if (Length == 0)
        return 0;

    if (Length > 2 && Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X'))
    {
        Base = 16;
        Index = 2;
    }

    for (; Index < Length; Index++)
    {
        Digit = NfmanHexDigit(Text[Index]);
        if (Digit < 0 || Digit >= Base)
            return 0;
        Result = Result * (u64)Base + (u64)Digit;
    }

    *Value = Result;
    return 1;
}

/* Hash values are written as 8 hex digits, with or without a 0x: nobody writes
   a checksum in decimal, and 1f0c3a95 is not a number in any other base. */
static int NfmanHashValue(const char *Text, u32 Length, u64 *Value)
{
    u32 Index = 0;
    u64 Result = 0;
    int Digit;

    if (Length > 2 && Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X'))
        Index = 2;

    if (Index >= Length)
        return 0;

    for (; Index < Length; Index++)
    {
        Digit = NfmanHexDigit(Text[Index]);
        if (Digit < 0)
            return 0;
        Result = (Result << 4) | (u64)Digit;
    }

    *Value = Result;
    return 1;
}

unsigned int NfmanFnv1a(unsigned int Seed, const void *Data, unsigned int Length)
{
    const u8 *Bytes = (const u8 *)Data;
    unsigned int Hash = Seed;
    unsigned int Index;

    for (Index = 0; Index < Length; Index++)
    {
        Hash ^= Bytes[Index];
        Hash *= 16777619u;
    }

    return Hash;
}

/* "role = path [key=value ...]", already trimmed, inside Section. */
static void NfmanParseEntry(NFMAN *Manifest, const char *Section, const char *Line, u32 Length)
{
    u32 Index = 0, RoleEnd, PathStart, PathEnd, Copy;
    NFMAN_ENTRY *Entry;

    /* the role is one word, and may be glued to the '=' */
    while (Index < Length && !NfmanIsSpace(Line[Index]) && Line[Index] != '=')
        Index++;
    RoleEnd = Index;

    while (Index < Length && NfmanIsSpace(Line[Index]))
        Index++;
    if (RoleEnd == 0 || Index >= Length || Line[Index] != '=')
    {
        Manifest->Skipped++;
        return;
    }
    Index++;

    while (Index < Length && NfmanIsSpace(Line[Index]))
        Index++;

    /* the path is one word, or a quoted string when it has blanks in it */
    if (Index < Length && Line[Index] == '"')
    {
        Index++;
        PathStart = Index;
        while (Index < Length && Line[Index] != '"')
            Index++;
        PathEnd = Index;
        if (Index < Length)
            Index++;
    }
    else
    {
        PathStart = Index;
        while (Index < Length && !NfmanIsSpace(Line[Index]))
            Index++;
        PathEnd = Index;
    }

    if (PathEnd <= PathStart || (PathEnd - PathStart) >= NFMAN_PATH_MAX)
    {
        Manifest->Skipped++;
        return;
    }

    if (Manifest->Entries >= NFMAN_MAX_ENTRIES)
    {
        Manifest->Skipped++;
        return;
    }

    Entry = &Manifest->Table[Manifest->Entries++];
    NfmanCopyLower(Entry->Section, NFMAN_SECTION_MAX, Section, NfmanLength(Section));
    NfmanCopyLower(Entry->Role, NFMAN_ROLE_MAX, Line, RoleEnd);
    for (Copy = 0; Copy < PathEnd - PathStart; Copy++)
        Entry->Path[Copy] = Line[PathStart + Copy];
    Entry->Path[Copy] = 0;
    Entry->Size = 0;
    Entry->Hash = 0;
    Entry->Attributes = 0;

    while (Index < Length)
    {
        u32 KeyStart, KeyEnd, ValueStart, ValueEnd;
        u64 Number;

        while (Index < Length && NfmanIsSpace(Line[Index]))
            Index++;
        if (Index >= Length)
            break;

        KeyStart = Index;
        while (Index < Length && Line[Index] != '=' && !NfmanIsSpace(Line[Index]))
            Index++;
        KeyEnd = Index;

        if (Index >= Length || Line[Index] != '=')
        {
            /* a bare word: not something this loader knows, keep reading */
            continue;
        }

        Index++;
        ValueStart = Index;
        while (Index < Length && !NfmanIsSpace(Line[Index]))
            Index++;
        ValueEnd = Index;

        if (NfmanTokenIs(Line + KeyStart, KeyEnd - KeyStart, "hash") &&
            NfmanHashValue(Line + ValueStart, ValueEnd - ValueStart, &Number))
        {
            Entry->Hash = (unsigned int)Number;
            Entry->Attributes |= NFMAN_ATTR_HASH_STATED;
        }
        else if (NfmanTokenIs(Line + KeyStart, KeyEnd - KeyStart, "size") &&
                 NfmanNumber(Line + ValueStart, ValueEnd - ValueStart, &Number))
        {
            Entry->Size = Number;
            Entry->Attributes |= NFMAN_ATTR_SIZE_STATED;
        }
    }
}

/* "word value" at the top of the file. */
static int NfmanDirective(const char *Line, u32 Length, const char *Word, u32 *ValueStart)
{
    u32 Index = 0;

    while (Index < Length && !NfmanIsSpace(Line[Index]))
        Index++;
    if (!NfmanTokenIs(Line, Index, Word))
        return 0;

    while (Index < Length && NfmanIsSpace(Line[Index]))
        Index++;
    *ValueStart = Index;
    return 1;
}

int NfmanParse(NFMAN *Manifest, const char *Text, unsigned int Length)
{
    u32 Index = 0;
    char Section[NFMAN_SECTION_MAX];
    int SawDirective = 0;

    Manifest->Version = 0;
    Manifest->Timeout = 0;
    Manifest->Entries = 0;
    Manifest->Skipped = 0;
    Manifest->DefaultOs[0] = 0;
    Section[0] = 0;

    /* a byte order mark is not a comment, and a file saved on Windows has one */
    if (Length >= 3 && (u8)Text[0] == 0xEF && (u8)Text[1] == 0xBB && (u8)Text[2] == 0xBF)
        Index = 3;

    while (Index < Length)
    {
        u32 Start, End, ValueStart;

        Start = Index;
        while (Index < Length && Text[Index] != '\n' && Text[Index] != '\r')
            Index++;
        End = Index;
        while (Index < Length && (Text[Index] == '\n' || Text[Index] == '\r'))
            Index++;

        while (Start < End && NfmanIsSpace(Text[Start]))
            Start++;
        while (End > Start && NfmanIsSpace(Text[End - 1]))
            End--;

        if (End == Start || Text[Start] == '#')
            continue;

        if (Text[Start] == '[')
        {
            u32 Close = Start + 1, WordEnd, NameStart;

            while (Close < End && Text[Close] != ']')
                Close++;

            WordEnd = Start + 1;
            while (WordEnd < Close && !NfmanIsSpace(Text[WordEnd]))
                WordEnd++;
            NameStart = WordEnd;
            while (NameStart < Close && NfmanIsSpace(Text[NameStart]))
                NameStart++;

            if (NfmanTokenIs(Text + Start + 1, WordEnd - (Start + 1), "os") && NameStart < Close)
            {
                NfmanCopyLower(Section, NFMAN_SECTION_MAX, Text + NameStart, Close - NameStart);
            }
            else
            {
                /* an unknown kind of section: its lines are somebody else's */
                Section[0] = 0;
                Manifest->Skipped++;
            }
            continue;
        }

        if (Section[0] == 0)
        {
            u64 Number;

            if (NfmanDirective(Text + Start, End - Start, "version", &ValueStart))
            {
                if (NfmanNumber(Text + Start + ValueStart, End - Start - ValueStart, &Number))
                    Manifest->Version = (unsigned int)Number;
                else
                    Manifest->Skipped++;
                SawDirective = 1;
            }
            else if (NfmanDirective(Text + Start, End - Start, "default", &ValueStart))
            {
                NfmanCopyLower(Manifest->DefaultOs, NFMAN_SECTION_MAX,
                               Text + Start + ValueStart, End - Start - ValueStart);
                SawDirective = 1;
            }
            else if (NfmanDirective(Text + Start, End - Start, "timeout", &ValueStart))
            {
                if (NfmanNumber(Text + Start + ValueStart, End - Start - ValueStart, &Number))
                    Manifest->Timeout = (unsigned int)Number;
                else
                    Manifest->Skipped++;
                SawDirective = 1;
            }
            else
            {
                Manifest->Skipped++;
            }
            continue;
        }

        NfmanParseEntry(Manifest, Section, Text + Start, End - Start);
    }

    /* "saw a directive" separates a manifest from a text file somebody dropped
       on the volume: the caller falls back to the old behaviour on zero. */
    return SawDirective || Manifest->Entries != 0;
}

const NFMAN_ENTRY *NfmanFind(const NFMAN *Manifest, const char *Section, const char *Role)
{
    const char *Wanted = Section;
    u32 Index;

    if (Wanted == 0 || Wanted[0] == 0)
        Wanted = Manifest->DefaultOs;

    for (Index = 0; Index < Manifest->Entries; Index++)
    {
        const NFMAN_ENTRY *Entry = &Manifest->Table[Index];

        if (!NfmanTokenIs(Entry->Role, NfmanLength(Entry->Role), Role))
            continue;

        /* With no section named anywhere, the first entry with that role wins:
           a single-OS manifest should not have to name itself twice. */
        if (Wanted[0] == 0 || NfmanTokenIs(Entry->Section, NfmanLength(Entry->Section), Wanted))
            return Entry;
    }

    return 0;
}
