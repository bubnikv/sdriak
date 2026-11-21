# Fading tracker: a plan

A plan for tracking the HF fading channel in the CW decoder, so that faded
marks stay weak evidence instead of reading as spaces. Section and table
numbers refer to `ALGORITHM.md`. The measurements come from
`experiments/cw_bench` (E0, E3). K2 is built behind `SKIM_V3_TRACK`
(§8); §7 records what a reference built on Bell's decoder measured, and
the decisions it supports. [PLAN.md](PLAN.md) schedules these steps (P1.1,
P2.1, P3.1, P5).

## Why

At 24 WPM in fading, v3 needs about 9 dB more SNR than CW Skimmer's
published rates at 0.1 Hz Doppler, and over 10 dB more at 1 Hz. It keeps a
15 % CER floor even at +20 dB (E0). The comparison is not like for like
(short cold overs, a synthetic fist), but the floor points to the cause:
the decoder's idea of the signal level cannot follow a fade.

- v3's weak view estimates the mark power σ1² as the mean under a peak held
  with a 2 s release.
- v2's view tracks µ_mark with a 0.35 s release.
- Neither predicts the level through a fade. The weak/strong view switch
  (E3) only chooses between them.
- Making σ1² release faster helps in fading but breaks clean copy (0 dB
  CER 0.02 → 0.09). One fixed time constant cannot serve every channel.

## 1. The channel, seen from one 125 Hz channel

ITU-R F.1487 describes HF as a tapped delay line: two or more paths, each a
complex Gaussian gain with a Gaussian Doppler spectrum (Watterson). Inside
one CW channel most of that structure disappears.

- **Delay spread does not matter.** Paths 0.5–7 ms apart smear each key
  edge by at most two samples at 250 Hz. The unbiased dit,
  (dit + element space)/2, already cancels that bias. The paths collapse
  into one complex gain:

  ```
  z_t = k_t · h_t + n_t        k_t: keying 0/1 (the lattice)
                               h_t: the channel's complex gain
                               n_t: noise, CN(0, σ²)
  ```

- **Only the gain's Doppler spectrum matters.** Two independent paths with
  the same spread and shift sum to one Gaussian process with the same
  spectrum. ITU good, moderate and poor (two equal paths at 0.1 / 0.5 /
  1 Hz) are therefore exactly one fading process to this decoder. A second
  spectral component appears only when paths differ in Doppler shift. Then
  they beat, with notches every 1/Δν seconds: magneto-ionic O/X splitting,
  or the multi-path presets of channel simulators.
- **Past a few hertz, tracking stops paying.** At 10–30 Hz (ITU low- and
  high-latitude) the coherence time is under a dit. The gain cannot be
  predicted from mark to mark, and the right evidence is the energy
  detector. A correctly specified tracker reaches that form by itself: its
  uncertainty V grows to the full signal power P.

**The model:** one complex fading process with a Gaussian Doppler spectrum,
approximated as AR(2), plus a carrier rotation. A two-component model, for
beating, is an upgrade to make only where measurement shows the need.

- AR(1) has a Lorentzian spectrum: too cautious at short lags and too
  trusting at long ones.
- AR(2) fits the Gaussian correlation much better, and higher orders are
  numerically fragile (Baddour & Beaulieu 2005).
- Per-path filters for three ITU paths are not identifiable from a keyed
  signal at about 45 % duty. Carried through the lattice they would cost on
  the order of 1 MB per channel.

## 2. What is tracked

Two layers: a fast state updated every observation, and slow parameters
re-estimated every few seconds from committed marks.

| Quantity | Layer | Estimated by | Today |
|---|---|---|---|
| complex gain `h` (amplitude and phase) | fast state | Kalman filter | held peak σ1² (2 s), µ_mark (0.35 s); amplitude only |
| its rate of change (the AR(2) second state) | fast state | Kalman filter | — |
| residual carrier offset δf | fast state or slow parameter | slow phase drift of the innovation, fed back to the AFC | AFC: 1 s EMA of a spectral peak |
| noise power σ² | slow | band floor and channel percentile | `CwPowerStats` |
| mean signal power P | slow | long average of \|z\|² − σ² over committed marks | the held peak |
| Doppler spread B | slow | fit of −ln\|ρ(kT)\| = 2π²σ_f²(kT)² to lag products of committed key-down samples, k = 1 … 128, where 0.2 ≤ \|ρ\| ≤ 0.95 (§5.4 W1) | — |
| beating indicator | slow | \|ρ\| that dips and recovers (not monotone) | — |
| keying `k_t` | not in the filter | the lattice | the lattice |

- **B, P and σ² set the filter's coefficients:** the AR(2) poles from B,
  the process noise from P, and the measurement noise from σ².
- **They change slowly.** Watterson measured stationarity over stretches of
  about ten minutes. Keep them per channel, and per station across overs,
  and do not forget them after 8 s of quiet as the fist is.
