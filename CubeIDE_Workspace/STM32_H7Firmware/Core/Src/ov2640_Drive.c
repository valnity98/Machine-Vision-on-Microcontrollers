/*
 * ov2640_Drive.c
 *
 * OV2640 camera driver for STM32H7 DCMI + SCCB/I2C.
 */

#include "ov2640_Drive.h"
#include "ov2640_regs.h"
#include "FreeRTOS.h"
#include "task.h"
#include "core_cm7.h"


/* ---------------------------------------------------------------------------
 * Module-static DCMI state
 * Flags are set in HAL callbacks (ISR context) and read in task context.
 * ---------------------------------------------------------------------------*/
static volatile uint8_t    g_dcmi_frame_done = 0u;
static volatile uint8_t    g_dcmi_error      = 0u;
static DCMI_HandleTypeDef *g_active_hdcmi    = NULL;

/* ---------------------------------------------------------------------------
 * D-Cache helpers
 * ---------------------------------------------------------------------------*/
static uint32_t cache_aligned_len(uint32_t len)
{
    return (len + 31u) & ~31u;
}

static void cache_clean(uint32_t addr, uint32_t len)
{
    SCB_CleanDCache_by_Addr((uint32_t *)addr, (int32_t)cache_aligned_len(len));
}

static void cache_invalidate(uint32_t addr, uint32_t len)
{
    SCB_InvalidateDCache_by_Addr((uint32_t *)addr, (int32_t)cache_aligned_len(len));
}

/* ---------------------------------------------------------------------------
 * HAL DCMI callbacks (called from ISR)
 * ---------------------------------------------------------------------------*/
void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)
{
    if (hdcmi == g_active_hdcmi) {
        g_dcmi_frame_done = 1u;
    }
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *hdcmi)
{
    if (hdcmi == g_active_hdcmi) {
        g_dcmi_error = 1u;
    }
}

/* ---------------------------------------------------------------------------
 * Size helpers
 * ---------------------------------------------------------------------------*/
uint32_t ov2640_rgb565_bytes(uint16_t width, uint16_t height)
{
    return (uint32_t)width * (uint32_t)height * OV2640_RGB565_BPP;
}

uint32_t ov2640_words_from_bytes(uint32_t bytes)
{
    return (bytes + 3u) / 4u;
}

