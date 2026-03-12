/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file antmedia_adapter.c
 * @brief Ant Media Server CMAF/DASH ingest adapter
 *
 * Ant Media Server supports CMAF/DASH ingest via HTTP PUT:
 *   - Stream creation: POST /WebRTCAppEE/rest/v2/broadcasts (optional)
 *   - Init segment: PUT /WebRTCAppEE/streams/{streamId}/init.mp4
 *   - Media segments: PUT /WebRTCAppEE/streams/{streamId}/segment_0001.m4s, ...
 *   - MPD: Auto-generated or PUT /WebRTCAppEE/streams/{streamId}/index.mpd
 *
 * Documentation:
 *   - Docker: https://docs.antmedia.io/guides/installing-on-linux/ams-docker-installation/
 *   - DASH/CMAF: https://docs.antmedia.io/guides/playing-live-stream/dash-playing-cmaf/
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "ingest_transport.h"

static const char *TAG = "antmedia";

#define URL_BUF_LEN 512
#define RESP_BUF_LEN 512
#define DEFAULT_APP_NAME "WebRTCAppEE"  /* Default Ant Media Server application */

struct antmedia_ctx {
    char server_host[128];
    uint16_t server_port;
    char stream_name[64];
    char app_name[64];  /* Application name (default: WebRTCAppEE) */
    char stream_id[128]; /* Stream ID from server */
    char api_key[128];  /* Optional API key */

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

/* Helper: Build URL (HTTP or HTTPS based on port) */
static void build_url(char *buf, size_t buf_size, const char *host, uint16_t port,
                      const char *path)
{
    /* Use HTTP for common HTTP ports (80, 5080), HTTPS for others */
    const char *scheme = (port == 80 || port == 5080) ? "http" : "https";
    snprintf(buf, buf_size, "%s://%s:%d%s", scheme, host, port, path);
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
        char error_buf[256];
        int read_len = esp_http_client_read(http, error_buf, sizeof(error_buf) - 1);
        if (read_len > 0) {
            error_buf[read_len] = '\0';
            ESP_LOGE(TAG, "HTTP error response: %s", error_buf);
        }
        /* Drain any remaining response data */
        char drain_buf[128];
        while (esp_http_client_read(http, drain_buf, sizeof(drain_buf)) > 0);
        esp_http_client_close(http);
        return ESP_FAIL;
    }

    esp_http_client_close(http);
    return ESP_OK;
}

static esp_err_t antmedia_init(ingest_transport_handle_t h,
                                const ingest_transport_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && h, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->server_type == INGEST_SERVER_ANTMEDIA,
                        ESP_ERR_INVALID_ARG, TAG, "not Ant Media Server type");

    struct antmedia_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc");

    strncpy(ctx->server_host, config->server_host, sizeof(ctx->server_host) - 1);
    ctx->server_port = config->server_port;
    strncpy(ctx->stream_name, config->track_name, sizeof(ctx->stream_name) - 1);
    strncpy(ctx->app_name, DEFAULT_APP_NAME, sizeof(ctx->app_name) - 1);

    /* Extract API key if using API key auth */
    if (config->auth.method == INGEST_AUTH_API_KEY) {
        strncpy(ctx->api_key, config->auth.credentials.api_key.api_key,
                sizeof(ctx->api_key) - 1);
    }

    /* Determine transport type: HTTP for ports 80/5080, HTTPS for others */
    bool use_https = (ctx->server_port != 80 && ctx->server_port != 5080);

    /* Build base URL for HTTP client initialization */
    char base_url[URL_BUF_LEN];
    const char *scheme = use_https ? "https" : "http";
    snprintf(base_url, sizeof(base_url), "%s://%s:%d", scheme, ctx->server_host, ctx->server_port);

    esp_http_client_config_t http_cfg = {
        .url = base_url,
        .transport_type = use_https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .cert_pem = (use_https && config->auth.method == INGEST_AUTH_MTLS) ?
                     config->auth.credentials.mtls.server_ca_pem : NULL,
        .client_cert_pem = (use_https && config->auth.method == INGEST_AUTH_MTLS) ?
                            config->auth.credentials.mtls.client_cert_pem : NULL,
        .client_key_pem = (use_https && config->auth.method == INGEST_AUTH_MTLS) ?
                           config->auth.credentials.mtls.client_key_pem : NULL,
        .keep_alive_enable = true,
        .timeout_ms = 10000,
    };

    ctx->http = esp_http_client_init(&http_cfg);
    if (!ctx->http) {
        free(ctx);
        return ESP_FAIL;
    }

    ctx->segment_number = 1;
    ctx->session_active = false;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    transport_ctx->adapter_ctx = ctx;
    /* Note: transport_ctx->ops is already set by ingest_transport_init() dispatcher */

    ESP_LOGI(TAG, "Ant Media Server adapter initialized: %s:%d app=%s stream=%s",
             ctx->server_host, ctx->server_port, ctx->app_name, ctx->stream_name);
    return ESP_OK;
}

