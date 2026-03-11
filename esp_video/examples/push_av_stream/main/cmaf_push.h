/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file cmaf_push.h
 * @brief HTTPS CMAF ingest client for the Matter push_av_server.
 *
 * Implements the DASH Interface-2 upload sequence:
 *
 *   1. POST /streams?interface=dash       → allocate stream, get stream_id
 *   2. PUT  …/session_1/index.mpd         → dynamic MPD (starts session)
 *   3. PUT  …/session_1/<track>/<track>.init → fMP4 init segment
 *   4. PUT  …/session_1/<track>/segment_1001.m4s  → media segment
 *   5. …repeat for each segment…
 *   6. PUT  …/session_1/index.mpd         → static MPD  (ends session)
 *
 * TLS with mutual authentication (client certificate) is supported when
 * CONFIG_EXAMPLE_USE_MTLS is enabled; otherwise a TLS connection without
 * client certificate is used (requires server '--no-strict' mode).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** First segment number as required by the Matter/CMAF spec. */
#define CMAF_PUSH_FIRST_SEGMENT_NUMBER  1001

typedef struct cmaf_push_ctx *cmaf_push_handle_t;

/**
 * @brief Configuration for the CMAF push client.
 */
typedef struct {
    const char    *server_host;      /**< push_av_server hostname or IP */
    uint16_t       server_port;      /**< push_av_server HTTPS port (e.g. 1234) */
    const char    *track_name;       /**< Track / representation name, e.g. "video1" */

    /* TLS – pass NULL to skip (server must run with --no-strict) */
    const char    *server_ca_pem;    /**< Server root CA certificate (PEM, NUL-terminated) */
    const char    *client_cert_pem;  /**< Client certificate (PEM, NUL-terminated) */
    const char    *client_key_pem;   /**< Client private key  (PEM, NUL-terminated) */
} cmaf_push_config_t;

/**
 * @brief Initialise the CMAF push client.
 *
 * Creates and configures the underlying esp_http_client handle.
 * Does NOT establish a network connection yet.
 *
 * @param config  Configuration (host, port, TLS certs, track name).
 * @param handle  Output handle, set on success.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_init(const cmaf_push_config_t *config,
                          cmaf_push_handle_t *handle);

/**
 * @brief POST /streams?interface=dash to allocate a stream.
 *
 * Must be called once before cmaf_push_start_session().
 * The returned stream_id is stored internally.
 *
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_create_stream(cmaf_push_handle_t h);

/**
 * @brief Upload a dynamic DASH MPD to start a new session.
 *
 * PUT <stream_url>/session_<N>/index.mpd  with type="dynamic".
 *
 * @param mpd_xml  NUL-terminated MPD XML string.
 * @param mpd_len  String length (bytes, excluding NUL).
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_start_session(cmaf_push_handle_t h,
                                   const char *mpd_xml, size_t mpd_len);

/**
 * @brief Upload the fMP4 init segment.
 *
 * PUT <stream_url>/session_<N>/<track>/<track>.init
 *
 * @param data  Init segment bytes (ftyp + moov).
 * @param size  Byte count.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_upload_init_segment(cmaf_push_handle_t h,
                                         const uint8_t *data, size_t size);

/**
 * @brief Upload one fMP4 media segment.
 *
 * PUT <stream_url>/session_<N>/<track>/segment_<M>.m4s
 * The segment number is incremented automatically after each successful call.
 *
 * @param data  Media segment bytes (styp + moof + mdat).
 * @param size  Byte count.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_upload_media_segment(cmaf_push_handle_t h,
                                          const uint8_t *data, size_t size);

/**
 * @brief Upload a static DASH MPD to close the current session.
 *
 * PUT <stream_url>/session_<N>/index.mpd  with type="static".
 *
 * @param mpd_xml  NUL-terminated MPD XML string.
 * @param mpd_len  String length.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_push_end_session(cmaf_push_handle_t h,
                                 const char *mpd_xml, size_t mpd_len);

/**
 * @brief Return the current segment number (next to be uploaded).
 */
uint16_t cmaf_push_get_segment_number(cmaf_push_handle_t h);

/**
 * @brief Deinitialise the client and free all resources.
 */
void cmaf_push_deinit(cmaf_push_handle_t h);

#ifdef __cplusplus
}
#endif
