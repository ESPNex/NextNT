/*
 * PROJECT:     NextNT Media Foundation decoders
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Loads the FFmpeg libraries the H.264 and AAC decoders run on
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "gst_private.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include <ffmpeg_runtime.h>

#define FFMPEG_IMPORTS_DEFINE
#include "ffmpeg_imports.h"

#define FFMPEG_IMPORT(name) __typeof__(name) *p_##name;
MSMPEG2VDEC_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT

static INIT_ONCE FFmpegOnce = INIT_ONCE_STATIC_INIT;
static BOOL FFmpegLoaded;

static BOOL CALLBACK
LoadFFmpeg(
    _Inout_ PINIT_ONCE InitOnce,
    _Inout_opt_ PVOID Parameter,
    _Out_opt_ PVOID *Context)
{
    static PCWSTR const Libraries[] =
    {
        FFMPEG_RUNTIME_LIBRARY("avutil", LIBAVUTIL_VERSION_MAJOR),
        FFMPEG_RUNTIME_LIBRARY("swresample", LIBSWRESAMPLE_VERSION_MAJOR),
        FFMPEG_RUNTIME_LIBRARY("avcodec", LIBAVCODEC_VERSION_MAJOR),
    };
    static const FFMPEG_RUNTIME_IMPORT Imports[] =
    {
#define FFMPEG_IMPORT(name) { #name, (PVOID *)&p_##name },
        MSMPEG2VDEC_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT
    };

    UNREFERENCED_PARAMETER(InitOnce);
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    FFmpegLoaded = FFmpegRuntimeBind(Libraries, ARRAYSIZE(Libraries), Imports, ARRAYSIZE(Imports));
    return TRUE;
}

BOOL
msmpeg2vdec_load_ffmpeg(VOID)
{
    InitOnceExecuteOnce(&FFmpegOnce, LoadFFmpeg, NULL, NULL);
    return FFmpegLoaded;
}
