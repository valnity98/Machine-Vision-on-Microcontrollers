"""Conversion of the raw frame bytes from the STM32 into QImage objects.

JPG frames are JPEG data, RGB565 frames are packed little-endian 16-bit pixels.
The firmware can send a few extra bytes before the JPEG start marker (FFD8), so
the JPEG is cut out between FFD8 and FFD9 before Qt sees it. Otherwise Qt prints
"Corrupt JPEG data" warnings.
"""

from __future__ import annotations

from PySide6.QtGui import QImage, QPixmap


# SOI and EOI marker bytes for JPEG robustness scan.
_JPEG_SOI = b"\xff\xd8"
_JPEG_EOI = b"\xff\xd9"


def jpeg_bytes_to_qimage(data: bytes) -> QImage:
    """
    Decode a JPEG byte payload received after 'JPG: <size>'.

    Before passing the payload to Qt the function locates the first FFD8 SOI
    and the last (or next) FFD9 EOI marker.  Extra bytes before FFD8 are
    silently discarded, preventing Qt JPEG-decoder warnings.

    Returns an empty (null) QImage if no valid FFD8…FFD9 span is found.
    """
    # Locate the SOI marker.
    soi = data.find(_JPEG_SOI)
    if soi < 0:
        return QImage()

    # Locate the EOI marker after the SOI.
    eoi = data.find(_JPEG_EOI, soi + 2)
    if eoi < 0:
        return QImage()

    clean = data[soi : eoi + 2]
    img   = QImage()
    img.loadFromData(clean, "JPG")
    return img


def rgb565_bytes_to_qimage(data: bytes, width: int, height: int) -> QImage:
    """
    Convert a little-endian RGB565 payload (from STM32 DCMI frame buffer) to a
    QImage in RGB888 format.

    The STM32 frame buffer stores pixels as 16-bit words in little-endian byte
    order: the low byte arrives first, then the high byte.

    Pixel layout in the 16-bit word:
        [15:11] = R5  (5 bits of red)
        [10:5]  = G6  (6 bits of green)
        [4:0]   = B5  (5 bits of blue)

    Returns an empty (null) QImage if the buffer is too small or dimensions
    are invalid.
    """
    expected = int(width) * int(height) * 2
    if width <= 0 or height <= 0 or len(data) < expected:
        return QImage()

    out = bytearray(width * height * 3)
    j   = 0
    for i in range(0, expected, 2):
        p  = data[i] | (data[i + 1] << 8)    # little-endian 16-bit word
        r5 = (p >> 11) & 0x1F
        g6 = (p >>  5) & 0x3F
        b5 =  p        & 0x1F
        out[j]     = (r5 * 255 + 15) // 31   # scale 5-bit → 8-bit (rounded)
        out[j + 1] = (g6 * 255 + 31) // 63   # scale 6-bit → 8-bit (rounded)
        out[j + 2] = (b5 * 255 + 15) // 31   # scale 5-bit → 8-bit (rounded)
        j += 3

    return QImage(bytes(out), width, height, width * 3,
                  QImage.Format.Format_RGB888).copy()


def qimage_to_pixmap(img: QImage) -> QPixmap:
    """Return a QPixmap copy suitable for QLabel display."""
    return QPixmap.fromImage(img)
