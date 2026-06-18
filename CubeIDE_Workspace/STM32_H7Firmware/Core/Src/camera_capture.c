/*
 * camera_capture.c
 *
 * Frame capture helpers for the STM32H7 Edge Vision camera application.
 */

#include "camera_capture.h"
#include "camera_proto.h"

#include <stdio.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Mark the last RGB frame as invalid (called before any reconfiguration). */
static void camcap_invalidate_last_rgb(camera_app_t *app)
{
    app->last_rgb_valid = 0u;
    app->last_rgb_w     = 0u;
    app->last_rgb_h     = 0u;
}

/* Record a valid RGB565 frame with its dimensions. */
static void camcap_mark_last_rgb(camera_app_t *app, uint16_t w, uint16_t h)
{
    app->last_rgb_valid = 1u;
    app->last_rgb_w     = w;
    app->last_rgb_h     = h;
}

/* ── JPEG marker scan ────────────────────────────────────────────────────── */

/* Scan buf for JPEG SOI (0xFFD8) and EOI (0xFFD9) markers.
 * Returns 1 and sets jpeg_off/jpeg_len on success, 0 on failure. */
int camcap_find_jpeg_bounds(const uint8_t *buf,
                            uint32_t       maxlen,
                            uint32_t      *jpeg_off,
                            uint32_t      *jpeg_len)
{
    uint32_t start     = 0u;
    uint8_t  found_soi = 0u;

    if ((buf == NULL) || (jpeg_off == NULL) ||
        (jpeg_len == NULL) || (maxlen < 4u)) {
        return 0;
    }

    for (uint32_t i = 0u; (i + 1u) < maxlen; ++i) {
        if (!found_soi) {
            if ((buf[i] == 0xFFu) && (buf[i + 1u] == 0xD8u)) {
                start     = i;
                found_soi = 1u;
                ++i;
            }
            continue;
        }
        if ((buf[i] == 0xFFu) && (buf[i + 1u] == 0xD9u)) {
            *jpeg_off = start;
            *jpeg_len = (i + 2u) - start;
            return 1;
        }
    }
    return 0;
}

/* ── Resolution and byte-count helpers ──────────────────────────────────── */

/* Map a resolution enum to pixel dimensions.  Falls back to QVGA on unknown. */
void camcap_rgb_dims_from_res(ov2640_resolution_t res, uint16_t *w, uint16_t *h)
{
    if ((w == NULL) || (h == NULL)) { return; }
    switch (res) {
    case OV2640_RES_QQVGA_160x120: *w = 160u; *h = 120u; break;
    case OV2640_RES_QVGA_320x240:  *w = 320u; *h = 240u; break;
    case OV2640_RES_WQVGA_480x272: *w = 480u; *h = 272u; break;
    case OV2640_RES_VGA_640x480:   *w = 640u; *h = 480u; break;
    default:                        *w = 320u; *h = 240u; break;
    }
}

/* Return the byte count for an RGB565 frame at the current resolution. */
uint32_t camcap_current_rgb_bytes(camera_app_t *app)
{
    uint16_t w = 0u, h = 0u;
    if (app == NULL) { return 0u; }
    camcap_rgb_dims_from_res(app->res, &w, &h);
    return ov2640_rgb565_bytes(w, h);
}

/* Return the DMA word count for the current capture mode and resolution. */
uint32_t camcap_current_capture_words(camera_app_t *app)
{
    if (app == NULL) { return 0u; }
    if (app->pixfmt == OV2640_PIXFMT_JPEG) {
        return ov2640_words_from_bytes(app->frame_buf_size);
    }
    return ov2640_words_from_bytes(camcap_current_rgb_bytes(app));
}

/* Return 1 if streaming is supported for the current mode, resolution, and buffer. */
int camcap_stream_config_supported(camera_app_t *app)
{
    if (app == NULL) { return 0; }
    if (app->pixfmt == OV2640_PIXFMT_JPEG) {
        return (app->frame_buf_size > 0u);
    }
    return (camcap_current_rgb_bytes(app) <= app->frame_buf_size);
}

