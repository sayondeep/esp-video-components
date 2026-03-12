/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file matter_adapter.c
 * @brief Matter push_av_server adapter - self-contained implementation
 *
 * This adapter implements the ingest_transport interface for Matter's
 * push_av_server. All Matter-specific logic is contained in this file.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "ingest_transport.h"

static const char *TAG = "matter_adapter";

/* Maximum URL length for any individual request */
#define URL_BUF_LEN  512

/* Maximum response body length we bother reading */
#define RESP_BUF_LEN 512

/* First segment number as required by the Matter/CMAF spec. */
#define MATTER_FIRST_SEGMENT_NUMBER  1001

/* =========================================================================
 * Matter-specific context
 * ========================================================================= */

struct matter_ctx {
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
    uint16_t segment_number;  /**< Starts at MATTER_FIRST_SEGMENT_NUMBER */
};


/* =========================================================================
 * Internal structure for ingest_transport_ctx
 * ========================================================================= */

struct ingest_transport_ctx {
    void *adapter_ctx;  /**< Adapter-specific context (matter_adapter_ctx*) */
    const ingest_transport_ops_t *ops; /**< Adapter operations */
};

/**
 * @brief Matter adapter context (for ingest_transport interface)
 */
struct matter_adapter_ctx {
    struct matter_ctx *matter_ctx;  /**< Matter-specific context */
    ingest_stream_info_t stream_info; /**< Cached stream info */
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
 * The connection is kept alive between requests — esp_http_client_close()
 * is NOT called on success.  This avoids a full TLS re-handshake per
 * segment upload (~200-800 ms on ESP32).  esp_http_client_open() will
 * transparently reuse the existing TCP+TLS session when talking to the
 * same host:port, which is always the case for our segment uploads.
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
        /* Connection may be broken; close so the next open reconnects. */
        esp_http_client_close(http);
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

    /* Fully drain response body so the connection can be reused.
     * We read into resp_buf first (if requested), then drain the rest. */
    if (resp_buf && resp_buf_sz > 0 && content_length != 0) {
        int rd = esp_http_client_read(http, resp_buf, (int)(resp_buf_sz - 1));
        if (rd >= 0) {
            resp_buf[rd] = '\0';
            if (resp_len) *resp_len = (size_t)rd;
        }
    }
    /* Always drain any remaining bytes (covers partial reads & chunked TE) */
    drain_response(http);

    /* Do NOT call esp_http_client_close() here — keep the TCP+TLS session
     * alive.  The next esp_http_client_open() reuses it automatically. */

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        return ESP_FAIL;
    }

    (void)content_length;
    return ESP_OK;
}

/* =========================================================================
 * Internal Implementation Helpers
 * ========================================================================= */

/* These helpers work directly with matter_ctx to avoid code duplication */

static esp_err_t matter_create_stream_impl(struct matter_ctx *ctx)
{
    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url), "https://%s:%d/streams?interface=dash",
             ctx->server_host, ctx->server_port);

    char resp[RESP_BUF_LEN] = {0};
    size_t resp_len = 0;

    esp_err_t err = do_request(ctx->http, HTTP_METHOD_POST, url,
                                NULL, 0, NULL,
                                resp, sizeof(resp), &resp_len);
    if (err != ESP_OK) {
        return err;
    }

    const char *id_field = strstr(resp, "\"id\":");
    if (!id_field) {
        ESP_LOGE(TAG, "stream_id not found in response: %s", resp);
        return ESP_FAIL;
    }
    ctx->stream_id = atoi(id_field + 5);
    if (ctx->stream_id <= 0) {
        ESP_LOGE(TAG, "invalid stream_id %d", ctx->stream_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "stream created: id=%d", ctx->stream_id);
    return ESP_OK;
}

static esp_err_t matter_start_session_impl(struct matter_ctx *ctx,
                                           const char *mpd_xml, size_t mpd_len)
{
    ctx->session_number++;
    ctx->segment_number = MATTER_FIRST_SEGMENT_NUMBER;

    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/index.mpd",
             ctx->server_host, ctx->server_port,
             ctx->stream_id, ctx->session_number);

    ESP_LOGI(TAG, "PUT dynamic MPD → %s", url);
    return do_request(ctx->http, HTTP_METHOD_PUT, url,
                      mpd_xml, mpd_len, "application/dash+xml",
                      NULL, 0, NULL);
}

static esp_err_t matter_upload_init_segment_impl(struct matter_ctx *ctx,
                                                  const uint8_t *data, size_t size)
{
    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/%s/%s.init",
             ctx->server_host, ctx->server_port,
             ctx->stream_id, ctx->session_number,
             ctx->track_name, ctx->track_name);

    ESP_LOGI(TAG, "PUT init segment (%zu B) → %s", size, url);
    return do_request(ctx->http, HTTP_METHOD_PUT, url,
                      data, size, "video/mp4",
                      NULL, 0, NULL);
}

