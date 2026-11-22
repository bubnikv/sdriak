# CW decoder: the improvement plan

This is the execution plan for improving `cw_engine`'s decoding quality: what
to do, in which order, how each step is measured, and when to stop or change
course. It is the schedule; the reasoning lives in three companion documents:

- [ALGORITHM.md](ALGORITHM.md): how the decoder works (Levels 0–4), where it
  can improve (Level 5), and the measured experiments E0–E3;
- [FADING_TRACKER.md](FADING_TRACKER.md): the gain tracker for the HF fading
  channel (steps K0–K6);
- [BAYESIAN_DECODING.md](BAYESIAN_DECODING.md): the statistics, from
  calibrated evidence to the callsign posterior and the hypothesis bank
  (tracks A, B, C).

Step names below (P0.1, P2.3 …) are this plan's. Each step says which step
of the companion documents it implements (K2, B1, C3 …). §0 lists what is
built, measures the gap to DeepCW's v2 narrow model and sets the order from
here.

- [0. The target: DeepCW v2 narrow](#0-the-target-deepcw-v2-narrow)
- [1. Rules for every step](#1-rules-for-every-step)
- [2. Where the losses are](#2-where-the-losses-are)
- [3. Overview](#3-overview)
- [4. Phase 0: quick wins](#4-phase-0-quick-wins-days)
- [5. Phase 1: measurement](#5-phase-1-measurement)
- [6. Phase 2: the evidence, and the Bayesian structure beside it](#6-phase-2-the-evidence-and-the-bayesian-structure-beside-it)
- [7. Phase 3: channel, impulses, language model, hypothesis bank](#7-phase-3-channel-impulses-language-model-hypothesis-bank)
- [8. Phase 4: the callsign posterior](#8-phase-4-the-callsign-posterior)
- [9. Phase 5: refinements](#9-phase-5-refinements)
- [10. Decision points](#10-decision-points)
- [11. Effort and calendar](#11-effort-and-calendar)
- [12. Risks](#12-risks)
- [13. Not in the plan](#13-not-in-the-plan)

---

## 0. The target: DeepCW v2 narrow

The goal is to come close to DeepCW's v2 narrow model (`dcw2n`, DeepCW.md),
the small model e04 ships: about 0.5 s of CPU per 47 s over, five times
v3. The bench (`experiments/cw_bench`) runs all decoders on the same overs
behind the same channelizer; FADING_TRACKER.md §8 has the full tables.

**Built since this plan was written:**

| Step | What | Where | State |
|---|---|---|---|
| P2.1 (K2) | the gain tracker, one view on its evidence | `cw_gain_tracker.h`, `SKIM_V3_TRACK=1` | off by default; K2's pass criteria met |
| P3.2 (part) | impulse repair on the wideband stream, repair weights to the decoders | `impulse_repair.h`, an option of `SkimChannelizer` | measured, no pipeline uses it yet |
| P1.1 (part) | bench: Watterson paths, impulses, clipping, Class A, crowded band, drift, chirp, a second station in the channel | `cw_bench` | done |
| — | the Bell (1977) decoder as a reference, envelope and coherent | `decode_bell`, BELL.md | bench only |

**The gap to v2 narrow.** White noise, SNR500 for 10 % / 3 % payload CER:

| WPM | v3 | v3 + K2 | `dcw2n` | K2's gap |
|---|---|---|---|---|
| 12 | −1.1 / −0.5 | −1.1 / −0.3 | −5.8 / −4.0 | 4.7 / 3.7 dB |
| 25 | −0.6 / 0.0 | −0.7 / +0.6 | −3.7 / −1.7 | 3.0 / 2.3 dB |
| 35 | +0.2 / +1.0 | −0.1 / +1.9 | −2.1 / −0.5 | 2.0 / 2.4 dB |

Payload CER at 10 dB, 24 WPM:

| | v3 | v3 + K2 | `dcw2n` |
|---|---|---|---|
| ITU good | 0.121 | 0.054 | 0.032 |
| ITU poor | 0.459 | 0.147 | 0.125 |
| ITU low latitude (10 Hz) | 0.635 | 0.191 | 0.272 |
| flat 3 Hz | 0.560 | 0.203 | 0.200 |
| two paths beating ±0.5 Hz | 0.621 | 0.442 | 0.397 |
| a second station 30 Hz away, same level | 0.561 | 0.842 | 0.202 |
| chirp 30 Hz / 20 ms, 6 dB | 0.020 | 0.043 | 0.000 |

- **Fading is nearly closed.** K2 is level with v2 narrow at 10 dB on
  ITU poor and flat 3 Hz, ahead at 10 Hz, ahead at 20 dB nearly
  everywhere.
- **What is left is weak-signal sensitivity**, 2–5 dB on white noise,
  and **selectivity**: v2 narrow reads three 18.75 Hz bins, v3 a 60 Hz
  low-pass, so a second station in the channel costs v3 far more.
- **Impulses** behind the repair: v2 narrow 0.000–0.004, K2 up to 0.025.
- **No false characters** on noise for either.

**What could make up the weak-signal gap,** each with a measured or
estimated size:

| Source | Evidence | Size | Step |
|---|---|---|---|
| non-coherent evidence near threshold | `bell:coh` (a gain filter per survivor) against v3 on white noise | 1–3.5 dB at 12–25 WPM | K6 (S3) |
| clock acquisition at slow speed | §2: the bootstrap learns a wrong dit at the cliff | up to 4.3 dB at 12 WPM, 2.0 at 18, 1.2 at 25 (not re-measured with K2) | P3.4, the hypothesis bank |
| decision lag and max-product | DeepCW reads seconds of context both ways; v3 commits 4–8 dits back, best path only | unmeasured | P2.3 (forward-backward), a longer lag |
| text prior | DeepCW was trained on text; v3 uses the dictionary only in its re-reads | unmeasured; the bench text is mostly random callsigns | P2.4, P3.5 |

These overlap; S1 measured them (below).

**S1: where the gap is.** White noise, SNR500 for 10 % / 3 % payload CER,
20 overs a step (DeepCW 10), `--lead 2`; the oracles from the over's start
(`oracle.h` `clock`, `freq`, the latter now also fixing v3's matched-filter
carrier):

| | 12 WPM | 25 WPM | 35 WPM |
|---|---|---|---|
| v3 | −1.08 / −0.16 | −0.39 / +0.27 | +0.86 / — |
| v3, true clock and carrier | −1.15 / −0.70 | −0.93 / −0.01 | +0.27 / +1.11 |
| K2 | −1.07 / −0.20 | −0.78 / +0.52 | −0.21 / +1.75 |
| K2, true clock and carrier | −1.11 / −0.25 | −0.85 / +0.05 | −0.42 / +1.73 |
| K2, true clock and carrier, B 0.05 Hz (near-known gain) | −1.18 / −1.00 | −1.07 / −0.39 | −0.86 / +0.36 |
| K2, 20 Hz low-pass ahead of the sum | −1.15 / −0.30 | −0.52 / +0.91 | +0.56 / +3.28 |
| v3, random groups | −1.05 / −0.12 | −0.40 / +0.45 | +1.57 / +3.55 |
| DeepCW v2 narrow, the bench's text | −5.82 / −4.01 | −3.67 / −1.70 | −2.13 / −0.51 |
| DeepCW v2 narrow, random groups | −5.89 / −4.01 | −3.19 / −1.47 | −2.09 / −0.41 |
| DeepCW v1, random groups | | −3.32 / −1.67 | |
| `bell:coh` (BELL.md, per-survivor gain) | −4.5 / −2.7 | −1.4 / +0.1 | +0.7 / +1.6 |

- **The clock is not the gap.** The true clock and carrier gain K2 0.1–
  0.2 dB at 10 % (v3 up to 0.6). The "up to 4.3 dB at 12 WPM" of §2 does
  not hold on this bench now.
- **Nor is a better-known gain under soft association.** With the true
  carrier and clock and a near-static model, K2 gains 0.1 / 0.3 / 0.65 dB
  at 10 % (0.8–1.4 dB at 3 %): its key-down weights stay uncertain near
  threshold and V never shrinks. `bell:coh`, updating each survivor with
  its own hard keying, is 3.4 dB ahead of K2 at 12 WPM, 0.6 at 25: the
  coherence gain needs per-survivor beliefs (K6).
- **DeepCW's text prior is small:** random groups cost v2 narrow 0.0–0.5
  dB, v1 0.1 dB.
- **The evidence window cannot simply widen.** A whole-dit sum breaks the
  lattice (CER 0.26–0.36 at 0–4 dB: the sum stretches marks and shrinks
  spaces by its own length, and the duration priors expect ½ dit); a
  20 Hz low-pass gains 0.1 dB at 12 WPM and costs 0.3–0.8 dB above. DeepCW
  reads 80 ms windows and has learned to see through the smearing; the
  lattice would need an edge-aware likelihood to do the same.
- **The budget, 25 WPM (K2 → v2 narrow, 2.9 dB):** clock and carrier
  ≈ 0.1, a better gain under soft association ≈ 0.2, text ≈ 0.5, per-
  survivor coherence ≈ 0.6 (`bell:coh` against K2); about 1.5 dB left in
  the sequence model and the evidence's time resolution. At 12 WPM
  per-survivor coherence is most of it (3.4 of 4.7 dB); at 35 WPM it is
  none of it (`bell:coh` behind K2), and the gap (1.9 dB) is in time
  resolution and keying edges (BELL.md: Bell's loss at 35 WPM is the
  edges).

**S4, first measurements** (K2, white noise, 20 overs a step, SNR500 for
10 % / 3 % CER at 12 / 25 / 35 WPM; none of it is in the code):

- **The decision lag is not it.** Commit lags of 8 / 4 (as now), 16 and 32
  dits: within 0.2 dB of each other. Best-path decoding rarely revises
  anything older than 8 dits.
- **Nor the keying edges.** Raised-cosine edges of 1, 5 and 10 ms move K2
  by at most 0.3 dB and DeepCW v2 narrow by 0.1 dB.
- **Whole-element coherent evidence helps on a steady signal and breaks
  under fading.** A mark scored by the coherent sum of its derotated
  samples (ahead of the ½-dit sum) over its whole span, with the station's
  mark power as the amplitude and the phase unknown,
  −d·A²/σ² + ln I0(2A·|S|/σ²) in place of the per-sample sum: at 3 % CER
  −0.46 / −0.14 / +1.11 dB against K2's −0.20 / +0.52 / +1.75 (0.3–0.7 dB),
  at 10 % 0.0–0.2 dB, no false characters on noise. Under fading the
  steady amplitude reads faded marks as noise: ITU poor at 20 dB 0.024 →
  0.511, flat 3 Hz 0.018 → 0.664. The tracker's predicted amplitude in its
  place recovers slow fading (ITU good 0.015) but not fast (ITU poor at
  20 dB 0.121, flat 3 Hz 0.435) and loses the steady-signal gain (the
  prediction is diluted near threshold); derotating by the tracker's phase,
  averaging the two amplitudes, or taking the better of the segment and
  per-sample scores all lose most of the gain and keep the fading loss;
  capping the segment score lets marks merge across gaps. A Gaussian gain
  with unknown amplitude (CN(0, P)) makes false marks cheap (a mark over
  noise costs only ln(1 + d·SNR)).
- **What it says.** Coherent combining over an element is worth up to
  0.7 dB at 3 %, but only with a reliable amplitude, and a single phase
  per element does not survive fast fading. It needs coherence-limited
  blocks (several coherent sums per element where the gain moves,
  combined with the tracked amplitude), which is K5.

**The per-path decoder (R1–R6).** Coherent gain inside v3 needs a gain
filter per path on per-sample observations; `bell:coh` (BELL.md) already is
one. It is developed into the candidate for a v4 backend, each step
measured against v3 + K2 and DeepCW v2 narrow:

| Step | Work | State |
|---|---|---|
| R1 | v3's carrier detector as Bell's gate: text passes only while a carrier was seen within 4 s (`BellBackendConfig::gate`, bench `:gate`) | built: false characters on noise 97 → 0 an hour, nothing else moves |
| R2 | v3's AFC ahead of the decimation (`coarse_afc`, `:cafc`); the station's spread measured and shared | the edge fixed (62.5 Hz 0.70 → 0.04) at a cost on centred tones (12 WPM 1 dB, 25 WPM 0.3 dB: the estimate's moves disturb every path's filter); holding, stepping, per-over, smoothed and a 250 Hz decoder all worse; off. The spread measured (`measure_spread`, `:mspread`): built, below |
| R3 | observations at 250 Hz with correlated noise, an edge-aware model | not needed as planned: keying edges of 1–10 ms move Bell by ≤ 0.4 dB, and its threshold rises with the speed as the energy per dit falls (12 → 35 WPM: 4.9 dB against 4.6 dB of energy); DeepCW's rises only 3.7 dB, its context helps it at speed. 250 Hz as it stands breaks (25 WPM +2.2 dB) |
| R4 | the beam: 25 / 50 / 100 paths (`BellConfig::paths`, `:paths<n>`) | built, below: 100 paths 0.2–0.8 dB better at 18–35 WPM, nothing at 12 |
| R4b | a noise floor in the score at the path's predicted mark power over 1000 (`coh_floor_ratio`, `:floor1000`), as K2's | built: 60 dB 0.099 → 0.004, 20–40 dB ≤ 0.012, fading and weak signals unchanged |
| R5 | impulses: repair weights, the sferic anomaly | done, below: the impulse term (ε 0.01, κ 100, scoring only) and the repair weights |
| R6 | within 1.5 dB of `dcw2n` at 12–25 WPM, no false characters: a v4 backend | — |
| R7 | the station's speed: the paths kept within ±15 % of an average of the posterior speed (`BellConfig::speed_lock`, `:slock`) | built, below: 12 WPM +0.65 dB (3 %: +1.0), 25–35 WPM unchanged |
| R8 | the duration model's sharpness learned per station (`BellConfig::dur_learn`, `:dlearn`) | built, below: 10 % CER +0.1–0.2 dB, 3 % +0.3–0.5 dB; the bound's 2x was worth 0.6 dB |

Bell with the gate and the coarse AFC, white noise, SNR500 for 10 % / 3 %
at 12 / 25 / 35 WPM, and fading at 10 dB (ITU poor, flat 3 Hz, beat):

| | 12 | 25 | 35 | ITU poor | flat 3 Hz | beat |
|---|---|---|---|---|---|---|
| one spread (0.5 Hz) | −3.79 / −2.02 | −1.28 / −0.32 | +1.14 / +2.44 | 0.237 | 0.641 | 0.596 |
| the bank {0.3, 1, 3} Hz | −2.08 / +1.56 | −0.37 / +1.25 | +1.61 / — | 0.143 | 0.123 | 0.257 |
| v3 + K2 + K3 | −1.11 / −0.49 | −0.82 / −0.02 | −0.18 / +1.30 | 0.152 | 0.202 | 0.435 |

One spread wins on a steady signal, the bank under fast fading. The
spread measured on the station and shared by its paths (`:mspread`: K3's
estimator on the runs the decoder holds key-down with P > 0.9, 1.3 × the
measured spread, at least 0.3 Hz), with the gate and without the coarse
AFC:

| | 12 | 25 | 35 | ITU poor | flat 3 Hz | beat | 10 Hz |
|---|---|---|---|---|---|---|---|
| one spread (0.5 Hz) | −4.28 / −3.16 | −1.45 / −0.28 | +0.47 / +1.66 | 0.226 | 0.628 | 0.606 | 0.715 |
| measured spread | −4.56 / −3.24 | −1.48 / −0.39 | +0.38 / +1.54 | 0.138 | 0.287 | 0.366 | 0.548 |
| DeepCW v2 narrow | −5.82 / −4.01 | −3.67 / −1.70 | −2.13 / −0.51 | 0.125 | 0.200 | 0.397 | 0.272 |

- 1.3 / 2.2 / 2.5 dB from `dcw2n` at 12 / 25 / 35 WPM: R6's bar is met at
  12 WPM; the gap grows with the speed (R3).
- Slow and moderate fading as the bank or better; fast fading weaker (the
  estimator reads 3 Hz as about 0.8 Hz; 10 Hz is beyond the per-sample
  filters); no false characters on noise.
- Strong stations: 0.099 at 60 dB (one spread 0.718), fixed by R4b.

With R4b and the beam (gate, measured spread, floor 1000), white noise,
SNR500 for 10 % / 3 %:

| paths | 12 | 18 | 25 | 35 | CPU per 27 s over |
|---|---|---|---|---|---|
| 25 | −4.56 / −3.24 | −3.23 / −1.97 | −1.48 / −0.39 | +0.38 / +1.54 | 0.16 s |
| 50 | −4.40 / −2.63 | | −1.66 / −0.37 | | 0.30 s |
| 100 | −4.37 / −2.91 | −3.41 / −2.04 | −2.09 / −0.41 | −0.43 / +0.99 | 0.60 s |
| `dcw2n` | −5.82 / −4.01 | about −4.3 | −3.67 / −1.70 | −2.13 / −0.51 | |

v3 takes 0.08 s, the channelizer included. 100 paths are within 1.5–1.7 dB
of `dcw2n` at 12–35 WPM; fading at 10 dB as with 25 (ITU poor 0.123, flat
3 Hz 0.291, beat 0.359); no false characters on noise.

**R5, impulses** (behind the repair, 25 WPM, 3 / 6 / 10 dB, 100 paths).
Without an impulse term Bell reads what the repair leaves of an impulse
as an extra element (`#`, `*..--.*`: codes with dits inserted), more so
the stronger the station, as a stronger gain explains a spike better
than the unit noise: sferics 0.063 / 0.109 / 0.126. With BellConfig's
impulse term in the score only (`:imp<ε>:kappa<κ>:impscore`) and the
repair weights (`:repw`):

| ε / κ | sferics 6 / 10 dB | crashes 6 / 10 dB | 25 WPM 10 % / 3 % | ITU poor 10 / 20 dB |
|---|---|---|---|---|
| none | 0.096 / 0.118 | 0.037 / 0.034 | −2.09 / −0.41 | 0.123 / 0.014 |
| 0.05 / 30 | 0.005 / 0.022 | 0.006 / 0.009 | −1.73 / −0.29 | 0.117 / 0.024 |
| 0.01 / 100 | 0.010 / 0.014 | 0.008 / 0.014 | −2.05 / −0.34 | 0.106 / 0.016 |

**Bounds on the R-series' structure** (white noise, SNR500 for 10 % / 3 %,
20 overs a step; the bench keys with ±3 % jitter):

| | 12 WPM | 25 WPM | 35 WPM |
|---|---|---|---|
| R-series | −4.35 / −2.68 | −2.05 / −0.34 | −0.38 / +1.26 |
| speed known to ±10 % (`:wpm<lo>-<hi>`) | −4.84 / −2.69 | −1.92 / −0.07 | −0.39 / — |
| duration model 2x sharper (`:dsharp2`) | −4.61 / −3.23 | −2.07 / −1.05 | −0.61 / +1.08 |
| … 3x sharper | −4.90 / −3.34 | −1.81 / −0.47 | −0.17 / +1.68 |
| decision delay 2 s (`:delay400`) | −4.51 / −3.36 | −2.03 / −0.21 | −0.38 / +1.03 |
| … 3 s | −4.65 / −3.41 | −2.03 / −0.23 | −0.38 / +0.99 |
| 200 paths | −4.26 / −3.10 | −2.03 / −0.28 | −0.51 / +1.18 |
| frequency known (`:ofreq`) | −4.31 / −2.56 | −2.04 / −0.34 | −0.33 / +1.26 |
| frequency and gain known (`:ogain`) | −4.63 / −1.50 | −2.39 / −1.18 | −1.00 / +0.63 |
| every channel sample, 250 Hz (`:rate250`) | −3.45 / −0.53 | +0.16 / — | — / — |
| DeepCW v2 narrow | −5.82 / −4.01 | −3.67 / −1.70 | −2.13 / −0.51 |

- None of them holds the 1.5 dB. The speed, known to ±10 %, gains 0.5 dB
  at 12 WPM and nothing above; a 2x sharper duration model 0.0–0.3 dB at
  10 % and up to 0.7 dB at 3 % on keying this regular (a real fist needs
  it learned per station, as v3's); a longer decision 0.2–0.3 dB at
  12 WPM; the beam is saturated at 100 paths.
- Bell's paths started at the listing's 10–50 WPM even with a narrower
  speed range, and with a range that excluded them decoded nothing; they
  start inside the range now.
- The oracles (`:ofreq`, `:ogain`; the bench hands the channel the target
  alone through a second filter bank, SkimDecodeChannel::set_reference):
  the frequency known gains nothing; the frequency and the complex gain
  known (every path scores the exact likelihood, zero variance) 0.3 dB at
  12 and 25 WPM, 0.6 dB at 35. The front end holds at most that much of
  the gap; about 1.2 dB is in the lattice and what DeepCW reads beyond
  the elements (its context, about 0.5 dB of text prior at 25 WPM by S1).
  With the gain known, 12 WPM keeps an error floor of about 2.5 % up to
  +1 dB, so its 3 % point is worse than the R-series': the lattice's own
  errors.
- Random text (`--text random`, the same table's form) takes away what
  DeepCW knows of QSO text:

  | | 12 WPM | 25 WPM | 35 WPM |
  |---|---|---|---|
  | R-series | −4.26 / −3.14 | −2.15 / −0.58 | −0.37 / +0.92 |
  | gain known | −4.69 / −3.27 | −2.50 / −1.30 | −0.92 / +0.32 |
  | … and the speed to ±10 % | −5.36 / −4.14 | −2.51 / −1.08 | −0.93 / +0.49 |
  | … and durations 2x sharper | −6.00 / −4.50 | −3.12 / −1.42 | −1.55 / −0.10 |
  | DeepCW v2 narrow | −6.19 / −4.35 | −3.23 / −1.78 | −1.85 / −0.31 |

  DeepCW's text prior is 0.3–0.4 dB at 25 and 35 WPM, none at 12 (the
  two texts' characters differ, so loosely). With the gain, the speed
  and sharp durations given, Bell is within 0.1–0.3 dB of DeepCW at every
  speed and past it at 3 % at 12 WPM. The gap is, at 10 % CER: the gain
  filters 0.35–0.55 dB, the speed 0.7 dB at 12 WPM (none faster), the
  duration model 0.6 dB at every speed. The bench keys with ±3 % jitter;
  DeepCW reads a fist's timing from its 8 s of context, so the fair
  equivalent is the speed and the durations' spread learned per station,
  not fixed.
- **R7, the station's speed** (`:slock`). A slow average (4 s) of the
  posterior speed over the samples held key-down with P > 0.9, after
  0.5 s of them; after 2 s more the paths' speeds stay within ±15 % of
  it, and a step past the lock's limit lands on it (dropped, as SPDTR
  drops a step past the listing's limits, it leaves an error floor of 3 %
  at 35 WPM on any narrow range, static or locked). Acquired only while
  the posterior's relative spread about the average is under 0.08: on a
  weak station it wanders (0.1–0.6) and reads a 35 WPM station at
  8–22 WPM. Released after 1.5 s without a sure key-down sample (the gap
  between overs), or when the sure runs' average log distance from 1 or
  3 locked dits passes 0.38 (a speed change within an over: held in the
  range, the posterior reads it as a consistent text at the locked speed
  and never presses on a limit).

  | SNR500, 10 % / 3 % | 12 WPM | 25 WPM | 35 WPM | 12 WPM, ±10 % jitter | 12 WPM, random text |
  |---|---|---|---|---|---|
  | R-series | −4.35 / −2.68 | −2.05 / −0.34 | −0.38 / +1.26 | −4.41 / −1.65 | −4.26 / −3.14 |
  | + the speed lock | −4.99 / −3.69 | −2.04 / −0.37 | −0.37 / +1.21 | −4.82 / −3.38 | −4.72 / −3.48 |

  Payload CER at 0 / 6 / 15 dB, the text's second half at another speed
  after 2 s (bench `--wpm-step`): 25 → 15 0.017 / 0.004 / 0.005, 15 → 25
  0.022 / 0.002 / 0.004, 12 → 30 0.024 / 0.005 / 0.002, 35 → 18 0.068 /
  0.004 / 0.010, 20 → 28 0.032 / 0.004 / 0.000 (without the lock 0.002–
  0.046). ITU poor at 24 WPM 0.493 / 0.100 / 0.017 at 0 / 10 / 20 dB
  (without 0.498 / 0.100 / 0.017), ITU good 0.297 / 0.035 / 0.007 (0.307
  / 0.042 / 0.009); no false characters on noise.
- **R8, the station's timing** (`:slock:dlearn`). While the speed lock
  holds, each sure run (key-down peaking over 0.99; key-up under 0.01
  throughout and under 5 locked dits) is measured from the nearest
  nominal length, 1 or 3 locked dits; the average distance (about 20
  runs) is XTRANS's Laplace scale, and the sharpness 1/3 dit over it,
  1–3x. It reads 3x on machine keying at high SNR, 2x at ±15 % jitter
  and 10 dB, but only 1.4–1.8x near the thresholds, where the detection's
  own timing error widens the runs: hence less than the fixed 2x bound.

  | SNR500, 10 % / 3 % | 12 WPM | 25 WPM | 35 WPM |
  |---|---|---|---|
  | R7 | −4.99 / −3.69 | −2.04 / −0.37 | −0.37 / +1.21 |
  | R7 + R8 | −5.10 / −4.03 | −2.12 / −0.85 | −0.44 / +0.92 |
  | R7, random text | −4.72 / −3.48 | −2.08 / −0.74 | −0.37 / +0.92 |
  | R7 + R8, random text | −4.90 / −3.74 | −2.24 / −1.02 | −0.47 / +0.86 |
  | DeepCW v2 narrow | −5.82 / −4.01 | −3.67 / −1.70 | −2.13 / −0.51 |
  | DeepCW v2 narrow, random text | −6.19 / −4.35 | −3.23 / −1.78 | −1.85 / −0.31 |

  Looser keying: ±10 % jitter at 12 WPM −4.92 / −3.63 (R7 −4.82 /
  −3.38), at 25 WPM −2.01 / −0.66 (−1.94 / −0.38); ±15 % at 25 WPM
  −2.08 / −0.65 (−1.97 / +0.04). ITU poor at 24 WPM 0.493 / 0.095 /
  0.015 at 0 / 10 / 20 dB; speed steps 15 → 25 0.022 / 0.000 / 0.000,
  35 → 18 0.079 / 0.002 / 0.008. The gap to DeepCW, 10 % CER: 0.7 / 1.6
  / 1.7 dB on QSO text, 1.3 / 1.0 / 1.4 dB on random text.
- The decimation is not what costs at speed: every channel sample
  (250 Hz) does worse, more so the faster (an error floor of 5 % at
  25 WPM and 15 % at 35, +2 dB), and neither 200 paths nor dropping the
  impulse term moves it; the fold costs at most a fixed fraction of a
  dB. The channel's edge (below) needs the station centred before any
  narrowing, not a filter on the channel.

CPU per 27 s over (one thread, bench and channelizer subtracted): v3
0.05 s, the R-series 0.73 s with 100 paths and 0.12 s with 25, DeepCW v2
narrow 0.39 s, v2 wide 3.0 s, v1 9.4 s (BELL.md, "The R-series
configuration", has the full tables).

ε 0.01 / κ 100 is the setting: sferics level with v3 + K2 + K3 (0.002 /
0.019), crashes better, the clean ladder and fading unchanged. 10 pulses
a second, ignition and Class A at 0.000–0.012 (with ε 0.05).

**The spread's calibration.** On the decoder's sure marks the estimator
reads (10 / 20 dB) 0.20 with no fading, 0.23 at 0.1 Hz, 0.25–0.29 at
0.3, 0.67–0.72 at 1, 0.94–1.14 at 2, 1.18–1.68 at 3, 1.06–1.35 at 5,
1.30–2.20 at 10: a faded stretch is not a sure mark. The paths take 1.3x
the measured spread under 0.5 Hz (at least 0.3 Hz) and 2.5x as steep
above. With 100 paths, the impulse term and the repair weights, payload
CER at 10 / 20 dB (`dcw2n` in brackets): ITU good 0.042 / 0.009 (0.032 /
0.011), ITU poor 0.100 / 0.017 (0.125 / 0.047), flat 3 Hz 0.235 / 0.059
(0.200 / 0.111), beat 0.289 / 0.058 (0.397 / 0.258), 10 Hz 0.479 / 0.247
(0.272 / 0.241); white noise at 10 % −4.35 / −2.05 / −0.38 dB at 12 / 25
/ 35 WPM, unchanged.

**The order from here.** It replaces §3's order for the steps it names;
each step keeps its pass criteria and its switch, byte-identical when off.

| Step | Work | Pass / decides | Effort |
|---|---|---|---|
| S0 | **Input level and amplitude, all decoders.** Strong signals, SNR500 20–60 dB, steady and under 1 Hz fading, are measured: DeepCW v1 and v2 narrow lose nothing (0.000 steady, 0.014 / 0.044 fading) although v2 narrow's 8-bit input clips about 30 dB over the per-bin noise (v2 wide's about 47 dB; v1 is not quantised); K2 broke (0.34 at 30 dB, 0.83–0.89 at 40–60) until its score got a noise floor at the predicted mark power over 1000 (now 0.012–0.018 steady, 0.002–0.010 fading; FADING_TRACKER.md §8). The absolute input scale (±40 dB) changes no decoder's output at all; level steps of the target (−10, +20 dB) and of the noise (±10 dB) mid-over cost none of them more than 0.02. A receiver AGC pumped by a 60 dB station elsewhere in the band (instant attack, the target's SNR unchanged) costs all of them, the Bayesian ones most: decay 200 ms, at 10 / 20 dB, v3 0.350 / 0.189, K2 0.213 / 0.107, DeepCW v1 0.076 / 0.068, v2 narrow 0.082 / 0.058; decay 1 s v3 0.076 / 0.048, K2 0.080 / 0.042, v1 0.080 / 0.078, v2 narrow 0.012 / 0.010. The decoders' noise estimates follow the level too slowly; v2 narrow's 8 s windows rescale best | whether DeepCW's lead holds at real band levels, and what any decoder needs from the front end (scaling, AGC rules) | 1–2 days |
| S1 | **Gap attribution** (P1.2, narrowed): on the white-noise ladder at 12 / 25 / 35 WPM, v3 + K2 with the true clock (`oracle.h` `clock`), with the true complex gain in the tracker, and with both; DeepCW on random-letter text against the bench's text | a dB budget: clock, coherence, the rest; it orders S2–S5 | 3–5 days |
| S2 | **K3, the spread and the frequency offset per station** (P3.1; built, `SKIM_V3_SPREAD=1`, FADING_TRACKER.md §8: the 3 % thresholds 0.3–0.5 dB better, the rest level with K2; chirp and beating not met): B measured from committed marks and shared by the station's hypotheses, δf tracked so B can come down from 1 Hz | the 3 % gap on white noise closed (K2 at B = 0.5 Hz already matches v3); chirp 30 Hz / 20 ms at 6 dB ≤ v3; beating ±0.5 Hz at 10 dB < 0.40; slow fading not worse | 1–2 weeks |
| S3 | **K6, a gain estimate per lattice node** (per-survivor processing; tried in v3's lattice and left out, FADING_TRACKER.md §8: the gain either fits the noise inside a false mark or cannot follow a fade; retry after S4, on per-sample observations as `bell:coh`): each node carries (µ, V) from its best predecessor and updates it with its own hypothesis's keying; K2's soft estimate seeds new paths | white noise within 1.5 dB of `dcw2n` at 12–25 WPM (at least 1.5 dB over K2); CPU per channel measured; fading not worse | 2–3 weeks |
| S3b | **K5, coherence-limited blocks** (tried and left out, FADING_TRACKER.md §8: no amplitude both reliable near threshold and following a fade) | — | — |
| S4 | **The lattice and the edges** (S1: the clock bound is 0.1–0.6 dB, so the hypothesis bank P3.4 waits): an edge-aware likelihood (the keying edge and the matched filter's ramps modelled, so the evidence can use a longer, narrower window without stretching marks), and the forward-backward pass with a longer lag (P2.3) | at 25–35 WPM within 1 dB of `dcw2n`; no false characters on noise | 3–5 weeks |
| S5 | **K4, the smoother at commit**: σ², P, B and δf from committed stretches; the state restarted from the smoothed estimate | dense impulse trains without the repair at least v3's (ignition 50/s at 10 dB ≤ 0.02); feeds S2 | 1 week |
| S6 | **Selectivity**: the matched filter's low-pass tied to the tracked carrier and narrowed with it; a second carrier in the channel tracked or rejected | second station at 30 Hz, same level ≤ 0.30; offsets to 62.5 Hz unchanged | 1–2 weeks |

**Alongside:**

- **K2 into v3's default**, with the repair in front, once S2 closes the
  3 % gap: it already halves to quarters v3's fading errors at no cost
  worth measuring.
- **DeepCW v2 narrow as an option** where the CPU allows: it is the best
  decoder on the bench today, and the classical one stays the fallback for
  a full band.

**Decision points.**

| Point | After | Question | If yes | If no |
|---|---|---|---|---|
| T0 | S0 | does any decoder lose copy at real band levels (above 30 dB, level steps, AGC)? | fix in the front end first (scaling, the noise estimate) | the comparison stands |
| T1 | S1 | does the clock bound exceed the coherence bound? | the bank (P3.4) before S3 | S3 first. **Answered: no** (clock 0.1–0.6 dB; per-survivor coherence up to 3.4 dB at 12 WPM). S3 (K6) next, then S4 (edges, lattice), S2 alongside |
| T2 | S3 | is K6 within 1.5 dB of `dcw2n` at 12–25 WPM? | S4 and S6 for the rest | the gap is not in the evidence: measure DeepCW's text prior and context (S1's second half) before more Bayesian work |
| T3 | S2–S6 | within 1 dB of `dcw2n` on white noise, at or under it on the fading and selectivity tables, at under twice v3's CPU? | v4 preset | the hybrid: DeepCW where the CPU allows |

---

## 1. Rules for every step

1. **One switch per change.** Every change goes behind a `CwDecoderConfig`
   switch with an environment override (`SKIM_…`), default off, as v3 did
   next to v2. Accepted switches are collected into a new preset (a `v4`
   next to `cw_preset_v2` and v3) rather than flipping v3's defaults, so v3
   stays the reference.
2. **Byte-identical when off.** With the switch off, `cw_engine_cw_test`
   (67 checks) and `cw_engine_call_test` (65 checks) produce byte-identical
   output.
3. **No regression when on.** With it on, both gates pass, and so do
   `cw_engine_bench --gate quick` and `--gate startup` (once Phase 0 makes
   the latter pass).
4. **Bench, then air.** A step is measured on the bench
   (`experiments/cw_bench`, `bench.py ladder / offset / startup / fading /
   impulse / noise`), then A/B on replayed off-air IQ.
5. **False characters on noise never rise.** `bench.py noise` and the
   noise-only beds are run for every step. Every gain in sensitivity is
   paid for here first.
6. **From Phase 1 on, judge callsigns by cost, not count:** misses +
   k·busts (k = 5 to start), split by SCP and non-SCP calls
   (BAYESIAN_DECODING.md §2.5). The number of calls detected alone rewards
   snapping to the dictionary.
7. **Compare against the frozen baseline** of P1.6, not against the
   previous step's tuning.

The user builds and runs the tests; each step ends with the commands to run
and the numbers to compare.

---

## 2. Where the losses are

Measured on the bench (ALGORITHM.md E0, E3, §5.3.6). These numbers set the
order of the plan.

| Loss | Size | Cause | Addressed by |
|---|---|---|---|
| Fading | 9 dB behind CW Skimmer at 0.1 Hz Doppler, over 10 dB at 1 Hz; 15 % CER floor at +20 dB | no amplitude tracker follows a fade; σ1² held 2 s, µ_mark 0.35 s | P0.1 (part), P2.1: built (K2, §0), ITU poor at 10 dB 0.46 → 0.15 |
| Impulses | 3/s at 40 dB: CER 1.2 → 32 %; 10/s: 250 false characters per hour | no impulse model at all | P1.5 (part), P3.2: the repair built (§0), sferics at 6 dB 0.755 → 0.030 |
| Clock acquisition at slow speed | up to 4.3 dB at 12 WPM, 2.0 at 18, 1.2 at 25 | the bootstrap learns a wrong dit at the cliff | P3.4 |
| The weak-view pause rule | 0.9 dB at 18 WPM | judged on the 45 Hz envelope, not on the matched path | P0.6 |
| Channel start | 35 WPM: 9 % CER floor at any SNR | zero-history channelizer output, squelch opens on noise | P0.2, P0.3 |
| Channel edge | 2–3 dB for signals more than 55 Hz off a channel centre, 0.5–1 dB on average | the prototype rolls off towards the edge | P0.4 |
| v2 gating bandwidth | 1.8 dB in the v2 view | squelch, duty and bootstrap read the raw channel envelope | P0.5 |
| Busts from dictionary snapping | unmeasured | an SCP call within 5 score units of the reading wins; the reading as heard never competes | P1.4, Phase 4 |
| Speed-matched linear filters | under 0.3–0.6 dB left | — | nothing to do |

---

## 3. Overview

§0 sets the order from here for the steps it names (S0–S6); the phases
below keep the rest. Five phases. From Phase 2 on, two lines run side by side: the **main line**
improves the evidence, and the **side line** builds the Bayesian structure
(priors, the forward algorithm, the language model) so that it is ready, and
byte-identical, when the evidence settles. The side line is built and
checked, but **not tuned** until the main line's B1 is accepted, because
everything it fits depends on the evidence.

```mermaid
gantt
    dateFormat  YYYY-MM-DD
    axisFormat  w%W
    section Phase 0
    P0 quick wins (E1 E2 E3, passband, v2 envelope)   :p0, 2026-11-23, 7d
    section Phase 1
    P1.1 bench extensions                              :p11, 2026-11-23, 14d
    P1.2 oracle bounds (bench)                         :p12, after p11, 7d
    P1.3 off-air oracle runs                           :p13, 2026-11-30, 7d
    P1.4 bust metric and probe                         :p14, 2026-11-30, 10d
    P1.5 outlier term                                  :p15, 2026-12-07, 3d
    P1.6 baseline freeze                               :milestone, m1, after p12, 0d
    section Main line
    P2.1 gain tracker (K1 prototype, K2 C++)           :p21, after p12, 42d
    P3.1 slow channel parameters, tracker bank         :p31, after p21, 21d
    P3.2 impulse marking                               :p32, after p21, 21d
    P5 smoother, coherent blocks                       :p5, after p31, 28d
    section Side line
    P2.2 normalised priors (identical output)          :p22, after p12, 10d
    P2.3 semiring lattice, forward scores              :p23, after p22, 14d
    P2.4 language-model data and grammars              :p24, after p12, 21d
    section Decision layer
    P3.3 retune SCP logic on new evidence              :p33, after p21, 7d
    P3.4 hypothesis bank speed x fist                  :p34, after p21, 28d
    P3.5 language-model rescoring                      :p35, after p34, 21d
    P4 callsign posterior (C4-C7)                      :p4, after p35, 42d
```

The chart assumes one developer interleaving the lines; the bars show the
order and the dependencies, not exclusive weeks. §11 gives the effort.

**Milestones.**

| Milestone | After | What it gives |
|---|---|---|
| M0 | Phase 0 | the measured quick wins in a `v4` preset: channel start, channel edge, part of the fading loss |
| M1 | Phase 1 | a bench and an off-air harness that measure busts, fading, impulses, fists; oracle bounds; the frozen baseline |
| M2 | P2.1 | faded marks stay marks; one calibrated evidence for all SNR; the first release worth shipping |
| M3 | Phase 3 | impulse handling, the hypothesis bank in place of the decision-directed clock, the language model rescoring overs |
| M4 | Phase 4 | callsign posteriors and a calibrated spot gate |

---

## 4. Phase 0: quick wins (days)

Already built, or a few lines. Apply them together behind their switches,
A/B them on off-air replays, and put the accepted ones into the `v4` preset.

### P0.1 The view at word spaces (E3)

- **Change:** `experiments/view-at-word-space.diff`; the weak/strong view
  may change at a live word space, not only between overs.
- **Switch:** `CwFrontEndConfig::view_at_word`, `SKIM_V3_VIEW_WORD=1`.
- **Measured:** CER at +10 dB: ITU moderate 0.30 → 0.25, ITU poor
  0.51 → 0.41, 1 Hz 0.45 → 0.36. Unchanged at 0 dB and below, without
  fading, and on the ladder.
- **To do:** off-air A/B on replays with fading.
- **Pass:** no off-air regression; gates as measured.
- **Lifetime:** a stopgap; P2.1 removes the view switch it works around.
- **Effort:** 1–2 days.

### P0.2 The slow-clock witness (E2)

- **Change:** `experiments/acquire-settle-and-witness.diff`, the witness part;
  on a strong channel a second Schmitt trigger keeps collecting marks, and
  the clock jumps when the shortest tight class sits under 0.6 dit.
- **Switch:** `CwAcquireConfig::witness`, `SKIM_V2_WITNESS=1` /
  `SKIM_V3_WITNESS=1`.
- **Measured:** 35 WPM CER 5.1 → 1.8 % and 10.3 → 6.2 %; never engages at
  low SNR.
- **Pass:** `startup` case improves; ladder unchanged.
- **Effort:** 1 day.

### P0.3 Squelch settling, v2 view only (E2)

- **Change:** the same diff, the settle part; the squelch stays shut for the
  first 0.5 s while the floor learns from every sample.
- **Switch:** `CwAcquireConfig::settle_s`, `SKIM_V2_SETTLE_S=0.5`.
- **Measured:** channel-start CER at 14 dB 3.5–10.3 % → 0.4–1.5 %, no failed
  overs. In v3 it costs 0.3–0.5 dB at 3 % CER, because the bootstrap then
  learns a too-fast clock at a marginal onset.
- **Decision:** enable for v2 and the strong view only; for v3 wait for
  P3.4, which removes the bootstrap the cost comes from.
- **Pass:** `cw_engine_bench --gate startup` passes (registered `WILL_FAIL`
  today; flip it).
- **Effort:** 1 day.

### P0.4 Flat channel passband (E1)

- **Change:** the skimmer module opts in to `create_ex(in_rate, 125,
  kCwFlatPassbandHz = 85, kCwFlatTaps = 12)`.
- **Measured:** flat to ±62.5 Hz (−0.2 dB), still more than 100 dB down at
  125 Hz; 0.5–1 dB on average over random offsets, 2–3 dB for signals more
  than 55 Hz from a centre.
- **Cost:** 1.5× the polyphase work; each signal shows more strongly in the
  neighbouring channel.
- **To do:** `bench.py offset`, and on off-air replays count duplicate
  decodes of one station in adjacent channels: the ghost arbitration and the
  detector's own-quarter rule must keep them out.
- **Pass:** offset thresholds flat to 62.5 Hz; no rise in duplicates; CPU
  within budget on a full band.
- **Effort:** 2–3 days.

### P0.5 v2 gating on the narrow envelope

- **Change:** v2's squelch, keying-duty test and bootstrap read the 45 Hz
  narrow envelope instead of `raw` (ALGORITHM.md §5.3.6, "Centre").
- **Switch:** new, e.g. `CwLevelsConfig::narrow`, `SKIM_V2_NARROW_GATE=1`.
- **Expected:** 0.4–0.6 dB in the v2 view, measured once in a scratch build.
- **Pass:** ladder and offset cases improve by that much; `noise` false
  characters unchanged; startup unchanged.
- **Effort:** 2 days.

### P0.6 The pause rule on the matched path (E1)

- **Change:** `experiments/matched-alive-and-flat-passband.diff`, the
  `matched_alive` part.
- **Switch:** `CwWeakViewConfig::matched_alive`, `SKIM_V3_MATCHED_ALIVE=1`.
- **Measured:** 18 WPM threshold 0.9 dB better; 12 WPM at −3 dB 80 → 46 %
  CER; weak ladder total 1056 → 1036; but one QSB rung worse (6 dB: 37 → 51).
- **Decision:** on only if off-air replays with QSB agree.
- **Effort:** 1–2 days of A/B.

---

## 5. Phase 1: measurement

Phase 1 builds what every later decision needs. It ends with a frozen
baseline (P1.6).

### P1.1 Bench extensions (K0, A1)

1. **Truth output:** the true complex gain and keying per sample, for the
   tracker prototype (P2.1) and the reliability diagrams.
2. **Long continuous streams:** minutes of one station, many overs, as
   DeepCW's runs are. Today's 25 s cold overs over-weight acquisition.
3. **Channel presets:** two paths with different Doppler shifts (beating),
   drift, chirp; the ITU-R sets exist.
4. **Keying styles:** hand key, bug, paddle, computer, with log-normal
   element and space durations per style and an on/off imbalance (DeepCW's
   four styles, ALGORITHM.md §5.4 W0).
5. **Reliability diagrams:** a `bench.py` case that bins per-sample LLRs and
   segment scores against the true keying, per condition
   (BAYESIAN_DECODING.md §1.5).
6. **Callsign-level scoring:** inserted call lists with a set share (10–20 %)
   not in the MASTER.SCP snapshot used, some of them one or two edits from
   SCP calls; for each: correct, bust, missed; plus false spots.

**Effort:** 1–2 weeks. **Implements:** FADING_TRACKER.md K0,
BAYESIAN_DECODING.md A1 items 1–3, A2 items 1–2.

### P1.2 Oracle bounds on the bench (A1 item 4)

What would perfect knowledge buy? Each bound decides whether a later step is
worth building (§10).

| Oracle | How | Decides |
|---|---|---|
| clock | `oracle.h` mode `clock`: the dit from the true WPM; the bench writes the truth file (`t0_s t1_s carrier_hz wpm call`) | the speed part of the hypothesis bank (P3.4) |
| fist | a new oracle mode: char and word space from the bench's keying parameters | the fist part of P3.4 |
| channel | each over decoded under several fixed settings (coherent span, envelope bandwidth), the best per over kept against the truth | the tracker bank (P3.1) |
| gain | the true gain fed in place of σ1² and µ_mark | the ceiling of P2.1 |
| pause | the gate held open on the true keying (`oracle.h` `gate`) | P0.6, kept apart from the bank |

Run on `ladder`, `fading`, and the long-stream mode, all speeds and styles.

**Effort:** about 1 week.

### P1.3 Off-air oracle runs

On replayed off-air IQ with CW Skimmer Server's decodes as the truth
(`SKIM_ORACLE`, `skimmer-compare/oracle-truth.py`):

- `clock` and `gate`: how much of R's stations each stage costs today;
- `kws`: is R's call in the audio at all, by forced alignment against a free
  reading (the ceiling of the call reader);
- `combine`: how far soft-combining repeats would carry a station one
  reading does not (the ceiling of P4.2).

**Effort:** a few days, plus replay time. **Note:** R's decodes are a
reference, not ground truth; use them for recall, and the bench for busts.

### P1.4 The bust metric and the quick-win probe (A2)

1. **Off-air labels:** consensus with other RBN skimmers in the same minute
   and band (precision), and, after a contest, the published logs (a call in
   no log is very likely a bust).
2. **The probe:** add the reading as heard, W, to the candidates of the word
   re-read (`cw_reread.cpp:176`) and the call reader (`cw_callreader.cpp:189`),
   with an explicit SCP bonus β in place of the hidden one:
   `take X if s(X) + β > s(W)`. A few dozen lines, behind
   `SKIM_V3_RS_RAW_BETA` / `SKIM_V3_KWS_RAW_BETA` (β unset = today's rule).
3. **Sweep β** on the bench call lists and off air; report cost =
   misses + k·busts, SCP and non-SCP separately.

The probe is a **measurement**, not a setting to tune now: it is run again
after P2.1 (P3.3).

**Effort:** 1–2 weeks; off-air labels take calendar time.

### P1.5 Outlier term in the per-sample LLR

- **Change:** an ε-contamination term in both views
  (BAYESIAN_DECODING.md §1.8): `llr ← ln[(1−ε)p₁ + εq] − ln[(1−ε)p₀ + εq]`
  with a broad q, so one sample costs at most a few nats.
- **Where:** `CwPowerStats::llr` (`cw_frontend.h:490`),
  `CwDiscriminator::llr` (`cw_discriminator.h`).
- **Switch:** `SKIM_LLR_EPS` (0 = off).
- **Measure:** `bench.py impulse` (CER and false characters per hour), the
  ladder (must not move), `fading`.
- **Effort:** 2–3 days. It carries over into P2.1's LLR.

### P1.6 Baseline freeze

One report, run with the `v4` preset of Phase 0 and with v3: `ladder`,
`offset`, `startup`, `fading` (also long streams and all keying styles),
`impulse`, `noise`, the reliability diagrams, the callsign cost table, and
the oracle bounds. Every later step reports against it.

---

## 6. Phase 2: the evidence, and the Bayesian structure beside it

### Main line

#### P2.1 The gain tracker (K1, K2; B1)

The largest measured loss. FADING_TRACKER.md has the model and the design;
BAYESIAN_DECODING.md step B1 the additions.

1. **K1, prototype in Python** on P1.1's truth dumps: AR(1) against AR(2),
   the block length, the soft data-association weighting, the estimators of
   σ², P and B. **Pass:** tracker error against the true gain; B within 30 %
   from 0.1 to 10 Hz.
2. **K2, `CwGainTracker` in C++** next to `CwPowerStats`, on the ½-dit
   coherent block sums, supplying
   `llr = ln CN(z; μ, σ² + V) − ln CN(z; 0, σ²)` as the one view when on.
3. **The scale fix, in the same change:** a space scores 0, not −Σllr
   (`cw_lattice.h:281`, the `sign` in `cw_reread.cpp:15`, `CwCallAlignment`).
   First check byte-identical Viterbi output with the duration priors halved;
   then switch the LLR; then retune the evidence/prior balance once.
4. **Innovation gating** for impulses in the tracker's state, plus P1.5's
   ε term in its LLR.

- **Switch:** `CwGainTrackerConfig::enabled`, `SKIM_GAIN_TRACKER=1`; the old
  views stay selectable.
- **Pass (K2):** fading table at +10 dB clearly below v2 (ITU moderate
  < 0.19); no-fading ladder within 0.2 dB; `noise` false characters not up;
  gates pass; reliability diagrams near the diagonal in every condition;
  no off-air regression on strong stations.
- **Effort:** 4–6 weeks. **Depends:** P1.1, P1.6.

### Side line

Built and checked for identical output; tuned only after P2.1 is accepted.

#### P2.2 Normalised duration and transition priors (C1, structure)

- Normalise each duration prior over its window (`ln Z_type`, computed in
  `sync_prior_memo`), and replace the 0.5 segment cost by transition
  log-probabilities (BAYESIAN_DECODING.md step C1).
- Behind `SKIM_PRIORS_NORM=1`. With the switch off, identical output.
- Prepare, but do not fit yet: the transition tables from text statistics,
  the ε mixture on durations, the keying-style cases of P1.1 to fit them on.
- **Effort:** 1–2 weeks.

#### P2.3 One recursion, two semirings (C2, structure)

- Template `CwLattice::step`, `forced()` and `CwCallAlignment` over the
  "add": max (Viterbi) or log-sum-exp (forward). Viterbi output
  byte-identical.
- Forward scores for every candidate of the re-read and the call reader,
  written to the debug trace only (`rescore:` and `kws:` lines), next to
  today's Viterbi scores, so their difference can be studied before anything
  depends on them.
- The filler: a forward pass over the free reading with a uniform Morse
  character prior, also trace-only.
- **Effort:** 2 weeks.

#### P2.4 Language-model data (C2b, data)

No decoder change.

1. **Grammars** (finite-state): callsigns (shape rules from
   `SkimCallsign::is_valid` and the ITU tables), RST with cut numbers,
   contest serials and zones, Maidenhead locators, times, Q-codes, prosigns.
2. **Phrases:** `CQ … DE … K`, `TU`, `73`, `GL`, `FB OM`, `UR RST`, ragchew
   vocabulary, with frequencies.
3. **Corpus and character n-gram:** decoded traffic, contest logs, RBN spot
   streams; Kneser–Ney smoothing; a held-out set; never the bench's own
   texts.
4. **Mode classes:** contest, pile-up, ragchew, with the CQ/TEST context as
   the evidence for choosing.
5. **ε_new:** the share of calls on the air not in MASTER.SCP, measured from
   a contest's logs against the SCP file of the same date.

**Effort:** 2–3 weeks, mostly data.

---

## 7. Phase 3: channel, impulses, language model, hypothesis bank

Starts when P2.1 is accepted (decision D2).

### P3.1 Slow channel parameters and the tracker bank (K3; B2)

- B, the residual carrier offset, P and the beating indicator, per channel
  and per station across overs; B sets the tracker's poles.
- **Only if P1.2's channel oracle shows ≥ 1 dB:** a bank of 2–3 trackers
  with fixed spreads during acquisition, weighted by the likelihood of the IQ
  each predicted (multiple-model filter), collapsed once B is stable.
- **Pass (K3):** B and the offset followed on the beating and drift presets;
  fading table improves further or holds.
- **Effort:** 1–2 weeks, +1 for the bank.

### P3.2 Impulse marking (B3)

- Wideband detection from coincident jumps across channels; mapping onto the
  16 output samples the channelizer smears each impulse over; per-sample
  erasure weights to `process()` through a new hook on `SkimDecodeChannel`;
  marked samples kept out of the clock, the fist, the noise floor and the
  tracker. Dense trains repaired before the channelizer, never zeroed
  (ALGORITHM.md §5.7).
- Detection lives in the skimmer module, which owns the channelizer.
- **Pass:** `bench.py impulse` at 3/s well below the baseline 32 % CER;
  10/s false characters far below 250 per hour; no change without impulses.
- **Effort:** 2–3 weeks.

### P3.3 Retune the SCP logic on the new evidence

- Run P1.4's probe again with P2.1 on. If the re-read and the call reader
  fire much less often, they were compensating for the fading loss.
- Retune `max_deficit`, `min_margin`, `min_evidence` and the call reader's
  `min_score` / `min_margin` on the cost metric, now on half the old scale
  (the scale fix).
- This is the interim callsign logic until Phase 4 replaces it.
- **Effort:** about 1 week.

### P3.4 The hypothesis bank: speed × fist (C3)

**Only if P1.2's clock or fist oracle shows a gain worth it (≥ 1 dB on the
ladder or the long streams).**

- The hypothesis index θ = (speed cell, fist variant) in the lattice state:
  5–7 speed cells 1.3–1.4× apart over 10–50 WPM, each with its own adaptive
  dit clamped to the cell; 2–3 fist variants. All read the same evidence.
- A switching prior: θ changes at over breaks and pauses, elsewhere only
  with a small probability.
- A beam: all hypotheses in acquisition, those more than about 8 nats
  behind the best (forward probability, P2.3) dropped, 1–3 kept once locked;
  a pause or turnaround re-opens the bank.
- It replaces the Schmitt bootstrap, the re-lock and the watchdogs, and most
  of the pre-roll replay. With it, P0.3's settling can be enabled for v3.
- The re-read and the call reader score each candidate under the surviving
  hypotheses and sum them.
- The side line's P2.2 priors are fitted here, on P1.1's keying styles.
- **Pass:** the ladder approaches the oracle-clock bound at 12–18 WPM;
  speed-change and startup gates pass; `noise` false characters with the full
  bank not up (BAYESIAN_DECODING.md §1.13, trap 1); CPU on a full contest
  band within budget.
- **Effort:** 3–4 weeks (+1–2 for fitting the priors). **Depends:** P2.1,
  P2.2, P2.3.

### P3.5 Language-model rescoring (C2b)

- Forward–backward on the main lattice over the commit window
  (P2.3 extended) gives the N best readings per over, and per-character
  posteriors (grey text for unsure characters in the pane).
- Rescore them by acoustic + λ·language + ln P(θ), with P2.4's model; fit λ
  on the bench (expected near 1 after the scale fix).
- Rewrite the over in the pane through `SkimPaneOp`; reach the extractor
  only through correction tokens (the `take_aux_text()` rule).
- **Pass:** CER at the cliff improves; `noise` and bed false characters do
  not rise with the language model on; no regression on ragchew text with a
  contest model loaded.
- **Effort:** 2–3 weeks. **Depends:** P2.3, P2.4, P3.4.

---

## 8. Phase 4: the callsign posterior

Replaces counts and points with probabilities (BAYESIAN_DECODING.md C4–C7).

| Step | What | Effort |
|---|---|---|
| P4.1 (C4) | candidates: the reading as heard, N-best readings, SCP neighbours, recent calls; the open-world prior `(1−ε_new)·P_SCP + ε_new·P_shape`, activity-weighted; the normaliser with the filler and the noise hypothesis | 1–2 wk + data |
| P4.2 (C5) | an evidence ledger per channel and call across disjoint windows, tempered (τ fitted on long streams); retire `Confirmed::reads`, the wide-margin shortcut and the "heard twice" bonus | 1 wk |
| P4.3 (C6) | context (DE, CQ, 5NN, TEST) as likelihood ratios; the `␁CALL␂` token carries log-odds; the extractor's fused-run and despace fixes become candidate generators | 2 wk |
| P4.4 (C7) | calibration map (Platt, then isotonic or boosted trees) on bench and off-air labels; spot when P > k/(k+1); calibration checked separately for SCP and non-SCP calls | 1–2 wk + labels |

**Pass:** cost (misses + k·busts) below P3.3's retuned logic on the bench
and off air, at no loss of recall on SCP calls beyond what the cost ratio
accepts; reliability of the spot probability near the diagonal.

---

## 9. Phase 5: refinements

| Step | What | Gate | Effort |
|---|---|---|---|
| P5.1 (K4) | smoother at commit; posterior-weighted durations for the clock and the fist (one EM step per commit) | after P3.1 | 1–2 wk |
| P5.2 (K5) | coherence-limited coherent blocks from B and the tracked phase; about +1 dB on dits, up to +2.5 dB on long elements in quiet and moderate fading | after P3.1 | 1–2 wk |
| P5.3 | the lattice's forward key-down probability as the tracker's association weight (prediction before update in both directions) | after P3.4 | 1–2 wk |
| P5.4 | the language model inside the forward pass (Morse-tree node + context in the state, beamed) | if P3.5 gains | 2–4 wk |
| P5.5 (K6) | per-survivor gain beliefs in the lattice | only if a gap to the P1.2 gain oracle remains | 4+ wk |
| P5.6 | the hand constants (α, fist clamps, ε, τ, λ) fitted by gradient through the forward algorithm on the simulator | after Phase 4 | 2–3 wk |

---

## 10. Decision points

| Point | When | Question | If yes | If no |
|---|---|---|---|---|
| D0 | end of Phase 0 | does each quick win hold up off air? | into the `v4` preset | stays off, noted in ALGORITHM.md |
| D1 | P1.2 / P1.3 | which oracle bounds exceed about 1 dB (clock, fist, channel, gain)? | the matching step is built (P3.4, P3.1's bank, P5.5) | that dimension is dropped |
| D1b | P1.4 | does β change the cost much, i.e. are many spots snaps against the audio? | Phase 4 has a large payoff; keep it in the plan | Phase 4 can move after P5 |
| D2 | P2.1 | does the tracker meet K2's pass criteria? | into `v4`; start Phase 3 and the side line's tuning | back to K1 (model order, association); P0.1 stays the interim |
| D3 | P3.3 | did the SCP corrections fire much less on tracked evidence? | they were compensating; retuned margins ship until Phase 4 | margins stay as they are |
| D4 | P3.4 | does the bank reach most of the oracle-clock bound without false characters on noise? | it replaces bootstrap, re-lock and watchdogs | keep the decision-directed clock, use the bank only in the alignments |
| D5 | P3.5 | does language-model rescoring gain at the cliff without inventing text on noise? | P5.4 later | the language model stays as the filler's prior only |

---

## 11. Effort and calendar

Focused work for one developer. Bench sweeps, off-air A/B and retuning
dominate; labels take calendar time.

| Phase | Steps | Effort |
|---|---|---|
| 0 | P0.1–P0.6 | 1–2 wk |
| 1 | P1.1–P1.6 | 3–5 wk |
| 2 main | P2.1 | 4–6 wk |
| 2 side | P2.2–P2.4 | 5–7 wk |
| 3 | P3.1–P3.5 | 10–16 wk |
| 4 | P4.1–P4.4 | 5–7 wk |
| 5 | P5.1–P5.6, as gated | 4–10 wk |
| | **Total effort** | **about 32–53 weeks** |

**Calendar.** The side line overlaps the main line's bench and A/B waiting,
so the calendar is shorter than the sum: M2 (the tracker) after about
3 months, M3 after about 6, M4 after about 8, Phase 5 after that as gated.
With two people, one on each line, about 5–6 months to M4.

**CPU, to check on a full contest band at each milestone:** the flat
passband 1.5× the polyphase work; the tracker about 40 Mflop/s for 1536
channels; the hypothesis bank a few million operations per second per
channel in acquisition, run only on channels with a detected carrier; the
forward passes 2–4× the alignment DPs.

---

## 12. Risks

| Risk | Where | Guard |
|---|---|---|
| the tracker decodes worse than v2 on strong live air | P2.1 | old views selectable; strong-station A/B first; keep v2's view for Viterbi and the tracker's for the posterior if needed |
| AFC error, chirp or QRM read as fading | P2.1, P3.1 | FADING_TRACKER.md §5 risks; the innovation's phase drift; prefer the faster-fading assumption |
| more hypotheses and a language model invent text | P3.4, P3.5 | the `noise` case and beds with everything on; P(θ) and λ fitted, not hand-set |
| detections fall when snapping is reduced | P3.3, Phase 4 | judged by cost, not count (§1, rule 6) |
| one observation counted twice between tracker and lattice | P5.3 | prediction before update in both directions |
| correlated repeats overconfident | P4.2 | tempering; calibration checked against the number of copies |
| label bias | P1.3, P1.4, P4.4 | bench labels for busts, RBN for precision, logs for contests; check they agree |
| a stale MASTER.SCP | P4.1 | activity weights from recent spots; ε_new measured per contest |
| side-line work tuned on old evidence | Phase 2 | side line ships only byte-identical until D2 |

---

## 13. Not in the plan

- **Several channelizers at different bandwidths.** The channelizer is the
  expensive shared stage, and the bandwidth the decoder uses is set after
  it. Channel hypotheses are scored on the same IQ by the tracker bank
  (P3.1) instead.
- **Choosing a decoder by its text alone.** The language model enters as a
  prior multiplied with the acoustic likelihood (BAYESIAN_DECODING.md
  §1.13), never as a judge on its own.
- **A fixed whole-element coherent sum.** Without the measured Doppler
  spread it loses up to 6 dB in fast fading and 3 dB to a 2 Hz AFC error
  (ALGORITHM.md §5.3.4); P5.2 does it with B.
- **Speed-matched linear filters.** Measured: under 0.3–0.6 dB left
  (§5.3.6).
- **A learned denoiser in front of the decoder.** It can delete a weak
  signal or invent one (ALGORITHM.md §5.8). Networks enter only at the
  seams, as calibrated probabilities, after Phase 4.
