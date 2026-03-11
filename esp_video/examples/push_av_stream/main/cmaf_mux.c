/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file cmaf_mux.c
 * @brief CMAF / fMP4 muxer implementation
 *
 * Builds ISO BMFF boxes (big-endian byte order) using a lightweight
 * streaming box-writer.  All multi-byte fields are written MSB-first.
 *
 * Init segment layout:  ftyp  moov{mvhd trak{tkhd mdia{mdhd hdlr
 *                              minf{vmhd dinf stbl{stsd{avc1{avcC}}
 *                              stts stsc stsz stco}}} mvex{trex}}
 *
 * Media segment layout: styp  moof{mfhd traf{tfhd tfdt trun}}
 *                        mdat{<avcc-samples>}
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "cmaf_mux.h"

static const char *TAG = "cmaf_mux";

/* =========================================================================
 * Per-frame metadata stored during accumulation
 * ========================================================================= */

typedef struct {
    uint32_t avcc_size; /**< Total AVCC byte count for this sample */
    uint32_t duration;  /**< In timescale ticks */
    bool     is_sync;   /**< IDR (random-access) frame */
} frame_info_t;

/* =========================================================================
 * Muxer context
 * ========================================================================= */

struct cmaf_mux_ctx {
    cmaf_mux_params_t params;

    /* Copies of SPS/PPS (params.sps_data / pps_data point here) */
    uint8_t *sps_copy;
    uint8_t *pps_copy;

    /* AVCC sample data accumulation buffer (from PSRAM when available) */
    uint8_t *seg_buf;
    size_t   seg_buf_pos;

    /* Per-frame metadata */
    frame_info_t frames[CMAF_MUX_MAX_FRAMES_PER_SEGMENT];
    uint32_t     frame_count;
};

/* =========================================================================
 * Lightweight ISO BMFF box writer
 * ========================================================================= */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
} bw_t;

static inline void bw_init(bw_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->pos = 0; w->overflow = false;
}

static void bw_raw(bw_t *w, const void *data, size_t n)
{
    if (w->overflow || w->pos + n > w->cap) {
        w->overflow = true;
        return;
    }
    if (data) {
        memcpy(w->buf + w->pos, data, n);
    } else {
        memset(w->buf + w->pos, 0, n);
    }
    w->pos += n;
}

static inline void bw_zeros(bw_t *w, size_t n) { bw_raw(w, NULL, n); }

static inline void bw_u8(bw_t *w, uint8_t v)
{
    bw_raw(w, &v, 1);
}

static inline void bw_u16(bw_t *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    bw_raw(w, b, 2);
}

static inline void bw_u32(bw_t *w, uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)(v >> 24), (uint8_t)(v >> 16),
        (uint8_t)(v >>  8), (uint8_t)v
    };
    bw_raw(w, b, 4);
}

static inline void bw_u64(bw_t *w, uint64_t v)
{
    bw_u32(w, (uint32_t)(v >> 32));
    bw_u32(w, (uint32_t)v);
}

/**
 * Begin a box: write a 4-byte placeholder size then the 4-byte type tag.
 * Returns the byte offset of the beginning of this box for later patching.
 */
static size_t bw_box_begin(bw_t *w, const char type[4])
{
    size_t start = w->pos;
    bw_zeros(w, 4);        /* size placeholder */
    bw_raw(w, type, 4);
    return start;
}

/** End a box: write the final 4-byte size back at position @p start. */
static void bw_box_end(bw_t *w, size_t start)
{
    if (!w->overflow && start + 8 <= w->pos) {
        uint32_t sz = (uint32_t)(w->pos - start);
        w->buf[start + 0] = (uint8_t)(sz >> 24);
        w->buf[start + 1] = (uint8_t)(sz >> 16);
        w->buf[start + 2] = (uint8_t)(sz >>  8);
        w->buf[start + 3] = (uint8_t)sz;
    }
}

/* =========================================================================
 * H.264 Annex-B helpers
 * ========================================================================= */

/** NAL unit types of interest */
#define NAL_SPS   7
#define NAL_PPS   8
#define NAL_IDR   5

static inline uint8_t nal_type(const uint8_t *nalu) { return nalu[0] & 0x1F; }

/**
 * Find the next H.264 NAL unit in an Annex-B buffer.
 * Returns a pointer to the first byte AFTER the start code and sets
 * *sc_len to 3 or 4.  Returns NULL when no more NAL units are found.
 */
