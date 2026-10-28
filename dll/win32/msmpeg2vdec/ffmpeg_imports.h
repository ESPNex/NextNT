/*
 * PROJECT:     NextNT Media Foundation decoders
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FFmpeg functions used by the H.264 and AAC decoders
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define MSMPEG2VDEC_FFMPEG_IMPORTS(FFMPEG_IMPORT) \
    FFMPEG_IMPORT(av_channel_layout_compare) \
    FFMPEG_IMPORT(av_channel_layout_copy) \
    FFMPEG_IMPORT(av_channel_layout_default) \
    FFMPEG_IMPORT(av_channel_layout_from_mask) \
    FFMPEG_IMPORT(av_channel_layout_uninit) \
    FFMPEG_IMPORT(av_frame_alloc) \
    FFMPEG_IMPORT(av_frame_free) \
    FFMPEG_IMPORT(av_frame_unref) \
    FFMPEG_IMPORT(av_mallocz) \
    FFMPEG_IMPORT(av_new_packet) \
    FFMPEG_IMPORT(av_packet_alloc) \
    FFMPEG_IMPORT(av_packet_free) \
    FFMPEG_IMPORT(avcodec_alloc_context3) \
    FFMPEG_IMPORT(avcodec_find_decoder) \
    FFMPEG_IMPORT(avcodec_flush_buffers) \
    FFMPEG_IMPORT(avcodec_free_context) \
    FFMPEG_IMPORT(avcodec_open2) \
    FFMPEG_IMPORT(avcodec_receive_frame) \
    FFMPEG_IMPORT(avcodec_send_packet) \
    FFMPEG_IMPORT(swr_alloc_set_opts2) \
    FFMPEG_IMPORT(swr_convert) \
    FFMPEG_IMPORT(swr_free) \
    FFMPEG_IMPORT(swr_get_out_samples) \
    FFMPEG_IMPORT(swr_init)

#define FFMPEG_IMPORT(name) extern __typeof__(name) *p_##name;
MSMPEG2VDEC_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT

BOOL msmpeg2vdec_load_ffmpeg(VOID);

#ifndef FFMPEG_IMPORTS_DEFINE
#define av_channel_layout_compare p_av_channel_layout_compare
#define av_channel_layout_copy p_av_channel_layout_copy
#define av_channel_layout_default p_av_channel_layout_default
#define av_channel_layout_from_mask p_av_channel_layout_from_mask
#define av_channel_layout_uninit p_av_channel_layout_uninit
#define av_frame_alloc p_av_frame_alloc
#define av_frame_free p_av_frame_free
#define av_frame_unref p_av_frame_unref
#define av_mallocz p_av_mallocz
#define av_new_packet p_av_new_packet
#define av_packet_alloc p_av_packet_alloc
#define av_packet_free p_av_packet_free
#define avcodec_alloc_context3 p_avcodec_alloc_context3
#define avcodec_find_decoder p_avcodec_find_decoder
#define avcodec_flush_buffers p_avcodec_flush_buffers
#define avcodec_free_context p_avcodec_free_context
#define avcodec_open2 p_avcodec_open2
#define avcodec_receive_frame p_avcodec_receive_frame
#define avcodec_send_packet p_avcodec_send_packet
#define swr_alloc_set_opts2 p_swr_alloc_set_opts2
#define swr_convert p_swr_convert
#define swr_free p_swr_free
#define swr_get_out_samples p_swr_get_out_samples
#define swr_init p_swr_init
#endif
