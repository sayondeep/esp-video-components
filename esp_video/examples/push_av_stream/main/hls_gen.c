/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file hls_gen.c
 * @brief HLS m3u8 playlist generator for CMAF fMP4 live ingest.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_err.h"
#include "hls_gen.h"

/* =========================================================================
 * Master playlist
 * ========================================================================= */

esp_err_t hls_gen_master(char *buf, size_t buf_size,
                          const hls_master_params_t *p, size_t *out_len)
{
    if (!buf || !p || !out_len || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The media playlist lives in a sub-directory named after the track,
     * matching the segment layout used for DASH:
     *   {track_name}/playlist.m3u8  */
    int n = snprintf(buf, buf_size,
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-INDEPENDENT-SEGMENTS\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=%"PRIu32"\n"
        "%s/playlist.m3u8\n",
        p->bandwidth,
        p->track_name);

    if (n < 0 || (size_t)n >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    *out_len = (size_t)n;
    return ESP_OK;
}

/* =========================================================================
 * Media playlist
 * ========================================================================= */

esp_err_t hls_gen_media(char *buf, size_t buf_size,
                         const hls_media_params_t *p, size_t *out_len)
{
    if (!buf || !p || !out_len || buf_size == 0 ||
        !p->seg_ids || p->num_segs == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* EXT-X-TARGETDURATION must be an integer >= the max segment duration.
     * Compute ceiling without relying on <math.h> / libm. */
    uint32_t target_dur = (uint32_t)p->seg_duration_s;
    if ((float)target_dur < p->seg_duration_s) {
        target_dur++;
    }
    if (target_dur == 0) {
        target_dur = 1;
    }

    /* Header: version tags, timing attributes, init segment reference.
     *
     * EXT-X-MAP points to the fMP4 init segment (same directory as this
     * playlist), matching the DASH initialization="{track}/{track}.init"
     * template. */
    int n = snprintf(buf, buf_size,
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-INDEPENDENT-SEGMENTS\n"
        "#EXT-X-TARGETDURATION:%"PRIu32"\n"
        "#EXT-X-MEDIA-SEQUENCE:%"PRIu32"\n"
        "#EXT-X-MAP:URI=\"%s.init\"\n",
        target_dur,
        p->media_sequence,
        p->track_name);

    if (n < 0 || (size_t)n >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    size_t pos = (size_t)n;

    /* One entry per segment in the sliding window.
     *
     * Segment filename format: segment_{id}.m4s
     *   $Number$ mode — id is the sequence number (1001, 1002 …)
     *   $Time$   mode — id is the presentation timestamp (0, 90000 …)
     *
     * Both cases produce "segment_NNNN.m4s", consistent with what the
     * nagare adapter PUTs and what the DASH SegmentTemplate references. */
    for (uint32_t i = 0; i < p->num_segs; i++) {
        int seg_n = snprintf(buf + pos, buf_size - pos,
            "#EXTINF:%.3f,\n"
            "segment_%"PRIu32".m4s\n",
            (double)p->seg_duration_s,
            p->seg_ids[i]);

        if (seg_n < 0 || pos + (size_t)seg_n >= buf_size) {
            return ESP_ERR_NO_MEM;
        }
        pos += (size_t)seg_n;
    }

    /* Optional end-of-stream marker (for recordings / static sessions). */
    if (p->is_final) {
        int end_n = snprintf(buf + pos, buf_size - pos, "#EXT-X-ENDLIST\n");
        if (end_n < 0 || pos + (size_t)end_n >= buf_size) {
            return ESP_ERR_NO_MEM;
        }
        pos += (size_t)end_n;
    }

    *out_len = pos;
    return ESP_OK;
}