/* ---------------------------------------------------------------------------
 * SCCB low-level
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_sccb_write(ov2640_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    if ((dev == NULL) || (dev->hi2c == NULL)) { return HAL_ERROR; }
    return HAL_I2C_Master_Transmit(dev->hi2c, dev->i2c_addr, buf, 2, 100);
}

HAL_StatusTypeDef ov2640_sccb_read(ov2640_t *dev, uint8_t reg, uint8_t *val)
{
    HAL_StatusTypeDef st;
    if ((dev == NULL) || (dev->hi2c == NULL) || (val == NULL)) { return HAL_ERROR; }
    st = HAL_I2C_Master_Transmit(dev->hi2c, dev->i2c_addr, &reg, 1, 100);
    if (st != HAL_OK) { return st; }
    return HAL_I2C_Master_Receive(dev->hi2c, dev->i2c_addr, val, 1, 100);
}

/* ---------------------------------------------------------------------------
 * Banked register access
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_write_reg(ov2640_t *dev, uint8_t bank,
                                   uint8_t reg, uint8_t val)
{
    HAL_StatusTypeDef st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, bank);
    if (st != HAL_OK) { return st; }
    return ov2640_sccb_write(dev, reg, val);
}

HAL_StatusTypeDef ov2640_read_reg(ov2640_t *dev, uint8_t bank,
                                  uint8_t reg, uint8_t *val)
{
    HAL_StatusTypeDef st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, bank);
    if (st != HAL_OK) { return st; }
    return ov2640_sccb_read(dev, reg, val);
}

/* ---------------------------------------------------------------------------
 * Register list application
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_apply_reglist(ov2640_t *dev,
                                       const ov2640_reg_t *list,
                                       uint32_t delay_ms)
{
    if ((dev == NULL) || (list == NULL)) { return HAL_ERROR; }

    for (size_t i = 0u; ; ++i) {
        if ((list[i].reg == 0xFFu) && (list[i].val == 0xFFu)) { break; }
        HAL_StatusTypeDef st = ov2640_sccb_write(dev, list[i].reg, list[i].val);
        if (st != HAL_OK) { return st; }
        if (delay_ms != 0u) { HAL_Delay(delay_ms); }
    }
    return HAL_OK;
}

/* ---------------------------------------------------------------------------
 * Reset and initialisation
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_reset_hw(ov2640_t *dev)
{
    if ((dev == NULL) || (dev->rst_port == NULL)) { return HAL_ERROR; }
    HAL_GPIO_WritePin(dev->rst_port, dev->rst_pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(dev->rst_port, dev->rst_pin, GPIO_PIN_SET);
    HAL_Delay(10);
    return HAL_OK;
}

HAL_StatusTypeDef ov2640_init(ov2640_t *dev)
{
    HAL_StatusTypeDef st;

    if ((dev == NULL) || (dev->hi2c == NULL) || (dev->hdcmi == NULL)) {
        return HAL_ERROR;
    }

    if (dev->rst_port != NULL) {
        st = ov2640_reset_hw(dev);
        if (st != HAL_OK) { return st; }
    }

    st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR);
    if (st != HAL_OK) { return st; }

    /* Software reset. */
    st = ov2640_sccb_write(dev, OV2640_SENSOR_COM7, 0x80u);
    if (st != HAL_OK) { return st; }
    HAL_Delay(10);

    return ov2640_dcmi_stop(dev);
}

HAL_StatusTypeDef ov2640_read_id(ov2640_t *dev, uint8_t *pid, uint8_t *ver)
{
    HAL_StatusTypeDef st;
    if ((dev == NULL) || (pid == NULL) || (ver == NULL)) { return HAL_ERROR; }
    st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR);
    if (st != HAL_OK) { return st; }
    st = ov2640_sccb_read(dev, OV2640_SENSOR_PIDH, pid);
    if (st != HAL_OK) { return st; }
    return ov2640_sccb_read(dev, OV2640_SENSOR_PIDL, ver);
}

