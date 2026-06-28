/*
 * tinyml_preprocess.c
 *
 * RGB565 -> uint8 HWC tensor preprocessing for MobileNetV1 0.25.
 *
 * Resize mode: full-frame stretch (no letterboxing).
 * Every output pixel (ox, oy) maps to source pixel via integer-floor:
 *   sx = ox * width  / 96
 *   sy = oy * height / 96
 * This matches predict_count_tflite.py on the PC side; the FNV-1a hash
 * comparison verifies both use identical preprocessing.
 *
 * Note: the training config uses aspect_ratio: fit (letterboxing on 480x272
 * training images). This creates a known preprocessing difference between
 * training and inference, documented as a limitation in the thesis.
 *
 * Byte-swap note:
 *   OV2640 outputs RGB565 big-endian over DCMI. The STM32 DMA stores pixels
 *   little-endian in memory. Reading frame_buf as uint16_t* on a little-endian
 *   Cortex-M7 yields the same bit pattern as the GUI (image_utils.py), which
 *   also reads the raw bytes little-endian. BYTESWAP = 0 is therefore correct.
 *
 *   Set TINYML_PREPROCESS_BYTESWAP = 1 only if OV2640 register 0xDA was changed
 *   to force big-endian output AND the training dataset was captured without a
 *   compensating GUI swap. Verify by comparing TM_IN FNV-1a hashes on STM32
 *   versus a PC-side hash of the same frame resized to 96x96 (NEAREST).
 */

#include "tinyml_preprocess.h"
#include <stddef.h>
#include <string.h>

#ifndef TINYML_PREPROCESS_BYTESWAP
#define TINYML_PREPROCESS_BYTESWAP 0
#endif

/* Expand one RGB565 pixel to 8-bit per channel. */
static void rgb565_expand(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((uint32_t)((p >> 11) & 0x1Fu) * 255u) / 31u);
    *g = (uint8_t)(((uint32_t)((p >>  5) & 0x3Fu) * 255u) / 63u);
    *b = (uint8_t)(((uint32_t)( p        & 0x1Fu) * 255u) / 31u);
}

void tinyml_preprocess_rgb565_to_u8rgb(const uint16_t *src,
                                       uint16_t        width,
                                       uint16_t        height,
                                       uint8_t        *dst)
{
    if ((src == NULL) || (dst == NULL) ||
        (width == 0u) || (height == 0u)) {
        if (dst != NULL) {
            memset(dst, 0, TINYML_INPUT_ELEMENTS);
        }
        return;
    }

    for (uint16_t oy = 0u; oy < TINYML_INPUT_H; ++oy) {
        for (uint16_t ox = 0u; ox < TINYML_INPUT_W; ++ox) {
            uint16_t sx = (uint16_t)((uint32_t)ox * width  / TINYML_INPUT_W);
            uint16_t sy = (uint16_t)((uint32_t)oy * height / TINYML_INPUT_H);

            /* Clamp against potential edge rounding. */
            if (sx >= width)  { sx = (uint16_t)(width  - 1u); }
            if (sy >= height) { sy = (uint16_t)(height - 1u); }

            uint16_t p = src[(uint32_t)sy * width + sx];

#if TINYML_PREPROCESS_BYTESWAP
            p = (uint16_t)((p >> 8u) | (p << 8u));
#endif
            uint8_t  r, g, b;
            rgb565_expand(p, &r, &g, &b);

            uint32_t base = ((uint32_t)oy * TINYML_INPUT_W + ox) * TINYML_INPUT_CHANNELS;
            dst[base + 0u] = r;
            dst[base + 1u] = g;
            dst[base + 2u] = b;
        }
    }
}
