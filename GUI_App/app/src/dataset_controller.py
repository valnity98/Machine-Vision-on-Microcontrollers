"""
dataset_controller.py — Dataset capture logic.

Mixin providing all _dataset_* methods for ReferenceWindowController.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFileDialog, QLineEdit, QMessageBox,
    QPushButton, QSpinBox,
)

from app_constants import DEFAULT_BAUDRATE_INT
from app_helpers import timestamp


class DatasetMixin:
    """Automated dataset capture (RGB565/QVGA → labelled training images)."""

    # ---------------------------------------------------------------- UI helpers
    def _dataset_browse_root(self) -> None:
        root_le = self.window.findChild(QLineEdit, "dsRootLineEdit")
        start   = root_le.text().strip() if root_le else ""
        folder  = QFileDialog.getExistingDirectory(
            self.window, "Select dataset root folder", start
        )
        if folder and root_le:
            root_le.setText(folder)
            self._dataset_update_status()

    def _dataset_target_dir(self) -> Path:
        root_le  = self.window.findChild(QLineEdit, "dsRootLineEdit")
        split_cb = self.window.findChild(QComboBox,  "dsSplitCombo")
        label_cb = self.window.findChild(QComboBox,  "dsLabelCombo")
        root_text = root_le.text().strip()  if root_le  else str(self._dataset_default_root)
        split     = split_cb.currentText() if split_cb else "train"
        label     = label_cb.currentText() if label_cb else "count_0"
        return Path(root_text) / split / label

    def _dataset_update_status(self) -> None:
        """Refresh the dataset status label (target path, existing count, saved count)."""
        status_lbl = self.window.findChild(object, "dsStatusLabel")
        count_sp   = self.window.findChild(QSpinBox, "dsCountSpin")
        n          = count_sp.value() if count_sp else "?"
        target     = self._dataset_target_dir()
        try:
            existing = len(list(target.glob("*.png"))) + len(list(target.glob("*.jpg")))
        except Exception:
            existing = 0
        if status_lbl:
            parts = Path(target).parts
            short = "/".join(parts[-2:]) if len(parts) >= 2 else str(target)
            status_lbl.setText(
                f"Target: …/{short}  |  Existing: {existing}  |  "
                f"Saved: {self._dataset_saved_count}/{n}"
            )
            status_lbl.setToolTip(str(target))
        self._update_dataset_baud_hint()

    def _update_dataset_baud_hint(self) -> None:
        hint = self.window.findChild(object, "dsBaudHintLabel")
        if not hint:
            return
        try:
            baud = int(self._w("baudrateComboBox").currentText().strip())
        except Exception:
            baud = DEFAULT_BAUDRATE_INT
        baud    = max(1, baud)
        seconds = (320 * 240 * 2 * 10.0) / float(baud)   # QVGA RGB565
        hint.setText(
            f"At {baud} baud: QVGA/RGB565 ≈ {seconds:.1f} s/frame. "
        )

    def _dataset_set_running(self, running: bool) -> None:
        """Enable/disable dataset action buttons based on capture state."""
        for name, enabled in [
            ("dsCaptureButton", not running),
            ("dsStopButton",        running),
            ("dsSaveButton",    not running),
        ]:
            w = self.window.findChild(QPushButton, name)
            if w:
                w.setEnabled(enabled)

    # ---------------------------------------------------------------- single save
    def _dataset_save_current_clicked(self) -> None:
        if self._last_qimg is None or self._last_qimg.isNull():
            QMessageBox.information(self.window, "Dataset", "No frame available.  Press SNAP first.")
            return
        self._dataset_save_current_frame(show_message=True)

    def _dataset_save_current_frame(self, *, show_message: bool = False) -> bool:
        """Save the current frame to the dataset target directory. Returns True on success."""
        target = self._dataset_target_dir()
        try:
            target.mkdir(parents=True, exist_ok=True)
        except Exception as exc:
            self._log("ERR", f"Dataset: cannot create folder {target}: {exc}")
            return False

        ts = timestamp()
        if self._last_format == "JPEG" and self._last_raw_bytes:
            file_path = target / f"img_{ts}.jpg"
            try:
                file_path.write_bytes(self._last_raw_bytes)
            except Exception as exc:
                self._log("ERR", f"Dataset: write failed: {exc}")
                return False
        else:
            file_path = target / f"img_{ts}.png"
            if not self._last_qimg.save(str(file_path), "PNG"):
                self._log("ERR", "Dataset: QImage.save() failed")
                return False

        self._log("INFO", f"Dataset: saved → {self._relative_to_project(file_path)}")
        if show_message:
            QMessageBox.information(self.window, "Dataset", f"Saved:\n{file_path}")
        return True

    # ---------------------------------------------------------------- automated capture
    def _dataset_capture_n_clicked(self) -> None:
        """Start automated capture of N frames."""
        if not self.serial.is_open():
            QMessageBox.information(self.window, "Dataset", "Connect STM32 first.")
            return
        count_sp = self.window.findChild(QSpinBox, "dsCountSpin")
        self._dataset_target_count = count_sp.value() if count_sp else 10
        self._dataset_saved_count  = 0
        self._dataset_pending_save = False
        progress = self.window.findChild(object, "dsProgress")
        if progress:
            progress.setValue(0)
        self._dataset_set_running(True)
        self._dataset_update_status()
        self._log("INFO", f"Dataset capture started: {self._dataset_target_count} frames")
        self._dataset_next_capture()

    def _dataset_next_capture(self) -> None:
        """Trigger the next capture in the automated sequence."""
        if self._dataset_target_count > 0 and self._dataset_saved_count >= self._dataset_target_count:
            self._dataset_stop(finished=True)
            return
        if self._dataset_pending_save:
            return
        if self._camera_transfer_busy():
            self._log("WARN", "Dataset: waiting for transfer to finish")
            self._dataset_timer.start(1000)
            return
        self._dataset_pending_save  = True
        self._dataset_pending_frame = self._frame_count
        if not self._dataset_prepare_camera_and_snap():
            self._dataset_pending_save = False
            self._dataset_stop()

    def _dataset_prepare_camera_and_snap(self) -> bool:
        """Configure camera for RGB/QVGA and send SNAP for dataset capture."""
        cb        = self.window.findChild(QCheckBox, "dsForceRgbCheck")
        force_rgb = cb.isChecked() if cb else False
        if force_rgb:
            self._w("camModeComboBox").setCurrentText("RGB")
            self._w("camResComboBox").setCurrentText("QVGA")
        self._send("STREAM 0")
        self._update_stream_status(False)
        changed = self._send_camera_config_if_changed(force=force_rgb, log=False)
        if force_rgb:
            self._apply_camera_defaults(log=False)
        delay_ms = 600 if changed else 300
        return self._schedule_snap(delay_ms, "Dataset SNAP")

    def _dataset_on_new_frame(self) -> None:
        """Called after each new frame arrives — saves it if dataset capture is pending."""
        if not self._dataset_pending_save:
            return
        if self._frame_count <= self._dataset_pending_frame:
            return
        ok = self._dataset_save_current_frame(show_message=False)
        self._dataset_pending_save = False
        if ok:
            self._dataset_saved_count += 1
            progress = self.window.findChild(object, "dsProgress")
            if progress and self._dataset_target_count > 0:
                progress.setValue(int(100 * self._dataset_saved_count / self._dataset_target_count))
            self._dataset_update_status()
        if self._dataset_timer.isActive():
            return
        if self._dataset_target_count > 0 and self._dataset_saved_count >= self._dataset_target_count:
            self._dataset_stop(finished=True)
            return
        interval_sp = self.window.findChild(QSpinBox, "dsIntervalSpin")
        delay_ms    = interval_sp.value() if interval_sp else 1500
        self._dataset_timer.start(max(100, delay_ms))

    def _dataset_stop(self, *, finished: bool = False) -> None:
        self._dataset_timer.stop()
        self._dataset_pending_save = False
        self._dataset_set_running(False)
        self._dataset_update_status()
        msg = f"Dataset capture {'finished' if finished else 'stopped'}: {self._dataset_saved_count} frames"
        self._log("INFO", msg)
