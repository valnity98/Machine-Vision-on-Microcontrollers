/*
 * camera_parse.h
 *
 * Command-line parsing helpers for the UART control interface.
 */

#ifndef INC_CAMERA_PARSE_H_
#define INC_CAMERA_PARSE_H_

#include "shared_helper.h"
#include "camera_app.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t camparse_parse_level (const char *s, ov2640_level_t      *lvl);
uint8_t camparse_parse_res   (const char *s, ov2640_resolution_t *r);
uint8_t camparse_parse_effect(const char *s, ov2640_effect_t     *e);
uint8_t camparse_parse_light (const char *s, ov2640_lightmode_t  *m);

/* Read one complete line from the RX queue into out[0..maxlen-1].
 * Returns 1 when a full line is available, 0 otherwise. */
uint8_t camparse_uart_readline_from_queue(camera_app_t *app,
                                          char         *out,
                                          size_t        maxlen,
                                          TickType_t    wait);

#ifdef __cplusplus
}
#endif

#endif /* INC_CAMERA_PARSE_H_ */