static esp_err_t antmedia_create_stream(ingest_transport_handle_t h,
                                        ingest_stream_info_t *stream_info)
{
    ESP_RETURN_ON_FALSE(h && stream_info, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Ant Media Server can create streams via REST API, but for CMAF push ingest,
     * we can use the stream name directly. The stream will be created on first segment upload.
     */
    ctx->stream_info.stream_id = 0;  /* Not used for Ant Media Server */
    strncpy(ctx->stream_info.stream_key, ctx->stream_name, sizeof(ctx->stream_info.stream_key) - 1);
    strncpy(ctx->stream_id, ctx->stream_name, sizeof(ctx->stream_id) - 1);

    snprintf(ctx->stream_info.ingest_url, sizeof(ctx->stream_info.ingest_url),
             "%s://%s:%d/%s/streams/%s",
             (ctx->server_port == 80 || ctx->server_port == 5080) ? "http" : "https",
             ctx->server_host, ctx->server_port, ctx->app_name, ctx->stream_id);

    *stream_info = ctx->stream_info;

    ESP_LOGI(TAG, "Stream ready: %s (ID: %s)", ctx->stream_name, ctx->stream_id);
    return ESP_OK;
}

static esp_err_t antmedia_start_session(ingest_transport_handle_t h,
                                         const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ctx->segment_number = 1;
    ctx->session_active = true;

    /* Upload MPD if provided (Ant Media Server can auto-generate, but we can upload custom) */
    if (mpd_xml && mpd_len > 0) {
        /* Use relative path since HTTP client was initialized with base URL */
        char path[256];
        snprintf(path, sizeof(path), "/%s/streams/%s/index.mpd",
                 ctx->app_name, ctx->stream_id);

        return do_request(ctx->http, HTTP_METHOD_PUT, path,
                          mpd_xml, mpd_len, "application/dash+xml", ctx->api_key);
    }

    return ESP_OK;
}

static esp_err_t antmedia_upload_init_segment(ingest_transport_handle_t h,
                                               const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Use relative path since HTTP client was initialized with base URL */
    char path[256];
    snprintf(path, sizeof(path), "/%s/streams/%s/init.mp4",
             ctx->app_name, ctx->stream_id);

    ESP_LOGI(TAG, "PUT init segment → %s%s", ctx->server_host, path);
    return do_request(ctx->http, HTTP_METHOD_PUT, path,
                      data, size, "video/mp4", ctx->api_key);
}

static esp_err_t antmedia_upload_media_segment(ingest_transport_handle_t h,
                                                const uint8_t *data, size_t size, uint32_t presentation_time)
{
    (void)presentation_time; /* Not used for AntMedia - uses sequential numbering */
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Use relative path since HTTP client was initialized with base URL */
    char path[256];
    snprintf(path, sizeof(path), "/%s/streams/%s/segment_%04d.m4s",
             ctx->app_name, ctx->stream_id, ctx->segment_number);

    ESP_LOGI(TAG, "PUT seg #%d → %s%s", ctx->segment_number, ctx->server_host, path);
    esp_err_t err = do_request(ctx->http, HTTP_METHOD_PUT, path,
                                data, size, "video/iso.segment", ctx->api_key);
    if (err == ESP_OK) {
        ctx->segment_number++;
    }
    return err;
}

static esp_err_t antmedia_end_session(ingest_transport_handle_t h,
                                      const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ctx->session_active = false;

    /* Upload final MPD if provided */
    if (mpd_xml && mpd_len > 0) {
        /* Use relative path since HTTP client was initialized with base URL */
        char path[256];
        snprintf(path, sizeof(path), "/%s/streams/%s/index.mpd",
                 ctx->app_name, ctx->stream_id);

        return do_request(ctx->http, HTTP_METHOD_PUT, path,
                          mpd_xml, mpd_len, "application/dash+xml", ctx->api_key);
    }

    return ESP_OK;
}

static uint16_t antmedia_get_segment_number(ingest_transport_handle_t h)
{
    if (!h) return 0;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    return ctx ? ctx->segment_number : 0;
}

static void antmedia_deinit(ingest_transport_handle_t h)
{
    if (!h) return;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct antmedia_ctx *ctx = (struct antmedia_ctx *)transport_ctx->adapter_ctx;
    if (!ctx) return;

    if (ctx->http) {
        esp_http_client_cleanup(ctx->http);
    }
    free(ctx);
    transport_ctx->adapter_ctx = NULL;
}

/* Export adapter operations */
static const ingest_transport_ops_t antmedia_adapter_ops = {
    .init = antmedia_init,
    .create_stream = antmedia_create_stream,
    .start_session = antmedia_start_session,
    .upload_init_segment = antmedia_upload_init_segment,
    .upload_media_segment = antmedia_upload_media_segment,
    .end_session = antmedia_end_session,
    .get_segment_number = antmedia_get_segment_number,
    .deinit = antmedia_deinit,
};

const ingest_transport_ops_t *antmedia_adapter_get_ops(void)
{
    return &antmedia_adapter_ops;
}
