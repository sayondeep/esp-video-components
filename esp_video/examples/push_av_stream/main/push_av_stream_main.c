/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file push_av_stream_main.c
 * @brief Push AV Stream Transport Example — CMAF over HTTPS
 *
 * Captures H.264 video from the camera, wraps it into fragmented MP4 (CMAF)
 * segments, and pushes them to a push_av_server via HTTPS PUT requests that
 * follow the Matter PushAVStreamTransport / DASH Interface-2 ingest protocol.
 *
 * Upload sequence per session:
 *   1. POST  /streams?interface=dash               → get stream_id
 *   2. PUT   …/session_1/index.mpd   (dynamic)    → start session
 *   3. PUT   …/session_1/<track>/<track>.init      → init segment
 *   4. PUT   …/session_1/<track>/segment_1001.m4s  → media segments
 *      … repeat …
 *   5. PUT   …/session_1/index.mpd   (static)     → close session
 *
 * Testing against push_av_server:
 *   # Start server (use --no-strict if not providing client certs)
 *   cd connectedhomeip/src/tools/push_av_server
 *   python server.py --working-directory ~/.pavstest --server-ip <PC_IP>
 *
 *   # With mTLS enabled, generate device certs first:
 *   curl -k -XPOST https://<PC_IP>:1234/certs/esp32/keypair > keypair.json
 *   # Then extract and copy PEM files to main/certs/ and rebuild.
 *
 *   # View uploaded segments in the server UI:
 *   https://<PC_IP>:1234/ui/streams
 */

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "example_video_common.h"

#include "cmaf_mux.h"
#include "ingest_transport.h"
#include "mpd_gen.h"

/* =========================================================================
 * Configuration macros (from Kconfig)
 * ========================================================================= */

#define VIDEO_BUFFER_COUNT           2
#define VIDEO_ENCODER_BUFFER_COUNT   1
#define SKIP_STARTUP_FRAME_COUNT     2

#define ENCODE_DEV_PATH              ESP_VIDEO_H264_DEVICE_NAME
#define ENCODE_OUTPUT_FORMAT         V4L2_PIX_FMT_H264

/** H.264 media timescale (90 kHz is standard for video RTP/MP4) */
#define VIDEO_TIMESCALE              90000

/** Nominal frame rate numerator */
#define VIDEO_FRAMERATE              30

/** Default sample duration: 90000 / 30 = 3000 ticks */
#define DEFAULT_SAMPLE_DURATION      (VIDEO_TIMESCALE / VIDEO_FRAMERATE)

/** Segment duration in timescale ticks (Kconfig: frames × ticks/frame) */
#define SEGMENT_DURATION_TICKS \
    ((uint32_t)(CONFIG_EXAMPLE_FRAMES_PER_SEGMENT) * DEFAULT_SAMPLE_DURATION)

/** CMAF codecs string (placeholder; updated from SPS profile/level at runtime) */
#define CODECS_STR_DEFAULT           "avc1.4D001F"

static const char *TAG = "push_av";

/* =========================================================================
 * Embedded TLS certificates (placed in main/certs/ by the user)
 * ========================================================================= */

/* server_root_ca_pem is always embedded for TLS server certificate verification */
extern const char server_root_ca_pem_start[]
    asm("_binary_server_root_ca_pem_start");
#if CONFIG_EXAMPLE_USE_MTLS
extern const char client_cert_pem_start[]
    asm("_binary_client_cert_pem_start");
extern const char client_key_pem_start[]
    asm("_binary_client_key_pem_start");
#endif

/* =========================================================================
 * Context
 * ========================================================================= */

/* Queue item for asynchronous segment uploads */
typedef struct {
    uint8_t  *data;      /**< Segment data (allocated, caller transfers ownership) */
    size_t    size;      /**< Segment size in bytes */
    uint16_t  seg_num;   /**< Segment number for logging */
} upload_queue_item_t;

