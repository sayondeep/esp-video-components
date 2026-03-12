/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file mediamtx_adapter.c
 * @brief MediaMTX CMAF ingest adapter
 *
 * MediaMTX accepts HTTP PUT directly:
 *   - Init segment: PUT /stream_name/init.mp4
 *   - Media segments: PUT /stream_name/segment_0001.m4s, segment_0002.m4s, ...
 *   - MPD: Auto-generated at /stream_name/index.mpd
 *
 * No stream creation or session management needed.
 * Uses HTTP (not HTTPS) by default.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "ingest_transport.h"

static const char *TAG = "mediamtx";

#define URL_BUF_LEN 512

struct mediamtx_ctx {
    char server_host[128];
    uint16_t server_port;
    char stream_name[64];

    esp_http_client_handle_t http;
    uint16_t segment_number;
    bool session_active;
    ingest_stream_info_t stream_info;
};

/**
 * @brief Internal structure for ingest_transport_ctx
 */
struct ingest_transport_ctx {
    void *adapter_ctx;
    const ingest_transport_ops_t *ops;
};

/* Helper: Build URL (HTTP, not HTTPS for MediaMTX) */
static void build_url(char *buf, size_t buf_size, const char *host, uint16_t port,
                      const char *path)
{
    snprintf(buf, buf_size, "http://%s:%d%s", host, port, path);
}

