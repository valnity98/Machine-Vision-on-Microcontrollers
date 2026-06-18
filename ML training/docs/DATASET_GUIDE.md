# Dataset Guide

Use one folder per class and keep the class names fixed:

```text
count_0, count_1, count_2, count_3, count_4
```

Recommended capture rules:

- Use the same camera position and object area as the final STM32 experiment.
- Include different lighting conditions and small position variations.
- Keep train and validation images separate; do not copy the same image into both splits.
- Prefer real camera frames from the STM32 GUI dataset capture workflow.
- Keep the object-count label exact. Wrong labels are more damaging than a small dataset.

A practical minimum is about 100 training images per class and 20 validation images per class. More data improves robustness.
