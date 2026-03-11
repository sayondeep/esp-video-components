/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file ingest_transport.h
 * @brief Abstract interface for CMAF/DASH ingest to various media servers
 *
 * This interface abstracts the differences between various media server
 * implementations (Matter push_av_server, MistServer, AWS MediaLive, etc.)
 * allowing the same CMAF muxer to work with different backends.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ingest_transport_ctx *ingest_transport_handle_t;

/**
 * @brief Server type identifier
 */
typedef enum {
    INGEST_SERVER_MATTER,      /**< Matter push_av_server (DASH Interface-2) */
    INGEST_SERVER_MISTSERVER,  /**< MistServer (CMAF push) */
    INGEST_SERVER_AWS,         /**< AWS Elemental MediaLive */
    INGEST_SERVER_AZURE,       /**< Azure Media Services */
    INGEST_SERVER_WOWZA,       /**< Wowza Streaming Engine */
    INGEST_SERVER_GENERIC,     /**< Generic DASH-IF compliant server */
} ingest_server_type_t;

/**
 * @brief Authentication method
 */
typedef enum {
    INGEST_AUTH_NONE,          /**< No authentication */
    INGEST_AUTH_MTLS,          /**< Mutual TLS (client certificate) */
    INGEST_AUTH_API_KEY,       /**< API key in header */
    INGEST_AUTH_BEARER_TOKEN,  /**< OAuth Bearer token */
    INGEST_AUTH_BASIC,         /**< HTTP Basic Auth */
    INGEST_AUTH_AWS_SIGV4,     /**< AWS Signature Version 4 */
} ingest_auth_method_t;

/**
 * @brief Authentication credentials (union of different auth types)
 */
typedef struct {
    ingest_auth_method_t method;
    union {
        struct {
            const char *server_ca_pem;
            const char *client_cert_pem;
            const char *client_key_pem;
        } mtls;
        struct {
            const char *api_key;
            const char *header_name;  /* e.g., "X-API-Key" */
        } api_key;
        struct {
            const char *token;
        } bearer;
        struct {
            const char *username;
            const char *password;
        } basic;
        struct {
            const char *access_key_id;
            const char *secret_access_key;
            const char *region;
        } aws_sigv4;
    } credentials;
} ingest_auth_t;

/**
 * @brief URL template configuration for generic servers
 */
typedef struct {
    const char *base_url;              /**< Base URL: https://server:port */
    const char *stream_create_path;    /**< Path for stream creation, e.g., "/api/streams" */
    const char *mpd_path_template;   /**< MPD path: "/streams/{stream_id}/session_{session}/index.mpd" */
    const char *init_path_template;   /**< Init path: "/streams/{stream_id}/session_{session}/{track}/{track}.init" */
    const char *segment_path_template;/**< Segment path: "/streams/{stream_id}/session_{session}/{track}/segment_{num}.m4s" */
    const char *mpd_content_type;     /**< Content-Type for MPD, e.g., "application/dash+xml" */
    const char *init_content_type;    /**< Content-Type for init, e.g., "video/mp4" */
    const char *segment_content_type; /**< Content-Type for segments, e.g., "video/iso.segment" */
    const char **custom_headers;      /**< NULL-terminated array of "Header: Value" strings */
} ingest_url_config_t;

/**
 * @brief Transport configuration
 */
typedef struct {
    ingest_server_type_t server_type;
    const char *server_host;
    uint16_t server_port;
    const char *track_name;

    /* Authentication */
    ingest_auth_t auth;

    /* Server-specific configuration */
    union {
        /* For GENERIC server type */
        ingest_url_config_t url_config;

        /* For AWS */
        struct {
            const char *channel_id;
            const char *ingest_endpoint;
        } aws;

        /* For Azure */
        struct {
            const char *account_name;
            const char *streaming_locator;
        } azure;
    } server_config;
} ingest_transport_config_t;

/**
 * @brief Stream creation response (server-specific)
 */
typedef struct {
    int stream_id;              /**< Numeric stream ID (Matter, generic) */
    char stream_key[128];        /**< String stream key (MistServer, AWS) */
    char ingest_url[256];        /**< Full ingest URL (some servers) */
} ingest_stream_info_t;

/**
 * @brief Function table for transport operations
 */
typedef struct {
    /**
     * @brief Initialize transport (create HTTP client, authenticate, etc.)
     */
    esp_err_t (*init)(ingest_transport_handle_t h, const ingest_transport_config_t *config);

    /**
     * @brief Create/allocate a stream on the server
     * @param[out] stream_info Populated with stream identifier
     */
    esp_err_t (*create_stream)(ingest_transport_handle_t h, ingest_stream_info_t *stream_info);

    /**
     * @brief Start a new session (upload dynamic MPD if required)
     */
    esp_err_t (*start_session)(ingest_transport_handle_t h, const char *mpd_xml, size_t mpd_len);

    /**
     * @brief Upload init segment
     */
    esp_err_t (*upload_init_segment)(ingest_transport_handle_t h, const uint8_t *data, size_t size);

    /**
     * @brief Upload media segment
     */
    esp_err_t (*upload_media_segment)(ingest_transport_handle_t h, const uint8_t *data, size_t size);

    /**
     * @brief End session (upload static MPD if required)
     */
    esp_err_t (*end_session)(ingest_transport_handle_t h, const char *mpd_xml, size_t mpd_len);

    /**
     * @brief Get current segment number
     */
    uint16_t (*get_segment_number)(ingest_transport_handle_t h);

    /**
     * @brief Deinitialize and cleanup
     */
    void (*deinit)(ingest_transport_handle_t h);
} ingest_transport_ops_t;

/**
 * @brief Initialize ingest transport with specified adapter
 *
 * @param config Transport configuration
 * @param handle Output handle
 * @return ESP_OK on success
 */
esp_err_t ingest_transport_init(const ingest_transport_config_t *config,
                                 ingest_transport_handle_t *handle);

/**
 * @brief Convenience wrappers (delegate to adapter functions)
 */
esp_err_t ingest_transport_create_stream(ingest_transport_handle_t h,
                                          ingest_stream_info_t *stream_info);
esp_err_t ingest_transport_start_session(ingest_transport_handle_t h,
                                          const char *mpd_xml, size_t mpd_len);
esp_err_t ingest_transport_upload_init_segment(ingest_transport_handle_t h,
                                                 const uint8_t *data, size_t size);
esp_err_t ingest_transport_upload_media_segment(ingest_transport_handle_t h,
                                                  const uint8_t *data, size_t size);
esp_err_t ingest_transport_end_session(ingest_transport_handle_t h,
                                       const char *mpd_xml, size_t mpd_len);
uint16_t ingest_transport_get_segment_number(ingest_transport_handle_t h);
void ingest_transport_deinit(ingest_transport_handle_t h);

#ifdef __cplusplus
}
#endif