typedef struct {
    /* V4L2 capture */
    int      cap_fd;
    uint8_t *cap_buffer[VIDEO_BUFFER_COUNT];

    /* V4L2 M2M H.264 encoder */
    int      m2m_fd;
    uint8_t *m2m_cap_buffer;

    /* Frame geometry */
    uint32_t width;
    uint32_t height;

    /* CMAF modules */
    cmaf_mux_handle_t  mux;
    ingest_transport_handle_t transport;

    /* Asynchronous upload queue */
    QueueHandle_t upload_queue;      /**< FreeRTOS queue for pending segment uploads */
    TaskHandle_t  upload_task_handle; /**< Handle to the upload task */
    SemaphoreHandle_t http_mutex;    /**< Mutex to protect HTTP client (not thread-safe) */

    /* Decode-time tracking */
    uint64_t segment_base_decode_time; /**< decode time of first frame in current seg */
    uint64_t total_decode_time;        /**< running counter across all frames */

    /* Stats */
    uint32_t total_segments;
    int64_t  stream_start_us;
    bool     upload_task_running;     /**< Flag to signal upload task to exit */
} push_av_ctx_t;

/* =========================================================================
 * Video device helpers (unchanged from original)
 * ========================================================================= */

static void print_video_device_info(const struct v4l2_capability *cap)
{
    ESP_LOGI(TAG, "driver: %s  card: %s  bus: %s  version: %d.%d.%d",
             cap->driver, cap->card, cap->bus_info,
             (uint16_t)(cap->version >> 16),
             (uint8_t)(cap->version >>  8),
             (uint8_t)cap->version);
}

static esp_err_t set_codec_control(int fd, uint32_t ctrl_class,
                                   uint32_t id, int32_t value)
{
    struct v4l2_ext_controls controls = {0};
    struct v4l2_ext_control  control[1] = {0};

    controls.ctrl_class = ctrl_class;
    controls.count      = 1;
    controls.controls   = control;
    control[0].id       = id;
    control[0].value    = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "set_codec_control id=%" PRIu32 " failed", id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* =========================================================================
 * Camera + encoder pipeline (unchanged from original)
 * ========================================================================= */

static esp_err_t init_capture_video(push_av_ctx_t *ctx)
{
    struct v4l2_capability cap;
    ctx->cap_fd = open(EXAMPLE_CAM_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(ctx->cap_fd >= 0, ESP_FAIL, TAG, "open camera failed");
    ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QUERYCAP, &cap));
    print_video_device_info(&cap);
    return ESP_OK;
}

static esp_err_t init_codec_video(push_av_ctx_t *ctx)
{
    struct v4l2_capability cap;
    ctx->m2m_fd = open(ENCODE_DEV_PATH, O_RDONLY);
    ESP_RETURN_ON_FALSE(ctx->m2m_fd >= 0, ESP_FAIL, TAG, "open encoder failed");
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QUERYCAP, &cap));
    print_video_device_info(&cap);

    set_codec_control(ctx->m2m_fd, V4L2_CID_CODEC_CLASS,
                      V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                      CONFIG_EXAMPLE_H264_I_PERIOD);
    set_codec_control(ctx->m2m_fd, V4L2_CID_CODEC_CLASS,
                      V4L2_CID_MPEG_VIDEO_BITRATE,
                      CONFIG_EXAMPLE_H264_BITRATE);
    set_codec_control(ctx->m2m_fd, V4L2_CID_CODEC_CLASS,
                      V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                      CONFIG_EXAMPLE_H264_MIN_QP);
    set_codec_control(ctx->m2m_fd, V4L2_CID_CODEC_CLASS,
                      V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                      CONFIG_EXAMPLE_H264_MAX_QP);
    return ESP_OK;
}