/* ---------------------------------------------------------------------------
 * Format control
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_jpeg_enable(ov2640_t *dev, uint8_t enable)
{
    uint8_t v = 0u;
    HAL_StatusTypeDef st;

    if (dev == NULL) { return HAL_ERROR; }
    st = ov2640_read_reg(dev, OV2640_BANK_DSP, OV2640_DSP_IMAGE_MODE, &v);
    if (st != HAL_OK) { return st; }

    if (enable) { v |= (1u << 4); } else { v &= (uint8_t)~(1u << 4); }
    return ov2640_write_reg(dev, OV2640_BANK_DSP, OV2640_DSP_IMAGE_MODE, v);
}

HAL_StatusTypeDef ov2640_dcmi_set_jpeg_mode(ov2640_t *dev, uint8_t enable)
{
    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    if (enable) { SET_BIT(dev->hdcmi->Instance->CR, DCMI_CR_JPEG); }
    else        { CLEAR_BIT(dev->hdcmi->Instance->CR, DCMI_CR_JPEG); }
    return HAL_OK;
}

HAL_StatusTypeDef ov2640_set_pixformat(ov2640_t *dev, ov2640_pixformat_t fmt)
{
    if (dev == NULL) { return HAL_ERROR; }
    switch (fmt) {
    case OV2640_PIXFMT_JPEG:
        if (ov2640_jpeg_enable(dev, 1u) != HAL_OK) { return HAL_ERROR; }
        return ov2640_dcmi_set_jpeg_mode(dev, 1u);
    case OV2640_PIXFMT_RGB565:
        if (ov2640_jpeg_enable(dev, 0u) != HAL_OK) { return HAL_ERROR; }
        return ov2640_dcmi_set_jpeg_mode(dev, 0u);
    default:
        return HAL_ERROR;
    }
}

/* ---------------------------------------------------------------------------
 * Resolution
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_set_resolution_JPEG(ov2640_t *dev, ov2640_resolution_t res)
{
    HAL_StatusTypeDef   st;
    const ov2640_reg_t *sel;

    if (dev == NULL) { return HAL_ERROR; }

    st = ov2640_apply_reglist(dev, OV2640_JPEG_INIT, 1u);
    if (st != HAL_OK) { return st; }
    st = ov2640_apply_reglist(dev, OV2640_YUV422, 1u);
    if (st != HAL_OK) { return st; }
    st = ov2640_apply_reglist(dev, OV2640_JPEG, 1u);
    if (st != HAL_OK) { return st; }
    st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR);
    if (st != HAL_OK) { return st; }
    st = ov2640_sccb_write(dev, OV2640_SENSOR_COM10, 0x00u);
    if (st != HAL_OK) { return st; }

    switch (res) {
    case OV2640_RES_QQVGA_160x120: sel = OV2640_160x120_JPEG;  break;
    case OV2640_RES_QVGA_320x240:  sel = OV2640_320x240_JPEG;  break;
    case OV2640_RES_VGA_640x480:   sel = OV2640_640x480_JPEG;  break;
    case OV2640_RES_SVGA_800x600:  sel = OV2640_800x600_JPEG;  break;
    case OV2640_RES_XGA_1024x768:  sel = OV2640_1024x768_JPEG; break;
    case OV2640_RES_SXGA_1280x960: sel = OV2640_1280x960_JPEG; break;
    default:                        sel = OV2640_320x240_JPEG;  break;
    }
    return ov2640_apply_reglist(dev, sel, 1u);
}

HAL_StatusTypeDef ov2640_set_resolution_rgb(ov2640_t *dev, ov2640_resolution_t res)
{
    HAL_StatusTypeDef   st;
    const ov2640_reg_t *sel;

    if (dev == NULL) { return HAL_ERROR; }

    st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR);
    if (st != HAL_OK) { return st; }
    st = ov2640_sccb_write(dev, OV2640_SENSOR_COM7, 0x80u);
    if (st != HAL_OK) { return st; }
    HAL_Delay(10);

    switch (res) {
    case OV2640_RES_QQVGA_160x120: sel = OV2640_160x120_RGB;  break;
    case OV2640_RES_QVGA_320x240:  sel = OV2640_320x240_RGB;  break;
    case OV2640_RES_WQVGA_480x272: sel = OV2640_480x272_RGB;  break;
    case OV2640_RES_VGA_640x480:   sel = OV2640_640x480_RGB;  break;
    default:                        sel = OV2640_320x240_RGB;  break;
    }
    return ov2640_apply_reglist(dev, sel, 1u);
}

/* ---------------------------------------------------------------------------
 * Image controls
 * ---------------------------------------------------------------------------*/
static const ov2640_reg_t *level_to_brightness(ov2640_level_t lvl)
{
    switch (lvl) {
    case OV2640_LEVEL_PLUS2:  return OV2640_BRIGHTNESS2;
    case OV2640_LEVEL_PLUS1:  return OV2640_BRIGHTNESS1;
    case OV2640_LEVEL_MINUS1: return OV2640_BRIGHTNESS_1;
    case OV2640_LEVEL_MINUS2: return OV2640_BRIGHTNESS_2;
    default:                  return OV2640_BRIGHTNESS0;
    }
}

static const ov2640_reg_t *level_to_contrast(ov2640_level_t lvl)
{
    switch (lvl) {
    case OV2640_LEVEL_PLUS2:  return OV2640_CONTRAST2;
    case OV2640_LEVEL_PLUS1:  return OV2640_CONTRAST1;
    case OV2640_LEVEL_MINUS1: return OV2640_CONTRAST_1;
    case OV2640_LEVEL_MINUS2: return OV2640_CONTRAST_2;
    default:                  return OV2640_CONTRAST0;
    }
}

