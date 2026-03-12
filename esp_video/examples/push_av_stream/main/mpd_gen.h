/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file mpd_gen.h
 * @brief Minimal DASH MPD XML generator for CMAF ingest.
 *
 * The push_av_server (Interface-2 DASH) requires two MPD uploads per session:
 *  1. A dynamic MPD before any segments are sent (creates the session).
 *  2. A static  MPD after all segments are sent  (closes the session).
 *
 * The server only inspects the `type` attribute ('dynamic' / 'static') and
 * the path (`session_X/index.mpd`); other MPD content is for player use.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parameters used to build both dynamic and static MPDs.
 */
typedef struct {
    uint32_t    width;          /**< Video frame width  (pixels) */
    uint32_t    height;         /**< Video frame height (pixels) */
    uint32_t    framerate;      /**< Nominal frame rate (fps) */
    uint32_t    timescale;      /**< Media timescale (e.g. 90000) */
    uint32_t    seg_duration;   /**< Segment duration in timescale ticks */
    uint32_t    start_number;   /**< First segment number (spec: 1001) */
    const char *track_name;     /**< Track / Representation id, e.g. "video1" */
    const char *codecs;         /**< DASH codecs string, e.g. "avc1.4D001F" */
    uint32_t    bandwidth;      /**< Approximate bitrate in bps */
    time_t      availability_start_time; /**< UTC epoch when stream starts (for availabilityStartTime) */
} mpd_params_t;

/**
 * @brief Write a DASH dynamic MPD into @p buf.
 *
 * @param buf       Destination character buffer (will be NUL-terminated).
 * @param buf_size  Size of @p buf in bytes.
 * @param p         MPD parameters.
 * @param out_len   Set to the number of bytes written (excluding NUL).
 * @return ESP_OK or ESP_ERR_NO_MEM if the buffer is too small.
 */
esp_err_t mpd_gen_dynamic(char *buf, size_t buf_size,
                           const mpd_params_t *p, size_t *out_len);

/**
 * @brief Write a DASH static MPD into @p buf.
 *
 * @param buf            Destination buffer.
 * @param buf_size       Size of @p buf.
 * @param p              MPD parameters (same object used for dynamic MPD).
 * @param total_duration Total presentation duration in seconds.
 * @param out_len        Set to the number of bytes written.
 * @return ESP_OK or ESP_ERR_NO_MEM.
 */
esp_err_t mpd_gen_static(char *buf, size_t buf_size,
                          const mpd_params_t *p,
                          uint32_t total_duration_sec,
                          size_t *out_len);

#ifdef __cplusplus
}
#endif
