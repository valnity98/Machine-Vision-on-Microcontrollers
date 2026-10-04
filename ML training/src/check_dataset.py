#!/usr/bin/env python3
"""
check_dataset.py
================
Validate the dataset folder structure required by the STM32 AI Model Zoo
image_classification workflow and by predict_count_tflite.py.

Expected layout:
  <root>/
    train/
      count_0/  *.png | *.jpg
      count_1/
      count_2/
      count_3/
      count_4/
    val/
      count_0/
      ...
    test/          (optional)
      count_0/
      ...

Usage:
  python check_dataset.py --root "E:/Studium_Projekte/STM32_MERO2/ML training/dataset"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXPECTED_CLASSES = ["count_0", "count_1", "count_2", "count_3", "count_4"]
EXPECTED_SPLITS  = ["train", "val"]
OPTIONAL_SPLITS  = ["test"]
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp"}


def check_split(root: Path, split: str, required: bool) -> tuple[dict[str, int], list[str]]:
    """Check one split folder. Returns (images per class, list of problems found)."""
    split_dir = root / split
    counts: dict[str, int] = {}
    errors: list[str]      = []

    if not split_dir.exists():
        if required:
            errors.append(f"{split}: missing required split folder")
            print(f"  MISSING required split folder: {split_dir}")
        else:
            print(f"  SKIP optional split '{split}' (not found)")
        return counts, errors

    print(f"\n  [{split}]  {split_dir}")

    for cls in EXPECTED_CLASSES:
        cls_dir = split_dir / cls
        if not cls_dir.exists():
            print(f"    ERROR   MISSING class folder: {cls_dir}")
            errors.append(f"{split}/{cls}: missing class folder")
            counts[cls] = 0
            continue

        imgs = [f for f in cls_dir.iterdir()
                if f.suffix.lower() in IMAGE_EXTENSIONS]
        counts[cls] = len(imgs)

        if len(imgs) == 0:
            print(f"    ERROR   EMPTY   {cls:12s}  (0 images)")
            errors.append(f"{split}/{cls}: no images")
        elif len(imgs) < 10:
            print(f"    WARNING FEW     {cls:12s}  ({len(imgs)} images - recommend >= 50)")
        else:
            print(f"    OK      OK      {cls:12s}  ({len(imgs)} images)")

    # Check for unexpected class folders
    actual_classes = {d.name for d in split_dir.iterdir() if d.is_dir()}
    extra = actual_classes - set(EXPECTED_CLASSES)
    for e in sorted(extra):
        print(f"    ERROR   UNEXPECTED class folder: {e}")
        errors.append(f"{split}/{e}: unexpected class folder")

    return counts, errors


def main() -> None:
    ap = argparse.ArgumentParser(description="Validate ML dataset structure")
    ap.add_argument("--root", required=True,
                    help="Root dataset folder (contains train/ val/ test/)")
    args = ap.parse_args()

    root = Path(args.root)
    print(f"\nDataset root: {root}")
    if not root.exists():
        print(f"ERROR: root folder does not exist: {root}")
        sys.exit(1)

    all_counts: dict[str, dict[str, int]] = {}
    problems: list[str] = []

    for split in EXPECTED_SPLITS:
        all_counts[split], errs = check_split(root, split, required=True)
        problems.extend(errs)
    for split in OPTIONAL_SPLITS:
        all_counts[split], errs = check_split(root, split, required=False)
        problems.extend(errs)

    # Summary
    print("\n  Summary")
    print(f"  {'Class':12s}", end="")
    for split in EXPECTED_SPLITS + OPTIONAL_SPLITS:
        if (root / split).exists():
            print(f"  {split:>8s}", end="")
    print()
    for cls in EXPECTED_CLASSES:
        print(f"  {cls:12s}", end="")
        for split in EXPECTED_SPLITS + OPTIONAL_SPLITS:
            if (root / split).exists():
                n = all_counts.get(split, {}).get(cls, 0)
                print(f"  {n:>8d}", end="")
        print()

    # Class balance check
    for split in EXPECTED_SPLITS:
        if split not in all_counts:
            continue
        counts = list(all_counts[split].values())
        if max(counts, default=0) > 0:
            ratio = max(counts) / max(min(counts), 1)
            if ratio > 3:
                print(f"\n  WARNING [{split}] Class imbalance: max/min ratio = {ratio:.1f}x - consider balancing.")

    # Total
    total_train = sum(all_counts.get("train", {}).values())
    total_val   = sum(all_counts.get("val",   {}).values())
    print(f"\n  Total train: {total_train}  |  Total val: {total_val}")
    if total_train < 100:
        print("  WARNING Very few training images - model may overfit. Recommend >= 50 per class.")
    if total_val < 25:
        print("  WARNING Very few validation images. Recommend >= 10 per class.")

    if problems:
        print(f"\nFAILED: {len(problems)} problem(s) found:")
        for p in problems:
            print(f"  - {p}")
        print()
        sys.exit(1)

    print("\nDone.\n")


if __name__ == "__main__":
    main()
