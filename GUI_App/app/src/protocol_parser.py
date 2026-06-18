"""
protocol_parser.py — UART protocol parser for the STM32 Edge Vision Reference Dashboard.

This module is intentionally GUI-free. It parses the text protocol emitted by the
STM32 firmware (camera_proto.c) and returns typed dataclasses that the GUI consumes.

Supported STM32 message types (all lines use KEY=VALUE space-separated format):
  Image headers:
    JPG: <size>
    RGB565: <width> <height> <bytes>
    GRAY: <width> <height> <bytes>

  Classical Vision (CV):
    CVCFG:  — complete CV pipeline configuration (sent after CV GET and every
              parameter change). Contains all fields defined in cv_config_t.
    CVSTAT: — CV result summary (sent by camproto_send_cv_result).
    CVBOX:  — per-object bounding box (one line per detected object, up to CV_MAX_BOXES=8).
    CVDONE  — end-of-result marker; GUI commits the pending result on receipt.

  TinyML:
    TMCFG:  — model name, input shape, class count, enabled flag.
    TMINFO: — runtime status, RAM and Flash usage in KB.
    TMRES:  — top-1 inference result (class name, index, confidence, time, uncertain flag).
    TMPROB: — per-class score (one line per class, permille 0..1000).
    TMDONE  — end-of-result marker.

  Log / diagnostics:
    INFO: / WARN: / ERR: / DEBUG: — human-readable firmware log messages.
    STAT: — camera statistics (FPS×10, image size, heap, frame-buffer, latency).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# TinyML constants (must match firmware tinyml_engine.h)
# ---------------------------------------------------------------------------

TINYML_CLASSES: list[str] = ["COUNT_0", "COUNT_1", "COUNT_2", "COUNT_3", "COUNT_4"]
TINYML_INPUT_DESC = "96x96x3"


# ---------------------------------------------------------------------------
# Dataclasses — one per major firmware message type
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class ImageHeader:
    """Parsed image-transfer header emitted before the raw pixel/JPEG bytes."""
    fmt: str          # "JPEG", "RGB565", or "GRAY"
    size: int         # total byte count following this header
    width: int = 0    # pixel columns (RGB565 / GRAY only)
    height: int = 0   # pixel rows   (RGB565 / GRAY only)


@dataclass(slots=True)
class CvBox:
    """
    One bounding box from a CVBOX: line.

    The firmware sends one CVBOX line per accepted object, up to CV_MAX_BOXES=8.
    Circularity is stored as an integer in the range 0..1000 (permille).
    """
    box_id: int = 0
    area: int = 0
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    perimeter: int = 0
    circularity_x1000: int = 0   # 0 = flat line, 1000 = perfect circle


@dataclass(slots=True)
class CvResult:
    """
    Parsed CVSTAT: summary line, extended with CVBOX entries and the
    additional diagnostic fields added in firmware camera_proto.c.

    Fields that are absent in older firmware versions default to 0.
    """
    # Core count / area statistics
    count: int = 0
    mean_area: int = 0
    area_max: int = 0
    area_min: int = 0
    mean_brightness: int = 0
    processing_time_ms: int = 0

    # Rejection statistics
    rejected_small: int = 0
    rejected_large: int = 0
    rejected_border: int = 0
    rejected_shape: int = 0

    # Diagnostic fields
    fg_pixel_count: int = 0
    raw_comp_count: int = 0

    # Box data
    box_count: int = 0
    boxes: list[CvBox] = field(default_factory=list)


@dataclass(slots=True)
class CvConfig:
    """
    Parsed CVCFG: line reflecting the complete cv_config_t on the STM32.

    The firmware emits CVCFG: after CV GET and after every individual CV
    parameter change.  All fields correspond to members of cv_config_t.
    """
    # General
    enabled: int = 1
    preset: int = 0              # 0=CUSTOM, 1=FAST, 2=ROBUST, 3=ACCURATE

    # Step 3 — pre-threshold spatial filter
    filter_mode: int = 0         # 0=OFF, 1=BOX, 2=MEDIAN
    blur_kernel: int = 0         # 0=off, 1..7 → kernel size 3..15

    # Step 4 — threshold
    threshold_mode: int = 0      # 0=MANUAL, 1=OTSU
    threshold: int = 128
    invert: int = 0

    # Step 5 — morphology
    morph_kernel: int = 0        # 0=off, 1..7 → kernel size 3..15
    morph_mode: int = 0          # 0=OFF,1=OPEN,2=CLOSE,3=ERODE,4=DILATE

    # Step 6 — CCL connectivity
    connectivity: int = 8        # 4 or 8

    # Step 7 — object filters
    min_area: int = 20
    max_area: int = 0            # 0 = unlimited
    aspect_ratio_min_x1000: int = 0   # 0 = disabled
    aspect_ratio_max_x1000: int = 0
    circularity_min_x1000: int = 0
    border_filter_enabled: int = 0

    # Step 2 — background subtraction
    bgsub_enabled: int = 0
    bg_captured: int = 0         # 1 = a reference frame has been captured

    # ROI
    roi_enabled: int = 0
    roi_x: int = 0
    roi_y: int = 0
    roi_w: int = 0
    roi_h: int = 0


@dataclass(slots=True)
class TmResult:
    """Aggregated TinyML inference result built from TMCFG/TMINFO/TMRES/TMPROB lines."""
    predicted_name: str = "—"
    predicted_index: int = -1
    confidence: float = 0.0          # normalised 0.0–1.0
    is_uncertain: bool = False
    inference_time_ms: int = 0
    runtime_status: str = "—"
    ram_kb: int = 0
    flash_kb: int = 0
    scores: dict[str, float] = field(default_factory=dict)  # class_name → 0.0–1.0


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

_PAIR_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^\s,]+)")
_JPG_RE    = re.compile(r"^JPG\s*:\s*(\d+)\s*$",                     re.IGNORECASE)
_RGB565_RE = re.compile(r"^RGB565\s*:\s*(\d+)\s+(\d+)\s+(\d+)\s*$", re.IGNORECASE)
_GRAY_RE   = re.compile(r"^GRAY\s*:\s*(\d+)\s+(\d+)\s+(\d+)\s*$",   re.IGNORECASE)


def _pairs(line: str) -> dict[str, str]:
    """Extract KEY=VALUE pairs from a firmware line; keys are upper-cased."""
    return {k.upper(): v.strip() for k, v in _PAIR_RE.findall(line)}


def _to_int(value: object, default: int = 0) -> int:
    """Safely convert a firmware field value to int. Strips 'KB'/'B' suffixes."""
    if value is None:
        return default
    try:
        s = str(value).strip()
        if s.upper().endswith("KB"):
            s = s[:-2]
        elif s.upper().endswith("B"):
            s = s[:-1]
        return int(float(s))
    except (TypeError, ValueError):
        return default


def _norm_class_name(name: str) -> str:
    """Normalise a firmware class name (COUNT_N, uncertain, or raw)."""
    s = (name or "").strip()
    if not s:
        return "—"
    if s.upper() == "UNCERTAIN":
        return "uncertain"
    m = re.fullmatch(r"(?i)COUNT[_-]?(\d+)", s)
    if m:
        return f"COUNT_{m.group(1)}"
    return s


# ---------------------------------------------------------------------------
# Public parse functions
# ---------------------------------------------------------------------------

def try_parse_image_header(line: str) -> ImageHeader | None:
    """
    Attempt to parse a firmware image-transfer header line.

    The firmware emits one of three forms before sending raw pixel data:
      JPG: <size>                   — JPEG frame (size bytes follow)
      RGB565: <w> <h> <size>        — packed RGB565 frame
      GRAY: <w> <h> <size>          — packed 8-bit grayscale frame

    Returns an ImageHeader on success, or None if the line does not match.
    """
    text = line.strip()

    m = _JPG_RE.match(text)
    if m:
        size = _to_int(m.group(1), -1)
        if size > 0:
            return ImageHeader(fmt="JPEG", size=size)
        return None

    m = _RGB565_RE.match(text)
    if m:
        width  = _to_int(m.group(1), -1)
        height = _to_int(m.group(2), -1)
        size   = _to_int(m.group(3), -1)
        if width > 0 and height > 0 and size > 0:
            return ImageHeader(fmt="RGB565", width=width, height=height, size=size)
        return None

    m = _GRAY_RE.match(text)
    if m:
        width  = _to_int(m.group(1), -1)
        height = _to_int(m.group(2), -1)
        size   = _to_int(m.group(3), -1)
        if width > 0 and height > 0 and size > 0:
            return ImageHeader(fmt="GRAY", width=width, height=height, size=size)
        return None

    return None


def parse_cvstat(line: str) -> CvResult:
    """
    Parse a CVSTAT: line emitted by camproto_send_cv_result().

    All diagnostic fields (REJBORDER, REJSHAPE, FGPIX, RAWCOMP) are optional;
    older firmware that does not emit them will leave the corresponding fields at 0.
    """
    kv = _pairs(line)
    return CvResult(
        count               = _to_int(kv.get("COUNT")),
        mean_area           = _to_int(kv.get("MEAN")),
        area_max            = _to_int(kv.get("MAX")),
        area_min            = _to_int(kv.get("MIN")),
        mean_brightness     = _to_int(kv.get("BRIGHT")),
        processing_time_ms  = _to_int(kv.get("TIME")),
        rejected_small      = _to_int(kv.get("REJSMALL")),
        rejected_large      = _to_int(kv.get("REJLARGE")),
        rejected_border     = _to_int(kv.get("REJBORDER")),
        rejected_shape      = _to_int(kv.get("REJSHAPE")),
        fg_pixel_count      = _to_int(kv.get("FGPIX")),
        raw_comp_count      = _to_int(kv.get("RAWCOMP")),
        box_count           = _to_int(kv.get("BOXES")),
    )


def parse_cvbox(line: str) -> CvBox:
    """
    Parse a CVBOX: line emitted by camproto_send_cv_result().

    Each CVBOX line represents one accepted object; the firmware sends up to
    CV_MAX_BOXES (8) such lines after CVSTAT, followed by CVDONE.
    """
    kv = _pairs(line)
    return CvBox(
        box_id             = _to_int(kv.get("ID")),
        area               = _to_int(kv.get("AREA")),
        x                  = _to_int(kv.get("X")),
        y                  = _to_int(kv.get("Y")),
        w                  = _to_int(kv.get("W")),
        h                  = _to_int(kv.get("H")),
        perimeter          = _to_int(kv.get("PERI")),
        circularity_x1000  = _to_int(kv.get("CIRC")),
    )


def parse_cvcfg(line: str) -> CvConfig:
    """
    Parse a CVCFG: line emitted by camproto_send_cv_config().

    All fields correspond directly to members of cv_config_t in cv_engine.h.
    """
    kv = _pairs(line)
    return CvConfig(
        enabled                = _to_int(kv.get("EN"),       1),
        preset                 = _to_int(kv.get("PRESET"),   0),
        filter_mode            = _to_int(kv.get("FILTER"),   0),
        blur_kernel            = _to_int(kv.get("BLUR"),     0),
        threshold_mode         = _to_int(kv.get("THRMODE"),  0),
        threshold              = _to_int(kv.get("THR"),    128),
        invert                 = _to_int(kv.get("INV"),      0),
        morph_kernel           = _to_int(kv.get("MORPH"),    0),
        morph_mode             = _to_int(kv.get("MORPHMODE"),0),
        connectivity           = _to_int(kv.get("CON"),      8),
        min_area               = _to_int(kv.get("MIN"),     20),
        max_area               = _to_int(kv.get("MAX"),      0),
        aspect_ratio_min_x1000 = _to_int(kv.get("ARMIN"),   0),
        aspect_ratio_max_x1000 = _to_int(kv.get("ARMAX"),   0),
        circularity_min_x1000  = _to_int(kv.get("CIRCMIN"), 0),
        border_filter_enabled  = _to_int(kv.get("BORDFILT"),0),
        bgsub_enabled          = _to_int(kv.get("BGSUB"),   0),
        bg_captured            = _to_int(kv.get("BGCAP"),   0),
        roi_enabled            = _to_int(kv.get("ROIEN"),   0),
        roi_x                  = _to_int(kv.get("ROIX"),    0),
        roi_y                  = _to_int(kv.get("ROIY"),    0),
        roi_w                  = _to_int(kv.get("ROIW"),    0),
        roi_h                  = _to_int(kv.get("ROIH"),    0),
    )


def parse_tmcfg(line: str) -> tuple[str, str]:
    """
    Parse a TMCFG: line.

    Returns (model_name, input_descriptor).  The input descriptor is formatted
    as "WxHxC" (e.g. "96x96x3"), matching TINYML_INPUT_DESC.
    """
    kv         = _pairs(line)
    model      = kv.get("MODEL", "—")
    input_desc = kv.get("INPUT", "—")
    return model, input_desc


def parse_tminfo(line: str) -> tuple[str, int, int]:
    """
    Parse a TMINFO: line.

    Returns (runtime_status, ram_kb, flash_kb).
    """
    kv       = _pairs(line)
    status   = kv.get("STATUS",  "—")
    ram_kb   = _to_int(kv.get("RAM"))
    flash_kb = _to_int(kv.get("FLASH"))
    return status, ram_kb, flash_kb


def parse_tmres(line: str) -> tuple[str, int, float, int, bool]:
    """
    Parse a TMRES: line (top-1 inference result).

    Returns (class_name, class_index, confidence_0_to_1, time_ms, is_uncertain).

    The firmware sends CONF as a permille value (0..1000).  norm01() in
    app_helpers.py normalises it to 0.0–1.0 when the caller stores the result.
    """
    kv           = _pairs(line)
    is_uncertain = _to_int(kv.get("UNCERTAIN"), 0) != 0
    name         = _norm_class_name(kv.get("CLASS", "—"))
    idx          = _to_int(kv.get("IDX"), -1)
    conf         = float(_to_int(kv.get("CONF"), 0))
    t_ms         = _to_int(kv.get("TIME"), 0)
    return name, idx, conf, t_ms, is_uncertain


def parse_tmprob(line: str) -> tuple[int, str, float]:
    """
    Parse a TMPROB: line (per-class score).

    Returns (class_index, class_name, score_permille).  Scores are in the range
    0–1000 (permille); the GUI normalises them to 0.0–1.0.
    """
    kv    = _pairs(line)
    idx   = _to_int(kv.get("IDX"), -1)
    name  = _norm_class_name(kv.get("NAME", f"COUNT_{idx}" if idx >= 0 else "—"))
    score = float(_to_int(kv.get("SCORE"), 0))
    return idx, name, score
