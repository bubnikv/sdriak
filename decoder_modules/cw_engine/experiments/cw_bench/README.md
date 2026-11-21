# cw_bench — the CW decoders behind the real channelizer

The gates (`src/cw_test.cpp`, `src/call_test.cpp`) feed the decoder at
250 Hz directly. They never see the channelizer's passband or its
zero-history start, nor fading, impulses or a real band. `cw_bench` does:

1. It keys synthetic CW into wideband complex IQ (8 kHz by default).
2. Optionally it adds a Watterson fading channel, impulse noise, or a
   recorded IQ bed.
3. It runs that through `SkimChannelizer` and one decoder channel.
4. It scores the decoded text against what was sent.

`ALGORITHM.md` §5.3.6 and "Experiments: proposed diffs" quote its numbers.

| File | What |
|---|---|
| `cw_bench.cpp` | the driver: one configuration per run, lists of speeds, offsets and SNRs; `--gate` regression checks |
| `bench.py` | named cases run in parallel, with the report tables (Python 3, standard library only) |
| `coherent_fading.py` | one-element detection under Watterson fading: v3's ½-dit sums against whole-element and coherence-limited coherent sums and a known-gain bound; the table in `ALGORITHM.md` §5.3.4 (needs numpy and scipy, about 25 minutes) |

## Build and run

With `OPT_BUILD_TESTS=ON`, the target is `cw_engine_bench`. ctest runs two
gates:

- `cw_engine_bench` (`--gate quick`) checks four things:
  - clean copy at 25 WPM and 14 dB;
  - a tone midway between two channels;
  - ITU-R "good" fading;
  - six minutes of noise decoding to (almost) nothing.
- `cw_engine_bench_startup` (`--gate startup`) is the channel-start case:
  keying 0.5 s after the channel starts, at 35 WPM and 14 dB. It fails on
  the current code, so it is registered `WILL_FAIL`. It passes with
  `SKIM_V3_SETTLE_S=0.5` from `experiments/acquire-settle-and-witness.diff`.

```sh
cw_engine_bench --gate quick
python3 experiments/cw_bench/bench.py --bin <build>/cw_engine_bench ladder --cfg v3 --cfg v2
SKIM_V3_SETTLE_S=0.5 python3 .../bench.py --bin ... startup     # with a decoder switch
```

## The signal

| Part | Model |
|---|---|
| Text | `VVV` and 12 tokens per over: callsign-like (1–2 letters, a digit, 1–3 letters) or traffic words (CQ DE TEST 5NN TU K UR RST). Over `s` uses `std::mt19937(1000 + s)`, so every configuration sees the same texts and noise. |
| Keying | ITU timing (1 / 3 / 1 / 3 / 7 dits), ±`--jitter` (3 %) on every run, raised-cosine edges of `--edge` ms (5), and `--lead` seconds (0.5) of noise before the keying. |
| Noise | Complex white Gaussian for SNR500 = C/N0 − 27 dB, with a unit tone. The band floor the pipeline would publish goes to `set_floor()`, from the prototype's noise bandwidth. |
| Fading, `--fade` | `itu:good` / `moderate` / `poor` / `lowlat` / `highlat` / `highlat-poor`: ITU-R F.1487 as two equal paths with the condition's delay and spread. `flat:<B>`: one path of spread B (DeepCW's 0.1 / 0.3 / 1 / 3 Hz grid). `paths:p,B,shift,delay_ms;…`: your own. Each path is complex Gaussian noise through a Gaussian filter of width σ_t = √2/(2πB), normalised to unit power, with its Doppler shift and a delayed copy of the keyed tone. SNR is the mean over the fading. `--fade-check` prints the generator's power and autocorrelation against exp(−2π²σ_f²Δ²). |
| Impulses, `--impulse` | `poisson:<rate/s>:<dB>[:<shape>]`, `periodic:<rate/s>:<dB>[:<shape>]` (ignition, ±1 % jitter), or `crash:<flashes/s>:<dB>`. Amplitude × U(0.5, 1.5) over the wideband noise rms, random phase; the channelizer smears each one into every channel. Shapes: `delta` (default), one wideband sample; `rx`, the pulse a receiver delivers: a Hamming-windowed sinc low-pass to ±0.45 of the rate (±24 samples, ±3 ms at 8 kHz), peak 1, at a random sub-sample time; `sferic`, 2–4 `rx` pulses 0.5–3 ms apart, the later ones × U(0.3, 1), each with its own phase. A crash is a flash: 3–6 return strokes 40–60 ms apart, each a sferic at × U(0.5, 1.5), and 10–30 `rx` in-cloud pulses 5–20 dB weaker spread over it. |
| Clipping, `--clip <dB>` | ADC saturation: the complex magnitude of everything (signal, noise, impulses) limited to this far over the wideband noise rms. |
| Class A noise, `--classa <A>:<Γ>` | Middleton Class A in place of the Gaussian noise, at the same mean power: per sample m ~ Poisson(A) impulsive sources, variance (m/A + Γ)/(1 + Γ) of the Gaussian's. Small A and Γ: impulsive; large: close to Gaussian. |
| Impulse repair, `--repair[=<dB>]` | `ImpulseRepair` (`engine/impulse_repair.h`) on the wideband stream before the channelizer, for every decoder. A complex AR(32) model of the band predicts each sample; a prediction error this far (default 12 dB) over the error's median opens a gap, which is filled by forward and backward prediction, crossfaded. Strong signals (bins of a 31 Hz short-time FFT standing 25 dB over the median bin) are split off first and bypass it. A gap whose error spectrum is narrowband (flatness under 0.2), or that has not settled after 32 ms, is a carrier keyed on or a fade, not an impulse: it is left alone. When more than 10 % of recent samples were repaired, the threshold rises (Class A noise). Adds the split's frame and the look-ahead (564 samples, 70 ms at 8 kHz) to the latency. `--repair-split=<dB>` sets the split's level (0: no split), `--repair-holdoff=<ms>` the pause after a gap left alone (8), `--repair-order=<n>` the AR order (32). Per over it reports gaps filled and left alone on stderr. |
| Other stations, `--qrm <N>:<dB>[:<range>]` | N keyed CW stations in other channels (at least two channels from the decoded one), each with its own random text, 15–35 WPM, a random offset within its channel and an SNR500 drawn from `dB − range` to `dB`; their own random stream, so runs without `--qrm` are unchanged. Each station's keying is kept as on/off runs and rendered sample by sample (the same values as the target's envelope), so hundreds of stations at 48 kHz take a few MB. A crowded band for the impulse repair, whose AR model can only follow so many carriers. |
| Recorded IQ, `--bed file.cf32` | Interleaved little-endian float32 at `--rate`. The synthetic signal is added to the bed in complex IQ before the channelizer, where superposition is physically valid (ALGORITHM.md §5.9), and the bed's own noise replaces the synthetic noise. SNR and `set_floor()` come from the bed's floor in the decoded channel: the median of 0.25 s block powers over its first 20 s. Over `s` starts at a different offset into the bed. |
| Geometry | `--rate` (a multiple of 250 Hz), `--channel`, `--passband` / `--taps` (`create_ex`), `--offset` (the tone from the channel centre, Hz). |

