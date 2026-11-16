# Troubleshooting and dependencies

Back to the [README](../README.md).

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
| Build error: missing `stai.h`, `bsp_ai.h` or `aiValidation.h` | X-CUBE-AI files not generated (`X-CUBE-AI/Target/` and the validation files are git-ignored) | Open the `.ioc` in STM32CubeMX and run **Generate Code** (mandatory after cloning) |

## Dependencies

```
PySide6>=6.6.0          GUI + serial port (QtSerialPort)
Pillow>=10.3.0
tensorflow==2.16.2      training + TFLite quantisation
numpy>=1.24.0,<2.0
```

Python 3.10 to 3.12. The ST Model Zoo workflow needs its own environment (see [training.md](training.md)).

```bash
pip install -r requirements.txt
```

