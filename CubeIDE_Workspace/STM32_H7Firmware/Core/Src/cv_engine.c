/*
 * cv_engine.c
 *
 * Classical vision pipeline for STM32H7 Edge Vision — object counting.
 *
 * Pipeline stages (cv_run_rgb565):
 *   1. RGB565 → grayscale (BT.601, per-pixel, no heap)
 *   2. Background subtraction (optional, bg_buf in RAM_D2)
 *   3. Pre-threshold spatial filter (box or median)
 *   4. Threshold (manual or Otsu)
 *   5. Binary morphology (open / close / erode / dilate)
 *   6. Run-length CCL + union-find (stack arrays only, no heap)
 *   7. Object filter: area / aspect-ratio / circularity / border
 *
 * Buffer contract (caller must allocate):
 *   work_bin (cvBinBuffer) — proc_w × proc_h bytes, RAM_D1 (.dcmi_buf)
 *     Holds grayscale output (Stage 1), filtered gray (Stage 3), and the
 *     final binary result after morphology (Stage 5).  Read by CCL (Stage 6).
 *
 *   tmp_buf  (cvTmpBuffer) — proc_w × proc_h bytes, RAM_D2 (.cvtmb_buf)
 *     Holds grayscale staging (Stage 1→2), binary staging (Stage 4), and
 *     morphology intermediate (Stage 5).
 *     Cannot be smaller than proc_w × proc_h because Stages 1 and 4 write
 *     the full image region before morphology reads it.
 *
 *   bg_buf   (cvBgBuffer)  — proc_w × proc_h bytes, RAM_D2 (.cvbg_buf)
 *     Grayscale reference frame for background subtraction.
 *     Pass NULL to disable background subtraction.
 *
 *   proc_w / proc_h = ROI dimensions when ROI is active, else full capture size.
 *   At 480×272 without ROI: each buffer = 130 560 B.
 */

#include "cv_engine.h"

#include <string.h>
#include <stdint.h>
#include <limits.h>

/* Pointer to the application-owned cv_config_t registered via cvext_register(). */
static cv_config_t *g_cfg = NULL;

/* =========================================================================
 * Utility
 * ========================================================================= */

/* Map kernel slider (0–7) to odd pixel kernel size.  Slider 0 → disabled (0). */
static uint8_t cv_kernel_size(uint8_t slider)
{
    return (slider == 0u) ? 0u : (uint8_t)(2u * slider + 1u);
}

static void cv_reset_result(cv_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->area_min = UINT32_MAX;
}

/* =========================================================================
 * Stage 1 — Grayscale conversion (BT.601 luma, RGB565 source)
 * ========================================================================= */

static uint8_t cv_rgb565_to_gray(uint16_t p)
{
    uint32_t r = ((uint32_t)((p >> 11) & 0x1Fu) * 255u) / 31u;
    uint32_t g = ((uint32_t)((p >>  5) & 0x3Fu) * 255u) / 63u;
    uint32_t b = ((uint32_t)( p        & 0x1Fu) * 255u) / 31u;
    return (uint8_t)((30u * r + 59u * g + 11u * b) / 100u);
}

/* =========================================================================
 * Stage 2 — Background subtraction (optional)
 * ========================================================================= */

/* Compute absolute difference between current gray and reference frame. */
static void cv_bgsub_apply(const uint8_t *gray,
                            const uint8_t *bg,
                            uint8_t       *dst,
                            uint32_t       n)
{
    for (uint32_t i = 0u; i < n; ++i) {
        uint8_t a = gray[i], b = bg[i];
        dst[i] = (a >= b) ? (uint8_t)(a - b) : (uint8_t)(b - a);
    }
}

/* =========================================================================
 * Stage 3 — Pre-threshold spatial filter
 * ========================================================================= */