static esp_err_t pipeline_start(push_av_ctx_t *ctx)
{
    int type;
    struct v4l2_buffer     buf;
    struct v4l2_format     format;
    struct v4l2_requestbuffers req;
    struct v4l2_format     init_format;

    /* Query current camera format to get resolution */
    memset(&init_format, 0, sizeof(init_format));
    init_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(ioctl(ctx->cap_fd, VIDIOC_G_FMT, &init_format),
                        TAG, "VIDIOC_G_FMT");
    ctx->width  = init_format.fmt.pix.width;
    ctx->height = init_format.fmt.pix.height;
    ESP_LOGI(TAG, "resolution: %" PRIu32 "x%" PRIu32, ctx->width, ctx->height);

    uint32_t cap_fmt = V4L2_PIX_FMT_YUV420;

    /* Configure camera */
    memset(&format, 0, sizeof(format));
    format.type              = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width     = ctx->width;
    format.fmt.pix.height    = ctx->height;
    format.fmt.pix.pixelformat = cap_fmt;
    ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = VIDEO_BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_REQBUFS, &req));

    for (int i = 0; i < VIDEO_BUFFER_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QUERYBUF, &buf));
        ctx->cap_buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, ctx->cap_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(ctx->cap_buffer[i], ESP_ERR_NO_MEM, TAG, "mmap");
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QBUF, &buf));
    }

    /* Configure M2M encoder – output (raw YUV input) */
    memset(&format, 0, sizeof(format));
    format.type              = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width     = ctx->width;
    format.fmt.pix.height    = ctx->height;
    format.fmt.pix.pixelformat = cap_fmt;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = VIDEO_ENCODER_BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_REQBUFS, &req));

    /* Configure M2M encoder – capture (H.264 output) */
    memset(&format, 0, sizeof(format));
    format.type              = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width     = ctx->width;
    format.fmt.pix.height    = ctx->height;
    format.fmt.pix.pixelformat = ENCODE_OUTPUT_FORMAT;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_REQBUFS, &req));

    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = 0;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QUERYBUF, &buf));
    ctx->m2m_cap_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->m2m_fd, buf.m.offset);
    ESP_RETURN_ON_FALSE(ctx->m2m_cap_buffer, ESP_ERR_NO_MEM, TAG, "m2m mmap");
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &buf));

    /* Start streams */
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_STREAMON, &type));

    /* Drain startup frames for sensor AE settling */
    for (int i = 0; i < SKIP_STARTUP_FRAME_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_DQBUF, &buf));
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QBUF, &buf));
    }
    ESP_LOGI(TAG, "pipeline started");
    return ESP_OK;
}

static void pipeline_stop(push_av_ctx_t *ctx)
{
    int type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->cap_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
}

/* =========================================================================
 * Encode one frame  (V4L2 M2M workflow)
 * ========================================================================= */

static esp_err_t get_encoded_frame(push_av_ctx_t *ctx,
                                   uint8_t **out_buf, size_t *out_size)
{
    struct v4l2_buffer cap_buf, m2m_out_buf, m2m_cap_buf;

    /* 1. Dequeue raw frame from camera */
    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_ERROR(ioctl(ctx->cap_fd, VIDIOC_DQBUF, &cap_buf),
                        TAG, "cam DQBUF");

    /* 2. Feed raw frame to encoder */
    memset(&m2m_out_buf, 0, sizeof(m2m_out_buf));
    m2m_out_buf.index      = 0;
    m2m_out_buf.type       = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    m2m_out_buf.memory     = V4L2_MEMORY_USERPTR;
    m2m_out_buf.m.userptr  = (unsigned long)ctx->cap_buffer[cap_buf.index];
    m2m_out_buf.length     = cap_buf.bytesused;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &m2m_out_buf));

    /* 3. Dequeue encoded H.264 */
    memset(&m2m_cap_buf, 0, sizeof(m2m_cap_buf));
    m2m_cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf));

    /* Return camera buffer */
    ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QBUF, &cap_buf));
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &m2m_out_buf));

    *out_buf  = ctx->m2m_cap_buffer;
    *out_size = m2m_cap_buf.bytesused;
    return ESP_OK;
}

static void return_encoded_frame(push_av_ctx_t *ctx)
{
    struct v4l2_buffer m2m_cap_buf = {
        .index  = 0,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &m2m_cap_buf));
}

/* =========================================================================
 * Build a printable DASH codecs string from SPS header bytes
 * "avc1.PPCCLL"  PP=profile CC=constraints LL=level
 * ========================================================================= */

