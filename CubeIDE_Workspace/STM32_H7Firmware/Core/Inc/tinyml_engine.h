/*
 * tinyml_engine.h
 *
 * X-CUBE-AI v10.2 inference wrapper for the STM32 Edge Vision counting model.
 *
 * Required generated files: network.h, network_data.h
 *   AI_NETWORK_OUT_1_SIZE            — number of output classes
 *   AI_NETWORK_IN_1_SIZE             — cross-checked against TINYML_INPUT_ELEMENTS
 *   AI_NETWORK_DATA_ACTIVATIONS_SIZE — build error if absent
 *   AI_NETWORK_DATA_WEIGHTS_SIZE     — build warning if absent
 */

#ifndef INC_TINYML_ENGINE_H_
#define INC_TINYML_ENGINE_H_

#include <stdint.h>
#include "tinyml_preprocess.h"
#include "uart_tx.h"

#ifndef __NETWORK_H__
#include "network.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Number of output classes — derived from X-CUBE-AI macro, default 5. */
#if defined(AI_NETWORK_OUT_1_SIZE)
#define TINYML_NUM_CLASSES (AI_NETWORK_OUT_1_SIZE)
#else
#ifndef TINYML_NUM_CLASSES
#define TINYML_NUM_CLASSES 5u
#endif
#endif

#ifndef TINYML_CLASS_NAMELEN
#define TINYML_CLASS_NAMELEN 16u
#endif

#ifndef TINYML_STATUS_LEN
#define TINYML_STATUS_LEN 48u
#endif

#ifndef TINYML_NAME_LEN
#define TINYML_NAME_LEN 32u
#endif

/* Top-1 confidence threshold in permille (0..1000).
 * Predictions below this value are flagged as UNCERTAIN. */
#ifndef TINYML_UNCERTAIN_THRESHOLD_PERMILLE
#define TINYML_UNCERTAIN_THRESHOLD_PERMILLE 400u
#endif

#ifndef TINYML_MODEL_NAME
#define TINYML_MODEL_NAME "count_model"
#endif

typedef struct {
    uint8_t  enabled;
    uint16_t input_w;          /* set by tinyml_init() to TINYML_INPUT_W */
    uint16_t input_h;          /* set by tinyml_init() to TINYML_INPUT_H */
    char     model_name[TINYML_NAME_LEN];
} tinyml_config_t;

typedef struct {
    char     runtime_status[TINYML_STATUS_LEN]; /* "XCUBEAI_READY", "XCUBEAI_ERR_RUN", etc. */
    char     predicted_name[TINYML_NAME_LEN];   /* class label or "UNCERTAIN"               */
    uint16_t predicted_index;                   /* index of the top-1 class                 */
    uint16_t confidence_permille;               /* top-1 confidence 0..1000 (permille)      */
    uint8_t  is_uncertain;                      /* 1 when confidence < TINYML_UNCERTAIN_THRESHOLD_PERMILLE */
    uint32_t inference_time_ms;
    uint16_t class_count;
    uint16_t ram_kb;
    uint16_t flash_kb;
    char     class_names[TINYML_NUM_CLASSES][TINYML_CLASS_NAMELEN];
    uint16_t scores_permille[TINYML_NUM_CLASSES]; /* per-class score 0..1000 (permille)     */
} tinyml_result_t;

/* Register the UART used for TM_IN / TM_HEAD32 / TM_RAW_OUT diagnostics.
 * Call once from camera_app_init().  Pass NULL to disable diagnostics. */
void tinyml_set_diag_uart(uart_tx_t *uart);

/* Initialise the network.  Must be called before tinyml_run_rgb565().
 * Sets cfg->input_w and cfg->input_h to TINYML_INPUT_W/H on success. */
int tinyml_init(tinyml_config_t *cfg, tinyml_result_t *res);

/* Run preprocessing and inference on one RGB565 frame.
 * Returns 0 on success, negative on error. */
int tinyml_run_rgb565(const uint16_t        *rgb565,
                      uint16_t               width,
                      uint16_t               height,
                      const tinyml_config_t *cfg,
                      tinyml_result_t       *res);

/* Return a build-time string identifying the model and preprocessing version. */
const char *tinyml_get_model_build_id(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_TINYML_ENGINE_H_ */