static const ov2640_reg_t *level_to_saturation(ov2640_level_t lvl)
{
    switch (lvl) {
    case OV2640_LEVEL_PLUS2:  return OV2640_SATURATION2;
    case OV2640_LEVEL_PLUS1:  return OV2640_SATURATION1;
    case OV2640_LEVEL_MINUS1: return OV2640_SATURATION_1;
    case OV2640_LEVEL_MINUS2: return OV2640_SATURATION_2;
    default:                  return OV2640_SATURATION0;
    }
}

HAL_StatusTypeDef ov2640_set_brightness(ov2640_t *dev, ov2640_level_t lvl)
{
    if (dev == NULL) { return HAL_ERROR; }
    return ov2640_apply_reglist(dev, level_to_brightness(lvl), 1u);
}

HAL_StatusTypeDef ov2640_set_contrast(ov2640_t *dev, ov2640_level_t lvl)
{
    if (dev == NULL) { return HAL_ERROR; }
    return ov2640_apply_reglist(dev, level_to_contrast(lvl), 1u);
}

HAL_StatusTypeDef ov2640_set_saturation(ov2640_t *dev, ov2640_level_t lvl)
{
    if (dev == NULL) { return HAL_ERROR; }
    return ov2640_apply_reglist(dev, level_to_saturation(lvl), 1u);
}

HAL_StatusTypeDef ov2640_set_effect(ov2640_t *dev, ov2640_effect_t effect)
{
    const ov2640_reg_t *sel;
    if (dev == NULL) { return HAL_ERROR; }
    switch (effect) {
    case OV2640_EFFECT_ANTIQUE:     sel = OV2640_SPECIAL_EFFECTS_ANTIQUE;        break;
    case OV2640_EFFECT_BLUISH:      sel = OV2640_SPECIAL_EFFECTS_BLUISH;         break;
    case OV2640_EFFECT_GREENISH:    sel = OV2640_SPECIAL_EFFECTS_GREENISH;       break;
    case OV2640_EFFECT_REDDISH:     sel = OV2640_SPECIAL_EFFECTS_REDDISH;        break;
    case OV2640_EFFECT_BW:          sel = OV2640_SPECIAL_EFFECTS_BLACK;          break;
    case OV2640_EFFECT_NEGATIVE:    sel = OV2640_SPECIAL_EFFECTS_NEGATIVE;       break;
    case OV2640_EFFECT_NEGATIVE_BW: sel = OV2640_SPECIAL_EFFECTS_BLACK_NEGATIVE; break;
    default:                        sel = OV2640_SPECIAL_EFFECTS_NORMAL;         break;
    }
    return ov2640_apply_reglist(dev, sel, 1u);
}

HAL_StatusTypeDef ov2640_set_awb_simple(ov2640_t *dev, uint8_t enable)
{
    HAL_StatusTypeDef st;
    if (dev == NULL) { return HAL_ERROR; }
    st = ov2640_sccb_write(dev, OV2640_REG_BANK_SEL, OV2640_BANK_DSP);
    if (st != HAL_OK) { return st; }
    return ov2640_sccb_write(dev, 0xC7u, enable ? 0x10u : 0x00u);
}

HAL_StatusTypeDef ov2640_set_lightmode(ov2640_t *dev, ov2640_lightmode_t mode)
{
    const ov2640_reg_t *sel;
    if (dev == NULL) { return HAL_ERROR; }
    switch (mode) {
    case OV2640_LIGHT_SUNNY:  sel = OV2640_LIGHT_MODE_SUNNY;  break;
    case OV2640_LIGHT_CLOUDY: sel = OV2640_LIGHT_MODE_CLOUDY; break;
    case OV2640_LIGHT_OFFICE: sel = OV2640_LIGHT_MODE_OFFICE; break;
    case OV2640_LIGHT_HOME:   sel = OV2640_LIGHT_MODE_HOME;   break;
    default:                  sel = OV2640_LIGHT_MODE_AUTO;   break;
    }
    return ov2640_apply_reglist(dev, sel, 1u);
}

