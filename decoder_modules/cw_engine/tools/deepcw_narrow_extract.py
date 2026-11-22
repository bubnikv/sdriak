#!/usr/bin/env python3
"""deepcw_narrow_extract.py: the DeepCW v2 narrow model's weights for the
fused kernel (src/engine/deepcw_narrow_fused.h).

Reads the pinned e04/deepcw-engine model_narrow.onnx (sha256 fc80c4ba...),
finds each weight's role from the graph, checks every constant the kernel
hard-codes (epsilons, the 0.5 FFN and attention scales, the conv strides,
pads and groups, the QDQ zero points), and writes the weights in the
kernel's layout as a named-tensor blob. With --check it also runs a NumPy
forward pass on the blob and compares it with ONNX Runtime on the model.

    python3 deepcw_narrow_extract.py model_narrow.onnx deepcw2_narrow.fused
    python3 deepcw_narrow_extract.py model_narrow.onnx deepcw2_narrow.fused --check

Needs onnx and numpy; --check also onnxruntime.

Blob (little-endian):
    "DCWNARW1"            8 bytes
    u32 version = 1, u32 count
    char source_sha256[64]  the model file's sha256, hex
    count x { char name[48] (NUL-padded); u32 dtype (0 f32, 1 i8, 2 text);
              u32 ndim; u32 dims[4]; u32 offset; u32 bytes }
    data, each tensor at a 64-byte aligned offset from the file's start

Layout: every matrix is [in][out] (out contiguous: the output channels are
the SIMD lanes); the front-end conv weights stay int8 [out][in][kt][kf]
with their per-channel scale and zero point, dequantised by the loader as
ONNX DequantizeLinear does.

The weights it writes are e04's: model_narrow.onnx, e04/deepcw-engine,
MIT License, Copyright (c) 2026 e04. The blob carries that notice as its
"notice" tensor (--notice prints it); keep it with every copy.

This script: part of SDRIAK. GPL-3.0-or-later.
"""

import argparse
import hashlib
import struct
import sys

import numpy as np
import onnx
from onnx import numpy_helper

PINNED_SHA256 = "fc80c4baec72d756af1dbce9b70357a731b0b1a6b8306815bcfc61b6403c46e5"
MAGIC = b"DCWNARW1"
VERSION = 1
NAME_LEN = 48
LAYERS = 3
D = 64

NOTICE = """\
The weights in this file are from model_narrow.onnx, e04/deepcw-engine
(https://github.com/e04/deepcw-engine, commit 79104fb), converted to another
layout by SDRIAK's tools/deepcw_narrow_extract.py.

MIT License

Copyright (c) 2026 e04

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.
"""


class Graph:
    def __init__(self, model):
        g = model.graph
        self.nodes = list(g.node)
        self.inits = {t.name: numpy_helper.to_array(t) for t in g.initializer}
        self.producer = {}
        self.consumers = {}
        for n in self.nodes:
            for o in n.output:
                self.producer[o] = n
            for i in n.input:
                self.consumers.setdefault(i, []).append(n)

    def attrs(self, n):
        return {a.name: onnx.helper.get_attribute_value(a) for a in n.attribute}

    def of_type(self, op):
        return [n for n in self.nodes if n.op_type == op]

    def init(self, name):
        if name not in self.inits:
            raise SystemExit(f"not an initializer: {name}")
        return self.inits[name]

    def only_consumer(self, tensor, op):
        cs = [c for c in self.consumers.get(tensor, []) if c.op_type == op]
        if len(cs) != 1:
            raise SystemExit(f"{tensor}: expected one {op} consumer, found {len(cs)}")
        return cs[0]


def need(cond, what):
    if not cond:
        raise SystemExit("model_narrow.onnx is not the graph this kernel was written for: " + what)


def close(a, b):
    return abs(float(a) - float(b)) <= 1e-12 * max(1.0, abs(float(b)))


