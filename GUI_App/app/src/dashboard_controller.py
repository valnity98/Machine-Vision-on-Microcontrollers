"""
dashboard_controller.py — Serial connection, camera control, and stream management.

Mixin providing serial/camera methods for ReferenceWindowController.

Key design decisions:
  - Stream and snapshot are mutually exclusive; _snap() queues itself after
    STREAM OFF if the stream is active when SNAP is requested.
  - Camera config (MODE/RES) is only re-sent when the value actually changes
    (_last_sent_mode / _last_sent_res guards).
  - Auto-CV-preview fires with a 400 ms debounce after CV parameter changes
    so the user sees the effect on the last captured frame immediately.
  - Controls remain disabled until the STM32 sends "OV2640 ready" or the
    3-second startup guard expires — whichever comes first.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QCheckBox, QComboBox, QMessageBox

from app_constants import (
    DEFAULT_BAUDRATE, DEFAULT_BAUDRATE_INT,
    JPEG_RESOLUTIONS, RGB_RESOLUTIONS,
)
from serial_service import SerialService

# Debounce delay (ms) before auto-triggering CV RUN after a parameter change
_AUTO_CV_DEBOUNCE_MS = 400


class DashboardMixin:
    """Serial connection and camera control methods."""

    # ---------------------------------------------------------------- ports
    def _refresh_ports(self) -> None:
        cb      = self._w("portComboBox")
        current = cb.currentText()
        cb.clear()
        ports = SerialService.available_ports()
        cb.addItems(ports)
        if current in ports:
            cb.setCurrentText(current)
        self._log("INFO",
            f"Ports: {', '.join(ports) if ports else 'none detected'}")

    # ---------------------------------------------------------------- connect
    def _toggle_connect(self) -> None:
        if self.serial.is_open():
            self.serial.close()
            self._on_disconnected()
            return
        port = self._w("portComboBox").currentText().strip()
        if not port:
            QMessageBox.warning(self.window, "Connection",
                                "No COM port selected.")
            return
        try:
            baud = int(
                self._w("baudrateComboBox").currentText().strip())
        except ValueError:
            QMessageBox.warning(self.window, "Connection",
                                "Invalid baudrate.")
            return
        if not self.serial.open(port, baud):
            QMessageBox.critical(
                self.window, "Connection Error",
                f"Cannot open {port}\n{self.serial.error_string()}",
            )
            return
        self._on_connected(port, baud)

    def _on_connected(self, port: str, baud: int) -> None:
        self._rx_buf.clear()
        self._transfer.reset()
        self._snapshot_in_flight   = False
        self._stream_active        = False
        self._snap_timeout_timer.stop()
        self._stm32_ready          = False
        self._startup_guard_active = True
        self._saw_valid_stm32_line = False
        self._set_conn_label(
            True,
            f"● Connected: {port} @ {baud}  — waiting for STM32")
        self._set_conn_btn(True)
        self._set_controls_enabled(False)
        for name in ("btnSendManual", "manualCommandLineEdit"):
            w = self.window.findChild(object, name)
            if w:
                w.setEnabled(True)
        self._log("INFO",
            f"Connected to {port} @ {baud}; waiting for OV2640 ready")
        self._last_sent_mode = None
        self._last_sent_res  = None
        self._last_sent_zoom = None
        # Do NOT send GET_STATUS immediately — STM32 may still be booting.
        # Commands sent during boot cause UART parser desync (garbage bytes).
        # GET_STATUS and CV/TM GET are sent by _mark_stm32_ready() when the
        # firmware sends "OV2640 ready", or by _finish_startup_guard() after 3s.
        QTimer.singleShot(3000, self._finish_startup_guard)
        self._dataset_set_running(False)
        self._update_stream_status(False)

    def _on_disconnected(self) -> None:
        self._set_conn_label(False, "● Disconnected")
        self._set_conn_btn(False)
        self._set_controls_enabled(False)
        self._last_sent_mode = None
        self._last_sent_res  = None
        self._last_sent_zoom = None
        self._stm32_ready    = False
        self._startup_guard_active = False
        self._snapshot_in_flight   = False
        self._stream_active        = False
        self._snap_timeout_timer.stop()
        self._transfer.reset()
        self._rx_buf.clear()
        if hasattr(self, "_pending_after_stream_off"):
            self._pending_after_stream_off.clear()
        self._log("INFO", "Disconnected")
        if self._dataset_timer.isActive():
            self._dataset_stop()
        else:
            self._dataset_set_running(False)
        self._update_stream_status(False)

    def _set_conn_label(self, connected: bool, text: str) -> None:
        lbl = self._w("connStatusLabel")
        lbl.setText(text)
        lbl.setProperty("connected", "true" if connected else "false")
        lbl.style().unpolish(lbl)
        lbl.style().polish(lbl)

    def _set_conn_btn(self, connected: bool) -> None:
        btn = self._w("btnConnect")
        btn.setText("Disconnect" if connected else "Connect")
        btn.setProperty("connected", "true" if connected else "false")
        btn.style().unpolish(btn)
        btn.style().polish(btn)

    def _finish_startup_guard(self) -> None:
        if not self.serial.is_open():
            return
        was_ready              = self._stm32_ready
        self._startup_guard_active = False
        self._stm32_ready          = True
        self._set_controls_enabled(True)
        self._set_conn_label(
            True,
            f"● Connected: {self.serial.port_name()} — STM32 ready")
        self._dataset_set_running(False)
        if not was_ready:
            self._log("INFO",
                "STM32 camera is ready — controls enabled")
            self._last_sent_mode = None
            self._last_sent_res  = None
            self._send_camera_config_if_changed(force=True, log=False)
            self._apply_camera_defaults(log=False)
        self._send("GET_STATUS", log=False)
        self._send("TM GET",     log=False)
        self._send("CV GET",     log=False)

    def _mark_stm32_ready(self, *, send_initial_status: bool = True) -> None:
        if self._stm32_ready and not self._startup_guard_active:
            return
        was_ready                  = self._stm32_ready
        self._stm32_ready          = True
        self._startup_guard_active = False
        self._set_controls_enabled(True)
        self._set_conn_label(
            True,
            f"● Connected: {self.serial.port_name()} — STM32 ready")
        self._dataset_set_running(False)
        if not was_ready:
            self._log("INFO", "STM32/OV2640 ready — controls enabled")
            self._last_sent_mode = None
            self._last_sent_res  = None
            self._send_camera_config_if_changed(force=True, log=False)
            self._apply_camera_defaults(log=False)
        if send_initial_status:
            self._send("GET_STATUS", log=False)
            self._send("TM GET",     log=False)
            self._send("CV GET",     log=False)

    def _require_camera_ready(self, action: str) -> bool:
        if self._stm32_ready and not self._startup_guard_active:
            return True
        self._log("WARN",
            f"{action} blocked — waiting for STM32/OV2640 ready.")
        return False

    def _on_baudrate_selection_changed(self) -> None:
        self._update_dataset_baud_hint()
        if not self.serial.is_open():
            return
        try:
            current = int(self.serial.serial.baudRate())
        except Exception:
            current = DEFAULT_BAUDRATE_INT
        self._log("WARN",
            "Baudrate change blocked while connected — disconnect first.")
        cb = self._w("baudrateComboBox")
        cb.blockSignals(True)
        idx = cb.findText(str(current))
        if idx >= 0:
            cb.setCurrentIndex(idx)
        cb.blockSignals(False)

    # ---------------------------------------------------------------- UART send
    def _send(self, cmd: str, log: bool = True) -> bool:
        if not self.serial.is_open():
            self._log("WARN", "Not connected — cannot send command")
            return False
        ok = self.serial.write_line(cmd)
        if log:
            self._log(
                "TX" if ok else "ERR",
                f"> {cmd.strip()}" if ok
                else f"Send failed: {cmd.strip()}")
        return ok

    def _send_manual(self) -> None:
        """Send the manual command and sync UI widgets if the command is recognised."""
        line = self._w("manualCommandLineEdit").text().strip()
        if not line:
            return
        if self._send(line):
            self._w("manualCommandLineEdit").clear()
            self._sync_ui_after_manual(line)

    def _sync_ui_after_manual(self, cmd: str) -> None:
        """Update GUI widgets when a manual command matches a known setter."""
        parts = cmd.strip().upper().split()
        if not parts:
            return
        top = parts[0]

        if top == "MODE" and len(parts) >= 2:
            cb = self.window.findChild(object, "camModeComboBox")
            if cb:
                idx = cb.findText(parts[1], Qt.MatchFlag.MatchFixedString)
                if idx >= 0:
                    cb.blockSignals(True)
                    cb.setCurrentIndex(idx)
                    cb.blockSignals(False)
                    self._update_resolution_combo()
            self._last_sent_mode = parts[1]

        elif top == "RES" and len(parts) >= 2:
            cb = self.window.findChild(object, "camResComboBox")
            if cb:
                idx = cb.findText(parts[1], Qt.MatchFlag.MatchFixedString)
                if idx >= 0:
                    cb.blockSignals(True)
                    cb.setCurrentIndex(idx)
                    cb.blockSignals(False)
            self._last_sent_res = parts[1]

        elif top == "BRI" and len(parts) >= 2:
            cb = self.window.findChild(object, "briComboBox")
            if cb:
                idx = cb.findText(parts[1])
                if idx >= 0:
                    cb.setCurrentIndex(idx)

        elif top == "CON" and len(parts) >= 2:
            cb = self.window.findChild(object, "conComboBox")
            if cb:
                idx = cb.findText(parts[1])
                if idx >= 0:
                    cb.setCurrentIndex(idx)

        elif top == "SAT" and len(parts) >= 2:
            cb = self.window.findChild(object, "satComboBox")
            if cb:
                idx = cb.findText(parts[1])
                if idx >= 0:
                    cb.setCurrentIndex(idx)

        elif top == "EFFECT" and len(parts) >= 2:
            cb = self.window.findChild(object, "effectComboBox")
            if cb:
                idx = cb.findText(parts[1])
                if idx >= 0:
                    cb.setCurrentIndex(idx)

        elif top == "LIGHT" and len(parts) >= 2:
            cb = self.window.findChild(object, "lightComboBox")
            if cb:
                for i in range(cb.count()):
                    if parts[1] in cb.itemText(i).upper():
                        cb.setCurrentIndex(i)
                        break

        elif top == "AWB" and len(parts) >= 2:
            cb = self.window.findChild(object, "awbComboBox")
            if cb:
                target = "ON" if parts[1] == "1" else "OFF"
                for i in range(cb.count()):
                    if target in cb.itemText(i).upper():
                        cb.setCurrentIndex(i)
                        break

        elif top == "FLASH" and len(parts) >= 2:
            cb = self.window.findChild(object, "flashCheckBox")
            if cb:
                cb.blockSignals(True)
                cb.setChecked(parts[1] == "1")
                cb.blockSignals(False)

        elif top == "ZOOM" and len(parts) >= 2:
            cb = self.window.findChild(object, "zoomComboBox")
            if cb:
                idx = cb.findText(parts[1], Qt.MatchFlag.MatchFixedString)
                if idx >= 0:
                    cb.blockSignals(True)
                    cb.setCurrentIndex(idx)
                    cb.blockSignals(False)
            self._last_sent_zoom = parts[1]

        elif top == "STREAM" and len(parts) >= 2:
            self._update_stream_status(parts[1] == "1")

        elif top == "CV" and len(parts) >= 2:
            sub = parts[1]
            val = parts[2] if len(parts) >= 3 else ""

            def _cw(name):
                return self.window.findChild(object, name)

            if sub == "EN" and val:
                w = _cw("cvEnableCheck")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)
            elif sub == "THRMODE" and val:
                w = _cw("cvOtsuCheck")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)
            elif sub == "THR" and val:
                try:
                    v  = int(val)
                    sl = _cw("cvThrSlider")
                    if sl:
                        sl.blockSignals(True)
                        sl.setValue(v)
                        sl.blockSignals(False)
                    lbl = _cw("cvThrValueLabel")
                    if lbl:
                        lbl.setText(val)
                except ValueError:
                    pass
            elif sub == "INV" and val:
                w = _cw("cvInvertCheck")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)
            elif sub == "FILTER" and val:
                w = _cw("cvFilterCombo")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setCurrentIndex(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "BLUR" and val:
                w = _cw("cvBlurSpin")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setValue(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "MORPHMODE" and val:
                w = _cw("cvMorphModeCombo")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setCurrentIndex(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "MORPH" and val:
                w = _cw("cvMorphKernelSpin")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setValue(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "CON" and val:
                w = _cw("cvConnCombo")
                if w:
                    idx = w.findText(val)
                    if idx >= 0:
                        w.blockSignals(True)
                        w.setCurrentIndex(idx)
                        w.blockSignals(False)
            elif sub == "MINAREA" and val:
                w = _cw("cvMinAreaSpin")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setValue(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "MAXAREA" and val:
                w = _cw("cvMaxAreaSpin")
                if w:
                    try:
                        w.blockSignals(True)
                        w.setValue(int(val))
                        w.blockSignals(False)
                    except Exception:
                        pass
            elif sub == "BGSUB" and val:
                w = _cw("cvBgSubCheck")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)
            elif sub == "BORDFILT" and val:
                w = _cw("cvBordFiltCheck")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)

        elif top == "TM" and len(parts) >= 2:
            sub = parts[1]
            val = parts[2] if len(parts) >= 3 else ""
            if sub == "EN" and val:
                w = self.window.findChild(object, "mlEnabledCheckBox")
                if w:
                    w.blockSignals(True)
                    w.setChecked(val == "1")
                    w.blockSignals(False)

    def _on_serial_error(self, _error) -> None:
        msg = self.serial.error_string()
        if msg and msg != "No error":
            self._log("ERR", msg)

    # ---------------------------------------------------------------- stream
    def _update_stream_status(self, active: bool) -> None:
        self._stream_active = active
        text = "Stream: ● STREAMING" if active else "Stream: idle"
        lbl = self.window.findChild(object, "streamStatusLabel")
        if lbl:
            lbl.setText(text)

    def _start_stream(self) -> None:
        if not self._require_camera_ready("STREAM"):
            return

        self._snap_timeout_timer.stop()
        self._snapshot_in_flight = False
        self._transfer.reset()

        mode = self._w("camModeComboBox").currentText().strip().upper()
        if mode != "JPEG":
            self._log("WARN",
                "STREAM requires JPEG mode — switching to JPEG first")
            cb = self._w("camModeComboBox")
            cb.blockSignals(True)
            cb.setCurrentText("JPEG")
            cb.blockSignals(False)
            self._update_resolution_combo()
            self._send("MODE JPEG")
            self._last_sent_mode = "JPEG"
            QTimer.singleShot(500, lambda: self._send("STREAM 1"))
        else:
            QTimer.singleShot(150, lambda: self._send("STREAM 1"))
        self._update_stream_status(True)

    def _stop_stream(self) -> None:
        self._send("STREAM 0")
        self._update_stream_status(False)

    # ---------------------------------------------------------------- mode/res
    def _on_mode_changed(self) -> None:
        self._update_resolution_combo()
        self._send_camera_config_if_changed()

    def _update_resolution_combo(self) -> None:
        mode  = self._w("camModeComboBox").currentText().strip().upper()
        combo = self._w("camResComboBox")
        valid = JPEG_RESOLUTIONS if mode == "JPEG" else RGB_RESOLUTIONS
        combo.blockSignals(True)
        current = combo.currentText()
        combo.clear()
        combo.addItems(valid)
        idx = combo.findText(current)
        if idx >= 0:
            combo.setCurrentIndex(idx)
        else:
            idx = combo.findText("QVGA")
            combo.setCurrentIndex(max(0, idx))
        combo.blockSignals(False)

    def _send_camera_config_if_changed(
        self, *, force: bool = False, log: bool = True
    ) -> bool:
        if not self.serial.is_open():
            return False
        mode    = self._w("camModeComboBox").currentText().strip().upper()
        res     = self._w("camResComboBox").currentText().strip().upper()
        changed = False
        if force or self._last_sent_mode != mode:
            changed         |= bool(self._send(f"MODE {mode}", log=log))
            self._last_sent_mode = mode
        if force or self._last_sent_res != res:
            changed         |= bool(self._send(f"RES {res}", log=log))
            self._last_sent_res = res
        if changed:
            cam_label = self.window.findChild(object, "camCurrentModeLabel")
            if cam_label:
                cam_label.setText(f"Current: {mode} / {res}")
        return changed

    def _apply_camera_defaults(self, *, log: bool = False) -> None:
        for cmd in ("AWB 1", "BRI 0", "CON 0", "SAT 0", "EFFECT NORMAL"):
            self._send(cmd, log=log)

    # ---------------------------------------------------------------- quality
    def _apply_brightness(self) -> None:
        self._send(
            f"BRI {self._w('briComboBox').currentText().strip()}")

    def _apply_contrast(self) -> None:
        self._send(
            f"CON {self._w('conComboBox').currentText().strip()}")

    def _apply_saturation(self) -> None:
        self._send(
            f"SAT {self._w('satComboBox').currentText().strip()}")

    def _apply_effect(self) -> None:
        self._send(
            f"EFFECT {self._w('effectComboBox').currentText().strip()}")

    def _apply_light(self) -> None:
        self._send(
            f"LIGHT {self._w('lightComboBox').currentText().strip()}")

    def _apply_awb(self) -> None:
        val = "1" if "ON" in self._w("awbComboBox").currentText() else "0"
        self._send(f"AWB {val}")

    def _apply_zoom(self) -> None:
        lvl = self._w("zoomComboBox").currentText().strip()
        if lvl and self._send(f"ZOOM {lvl}"):
            self._last_sent_zoom = lvl

    def _apply_all_quality(self) -> None:
        self._apply_brightness()
        self._apply_contrast()
        self._apply_saturation()
        self._apply_effect()
        self._apply_light()
        self._apply_awb()

    def _reset_quality_defaults(self) -> None:
        defaults = {
            "briComboBox":    "0",
            "conComboBox":    "0",
            "satComboBox":    "0",
            "effectComboBox": "NORMAL",
            "lightComboBox":  "AUTO",
            "awbComboBox":    "ON (1)",
        }
        for name, val in defaults.items():
            w = self.window.findChild(object, name)
            if w:
                idx = w.findText(val)
                if idx >= 0:
                    w.setCurrentIndex(idx)
        self._apply_all_quality()
        self._log("INFO", "Quality settings reset to defaults")

    # ---------------------------------------------------------------- auto CV preview
    def _schedule_auto_cv_preview(self) -> None:
        """
        Debounce-delayed auto-CV-preview after a parameter change.

        Starts (or restarts) a timer.  When it fires, CV RUN is sent if a
        valid RGB565 frame is available, giving immediate visual feedback
        without hammering the STM32 for every slider tick.
        """
        if self._auto_cv_timer is None:
            from PySide6.QtCore import QTimer as _QT
            self._auto_cv_timer = _QT(self.window)
            self._auto_cv_timer.setSingleShot(True)
            self._auto_cv_timer.timeout.connect(self._auto_cv_preview_fire)
        self._auto_cv_timer.start(_AUTO_CV_DEBOUNCE_MS)

    def _auto_cv_preview_fire(self) -> None:
        """Triggered by debounce timer — send CV RUN if a valid frame exists."""
        if not self._stm32_ready:
            return
        if not self._has_valid_rgb565_for_cv():
            return
        self._log("INFO",
            "Auto-preview: re-running CV with updated parameters")
        self._send_cv_commands_only()
