"""
display_controller.py — Preview refresh, CV result display, TinyML result display.

Redesigned: full local CV pipeline (gray, binary, overlay) computed in Python.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QRect
from PySide6.QtGui import QColor, QFont, QFontMetrics, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QLabel

from image_utils import qimage_to_pixmap
from app_helpers import norm01, is_tm_uncertain, qimage_to_pil_rgb, pil_to_qimage_rgb
from protocol_parser import TINYML_CLASSES


class DisplayMixin:
    """Visual refresh: preview images, CV result strip, TinyML result strip."""

    # ---------------------------------------------------------------- preview
    def _refresh_preview(self) -> None:
        """Re-render all image preview labels."""
        def _display(name: str, qimg: QImage | None) -> None:
            lbl = self.window.findChild(QLabel, name)
            if lbl is None:
                return
            if qimg is None or qimg.isNull():
                lbl.setPixmap(QPixmap())
                lbl.setText("—")
                return
            pm = qimage_to_pixmap(qimg)
            lbl.setPixmap(pm.scaled(
                lbl.size(),
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            ))
            lbl.setText("")

        if self._last_qimg is None or self._last_qimg.isNull():
            self._w("imageLabel").setText("No frame — connect STM32 and press SNAP")
            self._w("imageLabel").setPixmap(QPixmap())
            return

        _display("imageLabel",        self._last_qimg)
        _display("grayImageLabel",    self._build_gray_qimage())
        _display("binaryImageLabel",  self._build_binary_qimage())
        _display("overlayImageLabel", self._build_overlay_qimage())

        # Update mode status banner after every frame
        self._update_mode_status_banner()

    # ---------------------------------------------------------------- local CV pipeline
    def _build_gray_qimage(self) -> QImage:
        if self._last_qimg is None:
            return QImage()
        return pil_to_qimage_rgb(qimage_to_pil_rgb(self._last_qimg).convert("L"))

    def _build_binary_qimage(self) -> QImage:
        """
        Full local binary preview using CURRENT UI parameter values.
        This gives instant feedback when sliders/checkboxes change — no STM32 needed.
        """
        if self._last_qimg is None:
            return QImage()

        # Read current UI state
        otsu_cb = self._cv_widget("cvOtsuCheck")
        inv_cb  = self._cv_widget("cvInvertCheck")
        slider  = self._cv_widget("cvThrSlider")
        invert  = bool(inv_cb and inv_cb.isChecked())
        thr     = slider.value() if slider else 128

        pil = qimage_to_pil_rgb(self._last_qimg).convert("L")

        # Local Otsu estimate
        if otsu_cb and otsu_cb.isChecked():
            thr = self._local_otsu(pil)

        binary = pil.point(lambda p: 255 if p >= thr else 0)
        if invert:
            binary = binary.point(lambda p: 255 - p)
        return pil_to_qimage_rgb(binary)

    def _local_otsu(self, pil_gray) -> int:
        """Compute Otsu threshold locally (matches STM32 cv_otsu_threshold)."""
        try:
            hist = [0] * 256
            for p in pil_gray.getdata():
                hist[p] += 1
            total = sum(hist)
            if total == 0:
                return 128
            sum_total = sum(i * hist[i] for i in range(256))
            sum_bg = 0
            weight_bg = 0
            max_var = 0.0
            best_thr = 128
            for t in range(256):
                weight_bg += hist[t]
                if weight_bg == 0:
                    continue
                weight_fg = total - weight_bg
                if weight_fg == 0:
                    break
                sum_bg += t * hist[t]
                mean_bg = sum_bg / weight_bg
                mean_fg = (sum_total - sum_bg) / weight_fg
                var = weight_bg * weight_fg * (mean_bg - mean_fg) ** 2
                if var > max_var:
                    max_var = var
                    best_thr = t
            return best_thr
        except Exception:
            return 128

    def _build_overlay_qimage(self) -> QImage:
        """Draw firmware CVBOX bounding boxes over the original frame."""
        if self._last_qimg is None:
            return QImage()
        overlay = self._last_qimg.copy()
        if self._cv_result is None or not self._cv_result.boxes:
            return overlay
        painter = QPainter(overlay)
        painter.setPen(QPen(QColor(0, 220, 0), 2))
        painter.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
        for b in self._cv_result.boxes:
            painter.drawRect(QRect(b.x, b.y, b.w, b.h))
            painter.drawText(b.x + 2, b.y + 14, str(b.box_id))
        painter.end()
        return overlay

    # ---------------------------------------------------------------- mode status banner
    def _update_mode_status_banner(self) -> None:
        """
        Show a clear status banner in the Vision tab indicating whether CV/TM
        can run. Grays out CV RUN and TM RUN when mode is JPEG.
        Also updates a compact status label visible at all times.
        """
        fmt    = self._last_format
        is_rgb = fmt in ("RGB565", "GRAY")
        has_frame = self._last_qimg is not None and not self._last_qimg.isNull()

        # Status text
        if not has_frame:
            status = "⚠  No frame — press SNAP"
            color  = "#e67e22"
        elif is_rgb:
            w = self._last_qimg.width() if self._last_qimg else 0
            h = self._last_qimg.height() if self._last_qimg else 0
            status = f"✓  RGB565 {w}×{h} — CV RUN and TM RUN available"
            color  = "#27ae60"
        else:
            status = f"⚠  JPEG mode — switch to RGB + SNAP before running CV or TinyML"
            color  = "#c0392b"

        # Update all status labels that show this info
        for lbl_name in ("cvModeStatusLabel", "mlModeStatusLabel", "visionModeStatusLabel"):
            lbl = self.window.findChild(object, lbl_name)
            if lbl and hasattr(lbl, "setText"):
                lbl.setText(status)
                lbl.setStyleSheet(f"color: {color}; font-weight: bold;")

        # Gray out / enable CV and TM action buttons based on frame availability
        cv_ok = has_frame and is_rgb
        for btn_name in ("btnRunCV", "btnSnapAndRunCV", "cvBtnBgCap"):
            btn = self.window.findChild(object, btn_name)
            if btn:
                btn.setEnabled(cv_ok and self._stm32_ready)
                if not cv_ok:
                    btn.setToolTip(
                        "Requires RGB565 frame — switch to RGB mode and press SNAP")

        tm_ok = has_frame and is_rgb
        for btn_name in ("btnRunML", "btnSnapAndRunML"):
            btn = self.window.findChild(object, btn_name)
            if btn:
                btn.setEnabled(tm_ok and self._stm32_ready)
                if not tm_ok:
                    btn.setToolTip(
                        "Requires RGB565 frame — switch to RGB mode and press SNAP")

    # ---------------------------------------------------------------- CV display
    def _update_cv_idle_display(self) -> None:
        """Reset CV result labels to idle state."""
        for label_name, text in [
            ("cvCountValueLabel",  "Run CV"),
            ("cvTimeValueLabel",   "—"),
            ("cvBrightValueLabel", "—"),
            ("cvBoxesValueLabel",  "—"),
        ]:
            lbl = self.window.findChild(object, label_name)
            if lbl:
                lbl.setText(text)
        self._clear_cv_result_ui()

    def _update_cv_display(self) -> None:
        """Populate CV result labels from the latest CvResult."""
        if self._cv_result is None:
            return
        r = self._cv_result
        for name, val in [
            ("cvCountValueLabel",  str(r.count)),
            ("cvTimeValueLabel",   f"{r.processing_time_ms} ms"),
            ("cvBrightValueLabel", str(r.mean_brightness)),
            ("cvBoxesValueLabel",  f"{len(r.boxes)} box(es)"),
        ]:
            lbl = self.window.findChild(object, name)
            if lbl:
                lbl.setText(val)

    # ---------------------------------------------------------------- TinyML display
    def _update_ml_display(self) -> None:
        """Populate all TinyML result labels and probability bars."""
        if self._tm_result is None:
            return
        r         = self._tm_result
        conf      = norm01(r.confidence)
        uncertain = is_tm_uncertain(r)
        pred_disp = "uncertain" if uncertain else (r.predicted_name or "—")

        for name in ("mlPredValueLabel", "mlPredValueLabel2"):
            self._set_compact_label(name, pred_disp, max_chars=22)
        for name in ("mlConfValueLabel", "mlConfValueLabel2"):
            self._set_compact_label(name, f"{conf:.3f}  ({conf*100:.1f}%)", max_chars=22)

        t_lbl = self.window.findChild(object, "mlTimeValueLabel")
        if t_lbl:
            t_lbl.setText(f"{r.inference_time_ms} ms")

        unc_lbl = self.window.findChild(object, "mlUncertainLabel")
        if unc_lbl:
            unc_lbl.setText("⚠ YES" if uncertain else "No")

        status_text = "⚠ UNCERTAIN" if uncertain else "X-CUBE-AI OK"
        self._set_compact_label("mlStatusLabel", status_text)

        for i, cls in enumerate(TINYML_CLASSES):
            score  = norm01(r.scores.get(cls, 0.0))
            is_top = (cls == r.predicted_name) and not uncertain
            bar  = self.window.findChild(object, f"sc{i}Bar")
            vlbl = self.window.findChild(object, f"sc{i}ValLabel")
            if bar:
                bar.setValue(int(score * 1000))
                bar.setProperty("active", "true" if is_top else "false")
                bar.style().unpolish(bar)
                bar.style().polish(bar)
            if vlbl:
                vlbl.setText(f"{score*100:.1f}%")

    # ---------------------------------------------------------------- compact label helper
    def _set_compact_label(
        self,
        name: str,
        text: str,
        *,
        tooltip: str | None = None,
        max_chars: int = 44,
    ) -> None:
        w = self.window.findChild(object, name)
        if w is None:
            return
        full  = text or "—"
        shown = full[:max_chars - 1] + "…" if len(full) > max_chars else full
        try:
            fm    = QFontMetrics(w.font())
            width = max(90, min(260, w.width() or 180))
            shown = fm.elidedText(full, Qt.TextElideMode.ElideRight, width)
        except Exception:
            pass
        w.setText(shown)
        w.setToolTip(tooltip if tooltip is not None else full)

