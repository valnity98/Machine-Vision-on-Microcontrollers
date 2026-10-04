# STM32 Edge Vision — Object Counting with Classical CV and TinyML

Embedded-vision demonstrator on an **STM32H743ZI** with an **OV2640** camera. The same scene is analysed by two methods, both running **on the chip**, and compared on real-world test frames:

- **Classical vision:** Otsu/manual threshold, morphology, connected components
- **TinyML:** quantised MobileNetV1-0.25 (5 classes: 0–4 objects) via X-CUBE-AI
- **PC dashboard (PySide6)** for control, benchmarking and dataset capture — it only visualises results and saves frames; all inference runs on the STM32.

Course project *Mechatronik und Robotik 2* (M.Sc. Mechatronics & Robotics, Frankfurt UAS, SoSe 2026, grade 1.0).

## Results

Test: 90 frames (10 × 0 objects, 20 × each 1–4 objects) in a home environment with changing daylight, a phone torch and directional shadows. Each data point is one `SNAP` → `CV RUN` → `TM RUN` cycle on the same 320×240 RGB565 frame.

| | Classical CV (Otsu) | TinyML (MobileNetV1-0.25) |
|---|---|---|
| **Accuracy (n = 90)** | 14.4 % | **73.3 %** |
| Mean absolute error (objects) | 1.99 | **0.29** |
| Mean run time on the STM32 (480 MHz) | 66.6 ms | **59.8 ms** |
| Flash / RAM (incl. runtime) | — | 279 KB / 57 KB |

