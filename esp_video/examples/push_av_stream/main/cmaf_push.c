/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file cmaf_push.c
 * @brief HTTPS CMAF push client implementation
 *
 * Uses esp_http_client with TLS (optionally mTLS) to upload fMP4 segments
 * to the push_av_server using the DASH Interface-2 ingest sequence.
 *
 * Connection strategy: a single esp_http_client handle is kept alive across
 * multiple requests to the same server so that the TLS session is reused.
 * The URL is updated via esp_http_client_set_url() before each request.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "cmaf_push.h"

static const char *TAG = "cmaf_push";

/* Maximum URL length for any individual request */
#define URL_BUF_LEN  512

/* Maximum response body length we bother reading */
#define RESP_BUF_LEN 512

/* =========================================================================
 * Context
 * ========================================================================= */

struct cmaf_push_ctx {
    char     server_host[128];
    uint16_t server_port;
    char     track_name[64];

    /* TLS credentials (pointers to caller-managed / embedded data) */
    const char *server_ca_pem;
    const char *client_cert_pem;
    const char *client_key_pem;

    /* Persistent HTTP client handle */
    esp_http_client_handle_t http;

    /* Session state */
    int      stream_id;       /**< Assigned by POST /streams */
    uint32_t session_number;  /**< Increments with each session */
    uint16_t segment_number;  /**< Starts at CMAF_PUSH_FIRST_SEGMENT_NUMBER */
};

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Drain any unconsumed response body so the connection can be reused.
 */
static void drain_response(esp_http_client_handle_t http)
{
    char tmp[128];
    int  rd;
    do {
        rd = esp_http_client_read(http, tmp, sizeof(tmp));
    } while (rd > 0);
}

/**
 * PUT or POST a data buffer to the given URL.
 *
 * @param http         HTTP client handle (URL already set by caller).
 * @param method       HTTP_METHOD_PUT or HTTP_METHOD_POST.
 * @param url          Full URL for this request.
 * @param data         Request body (may be NULL for empty body).
 * @param data_len     Body length in bytes.
 * @param content_type Content-Type header value (NULL to omit).
 * @param resp_buf     Optional buffer to receive the response body.
 * @param resp_buf_sz  Size of resp_buf (0 to skip reading).
 * @param resp_len     Bytes written to resp_buf (set if resp_buf != NULL).
 *
 * @return ESP_OK when the server responded 2xx, otherwise ESP_FAIL.
 */
static esp_err_t do_request(esp_http_client_handle_t http,
                             esp_http_client_method_t method,
                             const char *url,
                             const void *data, size_t data_len,
                             const char *content_type,
                             char *resp_buf, size_t resp_buf_sz,
                             size_t *resp_len)
{
    esp_err_t err;

    esp_http_client_set_url(http, url);
    esp_http_client_set_method(http, method);

    if (content_type) {
        esp_http_client_set_header(http, "Content-Type", content_type);
    }
    /* DASH-IF ingest version header (spec compliance) */
    esp_http_client_set_header(http, "DASH-IF-Ingest", "1.1");

    err = esp_http_client_open(http, (int)data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed (%s): %s", url, esp_err_to_name(err));
        return err;
    }

    if (data && data_len > 0) {
        int written = esp_http_client_write(http, (const char *)data, (int)data_len);
        if (written < 0) {
            ESP_LOGE(TAG, "write failed for %s", url);
            esp_http_client_close(http);
            return ESP_FAIL;
        }
    }

    int content_length = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);

    /* Read response body */
    if (resp_buf && resp_buf_sz > 0 && content_length != 0) {
        int rd = esp_http_client_read(http, resp_buf, (int)(resp_buf_sz - 1));
        if (rd >= 0) {
            resp_buf[rd] = '\0';
            if (resp_len) *resp_len = (size_t)rd;
        }
    } else {
        drain_response(http);
    }

    esp_http_client_close(http);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        return ESP_FAIL;
    }

    (void)content_length; /* suppress unused warning when resp_buf is NULL */
    return ESP_OK;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

esp_err_t cmaf_push_init(const cmaf_push_config_t *config,
                          cmaf_push_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && handle, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->server_host && config->server_port,
                        ESP_ERR_INVALID_ARG, TAG, "missing host/port");
    ESP_RETURN_ON_FALSE(config->track_name,
                        ESP_ERR_INVALID_ARG, TAG, "missing track_name");

    struct cmaf_push_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc ctx");

    strncpy(ctx->server_host, config->server_host, sizeof(ctx->server_host) - 1);
    ctx->server_port   = config->server_port;
    strncpy(ctx->track_name, config->track_name, sizeof(ctx->track_name) - 1);

    ctx->server_ca_pem    = config->server_ca_pem;
    ctx->client_cert_pem  = config->client_cert_pem;
    ctx->client_key_pem   = config->client_key_pem;

    ctx->stream_id      = -1;
    ctx->session_number = 0;
    ctx->segment_number = CMAF_PUSH_FIRST_SEGMENT_NUMBER;

    /* Build base URL for client init */
    char base_url[URL_BUF_LEN];
    snprintf(base_url, sizeof(base_url), "https://%s:%d/streams",
             ctx->server_host, ctx->server_port);

    esp_http_client_config_t http_cfg = {
        .url             = base_url,
        .transport_type  = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem        = ctx->server_ca_pem,
        .client_cert_pem = ctx->client_cert_pem,
        .client_key_pem  = ctx->client_key_pem,
        /* Always verify CN; set true only if no CA is available (should not happen) */
        .skip_cert_common_name_check = false,
        .keep_alive_enable = true,
        .timeout_ms      = 10000,
        .buffer_size     = 4096,
        .buffer_size_tx  = 4096,
    };

    ctx->http = esp_http_client_init(&http_cfg);
    if (!ctx->http) {
        free(ctx);
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init: https://%s:%d  track=%s  mtls=%s",
             ctx->server_host, ctx->server_port, ctx->track_name,
             ctx->client_cert_pem ? "yes" : "no");

    *handle = ctx;
    return ESP_OK;
}

