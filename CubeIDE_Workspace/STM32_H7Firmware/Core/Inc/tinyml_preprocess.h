/*
 * tinyml_preprocess.h
 *
 * RGB565 to uint8 HWC tensor preprocessing for MobileNetV1 0.25 (quantised input).
 *
 * Contract (matches Zoo user_config.yaml):
 *   1. Full-frame nearest-neighbour resize to TINYML_INPUT_W x TINYML_INPUT_H.
 *      Integer-floor mapping: sx = ox * src_w / W.
 *   2. Expand each RGB565 pixel:
 *        R8 = ((p >> 11) & 0x1F) * 255 / 31
 *        G8 = ((p >>  5) & 0x3F) * 255 / 63
 *        B8 = ( p        & 0x1F) * 255 / 31
 *   3. Write HWC: dst[oy * W * 3 + ox * 3 + c].
 *
 * The model expects uint8 input with QLinear(1/127.5, 127).
 * MobileNet normalisation is embedded in the first Conv2D by X-CUBE-AI,
 * so raw [0..255] uint8 values are passed directly — no float conversion.
 */

#ifndef INC_TINYML_PREPROCESS_H_
#define INC_TINYML_PREPROCESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TINYML_INPUT_W
#define TINYML_INPUT_W        96u
#endif

#ifndef TINYML_INPUT_H
#define TINYML_INPUT_H        96u
#endif

#ifndef TINYML_INPUT_CHANNELS
#define TINYML_INPUT_CHANNELS 3u
#endif

#define TINYML_INPUT_ELEMENTS (TINYML_INPUT_W * TINYML_INPUT_H * TINYML_INPUT_CHANNELS)
#define TINYML_INPUT_BYTES    (TINYML_INPUT_ELEMENTS * (uint32_t)sizeof(uint8_t))

/*
 * Convert a row-major RGB565 frame to a uint8 HWC tensor.
 * dst must point to TINYML_INPUT_ELEMENTS bytes allocated by the caller.
 * On NULL or zero-dimension input, dst is zeroed.
 */
void tinyml_preprocess_rgb565_to_u8rgb(const uint16_t *src,
                                       uint16_t        width,
                                       uint16_t        height,
                                       uint8_t        *dst);

#ifdef __cplusplus
}
#endif

#endif /* INC_TINYML_PREPROCESS_H_ */
