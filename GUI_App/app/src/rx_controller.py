"""
rx_controller.py — UART receive path, binary frame decoding, and protocol dispatch.

Mixin providing all RX-path methods for ReferenceWindowController.
"""

from __future__ import annotations

import re

from PySide6.QtCore import QTimer

from image_utils import jpeg_bytes_to_qimage, rgb565_bytes_to_qimage, gray8_bytes_to_qimage
from app_helpers import FrameTransfer, norm01
from protocol_parser import (
    TINYML_INPUT_DESC, TmResult,
    parse_cvcfg, parse_cvbox, parse_cvstat,
    parse_tmcfg, parse_tminfo, parse_tmprob, parse_tmres,
    try_parse_image_header,
)


class RxMixin:
    """UART receive path, binary frame decoding, and firmware protocol dispatch."""

    # ---------------------------------------------------------------- RX entry
    def _on_ready_read(self) -> None:
        """Qt slot: called when new bytes arrive on the serial port."""
        data = self.serial.read_bytes()
        if data:
            self._rx_buf.extend(data)
            self._drain()

    def _drain(self) -> None:
        """
        Process all buffered bytes.

        While a binary image transfer is active, bytes are consumed into the
        transfer buffer.  Otherwise the buffer is scanned for newlines and
        each complete text line is forwarded to _process_line().
        """
        while True:
            if self._transfer.active:
                if not self._consume_binary():
                    return
                continue
            nl = self._rx_buf.find(b"\n")
            if nl < 0:
                return
            raw  = self._rx_buf[:nl]
            del self._rx_buf[:nl + 1]
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                self._process_line(line)

    def _consume_binary(self) -> bool:
        """
        Append bytes from _rx_buf into the active image transfer buffer.

        Returns True when the transfer is complete, False when more bytes
        are needed.
        """
        remaining = (self._transfer.expected_size
                     - len(self._transfer.received))
        if remaining <= 0:
            self._finish_frame()
            return True
        if not self._rx_buf:
            return False
        take = min(remaining, len(self._rx_buf))
        self._transfer.received.extend(self._rx_buf[:take])
        del self._rx_buf[:take]
        if self._transfer.complete:
            self._finish_frame()
            return True
        return False

    # ---------------------------------------------------------------- frame decode
    def _finish_frame(self) -> None:
        """
        Decode and display a completed image transfer.

        The stream_active flag is NOT touched here — only _start_stream()
        and _stop_stream() (in dashboard_controller) manage it.
        """
        fmt = self._transfer.fmt
        w   = self._transfer.width
        h   = self._transfer.height
        raw = bytes(self._transfer.received)
        self._transfer.reset()
        self._snapshot_in_flight = False
        self._snap_timeout_timer.stop()
        self._frame_count += 1

        if fmt == "JPEG":
            clean = FrameTransfer.extract_jpeg_safe(raw)
            if clean is None:
                self._log("WARN",
                    f"Frame #{self._frame_count}: JPEG has no valid "
                    f"FFD8…FFD9 — discarded")
                return
            qimg = jpeg_bytes_to_qimage(clean)
        elif fmt == "RGB565":
            qimg = rgb565_bytes_to_qimage(raw, w, h)
        elif fmt == "GRAY":
            qimg = gray8_bytes_to_qimage(raw, w, h)
        else:
            self._log("WARN", f"Unknown frame format: {fmt}")
            return

        if qimg.isNull():
            self._log("WARN",
                f"Frame #{self._frame_count}: could not decode "
                f"{fmt} frame")
            return

        # Store decoded frame state
        self._last_qimg      = qimg.copy()
        self._last_raw_bytes = raw
        self._last_format    = fmt
        self._current_image_folder = None
        self._current_image_index += 1
        # Clear stale CV result when a new frame arrives
        self._cv_result = None
        self._cv_pending_boxes.clear()
        self._update_cv_idle_display()

        self._last_fmt_res = f"{fmt} | {qimg.width()}×{qimg.height()}"
        self._w("imgFormatLabel").setText(
            f"Format: {fmt}   {qimg.width()}×{qimg.height()}")
        self._w("imgSizeLabel").setText(f"Size: {len(raw):,} bytes")
        self._w("frameInfoLabel").setText(
            f"Frame #{self._frame_count}  |  {fmt}  |  "
            f"{qimg.width()}×{qimg.height()}  |  {len(raw):,} B"
        )

        self._refresh_preview()
        self._log("INFO",
            f"Frame #{self._frame_count}: {fmt} "
            f"{qimg.width()}×{qimg.height()} ({len(raw):,} B)")

        self._dataset_on_new_frame()

        if self._cv_run_after_next_rgb_frame:
            self._cv_run_after_next_rgb_frame = False
            if self._has_valid_rgb565_for_cv():
                QTimer.singleShot(120, self._send_cv_commands_only)
            else:
                self._log("ERR",
                    f"CV auto-run cancelled: frame format/size not "
                    f"supported ({fmt} {w}×{h}). "
                    f"Supported: RGB565/GRAY up to 640×480.")

        if self._tm_run_after_next_rgb_frame:
            self._tm_run_after_next_rgb_frame = False
            if self._has_valid_rgb565_for_tinyml():
                QTimer.singleShot(150, lambda: self._send("TM RUN"))
            else:
                self._log("ERR",
                    f"TinyML auto-run cancelled: frame format/size not "
                    f"supported ({fmt} {w}×{h}). "
                    f"Supported: RGB565/GRAY up to 640×480.")

    # ---------------------------------------------------------------- protocol dispatch
    def _process_line(self, line: str) -> None:
        """
        Dispatch a single UART text line to the appropriate handler.

        Order:
          1. Image header          → start binary transfer
          2. Binary garbage guard  → resync on corrupt data
          3. OV2640 ready          → enable controls
          4. STREAM ON/OFF         → update stream state
          5. CVCFG/CVSTAT/CVBOX/CVDONE  → CV result
          6. TMCFG/TMINFO/TMRES/TMPROB/TMDONE  → TinyML result
          7. STAT:                 → firmware stats
          8. INFO:/WARN:/ERR:/DEBUG:  → firmware log
          9. Else                  → raw RX log
        """
        hdr = try_parse_image_header(line)
        if hdr is not None:
            self._transfer.start(hdr)
            self._snapshot_in_flight = True
            self._snap_timeout_timer.start(self._frame_timeout_ms)
            return

        # Garbage guard runs before any state-changing checks to avoid
        # spurious state transitions caused by binary noise that happens to
        # contain "STREAM ON" or similar substrings.
        if self._looks_like_binary_garbage(line):
            self._log("WARN",
                "Binary garbage detected in text stream — resynchronising")
            self._recover_from_protocol_desync()
            return

        if "OV2640 ready" in line or "OV2640 READY" in line:
            self._saw_valid_stm32_line = True
            self._mark_stm32_ready(send_initial_status=False)

        if "STREAM ON" in line:
            self._update_stream_status(True)
        elif "STREAM OFF" in line:
            self._update_stream_status(False)
            # Execute actions that were queued while waiting for stream to stop
            pending = getattr(self, "_pending_after_stream_off", [])
            if pending:
                self._pending_after_stream_off = []
                for fn in pending:
                    QTimer.singleShot(50, fn)

        if line.startswith("CVCFG:"):
            cfg = parse_cvcfg(line)
            self._stm32_cv_cfg = cfg
            self._apply_cvcfg_to_ui(cfg)
            self._log(
                "STM32-CVCFG",
                f"EN={cfg.enabled} PRESET={cfg.preset} "
                f"THR={cfg.threshold} "
                f"MODE={'OTSU' if cfg.threshold_mode else 'MANUAL'} "
                f"INV={cfg.invert} CON={cfg.connectivity} "
                f"FILTER={cfg.filter_mode} BLUR={cfg.blur_kernel} "
                f"MORPH={cfg.morph_mode} MORPHK={cfg.morph_kernel} "
                f"MIN={cfg.min_area} MAX={cfg.max_area}",
            )
            return

        if line.startswith("CVSTAT:"):
            self._cv_result = parse_cvstat(line)
            self._cv_pending_boxes.clear()
            return

        if line.startswith("CVBOX:"):
            self._cv_pending_boxes.append(parse_cvbox(line))
            return

        if line.startswith("CVDONE"):
            if self._cv_result is not None:
                self._cv_result.boxes = list(self._cv_pending_boxes)
            # Clear in-progress guard so auto-preview can fire after CVDONE
            self._cv_run_in_progress = False
            self._update_cv_display()
            self._update_cv_result_ui()
            self._refresh_preview()
            count = (self._cv_result.count if self._cv_result else "?")
            self._log("INFO", f"STM32 CV done — count={count}")
            return

        if line.startswith("TMCFG:"):
            model, inp = parse_tmcfg(line)
            self._set_compact_label("mlModelLabel", model, max_chars=32)
            self._set_compact_label("mlInputLabel", inp,   max_chars=16)
            if inp != TINYML_INPUT_DESC:
                self._log("WARN",
                    f"TinyML input '{inp}' ≠ expected '{TINYML_INPUT_DESC}'. "
                    "Re-import count_model.tflite into X-CUBE-AI.")
            return

        if line.startswith("TMINFO:"):
            status, ram, flash = parse_tminfo(line)
            if self._tm_pending is None:
                self._tm_pending = TmResult()
            self._tm_pending.runtime_status = status
            self._tm_pending.ram_kb   = ram
            self._tm_pending.flash_kb = flash
            self._set_compact_label("mlRuntimeLabel",  status, max_chars=28)
            mem_lbl = self.window.findChild(object, "mlMemValueLabel")
            if mem_lbl:
                mem_lbl.setText(f"RAM {ram} KB / Flash {flash} KB")
            return

        if line.startswith("TMRES:"):
            name, idx, conf, t_ms, is_unc = parse_tmres(line)
            self._tm_pending = TmResult(
                predicted_name    = name,
                predicted_index   = idx,
                confidence        = norm01(conf),
                is_uncertain      = is_unc,
                inference_time_ms = t_ms,
            )
            return

        if line.startswith("TMPROB:"):
            if self._tm_pending is not None:
                pidx, pname, pscore = parse_tmprob(line)
                self._tm_pending.scores[pname] = norm01(pscore)
            return

        if line.startswith("TMDONE"):
            if self._tm_pending is not None:
                self._tm_result  = self._tm_pending
                self._tm_pending = None
            self._update_ml_display()
            pred = (self._tm_result.predicted_name if self._tm_result else "?")
            self._log("INFO", f"STM32 TinyML done — {pred}")
            return

        if line.startswith("STAT:"):
            self._saw_valid_stm32_line = True
            if not self._stm32_ready:
                self._mark_stm32_ready(send_initial_status=False)
            self._handle_stat_line(line)

        if line.startswith(("INFO:", "WARN:", "ERR:", "STAT:", "DEBUG:")):
            level = line.split(":", 1)[0]
            msg   = line[len(level) + 1:].strip()
            if level in ("WARN", "ERR", "STAT"):
                self._log_diag(f"{level}: {msg}")
            self._log(f"STM32-{level}", msg)
            return

        self._log("RX", line)

    def _handle_stat_line(self, line: str) -> None:
        """Parse STAT: key=value pairs and update Dashboard + Benchmark labels."""
        if not line.startswith("STAT:"):
            return
        kv    = dict(re.findall(r"([A-Z_]+)=([^\s]+)", line))
        parts = []
        if kv.get("FPS"):  parts.append(f"FPS {kv['FPS']}")
        if kv.get("SIZE"): parts.append(f"Size {kv['SIZE']}")
        if kv.get("HEAP") or kv.get("FB"):
            parts.append(f"Heap {kv.get('HEAP','-')} | FB {kv.get('FB','-')}")
        if kv.get("LAT"):  parts.append(f"Latency {kv['LAT']}")
        self._w("statLabel").setText(
            "   |   ".join(parts) if parts else "—")
        for fw_key, lbl_name in [
            ("FPS",  "benchFpsLabel"),
            ("LAT",  "benchLatLabel"),
            ("SIZE", "benchSizeLabel"),
            ("HEAP", "benchHeapLabel"),
            ("FB",   "benchFbLabel"),
        ]:
            val = kv.get(fw_key)
            lbl = self.window.findChild(object, lbl_name)
            if lbl and val:
                lbl.setText(val)

    # ---------------------------------------------------------------- garbage / desync
    def _looks_like_binary_garbage(self, line: str) -> bool:
        """Heuristic: return True if the line appears to be raw binary data."""
        valid_prefixes = (
            "INFO:", "WARN:", "ERR:", "ERROR:", "STAT:", "DEBUG:",
            "JPG:", "RGB565:", "GRAY:",
            "CVCFG:", "CVSTAT:", "CVBOX:", "CVDONE",
            "TMCFG:", "TMINFO:", "TMRES:", "TMPROB:", "TMDEBUG:", "TMDONE",
        )
        if line.startswith(valid_prefixes):
            return False
        boot_prefixes = (
            "Device", "Core Arch", "HAL version", "SYSCLK clock",
            "HCLK clock", "FLASH conf", "CACHE conf", "Timestamp",
            "MPU", "DCMI", "I2C", "Camera", "OV2640",
            "Use local GUI", "uration",
        )
        if line.startswith(boot_prefixes):
            return False
        if "\ufffd" in line and len(line) > 64:
            return True
        if len(line) < 16:
            return False
        sample = line[:256]
        bad = sum(
            1 for ch in sample
            if ord(ch) < 32 and ch not in "\t\r\n")
        return (bad / max(1, len(sample))) > 0.10

    def _recover_from_protocol_desync(self) -> None:
        """Reset all transfer state after a UART framing error."""
        self._snap_timeout_timer.stop()
        self._snapshot_in_flight   = False
        self._transfer.reset()
        self._rx_buf.clear()
        self._dataset_pending_save = False
        if self._dataset_timer.isActive():
            self._dataset_timer.stop()
        self._dataset_set_running(False)
        self._dataset_update_status()
        self._log("WARN", "UART parser reset — wait a moment then retry.")
