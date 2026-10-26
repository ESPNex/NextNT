/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Harness interfaces for lxcore memory tests on the NVS host machine
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include "lxcore.h"

#define LX_HOST_USER_LIMIT      0x100000000ULL
#define LX_HOST_OBJECT_FILE     1
#define LX_HOST_OBJECT_SECTION  2

struct _EPROCESS_HOST
{
    PMI_ADDRESS_SPACE Space;
};

typedef struct _LX_HOST_FILE
{
    FILE_OBJECT Object;
    PVOID Pointers[3];
    TEST_FILE *Data;
    PMI_SEGMENT DataSegment;
} LX_HOST_FILE, *PLX_HOST_FILE;

typedef struct _LX_HOST_SECTION
{
    LX_HOST_OBJECT Header;
    PMI_SEGMENT Segment;
    ULONG Protection;
    ULONG64 Size;
    PLX_HOST_FILE File;
} LX_HOST_SECTION, *PLX_HOST_SECTION;

extern TEST_WORLD *LxHostWorld;
extern _Thread_local PMI_ADDRESS_SPACE LxHostSpace;
extern _Thread_local PLX_THREAD LxHostThread;
extern _Thread_local ULONG64 LxHostQueries;
extern _Thread_local ULONG64 LxHostCommittedScan;
extern ULONG LxHostLogged;
extern BOOLEAN LxHostQuiet;

HANDLE LxHostCreateFile(PLX_HOST_FILE File, TEST_FILE *Data);
