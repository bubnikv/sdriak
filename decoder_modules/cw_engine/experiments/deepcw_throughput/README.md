# deepcw_throughput — DeepCW CPU throughput over many channels

The CPU throughput benchmark for the DeepCW models. It reports how many
keyed channels one core carries, and at which batch, worker and thread
counts. cw_bench runs one channel at batch 1, so it can't show this.
`DEEPCW_EXPLAINER.html` has the cost model the counts come from.

Built as `cw_engine_deepcw_throughput` with `OPT_BUILD_TESTS` and
`OPT_BUILD_CW_DEEPCW`. It is a measurement, not a ctest gate.

## What it runs

Both modes use synthetic tiles of one window length: Rayleigh noise on the
populated bins and a keyed line on the centre bin. The tiles are normalised
as the backends normalise them: the v2 models' noise level from the 90th
percentile of the populated bins (`noise_q90_log1p`, as `deepcw2n` does), and
v1 peak-normalised to 30. A pool of 64 such tiles is cycled.

| Mode | What | Shows |
|---|---|---|
| `raw` | W threads share one session with I intra-op threads; each runs batches of B windows back to back | the model's rate through ONNX Runtime, without the queue |
| `runtime` | N channels each keep one window in flight through `DeepcwRuntime` | the engine's path: its job queue, its workers, equal-length batching and clamps (workers 1 .. the hardware threads, batch 1..256, queue ≥ 256 windows within 64 MB), FTZ/DAZ on its workers |

For each configuration it reports:

- windows per second;
- the process CPU time it used, as cores (spinning threads included);
- CPU milliseconds per window;
- GMAC/s per core, from the counted multiply-accumulates;
- keyed channels per core at one 1 s tick each.

Raw mode also checks that one window reads the same alone and inside a
batch of 8. That is the runtime's equal-length batching rule, and the
difference should be at float rounding level.

With `--fused FILE` (the narrow model's fused-kernel weights,
`tools/deepcw_narrow_extract.py`) the fused kernel (`deepcw_narrow_fused.h`)
is measured too:
- its log-probs against ONNX Runtime's on the tile pool, every encoder the
  CPU runs;
- `fused` rows: W threads, one window per call;
- `rt-fused` rows: the runtime on the kernel (no batch, no intra-op threads).

## Run

```sh
cw_engine_deepcw_throughput --model narrow --path model_narrow.onnx --ort onnxruntime.dll
cw_engine_deepcw_throughput --model narrow --mode raw --threads 1 --workers 1,4,8 --batch 1,32,128 --ftz
cw_engine_deepcw_throughput --model wide --window 7          # v2 wide at deepcw2's steady state
cw_engine_deepcw_throughput --model v1 --window 10          # v1 re-reads its 10 s ring
```

The model file defaults to `SKIM_DEEPCW2_NARROW_MODEL`, `SKIM_DEEPCW2_MODEL`
or `SKIM_DEEPCW_MODEL`, depending on the contract. The library defaults to
`SKIM_ORT_LIB`. The file is checked against its pinned sha256; pass
`--noverify` for a transformed model, such as an int8 export.

| Option | Default | |
|---|---|---|
| `--window S` | 7 | window length; deepcw2's steady state is 7 s (4 s context + 2 s guard + 1 s step) |
| `--duration S` | 3 | timed seconds per configuration, after a warm-up run |
| `--batch L` | 1,8,32,64,128 | raw: windows per run; runtime: `DeepcwRuntimeConfig::batch` |
| `--threads L` | 1,2,4 | ONNX Runtime intra-op threads |
| `--workers L` | 1,2,4,… to the hardware threads | |
| `--channels N` | 300 | runtime: channels with a window in flight |
| `--fused FILE` | none | narrow only: also the fused kernel on these weights |
| `--fused-isa NAME` | auto | its encoder: `auto`, `avx2`, `sse`, `neon`, `generic` |
| `--ftz` | off | flush denormals to zero on the calling threads. With 1 intra-op thread ONNX Runtime computes on the calling thread, so this covers the model. |

Some configurations are skipped: workers × threads above twice the hardware
threads, and runtime mode with more workers than hardware threads (the
runtime's clamp).
In runtime mode the queue holds at least 256 windows, and more while their
tiles fit 64 MB (about 12,000 narrow 7 s windows, 570 wide). Beyond that the
runtime refuses the extra windows and warns once that it is behind; those
windows are simply submitted again later.

## Reading it

- **`ch/core`** is the main figure. The cost model predicts about 70 for the
  narrow model at batch 1 (0.061 GMAC per 7 s window at the bench's
  4.3 GMAC/s), and up to about 160 if batching lifts it to the wide
  models' ~10 GMAC/s.
- **`cores` above workers × threads** means ONNX Runtime threads are
  spinning. Their CPU is a real cost on a machine that also runs the
  channelizer.
- **raw against runtime** at the same settings is the cost of the queue and
  its batching rule. Runtime batches are only as large as the windows of
  one length the queue holds.