- **When in doubt, assume faster fading.** An overconfident spread costs
  more than a cautious one.

## 3. Feeding the filter: the hard part

The filter must know when the key is down. The lattice knows that only 4–8
dits later, at commit. At 1 Hz spread, 8 dits at 25 WPM (0.38 s) is a whole
half-correlation time, so a filter updated only from committed marks would
arrive too late. Three ways, in increasing cost:

1. **Soft data association, outside the lattice** (the first step). Update
   on every observation, weighted by the probability that the key is down.
   That probability comes from the observation's likelihood ratio and the
   keying duty. It is a two-hypothesis Gaussian mixture collapsed to one
   Gaussian by moment matching, the probabilistic data association idea
   (Bar-Shalom & Tse 1975). It is deterministic, a few dozen operations per
   observation, and independent of the lattice.
2. **A smoother at commit.** Rerun the committed stretch backwards with hard
   keying. That corrects the state, and feeds the slow parameter estimates
   with clean data.
3. **Per-survivor beliefs in the lattice** (§5.4 W4; Raheli, Polydoros &
   Tzou 1995). Each lattice node keeps the gain belief (μ, V) of its own
   best predecessor path. It is exact for the max-product search, and costs
   about 100 KB per active channel. Build it only if the bench shows the
   soft version leaves a gap.

**A better key-down probability, later.** Way 1 weighs each observation
by the probability that the key is down, from the observation's own
likelihood ratio and the average keying duty. Once the lattice has a
forward (sum-product) pass (BAYESIAN_DECODING.md, step C2), it supplies a
sharper prior: P(key down at t | everything before t), which knows that the
middle of a dah is almost surely key-down and the middle of a word space
almost surely not. Use the prediction before observation t in both
directions: the tracker combines the lattice's probability *before* t with
its own likelihood of observation t, and the lattice reads the LLR
computed from the tracker's prediction *before* its update. Otherwise
observation t is counted twice and both become overconfident.

**Observations.** Use v3's coherent ½-dit block sums (`CwMatchedFilter`),
not raw samples. A block stays inside the coherence time up to a few hertz
of spread (§5.3.4, measured), and costs less than raw samples.

## 4. What the decoder gets

- **One calibrated evidence for all SNR.** For each observation, the log
  likelihood ratio of key down against key up, given the predicted gain:

  ```
  llr = ln CN(z; μ, σ² + V) − ln CN(z; 0, σ²)
  ```

  With a well-known gain (small V) it is a matched filter against the
  prediction. With an unknown gain (V ≈ P) it is today's Rayleigh-faded
  energy statistic. It replaces v2's heuristic view, v3's calibrated view,
  and the switch between them, so the regression E3 works around cannot
  recur.
- **Two changes go in with it** (BAYESIAN_DECODING.md §1.5, §1.8, step B1):
  - the lattice scores a space as −Σllr, which doubles every comparison
    between paths; with a calibrated LLR a space should score 0, and the
    balance between the evidence and the duration priors is retuned once,
    for both changes;
  - the innovation gate protects the tracker's state from impulses, but the
    LLR the lattice reads needs its own outlier (ε-contamination) term, so
    that one impulse or an unpredicted notch costs at most a few nats.
- **A faded mark stays a mark.** Once a fade has been tracked, the
  prediction falls with it, and the next weak mark is scored against a low
  predicted level instead of a held peak.
- **Side benefits:**
  - an SNR report from the actual gain;
  - a real "signal still present" test for dead air, in place of the 45 Hz
    envelope ratio behind the pause problem (E1);
  - a coherent reference for coherence-limited blocks (§5.3.4);
  - the measured Doppler spread that sets those blocks' length.

## 5. Steps

Each step runs behind a `CwDecoderConfig` switch and is measured on the
bench. The gates stay byte-identical with the switch off.

| Step | Work | Pass criterion |
|---|---|---|
| K0 | Bench: output the true gain and keying; add a long continuous-stream mode (minutes of one station, many overs) for a fairer comparison with CW Skimmer's rates; add drift and chirp. (Two-path presets with Doppler shifts exist: `--fade paths:…`, §7) | — |
| K1 | Prototype the tracker and its parameter estimators against the true gain. The coherent Bell decoder (`bell:coh`, §7) already prototypes the filter itself in C++; K1 is left with the data-association weighting and the estimators of B and δf | tracker error against the true gain; B estimated within 30 % from 0.1 to 10 Hz |
| K2 | `CwGainTracker` in C++ next to `CwPowerStats`, supplying the evidence of §4 (one view when on): complex AR(1) gain, B fixed at 0.5 Hz, σ² from the band floor, P learned from marks, an ε-contaminated LLR (scoring only) | fading at +10 dB, 24 WPM: ITU moderate < 0.19 and ITU poor < 0.30 (v3 0.32 and 0.47; `bell:coh` with the {0.3, 1, 3} Hz bank 0.13 and 0.14); no-fading ladder within 0.2 dB; false characters on noise not up; gates pass |
| K3 | Slow parameters: B, δf, P, kept per channel and per station and re-acquired after a gap in the keying | flat 3 Hz at +10 dB < 0.25 (v3 0.57, `bell:coh` with the true spread 0.12); slow fading not worse than with B fixed |
| K4 | Smoother at commit, feeding the parameters and an amplitude-aware clock update | — |
| K5 | Coherence-limited coherent blocks with B and the tracked phase (§5.3.4) | about +1 dB on dits, as simulated |
| K6 | Per-survivor beliefs (§5.4 W4), only if K2–K5 leave a measurable gap | the gap to `bell:coh` on the fading grid, which is K6's ceiling on the bench (§7) |

