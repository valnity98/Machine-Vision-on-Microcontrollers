#!/usr/bin/env python3
"""
eval_quantized_numpy.py
=======================
Evaluate the quantized MobileNetV1 counting model (quantized_model.tflite) on the
dataset folders without a TFLite runtime. It reads the flatbuffer itself and runs
the layers in numpy (weights and activations are de-quantised, and every layer
output is rounded to the int8/uint8 grid of the model).

Use it when TensorFlow / ai-edge-litert cannot be installed or are blocked (for
example on Python 3.14 or under an application control policy).

Check against the real TFLite runtime: on six synthetic images (red, black, white,
blue, grey, green) the class scores were identical to three decimals. The result is
therefore the accuracy of the quantised model, but the numpy code is not a
bit-exact copy of the TFLite kernels.

Preprocessing is the one of the firmware: full-frame nearest-neighbour stretch to
96 x 96 with the integer-floor mapping (see predict_count_tflite.py).

Usage:
  python "ML training/src/eval_quantized_numpy.py"       --model "ML training/Model/2026_06_27_20_08_38/quantized_models/quantized_model.tflite"       --dataset "ML training/dataset" --split val
"""

import argparse
import glob
import os
import struct

import numpy as np
from PIL import Image

import struct
import numpy as np


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u8(b, o): return b[o]
def i8(b, o): return struct.unpack_from('<b', b, o)[0]


class Tbl:
    def __init__(self, b, pos): self.b, self.pos = b, pos
    def _f(self, idx):
        vt = self.pos - i32(self.b, self.pos); n = u16(self.b, vt)
        if 4 + 2 * idx >= n: return 0
        o = u16(self.b, vt + 4 + 2 * idx)
        return self.pos + o if o else 0
    def has(self, idx): return self._f(idx) != 0
    def scalar(self, idx, fmt, default=0):
        p = self._f(idx)
        return struct.unpack_from(fmt, self.b, p)[0] if p else default
    def table(self, idx):
        p = self._f(idx)
        return Tbl(self.b, p + u32(self.b, p)) if p else None
    def vec_start(self, idx):
        p = self._f(idx)
        if not p: return 0, 0
        q = p + u32(self.b, p); return q + 4, u32(self.b, q)
    def tables(self, idx):
        s, n = self.vec_start(idx)
        return [Tbl(self.b, s + 4 * i + u32(self.b, s + 4 * i)) for i in range(n)]
    def ints(self, idx, fmt, size):
        s, n = self.vec_start(idx)
        return list(struct.unpack_from('<%d%s' % (n, fmt), self.b, s)) if n else []
    def bytes_(self, idx):
        s, n = self.vec_start(idx); return self.b[s:s + n]


# builtin operator codes used by MobileNetV1
OPS = {0: 'ADD', 1: 'AVERAGE_POOL_2D', 3: 'CONV_2D', 4: 'DEPTHWISE_CONV_2D', 9: 'FULLY_CONNECTED', 14: 'LOGISTIC', 17: 'RELU6', 22: 'RESHAPE', 25: 'SOFTMAX', 40: 'MEAN', 114: 'QUANTIZE', 6: 'DEQUANTIZE', 19: 'RELU', 2: 'CONCAT'}
TYPES = {0: np.float32, 1: np.float16, 2: np.int32, 3: np.uint8, 4: np.int64, 9: np.int8}


def load(path):
    b = open(path, 'rb').read()
    m = Tbl(b, u32(b, 0))
    codes = []
    for oc in m.tables(1):
        dep = oc.scalar(0, '<b', 0); new = oc.scalar(3, '<i', 0)
        codes.append(max(dep, new))
    bufs = [t.bytes_(0) if t.has(0) else b'' for t in m.tables(4)]
    sg = m.tables(2)[0]
    tensors = []
    for t in sg.tables(0):
        q = t.table(4)
        scale = q.ints(2, 'f', 4) if q else []
        zp = q.ints(3, 'q', 8) if q else []
        qdim = q.scalar(6, '<i', 0) if q else 0
        tensors.append(dict(shape=t.ints(0, 'i', 4), type=t.scalar(1, '<b', 0), buffer=t.scalar(2, '<I', 0),
                            name=(lambda p: b[u32(b, p) + p + 4:u32(b, p) + p + 4 + u32(b, p + u32(b, p))] if False else b'')(0),
                            scale=scale, zp=zp, qdim=qdim))
    ops = []
    for o in sg.tables(3):
        opts = o.table(4)
        ops.append(dict(code=OPS.get(codes[o.scalar(0, '<I', 0)], codes[o.scalar(0, '<I', 0)]), inputs=o.ints(1, 'i', 4), outputs=o.ints(2, 'i', 4), opts=opts))
    return dict(b=b, bufs=bufs, tensors=tensors, ops=ops, inputs=sg.ints(1, 'i', 4), outputs=sg.ints(2, 'i', 4))


def const(model, idx):
    t = model['tensors'][idx]
    raw = model['bufs'][t['buffer']]
    if not raw: return None
    return np.frombuffer(raw, dtype=TYPES[t['type']]).reshape(t['shape'])


def dequant(model, idx, arr):
    t = model['tensors'][idx]
    sc = np.array(t['scale'], np.float32); zp = np.array(t['zp'], np.int64)
    if len(sc) <= 1:
        return (arr.astype(np.float32) - (zp[0] if len(zp) else 0)) * (sc[0] if len(sc) else 1.0)
    shp = [1] * arr.ndim; shp[t['qdim']] = -1
    return (arr.astype(np.float32) - zp.reshape(shp)) * sc.reshape(shp)


