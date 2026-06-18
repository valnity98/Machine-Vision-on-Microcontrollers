/*
 * camera_proto.c
 *
 * UART protocol formatters for STM32H7 Edge Vision.
 *
 * Message types:
 *   INFO / WARN / ERR / DEBUG   — log lines with prefix "INFO: ..."
 *   STAT                        — capture statistics
 *   CVCFG                       — CV pipeline configuration
 *   CVSTAT / CVBOX / CVDONE     — CV pipeline result
 *   TMCFG / TMINFO              — TinyML model configuration
 *   TMRES / TMPROB / TMDONE     — TinyML inference result
 *
 * All fields use key=value format separated by spaces.
 * Unknown fields are silently ignored by older GUI versions.
 */

#include "camera_proto.h"

#include <stddef.h>
#include <stdio.h>

/* ── Internal helper ─────────────────────────────────────────────────────── */

/* Format "PREFIX: msg\r\n" and queue for transmission.
 * Truncates silently when the formatted string exceeds the buffer. */
static void send_prefixed(uart_tx_t *u, const char *prefix, const char *msg)
{
    char line[192];
    int  n = snprintf(line, sizeof(line), "%s: %s\r\n", prefix, msg ? msg : "");
    if (n < 0) { return; }
    /* Ensure valid line termination when snprintf truncated. */
    if ((size_t)n >= sizeof(line)) {
        line[sizeof(line) - 3u] = '\r';
        line[sizeof(line) - 2u] = '\n';
        line[sizeof(line) - 1u] = '\0';
    }
    (void)uart_tx_send_text(u, line);
}

/* ── Log messages ────────────────────────────────────────────────────────── */

void camproto_send_info (uart_tx_t *u, const char *msg) { send_prefixed(u, "INFO",  msg); }
void camproto_send_warn (uart_tx_t *u, const char *msg) { send_prefixed(u, "WARN",  msg); }
void camproto_send_err  (uart_tx_t *u, const char *msg) { send_prefixed(u, "ERR",   msg); }
void camproto_send_debug(uart_tx_t *u, const char *msg) { send_prefixed(u, "DEBUG", msg); }

/* ── STAT ─────────────────────────────────────────────────────────────────── */

/* Send capture statistics: FPS (x10), last image size, heap free, frame buffer
 * size, and latency.  FB is taken directly from app->frame_buf_size. */
void camproto_send_status(camera_app_t *app)
{
    char line[160];
    if ((app == NULL) || (app->uart == NULL)) { return; }
    snprintf(line, sizeof(line),
             "STAT: FPS=%u.%u SIZE=%luB HEAP=%uKB FB=%uKB LAT=%ums\r\n",
             (unsigned)(app->last_fps_x10 / 10u),
             (unsigned)(app->last_fps_x10 % 10u),
             (unsigned long)app->last_image_size,
             (unsigned)(app->heap_free_bytes  / 1024u),
             (unsigned)(app->frame_buf_size   / 1024u),
             (unsigned)app->last_latency_ms);
    (void)uart_tx_send_text(app->uart, line);
}

/* ── CVCFG ───────────────────────────────────────────────────────────────── */

/* Send the complete CV pipeline configuration. */
void camproto_send_cv_config(camera_app_t *app)
{
    char line[320];
    if ((app == NULL) || (app->uart == NULL)) { return; }
    snprintf(line, sizeof(line),
             "CVCFG: EN=%u PRESET=%u "
             "THR=%u THRMODE=%u INV=%u "
             "BLUR=%u FILTER=%u "
             "MORPH=%u MORPHMODE=%u CON=%u "
             "MIN=%lu MAX=%lu "
             "ARMIN=%u ARMAX=%u CIRCMIN=%u "
             "BORDFILT=%u BGSUB=%u BGCAP=%u "
             "ROIEN=%u ROIX=%u ROIY=%u ROIW=%u ROIH=%u\r\n",
             (unsigned)app->cv_cfg.enabled,
             (unsigned)app->cv_cfg.preset,
             (unsigned)app->cv_cfg.threshold,
             (unsigned)app->cv_cfg.thr_mode,
             (unsigned)app->cv_cfg.invert,
             (unsigned)app->cv_cfg.blur_kernel,
             (unsigned)app->cv_cfg.filter_mode,
             (unsigned)app->cv_cfg.morph_kernel,
             (unsigned)app->cv_cfg.morph_mode,
             (unsigned)app->cv_cfg.connectivity,
             (unsigned long)app->cv_cfg.min_area,
             (unsigned long)app->cv_cfg.max_area,
             (unsigned)app->cv_cfg.aspect_ratio_min_x1000,
             (unsigned)app->cv_cfg.aspect_ratio_max_x1000,
             (unsigned)app->cv_cfg.circularity_min_x1000,
             (unsigned)app->cv_cfg.border_filter_enabled,
             (unsigned)app->cv_cfg.bgsub_enabled,
             (unsigned)app->bg_captured,
             (unsigned)app->cv_cfg.roi_enabled,
             (unsigned)app->cv_cfg.roi_x,
             (unsigned)app->cv_cfg.roi_y,
             (unsigned)app->cv_cfg.roi_w,
             (unsigned)app->cv_cfg.roi_h);
    (void)uart_tx_send_text(app->uart, line);
}

