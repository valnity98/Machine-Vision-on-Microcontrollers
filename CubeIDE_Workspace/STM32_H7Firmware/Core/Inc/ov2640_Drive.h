/*
 * ov2640_Drive.h
 *
 * OV2640 camera driver for STM32H7 DCMI + SCCB/I2C.
 */

#ifndef INC_OV2640_DRIVE_H_
#define INC_OV2640_DRIVE_H_

#include "stm32h7xx_hal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit SCCB address; HAL expects it shifted left by 1. */
#ifndef OV2640_I2C_ADDR_7BIT
#define OV2640_I2C_ADDR_7BIT  0x30u
#endif
#define OV2640_I2C_ADDR_HAL   (OV2640_I2C_ADDR_7BIT << 1)

/* SCCB register bank select and bank IDs. */
#define OV2640_REG_BANK_SEL   0xFFu
#define OV2640_BANK_DSP       0x00u
#define OV2640_BANK_SENSOR    0x01u

/* Commonly used DSP registers. */
#define OV2640_DSP_IMAGE_MODE 0xDAu

/* DSP digital-zoom output window registers. */
#define OV2640_DSP_ZMOW  0x5Au  /* output width[7:0]  */
#define OV2640_DSP_ZMOH  0x5Bu  /* output height[7:0] */
#define OV2640_DSP_ZMHH  0x5Cu  /* width[9:8] | height[9:8] packed */

/* Sensor registers. */
#define OV2640_SENSOR_PIDH    0x0Au
#define OV2640_SENSOR_PIDL    0x0Bu
#define OV2640_SENSOR_COM7    0x12u
#define OV2640_SENSOR_COM10   0x15u

/* Maximum DMA transfer length (STM32 DMA counter is 16-bit). */
#ifndef OV2640_DCMI_DMA_MAX_WORDS
#define OV2640_DCMI_DMA_MAX_WORDS 65535u
#endif

/* Bytes per pixel for RGB565. */
#define OV2640_RGB565_BPP 2u

/* Register list entry; {0xFF, 0xFF} marks end-of-list. */
typedef struct {
    uint8_t reg;
    uint8_t val;
} ov2640_reg_t;

#define OV2640_REG_LIST_END ((ov2640_reg_t){0xFFu, 0xFFu})

typedef enum {
    OV2640_RES_QQVGA_160x120 = 0,
    OV2640_RES_QVGA_320x240  = 1,
    OV2640_RES_WQVGA_480x272 = 2,
    OV2640_RES_VGA_640x480   = 3,
    OV2640_RES_SVGA_800x600  = 4,
    OV2640_RES_XGA_1024x768  = 5,
    OV2640_RES_SXGA_1280x960 = 6
} ov2640_resolution_t;

typedef enum {
    OV2640_EFFECT_ANTIQUE = 0,
    OV2640_EFFECT_BLUISH,
    OV2640_EFFECT_GREENISH,
    OV2640_EFFECT_REDDISH,
    OV2640_EFFECT_BW,
    OV2640_EFFECT_NEGATIVE,
    OV2640_EFFECT_NEGATIVE_BW,
    OV2640_EFFECT_NORMAL
} ov2640_effect_t;

typedef enum {
    OV2640_LEVEL_MINUS2 = -2,
    OV2640_LEVEL_MINUS1 = -1,
    OV2640_LEVEL_0      =  0,
    OV2640_LEVEL_PLUS1  =  1,
    OV2640_LEVEL_PLUS2  =  2
} ov2640_level_t;

typedef enum {
    OV2640_LIGHT_AUTO = 0,
    OV2640_LIGHT_SUNNY,
    OV2640_LIGHT_CLOUDY,
    OV2640_LIGHT_OFFICE,
    OV2640_LIGHT_HOME
} ov2640_lightmode_t;

typedef enum {
    OV2640_PIXFMT_JPEG   = 0,
    OV2640_PIXFMT_RGB565 = 1
} ov2640_pixformat_t;