/* Box blur (brute-force, border-clamped).  Both src and dst are proc_w × proc_h. */
static void cv_box_blur(const uint8_t *src, uint8_t *dst,
                        uint16_t w, uint16_t h, uint8_t ksize)
{
    if (ksize < 3u) { memcpy(dst, src, (uint32_t)w * h); return; }
    int r = (int)ksize / 2;
    for (uint16_t y = 0u; y < h; ++y) {
        for (uint16_t x = 0u; x < w; ++x) {
            uint32_t sum = 0u, cnt = 0u;
            for (int ky = -r; ky <= r; ++ky) {
                int yy = (int)y + ky;
                if (yy < 0) { yy = 0; } else if (yy >= (int)h) { yy = (int)h - 1; }
                for (int kx = -r; kx <= r; ++kx) {
                    int xx = (int)x + kx;
                    if (xx < 0) { xx = 0; } else if (xx >= (int)w) { xx = (int)w - 1; }
                    sum += src[(uint32_t)yy * w + (uint32_t)xx];
                    ++cnt;
                }
            }
            dst[(uint32_t)y * w + x] = (uint8_t)(sum / cnt);
        }
    }
}

/* Median filter (insertion sort, max 5×5 kernel).
 * Both src and dst are proc_w × proc_h. */
static void cv_median_filter(const uint8_t *src, uint8_t *dst,
                              uint16_t w, uint16_t h, uint8_t ksize)
{
    if (ksize < 3u) { memcpy(dst, src, (uint32_t)w * h); return; }
    if (ksize > 5u) { ksize = 5u; }
    int r = (int)ksize / 2;
    for (uint16_t y = 0u; y < h; ++y) {
        for (uint16_t x = 0u; x < w; ++x) {
            uint8_t window[25];
            uint8_t count = 0u;
            for (int ky = -r; ky <= r; ++ky) {
                int yy = (int)y + ky;
                if (yy < 0) { yy = 0; } else if (yy >= (int)h) { yy = (int)h - 1; }
                for (int kx = -r; kx <= r; ++kx) {
                    int xx = (int)x + kx;
                    if (xx < 0) { xx = 0; } else if (xx >= (int)w) { xx = (int)w - 1; }
                    window[count++] = src[(uint32_t)yy * w + (uint32_t)xx];
                }
            }
            /* Insertion sort to find the median. */
            for (uint8_t i = 1u; i < count; ++i) {
                uint8_t key = window[i];
                int     j   = (int)i - 1;
                while ((j >= 0) && (window[j] > key)) { window[j + 1] = window[j]; --j; }
                window[j + 1] = key;
            }
            dst[(uint32_t)y * w + x] = window[count / 2u];
        }
    }
}

/* =========================================================================
 * Stage 4 — Thresholding
 * ========================================================================= */

/* Otsu's method — histogram on stack (1 KB), no heap required. */
static uint8_t cv_otsu_threshold(const uint8_t *gray, uint32_t n)
{
    uint32_t hist[256];
    uint64_t sum_total  = 0u, sum_back = 0u;
    uint32_t weight_back = 0u, best_thr = 128u;
    uint64_t best_score  = 0u;

    memset(hist, 0, sizeof(hist));
    for (uint32_t i = 0u; i < n; ++i) { ++hist[gray[i]]; sum_total += gray[i]; }

    for (uint32_t t = 0u; t < 256u; ++t) {
        weight_back += hist[t];
        if (weight_back == 0u) { continue; }
        uint32_t weight_fore = n - weight_back;
        if (weight_fore == 0u) { break; }
        sum_back    += (uint64_t)t * hist[t];
        uint64_t diff  = sum_back * n - sum_total * weight_back;
        uint64_t score = (diff * diff) / ((uint64_t)weight_back * weight_fore);
        if (score > best_score) { best_score = score; best_thr = t; }
    }
    return (uint8_t)best_thr;
}

/* =========================================================================
 * Stage 5 — Morphology
 *
 * bin : work_bin, proc_w × proc_h — input and final output
 * tmp : tmp_buf,  proc_w × proc_h — intermediate scratch
 *
 * For OPEN and CLOSE the result stays in bin after two ping-pong passes
 * (erode/dilate or dilate/erode) — no extra memcpy.
 * For single ERODE/DILATE the result is written to tmp then copied to bin.
 * ========================================================================= */