HAL_StatusTypeDef ov2640_set_zoom(ov2640_t *dev, uint16_t out_w, uint16_t out_h)
{
    HAL_StatusTypeDef st;
    if (dev == NULL) { return HAL_ERROR; }
    /* ZMOW/ZMOH store actual_output / 4 — divide before writing (datasheet Table 12). */
    uint16_t zmow = out_w / 4u;
    uint16_t zmoh = out_h / 4u;
    st = ov2640_write_reg(dev, OV2640_BANK_DSP, OV2640_DSP_ZMOW,
                          (uint8_t)(zmow & 0xFFu));
    if (st != HAL_OK) { return st; }
    st = ov2640_write_reg(dev, OV2640_BANK_DSP, OV2640_DSP_ZMOH,
                          (uint8_t)(zmoh & 0xFFu));
    if (st != HAL_OK) { return st; }
    return ov2640_write_reg(dev, OV2640_BANK_DSP, OV2640_DSP_ZMHH,
        (uint8_t)(((zmow >> 8u) & 0x03u) | (((zmoh >> 8u) & 0x03u) << 2u)));
}

/* ---------------------------------------------------------------------------
 * DCMI helpers (file-private)
 * ---------------------------------------------------------------------------*/
static void dcmi_prepare(ov2640_t *dev)
{
    g_active_hdcmi    = dev->hdcmi;
    g_dcmi_frame_done = 0u;
    g_dcmi_error      = 0u;
    __HAL_DCMI_CLEAR_FLAG(dev->hdcmi, 0xFFFFFFFFu);
    __HAL_DCMI_ENABLE_IT(dev->hdcmi, DCMI_IT_FRAME);
    __HAL_DCMI_ENABLE_IT(dev->hdcmi, DCMI_IT_OVR);
    __HAL_DCMI_ENABLE_IT(dev->hdcmi, DCMI_IT_ERR);
}

static HAL_StatusTypeDef dma_set_mode(ov2640_t *dev, uint32_t dma_mode)
{
    DMA_HandleTypeDef *hdma;

    if ((dev == NULL) || (dev->hdcmi == NULL) ||
        (dev->hdcmi->DMA_Handle == NULL)) {
        return HAL_ERROR;
    }

    hdma = dev->hdcmi->DMA_Handle;
    (void)HAL_DCMI_Stop(dev->hdcmi);
    (void)HAL_DMA_Abort(hdma);

    hdma->Init.Mode = dma_mode;
    if (HAL_DMA_DeInit(hdma) != HAL_OK) { return HAL_ERROR; }
    if (HAL_DMA_Init(hdma)   != HAL_OK) { return HAL_ERROR; }
    __HAL_LINKDMA(dev->hdcmi, DMA_Handle, *hdma);
    return HAL_OK;
}

/* ---------------------------------------------------------------------------
 * DCMI capture
 * ---------------------------------------------------------------------------*/
HAL_StatusTypeDef ov2640_dcmi_stop(ov2640_t *dev)
{
    HAL_StatusTypeDef st;
    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    st = HAL_DCMI_Stop(dev->hdcmi);
    g_dcmi_frame_done = 0u;
    g_dcmi_error      = 0u;
    g_active_hdcmi    = NULL;
    return st;
}

