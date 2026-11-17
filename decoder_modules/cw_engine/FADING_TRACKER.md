# Fading tracker: a plan

A plan for tracking the HF fading channel in the CW decoder, so that faded
marks stay weak evidence instead of reading as spaces. Section and table
numbers refer to `ALGORITHM.md`. The measurements come from
`experiments/cw_bench` (E0, E3). Nothing here is built yet.
[PLAN.md](PLAN.md) schedules these steps (P1.1, P2.1, P3.1, P5).

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
| K0 | Bench: output the true gain and keying; add a long continuous-stream mode (minutes of one station, many overs) for a fairer comparison with CW Skimmer's rates; add two-path presets with Doppler shifts (beating), and drift and chirp | — |
| K1 | Prototype in Python on bench dumps of the channel output and truth: AR(1) against AR(2), the block length, the data-association weighting, the parameter estimators | tracker error against the true gain; B estimated within 30 % from 0.1 to 10 Hz |
| K2 | `CwGainTracker` in C++ next to `CwPowerStats`, supplying the evidence of §4 (one view when on) | fading table at +10 dB clearly below v2 (ITU moderate < 0.19); no-fading ladder within 0.2 dB; false characters on noise not up; gates pass |
| K3 | Slow parameters: B, δf, P and the beating indicator, kept per channel and per station | B and δf followed on the beating and drift presets |
| K4 | Smoother at commit, feeding the parameters and an amplitude-aware clock update | — |
| K5 | Coherence-limited coherent blocks with B and the tracked phase (§5.3.4) | about +1 dB on dits, as simulated |
| K6 | Per-survivor beliefs (§5.4 W4), only if K2–K5 leave a measurable gap | — |

**Cost.** An AR(2) complex Kalman filter per observation is on the order of
100 floating-point operations. For 1536 channels at 250 Hz that is about
40 Mflop/s, small next to the channelizer. Memory is a few hundred bytes per
channel until K6.

**Risks to watch on the bench.**

- AFC error and chirp turn into a fast rotation the model reads as fading;
  the innovation's phase drift should catch it.
- QRM inside the channel can be taken for the station's own fading.
- Impulses: reject or down-weight observations with an implausible
  innovation (§5.7).
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
  noise. It is the only open source Kalman-based CW decoder found.
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
