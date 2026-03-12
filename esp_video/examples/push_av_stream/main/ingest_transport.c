/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file ingest_transport.c
 * @brief Ingest transport implementation - dispatches to adapters
 *
 * This file implements the ingest_transport interface by dispatching
 * to the appropriate adapter based on server type.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "ingest_transport.h"

/* Include adapter headers */
#include "ingest_adapters/matter_adapter.h"
#include "ingest_adapters/mistserver_adapter.h"
#include "ingest_adapters/srs_adapter.h"

static const char *TAG = "ingest_transport";

/**
 * @brief Transport context structure
 */
struct ingest_transport_ctx {
    void *adapter_ctx;  /**< Adapter-specific context */
    const ingest_transport_ops_t *ops; /**< Adapter operations */
};

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

esp_err_t ingest_transport_init(const ingest_transport_config_t *config,
                                 ingest_transport_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && handle, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    /* Allocate transport context */
    struct ingest_transport_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc transport_ctx");

    /* Select adapter based on server type */
    const ingest_transport_ops_t *ops = NULL;
    switch (config->server_type) {
        case INGEST_SERVER_MATTER:
            ops = matter_adapter_get_ops();
            break;
        case INGEST_SERVER_MISTSERVER:
            ops = mistserver_adapter_get_ops();
            break;
        case INGEST_SERVER_WOWZA:
            /* SRS adapter - reuse Wowza type for SRS */
            ops = srs_adapter_get_ops();
            break;
        case INGEST_SERVER_AWS:
            /* TODO: Implement AWS adapter */
            ESP_LOGE(TAG, "AWS adapter not yet implemented");
            free(ctx);
            return ESP_ERR_NOT_SUPPORTED;
        case INGEST_SERVER_AZURE:
            /* TODO: Implement Azure adapter */
            ESP_LOGE(TAG, "Azure adapter not yet implemented");
            free(ctx);
            return ESP_ERR_NOT_SUPPORTED;
        case INGEST_SERVER_GENERIC:
            /* TODO: Implement generic adapter */
            ESP_LOGE(TAG, "Generic adapter not yet implemented");
            free(ctx);
            return ESP_ERR_NOT_SUPPORTED;
        default:
            ESP_LOGE(TAG, "Unknown server type: %d", config->server_type);
            free(ctx);
            return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_FALSE(ops, ESP_ERR_NOT_SUPPORTED, TAG, "no adapter for server type");

    /* Store ops before init */
    ctx->ops = ops;

    /* Initialize adapter */
    esp_err_t err = ops->init((ingest_transport_handle_t)ctx, config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Adapter init failed: %s", esp_err_to_name(err));
        free(ctx);
        return err;
    }

    *handle = (ingest_transport_handle_t)ctx;
    return ESP_OK;
}

esp_err_t ingest_transport_create_stream(ingest_transport_handle_t h,
                                          ingest_stream_info_t *stream_info)
{
    ESP_RETURN_ON_FALSE(h && stream_info, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    ESP_RETURN_ON_FALSE(ctx->ops, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    return ctx->ops->create_stream(h, stream_info);
}

esp_err_t ingest_transport_start_session(ingest_transport_handle_t h,
                                          const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    ESP_RETURN_ON_FALSE(ctx->ops, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    return ctx->ops->start_session(h, mpd_xml, mpd_len);
}

esp_err_t ingest_transport_upload_init_segment(ingest_transport_handle_t h,
                                                 const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    ESP_RETURN_ON_FALSE(ctx->ops, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    return ctx->ops->upload_init_segment(h, data, size);
}

esp_err_t ingest_transport_upload_media_segment(ingest_transport_handle_t h,
                                                  const uint8_t *data, size_t size, uint32_t presentation_time)
{
    ESP_RETURN_ON_FALSE(h && data && size, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    ESP_RETURN_ON_FALSE(ctx->ops, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    return ctx->ops->upload_media_segment(h, data, size, presentation_time);
}

esp_err_t ingest_transport_end_session(ingest_transport_handle_t h,
                                       const char *mpd_xml, size_t mpd_len)
{
    ESP_RETURN_ON_FALSE(h && mpd_xml && mpd_len, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    ESP_RETURN_ON_FALSE(ctx->ops, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    return ctx->ops->end_session(h, mpd_xml, mpd_len);
}

uint16_t ingest_transport_get_segment_number(ingest_transport_handle_t h)
{
    if (!h) return 0;

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    if (!ctx->ops) return 0;

    return ctx->ops->get_segment_number(h);
}

void ingest_transport_deinit(ingest_transport_handle_t h)
{
    if (!h) return;

    struct ingest_transport_ctx *ctx = (struct ingest_transport_ctx *)h;
    if (ctx->ops && ctx->ops->deinit) {
        ctx->ops->deinit(h);
    }
    free(ctx);
}