/* ── CVSTAT / CVBOX / CVDONE ─────────────────────────────────────────────── */

/* Send CV pipeline result: summary stats, per-box details, then CVDONE. */
void camproto_send_cv_result(camera_app_t *app)
{
    char line[320];
    if ((app == NULL) || (app->uart == NULL)) { return; }

    snprintf(line, sizeof(line),
             "CVSTAT: COUNT=%lu MEAN=%lu MAX=%lu MIN=%lu BRIGHT=%lu "
             "TIME=%lu REJSMALL=%lu REJLARGE=%lu "
             "REJBORDER=%lu REJSHAPE=%lu "
             "FGPIX=%lu RAWCOMP=%lu BOXES=%u\r\n",
             (unsigned long)app->cv_res.object_count,
             (unsigned long)app->cv_res.mean_area,
             (unsigned long)app->cv_res.area_max,
             (unsigned long)app->cv_res.area_min,
             (unsigned long)app->cv_res.mean_brightness,
             (unsigned long)app->cv_res.processing_time_ms,
             (unsigned long)app->cv_res.rejected_small,
             (unsigned long)app->cv_res.rejected_large,
             (unsigned long)app->cv_res.rejected_border,
             (unsigned long)app->cv_res.rejected_shape,
             (unsigned long)app->cv_res.fg_pixel_count,
             (unsigned long)app->cv_res.raw_comp_count,
             (unsigned)app->cv_res.box_count);
    (void)uart_tx_send_text(app->uart, line);

    for (uint16_t i = 0u; i < app->cv_res.box_count; ++i) {
        const cv_box_t *b = &app->cv_res.boxes[i];
        snprintf(line, sizeof(line),
                 "CVBOX: ID=%u AREA=%lu X=%u Y=%u W=%u H=%u PERI=%lu CIRC=%u\r\n",
                 (unsigned)b->id,        (unsigned long)b->area,
                 (unsigned)b->x,         (unsigned)b->y,
                 (unsigned)b->w,         (unsigned)b->h,
                 (unsigned long)b->perimeter,
                 (unsigned)b->circularity_x1000);
        (void)uart_tx_send_text(app->uart, line);
    }
    (void)uart_tx_send_text(app->uart, "CVDONE\r\n");
}

/* ── TMCFG ───────────────────────────────────────────────────────────────── */

/* Send TinyML model configuration: enabled flag, input shape, class count, model name. */
void camproto_send_tm_config(camera_app_t *app)
{
    char line[192];
    if ((app == NULL) || (app->uart == NULL)) { return; }
    snprintf(line, sizeof(line),
             "TMCFG: EN=%u INPUT=%ux%ux%u CLASSES=%u MODEL=%s\r\n",
             (unsigned)app->tm_cfg.enabled,
             (unsigned)TINYML_INPUT_W,
             (unsigned)TINYML_INPUT_H,
             (unsigned)TINYML_INPUT_CHANNELS,
             (unsigned)app->tm_res.class_count,
             app->tm_cfg.model_name);
    (void)uart_tx_send_text(app->uart, line);
}

/* ── TMINFO ──────────────────────────────────────────────────────────────── */

/* Send TinyML runtime status and memory footprint. */
void camproto_send_tm_info(camera_app_t *app)
{
    char line[192];
    if ((app == NULL) || (app->uart == NULL)) { return; }
    snprintf(line, sizeof(line),
             "TMINFO: STATUS=%s RAM=%uKB FLASH=%uKB\r\n",
             app->tm_res.runtime_status,
             (unsigned)app->tm_res.ram_kb,
             (unsigned)app->tm_res.flash_kb);
    (void)uart_tx_send_text(app->uart, line);
}

/* ── TMRES / TMPROB / TMDONE ─────────────────────────────────────────────── */

/* Send TinyML inference result: top-1 prediction, per-class scores, then TMDONE.
 * Scores are in permille (0..1000). */
void camproto_send_tm_result(camera_app_t *app)
{
    char line[256];
    if ((app == NULL) || (app->uart == NULL)) { return; }

    snprintf(line, sizeof(line),
             "TMRES: CLASS=%s IDX=%d CONF=%u TIME=%lu UNCERTAIN=%u\r\n",
             app->tm_res.predicted_name,
             (int)(app->tm_res.is_uncertain
                   ? -1
                   : (int)app->tm_res.predicted_index),
             (unsigned)app->tm_res.confidence_permille,
             (unsigned long)app->tm_res.inference_time_ms,
             (unsigned)app->tm_res.is_uncertain);
    (void)uart_tx_send_text(app->uart, line);

    for (uint16_t i = 0u; i < app->tm_res.class_count; ++i) {
        snprintf(line, sizeof(line),
                 "TMPROB: IDX=%u NAME=%s SCORE=%u\r\n",
                 (unsigned)i,
                 app->tm_res.class_names[i],
                 (unsigned)app->tm_res.scores_permille[i]);
        (void)uart_tx_send_text(app->uart, line);
    }
    (void)uart_tx_send_text(app->uart, "TMDONE\r\n");
}