def fake_quant(model, idx, x):
    t = model['tensors'][idx]
    if not t['scale'] or t['type'] == 0: return x
    sc = t['scale'][0]; zp = t['zp'][0]
    lo, hi = (0, 255) if t['type'] == 3 else (-128, 127)
    q = np.clip(np.round(x / sc) + zp, lo, hi)
    return ((q - zp) * sc).astype(np.float32)


def act(x, fa):
    if fa == 1: return np.maximum(x, 0)
    if fa == 3: return np.clip(x, 0, 6)
    if fa == 2: return np.clip(x, -1, 1)
    return x


def pad_same(h, w, kh, kw, sh, sw):
    oh, ow = -(-h // sh), -(-w // sw)
    ph = max((oh - 1) * sh + kh - h, 0); pw = max((ow - 1) * sw + kw - w, 0)
    return ph // 2, ph - ph // 2, pw // 2, pw - pw // 2, oh, ow


def conv(x, w, bias, stride, padding, depthwise):
    n, h, wd, c = x.shape
    if depthwise:
        _, kh, kw, oc = w.shape
    else:
        oc, kh, kw, _ = w.shape
    if padding == 0:   # SAME
        pt, pb, pl, pr, oh, ow = pad_same(h, wd, kh, kw, stride, stride)
        x = np.pad(x, ((0, 0), (pt, pb), (pl, pr), (0, 0)))
    else:
        oh, ow = (h - kh) // stride + 1, (wd - kw) // stride + 1
    out = np.zeros((n, oh, ow, oc), np.float32)
    for i in range(kh):
        for j in range(kw):
            patch = x[:, i:i + stride * oh:stride, j:j + stride * ow:stride, :]
            if depthwise:
                out += patch * w[0, i, j, :]
            else:
                out += np.tensordot(patch, w[:, i, j, :], axes=([3], [1]))
    return out + bias


def run(model, img_u8):
    """img_u8: (96,96,3) uint8 -> output probabilities (float)."""
    vals = {}
    t_in = model['inputs'][0]
    vals[t_in] = dequant(model, t_in, img_u8.reshape(model['tensors'][t_in]['shape']))
    for op in model['ops']:
        code = op['code']; ins = op['inputs']; out = op['outputs'][0]; o = op['opts']
        if code in ('CONV_2D', 'DEPTHWISE_CONV_2D'):
            dw = code == 'DEPTHWISE_CONV_2D'
            w = dequant(model, ins[1], const(model, ins[1]))
            bias = dequant(model, ins[2], const(model, ins[2])) if len(ins) > 2 and ins[2] >= 0 else 0.0
            padding = o.scalar(0, '<b', 0); sw = o.scalar(1, '<i', 1)
            fa = o.scalar(4 if dw else 3, '<b', 0)
            y = conv(vals[ins[0]], w, bias, sw, padding, dw)
            vals[out] = fake_quant(model, out, act(y, fa))
        elif code == 'AVERAGE_POOL_2D':
            x = vals[ins[0]]; fh = o.scalar(4, '<i', x.shape[1]); fw = o.scalar(3, '<i', x.shape[2])
            vals[out] = fake_quant(model, out, x.mean(axis=(1, 2), keepdims=True)) if (fh, fw) == x.shape[1:3] else None
        elif code == 'MEAN':
            vals[out] = fake_quant(model, out, vals[ins[0]].mean(axis=(1, 2), keepdims=True))
        elif code == 'RESHAPE':
            vals[out] = vals[ins[0]].reshape(model['tensors'][out]['shape'])
        elif code == 'FULLY_CONNECTED':
            w = dequant(model, ins[1], const(model, ins[1]))
            bias = dequant(model, ins[2], const(model, ins[2])) if len(ins) > 2 and ins[2] >= 0 else 0.0
            x = vals[ins[0]].reshape(1, -1)
            vals[out] = fake_quant(model, out, x @ w.T + bias)
        elif code == 'SOFTMAX':
            x = vals[ins[0]].reshape(1, -1); e = np.exp(x - x.max()); vals[out] = fake_quant(model, out, e / e.sum())
        elif code in ('QUANTIZE', 'DEQUANTIZE'):
            vals[out] = fake_quant(model, out, vals[ins[0]])
        else:
            raise RuntimeError("unsupported op %r" % (code,))
    return vals[model['outputs'][0]].reshape(-1)




CLASSES = 5


def preprocess(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    h, w = a.shape[:2]
    return a[((np.arange(96) * h) // 96)[:, None], ((np.arange(96) * w) // 96)[None, :]]


def main():
    ap = argparse.ArgumentParser(description="Evaluate the quantized counting model with numpy only.")
    ap.add_argument("--model", required=True, help="quantized_model.tflite")
    ap.add_argument("--dataset", required=True, help="dataset root with train/ and val/")
    ap.add_argument("--split", default="val", choices=("train", "val"))
    ap.add_argument("--per-class", type=int, default=0, help="use only the first N images of each class (0 = all)")
    args = ap.parse_args()

    model = load(args.model)
    cm = np.zeros((CLASSES, CLASSES), int)
    for c in range(CLASSES):
        files = sorted(glob.glob(os.path.join(args.dataset, args.split, "count_%d" % c, "*.png")))
        if args.per_class:
            files = files[:args.per_class]
        for f in files:
            cm[c, int(np.argmax(run(model, preprocess(f))))] += 1
    if cm.sum() == 0:
        raise SystemExit("no images found under %s" % os.path.join(args.dataset, args.split))
    print("images %d, accuracy %.1f %%" % (cm.sum(), np.trace(cm) / cm.sum() * 100))
    print("rows = true class 0..%d, columns = predicted class" % (CLASSES - 1))
    for c in range(CLASSES):
        print("  ", c, cm[c].tolist())


if __name__ == "__main__":
    main()
