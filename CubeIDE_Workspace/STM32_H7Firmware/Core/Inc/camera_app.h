/*
 * camera_app.h
 *
 * Top-level application state and task API for STM32H7 Edge Vision.
 *
 * Supported UART commands:
 *   MODE JPEG|RGB
 *   RES  QQVGA|QVGA|WQVGA|VGA|SVGA|XGA|SXGA
 *   STREAM 0|1  |  SNAP  |  GET_STATUS
 *   SAT / BRI / CON / EFFECT / LIGHT / AWB
 *   CV  GET|RUN|EN|PRESET|BGCAP|BGSUB|BORDFILT|
 *       THR|THRMODE|INV|MINAREA|MAXAREA|
 *       CON|BLUR|FILTER|MORPH|MORPHMODE|
 *       ASPECT|CIRC|ROI
 *   TM  GET|RUN|EN
 *   ZOOM 1|2|4
 *
 * Note: 'CV CON' sets CCL connectivity (4 or 8) and is unrelated to the
 * top-level 'CON' command (camera contrast).
 */

#ifndef INC_CAMERA_APP_H_
#define INC_CAMERA_APP_H_

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"

#include "ov2640_Drive.h"
#include "tinyml_engine.h"
#include "cv_engine.h"
#include "uart_tx.h"
#include "shared_helper.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a single UART command line including NUL. */
#ifndef CAMERA_APP_COMMAND_LINE_MAX
#define CAMERA_APP_COMMAND_LINE_MAX 96u
#endif

/* Event bits used to signal the stream task. */
#define STREAM_EV_START  (1u << 0)
#define STREAM_EV_STOP   (1u << 1)

typedef enum {
    CAM_CMD_UNKNOWN = 0,
    CAM_CMD_MODE,
    CAM_CMD_RES,
    CAM_CMD_STREAM,
    CAM_CMD_SNAP,
    CAM_CMD_GET_STATUS,
    CAM_CMD_SAT,
    CAM_CMD_BRI,
    CAM_CMD_CON,
    CAM_CMD_EFFECT,
    CAM_CMD_LIGHT,
    CAM_CMD_AWB,
    CAM_CMD_CV,
    CAM_CMD_TM,
	CAM_CMD_FLASH,
    CAM_CMD_ZOOM
} cam_cmd_id_t;

typedef struct {
	 GPIO_TypeDef       *flash_port;
	 uint16_t            flash_pin;
}flash_t;


typedef struct {
    ov2640_t             cam;
    uart_tx_t           *uart;
    ov2640_pixformat_t   pixfmt;
    ov2640_resolution_t  res;

    TaskHandle_t         cmd_task;
    TaskHandle_t         stream_task;
    EventGroupHandle_t   stream_events;

    volatile uint8_t     stream_enable;
    volatile uint8_t 	 flash_enable;
    uint8_t              zoom_level;    /* 1 = no zoom, 2 = 2×, 4 = 4× */
    uint16_t             zoom_w;       /* 0 = native, else zoomed output width  */
    uint16_t             zoom_h;       /* 0 = native, else zoomed output height */

    /* DCMI/DMA frame buffer in RAM_D1 (.dcmi_buf, Non-Cacheable).
     * Size = APP_RGB_MAX_WIDTH * APP_RGB_MAX_HEIGHT * 2 (RGB565, 2 B/px). */
    uint8_t  *frame_buf;
    uint32_t  frame_buf_size;

    /* CV work_bin: grayscale staging and binary pipeline result in RAM_D1.
     * Size = APP_CV_MAX_WIDTH * APP_CV_MAX_HEIGHT (1 B/px). */
    uint8_t  *cv_bin_buf;
    uint32_t  cv_bin_buf_size;

    /* CV tmp_buf: filter, threshold and morphology scratch in RAM_D2.
     * Must be the same size as cv_bin_buf (pipeline writes the full region). */
    uint8_t  *cv_tmp_buf;
    uint32_t  cv_tmp_buf_size;

    /* Background reference frame for subtraction in RAM_D2 (.cvbg_buf).
     * Must be the same size as cv_bin_buf.
     * NULL disables background subtraction regardless of cfg.bgsub_enabled. */
    uint8_t  *bg_buf;
    uint32_t  bg_buf_size;
    uint8_t   bg_captured;  /* 1 after CV BGCAP has been executed */

    QueueHandle_t        rx_q;
    UART_HandleTypeDef  *rx_huart;

    /* Capture statistics updated by camcap_update_stats(). */
    uint32_t last_image_size;
    uint32_t last_latency_ms;
    uint32_t last_fps_x10;      /* FPS scaled by 10 for one decimal digit */
    uint32_t last_frame_tick;   /* HAL tick of the last completed frame    */
    uint32_t heap_free_bytes;

    uint8_t  last_rgb_valid;    /* 1 when frame_buf holds a valid RGB565 frame */
    uint16_t last_rgb_w;
    uint16_t last_rgb_h;

    /* UART receive line buffer — assembled one byte at a time from rx_q. */
    size_t rx_line_len;
    char   rx_line[CAMERA_APP_COMMAND_LINE_MAX];

    tinyml_config_t tm_cfg;
    tinyml_result_t tm_res;

    cv_config_t     cv_cfg;
    cv_result_t     cv_res;

    flash_t 		flash_app;
} camera_app_t;

/* Initialise application state, bind hardware handles, and start TinyML.
 * Pass bg_buf=NULL / bg_buf_size=0 to permanently disable background subtraction. */
void camera_app_init(camera_app_t       *app,
                     uart_tx_t          *uart,
                     I2C_HandleTypeDef  *hi2c,
                     DCMI_HandleTypeDef *hdcmi,
                     GPIO_TypeDef       *rst_port,
                     uint16_t            rst_pin,
					 GPIO_TypeDef 		*flash_port,
					 uint16_t 			 flash_pin,
                     uint8_t  *frame_buf,   uint32_t frame_buf_size,
                     uint8_t  *cv_bin_buf,  uint32_t cv_bin_buf_size,
                     uint8_t  *cv_tmp_buf,  uint32_t cv_tmp_buf_size,
                     uint8_t  *bg_buf,      uint32_t bg_buf_size,
                     UART_HandleTypeDef *rx_huart,
                     QueueHandle_t       rx_q);

/* FreeRTOS task functions — pass camera_app_t * as the task argument. */
void camera_app_task   (void *arg);
void camera_stream_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INC_CAMERA_APP_H_ */