**Cost.** An AR(2) complex Kalman filter per observation is on the order of
100 floating-point operations. For 1536 channels at 250 Hz that is about
40 Mflop/s, small next to the channelizer. Memory is a few hundred bytes per
channel until K6.

**Risks to watch on the bench.**

- AFC error and chirp turn into a fast rotation the model reads as fading;
  the innovation's phase drift should catch it.
- QRM inside the channel can be taken for the station's own fading.
- Impulses: reject or down-weight observations with an implausible
  innovation (§5.7). §7 shows the limit of doing this inside one channel.
- Overconfident spread estimates. Prefer the faster assumption.

## 6. Prior work

**Morse decoding.**

- **E. L. Bell**, *Optimal Bayesian estimation of the state of a
  probabilistically mapped memory-conditional Markov process with
  application to manual Morse decoding*, Naval Postgraduate School, Doctor
  of Engineering, 1977. The decoder is a growing set of linear Kalman
  filters on an evolving trellis: a filter per path, the exact form
  per-survivor processing approximates. It was tested in simulation on
  random-letter texts, and the DTIC copy (ADA046503) includes the Fortran.
  A 1975 NPS thesis by Bell applies a Kalman filter and smoother to Morse
  as a random telegraph wave.
- **AG1LE, `morse-wip`** (github.com/ag1le/morse-wip, 2013–14): a C port
  of Bell's decoder (`kalfil.c` among others), integrated into fldigi as a
  "Bayes decoding" option. It stayed alpha-stage: testers reported missed
  word spaces and no AFC, and its noise estimator could not track the real
  noise. It is the only open source Kalman-based CW decoder found. Checked
  against the listing, it carries a sign error in the duration model that
  forbids elements longer than nominal and merges words. The corrected
  decoder is the bench backend "bell" (BELL.md).
- **VE3NEA/DeepCW:** the `RayleighFading` generator, Gaussian Doppler
  spectrum validated against Watterson. A channel model only.
- **Coherent CW:** R. Petit, QST September 1975; C. Woodson, QST May and
  June 1981. Clock-synchronised keying and stable oscillators allowed about
  9 Hz matched filters. It shows what coherent CW detection needs, and it
  evolved into BPSK.

**Fading-channel tracking.**

| Plan element | Reference |
|---|---|
| AR fading + Kalman tracking | Zhou, Kim & Giannakis (1999), AR fading tracked by a Kalman filter with pilot symbols; Tsatsanis & Xu, AR channel models with Doppler read from the AR spectrum and taps tracked by a Kalman filter |
| AR model order | K. E. Baddour and N. C. Beaulieu, "Autoregressive modeling for fading channel simulation", IEEE Trans. Wireless Comm. 4(4), 2005 |
| per-survivor beliefs (K6) | R. Raheli, A. Polydoros, C.-K. Tzou, "Per-Survivor Processing: a general approach to MLSE in uncertain environments", IEEE Trans. Comm. 43, 1995 |
| soft data association (§3) | Y. Bar-Shalom and E. Tse, "Tracking in a cluttered environment with probabilistic data association", Automatica 11, 1975 |
| switching-model filters | G. A. Ackerson and K. S. Fu, "On state estimation in switching environments", IEEE Trans. Automatic Control 15(1), 1970 (its tie to the GPB name is not confirmed) |
| Doppler spread (K3) | C. Tepedelenlioglu and G. B. Giannakis, IEEE Trans. Veh. Tech. 50(4), 2001; Tepedelenlioglu, Abdi, Giannakis and Kaveh, survey, Wireless Comm. and Mobile Computing, 2001 |
| learned gain correction (later, §5.8) | G. Revach, N. Shlezinger et al., "KalmanNet", IEEE Trans. Signal Processing 70, 2022; PyTorch code at github.com/KalmanNet |

**Code.**

- **HF channel simulators:**
  - Codec2's `ch.c` (drowe67/codec2) has `--mpg`, `--mpp` and `--fast`,
    from fading files generated in Octave, and is used to test FreeDV.
  - GNU Radio's `selective_fading_model` is a sum-of-sinusoids
    Rayleigh/Rician tap model, not Watterson's Gaussian spectrum as far as
    found.

  `cw_bench` covers the same ground, with a delay and a shift per path.
