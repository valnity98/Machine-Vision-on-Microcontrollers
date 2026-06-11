"""
app_constants.py — Application-wide constants, column definitions, and stylesheet.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Firmware-valid resolution sets
# ---------------------------------------------------------------------------

JPEG_RESOLUTIONS: list[str] = [
    "QQVGA", "QVGA", "WQVGA", "VGA", "SVGA", "XGA", "SXGA",
]

RGB_RESOLUTIONS: list[str] = [
    "QQVGA", "QVGA", "WQVGA", "VGA",
]

# ---------------------------------------------------------------------------
# Benchmark table column definitions
# ---------------------------------------------------------------------------

BENCH_COLS: list[tuple[str, str]] = [
    ("time",       "Time"),
    ("frame_fmt",  "Frame"),
    ("cv_count",   "CV Count"),
    ("cv_ms",      "CV ms"),
    ("ml_class",   "ML Class"),
    ("ml_ms",      "ML ms"),
    ("gt_count",   "GT Count"),
    ("cv_vs_gt",   "CV↔GT"),
    ("ml_vs_gt",   "ML↔GT"),
]
BENCH_KEYS   = [c[0] for c in BENCH_COLS]
BENCH_LABELS = [c[1] for c in BENCH_COLS]

BOX_LABELS = ["ID", "Area [px²]", "X", "Y", "W", "H", "Perimeter", "Circularity"]

LOG_MAX_LINES          = 1200
TM_UNCERTAIN_THRESHOLD = 0.40
FRAME_TIMEOUT_MS       = 10_000
DEFAULT_BAUDRATE       = "2000000"
DEFAULT_BAUDRATE_INT   = 2_000_000

# ---------------------------------------------------------------------------
# Stylesheet — clean, modern, consistent
# ---------------------------------------------------------------------------

STYLESHEET = """
/* ── Base ── */
QMainWindow, QWidget {
    background-color: #f0f4f9;
    color: #0f172a;
    font-family: "Segoe UI", "Inter", sans-serif;
    font-size: 10pt;
}

/* ── Cards / GroupBoxes ── */
QGroupBox {
    font-size: 10pt;
    font-weight: 700;
    color: #1e3a5f;
    border: 1px solid #dce6f0;
    border-radius: 10px;
    background: #ffffff;
    margin-top: 12px;
    padding: 14px 10px 10px 10px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #1e3a5f;
    background: #ffffff;
    font-size: 9.5pt;
}

/* ── Default button ── */
QPushButton {
    background-color: #e8eef8;
    color: #1e3a5f;
    border: 1px solid #c7d3e4;
    border-radius: 8px;
    padding: 4px 10px;
    font-weight: 600;
    min-height: 24px;
}
QPushButton:hover   { background-color: #d8e6f5; border-color: #a8c0db; }
QPushButton:pressed { background-color: #c8d8f0; }
QPushButton:disabled { background-color: #f0f4f9; color: #9baabf; border-color: #dce6f0; }

/* ── Connect button ── */
QPushButton#btnConnect[connected="true"]  {
    background: #fef2f2; color: #b91c1c; border-color: #fca5a5;
}
QPushButton#btnConnect[connected="false"] {
    background: #f0fdf4; color: #15803d; border-color: #86efac;
}

/* ── Primary action buttons (SNAP, CV RUN, TM RUN, stream start) ── */
QPushButton#btnSnap,
QPushButton#btnRunCV,   QPushButton#btnRunML,
QPushButton#btnSnapAndRunCV, QPushButton#btnSnapAndRunML,
QPushButton#btnStartStream,
QPushButton#btnAddRun,  QPushButton#dsCaptureButton {
    background: #2563eb;
    color: #ffffff;
    border-color: #1d4ed8;
    font-weight: 700;
}
QPushButton#btnSnap:hover,
QPushButton#btnRunCV:hover,   QPushButton#btnRunML:hover,
QPushButton#btnSnapAndRunCV:hover, QPushButton#btnSnapAndRunML:hover,
QPushButton#btnStartStream:hover,
QPushButton#btnAddRun:hover,  QPushButton#dsCaptureButton:hover {
    background: #1d4ed8;
}
QPushButton#btnSnap:disabled,
QPushButton#btnRunCV:disabled,   QPushButton#btnRunML:disabled,
QPushButton#btnSnapAndRunCV:disabled, QPushButton#btnSnapAndRunML:disabled {
    background: #cbd5e1; color: #64748b; border-color: #94a3b8;
}

/* ── Stop / destructive ── */
QPushButton#btnStopStream {
    background: #fffbeb; color: #92400e; border-color: #f59e0b;
}
QPushButton#btnStopStream:hover {
    background: #fef3c7;
}

