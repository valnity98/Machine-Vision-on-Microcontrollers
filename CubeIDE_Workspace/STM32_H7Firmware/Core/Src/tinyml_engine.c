/*
 * tinyml_engine.c
 *
 * X-CUBE-AI v10.2 inference wrapper for the STM32 Edge Vision counting model.
 *
 * Input contract (must match user_config.yaml and training pipeline):
 *   Shape:   [1, 96, 96, 3]  HWC layout
 *   Dtype:   uint8  (raw [0..255], no float conversion)
 *   Preproc: RGB565 full-frame nearest-neighbour resize, BYTESWAP=0
 *
 * Key design decisions:
 *   1. stai_runtime_init() must be called before ai_network_create_and_init().
 *   2. ai_network_create_and_init() is called with weights=NULL.
 *      The internal ai_network_data_params_get() already points to the Flash
 *      weight array; passing a non-NULL pointer corrupts inference.
 *   3. Input and output tensors live inside g_activations (RAM_D3, NOLOAD).
 *      Write the preprocessed tensor directly into ai_in[0].data.
 *   4. g_activations is memset to 0 before every ai_network_create_and_init()
 *      call because NOLOAD sections are not zeroed by startup code.
 *   5. RAM_D3 (0x38000000) must be covered by a Non-Cacheable MPU region
 *      (Region 3 in MPU_Config) — see main.c.  Without it the D-Cache can
 *      serve stale activation data between inference calls.
 */

#include "tinyml_engine.h"
#include "network.h"
#include "network_data.h"
#include "camera_proto.h"
#include "uart_tx.h"
#include "stai.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Compile-time check: X-CUBE-AI input size must match preprocessing constants. */
#if defined(AI_NETWORK_IN_1_SIZE)
_Static_assert(AI_NETWORK_IN_1_SIZE == TINYML_INPUT_ELEMENTS,
               "AI_NETWORK_IN_1_SIZE != TINYML_INPUT_ELEMENTS — "
               "regenerate X-CUBE-AI files or update tinyml_preprocess.h.");
#endif

#ifndef AI_NETWORK_DATA_ACTIVATIONS_SIZE
#error "AI_NETWORK_DATA_ACTIVATIONS_SIZE missing — regenerate X-CUBE-AI network files."
#endif

#ifndef AI_NETWORK_DATA_WEIGHTS_SIZE
#warning "AI_NETWORK_DATA_WEIGHTS_SIZE not defined — Flash usage reported as 0 KB."
#define AI_NETWORK_DATA_WEIGHTS_SIZE 0u
#endif

/* Round bytes up to the nearest kilobyte. */
#define KB_CEIL(bytes) ((uint16_t)((((uint32_t)(bytes)) + 1023u) / 1024u))

/* ---------------------------------------------------------------------------
 * Static storage
 * ---------------------------------------------------------------------------*/

static ai_handle g_network    = AI_HANDLE_NULL;
static uart_tx_t *g_diag_uart = NULL;

/* Activation buffer in RAM_D3 (.tinyml_buf, NOLOAD).
 * Placed in RAM_D3 (64 KB Backup SRAM) so neither RAM_D1 (DMA buffers)
 * nor RAM_D2 (CV scratch) are affected.
 * X-CUBE-AI only requires CPU access; no DMA needed for activations.
 * Must be zeroed by tinyml_init() before every ai_network_create_and_init()
 * call because NOLOAD sections are not initialised by startup code. */
static ai_u8 g_activations[AI_NETWORK_DATA_ACTIVATIONS_SIZE]
    __attribute__((section(".tinyml_buf"), aligned(32)));

