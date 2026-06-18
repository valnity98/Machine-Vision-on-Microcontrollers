"""
save_controller.py — File save operations: frame, debug images, CV result, TinyML result, log.

Mixin providing all _save_* methods for ReferenceWindowController.
"""

from __future__ import annotations

import csv
import json
from datetime import datetime

from PySide6.QtWidgets import QMessageBox

from app_helpers import norm01, is_tm_uncertain, timestamp


class SaveMixin:
    """Save operations: single frame, debug images, CV result, TinyML result, log, save-all."""

    # ---------------------------------------------------------------- frame
    def _save_current_frame_clicked(self) -> None:
        """Save the raw camera frame to the current image folder."""
        if self._last_qimg is None or self._last_qimg.isNull():
            QMessageBox.information(
                self.window, "Save Frame",
                "No frame available — press SNAP first.")
            return
        out = self._image_sub("frame")
        ts  = timestamp()
        if self._last_format == "JPEG" and self._last_raw_bytes:
            path = out / f"frame_{ts}.jpg"
            path.write_bytes(self._last_raw_bytes)
        else:
            path = out / f"frame_{ts}.png"
            if not self._last_qimg.save(str(path), "PNG"):
                QMessageBox.critical(
                    self.window, "Save Frame", "QImage.save() failed.")
                return
        self._log("INFO", f"Saved frame → {self._relative_to_project(path)}")

    # ---------------------------------------------------------------- debug images
    def _save_debug_images_clicked(self) -> None:
        """Save gray, binary, and overlay images derived from the current frame."""
        if self._last_qimg is None or self._last_qimg.isNull():
            QMessageBox.warning(
                self.window, "Save Images", "No valid frame available.")
            return
        out   = self._image_sub("debug")
        saved = []
        for img, name in [
            (self._build_gray_qimage(),    "gray_preview.png"),
            (self._build_binary_qimage(),  "binary_preview.png"),
            (self._build_overlay_qimage(), "overlay.png"),
        ]:
            if img and not img.isNull() and img.save(str(out / name), "PNG"):
                saved.append(name)
            else:
                self._log("WARN", f"Could not build/save {name} — frame may not have CV result")

        if not saved:
            QMessageBox.warning(
                self.window, "Save Images",
                "No images could be saved.\n"
                "Run CV RUN first to generate Binary and Overlay previews.")
            return

        self._log(
            "INFO",
            f"Saved images ({', '.join(saved)}) → {self._relative_to_project(out)}")
        self._set_eval_last_save(str(out))

    # ---------------------------------------------------------------- CV result
    def _save_cv_result_clicked(self) -> None:
        """Save the current CV result as cv_result.csv + cv_result.json."""
        if self._cv_result is None:
            QMessageBox.information(
                self.window, "Save CV",
                "No CV result — run STM32 CV first.")
            return
        out      = self._image_sub("cv")
        r        = self._cv_result
        ts       = datetime.now().isoformat(timespec="seconds")
        box_rows = [
            {
                "id": b.box_id, "area": b.area, "x": b.x, "y": b.y,
                "w": b.w, "h": b.h, "perimeter": b.perimeter,
                "circularity": f"{b.circularity_x1000 / 1000:.3f}",
            }
            for b in r.boxes
        ]

        # CSV
        with (out / "cv_result.csv").open("w", newline="", encoding="utf-8") as f:
            wr = csv.writer(f)
            wr.writerow([
                "timestamp", "count", "mean_area", "max_area", "min_area",
                "brightness", "processing_time_ms",
                "rejected_small", "rejected_large",
                "rejected_border", "rejected_shape",
                "fg_pixel_count", "raw_comp_count", "frame_fmt",
            ])
            wr.writerow([
                ts, r.count,
                getattr(r, "mean_area",  "—"),
                getattr(r, "area_max",   "—"),
                getattr(r, "area_min",   "—"),
                r.mean_brightness, r.processing_time_ms,
                getattr(r, "rejected_small",  0),
                getattr(r, "rejected_large",  0),
                getattr(r, "rejected_border", 0),
                getattr(r, "rejected_shape",  0),
                getattr(r, "fg_pixel_count",  0),
                getattr(r, "raw_comp_count",  0),
                self._last_fmt_res,
            ])
            if box_rows:
                wr.writerow([])
                wr.writerow(["CVBOX", "id", "area", "x", "y",
                             "w", "h", "perimeter", "circularity"])
                for br in box_rows:
                    wr.writerow([
                        "", br["id"], br["area"], br["x"], br["y"],
                        br["w"], br["h"], br["perimeter"], br["circularity"],
                    ])

        # JSON
        cfg_dict: dict = {}
        if self._stm32_cv_cfg is not None:
            c = self._stm32_cv_cfg
            cfg_dict = {
                "enabled":               c.enabled,
                "preset":                c.preset,
                "threshold":             c.threshold,
                "threshold_mode":        c.threshold_mode,
                "invert":                c.invert,
                "connectivity":          c.connectivity,
                "filter_mode":           c.filter_mode,
                "blur_kernel":           c.blur_kernel,
                "morph_mode":            c.morph_mode,
                "morph_kernel":          c.morph_kernel,
                "min_area":              c.min_area,
                "max_area":              c.max_area,
                "border_filter_enabled": getattr(c, "border_filter_enabled", 0),
                "bgsub_enabled":         getattr(c, "bgsub_enabled", 0),
            }
        (out / "cv_result.json").write_text(
            json.dumps(
                {
                    "timestamp":  ts,
                    "frame_fmt":  self._last_fmt_res,
                    "cv_config":  cfg_dict,
                    "cvstat": {
                        "count":              r.count,
                        "mean_area":          getattr(r, "mean_area",       None),
                        "brightness":         r.mean_brightness,
                        "processing_time_ms": r.processing_time_ms,
                        "rejected_small":     getattr(r, "rejected_small",  0),
                        "rejected_large":     getattr(r, "rejected_large",  0),
                        "rejected_border":    getattr(r, "rejected_border", 0),
                        "rejected_shape":     getattr(r, "rejected_shape",  0),
                        "fg_pixel_count":     getattr(r, "fg_pixel_count",  0),
                        "raw_comp_count":     getattr(r, "raw_comp_count",  0),
                    },
                    "cvbox":        box_rows,
                    "log_excerpt":  self._last_log_lines(20),
                },
                indent=2, ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        self._log("INFO",
            f"Saved CV result → {self._relative_to_project(out)}")
        self._set_eval_last_save(str(out))

    # ---------------------------------------------------------------- TinyML result
    def _save_ml_result_clicked(self) -> None:
        """Save the current TinyML result as tinyml_result.csv + tinyml_result.json."""
        if self._tm_result is None:
            QMessageBox.information(
                self.window, "Save ML",
                "No TinyML result — run TinyML first.")
            return
        out       = self._image_sub("tinyml")
        r         = self._tm_result
        ts        = datetime.now().isoformat(timespec="seconds")
        conf      = norm01(r.confidence)
        uncertain = is_tm_uncertain(r)

        with (out / "tinyml_result.csv").open(
            "w", newline="", encoding="utf-8"
        ) as f:
            wr = csv.writer(f)
            wr.writerow([
                "timestamp", "class_name", "class_index", "confidence",
                "inference_time_ms", "uncertain", "frame_fmt",
            ])
            wr.writerow([
                ts, r.predicted_name, r.predicted_index,
                f"{conf:.4f}", r.inference_time_ms,
                int(uncertain), self._last_fmt_res,
            ])
            wr.writerow([])
            wr.writerow(["TMPROB", "class_name", "score"])
            for cls, score in r.scores.items():
                wr.writerow(["", cls, f"{norm01(score):.4f}"])

        (out / "tinyml_result.json").write_text(
            json.dumps(
                {
                    "timestamp": ts,
                    "frame_fmt": self._last_fmt_res,
                    "tmres": {
                        "class_name":        r.predicted_name,
                        "class_index":       r.predicted_index,
                        "confidence":        conf,
                        "inference_time_ms": r.inference_time_ms,
                        "uncertain":         uncertain,
                    },
                    "tmprob":       {cls: norm01(s) for cls, s in r.scores.items()},
                    "tminfo": {
                        "ram_kb":   getattr(r, "ram_kb",         None),
                        "flash_kb": getattr(r, "flash_kb",       None),
                        "status":   getattr(r, "runtime_status", "—"),
                    },
                    "log_excerpt": self._last_log_lines(20),
                },
                indent=2, ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        self._log("INFO",
            f"Saved TinyML result → {self._relative_to_project(out)}")
        self._set_eval_last_save(str(out))

    # ---------------------------------------------------------------- log
    def _save_log_clicked(self) -> None:
        """Save the UART log to a timestamped text file."""
        text = self._w("logEdit").toPlainText()
        if not text.strip():
            QMessageBox.information(self.window, "Save Log", "Log is empty.")
            return
        out  = self._image_sub("logs")
        path = out / f"uart_log_{timestamp()}.txt"
        try:
            path.write_text(text + "\n", encoding="utf-8")
        except Exception as exc:
            QMessageBox.critical(self.window, "Save Log", f"Failed:\n{exc}")
            return
        self._log("INFO", f"Saved log → {self._relative_to_project(path)}")