TinyML counts 0, 1 and 2 objects almost perfectly (100 %, 100 %, 95 %) but confuses 3 and 4 objects (25 % and 60 %). A likely cause is a **domain shift**: the training images for these classes were taken under different light and camera positions than the test frames. The classical pipeline struggles with shadows and a brightness gradient under a global threshold (example below). The [known limitations](#known-limitations) list two further points that may have influenced both results.

![CV debug images of a 4-object scene](docs/images/cv_debug_count4.png)

*(a) original frame, (b) grayscale, (c) Otsu binary image computed in the dashboard (artefacts at the border), (d) STM32 result overlay: the classical pipeline on the chip returned `count=0`, TinyML classified the same frame correctly as 4 objects.*

> The 97 % in the [training results](#training-results) is the validation accuracy on images from the training domain, not the result on the real-world test above.

## Known limitations

Found in a later code review, after the measurements above:

1. **Otsu overflow in the firmware (`cv_engine.c`, `cv_otsu_threshold`).** The between-class score is squared in `uint64_t` and overflows for typical QVGA histograms. In a simulation of the exact C arithmetic, a scene with a bright background and dark objects gave a threshold of 46 instead of 92. The classical pipeline was measured with this version, so its 14.4 % is probably too pessimistic. A corrected version (floating-point score) is prepared but has **not been re-measured yet**.
2. **Training vs. inference preprocessing.** The checked-in `user_config.yaml` trains with `aspect_ratio: fit` (letterboxing), while the firmware stretches the full frame to 96×96. This mismatch may contribute to the weak 3- and 4-object classes, in addition to the domain shift.

Both points are the first things to fix before a re-measurement.

---

## Table of contents

1. [Results](#results)
2. [System architecture](#system-architecture)
3. [Repository structure](#repository-structure)
4. [Hardware setup](#hardware-setup)
5. [Quick start — GUI](#quick-start--gui)
6. [Quick start — ML training](#quick-start--ml-training)
7. [Model specification](#model-specification)
8. [Training results](#training-results)
9. [CV pipeline](#cv-pipeline)
10. [UART protocol](#uart-protocol)
11. [Dataset capture workflow](#dataset-capture-workflow)
12. [Training workflow](#training-workflow)
13. [STM32 deployment (X-CUBE-AI)](#stm32-deployment-x-cube-ai)
14. [Benchmarking](#benchmarking)
15. [Troubleshooting](#troubleshooting)
16. [Dependencies](#dependencies)

---

## System architecture

```mermaid
flowchart LR
    subgraph STM32H7["STM32H7 Nucleo-144"]
        CAM["OV2640\nCamera"] -->|DCMI/DMA| FB["Frame Buffer\nRGB565 / JPEG"]
        FB --> CV["CV Engine\nOtsu · Morph · CCL"]
        FB --> TM["TinyML Engine\nMobileNetV1-0.25\nX-CUBE-AI"]
        CV --> UART["UART TX\n2 Mbit/s"]
        TM --> UART
        FB --> UART
    end
    subgraph PC["PC — PySide6 Dashboard"]
        UART -->|USB/VCP| RX["RX Parser\nprotocol_parser.py"]
        RX --> DISP["Display\nPreview · CV · ML"]
        RX --> BENCH["Benchmark\nCSV Export"]
        RX --> DS["Dataset Capture\nRGB565 / QVGA"]
    end
```

---

## Repository structure

```text
Machine-Vision-on-Microcontrollers/
│
├─ GUI_App/                              # PySide6 Reference Dashboard
│  └─ app/
│     ├─ src/
│     │  ├─ main.py                      # Qt application entry point
│     │  ├─ reference_window.py          # Main controller (6 mixins)
│     │  ├─ protocol_parser.py           # STM32 UART protocol parser
│     │  ├─ serial_service.py            # QSerialPort wrapper
│     │  ├─ image_utils.py               # JPEG / RGB565 / GRAY conversion
│     │  ├─ dashboard_controller.py      # Serial + camera mode (DashboardMixin)
│     │  ├─ rx_controller.py             # UART RX path, frame decode (RxMixin)
│     │  ├─ display_controller.py        # Preview refresh, CV/ML labels (DisplayMixin)
│     │  ├─ bench_controller.py          # Benchmark table, CSV export (BenchMixin)
│     │  ├─ save_controller.py           # Frame/log save (SaveMixin)
│     │  ├─ dataset_controller.py        # Automated dataset capture (DatasetMixin)
│     │  ├─ app_constants.py             # Constants, stylesheet
│     │  └─ app_helpers.py               # Path helpers, FrameTransfer, norm01
│     ├─ ui/
│     │  └─ reference_window.ui          # Qt Designer layout
│     └─ assets/
│        └─ app_icon.ico
│
├─ CubeIDE_Workspace/
│  └─ STM32_H7Firmware/                  # STM32CubeIDE project
│     ├─ Core/
│     │  ├─ Src/
│     │  │  ├─ main.c                    # Buffer alloc, MPU, task creation
│     │  │  ├─ camera_app.c/.h           # UART command handler, FreeRTOS tasks
│     │  │  ├─ camera_capture.c/.h       # DCMI/DMA frame capture
│     │  │  ├─ camera_proto.c/.h         # UART protocol formatters
│     │  │  ├─ camera_parse.c/.h         # Command parser
│     │  │  ├─ cv_engine.c/.h            # CV pipeline (7 stages)
│     │  │  ├─ tinyml_engine.c/.h        # X-CUBE-AI inference wrapper
│     │  │  ├─ tinyml_preprocess.c/.h    # RGB565 → uint8 96×96 tensor
│     │  │  ├─ uart_tx.c/.h              # Non-blocking DMA UART TX
│     │  │  └─ ov2640_Drive.c/.h         # OV2640 camera driver
│     │  └─ Inc/                         # Corresponding header files
│     └─ X-CUBE-AI/                      # Generated by STM32CubeMX
│        └─ App/
│           ├─ network.c/.h              # Generated AI network kernel
│           ├─ network_data.c/.h         # Model weights (Flash)
│           ├─ network_data_params.c/.h  # Weight parameters
│           ├─ network_config.h          # Network configuration
│           ├─ network_generate_report.txt # X-CUBE-AI analysis report
│           └─ app_x-cube-ai.c/.h        # Integration layer
│
├─ ML training/                          # PC-side TinyML workflow
│  ├─ dataset/                           # git-ignored — keep locally
│  │  ├─ train/ count_0…count_4/         # ≥ 100 images per class
│  │  └─ val/   count_0…count_4/         # ≥ 20 images per class
│  ├─ src/
│  │  ├─ train_count_model.py            # Standalone Keras CNN script (alternative to the Model Zoo run)
│  │  ├─ predict_count_tflite.py         # PC-side TFLite verification + hash check
│  │  └─ check_dataset.py                # Dataset structure validator
│  └─ Model/
│     ├─ user_config.yaml                # ST Model Zoo training config
│     └─ 2026_05_03_09_11_10/            # Training run artefacts
│        ├─ Training_curves.png          # Loss / accuracy curves
│        ├─ logs/metrics/train_metrics.csv
│        └─ quantized_models/quantized_model.tflite   # ← deployable model (214 KiB)
│
├─ requirements.txt                      # Unified Python dependencies
└─ README.md
```

---

## Hardware setup

| Part | Details |
|------|---------|
| MCU board | STM32H743ZI — Nucleo-144 |
| Camera | OV2640, connected via DCMI + DMA |
| UART | USART3 → ST-LINK VCP, **2 000 000 Baud, 8N1** |
| LED (heartbeat) | Red LED PB14 — 1 Hz blink = firmware running |
| Push button | PC13 — manual snapshot trigger |

**Memory layout**

| Region | Size | Content |
|--------|------|---------|
| RAM_D1 | 512 KB | Frame buffer (255 KB) + CV binary buffer (128 KB) |
| RAM_D2 | 288 KB | CV temp buffer (128 KB) + CV background buffer (128 KB) |
| RAM_D3 | 64 KB | TinyML activation buffer (40 KB) |
| Flash | ~285 KB | X-CUBE-AI runtime + network weights (214 KB) |

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
The "STM32 ready" indicator appears after `OV2640 ready` is received.

**Basic workflow**

| Step | GUI action | UART command |
|------|-----------|--------------|
| 1 | Mode = RGB → **SNAP** | `MODE RGB` + `SNAP` |
| 2 | **Run STM32 CV** | `CV RUN` |
| 3 | **Run STM32 TinyML** | `TM RUN` |
| 4 | Set GT Count → **Add Run** | — |
| 5 | **Export CSV** | — |

> CV RUN and TM RUN always operate on the **last captured** RGB565 frame.

---

## Quick start — ML training

```bash
# 1. Validate dataset
python "ML training/src/check_dataset.py" --root "ML training/dataset"

# 2. Edit ML training/Model/user_config.yaml
#    Set training_path, validation_path, quantization_path to absolute paths

# 3. Clone ST Model Zoo (once)
cd "ML training/Model"
git clone https://github.com/STMicroelectronics/stm32ai-modelzoo-services
pip install -e stm32ai-modelzoo-services

# 4. Train + quantise (chain_tqe)
cd stm32ai-modelzoo-services/image_classification/tf
python stm32ai_main.py \
    --config-path ../../.. \
    --config-name user_config.yaml

# 5. Verify on PC
python "ML training/src/predict_count_tflite.py" \
    --model "ML training/Model/2026_05_03_09_11_10/quantized_models/quantized_model.tflite" \
    --image path/to/frame.png \
    --show-hash
```

---

## Model specification

| Field | Value |
|-------|-------|
| Architecture | MobileNetV1 α=0.25, depthwise separable convolutions |
| Input shape | **96 × 96 × 3** (H × W × C) |
| Colour space | **RGB** (3 channels) |
| Input dtype | **uint8 \[0…255\]** |
| Quantisation | Post-training, `QLinear(scale=1/127.5, zero_point=127)` |
| Output | float32 \[5\], Softmax probabilities |
| Classes | `count_0`, `count_1`, `count_2`, `count_3`, `count_4` |
| Parameters | 211,621 |
| MACC (ops) | **7,550,858** |
| Weights — Flash | **219,844 B (214.7 KiB)** |
| Activations — RAM | **41,152 B (40.2 KiB)** |
| Total Flash (incl. runtime) | **285,351 B (~279 KB)** |
| Total RAM (incl. runtime) | **57,876 B (~57 KB)** |
| Resize method (firmware) | **Nearest-neighbour, full-frame stretch** (no padding, no letterbox) |

### ⚠ Preprocessing — firmware and training should match

The STM32 firmware (`tinyml_preprocess.c`) maps each pixel with integer-floor nearest-neighbour and stretches the full frame:

```c
sx = ox * src_width  / 96;   // integer floor — no rounding
sy = oy * src_height / 96;
```

The checked-in `ML training/Model/user_config.yaml` uses `interpolation: nearest`, `color_mode: rgb` and **`aspect_ratio: fit`** (letterboxing). `fit` differs from the firmware's stretch, a known limitation (see [Known limitations](#known-limitations)). For a matching pipeline set `aspect_ratio: stretch` and retrain.

### Common misconfigurations

| Wrong value | Correct value | Impact |
|-------------|--------------|--------|
| `input_shape: (48, 48, 1)` | `(96, 96, 3)` | Wrong model size |
| `color_mode: grayscale` | `rgb` | 1-channel vs 3-channel mismatch |
| `aspect_ratio: fit` (current config) | `stretch` | Letterbox in training, stretch on the chip: domain shift |
| `board: STM32H747I-DISCO` | `NUCLEO-H743ZI2` | Wrong benchmarking target |

---

## Training results

**Run:** `2026_05_03_09_11_10` — ST Model Zoo `chain_tqe` (train → quantise → evaluate)

| Metric | Value |
|--------|-------|
| Best validation accuracy | **97.14 %** (epochs 35, 36, 43, 44) |
| Best training accuracy | **99.97 %** (epoch 55) |
| Final validation accuracy | **94.29 %** (epoch 55) |
| Training epochs | 56 logged (epochs 0–55, up to 100 configured) |
| LR schedule | 1e-3 → 5e-4 (ep 30) → 2.5e-4 (ep 44) → 1.25e-4 (ep 52) |
| Framework | TensorFlow 2 / Keras, ST Model Zoo |

![Training curves](ML%20training/Model/2026_05_03_09_11_10/Training_curves.png)

> Training on real OV2640 frames captured with the Dataset Capture tool. The real-world test in [Results](#results) used new scenes under different lighting, which is why its accuracy is lower than the validation accuracy here.  
> The preprocessing pipeline (nearest-floor resize, full-frame stretch, RGB) exactly mirrors the STM32 firmware.

**X-CUBE-AI analysis summary** (`network_generate_report.txt`)

| Layer type | Count | % of MACC |
|------------|-------|-----------|
| Conv2D (standard + depthwise) | 27 + 14 = 41 | 99.3 % |
| GlobalAvgPool + Dense + Softmax | 3 | 0.05 % |
| Input conversion (u8→s8) | 1 | 0.7 % |

---

## CV pipeline

```mermaid
flowchart TD
    A["RGB565 Frame\n(up to 480×272)"] --> B["Stage 1\nRGB565 → Grayscale\nBT.601 luma"]
    B --> C{"BG subtraction\nenabled?"}
    C -- yes --> D["Stage 2\nAbsolute diff\nvs reference frame"]
    C -- no --> E
    D --> E["Stage 3\nSpatial filter\nBox k=3…15 / Median k=3…5"]
    E --> F["Stage 4\nThreshold\nManual or Otsu"]
    F --> G["Stage 5\nMorphology\nOpen / Close / Erode / Dilate"]
    G --> H["Stage 6\nRun-length CCL\n+ Union-Find\n(4 or 8 connectivity)"]
    H --> I["Stage 7\nObject filter\nArea · Aspect ratio\nCircularity · Border"]
    I --> J["CVSTAT + CVBOX\n→ UART → GUI"]
```

**CV command reference**

| Command | Effect |
|---------|--------|
| `CV RUN` | Run full pipeline on last RGB565 frame |
| `CV GET` | Send current config (CVCFG) |
| `CV EN 0\|1` | Disable / enable CV engine |
| `CV PRESET 0..3` | CUSTOM / FAST / ROBUST / ACCURATE |
| `CV THRMODE 0\|1` | Manual / Otsu auto-threshold |
| `CV THR 0..255` | Manual threshold value |
| `CV INV 0\|1` | Invert binary image |
| `CV FILTER 0..2` | OFF / BOX / MEDIAN |
| `CV BLUR 0..7` | Pre-threshold blur kernel (0=off, 1→3×3 … 7→15×15) |
| `CV MORPHMODE 0..4` | OFF / OPEN / CLOSE / ERODE / DILATE |
| `CV MORPH 0..7` | Morphology kernel size |
| `CV CON 4\|8` | CCL connectivity |
| `CV MINAREA n` | Minimum object area (px²) |
| `CV MAXAREA n` | Maximum object area (0=unlimited) |
| `CV ASPECT min max` | Aspect ratio filter (×1000) |
| `CV CIRC min` | Circularity minimum (×1000, edge-count based metric) |
| `CV BGCAP` | Capture current frame as background |
| `CV BGSUB 0\|1` | Enable background subtraction |
| `CV BORDFILT 0\|1` | Reject large blobs touching the border (≥ 85 % of width/height or ≥ 35 % of the area) |
| `CV ROI 1 x y w h` | Set region of interest |
| `CV ROI 0` | Disable ROI |

---

## UART protocol

**Connection:** USART3, **2 000 000 Baud, 8N1**, no flow control

### Frame transfer headers (raw bytes follow immediately after `\r\n`)

```
JPG: <bytes>
RGB565: <width> <height> <bytes>
```

### Status / log lines

```
INFO:  <message>
WARN:  <message>
ERR:   <message>
DEBUG: <message>
STAT:  FPS=X.X SIZE=XB HEAP=XKB FB=XKB LAT=Xms
```

### Classical Vision (CV)

```
CVCFG: EN=1 PRESET=0 THR=128 THRMODE=0 INV=0
       BLUR=0 FILTER=0 MORPH=0 MORPHMODE=0 CON=8
       MIN=50 MAX=0 ARMIN=0 ARMAX=0 CIRCMIN=0
       BORDFILT=1 BGSUB=0 BGCAP=0
       ROIEN=0 ROIX=0 ROIY=0 ROIW=0 ROIH=0

CVSTAT: COUNT=N MEAN=X MAX=X MIN=X BRIGHT=X TIME=Xms
        REJSMALL=X REJLARGE=X REJBORDER=X REJSHAPE=X
        FGPIX=X RAWCOMP=X BOXES=N
CVBOX:  ID=N AREA=X X=X Y=X W=X H=X PERI=X CIRC=X
CVDONE
```

### TinyML

```
TMCFG:  EN=1 INPUT=96x96x3 CLASSES=5 MODEL=count_model
TMINFO: STATUS=XCUBEAI_OK RAM=41KB FLASH=215KB
TMRES:  CLASS=COUNT_N IDX=N CONF=XXX TIME=Xms UNCERTAIN=0
TMPROB: IDX=N NAME=COUNT_N SCORE=XXX
TMDONE
```

> `CONF` and `SCORE` are in **permille (0…1000)**. The GUI normalises to 0.0–1.0.  
> When `UNCERTAIN=1`, `CLASS=UNCERTAIN` and `IDX=-1` — the GUI marks it as uncertain, not a real class.

**TinyML commands**

| Command | Effect |
|---------|--------|
| `TM RUN` | Run inference on last RGB565 frame |
| `TM GET` | Request TMCFG + TMINFO |
| `TM EN 0\|1` | Enable / disable TinyML |

---

## Dataset capture workflow

```mermaid
flowchart LR
    A["Dashboard\nDataset tab"] -->|"Mode=RGB\nSNAP"| B["OV2640\nQVGA 320×240"]
    B -->|"RGB565 over UART"| C["GUI saves\n.png to disk"]
    C --> D["train/count_N/\nval/count_N/"]
    D --> E["check_dataset.py\nvalidate structure"]
    E --> F["Training pipeline\nuser_config.yaml"]
    F --> G["quantized_model.tflite\n→ X-CUBE-AI → STM32"]
```

1. Connect STM32 → GUI → Dataset tab.
2. Select **split** (`train` / `val`) and **label** (`count_0`…`count_4`).
3. Click **Capture N frames** — the GUI automatically snaps and saves.
4. Collect **≥ 100 training** and **≥ 20 validation** images per class (see `ML training/docs/DATASET_GUIDE.md`).
5. With "Force RGB / QVGA" ticked, the PNGs are 320×240 RGB — identical to the firmware inference input (otherwise JPEG frames are saved).

---

## Training workflow

### 1. Validate dataset

```bash
python "ML training/src/check_dataset.py" --root "ML training/dataset"
```

### 2. Configure `user_config.yaml`

```yaml
dataset:
  training_path:     "/abs/path/to/dataset/train"
  validation_path:   "/abs/path/to/dataset/val"
  quantization_path: "/abs/path/to/dataset/train"

preprocessing:
  resizing:
    interpolation: nearest
    aspect_ratio: stretch    # ← must match the firmware (the checked-in config still has "fit")
  color_mode: rgb            # ← critical

model:
  model_name: mobilenetv1_a025

training:
  epochs: 100
  batch_size: 16
```

### 3. Train (ST Model Zoo)

```bash
cd "ML training/Model/stm32ai-modelzoo-services/image_classification/tf"
python stm32ai_main.py \
    --config-path ../../.. \
    --config-name user_config.yaml
```

Output: `experiments_outputs/<timestamp>/quantized_models/quantized_model.tflite`

### 4. Verify preprocessing parity on PC

```bash
python "ML training/src/predict_count_tflite.py" \
    --model "ML training/Model/2026_05_03_09_11_10/quantized_models/quantized_model.tflite" \
    --image path/to/frame.png --show-hash
```

Compare `hash=XXXXXXXX` with the STM32 log `TM_IN: hash=...`. Note: PNGs saved by the GUI expand RGB565 → RGB888 with rounding, the firmware uses floor, so the hashes can differ slightly even when the resize is identical.

---

## STM32 deployment (X-CUBE-AI)

```mermaid
flowchart LR
    A["quantized_model.tflite"] --> B["STM32CubeMX\nX-CUBE-AI → Import"]
    B --> C["Analyse\nverify input/output shapes"]
    C --> D["Generate Code\n→ network.c / network_data.c"]
    D --> E["STM32CubeIDE\nRebuild + Flash"]
    E --> F["STM32 running\nnew model"]
```

1. Open **STM32CubeMX** → X-CUBE-AI → Import `quantized_model.tflite`.
2. Click **Analyse** — verify:
   - Input: `uint8(1×96×96×3)` — `QLinear(0.00784, 127)`
   - Output: `float32(1×5)`
   - Activations: **40.2 KiB**, Weights: **214.7 KiB**
3. **Generate Code** → rebuild in STM32CubeIDE.
4. `tinyml_engine.c` requires **no changes** after regeneration.

### X-CUBE-AI v10.2 — two mandatory lines in `tinyml_init()`

```c
// 1. Initialise STAI runtime before any network call.
//    Without this, ai_network_run() always produces constant outputs.
stai_runtime_init();

// 2. Pass NULL for weights — ai_network_data_params_get() sets the
//    correct Flash pointer internally. Passing the weights table directly
//    overrides it with the wrong address.
ai_network_create_and_init(&g_network, activations, NULL);
```

---

## Benchmarking

For a **reproducible CV vs TinyML comparison**:

1. `SNAP` — capture one RGB565/QVGA frame.
2. `CV THRMODE 1` + `CV RUN` — Otsu auto-threshold (no manual tuning).
3. `TM RUN` — inference on the **same frame** (no new SNAP).
4. Set **GT Count** in the GUI → **Add Current Run**.
5. Repeat for scenes with 0, 1, 2, 3, 4 objects.
6. **Export CSV** → analyse accuracy, timing, uncertain predictions.

**Measured on the STM32H743 @ 480 MHz** (mean of n = 90 runs, `HAL_GetTick()`, 1 ms resolution)

| Operation | Mean latency |
|-----------|--------------|
| CV RUN (Otsu, QVGA) | 66.6 ms (± 0.5 ms) |
| TM RUN (inference) | 59.8 ms (± 0.2 ms) |

Both run times are nearly constant and independent of the scene.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `TM_RAW_OUT` always `[0.5, 0.5, 0, 0, 0]` | `stai_runtime_init()` not called | Add before `ai_network_create_and_init()` |
| Constant output for all inputs | `weights=g_network_weights_table` | Pass `NULL` as weights argument |
| PC hash ≠ STM32 hash | Preprocessing mismatch | Check `color_mode: rgb`, `TINYML_PREPROCESS_BYTESWAP=0`; RGB565 expansion differs slightly (round vs. floor) |
| All predictions = count_0 | Domain shift (wrong train data) | Retrain with real OV2640 frames via Dataset Capture |
| `UNCERTAIN` on all frames | Confidence below threshold | Lower `TINYML_UNCERTAIN_THRESHOLD_PERMILLE` or retrain |
| Red LED not blinking | Firmware not running | Check build, power, ST-LINK |
| GUI shows no frames | Wrong port or baud | Verify 2 000 000 Baud, firmware flashed |
| CV returns 0 objects | Wrong threshold or too small area | Use Otsu mode; lower `CV MINAREA` |
| `CV RUN` error: "needs RGB565 frame" | JPEG mode active | Switch to RGB mode, press SNAP first |
| Build error: missing `stai.h` | X-CUBE-AI not generated | Re-run CubeMX code generation |

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

```bash
pip install -r requirements.txt
```

---

## Contributors

| Name | Role |
|------|------|
| Mutasem Bader | STM32 firmware (FreeRTOS, DCMI/DMA, CV engine, TinyML integration, UART protocol), ML training pipeline, measurements, system integration |

The PySide6 dashboard (`GUI_App/`) was created with AI assistance.

---

## License

Copyright (c) 2026 Mutasem Bader — All Rights Reserved.  
Viewing is permitted. Copying, modifying, or submitting as own work is strictly prohibited.  
See [LICENSE](LICENSE) for details.
