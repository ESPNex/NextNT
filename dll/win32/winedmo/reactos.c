/*
 * PROJECT:     LiberNT Wine DMO compatibility layer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Native FFmpeg adapter for Wine's winedmo demuxer
 * COPYRIGHT:   Copyright 2026 Ahmed Arif <arif.ing@outlook.com>
 */

/* Wine normally uses a Unix-call boundary. ReactOS invokes the same upstream demuxer callbacks in-process. */

#define FFMPEG_IMPORTS_DEFINE
#include "config.h"
#include "unix_private.h"
#include "reactos.h"

#include <ffmpeg_runtime.h>

#include <errno.h>
#include <stdio.h>

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

int64_t unix_seek_callback(void *opaque, int64_t offset, int whence)
{
    struct stream_context *context = opaque;
    struct winedmo_stream *stream = (struct winedmo_stream *)(UINT_PTR)context->stream;
    UINT64 position;
    NTSTATUS status;

    if (whence == AVSEEK_SIZE) return context->length;
    if (whence == SEEK_END) offset += context->length;
    if (whence == SEEK_CUR) offset += context->position;
    if (offset < 0) offset = 0;
    if ((UINT64)offset > context->length) offset = context->length;

    if ((UINT64)offset / context->capacity != context->position / context->capacity)
    {
        if (!stream->p_seek) return AVERROR(EINVAL);
        position = ((UINT64)offset / context->capacity) * context->capacity;
        status = stream->p_seek(stream, &position);
        if (status || position != ((UINT64)offset / context->capacity) * context->capacity) return AVERROR(EINVAL);
        context->size = 0;
    }

    context->position = offset;
    return offset;
}

static int stream_context_read(struct stream_context *context)
{
    struct winedmo_stream *stream = (struct winedmo_stream *)(UINT_PTR)context->stream;
    ULONG size = context->capacity;
    NTSTATUS status;

    if (!stream->p_read) return AVERROR(EINVAL);
    status = stream->p_read(stream, context->buffer, &size);
    if (status) return AVERROR(EINVAL);
    context->size = size;
    return 0;
}

int unix_read_callback(void *opaque, uint8_t *buffer, int size)
{
    struct stream_context *context = opaque;
    int result;
    int total = 0;

    if (!(size = min((UINT64)size, context->length - context->position))) return AVERROR_EOF;

    while (size)
    {
        int buffer_offset = context->position % context->capacity;
        int step;

        if (!context->size && (result = stream_context_read(context)) < 0) return result;
        if (!(step = min(size, context->size - buffer_offset))) break;
        memcpy(buffer, context->buffer + buffer_offset, step);
        buffer += step;
        total += step;
        size -= step;
        context->position += step;
        if (!(context->position % context->capacity)) context->size = 0;
    }

    return total ? total : AVERROR_EOF;
}

static void vlog(void *context, int level, const char *format, va_list args)
{
    enum __wine_debug_class debug_class = __WINE_DBCL_TRACE;

    UNREFERENCED_PARAMETER(context);
    if (level <= AV_LOG_WARNING) debug_class = __WINE_DBCL_WARN;
    if (level <= AV_LOG_ERROR) debug_class = __WINE_DBCL_ERR;
    wine_dbg_vlog(debug_class, __wine_dbch___default, __func__, format, args);
}

#define FFMPEG_IMPORT(name) __typeof__(name) *p_##name;
WINEDMO_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT

int av_strerror(int errnum, char *errbuf, size_t errbuf_size)
{
    return p_av_strerror(errnum, errbuf, errbuf_size);
}

NTSTATUS process_attach(void *params)
{
    UNREFERENCED_PARAMETER(params);
    p_av_log_set_callback(vlog);
    TRACE("FFmpeg avformat version %u, avcodec version %u\n", p_avformat_version(), p_avcodec_version());
    return STATUS_SUCCESS;
}

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
        FFMPEG_RUNTIME_LIBRARY("avcodec", LIBAVCODEC_VERSION_MAJOR),
        FFMPEG_RUNTIME_LIBRARY("avformat", LIBAVFORMAT_VERSION_MAJOR),
    };
    static const FFMPEG_RUNTIME_IMPORT Imports[] =
    {
#define FFMPEG_IMPORT(name) { #name, (PVOID *)&p_##name },
        WINEDMO_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT
    };

    UNREFERENCED_PARAMETER(InitOnce);
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    FFmpegLoaded = FFmpegRuntimeBind(Libraries, ARRAYSIZE(Libraries), Imports, ARRAYSIZE(Imports)) &&
                   NT_SUCCESS(process_attach(NULL));
    return TRUE;
}

BOOL winedmo_load_ffmpeg(VOID)
{
    InitOnceExecuteOnce(&FFmpegOnce, LoadFFmpeg, NULL, NULL);
    return FFmpegLoaded;
}
