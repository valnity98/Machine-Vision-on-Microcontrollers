"""
bench_controller.py — Benchmark run history, table refresh, eval label updates, CSV export.

Mixin providing all benchmark/evaluation methods for ReferenceWindowController.
"""

from __future__ import annotations

import csv
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QMessageBox, QPlainTextEdit, QTableWidgetItem

from app_constants import BENCH_KEYS
from app_helpers import is_tm_uncertain, timestamp


class BenchMixin:
    """Benchmark run history, accuracy/latency evaluation, CSV export, diagnostics."""

    # ---------------------------------------------------------------- add / delete run
    def _add_bench_run(self) -> None:
        """Append the current CV + TinyML results as a new benchmark row."""
        if self._cv_result is None and self._tm_result is None:
            QMessageBox.information(
                self.window, "Benchmark",
                "No results available.  Run STM32 CV and/or TinyML first.",
            )
            return

        cv_count = self._cv_result.count              if self._cv_result else None
        cv_ms    = self._cv_result.processing_time_ms if self._cv_result else None
        ml_cls   = self._tm_result.predicted_name     if self._tm_result else None
        ml_idx   = self._tm_result.predicted_index    if self._tm_result else None
        ml_ms    = self._tm_result.inference_time_ms  if self._tm_result else None
        unc      = is_tm_uncertain(self._tm_result)

        gt_raw   = self._w("gtComboBox").currentText().strip()
        gt_count = None if gt_raw == "—" else int(gt_raw)

        def _match(value, gt) -> str:
            if value is None or gt is None:
                return "—"
            return "MATCH" if value == gt else "MISMATCH"

        row = {
            "time":      datetime.now().strftime("%H:%M:%S"),
            "frame_fmt": self._last_fmt_res,
            "cv_count":  str(cv_count) if cv_count is not None else "—",
            "cv_ms":     str(cv_ms)    if cv_ms    is not None else "—",
            "ml_class":  (ml_cls or "—") + (" (?)" if unc else ""),
            "ml_ms":     str(ml_ms)    if ml_ms    is not None else "—",
            "gt_count":  str(gt_count) if gt_count is not None else "—",
            "cv_vs_gt":  _match(cv_count, gt_count),
            "ml_vs_gt":  "UNCERTAIN" if unc else _match(ml_idx, gt_count),
        }
        self._bench_rows.append(row)
        self._refresh_bench_table()
        self._update_bench_summary()
        self._update_eval_labels()

    def _delete_last_bench_run(self) -> None:
        if not self._bench_rows:
            QMessageBox.information(self.window, "Benchmark", "No rows to delete.")
            return
        self._bench_rows.pop()
        self._refresh_bench_table()
        self._update_bench_summary()
        self._update_eval_labels()
        self._log("INFO", "Last benchmark run deleted")

    def _clear_bench(self) -> None:
        self._bench_rows.clear()
        self._w("benchTable").setRowCount(0)
        self._update_bench_summary()
        self._update_eval_labels()

    # ---------------------------------------------------------------- table refresh
    def _refresh_bench_table(self) -> None:
        tbl = self._w("benchTable")
        tbl.setRowCount(len(self._bench_rows))
        for r_idx, row in enumerate(self._bench_rows):
            for c_idx, key in enumerate(BENCH_KEYS):
                val  = row.get(key, "—")
                item = QTableWidgetItem(val)
                item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
                if   val == "MATCH":     item.setForeground(QColor("#039855"))
                elif val == "MISMATCH":  item.setForeground(QColor("#d92d20"))
                elif val == "UNCERTAIN": item.setForeground(QColor("#b45309"))
                tbl.setItem(r_idx, c_idx, item)
        tbl.scrollToBottom()

    # ---------------------------------------------------------------- summary labels
    def _update_bench_summary(self) -> None:
        n    = len(self._bench_rows)
        cv_m = sum(1 for r in self._bench_rows if r["cv_vs_gt"] == "MATCH")
        cv_x = sum(1 for r in self._bench_rows if r["cv_vs_gt"] == "MISMATCH")
        ml_m = sum(1 for r in self._bench_rows if r["ml_vs_gt"] == "MATCH")
        ml_x = sum(1 for r in self._bench_rows if r["ml_vs_gt"] == "MISMATCH")
        ml_u = sum(1 for r in self._bench_rows if r["ml_vs_gt"] == "UNCERTAIN")
        lbl  = self.window.findChild(object, "benchSummaryLabel")
        if lbl:
            lbl.setText(
                f"Runs: {n}  |  CV {cv_m}✓/{cv_x}✗"
                f"  |  TinyML {ml_m}✓/{ml_x}✗"
                + (f"/{ml_u}?" if ml_u > 0 else "")
            )

    def _update_eval_labels(self) -> None:
        n    = len(self._bench_rows)
        cv_v = [r for r in self._bench_rows if r["cv_vs_gt"] in ("MATCH", "MISMATCH")]
        cv_m = sum(1 for r in cv_v if r["cv_vs_gt"] == "MATCH")
        cv_a = f"{cv_m/len(cv_v)*100:.0f}% ({cv_m}/{len(cv_v)})" if cv_v else "—"

        ml_v = [r for r in self._bench_rows if r["ml_vs_gt"] in ("MATCH", "MISMATCH")]
        ml_m = sum(1 for r in ml_v if r["ml_vs_gt"] == "MATCH")
        ml_a = f"{ml_m/len(ml_v)*100:.0f}% ({ml_m}/{len(ml_v)})" if ml_v else "—"

        cv_ts  = [float(r["cv_ms"]) for r in self._bench_rows
                  if r["cv_ms"].replace(".", "").isdigit()]
        ml_ts  = [float(r["ml_ms"].replace(" (?)", "")) for r in self._bench_rows
                  if r["ml_ms"].replace(" (?)", "").replace(".", "").isdigit()]
        cv_lat = f"{sum(cv_ts)/len(cv_ts):.0f} ms" if cv_ts else "—"
        ml_lat = f"{sum(ml_ts)/len(ml_ts):.0f} ms" if ml_ts else "—"
        ml_unc = sum(1 for r in self._bench_rows if r["ml_vs_gt"] == "UNCERTAIN")

        for label_name, val in [
            ("evalRunsLabel",  f"Runs: {n}"),
            ("evalCVAccLabel", f"CV Acc: {cv_a}"),
            ("evalMLAccLabel", f"ML Acc: {ml_a}"),
            ("evalCVLatLabel", f"CV Ø: {cv_lat}"),
            ("evalMLLatLabel", f"ML Ø: {ml_lat}"),
            ("evalUncLabel",   f"Uncertain: {ml_unc}"),
        ]:
            w = self.window.findChild(object, label_name)
            if w:
                w.setText(val)

    def _set_eval_last_save(self, path_str: str) -> None:
        w = self.window.findChild(object, "evalLastSaveLabel")
        if w:
            try:
                parts = Path(path_str).parts
                short = "/".join(parts[-2:]) if len(parts) >= 2 else path_str
            except Exception:
                short = path_str
            w.setText(f"Last save: {short}")
            w.setToolTip(path_str)

    # ---------------------------------------------------------------- CSV export
    def _export_csv(self) -> None:
        if not self._bench_rows:
            QMessageBox.information(self.window, "Export", "No data to export.")
            return
        out  = self._image_sub("benchmark")
        path = out / f"benchmark_{timestamp()}.csv"
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=BENCH_KEYS)
            writer.writeheader()      # write column names as first row
            writer.writerows(self._bench_rows)
        rel = self._relative_to_project(path)
        self._log("INFO", f"Exported {len(self._bench_rows)} rows → {rel}")
        self._set_eval_last_save(str(path.parent))
        self._update_eval_labels()

    # ---------------------------------------------------------------- diagnostics log
    def _log_diag(self, msg: str) -> None:
        """Append a message to the Benchmark diagnostics log."""
        diag = self.window.findChild(QPlainTextEdit, "benchDiagEdit")
        if diag:
            ts = datetime.now().strftime("%H:%M:%S")
            diag.appendPlainText(f"[{ts}] {msg}")

    def _clear_diag(self) -> None:
        diag = self.window.findChild(QPlainTextEdit, "benchDiagEdit")
        if diag:
            diag.clear()
