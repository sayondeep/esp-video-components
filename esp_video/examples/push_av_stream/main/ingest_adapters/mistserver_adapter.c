/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file mistserver_adapter.c
 * @brief MistServer CMAF ingest adapter
 *
 * MistServer uses a simpler URL structure:
 *   - Stream creation: POST /api/streams (optional, or use direct push)
 *   - Init segment: PUT /stream_name/init.mp4
 *   - Media segments: PUT /stream_name/segment_0001.m4s, segment_0002.m4s, ...
 *   - MPD: Auto-generated or PUT /stream_name/index.mpd
 *
 * Authentication: API key in X-API-Key header, or Basic Auth
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "ingest_transport.h"

static const char *TAG = "mistserver";

#define URL_BUF_LEN 512
#define RESP_BUF_LEN 512

struct mistserver_ctx {
    char server_host[128];
    uint16_t server_port;
    char stream_name[64];
    char api_key[128];  /* Optional */

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

/* Helper: Build URL */
static void build_url(char *buf, size_t buf_size, const char *host, uint16_t port,
                      const char *path)
{
    snprintf(buf, buf_size, "https://%s:%d%s", host, port, path);
}

/* Helper: HTTP request */
static esp_err_t do_request(esp_http_client_handle_t http,
                             esp_http_client_method_t method,
                             const char *url, const void *data, size_t data_len,
                             const char *content_type, const char *api_key)
{
    esp_http_client_set_url(http, url);
    esp_http_client_set_method(http, method);

    if (content_type) {
        esp_http_client_set_header(http, "Content-Type", content_type);
    }
    if (api_key && api_key[0]) {
        esp_http_client_set_header(http, "X-API-Key", api_key);
    }

    esp_err_t err = esp_http_client_open(http, (int)data_len);
    if (err != ESP_OK) return err;

    if (data && data_len > 0) {
        int written = esp_http_client_write(http, (const char *)data, (int)data_len);
        if (written < 0) {
            esp_http_client_close(http);
            return ESP_FAIL;
        }
    }

    esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_close(http);

    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t mistserver_init(ingest_transport_handle_t h,
                                  const ingest_transport_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && h, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->server_type == INGEST_SERVER_MISTSERVER,
                        ESP_ERR_INVALID_ARG, TAG, "not MistServer type");

    struct mistserver_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc");

    strncpy(ctx->server_host, config->server_host, sizeof(ctx->server_host) - 1);
    ctx->server_port = config->server_port;
    strncpy(ctx->stream_name, config->track_name, sizeof(ctx->stream_name) - 1);

    /* Extract API key if using API key auth */
    if (config->auth.method == INGEST_AUTH_API_KEY) {
        strncpy(ctx->api_key, config->auth.credentials.api_key.api_key,
                sizeof(ctx->api_key) - 1);
    }

    char base_url[URL_BUF_LEN];
    build_url(base_url, sizeof(base_url), ctx->server_host, ctx->server_port, "");

    esp_http_client_config_t http_cfg = {
        .url = base_url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (config->auth.method == INGEST_AUTH_MTLS) ?
                     config->auth.credentials.mtls.server_ca_pem : NULL,
        .client_cert_pem = (config->auth.method == INGEST_AUTH_MTLS) ?
                            config->auth.credentials.mtls.client_cert_pem : NULL,
        .client_key_pem = (config->auth.method == INGEST_AUTH_MTLS) ?
                           config->auth.credentials.mtls.client_key_pem : NULL,
        .keep_alive_enable = true,
        .timeout_ms = 10000,
    };

    ctx->http = esp_http_client_init(&http_cfg);
    if (!ctx->http) {
        free(ctx);
        return ESP_FAIL;
    }

    ctx->segment_number = 1;  /* MistServer starts at 1, not 1001 */
    ctx->session_active = false;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    transport_ctx->adapter_ctx = ctx;
    /* Note: transport_ctx->ops is already set by ingest_transport_init() dispatcher */

    ESP_LOGI(TAG, "MistServer adapter initialized: %s:%d stream=%s",
             ctx->server_host, ctx->server_port, ctx->stream_name);
    return ESP_OK;
}

static esp_err_t mistserver_create_stream(ingest_transport_handle_t h,
                                           ingest_stream_info_t *stream_info)
{
    ESP_RETURN_ON_FALSE(h && stream_info, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* MistServer doesn't require explicit stream creation for push ingest */
    /* Stream is created implicitly on first segment upload */
    ctx->stream_info.stream_id = 0;  /* Not used for MistServer */
    strncpy(ctx->stream_info.stream_key, ctx->stream_name, sizeof(ctx->stream_info.stream_key) - 1);
    snprintf(ctx->stream_info.ingest_url, sizeof(ctx->stream_info.ingest_url),
             "https://%s:%d/%s", ctx->server_host, ctx->server_port, ctx->stream_name);

    *stream_info = ctx->stream_info;

    ESP_LOGI(TAG, "Stream ready: %s", ctx->stream_name);
    return ESP_OK;
}

static esp_err_t mistserver_start_session(ingest_transport_handle_t h,
                                            const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ctx->segment_number = 1;
    ctx->session_active = true;

    /* Upload MPD if provided (optional for MistServer) */
    if (mpd_xml && mpd_len > 0) {
        char url[URL_BUF_LEN];
        snprintf(url, sizeof(url), "/%s/index.mpd", ctx->stream_name);
        build_url(url, sizeof(url), ctx->server_host, ctx->server_port, url);

        return do_request(ctx->http, HTTP_METHOD_PUT, url,
                          mpd_xml, mpd_len, "application/dash+xml",
                          ctx->api_key);
    }

    return ESP_OK;
}

static esp_err_t mistserver_upload_init_segment(ingest_transport_handle_t h,
                                                 const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    char url[URL_BUF_LEN];
    char path[256];
    snprintf(path, sizeof(path), "/%s/init.mp4", ctx->stream_name);
    build_url(url, sizeof(url), ctx->server_host, ctx->server_port, path);

    ESP_LOGI(TAG, "PUT init segment → %s", url);
    return do_request(ctx->http, HTTP_METHOD_PUT, url,
                      data, size, "video/mp4", ctx->api_key);
}

static esp_err_t mistserver_upload_media_segment(ingest_transport_handle_t h,
                                                   const uint8_t *data, size_t size, uint32_t presentation_time)
{
    (void)presentation_time; /* Not used for MistServer - uses sequential numbering */
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    char url[URL_BUF_LEN];
    char path[256];
    snprintf(path, sizeof(path), "/%s/segment_%04d.m4s",
             ctx->stream_name, ctx->segment_number);
    build_url(url, sizeof(url), ctx->server_host, ctx->server_port, path);

    esp_err_t err = do_request(ctx->http, HTTP_METHOD_PUT, url,
                                data, size, "video/iso.segment", ctx->api_key);
    if (err == ESP_OK) {
        ctx->segment_number++;
    }
    return err;
}

static esp_err_t mistserver_end_session(ingest_transport_handle_t h,
                                         const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ctx->session_active = false;

    /* Upload final MPD if provided */
    if (mpd_xml && mpd_len > 0) {
        char url[URL_BUF_LEN];
        char path[256];
        snprintf(path, sizeof(path), "/%s/index.mpd", ctx->stream_name);
        build_url(url, sizeof(url), ctx->server_host, ctx->server_port, path);

        return do_request(ctx->http, HTTP_METHOD_PUT, url,
                          mpd_xml, mpd_len, "application/dash+xml", ctx->api_key);
    }

    return ESP_OK;
}

static uint16_t mistserver_get_segment_number(ingest_transport_handle_t h)
{
    if (!h) return 0;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    return ctx ? ctx->segment_number : 0;
}

static void mistserver_deinit(ingest_transport_handle_t h)
{
    if (!h) return;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct mistserver_ctx *ctx = (struct mistserver_ctx *)transport_ctx->adapter_ctx;
    if (!ctx) return;

    if (ctx->http) {
        esp_http_client_cleanup(ctx->http);
    }
    free(ctx);
    transport_ctx->adapter_ctx = NULL;
}

/* Export adapter operations */
static const ingest_transport_ops_t mistserver_adapter_ops = {
    .init = mistserver_init,
    .create_stream = mistserver_create_stream,
    .start_session = mistserver_start_session,
    .upload_init_segment = mistserver_upload_init_segment,
    .upload_media_segment = mistserver_upload_media_segment,
    .end_session = mistserver_end_session,
    .get_segment_number = mistserver_get_segment_number,
    .deinit = mistserver_deinit,
};

const ingest_transport_ops_t *mistserver_adapter_get_ops(void)
{
    return &mistserver_adapter_ops;
}