static const uint8_t *annexb_next_nalu(const uint8_t *data, size_t size,
                                       int *sc_len)
{
    for (size_t i = 0; i + 2 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *sc_len = 3;
                return data + i + 3;
            }
            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *sc_len = 4;
                return data + i + 4;
            }
        }
    }
    return NULL;
}

/* =========================================================================
 * Public: SPS/PPS extraction and IDR detection
 * ========================================================================= */

void cmaf_mux_extract_sps_pps(const uint8_t *data, size_t size,
                               const uint8_t **sps_start, size_t *sps_len,
                               const uint8_t **pps_start, size_t *pps_len)
{
    *sps_start = NULL; *sps_len = 0;
    *pps_start = NULL; *pps_len = 0;

    int sc_len = 0;
    const uint8_t *nalu = annexb_next_nalu(data, size, &sc_len);

    while (nalu) {
        size_t offset = (size_t)(nalu - data);
        int    next_sc = 0;
        const uint8_t *next = annexb_next_nalu(nalu, size - offset, &next_sc);
        size_t nalu_size = next
                           ? (size_t)((next - next_sc) - nalu)
                           : (size - offset);

        uint8_t type = nal_type(nalu);
        if (type == NAL_SPS && *sps_start == NULL) {
            *sps_start = nalu;
            *sps_len   = nalu_size;
        } else if (type == NAL_PPS && *pps_start == NULL) {
            *pps_start = nalu;
            *pps_len   = nalu_size;
        }

        if (*sps_start && *pps_start) {
            break;
        }
        nalu = next;
    }
}

bool cmaf_mux_frame_is_idr(const uint8_t *annex_b, size_t size)
{
    int sc_len = 0;
    const uint8_t *nalu = annexb_next_nalu(annex_b, size, &sc_len);
    while (nalu) {
        if (nal_type(nalu) == NAL_IDR) {
            return true;
        }
        size_t offset = (size_t)(nalu - annex_b);
        nalu = annexb_next_nalu(nalu, size - offset, &sc_len);
    }
    return false;
}

/* =========================================================================
 * Init / deinit
 * ========================================================================= */

