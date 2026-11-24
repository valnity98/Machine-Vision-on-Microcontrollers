"""Main window of the dashboard.

Loads the Qt Designer layout (reference_window.ui), connects the widgets and
puts together the controller from the mixins in the other modules. The tabs are
Dashboard (preview, serial connection, log), Vision (gray, binary and overlay
images, TinyML, CV controls) and Benchmark (timing and accuracy per run).
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QIcon
from PySide6.QtUiTools import QUiLoader
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFrame,
    QHeaderView, QLineEdit,
    QMainWindow, QSizePolicy, QTableWidgetItem,
)

# Modular imports
from app_constants import (
    BENCH_LABELS, BOX_LABELS, LOG_MAX_LINES,
    FRAME_TIMEOUT_MS, DEFAULT_BAUDRATE,
    STYLESHEET,
)
from app_helpers import (
    application_root, resource_path, ui_path, timestamp,
    FrameTransfer,
)
from dashboard_controller import DashboardMixin
from rx_controller        import RxMixin
from dataset_controller   import DatasetMixin
from display_controller   import DisplayMixin
from save_controller      import SaveMixin
from bench_controller     import BenchMixin
from serial_service       import SerialService
from protocol_parser import (
    CvConfig, CvResult, TmResult,
    TINYML_CLASSES, TINYML_INPUT_DESC,
)


class ReferenceWindowController(
    DashboardMixin,
    RxMixin,
    DatasetMixin,
    BenchMixin,
    DisplayMixin,
    SaveMixin,
):
    """
    Top-level controller for the STM32 Edge Vision Dashboard.

    All CV pipeline controls are driven directly from static .ui widgets.
    No ClassicalVisionPanel instantiation or dynamic widget injection.
    """

    _DEFAULT_BAUDRATE     = DEFAULT_BAUDRATE

    # init
    def __init__(self) -> None:
        # Load Qt UI
        # Use QFile to open the .ui file - passing a raw path string to
        # QUiLoader.load() fails on Windows when the path contains spaces or
        # backslashes (known Qt/PySide6 issue on Windows).
        from PySide6.QtCore import QFile, QIODevice
        ui_file = QFile(str(ui_path()))
        if not ui_file.open(QIODevice.OpenModeFlag.ReadOnly):
            raise RuntimeError(
                f"Cannot open UI file: {ui_path()}\n"
                f"Make sure reference_window.ui is in the same folder as the Python scripts.")
        loader = QUiLoader()
        self.window: QMainWindow = loader.load(ui_file, None)
        ui_file.close()
        if self.window is None:
            raise RuntimeError(
                f"QUiLoader failed to parse: {ui_path()}\n"
                f"Error: {loader.errorString()}")
        self.window.setStyleSheet(STYLESHEET)
        icon = resource_path("assets", "app_icon.ico")
        if icon.exists():
            self.window.setWindowIcon(QIcon(str(icon)))

        # Output / dataset directories
        self._project_root = application_root()
        self._outputs_root = self._project_root / "outputs"

        # Serial service
        self.serial = SerialService(self.window)

        # UART receive state
        self._rx_buf   = bytearray()
        self._transfer = FrameTransfer()

        # Snapshot / stream / connection guards
        self._snapshot_in_flight   = False
        self._stream_active        = False
        self._frame_timeout_ms     = FRAME_TIMEOUT_MS
        self._snap_timeout_timer   = QTimer(self.window)
        self._snap_timeout_timer.setSingleShot(True)
        self._snap_timeout_timer.timeout.connect(self._on_snapshot_timeout)
        self._stm32_ready          = False
        self._startup_guard_active = False

        # Frame state
        self._frame_count                  = 0
        self._last_fmt_res                 = "—"
        self._last_qimg                    = None
        self._last_raw_bytes: bytes        = b""
        self._last_format                  = "—"
        self._last_sent_mode: str | None   = None
        self._last_sent_res:  str | None   = None

        # CV result state
        self._cv_result:        CvResult | None = None
        self._cv_pending_boxes: list            = []
        self._stm32_cv_cfg:     CvConfig | None = None

        # TinyML result state
        self._tm_result:  TmResult | None = None
        self._tm_pending: TmResult | None = None

        # Auto-run flags (set only by dataset capture)
        self._tm_run_after_next_rgb_frame = False
        self._cv_run_after_next_rgb_frame = False

        # Auto-preview debounce timer (initialised on first use by DashboardMixin)

        # Pending callables executed when firmware confirms STREAM OFF
        self._pending_after_stream_off: list = []

        # CV RUN in-progress guard: prevents starting a new auto-preview while
        # waiting for CVDONE from the last CV RUN

        # Benchmark state
        self._bench_rows: list[dict] = []

        # Export / image folder tracking
        self._current_image_folder: Path | None = None
        self._current_image_index:  int         = 0

        # Dataset capture state
        # Default dataset root: ml_training/dataset at the repo root (3 levels above src/).
        # In a frozen build the project root can be too shallow for that; fall back
        # to a "dataset" folder next to the executable instead of raising IndexError.
        _root_parents = self._project_root.parents
        if len(_root_parents) > 2:
            self._dataset_default_root = _root_parents[2] / "ml_training" / "dataset"
        else:
            self._dataset_default_root = self._project_root / "dataset"
        self._dataset_timer         = QTimer(self.window)
        self._dataset_timer.setSingleShot(True)
        self._dataset_timer.timeout.connect(self._dataset_next_capture)
        self._dataset_target_count  = 0
        self._dataset_saved_count   = 0
        self._dataset_pending_save  = False
        self._dataset_pending_frame = 0

        # Build and wire UI
        self._setup_ui()
        self._connect_signals()
        self._refresh_ports()
        self._set_controls_enabled(False)
        self._log("INFO", "STM32 Edge Vision Dashboard ready - connect STM32 via UART")

    # Setup
    def _setup_ui(self) -> None:
        """Initialise widget properties not set in the .ui file."""
        self.window.setMinimumSize(1024, 650)
        screen = QApplication.primaryScreen()
        if screen:
            avail = screen.availableGeometry()
            w     = min(1560, max(1024, avail.width()  - 60))
            h     = min(920,  max(650,  avail.height() - 60))
            self.window.resize(w, h)
            self.window.move(
                avail.x() + max(0, (avail.width()  - w) // 2),
                avail.y() + max(0, (avail.height() - h) // 2),
            )

        # Camera preview image label
        lbl = self._w("imageLabel")
        lbl.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        lbl.setMinimumSize(320, 200)
        lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)

        # Baudrate combo default
        baud_cb = self._w("baudrateComboBox")
        idx = baud_cb.findText(self._DEFAULT_BAUDRATE)
        if idx >= 0:
            baud_cb.setCurrentIndex(idx)

        # Resolution combo
        self._update_resolution_combo()

        # TinyML score bars
        for i in range(len(TINYML_CLASSES)):
            for name, mw, mh in [
                (f"sc{i}KeyLabel", 72, 24),
                (f"sc{i}ValLabel", 56, 24),
            ]:
                widget = self.window.findChild(object, name)
                if widget:
                    widget.setMinimumWidth(mw)
                    widget.setMinimumHeight(mh)
            bar = self.window.findChild(object, f"sc{i}Bar")
            if bar:
                bar.setRange(0, 1000)
                bar.setMinimumHeight(18)
                bar.setMaximumHeight(22)

        # Benchmark table
        tbl = self._w("benchTable")
        tbl.setColumnCount(len(BENCH_LABELS))
        tbl.setHorizontalHeaderLabels(BENCH_LABELS)
        tbl.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Interactive)
        tbl.horizontalHeader().setMinimumSectionSize(52)
        tbl.setEditTriggers(tbl.EditTrigger.NoEditTriggers)
        tbl.setSelectionBehavior(tbl.SelectionBehavior.SelectRows)
        tbl.setAlternatingRowColors(True)
        tbl.verticalHeader().hide()
        tbl.horizontalHeader().setFixedHeight(32)
        tbl.verticalHeader().setDefaultSectionSize(24)
        tbl.setMinimumHeight(120)
        tbl.setMaximumHeight(220)
        tbl.setFrameShape(QFrame.Shape.Box)
        for col, cw in enumerate([68, 90, 70, 60, 120, 60, 70, 80, 100]):
            tbl.setColumnWidth(col, cw)
        tbl.horizontalHeader().setStretchLastSection(True)

        # CV bounding-box table (static in .ui, configure column widths here)
        cv_tbl = self.window.findChild(object, "cvBoxTable")
        if cv_tbl:
            cv_tbl.setEditTriggers(cv_tbl.EditTrigger.NoEditTriggers)
            cv_tbl.setSelectionBehavior(cv_tbl.SelectionBehavior.SelectRows)
            cv_tbl.verticalHeader().hide()
            cv_tbl.horizontalHeader().setFixedHeight(28)
            cv_tbl.verticalHeader().setDefaultSectionSize(22)
            cv_tbl.setFrameShape(QFrame.Shape.Box)
            cv_tbl.setColumnCount(len(BOX_LABELS))
            cv_tbl.setHorizontalHeaderLabels(BOX_LABELS)
            for col, cw in enumerate([36, 72, 52, 52, 52, 52, 80, 76]):
                cv_tbl.setColumnWidth(col, cw)
            cv_tbl.horizontalHeader().setStretchLastSection(True)

        # CV connectivity combo - set default to "8"
        conn_cb = self.window.findChild(object, "cvConnCombo")
        if conn_cb:
            idx = conn_cb.findText("8")
            if idx >= 0:
                conn_cb.setCurrentIndex(idx)

        # ROI spins: disable until ROI is enabled
        for name in ("cvRoiXSpin", "cvRoiYSpin", "cvRoiWSpin", "cvRoiHSpin"):
            w = self.window.findChild(object, name)
            if w:
                w.setEnabled(False)

        # Constrain the Last Result Summary group box so it never forces the
        # left column wider than the Camera Preview (max 640 px).  Without this
        # the sum of all key+value labels pushes the group's preferred width to
        # ~800 px, which compresses the right column and clips Mode_Resolution.
        result_gb = self.window.findChild(object, "quickResultGroupBox")
        if result_gb:
            result_gb.setMaximumWidth(640)
            layout = result_gb.layout()
            if layout:
                layout.setSpacing(6)

        # Also cap individual value labels so elision kicks in for long strings.
        for lbl_name, max_w in [
            ("mlPredValueLabel",   100),
            ("mlConfValueLabel",    95),
            ("cvBoxesValueLabel",   80),
            ("cvBrightValueLabel",  44),
            ("cvTimeValueLabel",    60),
            ("cvCountValueLabel",   60),
        ]:
            w = self.window.findChild(object, lbl_name)
            if w:
                w.setMaximumWidth(max_w)

        # Dataset UI defaults
        root_le = self.window.findChild(QLineEdit, "dsRootLineEdit")
        if root_le:
            root_le.setText(str(self._dataset_default_root))
        self._update_dataset_baud_hint()
        self._dataset_update_status()
        self._dataset_set_running(False)

        # Initial display state
        self._update_bench_summary()
        ml_inp = self.window.findChild(object, "mlInputLabel")
        if ml_inp:
            ml_inp.setText(TINYML_INPUT_DESC)

        # Inject mode status banner labels into Vision tab dynamically
        # These show "JPEG → CV/TM disabled" or "RGB → ready" at all times
        self._inject_mode_banners()

    # Signal wiring
    def _connect_signals(self) -> None:
        """Wire all Qt signals to their handler slots."""

        def ci(widget_name: str, signal_name: str, slot) -> None:
            """Connect a named widget signal to a slot, silently skip if missing."""
            widget = self.window.findChild(object, widget_name)
            if widget is None:
                return
            getattr(widget, signal_name).connect(slot)

        # Serial / connection
        ci("btnRefreshPorts",       "clicked",       self._refresh_ports)
        ci("btnConnect",            "clicked",       self._toggle_connect)
        ci("btnSendManual",         "clicked",       self._send_manual)
        ci("manualCommandLineEdit", "returnPressed", self._send_manual)
        ci("commandPresetComboBox", "activated",     lambda _i: self._insert_manual_command_template())

        # Dashboard camera
        ci("btnSnap",         "clicked", self._snap)
        ci("btnStartStream",  "clicked", lambda: self._start_stream())
        ci("btnStopStream",   "clicked", lambda: self._stop_stream())
        ci("btnGetStatus",    "clicked", lambda: self._send("GET_STATUS"))
        ci("flashCheckBox",   "stateChanged",
           lambda state: self._send(f"FLASH {'1' if state else '0'}", log=True))

        # Save / log
        ci("btnSaveFrame",        "clicked", self._save_current_frame_clicked)
        ci("btnSaveDebugImages",  "clicked", self._save_debug_images_clicked)
        ci("btnSaveLog",          "clicked", self._save_log_clicked)
        ci("btnClearLog",         "clicked", lambda: self._w("logEdit").clear())

        # Camera Control
        ci("camModeComboBox",   "currentIndexChanged", lambda _i: self._on_mode_changed())
        ci("camResComboBox",    "currentIndexChanged", lambda _i: self._send_camera_config_if_changed())
        ci("btnApplyCamMode",   "clicked",             lambda: self._send_camera_config_if_changed(force=True))
        ci("baudrateComboBox",  "currentIndexChanged", lambda _i: self._on_baudrate_selection_changed())

        # Image quality
        # Quality combos: use "activated" signal (only fires on USER interaction, not programmatic changes)
        ci("briComboBox",    "activated", lambda _: self._apply_brightness())
        ci("conComboBox",    "activated", lambda _: self._apply_contrast())
        ci("satComboBox",    "activated", lambda _: self._apply_saturation())
        ci("effectComboBox", "activated", lambda _: self._apply_effect())
        ci("lightComboBox",  "activated", lambda _: self._apply_light())
        ci("awbComboBox",    "activated", lambda _: self._apply_awb())
        ci("zoomComboBox",   "activated", lambda _: self._apply_zoom())
        ci("btnApplyAllQuality", "clicked", self._apply_all_quality)
        ci("btnResetQuality",    "clicked", self._reset_quality_defaults)

        # Dataset
        ci("dsBrowseButton",  "clicked", self._dataset_browse_root)
        ci("dsSaveButton",    "clicked", self._dataset_save_current_clicked)
        ci("dsCaptureButton", "clicked", self._dataset_capture_n_clicked)
        ci("dsStopButton",    "clicked", self._dataset_stop)
        ds_root = self.window.findChild(QLineEdit, "dsRootLineEdit")
        if ds_root:
            ds_root.textChanged.connect(self._dataset_update_status)
        for wname in ("dsSplitCombo", "dsLabelCombo", "dsCountSpin"):
            w = self.window.findChild(object, wname)
            if not w:
                continue
            if hasattr(w, "currentIndexChanged"):   # the combo boxes
                w.currentIndexChanged.connect(lambda _i: self._dataset_update_status())
            else:                                   # the spin box
                w.valueChanged.connect(lambda _v: self._dataset_update_status())

        # CV Pipeline - action buttons
        ci("cvBtnGet",       "clicked", lambda: self._send("CV GET"))
        ci("btnRunCV",       "clicked", self._run_cv)
        ci("cvBtnBgCap",     "clicked", lambda: self._send("CV BGCAP"))
        ci("btnSnapAndRunCV","clicked", self._snap_and_run_cv)
        ci("btnSaveCVResult","clicked", self._save_cv_result_clicked)

        # CV Pipeline - enable / preset
        ci("cvEnableCheck",    "toggled", self._cv_send_enable)
        # Preset combo: activated fires only on user interaction, not programmatic index changes
        ci("cvPresetCombo", "activated", lambda _: self._cv_send_preset())

        # CV Pipeline - threshold
        ci("cvThrSlider",  "valueChanged", self._cv_thr_slider_changed)
        ci("cvOtsuCheck",  "stateChanged",  lambda _: self._refresh_preview())
        ci("cvInvertCheck","stateChanged",  lambda _: self._refresh_preview())

        # CV Pipeline - filter / morph
        # CV Pipeline - area / shape
        # CV Pipeline - BG / border / ROI
        ci("cvBgSubCheck",      "stateChanged", lambda _: self._cv_send_bgsub())
        ci("cvBordFiltCheck",   "stateChanged", lambda _: self._cv_send_bordfilt())
        ci("cvRoiEnableCheck",  "toggled", self._on_roi_enable_toggled)
        ci("cvBtnSendRoi",      "clicked", self._cv_send_roi)

        # TinyML
        ci("btnTMGet",          "clicked", lambda: self._send("TM GET"))
        ci("btnRunML",          "clicked", self._run_ml)
        ci("btnSnapAndRunML",   "clicked", self._snap_and_run_ml)
        ci("btnSaveMLResult",   "clicked", self._save_ml_result_clicked)
        # TinyML enable checkbox sends immediately on toggle
        ci("mlEnabledCheckBox", "stateChanged", lambda _: self._apply_tm_enable())

        # Benchmark
        ci("btnAddRun",       "clicked", self._add_bench_run)
        ci("btnDeleteLastRun","clicked", self._delete_last_bench_run)
        ci("btnExportCSV",    "clicked", self._export_csv)
        ci("btnClearBench",   "clicked", self._clear_bench)
        ci("btnClearDiag",    "clicked", self._clear_diag)

        # Serial service events
        self.serial.serial.readyRead.connect(self._on_ready_read)
        self.serial.serial.errorOccurred.connect(self._on_serial_error)

        # ── Tab switch: refresh image previews so labels have their real size ─
        tab = self.window.findChild(object, "topTabWidget")
        if tab:
            tab.currentChanged.connect(lambda _: self._refresh_preview())

    # Utility
    def _w(self, name: str):
        """Return the named widget, raising AttributeError if not found."""
        w = self.window.findChild(object, name)
        if w is None:
            raise AttributeError(f"Widget not found: '{name}'")
        return w

    def _cv_widget(self, name: str):
        """Return a named CV panel widget; returns None (no exception) if absent."""
        return self.window.findChild(object, name)

    def _get_or_create_image_folder(self) -> Path:
        if self._current_image_folder is not None:
            return self._current_image_folder
        ts     = timestamp()
        folder = self._outputs_root / f"image_{self._current_image_index:04d}_{ts}"
        folder.mkdir(parents=True, exist_ok=True)
        self._current_image_folder = folder
        return folder

    def _image_sub(self, sub: str) -> Path:
        d = self._get_or_create_image_folder() / sub
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _relative_to_project(self, path: Path) -> str:
        try:
            return str(path.resolve().relative_to(self._project_root.resolve()))
        except Exception:
            return str(path)

    def _last_log_lines(self, n: int = 20) -> list[str]:
        return self._w("logEdit").toPlainText().splitlines()[-n:]

    # Log
    def _log(self, level: str, msg: str) -> None:
        """Append a timestamped entry to the UART log widget."""
        ts  = datetime.now().strftime("%H:%M:%S")
        log = self._w("logEdit")
        log.appendPlainText(f"[{ts}] {level}: {msg}")
        doc = log.document()
        while doc.blockCount() > LOG_MAX_LINES:
            cursor = log.textCursor()
            cursor.movePosition(cursor.MoveOperation.Start)
            cursor.select(cursor.SelectionType.BlockUnderCursor)
            cursor.removeSelectedText()
            cursor.deleteChar()

        # Mirror important messages to the Vision tab status bar so the user
        # can see STM32 activity without switching to the Dashboard tab.
        if level in ("WARN", "ERR", "INFO"):
            stat = self.window.findChild(object, "statLabel")
            if stat:
                # Prepend level indicator for warnings/errors
                if level == "WARN":
                    indicator = f"⚠ {msg}"
                elif level == "ERR":
                    indicator = f"✗ {msg}"
                else:
                    indicator = msg
                # Keep the stat label short
                if len(indicator) > 80:
                    indicator = indicator[:77] + "…"
                stat.setToolTip(f"[{ts}] {level}: {msg}")
                # Only override if not showing live FPS data (preserve STAT info)
                current = stat.text()
                if not current.startswith("FPS") or level in ("WARN", "ERR"):
                    stat.setText(f"[{ts}] {indicator}")

    # Controls enable/disable
    def _set_controls_enabled(self, enabled: bool) -> None:
        for name in (
            # Camera / stream
            "btnSnap", "btnStartStream", "btnStopStream",
            "camModeComboBox", "camResComboBox", "btnApplyCamMode",
            "btnGetStatus",
            # Image quality
            "briComboBox", "conComboBox", "satComboBox", "effectComboBox",
            "lightComboBox", "awbComboBox", "flashCheckBox", "zoomComboBox",
            "btnApplyAllQuality", "btnResetQuality",
            # CV action buttons
            "cvBtnGet", "btnRunCV", "cvBtnBgCap",
            "btnSnapAndRunCV",
            # CV controls
            "cvEnableCheck", "cvPresetCombo",
            "cvOtsuCheck", "cvThrSlider",
            "cvInvertCheck",
            "cvFilterCombo", "cvBlurSpin",
            "cvMorphModeCombo", "cvMorphKernelSpin",
            "cvConnCombo",
            "cvMinAreaSpin", "cvMaxAreaSpin",
            "cvArMinSpin", "cvArMaxSpin", "cvCircMinSpin",
            "cvBgSubCheck",
            "cvBordFiltCheck",
            "cvRoiEnableCheck", "cvBtnSendRoi",
            # TinyML
            "btnRunML", "btnSnapAndRunML",
            "mlEnabledCheckBox", "btnTMGet",
            # Benchmark / save
            "btnAddRun",
        ):
            w = self.window.findChild(object, name)
            if w:
                w.setEnabled(enabled)
        for name in ("dsCaptureButton", "dsStopButton"):
            w = self.window.findChild(object, name)
            if w:
                w.setEnabled(enabled)
        self._after_controls_enabled(enabled)

    # Snapshot / stream helpers
    def _after_controls_enabled(self, enabled: bool) -> None:
        """Called after _set_controls_enabled to update mode-dependent states."""
        if enabled:
            self._update_mode_status_banner()

    def _camera_transfer_busy(self) -> bool:
        return bool(self._snapshot_in_flight or self._transfer.active)

    def _schedule_snap(self, delay_ms: int, reason: str = "SNAP") -> bool:
        if self._camera_transfer_busy():
            self._log("WARN", f"{reason} blocked: transfer already active")
            return False

        def do_snap() -> None:
            if self._camera_transfer_busy():
                self._log("WARN", f"{reason} skipped: transfer became active during delay")
                return
            if self._send("SNAP"):
                self._snapshot_in_flight = True
                self._snap_timeout_timer.start(self._frame_timeout_ms)

        if delay_ms > 0:
            QTimer.singleShot(delay_ms, do_snap)
        else:
            do_snap()
        return True

    def _on_snapshot_timeout(self) -> None:
        if not self._snapshot_in_flight and not self._transfer.active:
            return
        expected = self._transfer.expected_size
        received = len(self._transfer.received)
        self._log("WARN",
            f"Image transfer timeout ({self._frame_timeout_ms // 1000}s, "
            f"{received}/{expected} B received). Resynchronising.")
        self._recover_from_protocol_desync()

    def _snap(self) -> bool:
        if not self._require_camera_ready("SNAP"):
            return False
        if self._camera_transfer_busy():
            self._log("WARN", "SNAP blocked: previous transfer is still active")
            return False

        # STREAM + FLASH + SNAP FIX:
        # If the stream is active, we must wait for "STREAM OFF" from the
        # firmware before sending SNAP. Sending SNAP while stream_enable==1
        # causes "SNAP not allowed while STREAM ON" error.
        # We queue the actual snap as a pending action executed by rx_controller
        # when "STREAM OFF" is received.
        if self._stream_active:
            self._send("STREAM 0")
            self._update_stream_status(False)
            self._pending_after_stream_off.append(self._do_snap)
            return True

        self._do_snap()
        return True

    def _do_snap(self) -> None:
        """Execute the actual snap sequence (mode config + flash + SNAP)."""
        if self._camera_transfer_busy():
            self._log("WARN", "SNAP blocked: transfer became active")
            return

        flash_cb = self.window.findChild(QCheckBox, "flashCheckBox")
        flash_on = bool(flash_cb and flash_cb.isChecked())
        mode     = self._w("camModeComboBox").currentText().strip().upper()
        changed  = self._send_camera_config_if_changed(force=False, log=True)

        # FLASH: persistent toggle - send current state, no timer, no auto-off.
        self._send(f"FLASH {'1' if flash_on else '0'}", log=False)

        if mode == "RGB":
            # The quality settings the user chose (brightness, contrast,
            # saturation, effect, AWB) stay in effect for a manual SNAP. Only
            # the automatic runs (Snap + CV/TinyML, dataset capture) reset
            # them to the defaults so that their frames are comparable.
            delay_ms = 450 if changed else 250
            self._log("INFO", f"Waiting {delay_ms} ms for RGB sensor to settle")
            self._schedule_snap(delay_ms, "RGB SNAP")
        elif changed:
            self._log("INFO", "Waiting briefly for camera config to settle")
            self._schedule_snap(200, "SNAP after reconfig")
        else:
            self._schedule_snap(0, "SNAP")

    # CV command helpers
    def _cv_send_enable(self, checked: bool) -> None:
        """Send CV EN 1|0."""
        self._send(f"CV EN {'1' if checked else '0'}")

    def _cv_send_preset(self) -> None:
        """Send the selected preset to the STM32 and load its values into
        the UI widgets. The STM32 confirms the active configuration with a
        CVCFG line."""
        combo = self._cv_widget("cvPresetCombo")
        if not combo:
            return
        idx = combo.currentIndex()
        # Send preset to STM32 immediately (single command, fast)
        self._send(f"CV PRESET {idx}")
        # Also update UI to match (STM32 will confirm via CVCFG)

    def _cv_thr_slider_changed(self, value: int) -> None:
        """Slider moved → instantly update local binary preview (no STM32 needed)."""
        lbl = self._cv_widget("cvThrValueLabel")
        if lbl:
            lbl.setText(str(value))
        if self._last_qimg is not None and not self._last_qimg.isNull():
            # Refresh binary + overlay locally (gray stays the same)
            self._refresh_preview()

    def _cv_send_bgsub(self) -> None:
        """Send CV BGSUB 0|1."""
        cb  = self._cv_widget("cvBgSubCheck")
        self._send(f"CV BGSUB {'1' if (cb and cb.isChecked()) else '0'}")

    def _cv_send_bordfilt(self) -> None:
        """Send CV BORDFILT 0|1."""
        cb  = self._cv_widget("cvBordFiltCheck")
        self._send(f"CV BORDFILT {'1' if (cb and cb.isChecked()) else '0'}")

    def _on_roi_enable_toggled(self, enabled: bool) -> None:
        """Enable or disable the ROI coordinate spinboxes."""
        for name in ("cvRoiXSpin", "cvRoiYSpin", "cvRoiWSpin", "cvRoiHSpin"):
            w = self._cv_widget(name)
            if w:
                w.setEnabled(enabled)

    def _cv_send_roi(self) -> None:
        """Send CV ROI 1 x y w h (enabled) or CV ROI 0."""
        en = self._cv_widget("cvRoiEnableCheck")
        if en is None or not en.isChecked():
            self._send("CV ROI 0")
            return
        x  = self._cv_widget("cvRoiXSpin")
        y  = self._cv_widget("cvRoiYSpin")
        w  = self._cv_widget("cvRoiWSpin")
        h  = self._cv_widget("cvRoiHSpin")
        xv = x.value() if x else 0
        yv = y.value() if y else 0
        wv = w.value() if w else 0
        hv = h.value() if h else 0
        if wv == 0 or hv == 0:
            self._log("WARN", "CV ROI: width and height must be > 0")
            return
        self._send(f"CV ROI 1 {xv} {yv} {wv} {hv}")

    def _send_cv_commands_only(self) -> None:
        """
        Send all CV parameters + CV RUN with 15ms inter-command delay.

        The STM32 UART RX queue is 512 bytes (fixed in main.c).
        We still space commands 15ms apart as a safety margin and to avoid
        flooding the log.
        """
        cmds = self._build_cv_command_list()
        self._send_cmd_queue(cmds, delay_ms=15)

    def _build_cv_command_list(self) -> list:
        """Build the list of CV parameter commands + CV RUN."""
        cmds = []
        # If a named preset is active, send only the preset command - sending
        # individual params would cause the firmware to reset preset to CUSTOM.
        preset_combo = self._cv_widget("cvPresetCombo")
        if preset_combo and preset_combo.currentIndex() > 0:
            cmds.append(f"CV PRESET {preset_combo.currentIndex()}")
            cmds.append("CV RUN")
            return cmds
        # Threshold mode + value
        otsu = self._cv_widget("cvOtsuCheck")
        if otsu and otsu.isChecked():
            cmds.append("CV THRMODE 1")
        else:
            cmds.append("CV THRMODE 0")
            slider = self._cv_widget("cvThrSlider")
            if slider:
                cmds.append(f"CV THR {slider.value()}")
        # Invert
        cb = self._cv_widget("cvInvertCheck")
        cmds.append(f"CV INV {'1' if (cb and cb.isChecked()) else '0'}")
        # Filter + Blur
        combo = self._cv_widget("cvFilterCombo")
        spin  = self._cv_widget("cvBlurSpin")
        if combo: cmds.append(f"CV FILTER {combo.currentIndex()}")
        if spin:  cmds.append(f"CV BLUR {spin.value()}")
        # Morph mode + kernel
        mode = self._cv_widget("cvMorphModeCombo")
        k    = self._cv_widget("cvMorphKernelSpin")
        if mode: cmds.append(f"CV MORPHMODE {mode.currentIndex()}")
        if k:    cmds.append(f"CV MORPH {k.value()}")
        # CCL connectivity
        conn = self._cv_widget("cvConnCombo")
        cmds.append(f"CV CON {conn.currentText() if conn else '8'}")
        # Area filters
        mn = self._cv_widget("cvMinAreaSpin")
        mx = self._cv_widget("cvMaxAreaSpin")
        if mn: cmds.append(f"CV MINAREA {mn.value()}")
        if mx: cmds.append(f"CV MAXAREA {mx.value()}")
        # Shape filters (only if non-zero to avoid old-firmware errors)
        ar_min = self._cv_widget("cvArMinSpin")
        ar_max = self._cv_widget("cvArMaxSpin")
        circ   = self._cv_widget("cvCircMinSpin")
        if ar_min and ar_max:
            amin, amax = ar_min.value(), ar_max.value()
            cmds.append(f"CV ASPECT {amin} {amax}")
        if circ:
            cmds.append(f"CV CIRC {circ.value()}")
        # BG subtraction + border filter
        bgsub = self._cv_widget("cvBgSubCheck")
        cmds.append(f"CV BGSUB {'1' if (bgsub and bgsub.isChecked()) else '0'}")
        bord  = self._cv_widget("cvBordFiltCheck")
        cmds.append(f"CV BORDFILT {'1' if (bord and bord.isChecked()) else '0'}")
        # Run
        cmds.append("CV RUN")
        return cmds

    def _send_cmd_queue(self, cmds: list, delay_ms: int = 15) -> None:
        """Send a list of commands with a fixed delay between each one."""
        if not cmds:
            return
        self._send(cmds[0])
        remaining = cmds[1:]
        if remaining:
            QTimer.singleShot(delay_ms,
                lambda: self._send_cmd_queue(remaining, delay_ms))

    # CVCFG UI sync
    _PRESET_NAMES = {0: "0 - CUSTOM", 1: "1 - FAST", 2: "2 - ROBUST", 3: "3 - ACCURATE"}
    _FILTER_NAMES = {0: "0 - OFF", 1: "1 - BOX", 2: "2 - MEDIAN"}
    _MORPH_NAMES  = {0: "0 - OFF", 1: "1 - OPEN", 2: "2 - CLOSE", 3: "3 - ERODE", 4: "4 - DILATE"}

    def _set_label(self, name: str, text: str) -> None:
        w = self._cv_widget(name)
        if w:
            w.setText(text)

    def _apply_cvcfg_to_ui(self, cfg: CvConfig) -> None:
        """Show a received CVCFG line in the CV controls and in the read-back labels."""
        yes_no = lambda v: "Yes" if v else "No"

        # Read-back labels (the group box with the configuration of the STM32)
        self._set_label("cvFwEnabledLbl",   yes_no(cfg.enabled))
        self._set_label("cvFwPresetLbl",    self._PRESET_NAMES.get(cfg.preset, str(cfg.preset)))
        self._set_label("cvFwThrModeLbl",   "OTSU" if cfg.threshold_mode else "MANUAL")
        self._set_label("cvFwThrLbl",       str(cfg.threshold))
        self._set_label("cvFwInvLbl",       yes_no(cfg.invert))
        self._set_label("cvFwFilterLbl",    self._FILTER_NAMES.get(cfg.filter_mode, str(cfg.filter_mode)))
        self._set_label("cvFwBlurLbl",      f"{cfg.blur_kernel}  (k={cfg.blur_kernel * 2 + 1 if cfg.blur_kernel else 0})")
        self._set_label("cvFwMorphModeLbl", self._MORPH_NAMES.get(cfg.morph_mode, str(cfg.morph_mode)))
        self._set_label("cvFwMorphKLbl",    f"{cfg.morph_kernel}  (k={cfg.morph_kernel * 2 + 1 if cfg.morph_kernel else 0})")
        self._set_label("cvFwConnLbl",      str(cfg.connectivity))
        self._set_label("cvFwMinAreaLbl",   str(cfg.min_area))
        self._set_label("cvFwMaxAreaLbl",   f"{cfg.max_area}  (0=unlimited)")
        self._set_label("cvFwBordFiltLbl",  yes_no(cfg.border_filter_enabled))
        self._set_label("cvFwBgSubLbl",     yes_no(cfg.bgsub_enabled))

        # The controls themselves
        def put(name, how, value):
            w = self._cv_widget(name)
            if w is None:
                return
            w.blockSignals(True)
            if how == "check":
                w.setChecked(bool(value))
            elif how == "index":
                w.setCurrentIndex(min(value, w.count() - 1))
            elif how == "text":
                w.setCurrentText(value)
            else:
                w.setValue(value)
            w.blockSignals(False)

        put("cvEnableCheck",     "check", cfg.enabled)
        put("cvPresetCombo",     "index", cfg.preset)
        put("cvOtsuCheck",       "check", cfg.threshold_mode)
        put("cvThrSlider",       "value", max(0, min(255, cfg.threshold)))
        self._set_label("cvThrValueLabel", str(cfg.threshold))
        put("cvInvertCheck",     "check", cfg.invert)
        put("cvFilterCombo",     "index", cfg.filter_mode)
        put("cvBlurSpin",        "value", cfg.blur_kernel)
        put("cvMorphModeCombo",  "index", cfg.morph_mode)
        put("cvMorphKernelSpin", "value", cfg.morph_kernel)
        put("cvConnCombo",       "text",  str(cfg.connectivity))
        put("cvMinAreaSpin",     "value", cfg.min_area)
        put("cvMaxAreaSpin",     "value", cfg.max_area)
        put("cvArMinSpin",       "value", cfg.aspect_ratio_min_x1000)
        put("cvArMaxSpin",       "value", cfg.aspect_ratio_max_x1000)
        put("cvCircMinSpin",     "value", cfg.circularity_min_x1000)
        put("cvBgSubCheck",      "check", cfg.bgsub_enabled)
        put("cvBordFiltCheck",   "check", cfg.border_filter_enabled)

        put("cvRoiEnableCheck", "check", cfg.roi_enabled)
        for name, value in (("cvRoiXSpin", cfg.roi_x), ("cvRoiYSpin", cfg.roi_y),
                            ("cvRoiWSpin", cfg.roi_w), ("cvRoiHSpin", cfg.roi_h)):
            put(name, "value", value)
            w = self._cv_widget(name)
            if w:
                w.setEnabled(bool(cfg.roi_enabled))

    # CVSTAT UI update
    def _update_cv_result_ui(self) -> None:
        """Fill the result labels and the bounding-box table after CVDONE."""
        r = self._cv_result
        if r is None:
            return

        self._set_label("cvResObjLbl",      str(r.count))
        self._set_label("cvResTimeLbl",     f"{r.processing_time_ms} ms")
        self._set_label("cvResMeanAreaLbl", str(r.mean_area))
        self._set_label("cvResMinAreaLbl",  str(r.area_min))
        self._set_label("cvResMaxAreaLbl",  str(r.area_max))
        self._set_label("cvResBrightLbl",   str(r.mean_brightness))
        self._set_label("cvResFgPixLbl",    str(r.fg_pixel_count))
        self._set_label("cvResRawCompLbl",  str(r.raw_comp_count))
        self._set_label("cvResBoxesLbl",    str(r.box_count))
        self._set_label("cvRejSmallLbl",    str(r.rejected_small))
        self._set_label("cvRejLargeLbl",    str(r.rejected_large))
        self._set_label("cvRejBorderLbl",   str(r.rejected_border))
        self._set_label("cvRejShapeLbl",    str(r.rejected_shape))

        tbl = self._cv_widget("cvBoxTable")
        if tbl is None:
            return
        tbl.setRowCount(len(r.boxes))
        for row, b in enumerate(r.boxes):
            for col, val in enumerate([b.box_id, b.area, b.x, b.y, b.w, b.h,
                                       b.perimeter, b.circularity_x1000]):
                item = QTableWidgetItem(str(val))
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                tbl.setItem(row, col, item)

    def _clear_cv_result_ui(self) -> None:
        """Reset all CVSTAT result labels and the bounding-box table."""
        for name in (
            "cvResObjLbl", "cvResTimeLbl", "cvResMeanAreaLbl",
            "cvResMinAreaLbl", "cvResMaxAreaLbl", "cvResBrightLbl",
            "cvResFgPixLbl", "cvResRawCompLbl", "cvResBoxesLbl",
            "cvRejSmallLbl", "cvRejLargeLbl", "cvRejBorderLbl", "cvRejShapeLbl",
        ):
            self._set_label(name, "—")
        tbl = self._cv_widget("cvBoxTable")
        if tbl:
            tbl.setRowCount(0)

    # CV / TinyML requests
    def _run_cv(self) -> None:
        if not self._require_camera_ready("STM32 CV"):
            return
        if not self._has_valid_rgb565_frame():
            self._log("WARN",
                "CV RUN requires RGB565 frame - "
                "switch to RGB mode, press SNAP, then CV RUN. "
                "Or use 'Snap + CV RUN' to do both in one click.")
            # Flash the mode banner so user sees it on the Vision tab
            self._update_mode_status_banner()
            return
        self._send_cv_commands_only()

    def _run_ml(self) -> None:
        if not self._require_camera_ready("TinyML"):
            return
        if not self._has_valid_rgb565_frame():
            self._log("WARN",
                "TM RUN requires RGB565 frame - "
                "switch to RGB mode, press SNAP, then TM RUN. "
                "Or use 'Snap + TM RUN' to do both in one click.")
            self._update_mode_status_banner()
            return
        self._send("TM RUN")

    def _snap_and_run_cv(self) -> None:
        if not self._require_camera_ready("Snap + CV"):
            return
        self._request_rgb565_frame_for_cv()

    def _snap_and_run_ml(self) -> None:
        if not self._require_camera_ready("Snap + TinyML"):
            return
        self._request_rgb565_frame_for_tinyml()

    def _apply_tm_enable(self) -> None:
        cb  = self.window.findChild(QCheckBox, "mlEnabledCheckBox")
        val = "1" if (cb and cb.isChecked()) else "0"
        self._send(f"TM EN {val}")

    def _insert_manual_command_template(self) -> None:
        combo = self.window.findChild(QComboBox, "commandPresetComboBox")
        if combo:
            self._w("manualCommandLineEdit").setText(combo.currentText())

    # Frame checks
    def _has_valid_rgb565_frame(self) -> bool:
        """True if the last frame is an RGB565 frame that CV RUN and TM RUN can use.

        The frame buffer of the firmware holds 480x272 pixels at most. Smaller
        frames, also the ones of a zoomed image, are fine.
        """
        if self._last_qimg is None or self._last_qimg.isNull():
            return False
        if self._last_format != "RGB565":
            return False
        return self._last_qimg.width() * self._last_qimg.height() <= 480 * 272

    def _request_rgb565_frame_for_cv(self) -> bool:
        if self._camera_transfer_busy():
            self._log("WARN", "CV blocked: transfer still active")
            return False
        self._cv_run_after_next_rgb_frame = True
        self._w("camModeComboBox").setCurrentText("RGB")
        self._w("camResComboBox").setCurrentText("QVGA")
        self._send("STREAM 0")
        self._update_stream_status(False)
        changed = self._send_camera_config_if_changed(force=False, log=True)
        self._apply_camera_defaults(log=True)
        delay_ms = 500 if changed else 300
        self._log("INFO", "Snapping RGB565/QVGA for CV auto-run")
        ok = self._schedule_snap(delay_ms, "CV RGB SNAP")
        if not ok:
            self._cv_run_after_next_rgb_frame = False
        return ok

    def _request_rgb565_frame_for_tinyml(self) -> bool:
        if self._camera_transfer_busy():
            self._log("WARN", "TinyML blocked: transfer still active")
            return False
        self._tm_run_after_next_rgb_frame = True
        self._w("camModeComboBox").setCurrentText("RGB")
        # 480x272 like the training images of the deployed model.
        self._w("camResComboBox").setCurrentText("WQVGA")
        self._send("STREAM 0")
        self._update_stream_status(False)
        changed = self._send_camera_config_if_changed(force=False, log=True)
        self._apply_camera_defaults(log=True)
        delay_ms = 500 if changed else 300
        self._log("INFO", "Snapping RGB565/WQVGA for TinyML auto-run")
        ok = self._schedule_snap(delay_ms, "TinyML RGB SNAP")
        if not ok:
            self._tm_run_after_next_rgb_frame = False
        return ok


    # Entry point
    def _inject_mode_banners(self) -> None:
        """
        Create mode-status banner QLabels and insert them into the
        CV group and TinyML group headers dynamically.
        These labels are referenced by _update_mode_status_banner() in
        display_controller.py via the names cvModeStatusLabel / mlModeStatusLabel.
        """
        from PySide6.QtWidgets import QLabel

        def _add_banner(group_name: str, label_name: str) -> None:
            grp = self.window.findChild(object, group_name)
            if grp is None:
                return
            lbl = QLabel("", grp)
            lbl.setObjectName(label_name)
            lbl.setWordWrap(True)
            lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            lbl.setStyleSheet(
                "border-radius:6px; padding:4px 8px; font-size:9.5pt; "
                "background:#f8fafc; border:1px solid #e2e8f0;")
            layout = grp.layout()
            if layout is None:
                return
            from PySide6.QtWidgets import QBoxLayout, QGridLayout
            if isinstance(layout, QBoxLayout):
                # QVBoxLayout / QHBoxLayout - insert at top
                layout.insertWidget(0, lbl)
            elif isinstance(layout, QGridLayout):
                # Shift all existing rows down by one and insert banner in row 0
                for i in range(layout.count() - 1, -1, -1):
                    item = layout.itemAt(i)
                    if item is None:
                        continue
                    r, c, rs, cs = layout.getItemPosition(i)
                    w = item.widget()
                    if w is not None:
                        layout.removeWidget(w)
                        layout.addWidget(w, r + 1, c, rs, cs)
                layout.addWidget(lbl, 0, 0, 1, layout.columnCount() or 1)
            else:
                # FormLayout or unknown - just add at the end
                layout.addWidget(lbl)

        # Try the most likely CV group names (Qt Designer default vs named).
        # The first one found in the .ui is used.
        for cv_grp in ("cvControlGroupBox", "groupBox", "cvGroupBox"):
            if self.window.findChild(object, cv_grp):
                _add_banner(cv_grp, "cvModeStatusLabel")
                break
        _add_banner("mlControlGroupBox", "mlModeStatusLabel")

        # Initial update
        self._update_mode_status_banner()

    def show(self) -> None:
        """Show the main window."""
        self.window.show()