`--noise-only <s>` decodes noise (or the bed) without a signal and counts
the characters: the false-character rate.

## Scoring

Each over prints:

```
cfg wpm offset_hz snr500 seed ref_len dist pay_len pay_dist
```

- `dist` is the Levenshtein distance of the whole decoded over against
  `VVV <payload>`.
- `pay_dist` is the payload against the decoded text with a free alignment
  start and end. The warm-up and any junk around it don't count.
- `bench.py` sums each into a CER, and interpolates the SNR500 at which CER
  falls to a level.
- `--show` prints each over's reference and decoded text.

## Decoders (`--cfg`)

| Name | Decoder |
|---|---|
| `v3`, `v2`, `v2:<fc>` | the Bayesian decoders (`v2:<fc>`: v2 with its narrow envelope at `fc` Hz) |
| `dcw1` | DeepCW v1 (`engine/decode_deepcw.h`), model from `SKIM_DEEPCW_MODEL` |
| `dcw2` | DeepCW v2 wide (`engine/decode_deepcw2.h`), model from `SKIM_DEEPCW2_MODEL` |
| `dcw2n` | DeepCW v2 narrow, model from `SKIM_DEEPCW2_NARROW_MODEL` |
| `dcw2v1` | the v1 model (`SKIM_DEEPCW_MODEL`) through `dcw2`'s stitcher |
| `…:nogate` | a DeepCW backend without its keyed-line gate |
| `dcw2:q90`, `dcw2:fill` | `dcw2` with another tile noise level (default median; `DeepcwV2Config::noise_mode`) |

The DeepCW configurations need a build with `OPT_BUILD_CW_DEEPCW`, ONNX
Runtime (`SKIM_ORT_LIB`) and the models (`DeepCW.md`); they run inline, so
a run is deterministic. They commit behind the live edge (v1 1 s, v2 2 s
and a ramp), so pass `--tail 4`: four more seconds of noise after each
over, for every configuration compared. `bench.py fading` takes `--fades`
(space-separated `--fade` values) and `--snrs` (comma-separated).

```sh
SKIM_DEEPCW_THREADS=1 SKIM_DEEPCW2_THREADS=1 python3 bench.py --bin ... fading \
    --cfg v3 --cfg dcw1 --cfg dcw2 --cfg dcw2n -- --tail 4
```

`DeepCW.md` ("Synthetic bench") has the comparison of all of them: the noise
ladder and payload CER under the ITU conditions, flat fading and a two-path
beat.

## Cases (`bench.py`)

| Case | Measures |
|---|---|
| `ladder` | SNR500 at payload CER 10 / 3 / 1 % at 12 / 18 / 25 / 35 WPM |
| `offset` | SNR500 at payload CER 10 % by tone offset 0–62.5 Hz (25 WPM) |
| `startup` | whole-over CER and failed overs at 14 dB with 0.5 / 5 / 12 s of noise lead-in (25 / 35 WPM) |
| `fading` | payload CER at 24 WPM by ITU condition and flat spread, at DeepCW's SNR500 points (−12 … +10 dB, and +20) |
| `impulse` | payload CER at 25 WPM, 6 dB, under impulse noise, and false characters per hour on noise with the same impulses |
| `impulse2` | the same at 3, 6 and 10 dB for realistic impulses: receiver-shaped pulses, sferics, ignition and power-line trains, static crashes, Class A noise, clipping at 60 dB (a 3 s tail built in) |
| `noise` | false characters per hour on 20 minutes of noise, or of `--bed` |

## Validation

- **Against the earlier scratch harness** (`ALGORITHM.md` §5.3.6): 167 of
  170 overs reproduce their distances exactly. The 3 that differ sit at the
  cliff (−2 dB), where the band floor now comes from the prototype's exact
  noise bandwidth instead of a rounded 105.9 Hz.
- **The fading generator** gives mean power 1.00 ± 0.04 and |ρ| within about
  0.02 of the Gaussian at the half-correlation time and at half and twice
  it, for 0.1–30 Hz.
- **A synthetic bed** (noise, a carrier two channels away, a hard-keyed
  interferer one channel down) decodes to no false characters. CER at 0 dB
  rises from 3 % to 26 %: the interferer's key clicks reach the test
  channel in bursts that a median floor does not count.
