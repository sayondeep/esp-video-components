/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file hls_gen.h
 * @brief HLS m3u8 playlist generator for CMAF fMP4 live ingest.
 *
 * Generates two complementary playlists that reference the same fMP4 segments
 * as the DASH MPD — no extra data is uploaded:
 *
 *   master.m3u8    Top-level playlist with stream variant info.
 *                  PUT once at session start.
 *
 *   playlist.m3u8  Media playlist, re-PUT after each segment with a
 *                  sliding window of the last N segments.  A final PUT
 *                  with is_final=true appends EXT-X-ENDLIST.
 *
 * Playlist structure (HLS version 7, CMAF fMP4 with EXT-X-MAP):
 *
 *   #EXTM3U
 *   #EXT-X-VERSION:7
 *   #EXT-X-INDEPENDENT-SEGMENTS
 *   #EXT-X-TARGETDURATION:1
 *   #EXT-X-MEDIA-SEQUENCE:1001
 *   #EXT-X-MAP:URI="video1.init"
 *   #EXTINF:1.000,
 *   segment_1001.m4s
 *   #EXTINF:1.000,
 *   segment_1002.m4s
 *   ...
 *
 * All URIs are relative to the playlist location so they resolve correctly
 * regardless of the server base URL.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parameters for the HLS master playlist.
 */
typedef struct {
    const char *track_name; /**< Track directory name, e.g. "video1".
                             *   The media playlist URI becomes
                             *   "{track_name}/playlist.m3u8". */
    uint32_t    bandwidth;  /**< Peak bitrate in bps.
                             *   Required by HLS spec (EXT-X-STREAM-INF). */
} hls_master_params_t;

/**
 * @brief Parameters for the HLS media playlist (live sliding window).
 *
 * @p seg_ids contains the segment identifiers for the segments currently
 * in the live window.  For @c $Number$ naming these are sequential numbers
 * (e.g. 1001, 1002 …); for @c $Time$ naming these are presentation
 * timestamps in timescale ticks (e.g. 0, 90000, 180000 …).
 *
 * In both cases the segment filename is:
 *   @c segment_{seg_id}.m4s
 */
typedef struct {
    const char    *track_name;     /**< Used for EXT-X-MAP and segment URIs */
    uint32_t       media_sequence; /**< EXT-X-MEDIA-SEQUENCE value.
                                    *   For $Number$: set to seg_ids[0].
                                    *   For $Time$: set to the count of
                                    *   segments dropped from the window. */
    float          seg_duration_s; /**< Duration of each segment in seconds.
                                    *   Used for EXTINF and TARGETDURATION. */
    const uint32_t *seg_ids;       /**< Array of segment IDs in display order
                                    *   (oldest first). */
    uint32_t        num_segs;      /**< Valid entries in @p seg_ids. */
    bool            is_final;      /**< Append #EXT-X-ENDLIST when true. */
} hls_media_params_t;

/**
 * @brief Write an HLS master playlist into @p buf.
 *
 * @param buf       Destination buffer (will be NUL-terminated).
 * @param buf_size  Size of @p buf in bytes (256 B is sufficient).
 * @param p         Master playlist parameters.
 * @param out_len   Set to the number of bytes written (excluding NUL).
 * @return ESP_OK, or ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM on error.
 */
esp_err_t hls_gen_master(char *buf, size_t buf_size,
                          const hls_master_params_t *p, size_t *out_len);

/**
 * @brief Write an HLS media playlist into @p buf.
 *
 * @param buf       Destination buffer (1 KB is sufficient for ≤10 segments).
 * @param buf_size  Size of @p buf in bytes.
 * @param p         Media playlist parameters.
 * @param out_len   Set to the number of bytes written (excluding NUL).
 * @return ESP_OK, or ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM on error.
 */
esp_err_t hls_gen_media(char *buf, size_t buf_size,
                         const hls_media_params_t *p, size_t *out_len);

#ifdef __cplusplus
}
#endif