esp_err_t cmaf_mux_init(const cmaf_mux_params_t *params, cmaf_mux_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(params && handle, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(params->sps_data && params->sps_size >= 4,
                        ESP_ERR_INVALID_ARG, TAG, "SPS missing/too short");
    ESP_RETURN_ON_FALSE(params->pps_data && params->pps_size >= 1,
                        ESP_ERR_INVALID_ARG, TAG, "PPS missing");
    ESP_RETURN_ON_FALSE(params->timescale > 0 && params->default_sample_duration > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid timescale/duration");

    struct cmaf_mux_ctx *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "calloc context");

    ctx->params = *params;

    /* Copy SPS */
    ctx->sps_copy = malloc(params->sps_size);
    if (!ctx->sps_copy) { free(ctx); return ESP_ERR_NO_MEM; }
    memcpy(ctx->sps_copy, params->sps_data, params->sps_size);
    ctx->params.sps_data = ctx->sps_copy;

    /* Copy PPS */
    ctx->pps_copy = malloc(params->pps_size);
    if (!ctx->pps_copy) { free(ctx->sps_copy); free(ctx); return ESP_ERR_NO_MEM; }
    memcpy(ctx->pps_copy, params->pps_data, params->pps_size);
    ctx->params.pps_data = ctx->pps_copy;

    /* Segment accumulation buffer – prefer PSRAM */
    ctx->seg_buf = (uint8_t *)heap_caps_malloc(CMAF_MUX_SEG_BUF_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->seg_buf) {
        ctx->seg_buf = malloc(CMAF_MUX_SEG_BUF_SIZE);   /* internal SRAM fallback */
    }
    if (!ctx->seg_buf) {
        free(ctx->sps_copy);
        free(ctx->pps_copy);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init: %"PRIu32"x%"PRIu32" ts=%"PRIu32" spd=%"PRIu32
             " sps=%zuB pps=%zuB",
             params->width, params->height, params->timescale,
             params->default_sample_duration,
             params->sps_size, params->pps_size);

    *handle = ctx;
    return ESP_OK;
}

void cmaf_mux_deinit(cmaf_mux_handle_t h)
{
    if (!h) return;
    free(h->seg_buf);
    free(h->sps_copy);
    free(h->pps_copy);
    free(h);
}

/* =========================================================================
 * Init segment: ftyp + moov
 * ========================================================================= */

esp_err_t cmaf_mux_generate_init_segment(cmaf_mux_handle_t h,
                                          uint8_t **out_buf, size_t *out_size)
{
    ESP_RETURN_ON_FALSE(h && out_buf && out_size, ESP_ERR_INVALID_ARG, TAG, "NULL");

    const cmaf_mux_params_t *p = &h->params;
    size_t cap = 2048 + p->sps_size + p->pps_size;
    uint8_t *buf = malloc(cap);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc init segment");

    bw_t w;
    bw_init(&w, buf, cap);

    /* ---- ftyp -------------------------------------------------------- */
    {
        size_t s = bw_box_begin(&w, "ftyp");
        bw_raw(&w, "isom", 4); bw_u32(&w, 0x200);
        bw_raw(&w, "isom", 4);
        bw_raw(&w, "iso6", 4);
        bw_raw(&w, "cmf2", 4);
        bw_raw(&w, "mp41", 4);
        bw_box_end(&w, s);
    }

    /* ---- moov -------------------------------------------------------- */
    {
        size_t moov = bw_box_begin(&w, "moov");

        /* mvhd */
        {
            size_t s = bw_box_begin(&w, "mvhd");
            bw_u32(&w, 0);              /* version=0, flags=0 */
            bw_u32(&w, 0);              /* creation_time */
            bw_u32(&w, 0);              /* modification_time */
            bw_u32(&w, p->timescale);
            bw_u32(&w, 0);              /* duration */
            bw_u32(&w, 0x00010000);     /* rate 1.0 */
            bw_u16(&w, 0x0100);         /* volume 1.0 */
            bw_zeros(&w, 10);           /* reserved */
            /* unity matrix */
            bw_u32(&w, 0x00010000); bw_u32(&w, 0); bw_u32(&w, 0);
            bw_u32(&w, 0); bw_u32(&w, 0x00010000); bw_u32(&w, 0);
            bw_u32(&w, 0); bw_u32(&w, 0); bw_u32(&w, 0x40000000);
            bw_zeros(&w, 24);           /* pre_defined */
            bw_u32(&w, 2);              /* next_track_ID */
            bw_box_end(&w, s);
        }

        /* trak */
        {
            size_t trak = bw_box_begin(&w, "trak");

            /* tkhd */
            {
                size_t s = bw_box_begin(&w, "tkhd");
                bw_u32(&w, 0x00000003); /* enabled + in_movie */
                bw_u32(&w, 0); bw_u32(&w, 0); /* ctime, mtime */
                bw_u32(&w, 1);          /* track_ID */
                bw_zeros(&w, 4);
                bw_u32(&w, 0);          /* duration */
                bw_zeros(&w, 8);
                bw_u16(&w, 0); bw_u16(&w, 0); /* layer, alt_group */
                bw_u16(&w, 0); bw_zeros(&w, 2); /* volume, reserved */
                /* unity matrix */
                bw_u32(&w, 0x00010000); bw_u32(&w, 0); bw_u32(&w, 0);
                bw_u32(&w, 0); bw_u32(&w, 0x00010000); bw_u32(&w, 0);
                bw_u32(&w, 0); bw_u32(&w, 0); bw_u32(&w, 0x40000000);
                bw_u32(&w, (uint32_t)(p->width  << 16)); /* width  16.16 fp */
                bw_u32(&w, (uint32_t)(p->height << 16)); /* height 16.16 fp */
                bw_box_end(&w, s);
            }

            /* mdia */
            {
                size_t mdia = bw_box_begin(&w, "mdia");

                /* mdhd */
                {
                    size_t s = bw_box_begin(&w, "mdhd");
                    bw_u32(&w, 0);
                    bw_u32(&w, 0); bw_u32(&w, 0); /* ctime, mtime */
                    bw_u32(&w, p->timescale);
                    bw_u32(&w, 0);          /* duration */
                    bw_u16(&w, 0x55c4);     /* 'und' language */
                    bw_u16(&w, 0);
                    bw_box_end(&w, s);
                }

                /* hdlr */
                {
                    size_t s = bw_box_begin(&w, "hdlr");
                    bw_u32(&w, 0); bw_u32(&w, 0); /* version+flags, pre_defined */
                    bw_raw(&w, "vide", 4);
                    bw_zeros(&w, 12); /* reserved */
                    bw_raw(&w, "VideoHandler\0", 13);
                    bw_box_end(&w, s);
                }

                /* minf */
                {
                    size_t minf = bw_box_begin(&w, "minf");

                    /* vmhd */
                    {
                        size_t s = bw_box_begin(&w, "vmhd");
                        bw_u32(&w, 0x00000001); /* flags=1 */
                        bw_u16(&w, 0); bw_zeros(&w, 6); /* graphicsMode, opcolor */
                        bw_box_end(&w, s);
                    }

                    /* dinf / dref */
                    {
                        size_t dinf = bw_box_begin(&w, "dinf");
                        size_t dref = bw_box_begin(&w, "dref");
                        bw_u32(&w, 0); bw_u32(&w, 1); /* version+flags, entry_count */
                        {
                            size_t url = bw_box_begin(&w, "url ");
                            bw_u32(&w, 0x00000001); /* self-contained */
                            bw_box_end(&w, url);
                        }
                        bw_box_end(&w, dref);
                        bw_box_end(&w, dinf);
                    }

                    /* stbl */
                    {
                        size_t stbl = bw_box_begin(&w, "stbl");

                        /* stsd */
                        {
                            size_t stsd = bw_box_begin(&w, "stsd");
                            bw_u32(&w, 0); bw_u32(&w, 1); /* v+f, entry_count */

                            /* avc1 */
                            {
                                size_t avc1 = bw_box_begin(&w, "avc1");
                                bw_zeros(&w, 6);        /* reserved */
                                bw_u16(&w, 1);          /* data_reference_index */
                                bw_zeros(&w, 16);       /* pre_defined + reserved */
                                bw_u16(&w, (uint16_t)p->width);
                                bw_u16(&w, (uint16_t)p->height);
                                bw_u32(&w, 0x00480000); /* horiz DPI 72 */
                                bw_u32(&w, 0x00480000); /* vert  DPI 72 */
                                bw_zeros(&w, 4);        /* reserved */
                                bw_u16(&w, 1);          /* frame_count */
                                bw_zeros(&w, 32);       /* compressorname */
                                bw_u16(&w, 0x0018);     /* depth */
                                bw_u16(&w, 0xFFFF);     /* pre_defined = -1 */

                                /* avcC */
                                {
                                    size_t avcc = bw_box_begin(&w, "avcC");
                                    bw_u8(&w, 1);                    /* configurationVersion */
                                    bw_u8(&w, p->sps_data[1]);       /* AVCProfileIndication */
                                    bw_u8(&w, p->sps_data[2]);       /* profile_compatibility */
                                    bw_u8(&w, p->sps_data[3]);       /* AVCLevelIndication */
                                    bw_u8(&w, 0xFF);                 /* lengthSizeMinusOne = 3 */
                                    bw_u8(&w, 0xE1);                 /* numSPS = 1 */
                                    bw_u16(&w, (uint16_t)p->sps_size);
                                    bw_raw(&w, p->sps_data, p->sps_size);
                                    bw_u8(&w, 1);                    /* numPPS */
                                    bw_u16(&w, (uint16_t)p->pps_size);
                                    bw_raw(&w, p->pps_data, p->pps_size);
                                    bw_box_end(&w, avcc);
                                }
                                bw_box_end(&w, avc1);
                            }
                            bw_box_end(&w, stsd);
                        }

                        /* Empty mandatory sample table boxes for fragmented MP4 */
                        { size_t s = bw_box_begin(&w, "stts"); bw_u32(&w,0); bw_u32(&w,0); bw_box_end(&w,s); }
                        { size_t s = bw_box_begin(&w, "stsc"); bw_u32(&w,0); bw_u32(&w,0); bw_box_end(&w,s); }
                        { size_t s = bw_box_begin(&w, "stsz"); bw_u32(&w,0); bw_u32(&w,0); bw_u32(&w,0); bw_box_end(&w,s); }
                        { size_t s = bw_box_begin(&w, "stco"); bw_u32(&w,0); bw_u32(&w,0); bw_box_end(&w,s); }

                        bw_box_end(&w, stbl);
                    }
                    bw_box_end(&w, minf);
                }
                bw_box_end(&w, mdia);
            }
            bw_box_end(&w, trak);
        }

        /* mvex / trex */
        {
            size_t mvex = bw_box_begin(&w, "mvex");
            size_t trex = bw_box_begin(&w, "trex");
            bw_u32(&w, 0);              /* version+flags */
            bw_u32(&w, 1);              /* track_ID */
            bw_u32(&w, 1);              /* default_sample_description_index */
            bw_u32(&w, p->default_sample_duration);
            bw_u32(&w, 0);              /* default_sample_size */
            bw_u32(&w, 0x01010000);     /* default_sample_flags (non-sync) */
            bw_box_end(&w, trex);
            bw_box_end(&w, mvex);
        }

        bw_box_end(&w, moov);
    }

    if (w.overflow) {
        free(buf);
        ESP_LOGE(TAG, "init segment buffer overflow (cap=%zu)", cap);
        return ESP_ERR_NO_MEM;
    }

    *out_buf  = buf;
    *out_size = w.pos;
    ESP_LOGI(TAG, "init segment: %zu bytes", w.pos);
    return ESP_OK;
}

/* =========================================================================
 * Frame accumulation
 * ========================================================================= */

esp_err_t cmaf_mux_add_frame(cmaf_mux_handle_t h,
                              const uint8_t *annex_b, size_t size, bool is_sync)
{
    ESP_RETURN_ON_FALSE(h && annex_b && size > 0, ESP_ERR_INVALID_ARG, TAG, "NULL");
    if (h->frame_count >= CMAF_MUX_MAX_FRAMES_PER_SEGMENT) {
        ESP_LOGW(TAG, "frame buffer full – flush first");
        return ESP_ERR_INVALID_STATE;
    }

    size_t frame_avcc_bytes = 0;
    int sc_len = 0;
    const uint8_t *nalu = annexb_next_nalu(annex_b, size, &sc_len);

    while (nalu) {
        size_t offset = (size_t)(nalu - annex_b);
        int    next_sc = 0;
        const uint8_t *next = annexb_next_nalu(nalu, size - offset, &next_sc);
        size_t nalu_size = next
                           ? (size_t)((next - next_sc) - nalu)
                           : (size - offset);

        /* Skip SPS/PPS – they are in the init segment's avcC box */
        uint8_t type = nal_type(nalu);
        if (type != NAL_SPS && type != NAL_PPS && nalu_size > 0) {
            if (h->seg_buf_pos + 4 + nalu_size > CMAF_MUX_SEG_BUF_SIZE) {
                ESP_LOGE(TAG, "seg buf overflow pos=%zu", h->seg_buf_pos);
                return ESP_ERR_NO_MEM;
            }
            /* Write AVCC 4-byte big-endian length prefix */
            uint32_t len32 = (uint32_t)nalu_size;
            h->seg_buf[h->seg_buf_pos++] = (uint8_t)(len32 >> 24);
            h->seg_buf[h->seg_buf_pos++] = (uint8_t)(len32 >> 16);
            h->seg_buf[h->seg_buf_pos++] = (uint8_t)(len32 >>  8);
            h->seg_buf[h->seg_buf_pos++] = (uint8_t)len32;
            memcpy(h->seg_buf + h->seg_buf_pos, nalu, nalu_size);
            h->seg_buf_pos   += nalu_size;
            frame_avcc_bytes += 4 + nalu_size;
        }
        nalu = next;
    }

    if (frame_avcc_bytes == 0) {
        /* Frame had only SPS/PPS – not a real sample, skip it */
        return ESP_OK;
    }

    frame_info_t *f   = &h->frames[h->frame_count++];
    f->avcc_size = (uint32_t)frame_avcc_bytes;
    f->duration  = h->params.default_sample_duration;
    f->is_sync   = is_sync;
    return ESP_OK;
}

uint32_t cmaf_mux_pending_frame_count(cmaf_mux_handle_t h)
{
    return h ? h->frame_count : 0;
}

/* =========================================================================
 * Media segment: styp + moof + mdat
 *
 * Pre-computed moof size (exact, no alignment padding in MP4):
 *   moof header          8
 *   mfhd                16  (8 hdr + 4 v+f + 4 seq_num)
 *   traf header          8
 *   tfhd (0x020000)     16  (8 hdr + 4 v+f + 4 track_id)
 *   tfdt (version=1)    20  (8 hdr + 4 v+f + 8 decode_time)
 *   trun (flags=0x0701) 20 + n*12  (8 hdr + 4 v+f + 4 cnt + 4 offset
 *                                    + n*(4dur + 4size + 4flags))
 *   traf total  = 8+16+20+(20+n*12)   = 64 + n*12
 *   moof total  = 8+16+(64+n*12)      = 88 + n*12
 *
 * data_offset in trun is relative to start of moof (default-base-is-moof):
 *   data_offset = moof_size + 8  (mdat box header: 4 size + 4 'mdat')
 * ========================================================================= */

esp_err_t cmaf_mux_flush_segment(cmaf_mux_handle_t h,
                                  uint32_t sequence_number,
                                  uint64_t decode_time_ticks,
                                  uint8_t **out_buf, size_t *out_size)
{
    ESP_RETURN_ON_FALSE(h && out_buf && out_size, ESP_ERR_INVALID_ARG, TAG, "NULL");
    if (h->frame_count == 0) {
        ESP_LOGW(TAG, "flush_segment: no frames accumulated");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t n         = h->frame_count;
    uint32_t moof_size = 88 + n * 12;
    int32_t  data_off  = (int32_t)(moof_size + 8);

    /* Output buffer: styp(24) + moof(moof_size) + mdat(8 + seg_buf_pos) */
    size_t total_cap = 24 + moof_size + 8 + h->seg_buf_pos + 32 /* padding */;
    uint8_t *buf = malloc(total_cap);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc media segment");

    bw_t w;
    bw_init(&w, buf, total_cap);

    /* ---- styp -------------------------------------------------------- */
    {
        size_t s = bw_box_begin(&w, "styp");
        bw_raw(&w, "msdh", 4); bw_u32(&w, 0);
        bw_raw(&w, "msdh", 4);
        bw_raw(&w, "msix", 4);
        bw_box_end(&w, s);
        /* styp = 24 bytes (verified: 8 hdr + 4+4 major/minor + 2*4 brands) */
    }

    /* ---- moof -------------------------------------------------------- */
    size_t moof_start = w.pos;
    {
        size_t moof = bw_box_begin(&w, "moof");

        /* mfhd */
        {
            size_t s = bw_box_begin(&w, "mfhd");
            bw_u32(&w, 0);               /* version+flags */
            bw_u32(&w, sequence_number);
            bw_box_end(&w, s);
        }

        /* traf */
        {
            size_t traf = bw_box_begin(&w, "traf");

            /* tfhd: flags = 0x020000 (default-base-is-moof) */
            {
                size_t s = bw_box_begin(&w, "tfhd");
                bw_u32(&w, 0x00020000); /* version=0, flags=default-base-is-moof */
                bw_u32(&w, 1);          /* track_ID */
                bw_box_end(&w, s);
            }

            /* tfdt version=1 (64-bit decode time) */
            {
                size_t s = bw_box_begin(&w, "tfdt");
                bw_u32(&w, 0x01000000); /* version=1, flags=0 */
                bw_u64(&w, decode_time_ticks);
                bw_box_end(&w, s);
            }

            /* trun: flags = 0x000701
             *   bit 0:  data-offset-present
             *   bit 8:  sample-duration-present
             *   bit 9:  sample-size-present
             *   bit 10: sample-flags-present
             */
            {
                size_t s = bw_box_begin(&w, "trun");
                bw_u32(&w, 0x00000701);           /* version=0, flags */
                bw_u32(&w, n);                    /* sample_count */
                bw_u32(&w, (uint32_t)data_off);   /* data_offset */
                for (uint32_t i = 0; i < n; i++) {
                    bw_u32(&w, h->frames[i].duration);
                    bw_u32(&w, h->frames[i].avcc_size);
                    /* sync: 0x02000000, non-sync: 0x01010000 */
                    bw_u32(&w, h->frames[i].is_sync ? 0x02000000u : 0x01010000u);
                }
                bw_box_end(&w, s);
            }
            bw_box_end(&w, traf);
        }
        bw_box_end(&w, moof);
    }

    /* Sanity-check precomputed size */
    size_t actual_moof = w.pos - moof_start;
    if (actual_moof != moof_size) {
        ESP_LOGE(TAG, "moof size mismatch: expected %"PRIu32" got %zu",
                 moof_size, actual_moof);
        free(buf);
        return ESP_FAIL;
    }

    /* ---- mdat -------------------------------------------------------- */
    {
        size_t s = bw_box_begin(&w, "mdat");
        bw_raw(&w, h->seg_buf, h->seg_buf_pos);
        bw_box_end(&w, s);
    }

    if (w.overflow) {
        free(buf);
        ESP_LOGE(TAG, "media segment buffer overflow cap=%zu", total_cap);
        return ESP_ERR_NO_MEM;
    }

    /* Reset accumulation state for the next segment */
    h->seg_buf_pos = 0;
    h->frame_count = 0;

    *out_buf  = buf;
    *out_size = w.pos;

    ESP_LOGI(TAG, "seg #%"PRIu32": %"PRIu32" frames  dt=%"PRIu64"  size=%zu B",
             sequence_number, n, decode_time_ticks, w.pos);
    return ESP_OK;
}
