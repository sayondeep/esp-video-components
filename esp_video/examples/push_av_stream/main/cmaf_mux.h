/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file cmaf_mux.h
 * @brief Lightweight CMAF / fragmented-MP4 muxer for H.264
 *
 * Produces two kinds of ISO BMFF output from a raw H.264 Annex-B bitstream:
 *
 *  - Init segment  (`.init`):  ftyp + moov
 *  - Media segment (`.m4s`):   styp + moof + mdat
 *
 * The caller accumulates encoded frames with cmaf_mux_add_frame() and
 * calls cmaf_mux_flush_segment() whenever a segment boundary is reached.
 * Internally the muxer converts Annex-B start codes to AVCC 4-byte
 * length prefixes and builds the complete ISO BMFF box tree.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of H.264 frames that can be accumulated per segment. */
#define CMAF_MUX_MAX_FRAMES_PER_SEGMENT  128

/** Internal AVCC data buffer size (bytes, allocated from PSRAM). */
#define CMAF_MUX_SEG_BUF_SIZE  (2 * 1024 * 1024)

typedef struct cmaf_mux_ctx *cmaf_mux_handle_t;

/**
 * @brief Video stream parameters required to build the init segment.
 */
typedef struct {
    uint32_t width;                   /**< Encoded frame width  (pixels) */
    uint32_t height;                  /**< Encoded frame height (pixels) */
    uint32_t timescale;               /**< Media time scale, e.g. 90000 Hz */
    uint32_t default_sample_duration; /**< Frame duration in timescale ticks,
                                           e.g. 3000 at 90 kHz = 30 fps */
    const uint8_t *sps_data;          /**< SPS NALU bytes (no Annex-B start code) */
    size_t    sps_size;               /**< SPS byte count */
    const uint8_t *pps_data;          /**< PPS NALU bytes (no Annex-B start code) */
    size_t    pps_size;               /**< PPS byte count */
} cmaf_mux_params_t;

/**
 * @brief Initialise the CMAF muxer.
 *
 * SPS/PPS are copied internally; the caller may free them after this call.
 * The internal AVCC accumulation buffer (~256 KB) is allocated from PSRAM
 * when available, otherwise from internal heap.
 *
 * @param params Video stream parameters (must contain valid SPS/PPS).
 * @param handle Output handle, set on success.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_mux_init(const cmaf_mux_params_t *params, cmaf_mux_handle_t *handle);

/**
 * @brief Generate the init segment (ftyp + moov).
 *
 * Must be called once after cmaf_mux_init() and uploaded to the server
 * before any media segments.
 *
 * @param[out] out_buf  Allocated output buffer; caller must free().
 * @param[out] out_size Number of valid bytes in out_buf.
 * @return ESP_OK or an error code.
 */
esp_err_t cmaf_mux_generate_init_segment(cmaf_mux_handle_t h,
                                          uint8_t **out_buf, size_t *out_size);

/**
 * @brief Add one H.264 access unit (Annex-B encoded frame) to the buffer.
 *
 * SPS (NAL type 7) and PPS (NAL type 8) are silently skipped because they
 * are carried in the init segment's avcC box.  All other NAL units are
 * stored in AVCC format (4-byte big-endian length prefix).
 *
 * @param annex_b  Pointer to Annex-B H.264 data (start-code prefixed).
 * @param size     Byte length of annex_b.
 * @param is_sync  True when the frame is an IDR access unit.
 * @return ESP_OK, ESP_ERR_NO_MEM (buffer full), or ESP_ERR_INVALID_STATE.
 */
esp_err_t cmaf_mux_add_frame(cmaf_mux_handle_t h,
                              const uint8_t *annex_b, size_t size, bool is_sync);

/**
 * @brief Flush accumulated frames into a complete fMP4 media segment.
 *
 * Produces `styp + moof + mdat`.  After a successful return the internal
 * accumulation state is reset and the next call to cmaf_mux_add_frame()
 * starts a new segment.
 *
 * @param sequence_number   Monotonically increasing segment number
 *                          (starts at 1001 per the Matter/DASH spec).
 * @param decode_time_ticks Base-media decode time of the first sample,
 *                          expressed in timescale ticks.
 * @param[out] out_buf  Allocated output buffer; caller must free().
 * @param[out] out_size Number of valid bytes in out_buf.
 * @return ESP_OK, ESP_ERR_NO_MEM, or ESP_ERR_INVALID_STATE (no frames).
 */
esp_err_t cmaf_mux_flush_segment(cmaf_mux_handle_t h,
                                  uint32_t sequence_number,
                                  uint64_t decode_time_ticks,
                                  uint8_t **out_buf, size_t *out_size);

/**
 * @brief Return the number of frames currently buffered in this segment.
 */
uint32_t cmaf_mux_pending_frame_count(cmaf_mux_handle_t h);

/**
 * @brief Scan an Annex-B H.264 buffer for SPS and PPS NAL units.
 *
 * Used by the main loop to extract codec parameters from the first IDR
 * frame before initialising the muxer.  The returned pointers point
 * into the original @p data buffer and are only valid for its lifetime.
 *
 * @param[out] sps_start  Pointer into data at the first byte of the SPS
 *                        NALU (after start code), or NULL if not found.
 * @param[out] sps_len    Byte length of the SPS NALU.
 * @param[out] pps_start  Pointer into data at the first byte of the PPS
 *                        NALU (after start code), or NULL if not found.
 * @param[out] pps_len    Byte length of the PPS NALU.
 */
void cmaf_mux_extract_sps_pps(const uint8_t *data, size_t size,
                               const uint8_t **sps_start, size_t *sps_len,
                               const uint8_t **pps_start, size_t *pps_len);

/**
 * @brief Return true if any NAL unit in the Annex-B buffer is IDR (type 5).
 */
bool cmaf_mux_frame_is_idr(const uint8_t *annex_b, size_t size);

/**
 * @brief Deinitialise the muxer and free all internal memory.
 */
void cmaf_mux_deinit(cmaf_mux_handle_t h);

#ifdef __cplusplus
}
#endif