- **FreeDV** tracks the channel on pilot symbols. Its 700D OFDM mode
  degrades on fading faster than 1 Hz.
- **HF serial-tone modems** (MIL-STD-188-110, STANAG 4285) are the
  military and commercial state of the art: known probe symbols, and a
  decision-feedback equalizer usually adapted by RLS.
  - Open references: MathWorks' MIL-STD-188-110A receiver example, the
    MATLAB File Exchange "A Synchronized Mil-Std-188-110B Receiver", and
    `kg7hf/modem-core` (C++; its tracking not confirmed).
  - Check licences before reuse.
- **CW Skimmer** and **Rocky** (VE3NEA) are closed source.

**What transfers.**

- Bell's thesis is the template: Kalman amplitude tracking on a keying
  trellis. Its growing filter bank is the cost problem per-survivor
  processing bounds.
- Modems track the channel on known pilots, and CW has none: key-down
  periods are known only once the lattice decides. Hence soft data
  association first, rather than decision-directed tracking.
- AG1LE's experience says the filter is only as good as its noise and
  signal-power inputs, which is why σ², P and B are first-class estimates
  here.

## 7. What the Bell reference measured

BELL.md has the details; all figures are payload CER on `cw_engine_bench`
(24–25 WPM, 20–50 overs a point, tone 10 Hz off the channel centre).
`bell:coh` keeps Bell's trellis and replaces his envelope filter with a
complex Kalman filter on the channel gain, one per survivor path: the
exact form of K6, so it shows what this plan can reach on the bench.

**The tracker is worth building.**

| | v3 | `bell:coh` | |
|---|---|---|---|
| white noise, SNR500 for 10 % CER at 12 / 18 / 25 / 35 WPM | −1.1 / −1.0 / −0.6 / +0.2 dB | −4.5 / −3.0 / −1.4 / +0.7 dB | coherence: 1–3.5 dB at 12–25 WPM |
| ITU moderate, 10 dB | 0.316 | 0.129 | spread bank {0.3, 1, 3} Hz |
| ITU poor, 10 dB | 0.470 | 0.138 (0.093 with the true spread) | |
| flat 3 Hz, 10 dB | 0.569 | 0.119 | |
| ITU poor, 20 dB | 0.132 | 0.033 | v3's fading floor gone |
| beating paths ±1 Hz, 10 dB | 0.539 | 0.140 | the bank's fast classes absorb the beat |

**What it says about the design.**

- **AR(1) is enough to start.** An AR(1) gain whose correlation matches
  the Gaussian Doppler spectrum at 40 ms carried every result above; AR(2)
  (§1) is not needed for K2.
- **The spread must be estimated, and shared.** A fixed 0.5 Hz is best on
  slow fading and fails at 3 Hz (0.63); letting every survivor pick its
  own spread class from a bank gets the fast cases back but costs
  0.02–0.05 on slow fading, because a wrong hypothesis can use a fast
  class to explain noise. One spread per station, measured over seconds of
  marks (K3), is the design. It should measure how fast the gain actually
  decorrelates: with beating paths that is faster than either path's
  spread, and a fast estimate covers the beat without a second model
  component.
- **A floor under the spread.** With a small spread the model cannot
  absorb the AFC's residual frequency error: `bell:coh` with the true
  spread of ITU good (0.1 Hz) loses to a fixed 0.5 Hz (0.079 against
  0.047 at 10 dB). Until δf is tracked (K3), keep B at 0.3–0.5 Hz or more.
- **σ² from the band floor, P learned.** With the noise from
  `set_floor()` and the signal power averaged over marks the likelihoods
  are calibrated: `bell:coh` decodes 80–150 characters an hour from noise
  where Bell's envelope filter makes 400 (v3: 3, with its squelch).
- **The tone must sit still.** An AR(1) gain cannot follow a steady
  rotation; `bell:coh` needed an AFC (a 4 s periodogram, followed every
  0.5 s) that sees each sample 2 s before the decoder. v3's own AFC feeds
  K2; its residual is what δf in K3 is for.