/* Helper: HTTP request */
static esp_err_t do_request(esp_http_client_handle_t http,
                             esp_http_client_method_t method,
                             const char *url, const void *data, size_t data_len,
                             const char *content_type)
{
    esp_http_client_set_url(http, url);
    esp_http_client_set_method(http, method);

    if (content_type) {
        esp_http_client_set_header(http, "Content-Type", content_type);
    }

    esp_err_t err = esp_http_client_open(http, (int)data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (data && data_len > 0) {
        int written = esp_http_client_write(http, (const char *)data, (int)data_len);
        if (written < 0 || (size_t)written != data_len) {
            ESP_LOGE(TAG, "HTTP write failed: written=%d, expected=%zu", written, data_len);
            esp_http_client_close(http);
            return ESP_FAIL;
        }
    }

    esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    int content_length = esp_http_client_get_content_length(http);

    ESP_LOGI(TAG, "HTTP %s → status=%d, content-length=%d",
             (method == HTTP_METHOD_PUT) ? "PUT" : "GET", status, content_length);

    if (status < 200 || status >= 300) {
        /* Read error response body if available */
        if (content_length > 0 && content_length < 256) {
            char error_buf[256];
            int error_len = esp_http_client_read(http, error_buf, sizeof(error_buf) - 1);
            if (error_len > 0) {
                error_buf[error_len] = '\0';
                ESP_LOGE(TAG, "HTTP error response: %s", error_buf);
            }
        }
        esp_http_client_close(http);
        return ESP_FAIL;
    }

    /* Drain response body if any */
    if (content_length > 0) {
        char drain_buf[128];
        while (esp_http_client_read(http, drain_buf, sizeof(drain_buf)) > 0) {
            /* Just drain, don't process */
        }
    }

    esp_http_client_close(http);
    return ESP_OK;
}

static esp_err_t mediamtx_init(ingest_transport_handle_t h,
                                const ingest_transport_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && h, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->server_type == INGEST_SERVER_MEDIAMTX,
                        ESP_ERR_INVALID_ARG, TAG, "not MediaMTX server type");

    struct mediamtx_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc");

    strncpy(ctx->server_host, config->server_host, sizeof(ctx->server_host) - 1);
    ctx->server_port = config->server_port;
    strncpy(ctx->stream_name, config->track_name, sizeof(ctx->stream_name) - 1);

    char base_url[URL_BUF_LEN];
    build_url(base_url, sizeof(base_url), ctx->server_host, ctx->server_port, "");

    esp_http_client_config_t http_cfg = {
        .url = base_url,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,  // MediaMTX uses HTTP (not HTTPS)
        .keep_alive_enable = true,
        .timeout_ms = 10000,
    };

    ctx->http = esp_http_client_init(&http_cfg);
    if (!ctx->http) {
        free(ctx);
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    ctx->segment_number = 1;  /* MediaMTX starts at 1 */
    ctx->session_active = false;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    transport_ctx->adapter_ctx = ctx;
    /* Note: transport_ctx->ops is already set by ingest_transport_init() dispatcher */

    ESP_LOGI(TAG, "MediaMTX adapter initialized: %s:%d stream=%s",
             ctx->server_host, ctx->server_port, ctx->stream_name);
    return ESP_OK;
}

static esp_err_t mediamtx_create_stream(ingest_transport_handle_t h,
                                         ingest_stream_info_t *stream_info)
{
    ESP_RETURN_ON_FALSE(h && stream_info, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* MediaMTX doesn't require explicit stream creation - stream is created on first PUT */
    ctx->stream_info.stream_id = 0;  /* Not used for MediaMTX */
    strncpy(ctx->stream_info.stream_key, ctx->stream_name, sizeof(ctx->stream_info.stream_key) - 1);
    snprintf(ctx->stream_info.ingest_url, sizeof(ctx->stream_info.ingest_url),
             "http://%s:%d/%s", ctx->server_host, ctx->server_port, ctx->stream_name);

    *stream_info = ctx->stream_info;

    ESP_LOGI(TAG, "Stream ready: %s", ctx->stream_name);
    return ESP_OK;
}

static esp_err_t mediamtx_start_session(ingest_transport_handle_t h,
                                         const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* MediaMTX auto-generates MPD, but we can optionally upload one */
    /* For now, just mark session as active */
    ctx->session_active = true;
    ctx->segment_number = 1;

    ESP_LOGI(TAG, "Session started for stream: %s", ctx->stream_name);
    return ESP_OK;
}

static esp_err_t mediamtx_upload_init_segment(ingest_transport_handle_t h,
                                                const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    char url[URL_BUF_LEN];
    char path[256];
    snprintf(path, sizeof(path), "/%s/init.mp4", ctx->stream_name);
    build_url(url, sizeof(url), ctx->server_host, ctx->server_port, path);

    ESP_LOGI(TAG, "PUT init segment (%zu B) → %s", size, url);
    return do_request(ctx->http, HTTP_METHOD_PUT, url, data, size, "video/mp4");
}

static esp_err_t mediamtx_upload_media_segment(ingest_transport_handle_t h,
                                                 const uint8_t *data, size_t size, uint32_t presentation_time)
{
    (void)presentation_time; /* Not used for MediaMTX - uses sequential numbering */
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    char url[URL_BUF_LEN];
    char path[256];
    snprintf(path, sizeof(path), "/%s/segment_%04d.m4s", ctx->stream_name, ctx->segment_number);
    build_url(url, sizeof(url), ctx->server_host, ctx->server_port, path);

    ESP_LOGI(TAG, "PUT seg #%d (%zu B) → %s", ctx->segment_number, size, url);

    esp_err_t err = do_request(ctx->http, HTTP_METHOD_PUT, url, data, size, "video/iso.segment");
    if (err == ESP_OK) {
        ctx->segment_number++;
    }
    return err;
}

static esp_err_t mediamtx_end_session(ingest_transport_handle_t h,
                                       const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* MediaMTX auto-generates MPD, no need to upload */
    ctx->session_active = false;

    ESP_LOGI(TAG, "Session ended for stream: %s (%d segments)", ctx->stream_name,
             ctx->segment_number - 1);
    return ESP_OK;
}

static uint16_t mediamtx_get_segment_number(ingest_transport_handle_t h)
{
    if (!h) return 0;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    return ctx ? ctx->segment_number : 0;
}

static void mediamtx_deinit(ingest_transport_handle_t h)
{
    if (!h) return;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mediamtx_ctx *ctx = (struct mediamtx_ctx *)transport_ctx->adapter_ctx;
    if (!ctx) return;

    if (ctx->http) {
        esp_http_client_cleanup(ctx->http);
    }
    free(ctx);
    transport_ctx->adapter_ctx = NULL;
}

/* Export adapter operations */
static const ingest_transport_ops_t mediamtx_adapter_ops = {
    .init = mediamtx_init,
    .create_stream = mediamtx_create_stream,
    .start_session = mediamtx_start_session,
    .upload_init_segment = mediamtx_upload_init_segment,
    .upload_media_segment = mediamtx_upload_media_segment,
    .end_session = mediamtx_end_session,
    .get_segment_number = mediamtx_get_segment_number,
    .deinit = mediamtx_deinit,
};

const ingest_transport_ops_t *mediamtx_adapter_get_ops(void)
{
    return &mediamtx_adapter_ops;
}
