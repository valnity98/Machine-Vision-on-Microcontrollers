/*
 * camera_capture.h
 *
 * Frame capture helpers for the STM32 Edge Vision camera application.
 */

#ifndef INC_CAMERA_CAPTURE_H_
#define INC_CAMERA_CAPTURE_H_

#include "camera_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scan buf for JPEG SOI/EOI markers.
 * Returns 1 and sets jpeg_off/jpeg_len on success, 0 otherwise. */
int camcap_find_jpeg_bounds(const uint8_t *buf,
                            uint32_t       maxlen,
                            uint32_t      *jpeg_off,
                            uint32_t      *jpeg_len);

/* Map a resolution enum to pixel dimensions. */
void camcap_rgb_dims_from_res(ov2640_resolution_t res, uint16_t *w, uint16_t *h);

uint32_t camcap_current_rgb_bytes    (camera_app_t *app);
uint32_t camcap_current_capture_words(camera_app_t *app);
int      camcap_stream_config_supported(camera_app_t *app);

void camcap_update_stats(camera_app_t *app,
                         uint32_t image_size,
                         uint32_t t0_ms,
                         uint32_t t1_ms);

void camcap_stop_stream_if_active(camera_app_t *app);
void camcap_apply_resolution_only(camera_app_t *app);
void camcap_apply_mode_and_res   (camera_app_t *app);

void camcap_do_snapshot(camera_app_t *app);

/* Return 1 and fill w/h when a valid RGB565 frame is available, 0 otherwise. */
int camcap_get_last_rgb_frame_info(camera_app_t *app, uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif

#endif /* INC_CAMERA_CAPTURE_H_ */
