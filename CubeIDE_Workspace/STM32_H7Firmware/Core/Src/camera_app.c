/*
 * camera_app.c
 *
 * Top-level UART command handler and FreeRTOS task implementation for
 * STM32H7 Edge Vision.
 *
 * Architecture notes:
 *   - The stream task and the command handler share app->stream_enable.
 *     uart_tx_send_bin() uses a task-notification handshake internally;
 *     the caller must NOT wait on ulTaskNotifyTake() afterwards.
 *   - All CV sub-commands are dispatched through handle_cv() (single path).
 *   - BGCAP converts RGB565 → grayscale via cv_capture_background(); a
 *     raw memcpy would copy 2-byte pixels into a 1-byte-per-pixel buffer.
 */

#include "camera_app.h"
#include "camera_proto.h"
#include "camera_parse.h"
#include "camera_capture.h"
#include "tinyml_engine.h"
#include "cv_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Flash setting ──────────────────────────────────────────────────── */

static uint8_t last_stat_flash = 0;

static void flash_enabled(camera_app_t *flash)
{
    HAL_GPIO_WritePin(flash->flash_app.flash_port,
                      flash->flash_app.flash_pin,
                      flash->flash_enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ── Command ID lookup ──────────────────────────────────────────────────── */

static cam_cmd_id_t cmd_to_id(const char *cmd)
{
    if (cmd == NULL)                    { return CAM_CMD_UNKNOWN;    }
    if (ascii_equal(cmd, "MODE"))       { return CAM_CMD_MODE;       }
    if (ascii_equal(cmd, "RES"))        { return CAM_CMD_RES;        }
    if (ascii_equal(cmd, "STREAM"))     { return CAM_CMD_STREAM;     }
    if (ascii_equal(cmd, "SNAP"))       { return CAM_CMD_SNAP;       }
    if (ascii_equal(cmd, "GET_STATUS")) { return CAM_CMD_GET_STATUS; }
    if (ascii_equal(cmd, "SAT"))        { return CAM_CMD_SAT;        }
    if (ascii_equal(cmd, "BRI"))        { return CAM_CMD_BRI;        }
    if (ascii_equal(cmd, "CON"))        { return CAM_CMD_CON;        }
    if (ascii_equal(cmd, "EFFECT"))     { return CAM_CMD_EFFECT;     }
    if (ascii_equal(cmd, "LIGHT"))      { return CAM_CMD_LIGHT;      }
    if (ascii_equal(cmd, "AWB"))        { return CAM_CMD_AWB;        }
    if (ascii_equal(cmd, "CV"))         { return CAM_CMD_CV;         }
    if (ascii_equal(cmd, "TM"))         { return CAM_CMD_TM;         }
    if (ascii_equal(cmd, "FLASH"))      { return CAM_CMD_FLASH;      }
    if (ascii_equal(cmd, "ZOOM"))       { return CAM_CMD_ZOOM;       }
    return CAM_CMD_UNKNOWN;
}

/* ── TinyML runner ──────────────────────────────────────────────────────── */

static void run_tinyml(camera_app_t *app)
{
    uint16_t w = 0u, h = 0u;

    if (!app->tm_cfg.enabled) {
        camproto_send_warn(app->uart, "TinyML disabled");
        return;
    }
    if (!camcap_get_last_rgb_frame_info(app, &w, &h)) {
        camproto_send_err(app->uart,
            "TM RUN needs RGB565 frame — snap first");
        return;
    }

    uint32_t t0 = HAL_GetTick();
    int rc = tinyml_run_rgb565((const uint16_t *)app->frame_buf, w, h,
                             &app->tm_cfg, &app->tm_res);
    app->tm_res.inference_time_ms = HAL_GetTick() - t0;

    if (rc != 0) {
        camproto_send_err(app->uart, "TinyML run failed");
        return;
    }
    camproto_send_tm_result(app);
}

/* ── CV pipeline runner ─────────────────────────────────────────────────── */

static void run_cv(camera_app_t *app)
{
    uint16_t w = 0u, h = 0u;

    if (!app->cv_cfg.enabled) {
        camproto_send_warn(app->uart, "CV disabled");
        return;
    }
    if (!camcap_get_last_rgb_frame_info(app, &w, &h)) {
        camproto_send_err(app->uart,
            "CV RUN needs RGB565 frame — snap first");
        return;
    }

    uint32_t needed = (uint32_t)w * h;
    if (!app->cv_bin_buf || app->cv_bin_buf_size < needed) {
        camproto_send_err(app->uart, "CV work buffer too small");
        return;
    }
    if (!app->cv_tmp_buf || app->cv_tmp_buf_size < needed) {
        camproto_send_err(app->uart, "CV temp buffer too small");
        return;
    }

    const uint8_t *bg = (app->bg_captured &&
                         app->bg_buf != NULL &&
                         app->bg_buf_size >= needed)
                        ? app->bg_buf : NULL;

    uint32_t t0 = HAL_GetTick();
    int rc = cv_run_rgb565((const uint16_t *)app->frame_buf, w, h,
                           app->cv_bin_buf, app->cv_tmp_buf,
                           bg, &app->cv_cfg, &app->cv_res);
    app->cv_res.processing_time_ms = HAL_GetTick() - t0;

    if (rc != 0) {
        camproto_send_err(app->uart, "CV run failed");
        return;
    }
    camproto_send_cv_result(app);
}

/* ── CV sub-command dispatcher ──────────────────────────────────────────── */

static void handle_cv(camera_app_t *app, char *sub)
{
    char    *arg2 = strtok(NULL, " ");
    uint32_t v1;

    if (sub == NULL) {
        camproto_send_err(app->uart, "CV needs subcommand");
        return;
    }

    if (ascii_equal(sub, "GET")) { camproto_send_cv_config(app); return; }
    if (ascii_equal(sub, "RUN")) { run_cv(app);                  return; }

    /* EN */
    if (ascii_equal(sub, "EN")) {
        if (!arg2) { camproto_send_err(app->uart, "CV EN 0|1"); return; }
        app->cv_cfg.enabled = atoi(arg2) ? 1u : 0u;
        camproto_send_cv_config(app);
        return;
    }

    /* PRESET */
    if (ascii_equal(sub, "PRESET")) {
        if (!arg2) {
            camproto_send_err(app->uart,
                "CV PRESET 0=CUSTOM 1=FAST 2=ROBUST 3=ACCURATE");
            return;
        }
        if (cvext_set_preset((uint32_t)strtoul(arg2, NULL, 10)) != 0) {
            camproto_send_err(app->uart, "CV PRESET 0..3");
            return;
        }
        camproto_send_cv_config(app);
        return;
    }

    /* BGCAP */
    if (ascii_equal(sub, "BGCAP")) {
        uint16_t bw = 0u, bh = 0u;
        if (!camcap_get_last_rgb_frame_info(app, &bw, &bh)) {
            camproto_send_err(app->uart,
                "BGCAP needs RGB565 frame — snap first");
            return;
        }
        uint32_t needed = (uint32_t)bw * bh;  /* grayscale: 1 byte/pixel */
        if (!app->bg_buf || app->bg_buf_size < needed) {
            camproto_send_err(app->uart, "BG buffer too small");
            return;
        }
        /*
         * Convert current RGB565 frame to grayscale and store it in bg_buf.
         * cv_capture_background() handles the RGB565->grayscale conversion
         * correctly.  A raw memcpy would be wrong because frame_buf holds
         * 2 bytes/pixel (RGB565) while bg_buf expects 1 byte/pixel (luma).
         */
        cv_capture_background((const uint16_t *)app->frame_buf,
                              bw, bh, app->bg_buf, &app->cv_cfg);
        app->bg_captured = 1u;
        camproto_send_cv_config(app);
        camproto_send_info(app->uart, "BGCAP done");
        return;
    }

    /* BGSUB */
    if (ascii_equal(sub, "BGSUB")) {
        if (!arg2) { camproto_send_err(app->uart, "CV BGSUB 0|1"); return; }
        app->cv_cfg.bgsub_enabled = atoi(arg2) ? 1u : 0u;
        camproto_send_cv_config(app);
        return;
    }

    /* BORDFILT */
    if (ascii_equal(sub, "BORDFILT")) {
        if (!arg2) { camproto_send_err(app->uart, "CV BORDFILT 0|1"); return; }
        app->cv_cfg.border_filter_enabled = atoi(arg2) ? 1u : 0u;
        camproto_send_cv_config(app);
        return;
    }

    /* THR */
    if (ascii_equal(sub, "THR")) {
        if (!arg2) { camproto_send_err(app->uart, "CV THR 0..255"); return; }
        v1 = (uint32_t)strtoul(arg2, NULL, 10);
        if (v1 > 255u) { camproto_send_err(app->uart, "CV THR 0..255"); return; }
        app->cv_cfg.threshold = (uint8_t)v1;
        app->cv_cfg.preset    = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* THRMODE */
    if (ascii_equal(sub, "THRMODE")) {
        if (!arg2) { camproto_send_err(app->uart, "CV THRMODE 0|1"); return; }
        if (cvext_set_thr_mode((uint32_t)strtoul(arg2, NULL, 10)) != 0) {
            camproto_send_err(app->uart, "CV THRMODE 0=MANUAL|1=OTSU");
            return;
        }
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* INV */
    if (ascii_equal(sub, "INV")) {
        if (!arg2) { camproto_send_err(app->uart, "CV INV 0|1"); return; }
        app->cv_cfg.invert = atoi(arg2) ? 1u : 0u;
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* MINAREA */
    if (ascii_equal(sub, "MINAREA")) {
        if (!arg2) { camproto_send_err(app->uart, "CV MINAREA n"); return; }
        app->cv_cfg.min_area   = (uint32_t)strtoul(arg2, NULL, 10);
        app->cv_cfg.preset     = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* MAXAREA */
    if (ascii_equal(sub, "MAXAREA")) {
        if (!arg2) { camproto_send_err(app->uart, "CV MAXAREA n"); return; }
        app->cv_cfg.max_area   = (uint32_t)strtoul(arg2, NULL, 10);
        app->cv_cfg.preset     = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* CON — CCL connectivity */
    if (ascii_equal(sub, "CON")) {
        if (!arg2) { camproto_send_err(app->uart, "CV CON 4|8"); return; }
        v1 = (uint32_t)strtoul(arg2, NULL, 10);
        if ((v1 != 4u) && (v1 != 8u)) {
            camproto_send_err(app->uart, "CV CON 4|8");
            return;
        }
        app->cv_cfg.connectivity = (uint8_t)v1;
        camproto_send_cv_config(app);
        return;
    }

    /* BLUR */
    if (ascii_equal(sub, "BLUR")) {
        if (!arg2) { camproto_send_err(app->uart, "CV BLUR 0..7"); return; }
        v1 = (uint32_t)strtoul(arg2, NULL, 10);
        if (v1 > 7u) { camproto_send_err(app->uart, "CV BLUR 0..7"); return; }
        app->cv_cfg.blur_kernel = (uint8_t)v1;
        app->cv_cfg.preset      = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* FILTER */
    if (ascii_equal(sub, "FILTER")) {
        if (!arg2) {
            camproto_send_err(app->uart, "CV FILTER 0=OFF 1=BOX 2=MEDIAN");
            return;
        }
        if (cvext_set_filter_mode((uint32_t)strtoul(arg2, NULL, 10)) != 0) {
            camproto_send_err(app->uart, "CV FILTER 0=OFF 1=BOX 2=MEDIAN");
            return;
        }
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* MORPH */
    if (ascii_equal(sub, "MORPH")) {
        if (!arg2) { camproto_send_err(app->uart, "CV MORPH 0..7"); return; }
        v1 = (uint32_t)strtoul(arg2, NULL, 10);
        if (v1 > 7u) { camproto_send_err(app->uart, "CV MORPH 0..7"); return; }
        app->cv_cfg.morph_kernel = (uint8_t)v1;
        app->cv_cfg.preset       = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* MORPHMODE */
    if (ascii_equal(sub, "MORPHMODE")) {
        if (!arg2) {
            camproto_send_err(app->uart, "CV MORPHMODE 0..4");
            return;
        }
        if (cvext_set_morph_mode((uint32_t)strtoul(arg2, NULL, 10)) != 0) {
            camproto_send_err(app->uart,
                "CV MORPHMODE 0=OFF 1=OPEN 2=CLOSE 3=ERODE 4=DILATE");
            return;
        }
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* ASPECT */
    if (ascii_equal(sub, "ASPECT")) {
        char *arg3 = strtok(NULL, " ");
        if (!arg2 || !arg3) {
            camproto_send_err(app->uart,
                "CV ASPECT min_x1000 max_x1000");
            return;
        }
        cvext_set_aspect_ratio_range(
            (uint32_t)strtoul(arg2, NULL, 10),
            (uint32_t)strtoul(arg3, NULL, 10));
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* CIRC */
    if (ascii_equal(sub, "CIRC")) {
        if (!arg2) {
            camproto_send_err(app->uart, "CV CIRC min_x1000");
            return;
        }
        cvext_set_circularity_min((uint32_t)strtoul(arg2, NULL, 10));
        app->cv_cfg.preset = CV_PRESET_CUSTOM;
        camproto_send_cv_config(app);
        return;
    }

    /* ROI */
    if (ascii_equal(sub, "ROI")) {
        if (!arg2) {
            camproto_send_err(app->uart, "CV ROI en [x y w h]");
            return;
        }
        v1 = (uint32_t)strtoul(arg2, NULL, 10);
        if (v1 == 0u) {
            (void)cvext_set_roi(0u, 0u, 0u, 0u, 0u);
            camproto_send_cv_config(app);
            return;
        }
        {
            char *a3 = strtok(NULL, " ");
            char *a4 = strtok(NULL, " ");
            char *a5 = strtok(NULL, " ");
            char *a6 = strtok(NULL, " ");
            if (!a3 || !a4 || !a5 || !a6) {
                camproto_send_err(app->uart, "CV ROI 1 x y w h");
                return;
            }
            if (cvext_set_roi(v1,
                              (uint32_t)strtoul(a3, NULL, 10),
                              (uint32_t)strtoul(a4, NULL, 10),
                              (uint32_t)strtoul(a5, NULL, 10),
                              (uint32_t)strtoul(a6, NULL, 10)) != 0) {
                camproto_send_err(app->uart, "CV ROI invalid");
                return;
            }
        }
        camproto_send_cv_config(app);
        return;
    }

    camproto_send_err(app->uart, "unknown CV subcommand");
}

/* ── camera_app_init ────────────────────────────────────────────────────── */

void camera_app_init(camera_app_t       *app,
                     uart_tx_t          *uart,
                     I2C_HandleTypeDef  *hi2c,
                     DCMI_HandleTypeDef *hdcmi,
                     GPIO_TypeDef       *rst_port,  uint16_t rst_pin,
                     GPIO_TypeDef       *flash_Port, uint16_t flash_pin,
                     uint8_t  *frame_buf,     uint32_t frame_buf_size,
                     uint8_t  *cv_bin_buf,    uint32_t cv_bin_buf_size,
                     uint8_t  *cv_tmp_buf,    uint32_t cv_tmp_buf_size,
                     uint8_t  *bg_buf,        uint32_t bg_buf_size,
                     UART_HandleTypeDef *rx_huart,
                     QueueHandle_t       rx_q)
{
    memset(app, 0, sizeof(*app));

    app->uart            = uart;
    app->frame_buf       = frame_buf;
    app->frame_buf_size  = frame_buf_size;
    app->cv_bin_buf      = cv_bin_buf;
    app->cv_bin_buf_size = cv_bin_buf_size;
    app->cv_tmp_buf      = cv_tmp_buf;
    app->cv_tmp_buf_size = cv_tmp_buf_size;
    app->bg_buf          = bg_buf;
    app->bg_buf_size     = bg_buf_size;
    app->bg_captured     = 0u;

    app->cam.hi2c     = hi2c;
    app->cam.hdcmi    = hdcmi;
    app->cam.i2c_addr = OV2640_I2C_ADDR_HAL;
    app->cam.rst_port = rst_port;
    app->cam.rst_pin  = rst_pin;
    app->flash_app.flash_port = flash_Port;
    app->flash_app.flash_pin  = flash_pin;
    app->rx_huart     = rx_huart;
    app->rx_q         = rx_q;

    app->pixfmt = OV2640_PIXFMT_JPEG;
    app->res    = OV2640_RES_WQVGA_480x272;

    app->stream_enable = 0u;
    app->flash_enable  = 0u;
    app->zoom_level    = 1u;
    app->zoom_w        = 0u;
    app->zoom_h        = 0u;

    app->cv_cfg.enabled               = 1u;
    app->cv_cfg.connectivity          = 8u;
    app->cv_cfg.min_area              = 50u;
    app->cv_cfg.border_filter_enabled = 1u;
    cv_apply_preset(&app->cv_cfg, CV_PRESET_ROBUST);
    cvext_register(&app->cv_cfg);

    app->tm_cfg.enabled = 1u;
    (void)tinyml_init(&app->tm_cfg, &app->tm_res);
    tinyml_set_diag_uart(uart);

    app->stream_events = xEventGroupCreate();
    configASSERT(app->stream_events != NULL);
}

/* ── camera_stream_task ─────────────────────────────────────────────────── */

void camera_stream_task(void *arg)
{
    camera_app_t *app = (camera_app_t *)arg;
    app->stream_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (;;) {
        xEventGroupWaitBits(app->stream_events, STREAM_EV_START,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        if (!camcap_stream_config_supported(app)) {
            camproto_send_err(app->uart,
                "stream not supported for current mode / res / buffer");
            continue;
        }

        {
            app->flash_enable = last_stat_flash;
            flash_enabled(app);

            uint32_t words = camcap_current_capture_words(app);

            if (ov2640_capture_continuous_start(&app->cam,
                    (uint32_t)app->frame_buf, words) != HAL_OK) {
                camproto_send_err(app->uart, "STREAM start failed");
                continue;
            }

            app->stream_enable   = 1u;
            app->last_frame_tick = 0u;
            camproto_send_info(app->uart, "STREAM ON");

            for (;;) {
                uint32_t t0 = HAL_GetTick();

                /* Wait for one DMA frame */
                if (ov2640_wait_continuous_frame(&app->cam,
                        (uint32_t)app->frame_buf, words, 1000u) != HAL_OK) {
                    camproto_send_err(app->uart, "stream frame timeout");
                    break;
                }

                /* Stop DMA so we can safely read the buffer */
                if (ov2640_capture_continuous_stop(&app->cam) != HAL_OK) {
                    camproto_send_err(app->uart, "STREAM stop failed");
                    break;
                }

                /* Send the frame over UART */
                if (app->pixfmt == OV2640_PIXFMT_JPEG) {
                    uint32_t jpeg_off = 0u, jpeg_len = 0u;
                    char     hdr[64];

                    if (!camcap_find_jpeg_bounds(app->frame_buf,
                                                 app->frame_buf_size,
                                                 &jpeg_off, &jpeg_len)) {
                        camproto_send_err(app->uart, "JPEG markers not found");
                        break;
                    }
                    camcap_update_stats(app, jpeg_len, t0, HAL_GetTick());
                    snprintf(hdr, sizeof(hdr),
                             "JPG: %lu\r\n", (unsigned long)jpeg_len);
                    (void)uart_tx_send_text(app->uart, hdr);
                    /*
                     * uart_tx_send_bin() already waits internally for DMA
                     * TX completion via task notification.  Do NOT call
                     * ulTaskNotifyTake() here — it would consume the next
                     * STREAM_EV_START notification and exit the loop.
                     */
                    (void)uart_tx_send_bin(app->uart,
                                           app->frame_buf + jpeg_off,
                                           jpeg_len,
                                           app->stream_task);
                    camproto_send_status(app);
                } else {
                    uint16_t w = 0u, h = 0u;
                    char     hdr[64];

                    camcap_rgb_dims_from_res(app->res, &w, &h);
                    uint32_t bytes = ov2640_rgb565_bytes(w, h);
                    camcap_update_stats(app, bytes, t0, HAL_GetTick());
                    snprintf(hdr, sizeof(hdr),
                             "RGB565: %u %u %lu\r\n",
                             w, h, (unsigned long)bytes);
                    (void)uart_tx_send_text(app->uart, hdr);
                    (void)uart_tx_send_bin(app->uart, app->frame_buf,
                                           bytes, app->stream_task);
                    camproto_send_status(app);
                }

                /* Check whether the host requested stop */
                EventBits_t ev = xEventGroupWaitBits(app->stream_events,
                                     STREAM_EV_STOP, pdTRUE, pdFALSE, 0u);
                if (ev & STREAM_EV_STOP) { break; }

                /* Restart DMA for the next frame */
                if (ov2640_capture_continuous_start(&app->cam,
                        (uint32_t)app->frame_buf, words) != HAL_OK) {
                    camproto_send_err(app->uart, "STREAM restart failed");
                    break;
                }
            }

            (void)ov2640_capture_continuous_stop(&app->cam);
            (void)xEventGroupClearBits(app->stream_events, STREAM_EV_STOP);
            app->stream_enable = 0u;
            app->flash_enable  = 0u;
            flash_enabled(app);
            camproto_send_info(app->uart, "STREAM OFF");
        }
    }
}

/* ── camera_app_task — UART command parser ──────────────────────────────── */

void camera_app_task(void *arg)
{
    camera_app_t *app = (camera_app_t *)arg;
    char          line[CAMERA_APP_COMMAND_LINE_MAX];

    app->cmd_task = xTaskGetCurrentTaskHandle();

    if (ov2640_init(&app->cam) != HAL_OK) {
        camproto_send_err(app->uart, "ov2640_init failed");
        vTaskDelete(NULL);
        return;
    }

    {
        uint8_t pid = 0u, ver = 0u;
        char    info[64];

        if (ov2640_read_id(&app->cam, &pid, &ver) != HAL_OK) {
            camproto_send_err(app->uart, "read_id failed");
            vTaskDelete(NULL);
            return;
        }
        snprintf(info, sizeof(info), "OV2640 PID=0x%02X VER=0x%02X", pid, ver);
        camproto_send_info(app->uart, info);
    }

    camcap_apply_mode_and_res(app);
    camproto_send_info(app->uart, "OV2640 ready");
    camproto_send_status(app);
    camproto_send_tm_info(app);

    if (app->stream_task != NULL) { xTaskNotifyGive(app->stream_task); }

    for (;;) {
        /* Push-button snapshot */
        if (ulTaskNotifyTake(pdTRUE, 0u) > 0u) {
            if (app->stream_enable) {
                camproto_send_info(app->uart,
                    "button ignored while STREAM ON");
            } else {
                camcap_do_snapshot(app);
            }
        }

        if (!camparse_uart_readline_from_queue(app, line, sizeof(line),
                                               pdMS_TO_TICKS(5u))) {
            continue;
        }

        {
            char *cmd  = strtok(line, " ");
            char *arg1 = strtok(NULL, " ");
            if (cmd == NULL) { continue; }

            switch (cmd_to_id(cmd)) {

            case CAM_CMD_MODE: {
                if (!arg1) {
                    camproto_send_err(app->uart, "MODE JPEG|RGB");
                    break;
                }
                if      (ascii_equal(arg1, "JPEG")) {
                    app->pixfmt = OV2640_PIXFMT_JPEG;
                }
                else if (ascii_equal(arg1, "RGB")) {
                    app->pixfmt = OV2640_PIXFMT_RGB565;
                }
                else {
                    camproto_send_err(app->uart,
                        "unknown MODE (JPEG|RGB)");
                    break;
                }
                /* Re-init resets OV2640 DSP registers — clear zoom state. */
                app->zoom_level = 1u;
                app->zoom_w     = 0u;
                app->zoom_h     = 0u;
                camcap_apply_mode_and_res(app);
                break;
            }

            case CAM_CMD_RES: {
                ov2640_resolution_t res;
                if (!arg1 || !camparse_parse_res(arg1, &res)) {
                    camproto_send_err(app->uart,
                        "RES QQVGA|QVGA|WQVGA|VGA|SVGA|XGA|SXGA");
                    break;
                }
                app->res = res;
                /* Resolution re-init resets OV2640 DSP registers — clear zoom state. */
                app->zoom_level = 1u;
                app->zoom_w     = 0u;
                app->zoom_h     = 0u;
                camcap_apply_resolution_only(app);
                break;
            }

            case CAM_CMD_STREAM: {
                if (!arg1) {
                    camproto_send_err(app->uart, "STREAM 0|1");
                    break;
                }
                if (atoi(arg1)) {
                    if (app->stream_enable) {
                        camproto_send_warn(app->uart, "already streaming");
                        break;
                    }
                    if (app->pixfmt != OV2640_PIXFMT_JPEG) {
                        camproto_send_err(app->uart,
                            "STREAM only in JPEG mode");
                        break;
                    }
                    xEventGroupSetBits(app->stream_events,
                        STREAM_EV_START);
                } else {
                    xEventGroupSetBits(app->stream_events,
                        STREAM_EV_STOP);
                }
                break;
            }

            case CAM_CMD_FLASH: {
                if (!arg1) {
                    camproto_send_err(app->uart, "FLASH 0|1");
                    break;
                }
                if (atoi(arg1)) {
                    app->flash_enable = 1u;
                    camproto_send_info(app->uart, "flash on");
                } else {
                    app->flash_enable = 0u;
                    camproto_send_info(app->uart, "flash off");
                }
                last_stat_flash = app->flash_enable;
                flash_enabled(app);   /* apply GPIO immediately */
                break;
            }

            case CAM_CMD_ZOOM: {
                if (!arg1) {
                    camproto_send_err(app->uart, "ZOOM 1|2|4");
                    break;
                }
                uint8_t lvl = (uint8_t)atoi(arg1);
                if ((lvl != 1u) && (lvl != 2u) && (lvl != 4u)) {
                    camproto_send_err(app->uart, "ZOOM 1|2|4");
                    break;
                }
                if (app->pixfmt == OV2640_PIXFMT_JPEG) {
                    camproto_send_err(app->uart, "ZOOM RGB only");
                    break;
                }
                app->zoom_level = lvl;
                if (lvl == 1u) {
                    /* Restore native ZMOW/ZMOH so OV2640 outputs the full frame again. */
                    uint16_t fw0, fh0;
                    camcap_rgb_dims_from_res(app->res, &fw0, &fh0);
                    (void)ov2640_set_zoom(&app->cam, fw0, fh0);
                    app->zoom_w = 0u;
                    app->zoom_h = 0u;
                    camproto_send_info(app->uart, "zoom ok");
                    break;
                }
                uint16_t fw, fh;
                camcap_rgb_dims_from_res(app->res, &fw, &fh);
                uint16_t zw = fw / lvl;
                uint16_t zh = fh / lvl;
                HAL_StatusTypeDef zst = ov2640_set_zoom(&app->cam, zw, zh);
                if (zst == HAL_OK) {
                    app->zoom_w = zw;
                    app->zoom_h = zh;
                    camproto_send_info(app->uart, "zoom ok");
                } else {
                    camproto_send_err(app->uart, "zoom hw err");
                }
                break;
            }

            case CAM_CMD_SNAP: {
                if (app->stream_enable) {
                    camproto_send_err(app->uart,
                        "SNAP not allowed while STREAM ON");
                } else {
                    app->flash_enable = last_stat_flash;
                    flash_enabled(app);
                    /*
                     * If the host sent STREAM 0 just before FLASH 1 + SNAP,
                     * the stream task may still be clearing stream_enable.
                     * Wait up to 50 ms for it to settle before aborting.
                     */
                    if (app->flash_enable) {
                        HAL_Delay(1000);   /* LED warm-up */
                    }
                    camcap_do_snapshot(app);
                    app->flash_enable = 0u;
                    flash_enabled(app);
                }
                break;
            }

            case CAM_CMD_GET_STATUS:
                camproto_send_status(app);
                break;

            case CAM_CMD_SAT: {
                ov2640_level_t lvl;
                if (!arg1 || !camparse_parse_level(arg1, &lvl)) {
                    camproto_send_err(app->uart, "SAT -2|-1|0|+1|+2");
                    break;
                }
                if (ov2640_set_saturation(&app->cam, lvl) != HAL_OK)
                    camproto_send_err(app->uart, "SAT failed");
                else
                    camproto_send_info(app->uart, "SAT set");
                break;
            }

            case CAM_CMD_BRI: {
                ov2640_level_t lvl;
                if (!arg1 || !camparse_parse_level(arg1, &lvl)) {
                    camproto_send_err(app->uart, "BRI -2|-1|0|+1|+2");
                    break;
                }
                if (ov2640_set_brightness(&app->cam, lvl) != HAL_OK)
                    camproto_send_err(app->uart, "BRI failed");
                else
                    camproto_send_info(app->uart, "BRI set");
                break;
            }

            case CAM_CMD_CON: {
                ov2640_level_t lvl;
                if (!arg1 || !camparse_parse_level(arg1, &lvl)) {
                    camproto_send_err(app->uart, "CON -2|-1|0|+1|+2");
                    break;
                }
                if (ov2640_set_contrast(&app->cam, lvl) != HAL_OK)
                    camproto_send_err(app->uart, "CON failed");
                else
                    camproto_send_info(app->uart, "CON set");
                break;
            }

            case CAM_CMD_EFFECT: {
                ov2640_effect_t e;
                if (!arg1 || !camparse_parse_effect(arg1, &e)) {
                    camproto_send_err(app->uart,
                        "EFFECT NORMAL|BW|NEGATIVE|NEGATIVE_BW|"
                        "BLUISH|GREENISH|REDDISH|ANTIQUE");
                    break;
                }
                if (ov2640_set_effect(&app->cam, e) != HAL_OK)
                    camproto_send_err(app->uart, "EFFECT failed");
                else
                    camproto_send_info(app->uart, "EFFECT set");
                break;
            }

            case CAM_CMD_LIGHT: {
                ov2640_lightmode_t m;
                if (!arg1 || !camparse_parse_light(arg1, &m)) {
                    camproto_send_err(app->uart,
                        "LIGHT AUTO|SUNNY|CLOUDY|OFFICE|HOME");
                    break;
                }
                if (ov2640_set_lightmode(&app->cam, m) != HAL_OK)
                    camproto_send_err(app->uart, "LIGHT failed");
                else
                    camproto_send_info(app->uart, "LIGHT set");
                break;
            }

            case CAM_CMD_AWB: {
                if (!arg1) {
                    camproto_send_err(app->uart, "AWB 0|1");
                    break;
                }
                if (ov2640_set_awb_simple(&app->cam,
                        atoi(arg1) ? 1u : 0u) != HAL_OK)
                    camproto_send_err(app->uart, "AWB failed");
                else
                    camproto_send_info(app->uart, "AWB set");
                break;
            }

            /* Single unified CV dispatcher — all sub-commands handled in handle_cv() */
            case CAM_CMD_CV:
                handle_cv(app, arg1);
                break;

            case CAM_CMD_TM: {
                if (!arg1) {
                    camproto_send_err(app->uart, "TM GET|RUN|EN");
                    break;
                }
                if (ascii_equal(arg1, "GET")) {
                    camproto_send_tm_config(app);
                    camproto_send_tm_info(app);
                } else if (ascii_equal(arg1, "RUN")) {
                    run_tinyml(app);
                } else if (ascii_equal(arg1, "EN")) {
                    char *arg2 = strtok(NULL, " ");
                    if (!arg2) {
                        camproto_send_err(app->uart, "TM EN 0|1");
                        break;
                    }
                    app->tm_cfg.enabled = atoi(arg2) ? 1u : 0u;
                    camproto_send_tm_config(app);
                } else {
                    camproto_send_err(app->uart,
                        "unknown TM command (GET|RUN|EN)");
                }
                break;
            }

            case CAM_CMD_UNKNOWN:
            default:
                camproto_send_err(app->uart, "unknown command");
                break;
            }
        }
    }
}
