/*
 * PROJECT:     NextNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Debug print macros for lxcore code built on the host
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

void LxHostLog(const char *Format, ...);

#define DPRINT1(...) LxHostLog(__VA_ARGS__)
#ifdef NDEBUG
#define DPRINT(...) do { } while (0)
#else
#define DPRINT(...) LxHostLog(__VA_ARGS__)
#endif
