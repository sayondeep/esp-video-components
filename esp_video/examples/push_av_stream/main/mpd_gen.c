/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file mpd_gen.c
 * @brief DASH MPD XML generator
 *
 * Produces minimal but standards-conformant MPEG-DASH MPD documents.
 *
 * Dynamic MPD (type="dynamic") — sent before any segments:
 *   - Signals a live/growing presentation.
 *   - Creates a new Session in the push_av_server.
 *
 * Static MPD (type="static") — sent after the last segment:
 *   - Signals that the presentation is complete.
 *   - Closes the Session in the push_av_server.
 *
 * SegmentTemplate paths are relative to the MPD location (session_X/):
 *   initialization: "<track_name>/<track_name>.init"
 *   media:          "<track_name>/segment_$Number$.m4s" (for recordings)
 *                   or "<track_name>/segment_$Time$.m4s" (for live streams)
 *                   (selected via CONFIG_EXAMPLE_SEGMENT_NAMING)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "mpd_gen.h"

/* =========================================================================
 * Dynamic MPD
 * ========================================================================= */

esp_err_t mpd_gen_dynamic(char *buf, size_t buf_size,
                           const mpd_params_t *p, size_t *out_len)
{
    if (!buf || !p || !out_len || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Duration of one segment as an ISO 8601 duration string (PT<N>S) */
    float seg_sec = (float)p->seg_duration / (float)p->timescale;

    /* MPD update period: 1× segment duration gives the player a fresh
     * AST every segment, keeping the live-edge tight. */
    float min_update_sec = seg_sec;

    /* Format availabilityStartTime as ISO 8601 UTC string */
    char ast_str[32];
    struct tm tm_buf;
    gmtime_r(&p->availability_start_time, &tm_buf);
    strftime(ast_str, sizeof(ast_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    /* Build the media attribute with track name and segment naming scheme */
    char media_attr[128];
#if CONFIG_EXAMPLE_SEGMENT_NAMING_TIMESTAMP
    snprintf(media_attr, sizeof(media_attr), "%s/segment_$Time$.m4s", p->track_name);
#else
    snprintf(media_attr, sizeof(media_attr), "%s/segment_$Number$.m4s", p->track_name);
#endif

    int n = snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n"
        "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
        "     xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 "
              "http://standards.iso.org/ittf/PubliclyAvailableStandards/"
              "MPEG-DASH_schema_files/DASH-MPD.xsd\"\n"
        "     type=\"dynamic\"\n"
        "     profiles=\"urn:mpeg:dash:profile:isoff-live:2011,"
              "urn:mpeg:cmaf:2019\"\n"
        "     minBufferTime=\"PT1S\"\n"
        "     suggestedPresentationDelay=\"PT2S\"\n"
        "     timeShiftBufferDepth=\"PT30S\"\n"
        "     availabilityStartTime=\"%s\"\n"
        "     minimumUpdatePeriod=\"PT%.1fS\">\n"
        "  <UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\"\n"
        "             value=\"%s\"/>\n"
        "  <Period id=\"1\" start=\"PT0S\">\n"
        "    <AdaptationSet id=\"1\"\n"
        "                   mimeType=\"video/mp4\"\n"
        "                   codecs=\"%s\"\n"
        "                   width=\"%"PRIu32"\"\n"
        "                   height=\"%"PRIu32"\"\n"
        "                   frameRate=\"%"PRIu32"\"\n"
        "                   sar=\"1:1\">\n"
        "      <SegmentTemplate timescale=\"%"PRIu32"\"\n"
        "                       duration=\"%"PRIu32"\"\n"
        "                       startNumber=\"%"PRIu32"\"\n"
        "                       initialization=\"%s/%s.init\"\n"
        "                       media=\"%s\"/>\n"
        "      <Representation id=\"%s\" bandwidth=\"%"PRIu32"\"/>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n",
        ast_str,
        min_update_sec,
        ast_str,  /* UTCTiming value = same wall-clock time as AST */
        p->codecs,
        p->width, p->height, p->framerate,
        p->timescale,
        p->seg_duration,
        p->start_number,
        p->track_name, p->track_name,
        media_attr,
        p->track_name, p->bandwidth);

    if (n < 0 || (size_t)n >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    *out_len = (size_t)n;
    return ESP_OK;
}

/* =========================================================================
 * Static MPD
 * ========================================================================= */

esp_err_t mpd_gen_static(char *buf, size_t buf_size,
                          const mpd_params_t *p,
                          uint32_t total_duration_sec,
                          size_t *out_len)
{
    if (!buf || !p || !out_len || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build the media attribute with track name and segment naming scheme */
    char media_attr[128];
#if CONFIG_EXAMPLE_SEGMENT_NAMING_TIMESTAMP
    snprintf(media_attr, sizeof(media_attr), "%s/segment_$Time$.m4s", p->track_name);
#else
    snprintf(media_attr, sizeof(media_attr), "%s/segment_$Number$.m4s", p->track_name);
#endif

    int n = snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n"
        "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
        "     xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 "
              "http://standards.iso.org/ittf/PubliclyAvailableStandards/"
              "MPEG-DASH_schema_files/DASH-MPD.xsd\"\n"
        "     type=\"static\"\n"
        "     profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011,"
              "urn:mpeg:cmaf:2019\"\n"
        "     mediaPresentationDuration=\"PT%"PRIu32"S\"\n"
        "     minBufferTime=\"PT2S\">\n"
        "  <Period id=\"1\" start=\"PT0S\" duration=\"PT%"PRIu32"S\">\n"
        "    <AdaptationSet id=\"1\"\n"
        "                   mimeType=\"video/mp4\"\n"
        "                   codecs=\"%s\"\n"
        "                   width=\"%"PRIu32"\"\n"
        "                   height=\"%"PRIu32"\"\n"
        "                   frameRate=\"%"PRIu32"\"\n"
        "                   sar=\"1:1\">\n"
        "      <SegmentTemplate timescale=\"%"PRIu32"\"\n"
        "                       duration=\"%"PRIu32"\"\n"
        "                       startNumber=\"%"PRIu32"\"\n"
        "                       initialization=\"%s/%s.init\"\n"
        "                       media=\"%s\"/>\n"
        "      <Representation id=\"%s\" bandwidth=\"%"PRIu32"\"/>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n",
        total_duration_sec, total_duration_sec,
        p->codecs,
        p->width, p->height, p->framerate,
        p->timescale,
        p->seg_duration,
        p->start_number,
        p->track_name, p->track_name,
        media_attr,
        p->track_name, p->bandwidth);

    if (n < 0 || (size_t)n >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    *out_len = (size_t)n;
    return ESP_OK;
}
