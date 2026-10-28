/*
 * PROJECT:     LiberNT Media Foundation decoders
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FFmpeg functions used by the native winedmo demuxer
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define WINEDMO_FFMPEG_IMPORTS(FFMPEG_IMPORT) \
    FFMPEG_IMPORT(av_bsf_alloc) \
    FFMPEG_IMPORT(av_bsf_flush) \
    FFMPEG_IMPORT(av_bsf_free) \
    FFMPEG_IMPORT(av_bsf_get_by_name) \
    FFMPEG_IMPORT(av_bsf_get_null_filter) \
    FFMPEG_IMPORT(av_bsf_init) \
    FFMPEG_IMPORT(av_bsf_receive_packet) \
    FFMPEG_IMPORT(av_bsf_send_packet) \
    FFMPEG_IMPORT(av_codec_get_tag) \
    FFMPEG_IMPORT(av_dict_get) \
    FFMPEG_IMPORT(av_find_input_format) \
    FFMPEG_IMPORT(av_get_bits_per_sample) \
    FFMPEG_IMPORT(av_get_pix_fmt_name) \
    FFMPEG_IMPORT(av_log_set_callback) \
    FFMPEG_IMPORT(av_packet_alloc) \
    FFMPEG_IMPORT(av_packet_free) \
    FFMPEG_IMPORT(av_read_frame) \
    FFMPEG_IMPORT(av_rescale_q_rnd) \
    FFMPEG_IMPORT(av_strerror) \
    FFMPEG_IMPORT(avcodec_get_name) \
    FFMPEG_IMPORT(avcodec_parameters_copy) \
    FFMPEG_IMPORT(avcodec_version) \
    FFMPEG_IMPORT(avformat_alloc_context) \
    FFMPEG_IMPORT(avformat_find_stream_info) \
    FFMPEG_IMPORT(avformat_free_context) \
    FFMPEG_IMPORT(avformat_get_mov_audio_tags) \
    FFMPEG_IMPORT(avformat_get_mov_video_tags) \
    FFMPEG_IMPORT(avformat_get_riff_audio_tags) \
    FFMPEG_IMPORT(avformat_get_riff_video_tags) \
    FFMPEG_IMPORT(avformat_open_input) \
    FFMPEG_IMPORT(avformat_seek_file) \
    FFMPEG_IMPORT(avformat_version) \
    FFMPEG_IMPORT(avio_alloc_context) \
    FFMPEG_IMPORT(avio_context_free)

#define FFMPEG_IMPORT(name) extern __typeof__(name) *p_##name;
WINEDMO_FFMPEG_IMPORTS(FFMPEG_IMPORT)
#undef FFMPEG_IMPORT

BOOL winedmo_load_ffmpeg(VOID);

#ifndef FFMPEG_IMPORTS_DEFINE
#define av_bsf_alloc p_av_bsf_alloc
#define av_bsf_flush p_av_bsf_flush
#define av_bsf_free p_av_bsf_free
#define av_bsf_get_by_name p_av_bsf_get_by_name
#define av_bsf_get_null_filter p_av_bsf_get_null_filter
#define av_bsf_init p_av_bsf_init
#define av_bsf_receive_packet p_av_bsf_receive_packet
#define av_bsf_send_packet p_av_bsf_send_packet
#define av_codec_get_tag p_av_codec_get_tag
#define av_dict_get p_av_dict_get
#define av_find_input_format p_av_find_input_format
#define av_get_bits_per_sample p_av_get_bits_per_sample
#define av_get_pix_fmt_name p_av_get_pix_fmt_name
#define av_log_set_callback p_av_log_set_callback
#define av_packet_alloc p_av_packet_alloc
#define av_packet_free p_av_packet_free
#define av_read_frame p_av_read_frame
#define av_rescale_q_rnd p_av_rescale_q_rnd
#define av_strerror p_av_strerror
#define avcodec_get_name p_avcodec_get_name
#define avcodec_parameters_copy p_avcodec_parameters_copy
#define avcodec_version p_avcodec_version
#define avformat_alloc_context p_avformat_alloc_context
#define avformat_find_stream_info p_avformat_find_stream_info
#define avformat_free_context p_avformat_free_context
#define avformat_get_mov_audio_tags p_avformat_get_mov_audio_tags
#define avformat_get_mov_video_tags p_avformat_get_mov_video_tags
#define avformat_get_riff_audio_tags p_avformat_get_riff_audio_tags
#define avformat_get_riff_video_tags p_avformat_get_riff_video_tags
#define avformat_open_input p_avformat_open_input
#define avformat_seek_file p_avformat_seek_file
#define avformat_version p_avformat_version
#define avio_alloc_context p_avio_alloc_context
#define avio_context_free p_avio_context_free
#endif
