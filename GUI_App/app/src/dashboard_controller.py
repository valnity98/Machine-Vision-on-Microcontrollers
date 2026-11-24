"""Serial connection, camera control and stream handling of the dashboard.

Some rules the code follows:
  - A stream and a snapshot exclude each other. SNAP waits for STREAM OFF.
  - MODE and RES are only sent again when they changed.
  - The controls stay disabled until the STM32 sends "OV2640 ready", or for
    3 seconds at most.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QMessageBox

from app_constants import JPEG_RESOLUTIONS, RGB_RESOLUTIONS
from serial_service import SerialService


class DashboardMixin:
    """Serial connection and camera control methods."""

    # ports
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

    # connect
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
        self._set_conn_label(
            True,
            f"● Connected: {port} @ {baud}  - waiting for STM32")
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
        # Do NOT send GET_STATUS immediately - STM32 may still be booting.
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
        self._stm32_ready    = False
        self._startup_guard_active = False
        self._snapshot_in_flight   = False
        self._stream_active        = False
        self._snap_timeout_timer.stop()
        self._transfer.reset()
        self._rx_buf.clear()
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
            f"● Connected: {self.serial.port_name()} - STM32 ready")
        self._dataset_set_running(False)
        if not was_ready:
            self._log("INFO",
                "STM32 camera is ready - controls enabled")
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
            f"● Connected: {self.serial.port_name()} - STM32 ready")
        self._dataset_set_running(False)
        if not was_ready:
            self._log("INFO", "STM32/OV2640 ready - controls enabled")
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
            f"{action} blocked - waiting for STM32/OV2640 ready.")
        return False

    def _on_baudrate_selection_changed(self) -> None:
        self._update_dataset_baud_hint()
        if not self.serial.is_open():
            return
        current = int(self.serial.serial.baudRate())
        self._log("WARN",
            "Baudrate change blocked while connected - disconnect first.")
        cb = self._w("baudrateComboBox")
        cb.blockSignals(True)
        idx = cb.findText(str(current))
        if idx >= 0:
            cb.setCurrentIndex(idx)
        cb.blockSignals(False)

    # UART send
    def _send(self, cmd: str, log: bool = True) -> bool:
        if not self.serial.is_open():
            self._log("WARN", "Not connected - cannot send command")
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

    # Manual commands that change a widget: command -> (widget name, how to set it)
    _SYNC_SIMPLE = {
        "MODE":   ("camModeComboBox", "text"),
        "RES":    ("camResComboBox", "text"),
        "ZOOM":   ("zoomComboBox", "text"),
        "BRI":    ("briComboBox", "text"),
        "CON":    ("conComboBox", "text"),
        "SAT":    ("satComboBox", "text"),
        "EFFECT": ("effectComboBox", "text"),
        "FLASH":  ("flashCheckBox", "check"),
    }
    _SYNC_CV = {
        "EN":        ("cvEnableCheck", "check"),
        "THRMODE":   ("cvOtsuCheck", "check"),
        "THR":       ("cvThrSlider", "value"),
        "INV":       ("cvInvertCheck", "check"),
        "FILTER":    ("cvFilterCombo", "index"),
        "BLUR":      ("cvBlurSpin", "value"),
        "MORPHMODE": ("cvMorphModeCombo", "index"),
        "MORPH":     ("cvMorphKernelSpin", "value"),
        "CON":       ("cvConnCombo", "text"),
        "MINAREA":   ("cvMinAreaSpin", "value"),
        "MAXAREA":   ("cvMaxAreaSpin", "value"),
        "BGSUB":     ("cvBgSubCheck", "check"),
        "BORDFILT":  ("cvBordFiltCheck", "check"),
    }

    def _set_widget(self, name: str, how: str, val: str) -> None:
        """Set a widget to a value taken from a command, without firing its signals."""
        w = self.window.findChild(object, name)
        if w is None:
            return
        w.blockSignals(True)
        try:
            if how == "check":
                w.setChecked(val == "1")
            elif how == "index":
                w.setCurrentIndex(int(val))
            elif how == "value":
                w.setValue(int(val))
            elif how == "text":
                idx = w.findText(val, Qt.MatchFlag.MatchFixedString)
                if idx >= 0:
                    w.setCurrentIndex(idx)
        except ValueError:
            pass
        finally:
            w.blockSignals(False)

    def _select_item_containing(self, name: str, text: str) -> None:
        """Select the first item of a combo box whose text contains `text`."""
        cb = self.window.findChild(object, name)
        if cb is None:
            return
        for i in range(cb.count()):
            if text in cb.itemText(i).upper():
                cb.setCurrentIndex(i)
                return

    def _sync_ui_after_manual(self, cmd: str) -> None:
        """Update the widgets when a manually typed command is a known setter."""
        parts = cmd.strip().upper().split()
        if len(parts) < 2:
            return
        top, arg = parts[0], parts[1]

        if top in self._SYNC_SIMPLE:
            name, how = self._SYNC_SIMPLE[top]
            self._set_widget(name, how, arg)
            if top == "MODE":
                self._update_resolution_combo()
                self._last_sent_mode = arg
            elif top == "RES":
                self._last_sent_res = arg
        elif top == "LIGHT":
            self._select_item_containing("lightComboBox", arg)
        elif top == "AWB":
            self._select_item_containing("awbComboBox", "ON" if arg == "1" else "OFF")
        elif top == "STREAM":
            self._update_stream_status(arg == "1")
        elif top == "CV" and len(parts) >= 3 and arg in self._SYNC_CV:
            name, how = self._SYNC_CV[arg]
            self._set_widget(name, how, parts[2])
            if arg == "THR":
                lbl = self.window.findChild(object, "cvThrValueLabel")
                if lbl and parts[2].isdigit():
                    lbl.setText(parts[2])
        elif top == "TM" and len(parts) >= 3 and arg == "EN":
            self._set_widget("mlEnabledCheckBox", "check", parts[2])

    def _on_serial_error(self, _error) -> None:
        msg = self.serial.error_string()
        if msg and msg != "No error":
            self._log("ERR", msg)

    # stream
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
                "STREAM requires JPEG mode - switching to JPEG first")
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

    # mode/res
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
        # Show in the boxes what was just sent, otherwise they keep the old
        # choice while the camera runs with the defaults.
        for name, text in (("briComboBox", "0"), ("conComboBox", "0"),
                           ("satComboBox", "0"), ("effectComboBox", "NORMAL"),
                           ("awbComboBox", "ON (1)")):
            w = self.window.findChild(object, name)
            idx = w.findText(text) if w is not None else -1
            if idx >= 0:
                w.blockSignals(True)
                w.setCurrentIndex(idx)
                w.blockSignals(False)

    # quality
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
        if lvl:
            self._send(f"ZOOM {lvl}")

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
