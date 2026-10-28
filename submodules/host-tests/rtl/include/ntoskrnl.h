/*
 * PROJECT:     NextNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Minimal kernel definitions for building ntoskrnl/rtl/rbtree.c on the host
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define _In_
#define _In_opt_
#define _Inout_
#ifndef NTAPI
#define NTAPI
#endif
#define TRUE 1
#define FALSE 0

typedef void VOID;
typedef unsigned char UCHAR, BOOLEAN;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;

#include "rtltree.h"
