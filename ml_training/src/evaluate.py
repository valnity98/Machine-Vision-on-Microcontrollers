"""Accuracy of the quantised counting model on the dataset.

Runs quantized_model.tflite on every image of one split (train or val) and
prints the accuracy and the confusion matrix. The images are prepared like in
the firmware: the whole frame is stretched to 96x96 with nearest neighbour.

    python ml_training/src/evaluate.py \
        --model ml_training/Model/2026_06_27_20_08_38/quantized_models/quantized_model.tflite \
        --dataset ml_training/dataset --split val

Needs numpy, Pillow and ai-edge-litert (or TensorFlow, see the import).
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:
    from tensorflow.lite import Interpreter

CLASSES = 5


def load_image(path):
    """Stretch the whole frame to 96x96 (integer floor mapping, as in tinyml_preprocess.c)."""
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    h, w = img.shape[:2]
    rows = np.arange(96) * h // 96
    cols = np.arange(96) * w // 96
    return img[rows][:, cols]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--dataset", required=True, help="folder with train/ and val/")
    parser.add_argument("--split", default="val", choices=["train", "val"])
    args = parser.parse_args()

    interpreter = Interpreter(model_path=args.model)
    interpreter.allocate_tensors()
    input_index = interpreter.get_input_details()[0]["index"]
    output_index = interpreter.get_output_details()[0]["index"]

    confusion = np.zeros((CLASSES, CLASSES), dtype=int)
    for true_class in range(CLASSES):
        folder = Path(args.dataset) / args.split / f"count_{true_class}"
        for path in sorted(folder.glob("*.png")):
            interpreter.set_tensor(input_index, load_image(path)[None])
            interpreter.invoke()
            predicted = int(np.argmax(interpreter.get_tensor(output_index)))
            confusion[true_class, predicted] += 1

    total = confusion.sum()
    if total == 0:
        raise SystemExit(f"no .png images found in {Path(args.dataset) / args.split}")
    print(f"{total} images, accuracy {100 * np.trace(confusion) / total:.1f} %")
    print("rows: true class, columns: predicted class")
    print(confusion)


if __name__ == "__main__":
    main()