HAL_StatusTypeDef ov2640_capture_snapshot_framedone(ov2640_t *dev,
                                                    uint32_t dst,
                                                    uint32_t length_words,
                                                    uint32_t timeout_ms)
{
    HAL_StatusTypeDef st;
    uint32_t byte_len;

    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    if ((length_words == 0u) || (length_words > OV2640_DCMI_DMA_MAX_WORDS)) {
        return HAL_ERROR;
    }

    byte_len = length_words * 4u;
    cache_clean(dst, byte_len);
    dcmi_prepare(dev);

    if (dma_set_mode(dev, DMA_NORMAL) != HAL_OK) { return HAL_ERROR; }

    st = HAL_DCMI_Start_DMA(dev->hdcmi, DCMI_MODE_SNAPSHOT, dst, length_words);
    if (st != HAL_OK) { return st; }

    {
        uint32_t t0 = HAL_GetTick();
        while (!g_dcmi_frame_done && !g_dcmi_error) {
            if ((HAL_GetTick() - t0) > timeout_ms) {
                (void)HAL_DCMI_Stop(dev->hdcmi);
                g_active_hdcmi = NULL;
                return HAL_TIMEOUT;
            }
        }
    }

    st = HAL_DCMI_Stop(dev->hdcmi);
    cache_invalidate(dst, byte_len);
    g_active_hdcmi = NULL;

    return g_dcmi_error ? HAL_ERROR : st;
}

HAL_StatusTypeDef ov2640_capture_rgb565_frame(ov2640_t *dev,
                                              uint32_t dst,
                                              uint16_t width,
                                              uint16_t height,
                                              uint32_t timeout_ms)
{
    uint32_t bytes, words;
    if (dev == NULL) { return HAL_ERROR; }
    bytes = ov2640_rgb565_bytes(width, height);
    words = ov2640_words_from_bytes(bytes);
    if ((words == 0u) || (words > OV2640_DCMI_DMA_MAX_WORDS)) { return HAL_ERROR; }
    return ov2640_capture_snapshot_framedone(dev, dst, words, timeout_ms);
}

HAL_StatusTypeDef ov2640_capture_continuous_start(ov2640_t *dev,
                                                  uint32_t dst,
                                                  uint32_t length_words)
{
    uint32_t byte_len;
    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    if ((length_words == 0u) || (length_words > OV2640_DCMI_DMA_MAX_WORDS)) {
        return HAL_ERROR;
    }
    byte_len = length_words * 4u;
    cache_clean(dst, byte_len);
    dcmi_prepare(dev);
    if (dma_set_mode(dev, DMA_CIRCULAR) != HAL_OK) { return HAL_ERROR; }
    return HAL_DCMI_Start_DMA(dev->hdcmi, DCMI_MODE_CONTINUOUS, dst, length_words);
}

HAL_StatusTypeDef ov2640_wait_continuous_frame(ov2640_t *dev,
                                               uint32_t dst,
                                               uint32_t length_words,
                                               uint32_t timeout_ms)
{
    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    if ((length_words == 0u) || (length_words > OV2640_DCMI_DMA_MAX_WORDS)) {
        return HAL_ERROR;
    }

    {
        uint32_t t0 = HAL_GetTick();
        while (!g_dcmi_frame_done && !g_dcmi_error) {
            if ((HAL_GetTick() - t0) > timeout_ms) { return HAL_TIMEOUT; }
            /* Yield to higher-priority tasks (e.g. command parser) while
             * waiting for the next frame.  1-tick sleep keeps CPU load low
             * without adding more than one scheduler quantum of latency. */
            vTaskDelay(1u);
        }
    }

    if (g_dcmi_error) { return HAL_ERROR; }

    g_dcmi_frame_done = 0u;
    cache_invalidate(dst, length_words * 4u);
    return HAL_OK;
}

HAL_StatusTypeDef ov2640_capture_continuous_stop(ov2640_t *dev)
{
    HAL_StatusTypeDef st;
    if ((dev == NULL) || (dev->hdcmi == NULL)) { return HAL_ERROR; }
    st = HAL_DCMI_Stop(dev->hdcmi);
    g_dcmi_frame_done = 0u;
    g_dcmi_error      = 0u;
    g_active_hdcmi    = NULL;
    return st;
}