static esp_err_t matter_upload_media_segment_impl(struct matter_ctx *ctx,
                                                   const uint8_t *data, size_t size,
                                                   uint32_t presentation_time)
{
    char url[URL_BUF_LEN];
#if CONFIG_EXAMPLE_SEGMENT_NAMING_TIMESTAMP
    /* Use presentation_time (timestamp) for filename - better for live streams */
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/%s/segment_%"PRIu32".m4s",
             ctx->server_host, ctx->server_port,
             ctx->stream_id, ctx->session_number,
             ctx->track_name, presentation_time);

    ESP_LOGI(TAG, "PUT seg #%"PRIu16" (time=%"PRIu32", %zu B) → %s",
             ctx->segment_number, presentation_time, size, url);
#else
    /* Use segment_number for filename - Matter spec compliant for recordings */
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/%s/segment_%"PRIu16".m4s",
             ctx->server_host, ctx->server_port,
             ctx->stream_id, ctx->session_number,
             ctx->track_name, ctx->segment_number);

    ESP_LOGI(TAG, "PUT seg #%"PRIu16" (%zu B) → %s",
             ctx->segment_number, size, url);
    (void)presentation_time; /* Not used in number-based mode */
#endif

    esp_err_t err = do_request(ctx->http, HTTP_METHOD_PUT, url,
                                data, size, "video/iso.segment",
                                NULL, 0, NULL);
    if (err == ESP_OK) {
        ctx->segment_number++;
    }
    return err;
}

static esp_err_t matter_end_session_impl(struct matter_ctx *ctx,
                                          const char *mpd_xml, size_t mpd_len)
{
    char url[URL_BUF_LEN];
    snprintf(url, sizeof(url),
             "https://%s:%d/streams/%d/session_%"PRIu32"/index.mpd",
             ctx->server_host, ctx->server_port,
             ctx->stream_id, ctx->session_number);

    ESP_LOGI(TAG, "PUT static MPD → %s", url);
    esp_err_t err = do_request(ctx->http, HTTP_METHOD_PUT, url,
                                mpd_xml, mpd_len, "application/dash+xml",
                                NULL, 0, NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "session %"PRIu32" closed (%"PRIu16" segments)",
                 ctx->session_number,
                 (uint16_t)(ctx->segment_number - MATTER_FIRST_SEGMENT_NUMBER));
    }
    return err;
}

/* =========================================================================
 * Ingest Transport Interface Implementation
 * ========================================================================= */

/* Forward declarations */
static esp_err_t matter_adapter_init(ingest_transport_handle_t h,
                                     const ingest_transport_config_t *config);
static esp_err_t matter_adapter_create_stream(ingest_transport_handle_t h,
                                               ingest_stream_info_t *stream_info);
static esp_err_t matter_adapter_start_session(ingest_transport_handle_t h,
                                                const char *mpd_xml, size_t mpd_len);
static esp_err_t matter_adapter_upload_init_segment(ingest_transport_handle_t h,
                                                      const uint8_t *data, size_t size);
static esp_err_t matter_adapter_upload_media_segment(ingest_transport_handle_t h,
                                                       const uint8_t *data, size_t size, uint32_t presentation_time);
static esp_err_t matter_adapter_end_session(ingest_transport_handle_t h,
                                             const char *mpd_xml, size_t mpd_len);
static uint16_t matter_adapter_get_segment_number(ingest_transport_handle_t h);
static void matter_adapter_deinit(ingest_transport_handle_t h);

/* Adapter operations table */
static const ingest_transport_ops_t matter_adapter_ops = {
    .init = matter_adapter_init,
    .create_stream = matter_adapter_create_stream,
    .start_session = matter_adapter_start_session,
    .upload_init_segment = matter_adapter_upload_init_segment,
    .upload_media_segment = matter_adapter_upload_media_segment,
    .end_session = matter_adapter_end_session,
    .get_segment_number = matter_adapter_get_segment_number,
    .deinit = matter_adapter_deinit,
};