/* ── Statistics update ───────────────────────────────────────────────────── */

/* Update FPS, latency, image size, and free heap after a completed capture. */
void camcap_update_stats(camera_app_t *app,
                         uint32_t      image_size,
                         uint32_t      t0_ms,
                         uint32_t      t1_ms)
{
    if (app == NULL) { return; }

    app->last_image_size = image_size;
    app->last_latency_ms = (t1_ms >= t0_ms) ? (t1_ms - t0_ms) : 0u;

    if ((app->last_frame_tick != 0u) && (t1_ms >= app->last_frame_tick)) {
        uint32_t dt = t1_ms - app->last_frame_tick;
        app->last_fps_x10 = (dt > 0u) ? (10000u / dt) : 0u;
    } else {
        app->last_fps_x10 = 0u;
    }

    app->last_frame_tick = t1_ms;
    app->heap_free_bytes = xPortGetFreeHeapSize();
}

/* ── Reconfiguration helpers ─────────────────────────────────────────────── */

/* Stop the stream task if it is running and wait until it acknowledges.
 * Must be called before any MODE or RES reconfiguration. */
void camcap_stop_stream_if_active(camera_app_t *app)
{
    if (!app->stream_enable) { return; }
    xEventGroupSetBits(app->stream_events, STREAM_EV_STOP);
    while (app->stream_enable) {
        vTaskDelay(pdMS_TO_TICKS(2u));
    }
    camproto_send_info(app->uart, "STREAM auto-stopped for reconfig");
}

/* Apply the currently selected resolution without changing pixel format. */
void camcap_apply_resolution_only(camera_app_t *app)
{
    camcap_stop_stream_if_active(app);
    camcap_invalidate_last_rgb(app);

    if (app->pixfmt == OV2640_PIXFMT_JPEG) {
        if (ov2640_set_resolution_JPEG(&app->cam, app->res) != HAL_OK) {
            camproto_send_err(app->uart, "set_resolution(JPEG) failed");
        } else {
            camproto_send_info(app->uart, "RES applied");
        }
        return;
    }

    if (camcap_current_rgb_bytes(app) > app->frame_buf_size) {
        camproto_send_err(app->uart, "buffer too small for RGB frame");
        return;
    }
    if (ov2640_set_resolution_rgb(&app->cam, app->res) != HAL_OK) {
        camproto_send_err(app->uart, "set_resolution(RGB) failed");
        return;
    }
    if (ov2640_dcmi_set_jpeg_mode(&app->cam, 0u) != HAL_OK) {
        camproto_send_err(app->uart, "DCMI JPEG clear failed");
        return;
    }
    camproto_send_info(app->uart, "RES applied");
}

/* Apply both pixel format and resolution (e.g. after a MODE command). */
void camcap_apply_mode_and_res(camera_app_t *app)
{
    camcap_stop_stream_if_active(app);
    camcap_invalidate_last_rgb(app);

    if (app->pixfmt == OV2640_PIXFMT_JPEG) {
        if (ov2640_set_pixformat(&app->cam, OV2640_PIXFMT_JPEG) != HAL_OK) {
            camproto_send_err(app->uart, "set_pixformat(JPEG) failed");
            return;
        }
        if (ov2640_set_resolution_JPEG(&app->cam, app->res) != HAL_OK) {
            camproto_send_err(app->uart, "set_resolution(JPEG) failed");
            return;
        }
        camproto_send_info(app->uart, "MODE=JPEG");
        return;
    }

    if (camcap_current_rgb_bytes(app) > app->frame_buf_size) {
        camproto_send_err(app->uart, "buffer too small for RGB frame");
        return;
    }
    if (ov2640_set_resolution_rgb(&app->cam, app->res) != HAL_OK) {
        camproto_send_err(app->uart, "set_resolution(RGB) failed");
        return;
    }
    if (ov2640_dcmi_set_jpeg_mode(&app->cam, 0u) != HAL_OK) {
        camproto_send_err(app->uart, "DCMI JPEG clear failed");
        return;
    }
    camproto_send_info(app->uart, "MODE=RGB");
}