/* Erosion: out-of-bounds pixels are treated as background (0). */
static void cv_erode(const uint8_t *src, uint8_t *dst,
                     uint16_t w, uint16_t h, uint8_t ksize)
{
    if (ksize < 3u) { memcpy(dst, src, (uint32_t)w * h); return; }
    int r = (int)ksize / 2;
    for (uint16_t y = 0u; y < h; ++y) {
        for (uint16_t x = 0u; x < w; ++x) {
            uint8_t keep = 255u;
            for (int ky = -r; ky <= r && keep; ++ky) {
                int yy = (int)y + ky;
                if (yy < 0 || yy >= (int)h) { keep = 0u; break; }
                for (int kx = -r; kx <= r; ++kx) {
                    int xx = (int)x + kx;
                    if (xx < 0 || xx >= (int)w ||
                        src[(uint32_t)yy * w + (uint32_t)xx] == 0u) {
                        keep = 0u; break;
                    }
                }
            }
            dst[(uint32_t)y * w + x] = keep;
        }
    }
}

/* Dilation: out-of-bounds pixels are treated as background (0). */
static void cv_dilate(const uint8_t *src, uint8_t *dst,
                      uint16_t w, uint16_t h, uint8_t ksize)
{
    if (ksize < 3u) { memcpy(dst, src, (uint32_t)w * h); return; }
    int r = (int)ksize / 2;
    for (uint16_t y = 0u; y < h; ++y) {
        for (uint16_t x = 0u; x < w; ++x) {
            uint8_t hit = 0u;
            for (int ky = -r; ky <= r && !hit; ++ky) {
                int yy = (int)y + ky;
                if (yy < 0 || yy >= (int)h) { continue; }
                for (int kx = -r; kx <= r; ++kx) {
                    int xx = (int)x + kx;
                    if (xx < 0 || xx >= (int)w) { continue; }
                    if (src[(uint32_t)yy * w + (uint32_t)xx] != 0u) { hit = 255u; break; }
                }
            }
            dst[(uint32_t)y * w + x] = hit;
        }
    }
}

