# Firmware: image processing, deployment and benchmarking

Back to the [README](../README.md).

## Architecture

The firmware runs on FreeRTOS (1 kHz tick, 48 KiB heap). All peripheral interrupts (DCMI, USART3, the two DMA streams, the push button) have interrupt priority 5, so they may call the `...FromISR` functions of FreeRTOS. There are four tasks:

| Task | Priority | Stack | What it does |
|---|---|---|---|
| `HB` | 1 | 128 words | toggles the red LED every 500 ms |
| `UART_TX` | 2 | 768 words | takes jobs from a queue of 8 entries and sends them over USART3 with DMA |
| `CAM_APP` | 3 | 4096 words | reads the commands, takes snapshots, runs the CV engine and the network |
| `CAM_STREAM` | 4 | 1024 words | sends continuous JPEG frames while a stream is on |

How the data moves:

- **Commands in.** The USART3 receive interrupt hands every byte to a queue of 512 bytes. `CAM_APP` assembles lines from this queue (`camera_parse.c`), looks at the command and calls the camera driver, the CV engine or the TinyML engine. The push button wakes the same task with a task notification and takes a snapshot.
- **Frames.** The camera sends its pixels over DCMI, and DMA writes them straight into the frame buffer in RAM_D1. A single frame is taken in DCMI snapshot mode with a timeout, the stream uses continuous mode. The CV engine and the network read from this buffer, so both work on the same pixels. JPEG frames are cut out of the buffer between their start and end marker before they are sent.
- **Everything out.** All output goes through `uart_tx.c`. A text line is copied into the queue slot (at most 255 characters). A frame is queued as a pointer and a length, and the task that queued it gets a task notification when the last DMA chunk (at most 32,768 bytes) has been sent. This is how the stream task knows that the frame buffer may be written again.
- **Caches.** The buffers are in RAM that the MPU marks as non-cacheable. Where the driver and the UART code hand memory to DMA, they also clean or invalidate the data cache for that range.

The files:

| File | Content |
|---|---|
| `main.c` | buffers and their placement, MPU setup, task creation, interrupt callbacks |
| `camera_app.c` | command handling and the two camera tasks |
| `camera_parse.c` | line assembly from the receive queue |
| `camera_capture.c` | snapshot, mode and resolution changes, JPEG bounds |
| `camera_proto.c` | formatting of the lines that go to the PC |
| `cv_engine.c` | the CV pipeline |
| `tinyml_engine.c`, `tinyml_preprocess.c` | X-CUBE-AI wrapper and the conversion of the frame to the network input |
| `uart_tx.c` | the transmit queue and task |
| `ov2640_Drive.c`, `ov2640_regs.c` | OV2640 driver and register lists (adapted from code found on the Internet, see the notices) |

## CV pipeline

The CV engine works on the last RGB565 frame (up to 480×272 pixels) in these stages:

1. RGB565 to grayscale (BT.601 luma)
2. Background subtraction (optional): absolute difference to a stored reference frame
3. Spatial filter: box (kernel 3 to 15) or median (kernel 3 to 5)
4. Threshold: manual or Otsu
5. Morphology: open, close, erode or dilate
6. Connected components: run-length labelling with union-find, 4 or 8 connectivity
7. Object filter: area, aspect ratio, circularity, border

The result goes to the dashboard as `CVSTAT` and `CVBOX` lines.

The commands:

| Command | Effect |
|---|---|
| `CV RUN` | Run the pipeline on the last RGB565 frame |
| `CV GET` | Send the current configuration (`CVCFG`) |
| `CV EN 0\|1` | Switch the CV engine off or on |
| `CV PRESET 0..3` | CUSTOM, FAST, ROBUST or ACCURATE |
| `CV THRMODE 0\|1` | Manual threshold or Otsu |
| `CV THR 0..255` | Manual threshold |
| `CV INV 0\|1` | Invert the binary image |
| `CV FILTER 0..2` | Filter off, box or median |
| `CV BLUR 0..7` | Kernel of the filter before the threshold (0 is off, 1 is 3×3, up to 7 for 15×15) |
| `CV MORPHMODE 0..4` | Off, open, close, erode or dilate |
| `CV MORPH 0..7` | Kernel size of the morphology |
| `CV CON 4\|8` | Connectivity of the labelling |
| `CV MINAREA n` | Smallest object area in pixels |
| `CV MAXAREA n` | Largest object area (0 means no limit) |
| `CV ASPECT min max` | Aspect ratio filter, times 1000 |
| `CV CIRC min` | Smallest circularity, times 1000 (based on counting edge pixels) |
| `CV BGCAP` | Store the current frame as background, for the current frame size and ROI (`CV RUN` discards it with a warning when one of them has changed) |
| `CV BGSUB 0\|1` | Background subtraction off or on |
| `CV BORDFILT 0\|1` | Reject large blobs that touch the border (at least 85 % of the width or height, or at least 35 % of the area) |
| `CV ROI 1 x y w h` | Set the region of interest |
| `CV ROI 0` | Switch the ROI off |

## Deployment with X-CUBE-AI

### Building from a fresh clone

Not all X-CUBE-AI files are in the repository. `X-CUBE-AI/Target/` and the validation files in `X-CUBE-AI/App/` (for example `aiValidation.*` and `ai_device_adaptor.*`) are git-ignored, but `app_x-cube-ai.c` includes `bsp_ai.h` and `aiValidation.h` and calls `MX_UARTx_Init()` and `aiValidationInit()`. So a fresh clone does not build before the code has been generated:

1. Open `CubeIDE_Workspace/STM32_H7Firmware/STM32_H7Firmware.ioc` in STM32CubeMX and run Generate Code.
2. Open the project in STM32CubeIDE, build and flash.

The `.ioc` points to the model with a path that starts with the placeholder `<repo-root>` (an `experiments_outputs` run folder that is not part of the repository). If CubeMX cannot find it, select `quantized_model.tflite` again in the X-CUBE-AI settings, as described below.

### Updating the model

1. In STM32CubeMX, open X-CUBE-AI and import `quantized_model.tflite`.
2. Run Analyse and check the numbers: input `uint8(1×96×96×3)` with `QLinear(0.00784, 127)`, output `float32(1×5)`, activations 40.2 KiB, weights 214.7 KiB.
3. Generate Code and rebuild. `tinyml_engine.c` does not need changes.

### Two lines in `tinyml_init()` that must not be left out (X-CUBE-AI 10.2)

```c
// Initialise the STAI runtime before any network call. Without this,
// ai_network_run() always produces the same constant output.
stai_runtime_init();

// Pass NULL for the weights: ai_network_data_params_get() sets the correct
// flash pointer internally. Passing the weights table directly overrides it
// with the wrong address.
ai_network_create_and_init(&g_network, activations, NULL);
```

## Benchmarking

To compare CV and TinyML on the same frame:

1. `SNAP` captures one RGB565 frame at QVGA.
2. `CV THRMODE 1` and `CV RUN` run the CV engine with the Otsu threshold, so no manual tuning is involved.
3. `TM RUN` runs the network on the same frame, without a new `SNAP`.
4. In the dashboard, enter the true count and press Add Current Run.
5. Repeat for scenes with 0, 1, 2, 3 and 4 objects.
6. Export the table as CSV and look at accuracy, timing and the uncertain predictions.

Measured on the STM32H743 at 480 MHz: the mean of five consecutive runs per scene, several scenes, `HAL_GetTick()` with 1 ms resolution, firmware built with Arm GNU Toolchain 14.2 and `-Os`.

| Operation | Mean time |
|---|---|
| `CV RUN`, Otsu, preset FAST (no filter, no morphology), QVGA | 37 to 55 ms |
| `CV RUN`, Otsu, preset ROBUST (3×3 box, 3×3 opening), QVGA | 161 to 166 ms |
| `CV RUN`, Otsu, preset ACCURATE (5×5 median, 5×5 opening), QVGA | 551 to 704 ms |
| `TM RUN` (inference) | 53 ms |

The spread between the five runs of one scene was at most 1 ms. The CV time depends on the scene by up to 30 %; the TinyML time did not change. The activation buffer of the network is in DTCM. In the non-cacheable RAM_D3, where it was before, an inference took 157 ms.
