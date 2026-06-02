/*
 * uart_tx.h
 *
 * Non-blocking UART transmit service backed by FreeRTOS queue and HAL DMA.
 *
 * Text jobs are copied into the queue item (up to UART_TX_TEXT_MAX bytes).
 * Binary jobs store only a pointer; the buffer must remain valid until the
 * optional completion notification is received.
 */

#ifndef INC_UART_TX_H_
#define INC_UART_TX_H_

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "stm32h7xx_hal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum text payload (including NUL) stored inside one queue job. */
#ifndef UART_TX_TEXT_MAX
#define UART_TX_TEXT_MAX      192u
#endif

/* Maximum bytes per single DMA transfer (16-bit HAL length limit). */
#ifndef UART_TX_DMA_CHUNK_MAX
#define UART_TX_DMA_CHUNK_MAX 32768u
#endif

/* Timeout for one DMA chunk in milliseconds. */
#ifndef UART_TX_DMA_TIMEOUT_MS
#define UART_TX_DMA_TIMEOUT_MS 10000u
#endif

typedef enum {
    UART_JOB_TEXT = 0,
    UART_JOB_BIN  = 1
} uart_job_type_t;

typedef struct {
    uart_job_type_t type;
    uint8_t        *data;               /* Binary payload pointer (not copied). */
    char            text[UART_TX_TEXT_MAX]; /* Text payload (copied). */
    uint32_t        len;
    TaskHandle_t    notify_task;        /* Notified after job completes; may be NULL. */
} uart_tx_job_t;

typedef struct {
    UART_HandleTypeDef *huart;
    QueueHandle_t       q;
    TaskHandle_t        worker_task;
} uart_tx_t;

/* Initialise the transmit service. Must be called before any other uart_tx_*. */
BaseType_t uart_tx_init(uart_tx_t *u, UART_HandleTypeDef *huart, uint32_t queue_len);

/* FreeRTOS task function — create with xTaskCreate(uart_tx_task, ..., u). */
void uart_tx_task(void *arg);

/* Queue a NUL-terminated text string for transmission. */
BaseType_t uart_tx_send_text(uart_tx_t *u, const char *s);

/* Queue a binary buffer for transmission.
 * notify_task is notified (xTaskNotifyGive) when the transfer completes. */
BaseType_t uart_tx_send_bin(uart_tx_t *u, uint8_t *data, uint32_t len,
                            TaskHandle_t notify_task);

#ifdef __cplusplus
}
#endif

#endif /* INC_UART_TX_H_ */