static esp_err_t matter_adapter_init(ingest_transport_handle_t h,
                                      const ingest_transport_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && h, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->server_type == INGEST_SERVER_MATTER,
                        ESP_ERR_INVALID_ARG, TAG, "not Matter server type");

    /* Allocate adapter context */
    struct matter_adapter_ctx *adapter_ctx = calloc(1, sizeof(*adapter_ctx));
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_NO_MEM, TAG, "calloc adapter_ctx");

    /* Allocate Matter context */
    struct matter_ctx *matter_ctx = calloc(1, sizeof(*matter_ctx));
    if (!matter_ctx) {
        free(adapter_ctx);
        return ESP_ERR_NO_MEM;
    }

    strncpy(matter_ctx->server_host, config->server_host, sizeof(matter_ctx->server_host) - 1);
    matter_ctx->server_port = config->server_port;
    strncpy(matter_ctx->track_name, config->track_name, sizeof(matter_ctx->track_name) - 1);

    /* Extract TLS credentials from auth config */
    if (config->auth.method == INGEST_AUTH_MTLS) {
        matter_ctx->server_ca_pem = config->auth.credentials.mtls.server_ca_pem;
        matter_ctx->client_cert_pem = config->auth.credentials.mtls.client_cert_pem;
        matter_ctx->client_key_pem = config->auth.credentials.mtls.client_key_pem;
    } else if (config->auth.method == INGEST_AUTH_NONE) {
        matter_ctx->server_ca_pem = NULL;
        matter_ctx->client_cert_pem = NULL;
        matter_ctx->client_key_pem = NULL;
    } else {
        ESP_LOGE(TAG, "Unsupported auth method for Matter adapter: %d", config->auth.method);
        free(matter_ctx);
        free(adapter_ctx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    matter_ctx->stream_id = -1;
    matter_ctx->session_number = 0;
    matter_ctx->segment_number = MATTER_FIRST_SEGMENT_NUMBER;

    /* Build base URL for client init */
    char base_url[URL_BUF_LEN];
    snprintf(base_url, sizeof(base_url), "https://%s:%d/streams",
             matter_ctx->server_host, matter_ctx->server_port);

    esp_http_client_config_t http_cfg = {
        .url             = base_url,
        .transport_type  = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem        = matter_ctx->server_ca_pem,
        .client_cert_pem = matter_ctx->client_cert_pem,
        .client_key_pem  = matter_ctx->client_key_pem,
        .skip_cert_common_name_check = false,
        .keep_alive_enable = true,
        .timeout_ms      = 10000,
        .buffer_size     = 8192,
        .buffer_size_tx  = 16384,  /* Larger TX buffer → fewer TCP writes per ~600 KB segment */
    };

    matter_ctx->http = esp_http_client_init(&http_cfg);
    if (!matter_ctx->http) {
        free(matter_ctx);
        free(adapter_ctx);
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    adapter_ctx->matter_ctx = matter_ctx;
    adapter_ctx->stream_info.stream_id = -1;
    adapter_ctx->stream_info.stream_key[0] = '\0';
    adapter_ctx->stream_info.ingest_url[0] = '\0';

    /* Store adapter context in transport handle */
    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    transport_ctx->adapter_ctx = adapter_ctx;
    /* Note: transport_ctx->ops is already set by ingest_transport_init() dispatcher */

    ESP_LOGI(TAG, "Matter adapter initialized: %s:%d track=%s",
             config->server_host, config->server_port, config->track_name);
    return ESP_OK;
}

static esp_err_t matter_adapter_create_stream(ingest_transport_handle_t h,
                                                 ingest_stream_info_t *stream_info)
{
    ESP_RETURN_ON_FALSE(h && stream_info, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    esp_err_t err = matter_create_stream_impl(ctx);
    if (err != ESP_OK) {
        return err;
    }

    adapter_ctx->stream_info.stream_id = ctx->stream_id;
    snprintf(adapter_ctx->stream_info.stream_key, sizeof(adapter_ctx->stream_info.stream_key),
             "matter_stream_%d", ctx->stream_id);

    *stream_info = adapter_ctx->stream_info;
    return ESP_OK;
}

static esp_err_t matter_adapter_start_session(ingest_transport_handle_t h,
                                                const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    ESP_RETURN_ON_FALSE(ctx->stream_id > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call create_stream first");
    return matter_start_session_impl(ctx, mpd_xml, mpd_len);
}

static esp_err_t matter_adapter_upload_init_segment(ingest_transport_handle_t h,
                                                      const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    ESP_RETURN_ON_FALSE(ctx->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call start_session first");
    return matter_upload_init_segment_impl(ctx, data, size);
}

static esp_err_t matter_adapter_upload_media_segment(ingest_transport_handle_t h,
                                                       const uint8_t *data, size_t size,
                                                       uint32_t presentation_time)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    ESP_RETURN_ON_FALSE(ctx->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "call start_session first");
    return matter_upload_media_segment_impl(ctx, data, size, presentation_time);
}

static esp_err_t matter_adapter_end_session(ingest_transport_handle_t h,
                                              const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    ESP_RETURN_ON_FALSE(adapter_ctx, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    ESP_RETURN_ON_FALSE(ctx->session_number > 0, ESP_ERR_INVALID_STATE, TAG,
                        "no active session");
    return matter_end_session_impl(ctx, mpd_xml, mpd_len);
}

static uint16_t matter_adapter_get_segment_number(ingest_transport_handle_t h)
{
    if (!h) return 0;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    if (!adapter_ctx) return 0;

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    return ctx ? ctx->segment_number : 0;
}

static void matter_adapter_deinit(ingest_transport_handle_t h)
{
    if (!h) return;

    struct ingest_transport_ctx *transport_ctx = (struct ingest_transport_ctx *)h;
    struct matter_adapter_ctx *adapter_ctx = (struct matter_adapter_ctx *)transport_ctx->adapter_ctx;
    if (!adapter_ctx) return;

    struct matter_ctx *ctx = adapter_ctx->matter_ctx;
    if (ctx) {
        if (ctx->http) {
            esp_http_client_cleanup(ctx->http);
        }
        free(ctx);
    }
    free(adapter_ctx);
    transport_ctx->adapter_ctx = NULL;
}

const ingest_transport_ops_t *matter_adapter_get_ops(void)
{
    return &matter_adapter_ops;
}
