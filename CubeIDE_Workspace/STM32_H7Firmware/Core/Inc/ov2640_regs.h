/*
 * ov2640_regs.h
 *
 * External OV2640 register-list declarations.
 * Definitions are in ov2640_regs.c.
 */

#ifndef INC_OV2640_REGS_H_
#define INC_OV2640_REGS_H_

#include "ov2640_Drive.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ov2640_reg_t OV2640_BRIGHTNESS2[];
extern const ov2640_reg_t OV2640_BRIGHTNESS1[];
extern const ov2640_reg_t OV2640_BRIGHTNESS0[];
extern const ov2640_reg_t OV2640_BRIGHTNESS_1[];
extern const ov2640_reg_t OV2640_BRIGHTNESS_2[];

extern const ov2640_reg_t OV2640_CONTRAST2[];
extern const ov2640_reg_t OV2640_CONTRAST1[];
extern const ov2640_reg_t OV2640_CONTRAST0[];
extern const ov2640_reg_t OV2640_CONTRAST_1[];
extern const ov2640_reg_t OV2640_CONTRAST_2[];

extern const ov2640_reg_t OV2640_SATURATION2[];
extern const ov2640_reg_t OV2640_SATURATION1[];
extern const ov2640_reg_t OV2640_SATURATION0[];
extern const ov2640_reg_t OV2640_SATURATION_1[];
extern const ov2640_reg_t OV2640_SATURATION_2[];

extern const ov2640_reg_t OV2640_JPEG_INIT[];
extern const ov2640_reg_t OV2640_YUV422[];
extern const ov2640_reg_t OV2640_JPEG[];

extern const ov2640_reg_t OV2640_160x120_JPEG[];
extern const ov2640_reg_t OV2640_320x240_JPEG[];
extern const ov2640_reg_t OV2640_640x480_JPEG[];
extern const ov2640_reg_t OV2640_800x600_JPEG[];
extern const ov2640_reg_t OV2640_1024x768_JPEG[];
extern const ov2640_reg_t OV2640_1280x960_JPEG[];

extern const ov2640_reg_t OV2640_160x120_RGB[];
extern const ov2640_reg_t OV2640_320x240_RGB[];
extern const ov2640_reg_t OV2640_480x272_RGB[];
extern const ov2640_reg_t OV2640_640x480_RGB[];

extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_NORMAL[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_ANTIQUE[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_BLACK_NEGATIVE[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_BLUISH[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_BLACK[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_NEGATIVE[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_GREENISH[];
extern const ov2640_reg_t OV2640_SPECIAL_EFFECTS_REDDISH[];

extern const ov2640_reg_t OV2640_LIGHT_MODE_AUTO[];
extern const ov2640_reg_t OV2640_LIGHT_MODE_SUNNY[];
extern const ov2640_reg_t OV2640_LIGHT_MODE_CLOUDY[];
extern const ov2640_reg_t OV2640_LIGHT_MODE_OFFICE[];
extern const ov2640_reg_t OV2640_LIGHT_MODE_HOME[];

#ifdef __cplusplus
}
#endif

#endif /* INC_OV2640_REGS_H_ */