- **Impulses: score them, repair them before the channelizer, but do not
  let one channel's filter decide.** A heavy-tailed noise term in the LLR
  (§4's ε-contamination; ε = 0.05, κ = 30) halves the impulse damage. Using
  the same posterior to hold back the state update removes almost all of
  it but breaks fast fading (ITU poor at 10 dB 0.20 → 0.40): one channel
  cannot tell an impulse from a quick fade. So K2 uses the term in the LLR
  only, and impulses are repaired before the channelizer
  (`ImpulseRepair`, ALGORITHM.md §5.7), where they are short and land in
  every channel at once. With that stage v3 alone goes from 0.755 to 0.030
  on sferics and 0.995 to 0.028 on clipped strong pulses;
  `set_repair_weights()` can tell the tracker which samples were rebuilt.
- **Samples.** `bell:coh` runs on every second channel sample (125 Hz,
  noise nearly white). At 35 WPM a dit is four such samples and it falls
  0.5 dB behind v3; faster sampling with the noise counted as correlated
  did worse. v3's ½-dit blocks (§3) avoid the question.

**Revised order of work.**

1. K2 with B fixed at 0.5 Hz, σ² from the floor, the ε-term in the LLR
   only, AR(1). Measure on the fading grid against v3 and `bell:coh`.
2. K3's spread estimate, shared per station and re-acquired after a gap
   in the keying, with a 0.3 Hz floor until δf is tracked.
3. `ImpulseRepair` in the skimmer's front end once a pipeline exists, and
   the repair weights into the tracker's LLR.
4. K6 only if K2–K3 stay clearly behind `bell:coh` on the fading grid:
   soft data association (K2) against per-survivor beliefs is the one
   question the Bell reference cannot answer, because it is per-survivor
   throughout.

## 8. K2: built

`CwGainTracker` (`src/engine/cw_gain_tracker.h`), on with
`SKIM_V3_TRACK=<B>[,<eps>,<kappa>]` (default off, v3 byte-identical):

- a complex AR(1) gain on the matched filter's coherent sum u, correlation
  matched to a Gaussian spectrum of spread B at 40 ms;
- σ² and the key-down power from `CwPowerStats`; the gain's uncertainty is
  kept relative to that power, so it follows the power while the power is
  learned at the start of an over;
- updated once per sum length (where the sum's noise is independent) by
  soft data association, the key-down probability from the observation's
  own likelihood ratio and a 0.45 duty;
- the evidence of §4 with the impulse term ε = 0.05, κ = 30 in the score
  only; 1/L weighted and clamped as v3's weak view;
- one view: the weak view's machinery throughout, on the tracker's
  evidence; the pre-roll keeps the tracker's llr beside the matched
  amplitude, so a replay reads what the live path read.

**B = 1 Hz.** At 0.5 Hz the tracker is too sure of its phase on strong
signals: v3's AFC leaves a residual rotation it cannot follow (clean copy at
14 dB: 2.3 % CER, over the gate's 2 %; 1 Hz: 0.4 %, 2 Hz: 0). 1 Hz is the
best fixed spread on the grid; δf tracking (K3) is what would let it come
down.

Payload CER at 24 WPM, 50 overs a point, v3 → K2 (B = 1 Hz):

| | 0 dB | 3 dB | 6 dB | 10 dB | 15 dB | 20 dB |
|---|---|---|---|---|---|---|
| no fading | 0.051 → 0.041 | 0.003 → 0.008 | 0.002 → 0.002 | 0.002 → 0.000 | 0.007 → 0.001 | 0.014 → 0.001 |
| ITU good | 0.447 → 0.396 | 0.277 → 0.224 | 0.180 → 0.112 | 0.117 → 0.049 | 0.082 → 0.023 | 0.044 → 0.015 |
| ITU moderate | 0.543 → 0.446 | 0.443 → 0.278 | 0.402 → 0.175 | 0.316 → 0.080 | 0.202 → 0.033 | 0.080 → 0.014 |
| ITU poor | 0.625 → 0.537 | 0.582 → 0.397 | 0.561 → 0.272 | 0.470 → 0.144 | 0.252 → 0.052 | 0.132 → 0.017 |
| flat 0.1 Hz | 0.478 → 0.465 | 0.343 → 0.286 | 0.215 → 0.159 | 0.143 → 0.077 | 0.081 → 0.021 | 0.042 → 0.010 |
| flat 1 Hz | 0.627 → 0.537 | 0.591 → 0.396 | 0.563 → 0.270 | 0.463 → 0.145 | 0.243 → 0.066 | 0.143 → 0.027 |
| flat 3 Hz | 0.675 → 0.669 | 0.667 → 0.621 | 0.665 → 0.470 | 0.569 → 0.220 | 0.301 → 0.067 | 0.187 → 0.018 |

- **K2's criteria pass:** ITU moderate 0.080 (< 0.19) and poor 0.144
  (< 0.30) at 10 dB; v3's fading floor at 20 dB is gone. Flat 3 Hz at
  10 dB (0.220) already meets K3's bar (< 0.25) with the spread fixed.
- **Against `bell:coh`** (the per-survivor ceiling, §7): ITU moderate
  0.080 against 0.129 and poor 0.144 against 0.138 at 10 dB, flat 3 Hz
  0.220 against 0.119. On fading at 10 dB soft data association loses
  nothing on slow and moderate fading; the gap is on fast fading, where
  the spread should be measured (K3).
- **No coherence gain on white noise.** Near threshold K2 is within
  0.07 dB of v3, where `bell:coh` gains 1–3.5 dB (12–25 WPM). There the
  key-down weights are uncertain, the gain estimate is diluted by noise,
  V never gets small and the score stays the energy detector; `bell:coh`
  updates each survivor with hard keying. Recovering that gain is the
  case for K4 (the smoother at commit) and K6 (per-survivor beliefs).
- **No fading:** the 10 % CER threshold within 0.07 dB at 12–25 WPM and
  0.35 dB better at 35 WPM (v3 −1.14 / −1.02 / −0.61 / +0.21 dB, K2
  −1.07 / −1.03 / −0.65 / −0.14 dB, 20 overs a step).
- **At 3 % CER the tracker is behind on white noise:** v3 −0.48 / −0.03 /
  +1.03 dB at 12 / 25 / 35 WPM, K2 −0.27 / +0.60 / +1.87 dB (20 overs a
  step). It reaches 10 % as early as v3 but keeps a residue of errors
  for another dB or so.
- **Noise:** no false characters in an hour (v3: 1).
- **Gate** (`--gate quick`): passes; clean copy at 14 dB 0.8 % (v3 0.0 %).
- **Impulses** (25 WPM, 20 overs, 3 / 6 / 10 dB; the impulse grid of
  ALGORITHM.md §5.7):
  - with the repair before the channelizer, K2 is as good as v3 or better:
    sferics 0.033 / 0.030 / 0.021 → 0.009 / 0.006 / 0.019, clipped pulses
    0.051 / 0.028 / 0.036 → 0.009 / 0.012 / 0.013; no false characters on
    noise. The exception is 100/s periodic pulses at 3 dB (0.060 → 0.103);
  - without the repair, sparse strong impulses do better (sferics at 6 dB
    0.755 → 0.376, crashes 0.229 → 0.115) but dense trains break the
    tracker: ignition at 50/s, 10 dB, 0.017 → 0.918, and on noise with
    impulses it decodes 200–300 false characters an hour where v3 makes
    none. The ε term helps sparse impulses (sferics at 6 dB 0.505 without
    it) and hurts dense ones (ignition at 10 dB 0.619 without it): ε = 0.05
    assumes impulses are rare;
  - why dense trains break it: through the channel filter a 50/s train
    lifts every space a little, and σ² (the low percentile of
    `CwPowerStats`) stays at the quiet floor, so 69 % of samples score as
    key-down where v3 scores 43 %. v3 survives because its noise term never
    falls under a ninth of the mark power; the tracker cannot take that
    floor, which declares every deep fade noise (ITU poor at 20 dB
    0.017 → 0.47);
  - three remedies were measured and none holds both. The power of the
    spaces, averaged with key-up weight, fixes ignition (10 dB 0.023) but
    runs away in fading, where faded marks count as spaces (flat 3 Hz 0.65
    at every SNR); capped at a few times the percentile it trades one for
    the other. An impulse term that learns its share and power
    (`SKIM_V3_TRACK=1,0.05,30,1`) does not move towards a dense weak
    component; it helps sparse impulses a little (sferics without the
    repair at 10 dB 0.283 → 0.204) and learns a strong station's onsets as
    impulses (the gate's clean copy 0.8 → 2.1 %), so it is off. A floor
    under the mark hypothesis (|µ|² + V ≥ ½P) gains little and costs on
    fading;
  - power cannot tell a pulse train between marks from a faded mark; the
    keying can. σ² should come from the spaces the lattice has committed,
    a few dits late: K4's smoother feeding the slow parameters. Until then
    K2 needs the repair in front of it.
- **Crowded band** (8 kHz, 6 dB, with the repair): K2 is better on a clean
  band and under sferics (60 stations: 0.093 → 0.068) but not under
  ignition, where the repair leaves more behind as the band fills: 30
  stations 0.035 → 0.108, 60 stations 0.161 → 0.669. At a skimmer's width
  that does not happen: at 48 kHz (10 overs a point) with 30, 100 and 200
  stations, K2 reads 0.000–0.006 under no impulses, sferics and ignition,
  v3 0.002–0.014.
- **The spread against the SNR.** The 3 % gap comes from B = 1 Hz: at
  0.5 Hz K2 matches v3 at 3 % on white noise. `SKIM_V3_TRACK_NARROW=0.5`
  narrows the spread on weak stations (0.5 Hz under 3 dB SNR500, B over
  10 dB): white noise at 3 % −0.64 / −0.18 / +0.21 / +0.92 dB at
  12 / 18 / 25 / 35 WPM, but a weak fading station needs the wide spread
  too (ITU poor at 3 dB 0.376 → 0.449). SNR cannot tell a steady weak
  station from a fading one; K3's spread estimate can. Off by default.
- **Offsets, drift, chirp, a second station** (25 WPM, 20 overs, CER at
  6 / 14 dB, v3 → K2):
  - tone offset 0–55 Hz: both ≤ 0.03; at the channel edge (62.5 Hz) v3
    0.041 / 0.029, K2 0.000 / 0.003 (`bell:coh` fails there, 0.70);
  - drift up to 0.5 Hz/s: all ≤ 0.013;
  - chirp, each key-down starting off frequency: 10 Hz over 20 ms
    unchanged; 30 Hz over 20 ms 0.020 → 0.043 and 50 Hz over 10 ms
    0.012 → 0.043 at 6 dB, nothing at 14 dB: the phase the tracker holds
    is wrong for the first few ms of each mark;
  - a second keyed station in the channel, 30 Hz from the target at 10 dB:
    at −6 dB v3 0.157, K2 0.189; at the same level 0.561 and 0.842; at
    +6 dB both lose the target (0.87 / 0.76). At 60 Hz (mostly the next
    channel) 0.002 for both. One gain cannot follow two carriers; neither
    decoder separates them.
- **Strong stations.** The score's noise is at least the predicted mark
  power |µ|² + V over 1000 (`SKIM_V3_TRACK_RATIO`, 30 dB). Without it the
  score, an energy detector wherever the gain is uncertain, crosses zero
  at a tiny fraction of a strong mark's amplitude, on the matched filter's
  ramps: marks lengthen, dits fuse, and a clean over at 30 dB read 0.340,
  at 40–60 dB 0.83–0.89 (25 WPM, 10 overs; v3 0.018–0.020, the same
  problem CwPowerStats' kMaxMarkToNoise solves). With it: 0.012–0.018
  from 30 to 60 dB, and 0.002–0.010 under 1 Hz fading (DeepCW v1 0.014,
  v2 narrow 0.044). Tied to the prediction, not to the held mark power,
  the floor falls in a tracked fade: at 1000 the fading grid at 10–20 dB
  does not move; at 9 (v3's ratio, but on the prediction) flat 3 Hz at
  20 dB reads 0.64, at 100 0.12. A floor on V instead (V ≥ 0.01–0.1 P)
  does not help: the cause is the zero crossing, not the phase.
- **Input level** (25 WPM, 10 overs): the absolute scale (±40 dB) leaves
  K2's output unchanged; level steps of the target or the noise mid-over
  cost ≤ 0.012. A receiver AGC pumped by a 60 dB station elsewhere in the
  band costs it 0.213 / 0.107 at 10 / 20 dB with a 200 ms decay (v3
  0.350 / 0.189, DeepCW v2 narrow 0.082 / 0.058), 0.080 / 0.042 with 1 s:
  the noise and mark powers follow the pumped level too slowly. PLAN.md
  §0, S0.
- **Against DeepCW**, all five on the same overs (30 a point for fading,
  20 for the rest; DeepCW with `--tail 4`, as it commits late). Payload CER
  at 24 WPM, 10 / 20 dB:

  | | v3 | K2 | `bell:coh` bank | DeepCW v1 | DeepCW v2 narrow |
  |---|---|---|---|---|---|
  | ITU good | 0.121 / 0.036 | 0.054 / 0.013 | 0.075 / 0.038 | 0.033 / 0.005 | 0.032 / 0.011 |
  | ITU poor | 0.459 / 0.145 | 0.147 / 0.025 | 0.150 / 0.026 | 0.055 / 0.017 | 0.125 / 0.047 |
  | ITU low latitude (10 Hz) | 0.635 / 0.196 | 0.191 / 0.052 | 0.121 / 0.051 | 0.143 / 0.090 | 0.272 / 0.241 |
  | flat 3 Hz | 0.560 / 0.177 | 0.203 / 0.015 | 0.119 / 0.018 | 0.069 / 0.029 | 0.200 / 0.111 |
  | beat ±0.5 Hz | 0.621 / 0.421 | 0.442 / 0.119 | 0.258 / 0.055 | 0.112 / 0.094 | 0.397 / 0.258 |

  - White noise, SNR500 for 10 % CER at 12 / 25 / 35 WPM: v3 −1.1 / −0.6 /
    +0.2, K2 −1.1 / −0.7 / −0.1, DeepCW v1 −5.7 / −3.4 / −1.8, v2 narrow
    −5.8 / −3.7 / −2.1 dB. DeepCW is 2–5 dB ahead.
  - Fading: DeepCW v1 leads at 0–10 dB on every condition but 10 Hz,
    where `bell:coh` leads; at 20 dB K2 beats v1 at 10 Hz and 3 Hz and
    trails it on ITU good, ITU poor and beating. Against v2 narrow, the
    model DeepCW ships: K2 is ahead at 20 dB except on ITU good (0.013
    against 0.011), and at 10 dB ahead at 10 Hz, level at 3 Hz, behind on
    ITU good, ITU poor and beating.
  - Impulses behind the repair: DeepCW 0.000–0.018, K2 0.001–0.025.
  - Offsets, drift, chirp: DeepCW unaffected, except v1 at the channel
    edge at 6 dB (0.895). A second station 30 Hz away: v2 narrow (3 bins)
    copes best (−6 dB 0.000, same level 0.202; K2 0.189 and 0.842).
  - 48 kHz crowded band, no false characters on noise: all but `bell:coh`
    at or near zero.
  - Cost per 47 s over on an i3-N300: v3 < 0.1 s, DeepCW v2 narrow
    ≈ 0.5 s, v1 ≈ 8.5 s.
- **K3, the spread measured on the station** (`CwSpreadEstimator`,
  `SKIM_V3_SPREAD=1` with the tracker; off by default). Inside each
  committed mark, away from the matched filter's ramps, the tracker's sums
  pair with the interior samples of the last 0.8 s of marks, at least one
  sum length apart; the pairs estimate the gain's autocorrelation by 40 ms
  lag bin, forgetting over 20 s of mark time. B is the fit of
  −ln|ρ| = π²B²τ²/2 over the lags where 0.2 ≤ |ρ| ≤ 0.95, the rotation δf
  the slope of arg ρ. It reads 0.2 Hz on a steady signal or 0.1 Hz fading,
  0.7–1.2 Hz under 1 Hz fading and ITU poor, and 0.8 Hz under 3 Hz: the
  committed marks favour the moments the gain is up. So the tracker takes
  1.3× the measured B, at least 0.3 Hz, predicts the measured rotation,
  and on a strong station keeps at least the configured 1 Hz (the spread
  follows the SNR from the measured value under 3 dB to that over 10 dB):
  a narrow spread on a strong signal cannot absorb the AFC's wandering
  residual, which a constant rotation does not model (strong signals at
  30–60 dB 0.04–0.05 without that rule, 0.012–0.018 with it).
  - White noise, SNR500 for 10 % / 3 % at 12 / 25 / 35 WPM, K2 → K2 + K3:
    −1.07 / −0.20 → −1.11 / −0.49, −0.78 / +0.52 → −0.82 / −0.02,
    −0.21 / +1.75 → −0.18 / +1.30: the 3 % thresholds 0.3–0.5 dB better,
    the 10 % ones unchanged.
  - Fading at 0 / 10 / 20 dB, strong signals, chirp, impulses behind the
    repair: level with K2 within the noise (beating at 10 dB 0.442 →
    0.435, 10 Hz 0.191 → 0.192). No false characters on noise; the gate
    passes.
  - K3's pass criteria (PLAN.md S2) in part: slow fading not worse, the
    3 % gap to v3 closed at 12–25 WPM; chirp (0.046 against v3's 0.020)
    and beating (0.435, target < 0.40) are not met.
- **K5, coherence-limited blocks, tried and left out.** A mark scored
  in blocks of the lag at which K3's spread decorrelates the gain to 0.9
  (0.146/B s: the whole element at 0.3 Hz, about a dit at 3 Hz), each
  block a known-amplitude, unknown-phase test, −n·A²/σ² + ln I0(2A·|S|/σ²),
  in place of the per-sample evidence. With A blended from the station's
  mark power near threshold to the tracker's prediction above 10 dB:
  white noise mixed (25 WPM at −1 dB 0.113 → 0.089, at −3 dB 0.620 →
  0.690), ITU poor at 20 dB 0.025 → 0.168. With the block evidence itself
  blended out above 10 dB: 20 dB back to 0.025, but weak fading worse
  (ITU poor at 3 dB 0.447 → 0.585, 10 dB 0.162 → 0.310: a fading station
  reads under its mean SNR, and the steady amplitude is wrong in a fade).
  On a strong signal a known-amplitude test is brittle (a modest mismatch
  in amplitude or phase within a block flips its sign); near threshold it
  needs an amplitude that is both reliable and follows a fade, which
  neither the mark power nor the diluted tracker estimate is, and an
  amplitude estimated per block makes false marks cheap.
- **K6 inside v3's lattice, tried and left out.** A gain belief per lattice
  boundary, each space boundary opening a filter that scores the samples
  after it as a mark against its path's gain, in place of K2's evidence
  for marks. Updated inside the hypothesised mark, the gain fits the noise:
  at 10 dB the Kalman gain is about 0.75, a "mark" over noise drags the
  prediction to the noise within a sample or two and the rest scores almost
  free (−3 for a false mark K2 charges −13). Scored against the prediction
  and updated only at the mark's end, false marks cost as they should, but
  a fading gain moves away from the prediction within a dah: ITU poor at
  20 dB 0.024 → 0.589, and white noise near threshold still worse than K2
  (25 WPM, 1 dB: 0.024 → 0.052). A ramp-aware prediction changed little.
  `bell:coh` gets its coherence gain from the same idea on per-sample
  observations at 125 Hz, where each update moves the gain far less than
  on v3's overlapping ½-dit sums; that is where a second attempt would
  start, after the edge-aware likelihood (PLAN.md S4) has changed the
  evidence it builds on.
- **Not measured yet:** live air. The unit gates (`cw_test`, `call_test`) run with the tracker off.