static const char *const g_class_names[TINYML_NUM_CLASSES] = {
#if TINYML_NUM_CLASSES == 5u
    "COUNT_0", "COUNT_1", "COUNT_2", "COUNT_3", "COUNT_4"
#else
#error "Update g_class_names when TINYML_NUM_CLASSES changes."
#endif
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------*/

/* Reset result to a safe default state before each inference run. */
static void reset_result(tinyml_result_t *res)
{
    if (res == NULL) { return; }
    memset(res, 0, sizeof(*res));
    res->is_uncertain        = 1u;
    res->class_count         = TINYML_NUM_CLASSES;
    res->ram_kb              = KB_CEIL(AI_NETWORK_DATA_ACTIVATIONS_SIZE);
    res->flash_kb            = KB_CEIL(AI_NETWORK_DATA_WEIGHTS_SIZE);
    snprintf(res->predicted_name, sizeof(res->predicted_name), "N/A");
    snprintf(res->runtime_status, sizeof(res->runtime_status), "NOT_INITIALIZED");
    for (uint16_t i = 0u; i < TINYML_NUM_CLASSES; ++i) {
        snprintf(res->class_names[i], TINYML_CLASS_NAMELEN, "%s", g_class_names[i]);
    }
}

/* Log input tensor statistics (min/max/mean, FNV-1a hash, spot pixels, hex dump).
 * Called only when g_diag_uart is non-NULL. */
static void send_input_diagnostics(const uint8_t *buf)
{
    uint32_t sum = 0u, hash = 2166136261u;
    uint8_t  minv = 255u, maxv = 0u;
    char     msg[160];

    if ((buf == NULL) || (g_diag_uart == NULL)) { return; }

    for (uint32_t i = 0u; i < TINYML_INPUT_ELEMENTS; ++i) {
        uint8_t v = buf[i];
        if (v < minv) { minv = v; }
        if (v > maxv) { maxv = v; }
        sum  += v;
        hash ^= v;
        hash *= 16777619u;
    }

    snprintf(msg, sizeof(msg),
             "TM_IN: min=%u max=%u mean=%lu sum=%lu hash=%08lX"
             " p0=%u p100=%u p1000=%u plast=%u",
             (unsigned)minv, (unsigned)maxv,
             (unsigned long)(sum / TINYML_INPUT_ELEMENTS),
             (unsigned long)sum, (unsigned long)hash,
             (unsigned)buf[0], (unsigned)buf[100],
             (unsigned)buf[1000], (unsigned)buf[TINYML_INPUT_ELEMENTS - 1u]);
    camproto_send_info(g_diag_uart, msg);

    /* First 32 bytes as grouped hex. */
    {
        static const char hc[] = "0123456789ABCDEF";
        char     hex[80];
        uint32_t pos = 0u;
        for (uint32_t i = 0u; i < 32u; ++i) {
            uint8_t v = buf[i];
            hex[pos++] = hc[v >> 4u];
            hex[pos++] = hc[v & 0x0Fu];
            if ((i & 3u) == 3u) { hex[pos++] = ' '; }
        }
        hex[pos] = '\0';
        snprintf(msg, sizeof(msg), "TM_HEAD32: %s", hex);
        camproto_send_info(g_diag_uart, msg);
    }
}

/* Run the network and decode the output tensor into res.
 * The input tensor must be written into ai_in[0].data before calling.
 * Applies softmax when the output is not already a probability distribution. */
static int run_inference(ai_buffer *ai_in, ai_buffer *ai_out, tinyml_result_t *res)
{
    const float *out_data;
    uint16_t     best_idx   = 0u;
    float        best_score = 0.0f;

    if (res == NULL) { return -1; }

    ai_i32 batches = ai_network_run(g_network, ai_in, ai_out);
    if (batches != 1) {
        ai_error e = ai_network_get_error(g_network);
        char dbg[100];
        snprintf(dbg, sizeof(dbg),
                 "AI_RUN_FAIL: batches=%ld type=0x%02X code=0x%02X",
                 (long)batches, (unsigned)e.type, (unsigned)e.code);
        camproto_send_err(g_diag_uart, dbg);
        snprintf(res->runtime_status, sizeof(res->runtime_status), "XCUBEAI_ERR_RUN");
        return -4;
    }

    if (g_diag_uart != NULL) {
        const float *f = (const float *)ai_out[0].data;
        char dbg[192];
        snprintf(dbg, sizeof(dbg),
                 "TM_RAW_OUT: fmt=0x%08lX size=%lu"
                 " f0=%.6f f1=%.6f f2=%.6f f3=%.6f f4=%.6f",
                 (unsigned long)ai_out[0].format,
                 (unsigned long)AI_BUFFER_SIZE(&ai_out[0]),
                 (double)f[0], (double)f[1], (double)f[2],
                 (double)f[3], (double)f[4]);
        camproto_send_info(g_diag_uart, dbg);
    }

    out_data = (const float *)ai_out[0].data;

    /* Apply softmax when the output is not already a valid probability distribution.
     * softmax_buf is function-static (non-reentrant); safe on bare-metal
     * single-threaded inference. */
    {
        static float softmax_buf[TINYML_NUM_CLASSES];
        float   prob_sum = 0.0f, sum_exp = 0.0f, max_val = out_data[0];
        uint8_t need_sm  = 0u;

        for (uint16_t i = 0u; i < TINYML_NUM_CLASSES; ++i) {
            prob_sum += out_data[i];
            if ((out_data[i] < 0.0f) || (out_data[i] > 1.0f)) { need_sm = 1u; }
        }
        if ((prob_sum < 0.98f) || (prob_sum > 1.02f)) { need_sm = 1u; }

        if (need_sm) {
            for (uint16_t i = 1u; i < TINYML_NUM_CLASSES; ++i) {
                if (out_data[i] > max_val) { max_val = out_data[i]; }
            }
            for (uint16_t i = 0u; i < TINYML_NUM_CLASSES; ++i) {
                softmax_buf[i]  = expf(out_data[i] - max_val);
                sum_exp        += softmax_buf[i];
            }
            for (uint16_t i = 0u; i < TINYML_NUM_CLASSES; ++i) {
                softmax_buf[i] /= sum_exp;
            }
            out_data = softmax_buf;
        }
    }

    /* Decode scores and find top-1. */
    for (uint16_t i = 0u; i < TINYML_NUM_CLASSES; ++i) {
        float score = out_data[i];
        if      (score < 0.0f) { score = 0.0f; }
        else if (score > 1.0f) { score = 1.0f; }
        res->scores_permille[i] = (uint16_t)(score * 1000.0f + 0.5f);
        if (score > best_score) { best_score = score; best_idx = i; }
    }

    res->predicted_index     = best_idx;
    res->confidence_permille = res->scores_permille[best_idx];

    if (res->confidence_permille < TINYML_UNCERTAIN_THRESHOLD_PERMILLE) {
        snprintf(res->predicted_name, sizeof(res->predicted_name), "UNCERTAIN");
        res->is_uncertain = 1u;
    } else {
        snprintf(res->predicted_name, sizeof(res->predicted_name),
                 "%s", res->class_names[best_idx]);
        res->is_uncertain = 0u;
    }

    snprintf(res->runtime_status, sizeof(res->runtime_status), "XCUBEAI_OK");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

/* Return a build-time string identifying the model and preprocessing version. */
const char *tinyml_get_model_build_id(void)
{
    return TINYML_MODEL_NAME
           " | preproc=rgb565_u8rgb_fullframe_nn_v6 | "
           __DATE__ " " __TIME__;
}

/* Register the UART for diagnostic output.  Pass NULL to disable. */
void tinyml_set_diag_uart(uart_tx_t *uart)
{
    g_diag_uart = uart;
}

/* Initialise the X-CUBE-AI network.  Must be called before tinyml_run_rgb565(). */
int tinyml_init(tinyml_config_t *cfg, tinyml_result_t *res)
{
    const ai_handle activations[] = { g_activations };
    ai_error        err;
    stai_return_code rc;

    if (cfg == NULL) { return -1; }

    cfg->input_w = TINYML_INPUT_W;
    cfg->input_h = TINYML_INPUT_H;
    snprintf(cfg->model_name, sizeof(cfg->model_name), "%s", TINYML_MODEL_NAME);
    reset_result(res);

    /* Zero activations — NOLOAD section is not zeroed by startup code. */
    memset(g_activations, 0, sizeof(g_activations));

    /* Initialise STAI runtime layer (required by X-CUBE-AI v10.2). */
    rc = stai_runtime_init();
    if (rc != STAI_SUCCESS) {
        if (res != NULL) {
            snprintf(res->runtime_status, sizeof(res->runtime_status),
                     "STAI_INIT_FAIL_%d", (int)rc);
        }
        return -1;
    }

    /* Destroy any previously initialised network handle. */
    if (g_network != AI_HANDLE_NULL) {
        (void)ai_network_destroy(g_network);
        g_network = AI_HANDLE_NULL;
    }

    /* Pass weights=NULL: ai_network_data_params_get() sets the Flash pointer
     * internally.  A non-NULL pointer overrides it with a wrapper address,
     * corrupting inference. */
    err = ai_network_create_and_init(&g_network, activations, NULL);
    if (err.type != AI_ERROR_NONE) {
        if (res != NULL) {
            snprintf(res->runtime_status, sizeof(res->runtime_status),
                     "XCUBEAI_INIT_FAILED");
        }
        snprintf(cfg->model_name, sizeof(cfg->model_name),
                 "%s_INIT_ERR", TINYML_MODEL_NAME);
        g_network = AI_HANDLE_NULL;
        return -1;
    }

    if (res != NULL) {
        snprintf(res->runtime_status, sizeof(res->runtime_status), "XCUBEAI_READY");
    }
    return 0;
}

/* Run preprocessing and inference on one RGB565 frame.
 * Returns 0 on success, negative error code on failure. */
int tinyml_run_rgb565(const uint16_t        *rgb565,
                      uint16_t               width,
                      uint16_t               height,
                      const tinyml_config_t *cfg,
                      tinyml_result_t       *res)
{
    ai_u16     n_in = 0u, n_out = 0u;
    ai_buffer *ai_in  = NULL;
    ai_buffer *ai_out = NULL;

    if ((rgb565 == NULL) || (cfg == NULL) || (res == NULL)) { return -1; }
    if ((width  == 0u)   || (height == 0u))                  { return -2; }
    if ((cfg->input_w != TINYML_INPUT_W) ||
        (cfg->input_h != TINYML_INPUT_H))                    { return -3; }
    if (g_network == AI_HANDLE_NULL)                          { return -4; }

    ai_in  = ai_network_inputs_get (g_network, &n_in);
    ai_out = ai_network_outputs_get(g_network, &n_out);

    if ((ai_in  == NULL) || (n_in  < 1u) || (ai_in[0].data  == NULL) ||
        (ai_out == NULL) || (n_out < 1u) || (ai_out[0].data == NULL)) {
        return -5;
    }

    reset_result(res);

    /* Invalidate D-Cache over the DMA-written framebuffer before CPU reads. */
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    {
        uint32_t frame_bytes  = (uint32_t)width * height * 2u;
        uint32_t addr_aligned = (uint32_t)rgb565 & ~0x1Fu;
        uint32_t offset       = (uint32_t)rgb565 - addr_aligned;
        uint32_t size_aligned = (frame_bytes + offset + 31u) & ~0x1Fu;
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr_aligned, (int32_t)size_aligned);
    }
#endif

    /* Preprocess RGB565 frame directly into the X-CUBE-AI input tensor buffer. */
    tinyml_preprocess_rgb565_to_u8rgb(rgb565, width, height,
                                      (uint8_t *)ai_in[0].data);

    send_input_diagnostics((const uint8_t *)ai_in[0].data);

    return run_inference(ai_in, ai_out, res);
}