/* ── Save / secondary ── */
QPushButton#btnSaveCVResult, QPushButton#btnSaveMLResult,
QPushButton#btnSaveDebugImages, QPushButton#btnSaveFrame,
QPushButton#dsSaveButton {
    background: #f0fdf4; color: #166534; border-color: #86efac;
}

/* ── Inputs ── */
QLineEdit, QPlainTextEdit, QComboBox, QSpinBox {
    background: #ffffff;
    color: #0f172a;
    border: 1px solid #cbd5e1;
    border-radius: 8px;
    padding: 4px 8px;
    selection-background-color: #bfdbfe;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
    border-color: #60a5fa;
}

/* ── Image preview labels ── */
QLabel#imageLabel, QLabel#grayImageLabel,
QLabel#binaryImageLabel, QLabel#overlayImageLabel {
    background-color: #0d1117;
    border: 1px solid #1e293b;
    border-radius: 8px;
    color: #64748b;
}

/* ── Text roles ── */
QLabel[role="key"]   { color: #334155; font-weight: 600; font-size: 9.5pt; }
QLabel[role="value"] { color: #0f172a; font-weight: 500; }
QLabel[role="hint"]  { color: #64748b; font-size: 9pt; }

/* ── Status labels ── */
QLabel#connStatusLabel[connected="true"]  { color: #059669; font-weight: 700; }
QLabel#connStatusLabel[connected="false"] { color: #dc2626; font-weight: 700; }
QLabel#cvCountValueLabel { color: #059669; font-weight: 800; font-size: 14pt; }
QLabel#mlPredValueLabel, QLabel#mlPredValueLabel2 {
    color: #d97706; font-weight: 800;
}

/* ── Mode status banner labels ── */
QLabel#cvModeStatusLabel, QLabel#mlModeStatusLabel {
    border-radius: 6px;
    padding: 4px 8px;
    font-size: 9.5pt;
    background: #f8fafc;
    border: 1px solid #e2e8f0;
}

/* ── Log / mono ── */
QPlainTextEdit {
    font-family: "Consolas", "Courier New", monospace;
    font-size: 9pt;
    background: #f8fafc;
    color: #1e293b;
}

/* ── Tables ── */
QTableWidget {
    background: #ffffff;
    color: #0f172a;
    gridline-color: #e2e8f0;
    border: 1px solid #dce6f0;
    border-radius: 8px;
    alternate-background-color: #f8fafc;
    selection-background-color: #dbeafe;
}
QHeaderView::section {
    background: #edf3fb;
    color: #334155;
    padding: 4px 6px;
    min-height: 24px;
    border: none;
    border-right: 1px solid #dce6f0;
    border-bottom: 1px solid #dce6f0;
    font-weight: 700;
    font-size: 9pt;
}

/* ── Progress bars (TinyML scores) ── */
QProgressBar {
    background: #e2e8f0;
    border: none;
    border-radius: 7px;
    text-align: center;
    min-height: 16px;
    max-height: 20px;
    font-size: 8pt;
}
QProgressBar::chunk { background: #60a5fa; border-radius: 7px; }
QProgressBar[active="true"]::chunk { background: #22c55e; }

/* ── Slider ── */
QSlider::groove:horizontal {
    height: 6px;
    background: #e2e8f0;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #60a5fa;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #2563eb;
    border: 2px solid #1d4ed8;
    width: 16px;
    margin: -6px 0;
    border-radius: 8px;
}

/* ── Checkboxes ── */
QCheckBox { spacing: 6px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1.5px solid #94a3b8;
    border-radius: 4px;
    background: #ffffff;
}
QCheckBox::indicator:checked {
    background: #2563eb;
    border-color: #1d4ed8;
}

/* ── Tabs ── */
QTabWidget::pane {
    border: 1px solid #dce6f0;
    border-radius: 8px;
    background: #f0f4f9;
}
QTabBar::tab {
    background: #e8eef8;
    color: #475569;
    border: 1px solid #dce6f0;
    border-bottom: none;
    padding: 7px 20px;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
    font-weight: 600;
    font-size: 9.5pt;
}
QTabBar::tab:selected {
    background: #ffffff;
    color: #1e3a5f;
    border-bottom: 1px solid #ffffff;
}
QTabBar::tab:hover:!selected { background: #dce6f0; }

/* ── Scrollbar ── */
QScrollBar:vertical {
    width: 8px;
    background: #f0f4f9;
    border-radius: 4px;
}
QScrollBar::handle:vertical {
    background: #cbd5e1;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
"""
