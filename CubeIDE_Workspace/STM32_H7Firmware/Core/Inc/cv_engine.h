/*
 * cv_engine.h
 *
 * Classical vision pipeline for STM32H7 Edge Vision — object counting.
 *
 * Pipeline (cv_run_rgb565):
 *   1. RGB565 → grayscale (BT.601 per-pixel, no heap)
 *   2. Background subtraction (optional, bg_buf in RAM_D2)
 *   3. Pre-threshold spatial filter (box or median)
 *   4. Threshold (manual or Otsu)
 *   5. Binary morphology (open / close / erode / dilate)
 *   6. Run-length CCL + union-find (fixed stack arrays, no heap)
 *   7. Object filter: area / aspect-ratio / circularity / border
 *
 * All image processing functions are own implementations.
 * STM32IPL was evaluated and rejected for every CV stage because all
 * STM32IPL image functions require image_t wrappers and an internal heap
 * pool (STM32Ipl_InitLib), adding RAM overhead and non-determinism with
 * no accuracy benefit.  STM32Ipl_Resize() is used only in tinyml_preprocess.c.
 *
 * Buffer contract (caller must allocate):
 *   work_bin (cvBinBuffer): proc_w * proc_h bytes, in .dcmi_buf → RAM_D1
 *   tmp_buf  (cvTmpBuffer): proc_w * proc_h bytes, in .dcmi_buf → RAM_D1
 *   bg_buf   (cvBgBuffer) : proc_w * proc_h bytes, in .cvbg_buf → RAM_D2
 *   proc_w / proc_h = full capture size when ROI is disabled.
 */

#ifndef INC_CV_ENGINE_H_
#define INC_CV_ENGINE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Compile-time limits
 * ========================================================================= */

#ifndef CV_MAX_BOXES
#define CV_MAX_BOXES         8u   /* max bounding boxes reported per frame  */
#endif
#ifndef CV_MAX_COMPONENTS
#define CV_MAX_COMPONENTS   48u   /* max CCL labels on the stack             */
#endif
#ifndef CV_MAX_RUNS_PER_ROW
#define CV_MAX_RUNS_PER_ROW 128u  /* max foreground run-segments per row     */
#endif

/*
 * Peak stack usage inside cv_run_rgb565 (documentation only):
 *   cv_run_t       prev_runs[128]       6 * 128 =   768 B
 *   cv_run_t       curr_runs[128]       6 * 128 =   768 B
 *   cv_component_t comp[49]            16 *  49 =   784 B  (sizeof = 16 B)
 *   uint32_t       hist[256] (Otsu)     4 * 256 =  1024 B
 *   ─────────────────────────────────────────────────────
 *   Total                                        ≈ 3344 B  (≈ 3.3 KB)
 *
 * sizeof(cv_component_t): parent(2) + used(1) + valid(1) + [pad(0)] +
 *                         area(4) + min_x(2) + min_y(2) + max_x(2) + max_y(2)
 *                       = 16 bytes (naturally aligned, no padding needed).
 */

/* =========================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    CV_THR_MANUAL = 0u,  /* fixed threshold value      */
    CV_THR_OTSU   = 1u   /* Otsu automatic threshold   */
} cv_thr_mode_t;

typedef enum {
    CV_FILTER_OFF    = 0u,  /* no pre-threshold filter    */
    CV_FILTER_BOX    = 1u,  /* box (mean) blur            */
    CV_FILTER_MEDIAN = 2u   /* median filter (max 5×5)    */
} cv_filter_mode_t;

typedef enum {
    CV_MORPH_OFF    = 0u,  /* no morphology              */
    CV_MORPH_OPEN   = 1u,  /* erode then dilate          */
    CV_MORPH_CLOSE  = 2u,  /* dilate then erode          */
    CV_MORPH_ERODE  = 3u,  /* erosion only               */
    CV_MORPH_DILATE = 4u   /* dilation only              */
} cv_morph_mode_t;

typedef enum {
    CV_PRESET_CUSTOM   = 0u,  /* manual parameter control                   */
    CV_PRESET_FAST     = 1u,  /* no filter, Otsu, no morph                  */
    CV_PRESET_ROBUST   = 2u,  /* 3×3 box, Otsu, 3×3 open                   */
    CV_PRESET_ACCURATE = 3u   /* 5×5 median, Otsu, 5×5 open                */
} cv_preset_t;

/* =========================================================================
 * Configuration
 * ========================================================================= */

