/*
 * uart_tx.c
 *
 * Non-blocking UART transmit service backed by FreeRTOS queue and HAL DMA.
 */

#include "uart_tx.h"

#include <string.h>
#include <stdint.h>
#include "core_cm7.h"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------*/

static void uart_tx_clean_dcache(const void *addr, uint32_t len)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)31u;
    uintptr_t end   = ((uintptr_t)addr + (uintptr_t)len + 31u) & ~(uintptr_t)31u;
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
#else
    (void)addr;
    (void)len;
#endif
}

/* Send one DMA chunk and block until the TxCplt ISR notifies this task. */
static BaseType_t uart_tx_send_chunk(uart_tx_t *u, uint8_t *data, uint16_t len)
{
    if ((u == NULL) || (u->huart == NULL) || (data == NULL) || (len == 0u)) {
        return pdFALSE;
    }

    /* Drain any stale notification before arming the transfer. */
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    uart_tx_clean_dcache(data, (uint32_t)len);

    while (HAL_UART_Transmit_DMA(u->huart, data, len) != HAL_OK) {
        vTaskDelay(pdMS_TO_TICKS(1u));
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UART_TX_DMA_TIMEOUT_MS)) == 0u) {
        (void)HAL_UART_DMAStop(u->huart);
        return pdFALSE;
    }

    return pdTRUE;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

BaseType_t uart_tx_init(uart_tx_t *u, UART_HandleTypeDef *huart, uint32_t queue_len)
{
    if ((u == NULL) || (huart == NULL) || (queue_len == 0u)) {
        return pdFALSE;
    }

    memset(u, 0, sizeof(*u));
    u->huart = huart;
    u->q     = xQueueCreate(queue_len, sizeof(uart_tx_job_t));

    return (u->q != NULL) ? pdTRUE : pdFALSE;
}

BaseType_t uart_tx_send_text(uart_tx_t *u, const char *s)
{
    uart_tx_job_t job;
    size_t        n;

    if ((u == NULL) || (u->q == NULL) || (s == NULL)) {
        return pdFALSE;
    }

    memset(&job, 0, sizeof(job));
    job.type = UART_JOB_TEXT;
    n        = strnlen(s, UART_TX_TEXT_MAX - 1u);
    memcpy(job.text, s, n);
    job.text[n] = '\0';
    job.len     = (uint32_t)n;

    return xQueueSend(u->q, &job, portMAX_DELAY);
}

BaseType_t uart_tx_send_bin(uart_tx_t *u, uint8_t *data, uint32_t len,
                            TaskHandle_t notify_task)
{
    uart_tx_job_t job;

    if ((u == NULL) || (u->q == NULL) || (data == NULL) || (len == 0u)) {
        return pdFALSE;
    }

    memset(&job, 0, sizeof(job));
    job.type        = UART_JOB_BIN;
    job.data        = data;
    job.len         = len;
    job.notify_task = notify_task;

    return xQueueSend(u->q, &job, portMAX_DELAY);
}

void uart_tx_task(void *arg)
{
    uart_tx_t    *u = (uart_tx_t *)arg;
    uart_tx_job_t job;

    if (u == NULL) {
        vTaskDelete(NULL);
        return;
    }

    u->worker_task = xTaskGetCurrentTaskHandle();

    for (;;) {
        uint8_t  *p;
        uint32_t  remaining;

        if (xQueueReceive(u->q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        p         = (job.type == UART_JOB_TEXT) ? (uint8_t *)job.text : job.data;
        remaining = job.len;

        while (remaining > 0u) {
            uint16_t chunk = (remaining > UART_TX_DMA_CHUNK_MAX)
                             ? (uint16_t)UART_TX_DMA_CHUNK_MAX
                             : (uint16_t)remaining;

            if (uart_tx_send_chunk(u, p, chunk) != pdTRUE) {
                break;
            }

            p         += chunk;
            remaining -= chunk;
        }

        /* Always notify the waiting task, even on error, to prevent deadlock. */
        if (job.notify_task != NULL) {
            xTaskNotifyGive(job.notify_task);
        }
    }
}
