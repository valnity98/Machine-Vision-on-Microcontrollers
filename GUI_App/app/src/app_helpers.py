"""
app_helpers.py — Module-level helper functions and the FrameTransfer state machine.
"""

from __future__ import annotations

import sys
from datetime import datetime
from pathlib import Path

from PIL import Image
from PySide6.QtCore import Qt
from PySide6.QtGui import QImage

from app_constants import TM_UNCERTAIN_THRESHOLD


# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------

def application_root() -> Path:
    """
    Return the project root directory.

    Handles two common layouts:
      flat  : project/main.py + project/reference_window.ui  → returns project/
      nested: project/src/main.py + project/src/reference_window.ui → returns project/src/

    In a PyInstaller frozen build, returns the executable directory.
    """
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    script_dir = Path(__file__).resolve().parent
    # If the .ui file lives one level up (nested src/ layout), use that as root
    if (script_dir.parent / "reference_window.ui").exists():
        return script_dir.parent
    return script_dir


def resource_path(*parts: str) -> Path:
    """Resolve a resource path for both source and frozen (PyInstaller) builds."""
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return Path(sys._MEIPASS).joinpath(*parts)
    return Path(__file__).resolve().parent.joinpath(*parts)


def ui_path() -> Path:
    """
    Return the absolute path to reference_window.ui.

    Search order (source builds):
      1. <script_dir>/ui/reference_window.ui      (app/src/ → app/ui/)
      2. <script_dir>/../ui/reference_window.ui   (app/src/ → app/ui/ via parent)
      3. <script_dir>/reference_window.ui         (flat layout fallback)

    In a PyInstaller frozen build, looks in the _MEIPASS temp directory.
    """
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return Path(sys._MEIPASS) / "reference_window.ui"

    script_dir = Path(__file__).resolve().parent

    # Primary: ui/ subfolder next to src/ (app/ui/)
    candidate = script_dir.parent / "ui" / "reference_window.ui"
    if candidate.exists():
        return candidate

    # Secondary: ui/ inside the script folder
    candidate = script_dir / "ui" / "reference_window.ui"
    if candidate.exists():
        return candidate

    # Fallback: flat layout (all files in same folder)
    return script_dir / "reference_window.ui"


def timestamp() -> str:
    """Return a compact filesystem-safe timestamp string (YYYYMMDD_HHmmss)."""
    return datetime.now().strftime("%Y%m%d_%H%M%S")


# ---------------------------------------------------------------------------
# Normalisation / uncertainty helpers
# ---------------------------------------------------------------------------

def norm01(value: float) -> float:
    """
    Normalise a firmware confidence value to 0.0–1.0.

    The firmware sends confidence as permille (0..1000).
    Values already in [0.0, 1.0] are returned unchanged.
    """
    try:
        v = float(value)
    except Exception:
        return 0.0
    if v > 1.0:
        v /= 1000.0
    return max(0.0, min(1.0, v))


def is_tm_uncertain(tm_result) -> bool:
    """Return True if the TinyML result should be treated as uncertain."""
    if tm_result is None:
        return False
    if getattr(tm_result, "is_uncertain", False):
        return True
    return norm01(tm_result.confidence) < TM_UNCERTAIN_THRESHOLD


# ---------------------------------------------------------------------------
# QImage / PIL conversion helpers
# ---------------------------------------------------------------------------

def qimage_to_pil_rgb(qimg: QImage) -> Image.Image:
    """Convert a QImage to a PIL RGB image (used for local binary/gray preview)."""
    img  = qimg.convertToFormat(QImage.Format.Format_RGB888)
    w, h = img.width(), img.height()
    data = bytes(img.bits()[: img.bytesPerLine() * h])
    return Image.frombytes("RGB", (w, h), data, "raw", "RGB", img.bytesPerLine())


def pil_to_qimage_rgb(pil_img: Image.Image) -> QImage:
    """Convert a PIL image back to a QImage (always outputs RGB888)."""
    rgb  = pil_img.convert("RGB")
    w, h = rgb.size
    data = rgb.tobytes("raw", "RGB")
    return QImage(data, w, h, w * 3, QImage.Format.Format_RGB888).copy()


# ---------------------------------------------------------------------------
# JPEG-robust image transfer state machine
# ---------------------------------------------------------------------------

class FrameTransfer:
    """
    State machine for receiving a raw binary image over UART.

    After the firmware emits a JPG:/RGB565:/GRAY: header line, the controller
    calls start() to begin accumulating binary bytes.  _consume_binary() drains
    the raw UART buffer into this object.

    JPEG robustness
    ---------------
    The firmware may prepend a few framing bytes before the FFD8 SOI marker.
    extract_jpeg_safe() scans the received payload for FFD8…FFD9 and returns
    only the clean JPEG payload.
    """

    _MAX_FRAME_BYTES = 4 * 1024 * 1024   # 4 MB hard upper limit

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        """Reset all transfer state."""
        self.active        = False
        self.fmt           = ""
        self.expected_size = 0
        self.width         = 0
        self.height        = 0
        self.received      = bytearray()

    def start(self, header) -> None:
        """Begin a new transfer from a parsed ImageHeader."""
        if header.size <= 0 or header.size > self._MAX_FRAME_BYTES:
            return
        self.active        = True
        self.fmt           = header.fmt
        self.expected_size = header.size
        self.width         = header.width
        self.height        = header.height
        self.received      = bytearray()

    @property
    def complete(self) -> bool:
        """True once all expected bytes have been received."""
        return self.active and len(self.received) >= self.expected_size

    @staticmethod
    def extract_jpeg_safe(raw: bytes) -> bytes | None:
        """
        Locate FFD8 SOI and FFD9 EOI markers in *raw*.

        Returns a slice from FFD8 to FFD9 inclusive, or None when either
        marker is absent.
        """
        soi = raw.find(b"\xff\xd8")
        if soi < 0:
            return None
        eoi = raw.find(b"\xff\xd9", soi + 2)
        if eoi < 0:
            return None
        return raw[soi : eoi + 2]