/* Dispatch morphology operation.  Result always ends in bin. */
static void cv_apply_morphology(uint8_t *bin, uint8_t *tmp,
                                uint16_t w, uint16_t h,
                                cv_morph_mode_t mode, uint8_t ksize)
{
    if ((mode == CV_MORPH_OFF) || (ksize < 3u)) { return; }
    uint32_t n = (uint32_t)w * h;
    switch (mode) {
    case CV_MORPH_OPEN:
        /* erode(bin→tmp), dilate(tmp→bin): result in bin, no extra copy. */
        cv_erode (bin, tmp, w, h, ksize);
        cv_dilate(tmp, bin, w, h, ksize);
        break;
    case CV_MORPH_CLOSE:
        /* dilate(bin→tmp), erode(tmp→bin): result in bin, no extra copy. */
        cv_dilate(bin, tmp, w, h, ksize);
        cv_erode (tmp, bin, w, h, ksize);
        break;
    case CV_MORPH_ERODE:
        cv_erode(bin, tmp, w, h, ksize);
        memcpy(bin, tmp, n);
        break;
    case CV_MORPH_DILATE:
        cv_dilate(bin, tmp, w, h, ksize);
        memcpy(bin, tmp, n);
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Stage 6/7 helpers — CCL (union-find), perimeter, circularity, filters
 * ========================================================================= */

/* Path-halving union-find: find root of label. */
static uint16_t cv_find_root(cv_component_t *comp, uint16_t label)
{
    while (comp[label].parent != label) {
        comp[label].parent = comp[comp[label].parent].parent;
        label = comp[label].parent;
    }
    return label;
}

/* Union two components by smaller root index. */
static void cv_union_root(cv_component_t *comp, uint16_t a, uint16_t b)
{
    uint16_t ra = cv_find_root(comp, a);
    uint16_t rb = cv_find_root(comp, b);
    if (ra == rb) { return; }
    if (ra < rb) { comp[rb].parent = ra; } else { comp[ra].parent = rb; }
}

/* Initialise a new CCL component entry. */
static void cv_init_component(cv_component_t *c, uint16_t label)
{
    c->parent = label;
    c->used   = 1u;
    c->valid  = 1u;
    c->area   = 0u;
    c->min_x  = 0xFFFFu;
    c->min_y  = 0xFFFFu;
    c->max_x  = 0u;
    c->max_y  = 0u;
}

/* 4-connected perimeter estimate (count foreground pixels with background neighbours). */
static uint32_t cv_calc_perimeter(const uint8_t *bin,
                                  uint16_t w, uint16_t h,
                                  uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1)
{
    uint32_t p = 0u;
    for (uint16_t y = y0; y <= y1; ++y) {
        for (uint16_t x = x0; x <= x1; ++x) {
            uint32_t idx = (uint32_t)y * w + x;
            if (bin[idx] == 0u) { continue; }
            if ((x == 0u)              || (bin[idx - 1u] == 0u)) { ++p; }
            if ((x >= (uint16_t)(w-1)) || (bin[idx + 1u] == 0u)) { ++p; }
            if ((y == 0u)              || (bin[idx - w]  == 0u)) { ++p; }
            if ((y >= (uint16_t)(h-1)) || (bin[idx + w]  == 0u)) { ++p; }
        }
    }
    return p;
}

/* Circularity = 4π·area / perimeter², scaled to [0, 1000]. */
static uint16_t cv_circularity(uint32_t area, uint32_t perimeter)
{
    if ((area == 0u) || (perimeter == 0u)) { return 0u; }
    uint64_t p2   = (uint64_t)perimeter * perimeter;
    uint64_t circ = ((uint64_t)12566u * area + p2 / 2u) / p2;
    return (uint16_t)((circ > 1000u) ? 1000u : circ);
}

/* Reject large blobs that touch and span most of the frame edge.
 * Heuristic thresholds: width/height ≥ 85% of frame, or area ≥ 35% of frame. */
static uint8_t cv_is_border_blob(uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1,
                                  uint32_t area, uint16_t pw, uint16_t ph)
{
    if ((pw == 0u) || (ph == 0u)) { return 0u; }

    uint32_t bw = (uint32_t)x1 - x0 + 1u;
    uint32_t bh = (uint32_t)y1 - y0 + 1u;
    uint32_t fa = (uint32_t)pw * ph;

    if ((bw >= pw) || (bh >= ph)) { return 1u; }

    uint8_t border = (uint8_t)((x0 == 0u) || (y0 == 0u) ||
                               (x1 >= (uint16_t)(pw - 1u)) ||
                               (y1 >= (uint16_t)(ph - 1u)));
    if (!border) { return 0u; }

    return (uint8_t)((bw * 100u >= (uint32_t)pw * 85u) ||
                     (bh * 100u >= (uint32_t)ph * 85u) ||
                     (area * 100u >= fa * 35u));
}

/* =========================================================================
 * Preset loader
 * ========================================================================= */

void cv_apply_preset(cv_config_t *cfg, cv_preset_t preset)
{
    if (cfg == NULL) { return; }
    cfg->preset       = preset;
    cfg->connectivity = 8u;
    switch (preset) {
    case CV_PRESET_FAST:
        cfg->filter_mode  = CV_FILTER_OFF;    cfg->blur_kernel  = 0u;
        cfg->thr_mode     = CV_THR_OTSU;      cfg->threshold    = 128u;
        cfg->invert       = 0u;
        cfg->morph_mode   = CV_MORPH_OFF;     cfg->morph_kernel = 0u;
        break;
    case CV_PRESET_ROBUST:
        cfg->filter_mode  = CV_FILTER_BOX;    cfg->blur_kernel  = 1u;  /* 3×3 */
        cfg->thr_mode     = CV_THR_OTSU;      cfg->threshold    = 128u;
        cfg->invert       = 0u;
        cfg->morph_mode   = CV_MORPH_OPEN;    cfg->morph_kernel = 1u;  /* 3×3 */
        break;
    case CV_PRESET_ACCURATE:
        cfg->filter_mode  = CV_FILTER_MEDIAN; cfg->blur_kernel  = 2u;  /* 5×5 */
        cfg->thr_mode     = CV_THR_OTSU;      cfg->threshold    = 128u;
        cfg->invert       = 0u;
        cfg->morph_mode   = CV_MORPH_OPEN;    cfg->morph_kernel = 2u;  /* 5×5 */
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Background capture
 * ========================================================================= */

/* Convert the current RGB565 frame to grayscale and store in bg_buf.
 * Call with an empty scene (no objects) to establish the background reference. */
void cv_capture_background(const uint16_t *rgb565,
                            uint16_t width, uint16_t height,
                            uint8_t *bg_buf, const cv_config_t *cfg)
{
    uint16_t proc_w = width, proc_h = height;
    uint16_t roi_x0 = 0u, roi_y0 = 0u;

    if ((rgb565 == NULL) || (bg_buf == NULL) || (cfg == NULL)) { return; }

    if (cfg->roi_enabled && (cfg->roi_w > 0u) && (cfg->roi_h > 0u)) {
        roi_x0 = cfg->roi_x; roi_y0 = cfg->roi_y;
        proc_w = cfg->roi_w; proc_h = cfg->roi_h;
        if ((uint32_t)roi_x0 + proc_w > width)  { proc_w = (uint16_t)(width  - roi_x0); }
        if ((uint32_t)roi_y0 + proc_h > height) { proc_h = (uint16_t)(height - roi_y0); }
    }

    for (uint16_t y = 0u; y < proc_h; ++y) {
        for (uint16_t x = 0u; x < proc_w; ++x) {
            uint32_t si = (uint32_t)(roi_y0 + y) * width + (roi_x0 + x);
            bg_buf[(uint32_t)y * proc_w + x] = cv_rgb565_to_gray(rgb565[si]);
        }
    }
}

/* =========================================================================
 * Main pipeline
 * ========================================================================= */

int cv_run_rgb565(const uint16_t    *rgb565,
                  uint16_t           width,
                  uint16_t           height,
                  uint8_t           *work_bin,
                  uint8_t           *tmp_buf,
                  const uint8_t     *bg_buf,
                  const cv_config_t *cfg,
                  cv_result_t       *out)
{
    cv_run_t       prev_runs[CV_MAX_RUNS_PER_ROW];
    cv_run_t       curr_runs[CV_MAX_RUNS_PER_ROW];
    cv_component_t comp[CV_MAX_COMPONENTS + 1u];
    uint16_t       prev_count = 0u;
    uint16_t       next_label = 1u;
    uint16_t       proc_w = width, proc_h = height;
    uint16_t       roi_x0 = 0u, roi_y0 = 0u;

    if (!rgb565 || !work_bin || !tmp_buf || !cfg || !out) { return -1; }
    if ((width == 0u) || (height == 0u)) { return -2; }
    if ((cfg->connectivity != 4u) && (cfg->connectivity != 8u)) { return -3; }

    cv_reset_result(out);
    memset(comp, 0, sizeof(comp));

    /* ── ROI ──────────────────────────────────────────────────────────────
     * Shrink proc_w / proc_h to the ROI dimensions.  All subsequent stages
     * operate on the proc_w × proc_h region only.  work_bin and tmp_buf must
     * be at least proc_w * proc_h bytes — the caller allocates for the maximum
     * expected proc_w * proc_h (= full frame when ROI can be disabled). */
    if (cfg->roi_enabled && (cfg->roi_w > 0u) && (cfg->roi_h > 0u)) {
        if ((cfg->roi_x >= width) || (cfg->roi_y >= height)) { return -4; }
        roi_x0 = cfg->roi_x; roi_y0 = cfg->roi_y;
        proc_w = cfg->roi_w; proc_h = cfg->roi_h;
        if ((uint32_t)roi_x0 + proc_w > width)  { proc_w = (uint16_t)(width  - roi_x0); }
        if ((uint32_t)roi_y0 + proc_h > height) { proc_h = (uint16_t)(height - roi_y0); }
        if ((proc_w == 0u) || (proc_h == 0u)) { return -5; }
    }

    const uint32_t n       = (uint32_t)proc_w * proc_h;
    const uint8_t  blur_k  = cv_kernel_size(cfg->blur_kernel);
    const uint8_t  morph_k = cv_kernel_size(cfg->morph_kernel);
    uint64_t       bright_sum = 0u;

    /* ── Stage 1: RGB565 → grayscale → tmp_buf ────────────────────────────
     * Writes proc_w * proc_h bytes into tmp_buf. */
    for (uint16_t y = 0u; y < proc_h; ++y) {
        for (uint16_t x = 0u; x < proc_w; ++x) {
            uint32_t si = (uint32_t)(roi_y0 + y) * width + (roi_x0 + x);
            uint32_t di = (uint32_t)y * proc_w + x;
            uint8_t  g  = cv_rgb565_to_gray(rgb565[si]);
            bright_sum += g;
            tmp_buf[di] = g;
        }
    }
    out->mean_brightness = (n > 0u) ? (uint32_t)(bright_sum / n) : 0u;

    /* ── Stage 2: Background subtraction (optional) ──────────────────────
     * tmp_buf (current gray) − bg_buf (reference gray) → work_bin (diff).
     * Copy result back to tmp_buf so Stage 3 still reads from tmp_buf. */
    if (cfg->bgsub_enabled && (bg_buf != NULL)) {
        cv_bgsub_apply(tmp_buf, bg_buf, work_bin, n);
        memcpy(tmp_buf, work_bin, n);
    }

    /* ── Stage 3: Pre-threshold spatial filter ───────────────────────────
     * tmp_buf (gray) → work_bin (filtered gray). */
    if ((cfg->filter_mode == CV_FILTER_MEDIAN) && (blur_k >= 3u)) {
        cv_median_filter(tmp_buf, work_bin, proc_w, proc_h, blur_k);
    } else if ((cfg->filter_mode == CV_FILTER_BOX) && (blur_k >= 3u)) {
        cv_box_blur(tmp_buf, work_bin, proc_w, proc_h, blur_k);
    } else {
        memcpy(work_bin, tmp_buf, n);
    }

    /* ── Stage 4: Threshold ───────────────────────────────────────────────
     * work_bin (filtered gray) → tmp_buf (binary, 0 or 255). */
    {
        uint8_t thr = cfg->threshold;
        if (cfg->thr_mode == CV_THR_OTSU) {
            thr = cv_otsu_threshold(work_bin, n);
        }
        const uint8_t inv = cfg->invert;
        for (uint32_t i = 0u; i < n; ++i) {
            uint8_t v = (work_bin[i] >= thr) ? 255u : 0u;
            tmp_buf[i] = inv ? (uint8_t)(255u - v) : v;
        }
    }

    /* ── Stage 5: Morphology ──────────────────────────────────────────────
     * Entry: tmp_buf holds the binary image (proc_w * proc_h bytes).
     * Copy to work_bin so morphology has separate src and dst.
     * Exit:  work_bin holds the final binary frame ready for CCL. */
    memcpy(work_bin, tmp_buf, n);
    cv_apply_morphology(work_bin, tmp_buf, proc_w, proc_h,
                        cfg->morph_mode, morph_k);

    /* Count foreground pixels for diagnostic output. */
    {
        uint32_t fg = 0u;
        for (uint32_t i = 0u; i < n; ++i) {
            if (work_bin[i] != 0u) { ++fg; }
        }
        out->fg_pixel_count = fg;
    }

    /* ── Stage 6: Run-length CCL ──────────────────────────────────────────
     * Reads work_bin[y * proc_w + x].
     * Encodes foreground runs, assigns labels, merges overlapping runs using
     * union-find, and accumulates per-component bounding boxes. */
    for (uint16_t y = 0u; y < proc_h; ++y) {
        uint32_t row       = (uint32_t)y * proc_w;
        uint16_t curr_count = 0u;
        uint16_t x          = 0u;

        /* Extract foreground runs on this row. */
        while (x < proc_w) {
            while ((x < proc_w) && (work_bin[row + x] == 0u)) { ++x; }
            if (x >= proc_w) { break; }
            {
                uint16_t x0 = x;
                while ((x < proc_w) && (work_bin[row + x] != 0u)) { ++x; }
                if (curr_count < CV_MAX_RUNS_PER_ROW) {
                    curr_runs[curr_count].x0    = x0;
                    curr_runs[curr_count].x1    = (uint16_t)(x - 1u);
                    curr_runs[curr_count].label = 0u;
                    ++curr_count;
                }
            }
        }

        /* Match current runs to previous-row runs and assign labels. */
        for (uint16_t i = 0u; i < curr_count; ++i) {
            uint16_t assigned = 0u;
            for (uint16_t j = 0u; j < prev_count; ++j) {
                uint16_t px0 = prev_runs[j].x0;
                uint16_t px1 = prev_runs[j].x1;
                if (cfg->connectivity == 8u) {
                    if (px0 > 0u) { px0--; }
                    if (px1 < (uint16_t)(proc_w - 1u)) { px1++; }
                }
                if ((curr_runs[i].x1 >= px0) && (curr_runs[i].x0 <= px1)) {
                    if (assigned == 0u) {
                        assigned = prev_runs[j].label;
                    } else {
                        cv_union_root(comp, assigned, prev_runs[j].label);
                    }
                }
            }
            if (assigned == 0u) {
                if (next_label > CV_MAX_COMPONENTS) { continue; }
                assigned = next_label;
                cv_init_component(&comp[assigned], assigned);
                ++next_label;
            }
            curr_runs[i].label = assigned;

            /* Update bounding box and area of the root component. */
            {
                uint16_t root    = cv_find_root(comp, assigned);
                uint16_t run_len = (uint16_t)(curr_runs[i].x1 - curr_runs[i].x0 + 1u);
                comp[root].area += run_len;
                if (curr_runs[i].x0 < comp[root].min_x) { comp[root].min_x = curr_runs[i].x0; }
                if (curr_runs[i].x1 > comp[root].max_x) { comp[root].max_x = curr_runs[i].x1; }
                if (y < comp[root].min_y) { comp[root].min_y = y; }
                if (y > comp[root].max_y) { comp[root].max_y = y; }
            }
        }

        memcpy(prev_runs, curr_runs, (size_t)curr_count * sizeof(cv_run_t));
        prev_count = curr_count;
    }

    /* Merge non-root components into their roots. */
    for (uint16_t i = 1u; i < next_label; ++i) {
        if (!comp[i].used) { continue; }
        uint16_t r = cv_find_root(comp, i);
        if (r == i) { continue; }
        comp[r].area += comp[i].area;
        if (comp[i].min_x < comp[r].min_x) { comp[r].min_x = comp[i].min_x; }
        if (comp[i].min_y < comp[r].min_y) { comp[r].min_y = comp[i].min_y; }
        if (comp[i].max_x > comp[r].max_x) { comp[r].max_x = comp[i].max_x; }
        if (comp[i].max_y > comp[r].max_y) { comp[r].max_y = comp[i].max_y; }
        comp[i].valid = 0u;
    }

    /* Count raw (unfiltered) components for diagnostic output. */
    {
        uint32_t raw = 0u;
        for (uint16_t i = 1u; i < next_label; ++i) {
            if (comp[i].used && comp[i].valid) { ++raw; }
        }
        out->raw_comp_count = raw;
    }

    /* ── Stage 7: Object filtering and result extraction ──────────────────*/
    for (uint16_t i = 1u; i < next_label; ++i) {
        if (!comp[i].used || !comp[i].valid) { continue; }

        uint32_t area = comp[i].area;
        uint16_t bw   = (uint16_t)(comp[i].max_x - comp[i].min_x + 1u);
        uint16_t bh   = (uint16_t)(comp[i].max_y - comp[i].min_y + 1u);

        if (area < cfg->min_area) { out->rejected_small++; continue; }
        if ((cfg->max_area > 0u) && (area > cfg->max_area)) {
            out->rejected_large++; continue;
        }
        if (cfg->border_filter_enabled &&
            cv_is_border_blob(comp[i].min_x, comp[i].min_y,
                              comp[i].max_x, comp[i].max_y,
                              area, proc_w, proc_h)) {
            out->rejected_border++; continue;
        }

        uint32_t peri = cv_calc_perimeter(work_bin, proc_w, proc_h,
                                          comp[i].min_x, comp[i].min_y,
                                          comp[i].max_x, comp[i].max_y);
        uint16_t circ = cv_circularity(area, peri);
        uint32_t ar   = ((uint32_t)bw * 1000u) / ((bh == 0u) ? 1u : bh);

        if ((cfg->aspect_ratio_min_x1000 > 0u) && (ar < cfg->aspect_ratio_min_x1000)) {
            out->rejected_shape++; continue;
        }
        if ((cfg->aspect_ratio_max_x1000 > 0u) && (ar > cfg->aspect_ratio_max_x1000)) {
            out->rejected_shape++; continue;
        }
        if ((cfg->circularity_min_x1000 > 0u) && (circ < cfg->circularity_min_x1000)) {
            out->rejected_shape++; continue;
        }

        out->object_count++;
        out->area_sum += area;
        if (area < out->area_min) { out->area_min = area; }
        if (area > out->area_max) { out->area_max = area; }
        out->mean_area = out->area_sum / out->object_count;

        if (out->box_count < CV_MAX_BOXES) {
            cv_box_t *b          = &out->boxes[out->box_count++];
            b->id                = (uint16_t)out->box_count;
            b->x                 = (uint16_t)(roi_x0 + comp[i].min_x);
            b->y                 = (uint16_t)(roi_y0 + comp[i].min_y);
            b->w                 = bw;
            b->h                 = bh;
            b->area              = area;
            b->perimeter         = peri;
            b->circularity_x1000 = circ;
        }
    }

    if (out->object_count == 0u) { out->area_min = 0u; }
    return 0;
}

/* =========================================================================
 * cvext_* setters
 *
 * Register the application-owned cv_config_t once via cvext_register(),
 * then call individual setters from the UART command parser.
 * ========================================================================= */

void cvext_register(cv_config_t *cfg) { g_cfg = cfg; }

int cvext_set_preset(uint32_t p)
{
    if ((g_cfg == NULL) || (p > (uint32_t)CV_PRESET_ACCURATE)) { return -1; }
    cv_apply_preset(g_cfg, (cv_preset_t)p);
    return 0;
}

int cvext_set_thr_mode(uint32_t m)
{
    if ((g_cfg == NULL) || (m > (uint32_t)CV_THR_OTSU)) { return -1; }
    g_cfg->thr_mode = (cv_thr_mode_t)m;
    return 0;
}

int cvext_set_morph_mode(uint32_t m)
{
    if ((g_cfg == NULL) || (m > (uint32_t)CV_MORPH_DILATE)) { return -1; }
    g_cfg->morph_mode = (cv_morph_mode_t)m;
    return 0;
}

int cvext_set_filter_mode(uint32_t m)
{
    if ((g_cfg == NULL) || (m > (uint32_t)CV_FILTER_MEDIAN)) { return -1; }
    g_cfg->filter_mode = (cv_filter_mode_t)m;
    return 0;
}

void cvext_set_aspect_ratio_range(uint32_t min_x1000, uint32_t max_x1000)
{
    if (g_cfg == NULL) { return; }
    g_cfg->aspect_ratio_min_x1000 = (min_x1000 > 65535u) ? 65535u : (uint16_t)min_x1000;
    g_cfg->aspect_ratio_max_x1000 = (max_x1000 > 65535u) ? 65535u : (uint16_t)max_x1000;
}

void cvext_set_circularity_min(uint32_t min_x1000)
{
    if (g_cfg == NULL) { return; }
    g_cfg->circularity_min_x1000 = (min_x1000 > 1000u) ? 1000u : (uint16_t)min_x1000;
}

int cvext_set_roi(uint32_t enable, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (g_cfg == NULL) { return -1; }
    g_cfg->roi_enabled = enable ? 1u : 0u;
    g_cfg->roi_x = (x > 65535u) ? 65535u : (uint16_t)x;
    g_cfg->roi_y = (y > 65535u) ? 65535u : (uint16_t)y;
    g_cfg->roi_w = (w > 65535u) ? 65535u : (uint16_t)w;
    g_cfg->roi_h = (h > 65535u) ? 65535u : (uint16_t)h;
    if (g_cfg->roi_enabled && ((g_cfg->roi_w == 0u) || (g_cfg->roi_h == 0u))) {
        return -1;
    }
    return 0;
}
