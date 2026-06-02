# STM32 Edge Vision — Reference Dashboard & TinyML Counting Model

A complete embedded-vision demonstrator for **STM32H7 + OV2640**:

- Real-time camera capture via DCMI/DMA
- Classical Vision (CV) object detection with Otsu/manual threshold, morphology and bounding-box extraction — runs on-chip
- TinyML object counting (MobileNetV1-0.25, 5 classes) via X-CUBE-AI — runs on-chip
- PySide6 Reference Dashboard on the PC for control, visualisation, benchmarking and dataset capture

> **All inference runs on the STM32.** The GUI only visualises results, saves frames, and manages the benchmark. It never simulates or locally computes CV/TinyML results.

---

## Table of contents

1. [Repository structure](#repository-structure)
2. [Hardware setup](#hardware-setup)
3. [Quick start — GUI](#quick-start--gui)
4. [Quick start — ML training](#quick-start--ml-training)
5. [Model specification](#model-specification)
6. [UART protocol](#uart-protocol)
7. [Dataset capture workflow](#dataset-capture-workflow)
8. [Training workflow](#training-workflow)
9. [STM32 deployment (X-CUBE-AI)](#stm32-deployment-x-cube-ai)
10. [Benchmarking](#benchmarking)
11. [Troubleshooting](#troubleshooting)
12. [Dependencies](#dependencies)

---

## Repository structure

```text
STM32_MERO2/
│
├─ GUI_App/                              # PySide6 Reference Dashboard
│  ├─ app/
│  │  ├─ src/
│  │  │  ├─ main.py                      # Qt application entry point
│  │  │  ├─ reference_window.py          # Main GUI controller
│  │  │  ├─ protocol_parser.py           # STM32 UART protocol parser
│  │  │  ├─ serial_service.py            # QSerialPort wrapper
│  │  │  ├─ image_utils.py               # JPEG / RGB565 image conversion
│  │  │  ├─ dashboard_controller.py      # Serial + camera mode (DashboardMixin)
│  │  │  ├─ rx_controller.py             # UART RX path, frame decode (RxMixin)
│  │  │  ├─ display_controller.py        # Preview refresh, CV/ML labels (DisplayMixin)
│  │  │  ├─ bench_controller.py          # Benchmark table, CSV export (BenchMixin)
│  │  │  ├─ save_controller.py           # Frame/log save operations (SaveMixin)
│  │  │  ├─ dataset_controller.py        # Automated dataset capture (DatasetMixin)
│  │  │  ├─ app_constants.py             # Constants, stylesheet
│  │  │  └─ app_helpers.py               # Path helpers, FrameTransfer, norm01
│  │  ├─ ui/
│  │  │  └─ reference_window.ui          # Qt Designer layout
│  │  └─ assets/
│  │     └─ app_icon.ico
│  ├─ packaging/
│  │  ├─ build_windows_exe.bat           # PyInstaller build script
│  │  └─ STM32_Edge_Vision_Reference_Dashboard.spec
│  └─ outputs/                           # Runtime exports (git-ignored)
│     ├─ logs/
│     ├─ frames/
│     ├─ debug_images/
│     ├─ benchmark/
│     └─ dataset/
│
├─ CubeIDE_Workspace/
│  └─ STM32_H7Firmware/                  # STM32CubeIDE project
│     └─ Core/
│        ├─ Src/
│        │  ├─ main.c
│        │  ├─ camera_app.c / .h
│        │  ├─ camera_capture.c / .h
│        │  ├─ camera_proto.c / .h
│        │  ├─ camera_parse.c / .h
│        │  ├─ cv_engine.c / .h
│        │  ├─ tinyml_engine.c / .h
│        │  ├─ tinyml_preprocess.c / .h
│        │  ├─ uart_tx.c / .h
│        │  └─ ov2640_Drive.c / .h
│        └─ Inc/                         # Corresponding header files
│
├─ ML training/                          # PC-side TinyML workflow
│  ├─ dataset/
│  │  ├─ train/ count_0…count_4/         # ≥ 50 images per class recommended
│  │  └─ val/   count_0…count_4/         # ≥ 10 images per class recommended
│  ├─ src/
│  │  ├─ predict_count_tflite.py         # PC-side TFLite verification
│  │  ├─ check_dataset.py                # Dataset structure validator
│  │  └─ train_count_model.py            # Standalone Keras training script
│  ├─ Model/
│  │  └─ image_classification/
│  │     └─ user_config.yaml             # ST Model Zoo training config
│  └─ docs/
│     └─ DATASET_GUIDE.md
│
├─ Ablauf/                               # Architecture diagrams (PlantUML)
│  ├─ plantuml_alle_diagramme.puml
│  └─ cv_tinyml_uebersicht.puml
│
├─ requirements.txt                      # Unified Python dependencies
└─ README.md                             # This file
```

---

## Hardware setup

| Part | Details |
|------|---------|
| MCU | STM32H7 — Nucleo-144 |
| Camera | OV2640, connected via DCMI + DMA |
| UART | USART3 → ST-LINK VCP, **2 000 000 Baud, 8N1** |
| LED | Red LED (PA0) = heartbeat (1 Hz = firmware running) |
| Button | PC13 = manual snapshot trigger |

---

## Quick start — GUI

```bash
# 1. Create virtual environment
python -m venv .venv
.venv\Scripts\activate          # Windows
source .venv/bin/activate       # Linux / macOS

# 2. Install dependencies
pip install -r requirements.txt

# 3. Run the dashboard
python GUI_App/app/src/main.py
```

Connect to the correct COM port at **2 000 000 Baud**.  
The green "STM32 ready" indicator appears after the first valid protocol line is received.

**Basic workflow:**

| Step | GUI action | UART command sent |
|------|-----------|-------------------|
| Capture frame | Mode = RGB → **SNAP** | `SNAP` |
| Run CV | **Run STM32 CV** | `CV RUN` |
| Run TinyML | **Run STM32 TinyML** | `TM RUN` |
| Save frame | **Save Frame…** | — |
| Benchmark | **Add Current Run** | — |
| Export data | **Export CSV** | — |

> **Important:** CV RUN and TM RUN always operate on the *last captured* RGB565 frame.  
> For a fair benchmark, always SNAP once, then run CV RUN and TM RUN on the same frame.

---

## Quick start — ML training

```bash
# 1. Clone the ST Model Zoo into ML training/Model/ (required once)
cd "ML training/Model"
git clone https://github.com/STMicroelectronics/stm32ai-modelzoo-services
pip install -e stm32ai-modelzoo-services
cd ../..

# 2. Validate your dataset
python "ML training/src/check_dataset.py" --root "ML training/dataset"

# 3. Edit ML training/Model/image_classification/user_config.yaml
#    Update training_path, validation_path, quantization_path with absolute paths

# 4. Train + quantise + evaluate (chain_tqe)
cd "ML training/Model/stm32ai-modelzoo-services/image_classification/tf"
python stm32ai_main.py \
    --config-path ../../../image_classification \
    --config-name user_config.yaml

# Output: experiments_outputs/<timestamp>/quantized_models/quantized_model.tflite

# 5. Verify on a saved frame (from project root)
python "ML training/src/predict_count_tflite.py" \
    --model path/to/quantized_model.tflite \
    --image path/to/frame.png \
    --show-hash
```

---

## Model specification

| Field | Value |
|-------|-------|
| Architecture | MobileNetV1 α=0.25 |
| Input shape | **96 × 96 × 3** (H × W × C) |
| Colour space | **RGB** (3 channels) |
| Input dtype | **uint8 \[0…255\]** |
| Resize method | **Nearest-neighbour, full-frame stretch** (no padding, no letterbox) |
| Quantisation | PTQ, `QLinear(scale=1/127.5, zero_point=127)` |
| Output | float32 \[5\], Softmax probabilities |
| Classes | `count_0`, `count_1`, `count_2`, `count_3`, `count_4` |
| Activation RAM | ≈ 41 KB |
| Weight Flash | ≈ 215 KB |

### ⚠ Preprocessing contract — read before retraining

The STM32 firmware (`tinyml_preprocess.c`) maps each output pixel as:

```c
sx = ox * src_width  / 96   // integer-floor, full-frame stretch
sy = oy * src_height / 96
```

`user_config.yaml` **must** set:

```yaml
resizing:
  interpolation: nearest
  aspect_ratio: stretch     # NOT "fit" — "fit" adds letterboxing the firmware never replicates
color_mode: rgb             # NOT grayscale — model expects 3 channels
```

Setting `aspect_ratio: fit` introduces black borders during training that the firmware never produces → domain shift → wrong predictions.

### ❌ Removed from older versions

The following settings appeared in earlier project files and are **no longer valid**:

| Old value | Correct value | Impact if not corrected |
|-----------|--------------|------------------------|
| `input_shape: (48, 48, 1)` | `(96, 96, 3)` | Wrong model size |
| `color_mode: grayscale` | `rgb` | 1-channel vs 3-channel mismatch |
| `aspect_ratio: fit` | `stretch` | Domain shift, wrong predictions |
| `board: STM32H747I-DISCO` | `NUCLEO-H7` | Wrong benchmarking target |
| `quantization_path:` (empty) | set to training path | Poor PTQ calibration |

---

## UART protocol

**Connection:** USART3, 2 000 000 Baud, 8N1, no flow control

### Frame transfer headers (binary payload follows immediately)

```
JPG: <bytes>
RGB565: <width> <height> <bytes>
```

### Status lines

```
INFO: <message>
WARN: <message>
ERR:  <message>
STAT: FPS=X.X SIZE=XB HEAP=XKB FB=XKB LAT=Xms
```

### Classical Vision

```
CVCFG: EN=1 THR=128 THRMODE=0 INV=0 BLUR=0 FILTER=0 MORPH=0
       MORPHMODE=0 CON=8 MIN=20 MAX=0 ARMIN=0 ARMAX=0
       CIRCMIN=0 ROIEN=0 ROIX=0 ROIY=0 ROIW=0 ROIH=0

CVSTAT: COUNT=N MEAN=X MAX=X MIN=X BRIGHT=X TIME=Xms
        REJSMALL=X REJLARGE=X BOXES=N
CVBOX:  ID=N AREA=X X=X Y=X W=X H=X PERI=X CIRC=X
CVDONE
```

### TinyML

```
TMCFG:  EN=1 INPUT=96x96x3 CLASSES=5 MODEL=mobilenetv1_a025
TMINFO: STATUS=XCUBEAI_OK RAM=41KB FLASH=215KB
TMRES:  CLASS=count_N IDX=N CONF=XXX TIME=Xms UNCERTAIN=0
TMPROB: IDX=N NAME=COUNT_N SCORE=XXX
TMDONE
```

`CONF` carries the confidence in permille (0…1000); the GUI normalises it to 0.0–1.0.  
When `UNCERTAIN=1`, `CLASS=UNCERTAIN` and the GUI must not display the class as a real prediction.

### CV commands

| Command | Effect |
|---------|--------|
| `CV RUN` | Run CV on last RGB565 frame |
| `CV THRMODE 0` | Manual threshold |
| `CV THRMODE 1` | Otsu auto-threshold ← recommended for benchmarking |
| `CV THR <0..255>` | Set manual threshold |
| `CV INV 0\|1` | Invert binary image |
| `CV BLUR <0..7>` | Pre-threshold blur kernel |
| `CV MORPH <0..7>` | Morphology kernel |
| `CV MORPHMODE 0..4` | OFF / OPEN / CLOSE / ERODE / DILATE |
| `CV MINAREA <n>` | Minimum object area (px²) |
| `CV ROI 1 x y w h` | Enable ROI |
| `CV GET` | Request current config |

### TinyML commands

| Command | Effect |
|---------|--------|
| `TM RUN` | Inference on last RGB565 frame |
| `TM GET` | Request config + info |
| `TM EN 0\|1` | Enable / disable TinyML |

---

## Dataset capture workflow

1. Connect STM32 → GUI.
2. Select **Mode = RGB**, click **SNAP**.
3. Check the camera preview — adjust lighting if needed.
4. Click **Dataset Capture…** and select the target class (`count_0`…`count_4`).
5. Repeat for all classes with at least **50 images per class** in `train/` and **10 in `val/`**.

Every saved PNG is a 320×240 RGB image from the actual OV2640 pipeline — the exact same data the firmware uses for inference.

---

## Training workflow

### 1. Validate dataset

```bash
python "ML training/src/check_dataset.py" --root "ML training/dataset"
```

### 2. Configure

Edit `ML training/Model/image_classification/user_config.yaml`:

```yaml
dataset:
  # Replace with your local absolute paths:
  training_path:   "/path/to/STM32_MERO2/ML training/dataset/train"
  validation_path: "/path/to/STM32_MERO2/ML training/dataset/val"
  quantization_path: "/path/to/STM32_MERO2/ML training/dataset/train"

preprocessing:
  resizing:
    interpolation: nearest
    aspect_ratio: stretch   # critical — must match firmware
  color_mode: rgb
```

### 3. Train

```bash
cd "ML training/Model/stm32ai-modelzoo-services/image_classification/tf"
python stm32ai_main.py --config-path ../../../image_classification --config-name user_config.yaml
```

Output: `experiments_outputs/<timestamp>/quantized_models/quantized_model.tflite`

### 4. Verify on PC

```bash
python "ML training/src/predict_count_tflite.py" \
    --model quantized_model.tflite \
    --image "GUI_App/outputs/frames/frame_latest.png" \
    --show-hash
```

Compare the printed `hash=XXXXXXXX` with the STM32 `TM_IN: hash=...` log.  
**Identical hash = identical preprocessing pipeline.**

---

## STM32 deployment (X-CUBE-AI)

1. Open **STM32CubeMX** → X-CUBE-AI → Import `quantized_model.tflite`.
2. Click **Analyse** — verify:
   - Input: `uint8(1×96×96×3)`, `QLinear(0.00784, 127)`
   - Output: `float32(1×5)`
   - Activations ≈ 41 KB, Weights ≈ 215 KB
3. **Generate Code** → rebuild in STM32CubeIDE.
4. **No changes** to `tinyml_engine.c` are needed after regeneration.

### X-CUBE-AI v10.2 — two mandatory lines in `tinyml_init()`

```c
// Required: initialise STAI runtime before any network call.
// Without this, ai_network_run() always produces constant outputs.
stai_runtime_init();

// Required: pass NULL for weights.
// ai_network_data_params_get() sets the correct Flash pointer internally.
// Passing g_network_weights_table overrides it with the wrong address.
ai_network_create_and_init(&g_network, activations, NULL);
```

---

## Benchmarking

For a **reproducible, fair CV vs TinyML comparison**:

1. `SNAP` — capture one RGB565 frame.
2. `CV THRMODE 1` + `CV RUN` — Otsu auto-threshold, no manual tuning needed.
3. `TM RUN` — inference on the **same frame** (no new SNAP).
4. Verify both used the same buffer: `TMDEBUG: FBADDR=0x...` must match the frame address.
5. Set **GT Count** in the GUI, click **Add Current Run**.
6. Repeat for scenes with 0, 1, 2, 3, 4 objects.
7. **Export CSV** → analyse accuracy and timing.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| TM_RAW_OUT always `[0.5, 0.5, 0, 0, 0]` | `stai_runtime_init()` not called | Add before `ai_network_create_and_init()` |
| TM_RAW_OUT constant for all inputs | `weights=g_network_weights_table` | Pass `NULL` as weights |
| PC hash ≠ STM32 hash | Preprocessing mismatch | Check `aspect_ratio: stretch`, `color_mode: rgb`, `BYTESWAP=0` |
| All predictions = count_0 | Domain shift | Retrain with real OV2640 frames from Dataset Capture |
| UNCERTAIN on all frames | Low confidence | Lower `TINYML_UNCERTAIN_THRESHOLD_PERMILLE` or retrain |
| Red LED not blinking | Firmware not running / scheduler not started | Check build, power, ST-LINK connection |
| GUI shows no frames | Wrong COM port or baud rate | Verify 2 000 000 Baud, STM32 firmware flashed |
| Build error: missing `stai.h` | X-CUBE-AI files not generated | Re-run CubeMX code generation |

---

## Dependencies

```
PySide6>=6.6.0          GUI + serial port
pyserial>=3.5
Pillow>=10.3.0
tensorflow==2.16.2      training + TFLite quantisation
numpy>=1.24.0,<2.0
pandas>=2.0.0
mlflow>=2.11.0
matplotlib>=3.8.0
pyyaml>=6.0
omegaconf>=2.3.0
tqdm>=4.66.0
```

Install everything with:

```bash
pip install -r requirements.txt
```

---

## Contributors

| Name | Role |
|---|---|
| Mutasem Bader | STM32 firmware (FreeRTOS, DCMI/DMA, CV engine, TinyML, UART protocol), Python GUI dashboard, ML training pipeline, system integration |

---

## License

Copyright (c) 2026 Mutasem Bader — All Rights Reserved.  
Viewing is permitted. Copying, modifying, or submitting as own work is strictly prohibited.  
See [LICENSE](LICENSE) for details.