static void build_codecs_str(const uint8_t *sps, size_t sps_size,
                              char *out, size_t out_size)
{
    if (sps && sps_size >= 4) {
        snprintf(out, out_size, "avc1.%02X%02X%02X",
                 sps[1], sps[2], sps[3]);
    } else {
        strncpy(out, CODECS_STR_DEFAULT, out_size - 1);
    }
}

/* =========================================================================
 * Wait for first IDR frame and extract SPS / PPS
 * ========================================================================= */

static esp_err_t wait_for_idr_and_extract_params(push_av_ctx_t *ctx,
                                                   uint8_t **sps_buf,
                                                   size_t   *sps_len,
                                                   uint8_t **pps_buf,
                                                   size_t   *pps_len)
{
    ESP_LOGI(TAG, "Waiting for first IDR frame with SPS/PPS…");

    for (int attempts = 0; attempts < 120; attempts++) {
        uint8_t *data = NULL;
        size_t   size = 0;

        esp_err_t ret = get_encoded_frame(ctx, &data, &size);
        if (ret != ESP_OK || size == 0) {
            return_encoded_frame(ctx);
            vTaskDelay(pdMS_TO_TICKS(33));
            continue;
        }

        if (!cmaf_mux_frame_is_idr(data, size)) {
            return_encoded_frame(ctx);
            continue;
        }

        const uint8_t *sps_ptr = NULL, *pps_ptr = NULL;
        size_t         sps_sz  = 0,     pps_sz  = 0;
        cmaf_mux_extract_sps_pps(data, size,
                                  &sps_ptr, &sps_sz,
                                  &pps_ptr, &pps_sz);

        if (!sps_ptr || !pps_ptr || sps_sz < 4 || pps_sz < 1) {
            ESP_LOGW(TAG, "IDR frame without SPS/PPS, retrying");
            return_encoded_frame(ctx);
            continue;
        }

        /* Copy SPS/PPS before returning the encoder buffer */
        *sps_buf = malloc(sps_sz);
        *pps_buf = malloc(pps_sz);
        if (!*sps_buf || !*pps_buf) {
            free(*sps_buf); free(*pps_buf);
            return_encoded_frame(ctx);
            return ESP_ERR_NO_MEM;
        }
        memcpy(*sps_buf, sps_ptr, sps_sz);
        memcpy(*pps_buf, pps_ptr, pps_sz);
        *sps_len = sps_sz;
        *pps_len = pps_sz;

        ESP_LOGI(TAG, "IDR found: SPS=%zu B  PPS=%zu B", sps_sz, pps_sz);

        /* Return the frame - it will be captured again in the normal streaming
         * loop after the muxer is initialized */
        return_encoded_frame(ctx);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "timeout waiting for IDR frame");
    return ESP_FAIL;
}

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static void upload_task(void *arg);

/* =========================================================================
 * CMAF / push setup
 * ========================================================================= */