void cmaf_push_deinit(cmaf_push_handle_t h)
{
    if (!h) return;
    if (h->http) {
        esp_http_client_cleanup(h->http);
    }
    free(h);
}

/* -------------------------------------------------------------------------
 * Step 1: POST /streams?interface=dash → get stream_id
 * ------------------------------------------------------------------------- */

esp_err_t cmaf_push_create_stream(cmaf_push_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "NULL handle");

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url), "https://%s:%d/streams?interface=dash",
             h->server_host, h->server_port);

    char resp[RESP_BUF_LEN] = {0};
    size_t resp_len = 0;

    esp_err_t err = do_request(h->http, HTTP_METHOD_POST, url,
                                NULL, 0, NULL,
                                resp, sizeof(resp), &resp_len);
    ESP_RETURN_ON_ERROR(err, TAG, "POST /streams failed");

    /*
     * Parse stream_id from JSON response: {"id": N, ...}
     * Use a simple forward scan – no JSON library needed.
     */
    const char *id_field = strstr(resp, "\"id\":");
    if (!id_field) {
        ESP_LOGE(TAG, "stream_id not found in response: %s", resp);
        return ESP_FAIL;
    }
    h->stream_id = atoi(id_field + 5); /* skip past `"id":` */
    if (h->stream_id <= 0) {
        ESP_LOGE(TAG, "invalid stream_id %d", h->stream_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "stream created: id=%d", h->stream_id);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Step 2: PUT session_<N>/index.mpd (type="dynamic") → start session
 * ------------------------------------------------------------------------- */

esp_err_t cmaf_push_start_session(cmaf_push_handle_t h,
                                   const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL");
    ESP_RETURN_ON_FALSE(h->stream_id > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call cmaf_push_create_stream first");

    h->session_number++;
    h->segment_number = CMAF_PUSH_FIRST_SEGMENT_NUMBER;

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/index.mpd",
             h->server_host, h->server_port,
             h->stream_id, h->session_number);

    ESP_LOGI(TAG, "PUT dynamic MPD → %s", url);
    return do_request(h->http, HTTP_METHOD_PUT, url,
                      mpd_xml, mpd_len, "application/dash+xml",
                      NULL, 0, NULL);
}

/* -------------------------------------------------------------------------
 * Step 3: PUT session_<N>/<track>/<track>.init → init segment
 * ------------------------------------------------------------------------- */

esp_err_t cmaf_push_upload_init_segment(cmaf_push_handle_t h,
                                         const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL");
    ESP_RETURN_ON_FALSE(h->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call cmaf_push_start_session first");

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/%s/%s.init",
             h->server_host, h->server_port,
             h->stream_id, h->session_number,
             h->track_name, h->track_name);

    ESP_LOGI(TAG, "PUT init segment (%zu B) → %s", size, url);
    return do_request(h->http, HTTP_METHOD_PUT, url,
                      data, size, "video/mp4",
                      NULL, 0, NULL);
}

/* -------------------------------------------------------------------------
 * Step 4…N: PUT session_<N>/<track>/segment_<M>.m4s → media segments
 * ------------------------------------------------------------------------- */

esp_err_t cmaf_push_upload_media_segment(cmaf_push_handle_t h,
                                          const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL");
    ESP_RETURN_ON_FALSE(h->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call cmaf_push_start_session first");

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/%s/segment_%"PRIu16".m4s",
             h->server_host, h->server_port,
             h->stream_id, h->session_number,
             h->track_name, h->segment_number);

    ESP_LOGI(TAG, "PUT seg #%"PRIu16" (%zu B) → %s",
             h->segment_number, size, url);

    esp_err_t err = do_request(h->http, HTTP_METHOD_PUT, url,
                                data, size, "video/iso.segment",
                                NULL, 0, NULL);
    if (err == ESP_OK) {
        h->segment_number++;
    }
    return err;
}

/* -------------------------------------------------------------------------
 * Final step: PUT session_<N>/index.mpd (type="static") → close session
 * ------------------------------------------------------------------------- */

esp_err_t cmaf_push_end_session(cmaf_push_handle_t h,
                                 const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL");
    ESP_RETURN_ON_FALSE(h->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "no active session");

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/index.mpd",
             h->server_host, h->server_port,
             h->stream_id, h->session_number);

    ESP_LOGI(TAG, "PUT static MPD → %s", url);
    esp_err_t err = do_request(h->http, HTTP_METHOD_PUT, url,
                                mpd_xml, mpd_len, "application/dash+xml",
                                NULL, 0, NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "session %"PRIu32" closed (%"PRIu16" segments)",
                 h->session_number,
                 (uint16_t)(h->segment_number - CMAF_PUSH_FIRST_SEGMENT_NUMBER));
    }
    return err;
}

uint16_t cmaf_push_get_segment_number(cmaf_push_handle_t h)
{
    return h ? h->segment_number : 0;
}