typedef struct {
    I2C_HandleTypeDef  *hi2c;
    DCMI_HandleTypeDef *hdcmi;
    uint16_t            i2c_addr;
    GPIO_TypeDef       *rst_port;
    uint16_t            rst_pin;
} ov2640_t;

/* Initialisation. */
HAL_StatusTypeDef ov2640_init(ov2640_t *dev);
HAL_StatusTypeDef ov2640_reset_hw(ov2640_t *dev);
HAL_StatusTypeDef ov2640_read_id(ov2640_t *dev, uint8_t *pid, uint8_t *ver);

/* Low-level SCCB access. */
HAL_StatusTypeDef ov2640_sccb_write(ov2640_t *dev, uint8_t reg, uint8_t val);
HAL_StatusTypeDef ov2640_sccb_read(ov2640_t *dev, uint8_t reg, uint8_t *val);
HAL_StatusTypeDef ov2640_write_reg(ov2640_t *dev, uint8_t bank, uint8_t reg, uint8_t val);
HAL_StatusTypeDef ov2640_read_reg(ov2640_t *dev, uint8_t bank, uint8_t reg, uint8_t *val);
HAL_StatusTypeDef ov2640_apply_reglist(ov2640_t *dev, const ov2640_reg_t *list,
                                       uint32_t delay_ms);

/* Format and resolution. */
HAL_StatusTypeDef ov2640_set_pixformat(ov2640_t *dev, ov2640_pixformat_t fmt);
HAL_StatusTypeDef ov2640_set_resolution_JPEG(ov2640_t *dev, ov2640_resolution_t res);
HAL_StatusTypeDef ov2640_set_resolution_rgb(ov2640_t *dev, ov2640_resolution_t res);
HAL_StatusTypeDef ov2640_jpeg_enable(ov2640_t *dev, uint8_t enable);
HAL_StatusTypeDef ov2640_dcmi_set_jpeg_mode(ov2640_t *dev, uint8_t enable);

/* Image controls. */
HAL_StatusTypeDef ov2640_set_brightness(ov2640_t *dev, ov2640_level_t level);
HAL_StatusTypeDef ov2640_set_contrast(ov2640_t *dev, ov2640_level_t level);
HAL_StatusTypeDef ov2640_set_saturation(ov2640_t *dev, ov2640_level_t level);
HAL_StatusTypeDef ov2640_set_effect(ov2640_t *dev, ov2640_effect_t effect);
HAL_StatusTypeDef ov2640_set_lightmode(ov2640_t *dev, ov2640_lightmode_t mode);
HAL_StatusTypeDef ov2640_set_awb_simple(ov2640_t *dev, uint8_t enable);
HAL_StatusTypeDef ov2640_set_zoom(ov2640_t *dev, uint16_t out_w, uint16_t out_h);

/* Size helpers. */
uint32_t ov2640_rgb565_bytes(uint16_t width, uint16_t height);
uint32_t ov2640_words_from_bytes(uint32_t bytes);

/* DCMI capture. */
HAL_StatusTypeDef ov2640_dcmi_stop(ov2640_t *dev);
HAL_StatusTypeDef ov2640_capture_snapshot_framedone(ov2640_t *dev,
                                                    uint32_t dst,
                                                    uint32_t length_words,
                                                    uint32_t timeout_ms);
HAL_StatusTypeDef ov2640_capture_rgb565_frame(ov2640_t *dev,
                                              uint32_t dst,
                                              uint16_t width,
                                              uint16_t height,
                                              uint32_t timeout_ms);
HAL_StatusTypeDef ov2640_capture_continuous_start(ov2640_t *dev,
                                                  uint32_t dst,
                                                  uint32_t length_words);
HAL_StatusTypeDef ov2640_wait_continuous_frame(ov2640_t *dev,
                                               uint32_t dst,
                                               uint32_t length_words,
                                               uint32_t timeout_ms);
HAL_StatusTypeDef ov2640_capture_continuous_stop(ov2640_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* INC_OV2640_DRIVE_H_ */