static esp_err_t setup_cmaf(push_av_ctx_t *ctx,
                             const uint8_t *sps, size_t sps_len,
                             const uint8_t *pps, size_t pps_len)
{
    /* Initialise fMP4 muxer */
    cmaf_mux_params_t mux_params = {
        .width                  = ctx->width,
        .height                 = ctx->height,
        .timescale              = VIDEO_TIMESCALE,
        .default_sample_duration = DEFAULT_SAMPLE_DURATION,
        .sps_data               = sps,
        .sps_size               = sps_len,
        .pps_data               = pps,
        .pps_size               = pps_len,
    };
    ESP_RETURN_ON_ERROR(cmaf_mux_init(&mux_params, &ctx->mux),
                        TAG, "cmaf_mux_init");

    /* Initialise ingest transport */
    ingest_transport_config_t transport_cfg = {
        .server_type = INGEST_SERVER_MATTER,
        .server_host = CONFIG_EXAMPLE_SERVER_HOST,
        .server_port = CONFIG_EXAMPLE_SERVER_PORT,
        .track_name  = CONFIG_EXAMPLE_TRACK_NAME,
        .auth = {
#if CONFIG_EXAMPLE_USE_MTLS
            .method = INGEST_AUTH_MTLS,
            .credentials = {
                .mtls = {
                    .server_ca_pem   = server_root_ca_pem_start,
                    .client_cert_pem = client_cert_pem_start,
                    .client_key_pem  = client_key_pem_start,
                }
            }
#else
            .method = INGEST_AUTH_NONE,
            .credentials = {0}
#endif
        }
    };
    ESP_RETURN_ON_ERROR(ingest_transport_init(&transport_cfg, &ctx->transport),
                        TAG, "ingest_transport_init");

    /* Create upload queue (hold up to 5 segments to buffer network delays) */
    ctx->upload_queue = xQueueCreate(5, sizeof(upload_queue_item_t));
    ESP_RETURN_ON_FALSE(ctx->upload_queue, ESP_ERR_NO_MEM, TAG,
                        "Failed to create upload queue");

    /* Create mutex to protect HTTP client (not thread-safe) */
    ctx->http_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(ctx->http_mutex, ESP_ERR_NO_MEM, TAG,
                        "Failed to create HTTP mutex");

    /* Initialize upload task flag */
    ctx->upload_task_running = true;

    /* Create upload task */
    BaseType_t ret = xTaskCreate(upload_task, "upload_task", 8192, ctx,
                                 5, &ctx->upload_task_handle);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "Failed to create upload task");

    return ESP_OK;
}

/* =========================================================================
 * Asynchronous upload task
 * ========================================================================= */

