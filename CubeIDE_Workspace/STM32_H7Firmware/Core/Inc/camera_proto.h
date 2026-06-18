/*
 * camera_proto.h
 *
 * UART protocol formatter for STM32 Edge Vision.
 *
 * MCU -> GUI message prefixes:
 *   INFO / WARN / ERR / DEBUG
 *   STAT
 *   JPG / RGB565
 *   CVCFG / CVSTAT / CVBOX / CVDONE
 *   TMCFG / TMINFO / TMRES / TMPROB / TMDONE
 *
 * Field names must remain stable — the Python GUI parser depends on them.
 */

#ifndef INC_CAMERA_PROTO_H_
#define INC_CAMERA_PROTO_H_

#include "uart_tx.h"
#include "camera_app.h"

#ifdef __cplusplus
extern "C" {
#endif

void camproto_send_info (uart_tx_t *uart, const char *msg);
void camproto_send_warn (uart_tx_t *uart, const char *msg);
void camproto_send_err  (uart_tx_t *uart, const char *msg);
void camproto_send_debug(uart_tx_t *uart, const char *msg);

void camproto_send_status(camera_app_t *app);

void camproto_send_cv_config(camera_app_t *app);
void camproto_send_cv_result(camera_app_t *app);

void camproto_send_tm_config(camera_app_t *app);
void camproto_send_tm_info  (camera_app_t *app);
void camproto_send_tm_result(camera_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* INC_CAMERA_PROTO_H_ */
