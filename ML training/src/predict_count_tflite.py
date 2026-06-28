#!/usr/bin/env python3
"""
predict_count_tflite.py
=======================
Run the quantized MobileNetV1 counting model on a single image or a folder
of images and print the class predictions.

The preprocessing pipeline is intentionally identical to the STM32 firmware
(tinyml_preprocess.c) so you can cross-check STM32 TM_IN diagnostics against
the PC output.

Usage
-----
  # Single image
  python predict_count_tflite.py --model path/to/quantized_model.tflite \
                                  --image path/to/frame.png

  # Folder of images (prints per-image results + accuracy if --labels given)
  python predict_count_tflite.py --model path/to/quantized_model.tflite \
                                  --folder path/to/test/count_2 \
                                  --true-class count_2

  # Hash comparison with STM32 TM_IN log
  python predict_count_tflite.py --model path/to/quantized_model.tflite \
                                  --image frame.png --show-hash
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------
# Constants — must match user_config.yaml and tinyml_preprocess.h
# ---------------------------------------------------------------------------
INPUT_W      = 96
INPUT_H      = 96
INPUT_C      = 3
CLASS_NAMES  = ["count_0", "count_1", "count_2", "count_3", "count_4"]


# ---------------------------------------------------------------------------
# Preprocessing — matches tinyml_preprocess.c exactly
# ---------------------------------------------------------------------------

def preprocess_image(img: Image.Image) -> np.ndarray:
    """
    Resize with nearest-neighbour (integer-floor mapping, full-frame stretch)
    and return a uint8 array of shape (1, 96, 96, 3).

    This replicates the STM32 firmware behaviour (tinyml_preprocess.c):
      sx = ox * src_w // INPUT_W
      sy = oy * src_h // INPUT_H
    """
    src_w, src_h = img.size
    if img.mode != "RGB":
        img = img.convert("RGB")

    arr_src = np.array(img, dtype=np.uint8)
    arr_dst = np.zeros((INPUT_H, INPUT_W, INPUT_C), dtype=np.uint8)
    for oy in range(INPUT_H):
        for ox in range(INPUT_W):
            sx = ox * src_w // INPUT_W
            sy = oy * src_h // INPUT_H
            sx = min(sx, src_w - 1)
            sy = min(sy, src_h - 1)
            arr_dst[oy, ox, :] = arr_src[sy, sx, :]

    return arr_dst[np.newaxis, ...]  # [1, 96, 96, 3]


def fnv1a_32(data: bytes) -> int:
    """FNV-1a 32-bit hash — identical to STM32 send_input_diagnostics()."""
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------------------
# Inference
# ---------------------------------------------------------------------------

def run_tflite(model_path: str, inp: np.ndarray) -> np.ndarray:
    """Run TFLite interpreter and return float32 output [5]."""
    try:
        import tensorflow as tf
        interp = tf.lite.Interpreter(model_path=model_path)
    except ImportError:
        try:
            from ai_edge_litert.interpreter import Interpreter
            interp = Interpreter(model_path=model_path)
        except ImportError:
            print("ERROR: Neither tensorflow nor ai_edge_litert is installed.")
            print("  pip install tensorflow   or   pip install ai-edge-litert")
            sys.exit(1)

    interp.allocate_tensors()
    inp_det = interp.get_input_details()[0]
    out_det = interp.get_output_details()[0]

    # Validate input dtype and shape
    if inp_det["dtype"] != np.uint8:
        print(f"WARNING: model expects {inp_det['dtype']}, got uint8 — check model.")
    if list(inp_det["shape"]) != [1, INPUT_H, INPUT_W, INPUT_C]:
        print(f"WARNING: model input shape {inp_det['shape']} != [1,{INPUT_H},{INPUT_W},{INPUT_C}]")

    interp.set_tensor(inp_det["index"], inp)
    interp.invoke()
    return interp.get_tensor(out_det["index"]).squeeze()


def predict(model_path: str, img_path: str,
            show_hash: bool = False) -> tuple[str, float, np.ndarray]:
    img = Image.open(img_path)
    inp = preprocess_image(img)

    if show_hash:
        h = fnv1a_32(inp.tobytes())
        stats = inp.squeeze()
        print(f"  Input stats: min={stats.min()} max={stats.max()} "
              f"mean={stats.mean():.1f} hash={h:08X}")
        print(f"  HEAD32: {' '.join(f'{b:02X}' for b in inp.tobytes()[:32])}")

    scores = run_tflite(model_path, inp)
    best   = int(np.argmax(scores))
    return CLASS_NAMES[best], float(scores[best]), scores


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description="TFLite counting model predictor")
    ap.add_argument("--model",  required=True, help="Path to quantized_model.tflite")
    ap.add_argument("--image",  help="Single image file (PNG/JPG)")
    ap.add_argument("--folder", help="Folder of images")
    ap.add_argument("--true-class", dest="true_class",
                    help="Expected class name for accuracy reporting")
    ap.add_argument("--show-hash", action="store_true",
                    help="Print input tensor hash / statistics for STM32 cross-check")
    args = ap.parse_args()

    if not args.image and not args.folder:
        ap.error("Provide --image or --folder")

    model = args.model
    if not Path(model).exists():
        print(f"ERROR: model not found: {model}")
        sys.exit(1)

    if args.image:
        name, conf, scores = predict(model, args.image, args.show_hash)
        print(f"\n{args.image}")
        print(f"  Prediction : {name}  ({conf*100:.1f}%)")
        for i, s in enumerate(scores):
            marker = "  <--" if i == int(name[-1]) else ""
            print(f"    {CLASS_NAMES[i]}: {s*100:.1f}%{marker}")

    elif args.folder:
        folder  = Path(args.folder)
        images  = sorted(folder.glob("*.png")) + sorted(folder.glob("*.jpg"))
        correct = 0
        for p in images:
            name, conf, _ = predict(model, str(p), args.show_hash)
            ok = (name == args.true_class) if args.true_class else None
            flag = "✓" if ok else ("✗" if ok is False else "")
            print(f"{flag:2} {p.name:40s}  -> {name}  ({conf*100:.1f}%)")
            if ok:
                correct += 1
        if args.true_class and images:
            print(f"\nAccuracy: {correct}/{len(images)} = {100*correct/len(images):.1f}%")


if __name__ == "__main__":
    main()