static void upload_task(void *arg)
{
    push_av_ctx_t *ctx = (push_av_ctx_t *)arg;
    upload_queue_item_t item;
    TickType_t timeout = pdMS_TO_TICKS(1000); /* 1 second timeout */

    ESP_LOGI(TAG, "Upload task started");

    while (ctx->upload_task_running) {
        /* Wait for a segment to upload */
        if (xQueueReceive(ctx->upload_queue, &item, timeout) == pdTRUE) {
            /* Take mutex before using HTTP client */
            if (xSemaphoreTake(ctx->http_mutex, portMAX_DELAY) == pdTRUE) {
                /* Upload the segment */
                esp_err_t err = ingest_transport_upload_media_segment(ctx->transport, item.data, item.size);
                xSemaphoreGive(ctx->http_mutex);

                if (err == ESP_OK) {
                    ctx->total_segments++;
                    ESP_LOGD(TAG, "Uploaded segment #%"PRIu16" (%zu B)", item.seg_num, item.size);
                } else {
                    ESP_LOGW(TAG, "Upload segment #%"PRIu16" failed: %s", item.seg_num, esp_err_to_name(err));
                }
            }

            /* Free the segment data (we took ownership when enqueuing) */
            free(item.data);
        }
        /* If timeout, check if we should continue (allows graceful shutdown) */
    }

    /* Drain any remaining items in the queue before exiting */
    while (xQueueReceive(ctx->upload_queue, &item, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Draining queued segment #%"PRIu16" on shutdown", item.seg_num);
        if (xSemaphoreTake(ctx->http_mutex, portMAX_DELAY) == pdTRUE) {
            esp_err_t err = ingest_transport_upload_media_segment(ctx->transport, item.data, item.size);
            xSemaphoreGive(ctx->http_mutex);
            if (err == ESP_OK) {
                ctx->total_segments++;
            }
        }
        free(item.data);
    }

    ESP_LOGI(TAG, "Upload task exiting");
    vTaskDelete(NULL);
}

/* =========================================================================
 * Flush one CMAF segment and enqueue it for asynchronous upload
 * ========================================================================= */

static esp_err_t flush_and_upload_segment(push_av_ctx_t *ctx)
{
    uint8_t *seg  = NULL;
    size_t   size = 0;

    uint16_t seg_num = ingest_transport_get_segment_number(ctx->transport);

    esp_err_t err = cmaf_mux_flush_segment(ctx->mux,
                                            seg_num,
                                            ctx->segment_base_decode_time,
                                            &seg, &size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush_segment failed");
        return err;
    }

    /* Enqueue the segment for asynchronous upload */
    upload_queue_item_t item = {
        .data = seg,      /* Transfer ownership to upload task */
        .size = size,
        .seg_num = seg_num,
    };

    if (xQueueSend(ctx->upload_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* Queue full - this should be rare, but log it */
        ESP_LOGW(TAG, "Upload queue full, dropping segment #%"PRIu16, seg_num);
        free(seg);  /* Free if we can't enqueue */
        return ESP_ERR_NO_MEM;
    }

    /* Update base decode time for the next segment (do this immediately) */
    ctx->segment_base_decode_time = ctx->total_decode_time;

    ESP_LOGD(TAG, "Enqueued segment #%"PRIu16" (%zu B) for upload", seg_num, size);
    return ESP_OK;
}

/* =========================================================================
 * Main streaming task
 * ========================================================================= */

static void push_av_stream_task(void *arg)
{
    push_av_ctx_t *ctx = (push_av_ctx_t *)arg;
    esp_err_t ret;

    /* ------------------------------------------------------------------ */
    /* 1. Wait for IDR + extract SPS/PPS                                   */
    /* ------------------------------------------------------------------ */
    uint8_t *sps = NULL, *pps = NULL;
    size_t   sps_len = 0, pps_len = 0;

    ret = wait_for_idr_and_extract_params(ctx, &sps, &sps_len, &pps, &pps_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to obtain SPS/PPS; aborting");
        vTaskDelete(NULL);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Init CMAF muxer + push client                                    */
    /* ------------------------------------------------------------------ */
    ret = setup_cmaf(ctx, sps, sps_len, pps, pps_len);
    free(sps); free(pps);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CMAF init failed; aborting");
        vTaskDelete(NULL);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 3. Allocate stream on the server                                     */
    /* ------------------------------------------------------------------ */
    if (xSemaphoreTake(ctx->http_mutex, portMAX_DELAY) == pdTRUE) {
        ingest_stream_info_t stream_info;
        ret = ingest_transport_create_stream(ctx->transport, &stream_info);
        xSemaphoreGive(ctx->http_mutex);
    } else {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create_stream failed; aborting");
        vTaskDelete(NULL);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Build MPD parameters for this stream                             */
    /* ------------------------------------------------------------------ */
    char codecs_str[24];
    /* We need SPS bytes to build the codecs string; re-read from mux params
     * which stored an internal copy of SPS. We can just use the config. */
    strncpy(codecs_str, CODECS_STR_DEFAULT, sizeof(codecs_str) - 1);

    mpd_params_t mpd_p = {
        .width        = ctx->width,
        .height       = ctx->height,
        .framerate    = VIDEO_FRAMERATE,
        .timescale    = VIDEO_TIMESCALE,
        .seg_duration = SEGMENT_DURATION_TICKS,
        .start_number = 1001,  /* Matter/CMAF spec requires segment numbers start at 1001 */
        .track_name   = CONFIG_EXAMPLE_TRACK_NAME,
        .codecs       = codecs_str,
        .bandwidth    = (uint32_t)CONFIG_EXAMPLE_H264_BITRATE,
    };

    /* ------------------------------------------------------------------ */
    /* 5. Upload dynamic MPD → start session                               */
    /* ------------------------------------------------------------------ */
    char mpd_buf[2048];
    size_t mpd_len = 0;

    ret = mpd_gen_dynamic(mpd_buf, sizeof(mpd_buf), &mpd_p, &mpd_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mpd_gen_dynamic failed");
        vTaskDelete(NULL);
        return;
    }
    if (xSemaphoreTake(ctx->http_mutex, portMAX_DELAY) == pdTRUE) {
        ret = ingest_transport_start_session(ctx->transport, mpd_buf, mpd_len);
        xSemaphoreGive(ctx->http_mutex);
    } else {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_session failed");
        vTaskDelete(NULL);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 6. Generate and upload init segment                                  */
    /* ------------------------------------------------------------------ */
    uint8_t *init_seg  = NULL;
    size_t   init_size = 0;

    ret = cmaf_mux_generate_init_segment(ctx->mux, &init_seg, &init_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "generate_init_segment failed");
        vTaskDelete(NULL);
        return;
    }
    if (xSemaphoreTake(ctx->http_mutex, portMAX_DELAY) == pdTRUE) {
        ret = ingest_transport_upload_init_segment(ctx->transport, init_seg, init_size);
        xSemaphoreGive(ctx->http_mutex);
    } else {
        ret = ESP_FAIL;
    }
    free(init_seg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "upload_init_segment failed");
        vTaskDelete(NULL);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 7. Initialize decode time tracking for the first segment */
    /* ------------------------------------------------------------------ */
    ctx->segment_base_decode_time = 0;
    ctx->total_decode_time = 0;

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  CMAF push started!");
    ESP_LOGI(TAG, "  Resolution : %" PRIu32 "x%" PRIu32, ctx->width, ctx->height);
    ESP_LOGI(TAG, "  Server     : %s:%d",
             CONFIG_EXAMPLE_SERVER_HOST, CONFIG_EXAMPLE_SERVER_PORT);
    ESP_LOGI(TAG, "  Track      : %s", CONFIG_EXAMPLE_TRACK_NAME);
    ESP_LOGI(TAG, "  Seg frames : %d  (~%.1f s)",
             CONFIG_EXAMPLE_FRAMES_PER_SEGMENT,
             (float)CONFIG_EXAMPLE_FRAMES_PER_SEGMENT / VIDEO_FRAMERATE);
    ESP_LOGI(TAG, "  mTLS       : %s",
#if CONFIG_EXAMPLE_USE_MTLS
             "enabled"
#else
             "disabled"
#endif
             );
    ESP_LOGI(TAG, "==============================================");

    ctx->stream_start_us = esp_timer_get_time();
    int64_t  last_log_us = ctx->stream_start_us;
    uint32_t log_frames  = 0;

    /* ------------------------------------------------------------------ */
    /* 8. Streaming loop                                                    */
    /* ------------------------------------------------------------------ */
    while (1) {
        uint8_t *h264_data = NULL;
        size_t   h264_size = 0;

#if CONFIG_EXAMPLE_STREAM_DURATION_SEC > 0
        int64_t now_us = esp_timer_get_time();
        if ((now_us - ctx->stream_start_us) >
            ((int64_t)CONFIG_EXAMPLE_STREAM_DURATION_SEC * 1000000LL)) {
            ESP_LOGI(TAG, "stream duration limit reached");
            break;
        }
#endif

        ret = get_encoded_frame(ctx, &h264_data, &h264_size);
        if (ret != ESP_OK || h264_size == 0) {
            ESP_LOGW(TAG, "get_encoded_frame failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool is_idr = cmaf_mux_frame_is_idr(h264_data, h264_size);

        /* Add frame to current segment buffer */
        ret = cmaf_mux_add_frame(ctx->mux, h264_data, h264_size, is_idr);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "add_frame failed: %s", esp_err_to_name(ret));
        } else {
            ctx->total_decode_time += DEFAULT_SAMPLE_DURATION;
        }

        return_encoded_frame(ctx);
        log_frames++;

        /* Flush segment when enough frames have accumulated */
        if (cmaf_mux_pending_frame_count(ctx->mux) >=
            (uint32_t)CONFIG_EXAMPLE_FRAMES_PER_SEGMENT) {
            flush_and_upload_segment(ctx);
        }

        /* Log FPS and segment stats every 5 seconds */
        int64_t now_log = esp_timer_get_time();
        if ((now_log - last_log_us) >= 5000000LL) {
            float elapsed = (float)(now_log - last_log_us) / 1e6f;
            ESP_LOGI(TAG, "FPS: %.1f  total segments: %" PRIu32,
                     (float)log_frames / elapsed, ctx->total_segments);
            log_frames  = 0;
            last_log_us = now_log;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. Flush any remaining frames into a final segment                   */
    /* ------------------------------------------------------------------ */
    if (cmaf_mux_pending_frame_count(ctx->mux) > 0) {
        ESP_LOGI(TAG, "Flushing final partial segment");
        flush_and_upload_segment(ctx);
    }

    /* ------------------------------------------------------------------ */
    /* 9.5. Wait for upload queue to drain before uploading static MPD      */
    /*      (HTTP client is not thread-safe, must wait for upload task)     */
    /* ------------------------------------------------------------------ */
    if (ctx->upload_queue) {
        ESP_LOGI(TAG, "Waiting for queued uploads to complete...");
        TickType_t wait_start = xTaskGetTickCount();
        while (uxQueueMessagesWaiting(ctx->upload_queue) > 0 &&
               (xTaskGetTickCount() - wait_start) < pdMS_TO_TICKS(30000)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (uxQueueMessagesWaiting(ctx->upload_queue) > 0) {
            ESP_LOGW(TAG, "Timeout waiting for upload queue to drain");
        } else {
            ESP_LOGI(TAG, "Upload queue drained");
            /* Small delay to ensure any in-flight HTTP operations complete */
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    /* ------------------------------------------------------------------ */
    /* 10. Upload static MPD → close session                               */
    /* ------------------------------------------------------------------ */
    /* Calculate duration from actual segments uploaded, not wall-clock time.
     * Each segment is SEGMENT_DURATION_TICKS / VIDEO_TIMESCALE seconds.
     * For 30 fps and 30 frames/segment: 90000 ticks / 90000 Hz = 1 second/segment */
    uint32_t total_sec = ctx->total_segments * (SEGMENT_DURATION_TICKS / VIDEO_TIMESCALE);
    if (total_sec == 0) total_sec = 1;

    ret = mpd_gen_static(mpd_buf, sizeof(mpd_buf), &mpd_p, total_sec, &mpd_len);
    if (ret == ESP_OK && ctx->http_mutex) {
        /* Take mutex before using HTTP client for static MPD upload */
        if (xSemaphoreTake(ctx->http_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
            ingest_transport_end_session(ctx->transport, mpd_buf, mpd_len);
            xSemaphoreGive(ctx->http_mutex);
        } else {
            ESP_LOGW(TAG, "Timeout waiting for HTTP mutex to upload static MPD");
        }
    }

    int64_t total_us = esp_timer_get_time() - ctx->stream_start_us;
    uint32_t wall_clock_sec = (uint32_t)(total_us / 1000000ULL);
    ESP_LOGI(TAG, "Stream ended: %" PRIu32 " segments (%" PRIu32 " s content, %" PRIu32 " s wall-clock)",
             ctx->total_segments, total_sec, wall_clock_sec);

    /* ------------------------------------------------------------------ */
    /* 11. Cleanup: stop upload task and wait for queued uploads to finish */
    /* ------------------------------------------------------------------ */
    if (ctx->upload_task_handle) {
        ESP_LOGI(TAG, "Stopping upload task and waiting for queued uploads...");
        ctx->upload_task_running = false;

        /* Wait for upload task to drain the queue and exit (max 10 seconds) */
        /* The task will delete itself after draining */
        vTaskDelay(pdMS_TO_TICKS(10000));

        /* Delete the queue (task should have exited by now) */
        if (ctx->upload_queue) {
            vQueueDelete(ctx->upload_queue);
            ctx->upload_queue = NULL;
        }

        /* Delete the mutex */
        if (ctx->http_mutex) {
            vSemaphoreDelete(ctx->http_mutex);
            ctx->http_mutex = NULL;
        }
    }

    vTaskDelete(NULL);
}

/* =========================================================================
 * Application entry point
 * ========================================================================= */

void app_main(void)
{
    esp_err_t ret;

    /* NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Video subsystem – provides XCLK to camera sensor */
    ESP_ERROR_CHECK(example_video_init());

    /* Network */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    /* Allocate context */
    push_av_ctx_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx);

    /* Open devices */
    ESP_ERROR_CHECK(init_capture_video(ctx));
    ESP_ERROR_CHECK(init_codec_video(ctx));

    /* Start V4L2 pipeline */
    ESP_ERROR_CHECK(pipeline_start(ctx));

    /* Launch streaming task on the highest available priority */
    xTaskCreate(push_av_stream_task, "push_av", 12288, ctx, 5, NULL);
}