/* ── Snapshot ────────────────────────────────────────────────────────────── */

/* Capture one frame and transmit it over UART (JPEG or RGB565). */
void camcap_do_snapshot(camera_app_t *app)
{
    if (app->pixfmt == OV2640_PIXFMT_JPEG) {
        uint32_t words    = ov2640_words_from_bytes(app->frame_buf_size);
        uint32_t t0       = HAL_GetTick();
        uint32_t jpeg_off = 0u;
        uint32_t jpeg_len = 0u;
        char     hdr[64];

        if (ov2640_capture_snapshot_framedone(&app->cam,
                (uint32_t)app->frame_buf, words, 5000u) != HAL_OK) {
            camproto_send_err(app->uart, "SNAP failed");
            return;
        }
        if (!camcap_find_jpeg_bounds(app->frame_buf, app->frame_buf_size,
                                     &jpeg_off, &jpeg_len)) {
            camproto_send_err(app->uart, "JPEG markers not found");
            return;
        }

        camcap_update_stats(app, jpeg_len, t0, HAL_GetTick());
        camcap_invalidate_last_rgb(app);

        snprintf(hdr, sizeof(hdr), "JPG: %lu\r\n", (unsigned long)jpeg_len);
        (void)uart_tx_send_text(app->uart, hdr);
        (void)uart_tx_send_bin(app->uart, app->frame_buf + jpeg_off,
                               jpeg_len, app->cmd_task);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000u));
        camproto_send_info(app->uart, "SNAP JPEG sent");
        camproto_send_status(app);
        return;
    }

    /* RGB565 snapshot. */
    {
        uint16_t w = 0u, h = 0u;
        char     hdr[64];

        if (app->zoom_w != 0u && app->zoom_h != 0u) {
            w = app->zoom_w;
            h = app->zoom_h;
        } else {
            camcap_rgb_dims_from_res(app->res, &w, &h);
        }
        uint32_t bytes = ov2640_rgb565_bytes(w, h);

        if (bytes > app->frame_buf_size) {
            camproto_send_err(app->uart, "buffer too small for RGB frame");
            return;
        }

        uint32_t t0 = HAL_GetTick();
        if (ov2640_capture_rgb565_frame(&app->cam,
                (uint32_t)app->frame_buf, w, h, 5000u) != HAL_OK) {
            camproto_send_err(app->uart, "SNAP RGB failed");
            return;
        }

        camcap_update_stats(app, bytes, t0, HAL_GetTick());
        camcap_mark_last_rgb(app, w, h);

        snprintf(hdr, sizeof(hdr), "RGB565: %u %u %lu\r\n",
                 w, h, (unsigned long)bytes);
        (void)uart_tx_send_text(app->uart, hdr);
        (void)uart_tx_send_bin(app->uart, app->frame_buf, bytes, app->cmd_task);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000u));
        camproto_send_info(app->uart, "SNAP RGB sent");
        camproto_send_status(app);
    }
}

/* ── Last RGB frame query ─────────────────────────────────────────────────── */

/* Return 1 and fill w/h if a valid RGB565 frame is in frame_buf, 0 otherwise. */
int camcap_get_last_rgb_frame_info(camera_app_t *app, uint16_t *w, uint16_t *h)
{
    if ((app == NULL) || (app->last_rgb_valid == 0u)) { return 0; }
    if (ov2640_rgb565_bytes(app->last_rgb_w, app->last_rgb_h) >
        app->frame_buf_size) { return 0; }
    if (w != NULL) { *w = app->last_rgb_w; }
    if (h != NULL) { *h = app->last_rgb_h; }
    return 1;
}
