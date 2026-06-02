#!/usr/bin/env python3
"""
train_count_model.py

Standalone training and export script for the STM32 Edge Vision TinyML counting
model.

Purpose
-------
This script trains a reusable Keras image-classification model for the classes:

    count_0, count_1, count_2, count_3, count_4

The exported quantized TFLite model is designed for the STM32 TinyML firmware
input contract:

    input shape : [1, 96, 96, 3]
    input dtype : uint8
    input range : [0, 255]
    color order : RGB / HWC

Preprocessing contract
----------------------
Every image is preprocessed as follows:

    image file
        -> RGB conversion
        -> optional RGB565 precision simulation on the full-resolution image
        -> integer-floor nearest-neighbour resize to 96 x 96
        -> RGB HWC uint8 tensor in range [0, 255]

The model itself starts with a Rescaling layer:

    x -> x / 127.5 - 1.0

This mirrors the STM32 Model Zoo configuration where preprocessing uses
rescaling scale=1/127.5 and offset=-1. For the exported uint8 TFLite model, the
runtime input remains raw uint8. The normalization is represented inside the
model graph / quantization path.

Dataset layout
--------------
Expected folder structure:

    dataset/
      train/
        count_0/
        count_1/
        count_2/
        count_3/
        count_4/
      val/
        count_0/
        count_1/
        count_2/
        count_3/
        count_4/

Usage
-----
Install dependencies:

    python -m pip install numpy pillow tensorflow

Create folders only:

    python train_count_model.py --init-dirs

Train and export:

    python train_count_model.py \
        --dataset "E:/Studium_Projekte/STM32_MERO2/ML training/dataset" \
        --artifacts "E:/Studium_Projekte/STM32_MERO2/ML training/artifacts" \
        --epochs 100 --batch-size 16

Then verify one image:

    python predict_count_tflite.py \
        --image "E:/Studium_Projekte/STM32_MERO2/ML training/dataset/val/count_3/example.png" \
        --model "E:/Studium_Projekte/STM32_MERO2/ML training/artifacts/count_model_uint8.tflite" \
        --save-preview --save-json

Generated artifacts
-------------------
The script writes:

    labels.txt
    training_config.json
    history.csv
    count_model.keras
    count_model_float.tflite
    count_model_uint8.tflite
    preprocessed_preview/*.png
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

import numpy as np
import tensorflow as tf
from PIL import Image
from tensorflow import keras

# ---------------------------------------------------------------------------
# Project constants. Keep these synchronized with STM32 tinyml_preprocess.h and
# the PC predictor.
# ---------------------------------------------------------------------------

CLASS_NAMES = ["count_0", "count_1", "count_2", "count_3", "count_4"]
NUM_CLASSES = len(CLASS_NAMES)
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp"}

INPUT_W = 96
INPUT_H = 96
INPUT_C = 3
INPUT_SHAPE = (INPUT_H, INPUT_W, INPUT_C)

DEFAULT_BATCH_SIZE = 16
DEFAULT_EPOCHS = 100
DEFAULT_SEED = 42
DEFAULT_ML_ROOT = Path(r"E:\Studium_Projekte\STM32_MERO2\ML training")


@dataclass(frozen=True)
class TrainingConfig:
    """Serializable training configuration stored beside the artifacts."""

    input_shape: tuple[int, int, int]
    input_dtype_after_preprocessing: str
    input_range_after_preprocessing: tuple[int, int]
    class_names: list[str]
    preprocessing: str
    rgb565_simulation: bool
    batch_size: int
    epochs: int
    seed: int
    train_images: int
    val_images: int
    created_utc: str


class ImageSequence(keras.utils.Sequence):
    """Keras Sequence that preprocesses image files batch-by-batch.

    This avoids loading the complete dataset into RAM. Each sample is converted
    to the exact STM32-compatible 96x96 RGB uint8 input. Keras receives float32
    arrays, but their numeric values are still [0, 255]. The first model layer
    performs x / 127.5 - 1.0.
    """

    def __init__(
        self,
        image_paths: list[Path],
        labels: list[int],
        *,
        batch_size: int,
        shuffle: bool,
        seed: int,
        apply_rgb565: bool,
    ) -> None:
        if len(image_paths) != len(labels):
            raise ValueError("image_paths and labels must have the same length.")
        self.image_paths = image_paths
        self.labels = np.asarray(labels, dtype=np.int32)
        self.batch_size = int(batch_size)
        self.shuffle = bool(shuffle)
        self.apply_rgb565 = bool(apply_rgb565)
        self.rng = np.random.default_rng(seed)
        self.indices = np.arange(len(self.image_paths), dtype=np.int32)
        self.on_epoch_end()

    def __len__(self) -> int:
        return int(np.ceil(len(self.image_paths) / self.batch_size))

    def __getitem__(self, batch_index: int) -> tuple[np.ndarray, np.ndarray]:
        start = batch_index * self.batch_size
        stop = min(start + self.batch_size, len(self.image_paths))
        batch_indices = self.indices[start:stop]

        x = np.empty((len(batch_indices), INPUT_H, INPUT_W, INPUT_C), dtype=np.float32)
        y = np.empty((len(batch_indices),), dtype=np.int32)

        for out_i, dataset_i in enumerate(batch_indices):
            image_path = self.image_paths[int(dataset_i)]
            x[out_i] = preprocess_image_to_uint8(image_path, apply_rgb565=self.apply_rgb565).astype(np.float32)
            y[out_i] = self.labels[int(dataset_i)]

        return x, y

    def on_epoch_end(self) -> None:
        if self.shuffle:
            self.rng.shuffle(self.indices)


# ---------------------------------------------------------------------------
# Command line interface
# ---------------------------------------------------------------------------


def default_ml_root() -> Path:
    """Return the preferred project root or the current directory."""
    return DEFAULT_ML_ROOT if DEFAULT_ML_ROOT.exists() else Path.cwd()


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    ml_root = default_ml_root()
    parser = argparse.ArgumentParser(
        description="Train and export a 96x96x3 RGB uint8 TinyML counting model.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--dataset",
        type=Path,
        default=ml_root / "dataset",
        help="Dataset root containing train/ and val/. Default: ML training/dataset",
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        default=ml_root / "artifacts",
        help="Output folder for exported models and reports. Default: ML training/artifacts",
    )
    parser.add_argument("--epochs", type=int, default=DEFAULT_EPOCHS, help="Maximum number of epochs.")
    parser.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE, help="Training batch size.")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Random seed for reproducibility.")
    parser.add_argument(
        "--learning-rate",
        type=float,
        default=1e-3,
        help="Adam learning rate.",
    )
    parser.add_argument(
        "--dropout",
        type=float,
        default=0.30,
        help="Dropout probability before the classifier.",
    )
    parser.add_argument(
        "--patience",
        type=int,
        default=20,
        help="EarlyStopping patience on val_accuracy.",
    )
    parser.add_argument(
        "--no-rgb565-simulation",
        action="store_true",
        help="Disable RGB565 simulation. Use only for ablation tests, not STM32 deployment.",
    )
    parser.add_argument(
        "--no-class-weights",
        action="store_true",
        help="Disable automatic class weighting for imbalanced datasets.",
    )
    parser.add_argument(
        "--init-dirs",
        action="store_true",
        help="Create the expected dataset/artifact folders and exit.",
    )
    return parser.parse_args()


# ---------------------------------------------------------------------------
# Reproducibility and filesystem helpers
# ---------------------------------------------------------------------------


def set_reproducible_seed(seed: int) -> None:
    """Seed Python, NumPy and TensorFlow."""
    os.environ.setdefault("PYTHONHASHSEED", str(seed))
    random.seed(seed)
    np.random.seed(seed)
    tf.random.set_seed(seed)


def create_project_dirs(dataset_root: Path, artifacts_root: Path) -> None:
    """Create the expected dataset and artifact folders without deleting data."""
    for split in ("train", "val"):
        for class_name in CLASS_NAMES:
            (dataset_root / split / class_name).mkdir(parents=True, exist_ok=True)
    (artifacts_root / "preprocessed_preview").mkdir(parents=True, exist_ok=True)
    print(f"Created/checked dataset folders: {dataset_root}")
    print(f"Created/checked artifact folders: {artifacts_root}")


def write_labels(artifacts_root: Path) -> Path:
    """Write labels.txt and return its path."""
    labels_path = artifacts_root / "labels.txt"
    labels_path.write_text("\n".join(CLASS_NAMES) + "\n", encoding="utf-8")
    return labels_path


# ---------------------------------------------------------------------------
# Dataset handling
# ---------------------------------------------------------------------------


def list_images(folder: Path) -> list[Path]:
    """Return all supported images in one folder."""
    if not folder.exists():
        return []
    return sorted(path for path in folder.iterdir() if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES)


def collect_split(split_root: Path) -> tuple[list[Path], list[int]]:
    """Collect image paths and numeric labels for one split."""
    image_paths: list[Path] = []
    labels: list[int] = []

    for index, class_name in enumerate(CLASS_NAMES):
        class_dir = split_root / class_name
        for path in list_images(class_dir):
            image_paths.append(path)
            labels.append(index)

    return image_paths, labels


def validate_dataset(dataset_root: Path) -> tuple[list[Path], list[int], list[Path], list[int]]:
    """Validate dataset layout and return train/validation lists."""
    missing = []
    for split in ("train", "val"):
        for class_name in CLASS_NAMES:
            class_dir = dataset_root / split / class_name
            if not class_dir.exists():
                missing.append(class_dir)

    if missing:
        message = "Missing dataset folders:\n" + "\n".join(f"  - {path}" for path in missing)
        message += "\n\nRun with --init-dirs to create the expected structure."
        raise FileNotFoundError(message)

    train_paths, train_labels = collect_split(dataset_root / "train")
    val_paths, val_labels = collect_split(dataset_root / "val")

    if not train_paths:
        raise RuntimeError(f"No training images found in {dataset_root / 'train'}")
    if not val_paths:
        raise RuntimeError(f"No validation images found in {dataset_root / 'val'}")

    print_dataset_report(train_labels, val_labels)
    return train_paths, train_labels, val_paths, val_labels


def print_dataset_report(train_labels: list[int], val_labels: list[int]) -> None:
    """Print per-class image counts."""
    print("Dataset report")
    print("-" * 64)
    print(f"{'class':<10} {'train':>8} {'val':>8}")
    for index, class_name in enumerate(CLASS_NAMES):
        train_count = sum(1 for label in train_labels if label == index)
        val_count = sum(1 for label in val_labels if label == index)
        print(f"{class_name:<10} {train_count:8d} {val_count:8d}")
    print("-" * 64)
    print(f"{'total':<10} {len(train_labels):8d} {len(val_labels):8d}")


def compute_class_weights(labels: list[int]) -> dict[int, float]:
    """Compute balanced class weights without requiring scikit-learn."""
    counts = np.bincount(np.asarray(labels, dtype=np.int32), minlength=NUM_CLASSES)
    total = int(np.sum(counts))
    weights: dict[int, float] = {}
    for index, count in enumerate(counts):
        if count > 0:
            weights[index] = total / (NUM_CLASSES * float(count))
    return weights


# ---------------------------------------------------------------------------
# STM32-compatible preprocessing
# ---------------------------------------------------------------------------


def simulate_rgb565(rgb_0_255: np.ndarray) -> np.ndarray:
    """Simulate RGB565 quantization and expansion back to 8-bit RGB."""
    image = rgb_0_255.astype(np.float32, copy=False)
    out = np.empty_like(image, dtype=np.float32)
    out[..., 0] = np.round(image[..., 0] * 31.0 / 255.0) * (255.0 / 31.0)
    out[..., 1] = np.round(image[..., 1] * 63.0 / 255.0) * (255.0 / 63.0)
    out[..., 2] = np.round(image[..., 2] * 31.0 / 255.0) * (255.0 / 31.0)
    return out


def resize_nearest_floor(rgb: np.ndarray, out_h: int = INPUT_H, out_w: int = INPUT_W) -> np.ndarray:
    """Resize with the same integer-floor nearest-neighbour mapping as STM32."""
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError(f"Expected HxWx3 RGB array, got shape {rgb.shape}")

    src_h, src_w = rgb.shape[:2]
    y_idx = (np.arange(out_h, dtype=np.int32) * src_h // out_h)
    x_idx = (np.arange(out_w, dtype=np.int32) * src_w // out_w)
    return rgb[y_idx[:, None], x_idx[None, :]]


def preprocess_image_to_uint8(image_path: Path, *, apply_rgb565: bool) -> np.ndarray:
    """Load one image and return a 96x96x3 RGB uint8 array."""
    image = Image.open(image_path).convert("RGB")
    rgb = np.asarray(image, dtype=np.float32)

    if apply_rgb565:
        rgb = simulate_rgb565(rgb)

    resized = resize_nearest_floor(rgb)
    return np.clip(resized, 0.0, 255.0).astype(np.uint8)


def save_preprocessed_previews(image_paths: list[Path], artifacts_root: Path, *, apply_rgb565: bool, max_count: int = 10) -> None:
    """Save a few preprocessed input previews for visual inspection."""
    preview_dir = artifacts_root / "preprocessed_preview"
    preview_dir.mkdir(parents=True, exist_ok=True)

    for path in image_paths[:max_count]:
        preview = preprocess_image_to_uint8(path, apply_rgb565=apply_rgb565)
        Image.fromarray(preview, mode="RGB").save(preview_dir / f"{path.stem}_input96_rgb.png")


# ---------------------------------------------------------------------------
# Model and training
# ---------------------------------------------------------------------------


def build_model(*, dropout: float) -> keras.Model:
    """Build a compact CNN that accepts raw 96x96x3 input values in [0, 255]."""
    inputs = keras.Input(shape=INPUT_SHAPE, dtype=tf.float32, name="image_rgb_u8_values")
    x = keras.layers.Rescaling(scale=1.0 / 127.5, offset=-1.0, name="rescale_to_minus1_plus1")(inputs)

    # Lightweight convolutional backbone for STM32 deployment.
    x = keras.layers.Conv2D(16, 3, padding="same", use_bias=False)(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.ReLU()(x)
    x = keras.layers.MaxPooling2D()(x)

    x = keras.layers.SeparableConv2D(32, 3, padding="same", use_bias=False)(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.ReLU()(x)
    x = keras.layers.MaxPooling2D()(x)

    x = keras.layers.SeparableConv2D(64, 3, padding="same", use_bias=False)(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.ReLU()(x)
    x = keras.layers.MaxPooling2D()(x)

    x = keras.layers.SeparableConv2D(96, 3, padding="same", use_bias=False)(x)
    x = keras.layers.BatchNormalization()(x)
    x = keras.layers.ReLU()(x)

    x = keras.layers.GlobalAveragePooling2D()(x)
    x = keras.layers.Dropout(dropout)(x)
    outputs = keras.layers.Dense(NUM_CLASSES, activation="softmax", name="count_probabilities")(x)

    return keras.Model(inputs=inputs, outputs=outputs, name="stm32_edge_vision_count_model")


def make_callbacks(artifacts_root: Path, patience: int) -> list[keras.callbacks.Callback]:
    """Create training callbacks."""
    return [
        keras.callbacks.ModelCheckpoint(
            filepath=str(artifacts_root / "best_count_model.keras"),
            monitor="val_accuracy",
            mode="max",
            save_best_only=True,
            verbose=1,
        ),
        keras.callbacks.ReduceLROnPlateau(
            monitor="val_accuracy",
            mode="max",
            factor=0.5,
            patience=max(3, patience // 3),
            min_lr=1e-6,
            verbose=1,
        ),
        keras.callbacks.EarlyStopping(
            monitor="val_accuracy",
            mode="max",
            patience=patience,
            restore_best_weights=True,
            verbose=1,
        ),
    ]


def save_history(history: keras.callbacks.History, artifacts_root: Path) -> Path:
    """Save Keras history as CSV."""
    path = artifacts_root / "history.csv"
    keys = list(history.history.keys())
    rows = zip(*[history.history[key] for key in keys])

    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["epoch", *keys])
        for epoch, row in enumerate(rows, start=1):
            writer.writerow([epoch, *row])

    return path


# ---------------------------------------------------------------------------
# TFLite export
# ---------------------------------------------------------------------------


def representative_dataset(image_paths: list[Path], *, apply_rgb565: bool, max_samples: int = 200):
    """Yield representative samples for full-integer post-training quantization."""
    sample_paths = image_paths[:max_samples]
    for path in sample_paths:
        arr = preprocess_image_to_uint8(path, apply_rgb565=apply_rgb565).astype(np.float32)
        yield [arr.reshape(1, INPUT_H, INPUT_W, INPUT_C)]


def export_tflite_models(model: keras.Model, artifacts_root: Path, train_paths: list[Path], *, apply_rgb565: bool) -> tuple[Path, Path]:
    """Export float and uint8 TFLite models."""
    float_converter = tf.lite.TFLiteConverter.from_keras_model(model)
    float_tflite = float_converter.convert()
    float_path = artifacts_root / "count_model_float.tflite"
    float_path.write_bytes(float_tflite)

    uint8_converter = tf.lite.TFLiteConverter.from_keras_model(model)
    uint8_converter.optimizations = [tf.lite.Optimize.DEFAULT]
    uint8_converter.representative_dataset = lambda: representative_dataset(
        train_paths,
        apply_rgb565=apply_rgb565,
        max_samples=min(200, len(train_paths)),
    )
    uint8_converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    uint8_converter.inference_input_type = tf.uint8
    uint8_converter.inference_output_type = tf.float32
    uint8_tflite = uint8_converter.convert()
    uint8_path = artifacts_root / "count_model_uint8.tflite"
    uint8_path.write_bytes(uint8_tflite)

    return float_path, uint8_path


def sanity_check_tflite(model_path: Path) -> None:
    """Print exported TFLite input and output details."""
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    print("TFLite sanity check")
    print("-" * 64)
    print(f"model : {model_path}")
    print(f"input : shape={input_detail['shape']} dtype={input_detail['dtype']} quant={input_detail.get('quantization')}")
    print(f"output: shape={output_detail['shape']} dtype={output_detail['dtype']} quant={output_detail.get('quantization')}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    """Program entry point."""
    args = parse_args()
    apply_rgb565 = not args.no_rgb565_simulation

    set_reproducible_seed(args.seed)
    args.artifacts.mkdir(parents=True, exist_ok=True)

    if args.init_dirs:
        create_project_dirs(args.dataset, args.artifacts)
        write_labels(args.artifacts)
        return 0

    create_project_dirs(args.dataset, args.artifacts)
    train_paths, train_labels, val_paths, val_labels = validate_dataset(args.dataset)
    write_labels(args.artifacts)
    save_preprocessed_previews(train_paths, args.artifacts, apply_rgb565=apply_rgb565)

    train_seq = ImageSequence(
        train_paths,
        train_labels,
        batch_size=args.batch_size,
        shuffle=True,
        seed=args.seed,
        apply_rgb565=apply_rgb565,
    )
    val_seq = ImageSequence(
        val_paths,
        val_labels,
        batch_size=args.batch_size,
        shuffle=False,
        seed=args.seed,
        apply_rgb565=apply_rgb565,
    )

    model = build_model(dropout=args.dropout)
    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=args.learning_rate),
        loss=keras.losses.SparseCategoricalCrossentropy(),
        metrics=[keras.metrics.SparseCategoricalAccuracy(name="accuracy")],
    )
    model.summary()

    class_weight = None if args.no_class_weights else compute_class_weights(train_labels)
    if class_weight:
        print(f"Class weights: {class_weight}")

    history = model.fit(
        train_seq,
        validation_data=val_seq,
        epochs=args.epochs,
        callbacks=make_callbacks(args.artifacts, args.patience),
        class_weight=class_weight,
        verbose=1,
    )

    keras_path = args.artifacts / "count_model.keras"
    model.save(keras_path)
    history_path = save_history(history, args.artifacts)

    config = TrainingConfig(
        input_shape=INPUT_SHAPE,
        input_dtype_after_preprocessing="uint8",
        input_range_after_preprocessing=(0, 255),
        class_names=CLASS_NAMES,
        preprocessing=(
            "RGB -> optional full-resolution RGB565 simulation -> "
            "integer-floor nearest-neighbour resize to 96x96 -> RGB uint8"
        ),
        rgb565_simulation=apply_rgb565,
        batch_size=args.batch_size,
        epochs=args.epochs,
        seed=args.seed,
        train_images=len(train_paths),
        val_images=len(val_paths),
        created_utc=datetime.now(timezone.utc).isoformat(timespec="seconds"),
    )
    config_path = args.artifacts / "training_config.json"
    config_path.write_text(json.dumps(asdict(config), indent=2), encoding="utf-8")

    float_path, uint8_path = export_tflite_models(
        model,
        args.artifacts,
        train_paths,
        apply_rgb565=apply_rgb565,
    )
    sanity_check_tflite(uint8_path)

    print("-" * 64)
    print("Training and export complete")
    print(f"Keras model : {keras_path}")
    print(f"Float TFLite: {float_path}")
    print(f"Uint8 TFLite: {uint8_path}")
    print(f"Labels      : {args.artifacts / 'labels.txt'}")
    print(f"Config      : {config_path}")
    print(f"History     : {history_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