def extract(model):
    """The tensors in the kernel's layout, in a fixed order: [(name, array)]."""
    g = Graph(model)
    out = []

    def put(name, arr, dtype=np.float32):
        arr = np.ascontiguousarray(np.asarray(arr).astype(dtype))
        out.append((name, arr))

    # ---- front end: two QDQ convs, each GroupNorm (InstanceNormalization on
    # a [B, groups, -1] view, then a per-channel affine) and ReLU
    convs = [n for n in g.of_type("Conv") if n.input[0].startswith("frontend_qdq_")]
    need(len(convs) == 2, "two front-end convs")
    inorms = g.of_type("InstanceNormalization")
    need(len(inorms) == 2, "two GroupNorms")
    expect = [
        # (out ch, in ch, strides, groups)
        (12, 1, [2, 2], 6),
        (24, 12, [1, 2], 8),
    ]
    for i, (conv, inorm, (co, ci, strides, groups)) in enumerate(zip(convs, inorms, expect)):
        a = g.attrs(conv)
        need(list(a["kernel_shape"]) == [3, 3] and list(a["pads"]) == [1, 1, 1, 1] and
             list(a["strides"]) == strides and a.get("group", 1) == 1 and len(conv.input) == 2,
             f"front-end conv {i} shape")
        p = f"frontend_qdq_{i}_"
        in_scale = g.init(p + "input_scale")
        in_zp = g.init(p + "input_zero_point")
        need(in_zp.dtype == np.uint8 and int(in_zp) == 0, f"conv {i} input zero point uint8 0")
        w = g.init(p + "weight_int8")
        ws = g.init(p + "weight_scale")
        wz = g.init(p + "weight_zero_point")
        need(w.dtype == np.int8 and w.shape == (co, ci, 3, 3), f"conv {i} int8 weight")
        need(ws.shape == (co,) and wz.shape == (co,) and wz.dtype == np.int8, f"conv {i} weight scale")
        dq = g.producer[conv.input[1]]
        need(dq.op_type == "DequantizeLinear" and g.attrs(dq).get("axis", 1) == 0, f"conv {i} weight axis")
        # the input QDQ pair feeds this conv
        dqi = g.producer[conv.input[0]]
        qi = g.producer[dqi.input[0]]
        need(dqi.op_type == "DequantizeLinear" and qi.op_type == "QuantizeLinear", f"conv {i} input QDQ")
        need(qi.input[1] == p + "input_scale" and dqi.input[1] == p + "input_scale", f"conv {i} one input scale")
        # GroupNorm: Reshape [0, groups, -1] -> InstanceNormalization -> Reshape back -> Mul -> Add -> Relu
        rs = g.producer[inorm.input[0]]
        need(rs.op_type == "Reshape" and g.producer[rs.input[0]] is conv, f"GroupNorm {i} follows conv {i}")
        need(list(g.init(rs.input[1])) == [0, groups, -1], f"GroupNorm {i} groups")
        need(close(g.attrs(inorm)["epsilon"], np.float32(1e-5)), f"GroupNorm {i} epsilon")
        rs2 = g.only_consumer(inorm.output[0], "Reshape")
        mul = g.only_consumer(rs2.output[0], "Mul")
        add = g.only_consumer(mul.output[0], "Add")
        relu = g.only_consumer(add.output[0], "Relu")
        gamma = g.init(mul.input[1])
        beta = g.init(add.input[1])
        need(gamma.shape == (co, 1, 1) and beta.shape == (co, 1, 1), f"GroupNorm {i} affine")
        put(f"fe{i}.in_scale", in_scale.reshape(1))
        put(f"fe{i}.w", w, np.int8)
        put(f"fe{i}.w_scale", ws)
        put(f"fe{i}.w_zp", wz, np.int8)
        put(f"fe{i}.gn_scale", g.init(inorm.input[1]))
        put(f"fe{i}.gn_bias", g.init(inorm.input[2]))
        put(f"fe{i}.gamma", gamma.reshape(co))
        put(f"fe{i}.beta", beta.reshape(co))
        if i == 0:
            q1 = g.only_consumer(relu.output[0], "QuantizeLinear")
            need(q1.input[1] == "frontend_qdq_1_input_scale", "ReLU 0 feeds QDQ 1")
        else:
            need(any(c.op_type == "Transpose" for c in g.consumers[relu.output[0]]), "ReLU 1 feeds the projection")

    # ---- every MatMul / Gemm / pointwise Conv is named by the bias after it
    def matmul_by_bias(bias_name):
        adds = [n for n in g.of_type("Add") if bias_name in n.input]
        need(len(adds) == 1, f"one Add of {bias_name}")
        mm = g.producer[adds[0].input[0] if adds[0].input[1] == bias_name else adds[0].input[1]]
        need(mm.op_type == "MatMul", f"{bias_name} follows a MatMul")
        return g.init(mm.input[1]), g.init(bias_name), adds[0]

    def layernorm(prefix):
        lns = [n for n in g.of_type("LayerNormalization") if n.input[1] == prefix + ".weight"]
        need(len(lns) == 1, f"LayerNorm {prefix}")
        a = g.attrs(lns[0])
        need(a.get("axis", -1) == -1 and close(a["epsilon"], np.float32(1e-5)), f"LayerNorm {prefix} axis, epsilon")
        need(lns[0].input[2] == prefix + ".bias", f"LayerNorm {prefix} bias")
        return g.init(prefix + ".weight"), g.init(prefix + ".bias")

    def put_ln(name, prefix):
        w, b = layernorm(prefix)
        need(w.shape == (D,) and b.shape == (D,), f"{prefix} shape")
        put(name + ".g", w)
        put(name + ".b", b)

    w, b, _ = matmul_by_bias("input_projection.bias")
    need(w.shape == (24, D), "input projection 24 -> 64")
    put("proj.w", w)
    put("proj.b", b)
    put_ln("proj_norm", "input_norm")

    # sinusoidal position code: pe[t, 2i] = sin(t f_i), pe[t, 2i + 1] = cos(t f_i)
    freq = g.init("exp")
    need(freq.shape == (D // 2,) and freq.dtype == np.float32, "position frequencies [32]")
    need(len(g.of_type("Sin")) == 1 and len(g.of_type("Cos")) == 1, "one sin/cos table")
    slices = [n for n in g.of_type("Slice") if len(n.input) == 5 and n.input[4] in g.inits]
    starts = sorted(int(g.init(n.input[1])[0]) for n in slices if int(g.init(n.input[4])[0]) == 2)
    need(starts == [0, 1], "sin on even, cos on odd channels")
    put("pe.freq", freq)

    for l in range(LAYERS):
        e = f"encoder.{l}"
        k = f"l{l}"
        for ffn in ("ffn1", "ffn2"):
            put_ln(f"{k}.{ffn}.norm", f"{e}.{ffn}.norm")
            w1, b1, a1 = matmul_by_bias(f"{e}.{ffn}.linear1.bias")
            w2, b2, a2 = matmul_by_bias(f"{e}.{ffn}.linear2.bias")
            need(w1.shape == (D, 128) and w2.shape == (128, D), f"{e}.{ffn} 64 -> 128 -> 64")
            sig = g.only_consumer(a1.output[0], "Sigmoid")
            need(sig is not None, f"{e}.{ffn} SiLU")
            half = g.only_consumer(a2.output[0], "Mul")
            need(close(g.init(half.input[1]), 0.5), f"{e}.{ffn} half step")
            put(f"{k}.{ffn}.w1", w1)
            put(f"{k}.{ffn}.b1", b1)
            put(f"{k}.{ffn}.w2", w2)
            put(f"{k}.{ffn}.b2", b2)
        put_ln(f"{k}.attn_norm", f"{e}.attn_norm")
        wq, bq, _ = matmul_by_bias(f"{e}.self_attn.in_proj_bias")
        need(wq.shape == (D, 3 * D), f"{e} in_proj 64 -> 192")
        put(f"{k}.qkv.w", wq)
        put(f"{k}.qkv.b", bq)
        gemms = [n for n in g.of_type("Gemm") if n.input[1] == f"{e}.self_attn.out_proj.weight"]
        need(len(gemms) == 1, f"{e} out_proj Gemm")
        a = g.attrs(gemms[0])
        need(a.get("transB", 0) == 1 and a.get("transA", 0) == 0 and close(a.get("alpha", 1.0), 1.0) and
             close(a.get("beta", 1.0), 1.0), f"{e} out_proj Gemm form")
        put(f"{k}.out.w", g.init(f"{e}.self_attn.out_proj.weight").T)   # [out][in] -> [in][out]
        put(f"{k}.out.b", g.init(f"{e}.self_attn.out_proj.bias"))
        put_ln(f"{k}.conv.norm", f"{e}.conv_module.norm")
        c = f"{e}.conv_module."
        pin = g.init(c + "pointwise_in.weight")
        dw = g.init(c + "depthwise.weight")
        pout = g.init(c + "pointwise_out.weight")
        need(pin.shape == (128, D, 1) and dw.shape == (D, 1, 15) and pout.shape == (D, D, 1), f"{e} conv module")
        dwc = [n for n in g.of_type("Conv") if n.input[1] == c + "depthwise.weight"][0]
        a = g.attrs(dwc)
        need(a["group"] == D and list(a["pads"]) == [7, 7] and list(a.get("strides", [1])) == [1] and
             list(a.get("dilations", [1])) == [1], f"{e} depthwise k15 pad 7")
        put(f"{k}.pw_in.w", pin[:, :, 0].T)                               # [128][64] -> [64][128]
        put(f"{k}.pw_in.b", g.init(c + "pointwise_in.bias"))
        put(f"{k}.dw.w", dw[:, 0, :].T)                                   # [64][15] -> [15][64]
        put(f"{k}.dw.b", g.init(c + "depthwise.bias"))
        put(f"{k}.pw_out.w", pout[:, :, 0].T)
        put(f"{k}.pw_out.b", g.init(c + "pointwise_out.bias"))
        put_ln(f"{k}.final_norm", f"{e}.final_norm")
    put_ln("out_norm", "output_norm")
    w, b, a = matmul_by_bias("classifier.bias")
    need(w.shape == (D, 42), "classifier 64 -> 42")
    need(g.only_consumer(a.output[0], "LogSoftmax") is not None, "log-softmax head")
    put("cls.w", w)
    put("cls.b", b)

    att = [n for n in g.of_type("Mul") if n.input[1] == "val_223" or
           (n.input[1] in g.inits and g.inits[n.input[1]].shape == (1,) and close(g.inits[n.input[1]][0], 0.5))]
    need(len(att) == 2 * LAYERS, "q and k each scaled by 0.5 (1/sqrt(16) split)")
    need(len(g.of_type("Softmax")) == LAYERS, "one softmax per layer")
    return out


def write_blob(path, tensors, sha):
    header = 8 + 8 + 64
    entry = NAME_LEN + 4 * 2 + 4 * 4 + 4 * 2
    off = header + entry * len(tensors)
    off = (off + 63) & ~63
    entries, chunks = [], []
    for name, arr in tensors:
        nb = name.encode()
        assert len(nb) < NAME_LEN and arr.ndim <= 4
        dt = {np.dtype(np.float32): 0, np.dtype(np.int8): 1, np.dtype(np.uint8): 2}[arr.dtype]
        dims = list(arr.shape) + [1] * (4 - arr.ndim)
        data = arr.astype({0: "<f4", 1: "i1", 2: "u1"}[dt]).tobytes()
        entries.append(nb.ljust(NAME_LEN, b"\0") + struct.pack("<II4III", dt, arr.ndim, *dims, off, len(data)))
        chunks.append((off, data))
        off = (off + len(data) + 63) & ~63
    buf = bytearray(off)
    buf[0:8] = MAGIC
    buf[8:16] = struct.pack("<II", VERSION, len(tensors))
    buf[16:80] = sha.encode()
    p = header
    for e in entries:
        buf[p:p + len(e)] = e
        p += len(e)
    for o, data in chunks:
        buf[o:o + len(data)] = data
    with open(path, "wb") as f:
        f.write(buf)
    return hashlib.sha256(buf).hexdigest()


# ---- NumPy reference on the extracted tensors (--check) ----------------------

def qdq_u8(x, scale):
    s = np.float32(scale)
    return (np.clip(np.rint(x / s), 0, 255).astype(np.float32) * s).astype(np.float32)


def conv2d(x, w, stride):
    """x [C, T, F], w [O, C, 3, 3], pad 1."""
    C, T, F = x.shape
    st, sf = stride
    xp = np.zeros((C, T + 2, F + 2), np.float32)
    xp[:, 1:-1, 1:-1] = x
    To = (T - 1) // st + 1
    Fo = (F - 1) // sf + 1
    y = np.zeros((w.shape[0], To, Fo), np.float64)
    for dt in range(3):
        for df in range(3):
            patch = xp[:, dt:dt + st * (To - 1) + 1:st, df:df + sf * (Fo - 1) + 1:sf]
            y += np.einsum("oc,ctf->otf", w[:, :, dt, df].astype(np.float64), patch.astype(np.float64))
    return y.astype(np.float32)


def group_norm(x, groups, gs, gb, gamma, beta):
    C = x.shape[0]
    xg = x.reshape(groups, -1).astype(np.float64)
    m = xg.mean(axis=1, keepdims=True)
    v = ((xg - m) ** 2).mean(axis=1, keepdims=True)
    y = (xg - m) / np.sqrt(v + np.float32(1e-5))
    y = y * gs[:, None] + gb[:, None]
    y = y.reshape(x.shape).astype(np.float32)
    return y * gamma.reshape(C, 1, 1) + beta.reshape(C, 1, 1)


def layer_norm(x, g, b):
    x64 = x.astype(np.float64)
    m = x64.mean(axis=-1, keepdims=True)
    v = ((x64 - m) ** 2).mean(axis=-1, keepdims=True)
    return (((x64 - m) / np.sqrt(v + np.float32(1e-5))).astype(np.float32) * g + b).astype(np.float32)


def silu(x):
    return (x / (np.float32(1) + np.exp(-x))).astype(np.float32)


def sigmoid(x):
    return (np.float32(1) / (np.float32(1) + np.exp(-x))).astype(np.float32)


def reference(t, tile):
    """tile [T][3] -> log-probs [T1][42]."""
    x = tile.astype(np.float32)[None]                       # [1, T, 3]
    for i, stride, groups in ((0, (2, 2), 6), (1, (1, 2), 8)):
        x = qdq_u8(x, t[f"fe{i}.in_scale"][0])
        w = ((t[f"fe{i}.w"].astype(np.int32) - t[f"fe{i}.w_zp"].astype(np.int32)[:, None, None, None])
             .astype(np.float32) * t[f"fe{i}.w_scale"][:, None, None, None]).astype(np.float32)
        x = conv2d(x, w, stride)
        x = group_norm(x, groups, t[f"fe{i}.gn_scale"], t[f"fe{i}.gn_bias"], t[f"fe{i}.gamma"], t[f"fe{i}.beta"])
        x = np.maximum(x, 0)
    T1 = x.shape[1]
    h = x[:, :, 0].T.copy()                                 # [T1, 24]
    h = h @ t["proj.w"] + t["proj.b"]
    h = layer_norm(h, t["proj_norm.g"], t["proj_norm.b"])
    ang = (np.arange(T1, dtype=np.float32)[:, None] * t["pe.freq"][None, :]).astype(np.float32)
    pe = np.zeros((T1, D), np.float32)
    pe[:, 0::2] = np.sin(ang)
    pe[:, 1::2] = np.cos(ang)
    h = (h + pe).astype(np.float32)

    def ffn(h, k):
        y = layer_norm(h, t[k + ".norm.g"], t[k + ".norm.b"])
        y = silu(y @ t[k + ".w1"] + t[k + ".b1"])
        y = (y @ t[k + ".w2"] + t[k + ".b2"]) * np.float32(0.5)
        return (h + y).astype(np.float32)

    for l in range(LAYERS):
        k = f"l{l}"
        h = ffn(h, k + ".ffn1")
        y = layer_norm(h, t[k + ".attn_norm.g"], t[k + ".attn_norm.b"])
        qkv = y @ t[k + ".qkv.w"] + t[k + ".qkv.b"]
        att = np.zeros((T1, D), np.float32)
        for hd in range(4):
            q = qkv[:, hd * 16:(hd + 1) * 16] * np.float32(0.5)
            kk = qkv[:, D + hd * 16:D + (hd + 1) * 16] * np.float32(0.5)
            v = qkv[:, 2 * D + hd * 16:2 * D + (hd + 1) * 16]
            s = q @ kk.T
            s = s - s.max(axis=1, keepdims=True)
            p = np.exp(s)
            p = p / p.sum(axis=1, keepdims=True)
            att[:, hd * 16:(hd + 1) * 16] = p @ v
        h = (h + att @ t[k + ".out.w"] + t[k + ".out.b"]).astype(np.float32)
        y = layer_norm(h, t[k + ".conv.norm.g"], t[k + ".conv.norm.b"])
        y = y @ t[k + ".pw_in.w"] + t[k + ".pw_in.b"]
        y = (y[:, :D] * sigmoid(y[:, D:])).astype(np.float32)
        yp = np.zeros((T1 + 14, D), np.float32)
        yp[7:7 + T1] = y
        z = np.zeros((T1, D), np.float64)
        for j in range(15):
            z += yp[j:j + T1].astype(np.float64) * t[k + ".dw.w"][j].astype(np.float64)
        z = silu((z + t[k + ".dw.b"]).astype(np.float32))
        h = (h + z @ t[k + ".pw_out.w"] + t[k + ".pw_out.b"]).astype(np.float32)
        h = ffn(h, k + ".ffn2")
        h = layer_norm(h, t[k + ".final_norm.g"], t[k + ".final_norm.b"])
    h = layer_norm(h, t["out_norm.g"], t["out_norm.b"])
    z = (h @ t["cls.w"] + t["cls.b"]).astype(np.float64)
    z = z - z.max(axis=1, keepdims=True)
    return (z - np.log(np.exp(z).sum(axis=1, keepdims=True))).astype(np.float32)


def bench_tile(rng, T, snr_db=6.0, wpm=20.0):
    """A keyed line on the centre bin over Rayleigh noise, normalised as
    deepcw2n does (noise_q90_log1p): tile [T][3]."""
    fps = 62.5
    dot = 1.2 / wpm * fps
    key = np.zeros(T, bool)
    t = rng.uniform(0, 3 * dot)
    while t < T:
        n = int(rng.choice([1, 3]))
        key[int(t):int(t + n * dot)] = True
        t += n * dot + dot * float(rng.choice([1, 1, 3, 7]))
    a = 10 ** (snr_db / 20)
    re = rng.standard_normal((T, 3)) * np.sqrt(0.5)
    im = rng.standard_normal((T, 3)) * np.sqrt(0.5)
    re[:, 1] += a * key
    mag = np.hypot(re, im).astype(np.float32)
    q = np.sort(mag.ravel())[int(np.floor(0.9 * (mag.size - 1)))]
    noise = max(q / np.sqrt(-2 * np.log(0.1)), mag.max() * 1e-3, 1e-5)
    return np.log1p(mag / np.float32(noise)).astype(np.float32)


def check(model_path, tensors):
    import onnxruntime as ort
    t = dict(tensors)
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    s = ort.InferenceSession(model_path, so, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(1)
    worst = 0.0
    for T in (438, 437, 125, 64, 9):
        for snr in (20.0, 6.0, -3.0):
            tile = bench_tile(rng, T, snr)
            got = reference(t, tile)
            want = s.run(None, {"spectrogram": tile[None, None]})[0][0]
            assert got.shape == want.shape, (got.shape, want.shape)
            d = float(np.abs(got - want).max())
            same = (got.argmax(1) == want.argmax(1)).all()
            worst = max(worst, d)
            print(f"  T {T:4d} snr {snr:5.1f}: max |logp - ORT| = {d:.2e}, greedy {'same' if same else 'DIFFERS'}")
    print(f"worst {worst:.2e}")
    return worst < 1e-4


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("model")
    ap.add_argument("blob", nargs="?")
    ap.add_argument("--check", action="store_true", help="NumPy forward on the blob against ONNX Runtime")
    ap.add_argument("--noverify", action="store_true", help="skip the sha256 pin")
    ap.add_argument("--notice", action="store_true", help="print the weights' licence notice")
    a = ap.parse_args()
    if a.notice:
        print(NOTICE, end="")
        return 0
    raw = open(a.model, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    if sha != PINNED_SHA256 and not a.noverify:
        raise SystemExit(f"{a.model}: sha256 {sha[:8]}..., expected {PINNED_SHA256[:8]}... (model_narrow.onnx "
                         f"of e04/deepcw-engine 79104fb)")
    tensors = extract(onnx.load_from_string(raw))
    n = sum(arr.size for _, arr in tensors)
    print(f"{len(tensors)} tensors, {n} values")
    if a.blob:
        notice = np.frombuffer(NOTICE.encode(), np.uint8)
        bsha = write_blob(a.blob, tensors + [("notice", notice)], sha)
        print(f"{a.blob}: sha256 {bsha}")
    if a.check:
        return 0 if check(a.model, tensors) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