typedef struct {
    uint8_t          enabled;

    cv_preset_t      preset;           /* CV_PRESET_CUSTOM = fully manual    */

    /* Step 2: Background subtraction. */
    uint8_t          bgsub_enabled;    /* 1 = subtract bg_buf before threshold */

    /* Step 3: Pre-threshold filter. */
    cv_filter_mode_t filter_mode;
    uint8_t          blur_kernel;      /* 0=off, 1..7 → kernel size 3..15   */

    /* Step 4: Threshold. */
    cv_thr_mode_t    thr_mode;
    uint8_t          threshold;        /* manual value 0–255                 */
    uint8_t          invert;           /* 1 = invert binary result           */

    /* Step 5: Morphology. */
    cv_morph_mode_t  morph_mode;
    uint8_t          morph_kernel;     /* 0=off, 1..7 → kernel size 3..15   */

    /* Step 6: CCL. */
    uint8_t          connectivity;     /* 4 or 8                             */

    /* Step 7: Object filters. */
    uint32_t         min_area;
    uint32_t         max_area;                 /* 0 = no upper limit         */
    uint16_t         aspect_ratio_min_x1000;  /* 0 = disabled               */
    uint16_t         aspect_ratio_max_x1000;  /* 0 = disabled               */
    uint16_t         circularity_min_x1000;   /* 0 = disabled               */
    uint8_t          border_filter_enabled;   /* 1 = reject border blobs    */

    /* ROI — applied before grayscale conversion. */
    uint8_t          roi_enabled;
    uint16_t         roi_x;
    uint16_t         roi_y;
    uint16_t         roi_w;
    uint16_t         roi_h;
} cv_config_t;

/* =========================================================================
 * Result structures
 * ========================================================================= */

typedef struct {
    uint16_t id;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint32_t area;
    uint32_t perimeter;
    uint16_t circularity_x1000;
} cv_box_t;

typedef struct {
    uint32_t object_count;
    uint16_t box_count;
    cv_box_t boxes[CV_MAX_BOXES];

    uint32_t area_sum;
    uint32_t area_min;
    uint32_t area_max;
    uint32_t mean_area;
    uint32_t mean_brightness;

    uint32_t rejected_small;
    uint32_t rejected_large;
    uint32_t rejected_border;
    uint32_t rejected_shape;

    uint32_t fg_pixel_count;   /* foreground pixels after threshold + morph */
    uint32_t raw_comp_count;   /* CCL labels before area / shape filtering  */

    uint32_t processing_time_ms;
} cv_result_t;

/* Internal CCL types — on the stack inside cv_run_rgb565.
 * Application code must not access their fields directly. */
typedef struct {
    uint16_t parent;
    uint8_t  used;
    uint8_t  valid;
    uint32_t area;
    uint16_t min_x;
    uint16_t min_y;
    uint16_t max_x;
    uint16_t max_y;
} cv_component_t;

typedef struct {
    uint16_t x0;
    uint16_t x1;
    uint16_t label;
} cv_run_t;

/* =========================================================================
 * Main pipeline
 *
 * work_bin, tmp_buf : each proc_w * proc_h bytes
 *   proc_w / proc_h = cfg->roi_w/h when ROI is active, else width / height.
 *   Both must be allocated for the MAXIMUM expected proc_w * proc_h
 *   (= APP_CV_MAX_WIDTH * APP_CV_MAX_HEIGHT when ROI can be disabled).
 *
 * bg_buf : proc_w * proc_h bytes, or NULL to skip background subtraction.
 *
 * Returns 0 on success, negative on error.
 * ========================================================================= */
int cv_run_rgb565(const uint16_t    *rgb565,
                  uint16_t           width,
                  uint16_t           height,
                  uint8_t           *work_bin,
                  uint8_t           *tmp_buf,
                  const uint8_t     *bg_buf,
                  const cv_config_t *cfg,
                  cv_result_t       *out);

/* =========================================================================
 * Background reference capture
 *
 * Converts the current RGB565 frame to grayscale and stores it in bg_buf.
 * Call with an empty scene (no objects on the tray / conveyor).
 * bg_buf must be at least proc_w * proc_h bytes.
 * ========================================================================= */
void cv_capture_background(const uint16_t    *rgb565,
                            uint16_t           width,
                            uint16_t           height,
                            uint8_t           *bg_buf,
                            const cv_config_t *cfg);

/* =========================================================================
 * Preset loader
 *
 * Writes filter, threshold and morphology defaults for the given preset.
 * Does NOT touch: enabled, bgsub_enabled, min/max_area, shape-filter
 * thresholds, border_filter_enabled, roi_*, connectivity.
 * Those are always set manually.
 * ========================================================================= */
void cv_apply_preset(cv_config_t *cfg, cv_preset_t preset);

/* =========================================================================
 * cvext_* setters
 *
 * Register the application-owned cv_config_t once with cvext_register(),
 * then call individual setters from the UART command parser.
 * ========================================================================= */
void cvext_register(cv_config_t *cfg);

int  cvext_set_preset     (uint32_t preset);
int  cvext_set_thr_mode   (uint32_t mode);
int  cvext_set_morph_mode (uint32_t mode);
int  cvext_set_filter_mode(uint32_t mode);
void cvext_set_aspect_ratio_range(uint32_t min_x1000, uint32_t max_x1000);
void cvext_set_circularity_min   (uint32_t min_x1000);
int  cvext_set_roi(uint32_t enable, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* INC_CV_ENGINE_H_ */
