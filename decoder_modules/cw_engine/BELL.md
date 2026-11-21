# Bell's Bayesian Morse decoder

E. L. Bell, *Optimal Bayesian estimation of the state of a probabilistically
mapped memory-conditional Markov process with application to manual Morse
decoding*, Doctor of Engineering thesis, Naval Postgraduate School, 1977.
DTIC ADA046503; the scan and its OCR text are on NPS's repository, Calhoun
(hdl 10945/18334). The Fortran listing is on thesis pages 159–190.

The decoder here, `src/engine/bell/bell_decoder.*`, is translated from
M. Niininen (AG1LE)'s C port, [ag1le/morse-wip](https://github.com/ag1le/morse-wip)
(commit 5eb011e, 2014, GPL-3.0-or-later), and checked routine by routine
against the listing. `src/engine/decode_bell.*` makes it a decode backend
("bell") for the bench. It is a reference to measure the planned Kalman
gain tracker against (FADING_TRACKER.md), not a candidate for the skimmer:
up to 750 Kalman updates per 5 ms per channel is far too much for 1536
channels. `KALMAN_EXPLAINER.html` (open it in a browser) explains the
tracking problem and where Bell's decoder fits.

## How it works

Every 5 ms the envelope detector gives one sample z.

- **State of a path.** A letter state λ (in the listing's 16-state model,
  the pair previous element / current element), the speed in WPM, and the
  time spent in the current element.
- **Extension.** Each saved path (at most 25) extends to 30 nodes: 6
  element types (dit, dah, element space, character space, word space,
  pause) × 5 speed steps (−2…+2).
- **Transition probability** (`TRPROB`, `XTRANS`, `PTRANS`, `SPDTR`).
  Staying in an element follows a duration density centred on 1, 3, 1, 3, 7
  or 14 dits. Leaving it follows Bell's second-order element table (Table
  XII) and a speed-step table.
- **Likelihood** (`LIKHD`, `KALFIL`, `MODEL`). Each node runs one scalar
  Kalman filter on the signal amplitude, z = key·a + v, and is scored by
  its innovation.
- **Pruning** (`SAVEP`). Keep the best node of each element type, then the
  best of the rest until 99 % of the mass or 25 nodes. Merge nodes with the
  same (λ, duration, speed).
- **Decision** (`TRELIS`, `TRANSL`). When the paths back from every saved
  node meet, the letter states up to that point are final. Without a
  meeting within 1 s, read the best path and prune the rest.

## The listing against morse-wip

Every routine of the 16-state model was compared with the scan.

| Routine | Listing page | morse-wip | Here |
|---|---|---|---|
| PROCES | 167–168 | 20 paths (`PATHS`); initial speeds 10–40 WPM | 25 paths, initial speeds 10–50 WPM (`DATA ILRATE/5*10,…,5*50/`) |
| TRPROB, PTRANS | 169–171 | same | same |
| XTRANS | 170 | **sign error**: `p1 = exp(-alpha*(b1-1)) * -.5f` (xtrans.cxx:78); the listing has `P1=0.5*EXP(-ALPHA*(B1-1.))` | the listing |
| SPDTR | 172 | speeds 5–80 WPM | 5–60 WPM; the listing's 10–60 in `bell:listing` (below 15 WPM the listing's floor merges characters, see "Measurements") |
| PATH, LIKHD | 173–175 | same | same |
| KALFIL, MODEL | 176–178 | same | same |
| PROBP | 178–179 | returns with P stale when every node scores 0 | restarts the paths and the trellis, keeping NOISE's estimate (the listing divides by 0). The letters still in the trellis are lost |
| SPROB | 179–180 | same | same |
| SAVEP | 180–183 | normalises with P(1) included | same as morse-wip. The listing starts the final sum at 0 and leaves P(1) out (line 40800), a slip in the listing |
| TRELIS | 184–186 | trellis zeroed, so the first 1 s indexes path 0, out of bounds; `ndel` read uninitialised | the listing's `DATA PTHTRL/5000*5/, LMDSAV/5000*5/` |
| TRANSL | 186–188 | its own: a code tree turns element pairs into characters. Z has no dah child, so `,` is unreadable; Q's dit child points at `0`; a code that runs off the tree goes on from the root, so the 8-dit error sign reads as `I`; when one trellis step settles several letters, each is written over the last in the same buffer | the tree, with Z and Q fixed, appending every letter; a code off the tree stays in a sink node and prints one `#` |
| NOISE | 190 (and the OCR of the page the scan lost) | same, then its output clamped to [0, 1] (bmorse.cxx:352) | the listing, then the same clamp, as part of the front end (below) |
| front end | 189–190: two resonators at 1 kHz (pole radii 0.97 and 0.9 at 8 kHz), rectifier, 100 Hz Chebyshev low-pass, every 40th sample | FFT filter, magnitude, decimation, an optional peak AGC | see below |
| state | | NOISE, TRELIS and the driver keep state in function statics, so two decoders share it | one object per channel |

**Not in the listing: the tree model.** Bell's "independent letter" model
uses 300 more letter states, read at run time from a file, `MORSEM`, that
the thesis does not print. Only the 16 element-pair states are in the DATA
statements. morse-wip and this decoder both run the 16-state model.

## The front end

Bell's NOISE estimates the noise from the minimum of the envelope and
subtracts a fixed multiple of it. Its constants (2.4, 0.3, 0.05) are fitted
to the envelope his receiver leaves. The backend therefore copies three
things from his receiver:

- **Bell's IF filter.** His two resonators become complex one-pole
  low-passes at the channel rate, about ±35 Hz. On the channelizer's wider
  channel the noise envelope moves faster, its minimum drops, and NOISE
  reads the noise several times too low. Every noise peak then becomes a
  mark: CER above 1 at 6 dB SNR500 without the filter, about 0.1 with it.
- **200 Hz.** |IQ| is averaged over two samples and interpolated from
  250 Hz to 200 Hz.
- **A peak AGC** (morse-wip's: rise 50 ms, fall 4 s). NOISE and the Kalman
  constants assume a signal near unit amplitude, a level Bell's simulation
  fixed by hand.

Two clamps from morse-wip stay on by default, though Bell had neither:

- **The AGC output** is clamped to [0, 1].
- **NOISE's output** is clamped to [0, 1] (`BellConfig::clamp_z`). NOISE's
  variance estimate fits Bell's receiver only. Elsewhere, a noise sample
  far below zero is better explained as a mark of uncertain amplitude than
  as a space of variance RN, and it decodes as a character.

`bell:listing` turns both off and restores the listing's 10 WPM floor.

## Measurements

`cw_engine_bench` (MSVC RelWithDebInfo build): the bench's own overs
(`VVV` and 12 tokens, ±3 % jitter, 0.5 s of noise before the keying)
through the channelizer, 20 overs a point. **Payload** CER scores the
decoded text against the sent tokens with a free start and end, so
characters decoded from the noise around the over don't count. **Whole**
CER is the whole over against `VVV <payload>`, and counts them.

SNR500 (dB) at which payload CER falls to 10 % / 3 % / 1 %, white noise,
tone at the channel centre; "—" where it doesn't by 16 dB (v3: 8 dB):

| | 12 WPM | 18 WPM | 25 WPM | 35 WPM |
|---|---|---|---|---|
| `bell` | 5.1 / 7.8 / — | 3.7 / 5.5 / — | 2.8 / 4.1 / 5.8 | 2.4 / 4.2 / 5.0 |
| `bell:wpm10-60` | 5.5 / — / — | 3.7 / 5.8 / — | 2.8 / 4.1 / 5.8 | 2.4 / 4.2 / 5.0 |
| `bell:listing` | 5.2 / — / — | 4.4 / — / — | 2.8 / 4.5 / — | 2.3 / 4.1 / 5.0 |
| `bell:sign` | 6.4 / 10.0 / — | — / — / — | 4.0 / — / — | 3.7 / 7.8 / — |
| `bell:port` | 5.8 / 15.6 / — | 4.8 / 8.6 / — | 4.0 / — / — | 4.2 / 8.7 / — |
| `v3` | −1.1 / −0.5 / 0.9 | −1.0 / −0.2 / 1.4 | −0.6 / 0.0 / 1.4 | 0.2 / 1.0 / 2.1 |

Payload / whole CER at 3 and 10 dB:

| | 12 WPM | 18 WPM | 25 WPM | 35 WPM |
|---|---|---|---|---|
| `bell`, 3 dB | 0.25 / 0.53 | 0.17 / 0.39 | 0.08 / 0.14 | 0.06 / 0.11 |
| `bell`, 10 dB | 0.023 / 0.30 | 0.026 / 0.25 | 0.002 / 0.06 | 0.001 / 0.05 |
| `bell:wpm10-60`, 10 dB | 0.039 / 0.31 | 0.028 / 0.26 | 0.002 / 0.06 | 0.001 / 0.05 |
| `bell:listing`, 10 dB | 0.040 / 0.34 | 0.036 / 0.39 | 0.025 / 0.21 | 0.000 / 0.09 |
| `bell:sign`, 10 dB | 0.030 / 0.33 | 0.239 / 0.67 | 0.049 / 0.16 | 0.026 / 0.09 |
| `bell:port`, 10 dB | 0.051 / 0.38 | 0.035 / 0.18 | 0.041 / 0.09 | 0.027 / 0.07 |

- **Bell is 4–6 dB behind v3** on white noise at the 10 % level, and
  further behind at 1 %.
- **The sign error is the damaging bug.** Alone (`:sign`) it raises
  payload CER at 10 dB from 0.026 to 0.24 at 18 WPM and from 0.002 to
  0.049 at 25 WPM. It stays hidden for paths at a speed where the 5 ms
  grid lands exactly on the nominal lengths (WPM dividing 240: 10, 12, 15,
  16, 20, 24, 30, 40, 48, 60). Its signature is words run together
  (`OK1RRDL5ABC`), the "missed word spaces" AG1LE's testers reported.
- **Above 10 dB payload CER levels off** instead of falling to 0: about
  0.03 at 12 and 18 WPM, under 0.01 at 25 and 35 (`bell`, 10–16 dB).
- **With the listing's 10 WPM floor, slow characters merge.** Within a
  few WPM of the floor a character space before a dit decodes as an
  element space: `TEST` as `NST`, `TU` as `X`, `DE` as `B`. At 40 dB,
  40 overs, `bell:wpm10-60` reads 22 of 24 `TEST`s as `NST` at 12 WPM and
  11 of 24 at 15 WPM; with the default 5 WPM floor, 0 and 1. The 10–16 dB
  payload CER at 12 WPM falls from 0.050 to 0.030; at 18 WPM and above
  the floor makes no difference. Part of the cause is in SPDTR, which
  zeroes steps past the floor without renormalising the rest, so a
  character space (RTRANS 0.15 on each outer step) loses more than an
  element space (0.1). Renormalising alone fixes 12 WPM but not 15.
- **The clamps remove most of the junk before the over** (`bell` against
  `bell:listing`, whole CER). What junk is left comes from the missing
  idle state (below), not from the port. At slow speeds the start adds
  more: in the noise before the over the speed estimate settles near 40
  WPM and falls at most 4 WPM per element, so at 12 WPM the `VVV` reads
  as `TTT TTTT TTTT`.
- **20 paths against 25** (`:paths20`) makes no consistent difference on
  the payload. The listing's 25 stay.

Speed tracking (a standalone harness on BellDecoder, clean keying at 30
and 10 dB): a step up from 20 to 35 or 15 to 55 WPM settles within 15 %
in 0.7–1.7 s; a drop from 55 to 12 WPM takes 3 s at 30 dB and 9 s at
10 dB, and the first slow dahs read as `T`. MEMDEL lets a path's speed
move at most 4 WPM per change between mark and space. The estimate
stays within the speed limits. A 0.06 % change of amplitude leaves the
decoded text unchanged.

### Fading

Payload CER at 24 WPM by fading and SNR500, 20 overs a point, 2 s of
noise before the keying (`bench.py fading`):

| | −6 dB | 0 dB | 6 dB | 10 dB | 20 dB |
|---|---|---|---|---|---|
| `bell`, none | 0.87 | 0.40 | 0.01 | 0.00 | 0.00 |
| `v3`, none | 0.97 | 0.02 | 0.00 | 0.00 | 0.01 |
| `bell`, ITU good | 0.81 | 0.50 | 0.20 | 0.10 | 0.02 |
| `v3`, ITU good | 0.85 | 0.44 | 0.19 | 0.13 | 0.03 |
| `bell`, ITU moderate | 0.83 | 0.61 | 0.25 | 0.12 | 0.07 |
| `v3`, ITU moderate | 0.79 | 0.53 | 0.39 | 0.29 | 0.08 |
| `bell`, ITU poor | 0.85 | 0.64 | 0.36 | 0.26 | 0.14 |
| `v3`, ITU poor | 0.76 | 0.61 | 0.55 | 0.48 | 0.14 |
| `bell`, flat 0.3 Hz | 0.83 | 0.57 | 0.24 | 0.10 | 0.04 |
| `v3`, flat 0.3 Hz | 0.82 | 0.50 | 0.29 | 0.23 | 0.05 |
| `bell`, flat 1 Hz | 0.84 | 0.64 | 0.34 | 0.23 | 0.14 |
| `v3`, flat 1 Hz | 0.76 | 0.63 | 0.56 | 0.43 | 0.15 |
| `bell`, flat 3 Hz | 0.85 | 0.74 | 0.53 | 0.43 | 0.29 |
| `v3`, flat 3 Hz | 0.73 | 0.68 | 0.67 | 0.56 | 0.18 |

- **Under fading Bell beats v3 at 6–10 dB.** From ITU moderate and flat
  0.3 Hz up, Bell's payload CER at 10 dB is 0.10–0.43 against v3's
  0.23–0.56. Each of Bell's paths carries its own amplitude filter, so a
  fade that v3 reads as a space Bell can still explain as a weaker mark.
- **At 20 dB v3 is level or better**, and much better at 3 Hz (0.18
  against 0.29), where Bell's fixed amplitude random walk (below) is too
  slow for the spread.

#### The amplitude random walk against the Doppler spread

MODEL's process noise while the key stays down is a fixed q = 10⁻⁴ per
5 ms in the listing (`BellConfig::mark_q`, switch `:q<x>`). Payload CER
at 24 WPM, 50 overs a point, 6 / 10 / 20 dB:

| Fading | `bell` (q 10⁻⁴) | best q | `bell`, best q | `v3` |
|---|---|---|---|---|
| none | 0.012 / 0.002 / 0.004 | ≤ 3·10⁻⁴ | same | 0.002 / 0.002 / 0.014 |
| ITU good | 0.238 / 0.104 / 0.025 | ≤ 3·10⁻⁴ | same | 0.180 / 0.117 / 0.044 |
| flat 0.1 Hz | 0.261 / 0.146 / 0.025 | 10⁻⁵ | 0.261 / 0.133 / 0.027 | 0.215 / 0.143 / 0.042 |
| flat 0.3 Hz | 0.283 / 0.139 / 0.052 | ≤ 10⁻⁴ | same | 0.316 / 0.248 / 0.061 |
| ITU moderate | 0.284 / 0.134 / 0.074 | 10⁻⁴–3·10⁻⁴ | same | 0.402 / 0.316 / 0.080 |
| ITU poor | 0.353 / 0.239 / 0.148 | 3·10⁻⁴ | 0.349 / 0.231 / 0.143 | 0.561 / 0.470 / 0.132 |
| flat 1 Hz | 0.344 / 0.221 / 0.145 | 3·10⁻⁴ | 0.346 / 0.205 / 0.132 | 0.563 / 0.463 / 0.143 |
| flat 3 Hz | 0.511 / 0.410 / 0.294 | 10⁻³ | 0.403 / 0.231 / 0.140 | 0.665 / 0.569 / 0.187 |

(q swept over 10⁻⁵, 10⁻⁴, 3·10⁻⁴, 10⁻³, 3·10⁻³, 10⁻²; at 50 overs,
differences under about 0.02 are noise.)

- **The best q grows as B²** (B the bench's spread in Hz, B = 2σ_f). A
  complex Gaussian gain with Doppler spread σ_f moves by
  E|Δg|² = (2πσ_fΔt)²·Ω per sample, half of it along the amplitude, so
  q ≈ (πBΔt)²/2 ≈ 1.2·10⁻⁴·B² for Δt = 5 ms and Ω = 1: 1.1·10⁻⁵ at
  0.3 Hz, 1.2·10⁻⁴ at 1 Hz, 1.1·10⁻³ at 3 Hz. The best values measured,
  10⁻⁵, 3·10⁻⁴ and 10⁻³, are within one step of the sweep's grid.
- **Bell's fixed 10⁻⁴ is right up to about 0.5 Hz.** A q matched to the
  spread gains only from 1 Hz up, and at 3 Hz it nearly halves the error
  rate (10 dB: 0.41 → 0.23; 20 dB: 0.29 → 0.14), putting Bell ahead of v3
  at every SNR from 6 dB.
- **Too large a q costs** on slow fading and on a steady signal: without
  fading at 0 dB, q 3·10⁻³ raises payload CER from 0.40 to 0.57. A
  tracker has to estimate the spread, not assume a fast one.
- **The advantage over v3 holds at 50 overs:** at 10 dB, ITU poor 0.24
  against 0.47, flat 1 Hz 0.22 against 0.46. v3 stays ahead at 0 dB,
  and on slow fading (ITU good, flat 0.1 Hz) up to 6 dB.

#### q chosen by the posterior

`:qbank` gives every node a q class as well as an element type and a
speed (Magill's bank of filters; `BellConfig::q_bank`, `q_switch`). A
path keeps its class while an element lasts and may move to another when
it changes, so the posterior picks the fading rate the way it picks the
speed. With one class the decoder is bit-exact with `mark_q` alone.
Payload CER at 10 dB (0 dB in the first row), 50 overs:

| Fading | q 10⁻⁴ | best fixed q | 4 classes, switch 0.01 | 4 classes, 0.002 | 2 classes {10⁻⁴, 10⁻³}, 0.01 | `v3` |
|---|---|---|---|---|---|---|
| none, 0 dB | 0.400 | 0.393 | 0.469 | 0.470 | 0.453 | 0.051 |
| ITU good | 0.104 | 0.104 | 0.129 | 0.129 | 0.127 | 0.117 |
| flat 0.1 Hz | 0.146 | 0.133 | 0.165 | 0.164 | 0.161 | 0.143 |
| flat 0.3 Hz | 0.139 | 0.134 | 0.160 | 0.159 | 0.158 | 0.248 |
| ITU moderate | 0.134 | 0.131 | 0.161 | 0.161 | 0.157 | 0.316 |
| ITU poor | 0.239 | 0.231 | 0.244 | 0.244 | 0.243 | 0.470 |
| flat 1 Hz | 0.221 | 0.205 | 0.219 | 0.219 | 0.215 | 0.463 |
| flat 3 Hz | 0.410 | 0.231 | 0.235 | 0.233 | 0.234 | 0.569 |

(Four classes are 10⁻⁴, 3·10⁻⁴, 10⁻³, 3·10⁻³. With switch 0.05 the bank
is 0.01–0.02 worse on slow fading; 50 paths instead of 25 changes
nothing.)

- **At 3 Hz the bank matches the best fixed q** (0.23 against 0.41 for
  the listing's), without knowing the spread. At 1 Hz and ITU poor it is
  level with the listing's q.
- **On slow fading and a steady signal it costs 0.02–0.03 at 10 dB and
  about 0.05 at 0–3 dB.** The switch rate is not the cause (0.01 and
  0.002 agree to 0.003), nor the number of classes or paths: with a fast
  class available, some noise is explained as amplitude movement within a
  mark. Two classes cost the least.
- **Against v3 the bank keeps the fading lead** (6–10 dB, ITU moderate
  and up) and gives up a little more on slow fading.
- The bank multiplies the nodes per path by the number of classes (two
  classes: 60, four: 120).

So a per-path choice of q recovers the fast-fading gain but pays on slow
fading, because each path judges the fading rate from a few elements. An
estimate of the spread over many elements, shared by the paths, should
keep the gain without the cost; that is the next experiment.
- **`bell:port` is within 0.1 of `bell`** in every fading cell, and
  worse in most.

## The coherent variant

`bell:coh` (`BellConfig::coherent`, not in the listing) keeps Bell's
trellis (letter states, XTRANS durations, speeds, SAVEP, TRELIS, TRANSL)
and replaces his envelope filter with a linear Kalman filter on the
channel's complex gain g. Along any one path the key sequence is known, so

    y = k·g + w,  w ~ CN(0, 1)          (k = 1 in a mark, 0 in a space)
    g' = a·g + v,  v ~ CN(0, (1 − a²)·Ω)

is linear and Gaussian, and the classic Kalman filter gives the exact
posterior of g and the exact likelihood of each node (per-survivor
processing). In a mark the filter updates on y, in a space it only
predicts, so its variance grows back towards Ω by itself. a comes from the
Doppler spread: an AR(1) whose correlation equals the bench's Gaussian
exp(−π²B²τ²/2) at τ = 40 ms. Ω is learned from the marks. NOISE, MODEL,
the IF emulation, the AGC and both clamps are not used.

Front end (`decode_bell`): every 2nd channel sample (8 ms at 250 Hz; the
±62 Hz passband leaves the noise close to white), divided by √floor so
the noise has variance 1. The AR(1) model cannot follow a steady
rotation, and the bench puts the tone 10 Hz off the channel centre, so an
AFC derotates first: every 0.5 s the Hann-windowed periodogram of the
last 4 s, its peak accepted when 30× the median, followed with weight
0.3. The decoder sees each sample 2 s after the AFC, so the first marks
are derotated with a locked estimate. Bell's trellis already delays text
by up to 1 s; the AFC adds 2 s, so these runs use `--tail 3`.

White noise, SNR500 (dB) for payload CER 10 % / 3 % / 1 %, 20 overs a
point, tone 10 Hz off centre:

| | 12 WPM | 18 WPM | 25 WPM | 35 WPM |
|---|---|---|---|---|
| `bell:coh` | −4.5 / −2.7 / −1.3 | −3.0 / −1.4 / −0.2 | −1.4 / 0.1 / 0.9 | 0.7 / 1.6 / 2.0 |
| `v3` (`--tail 3`) | −1.1 / −0.5 / 0.9 | −1.0 / −0.2 / 1.4 | −0.6 / 0.0 / 1.4 | 0.2 / 1.0 / 2.1 |
| `bell` (envelope) | 5.1 / 7.8 / — | 3.7 / 5.5 / — | 2.8 / 4.1 / 5.8 | 2.4 / 4.2 / 5.0 |

- **Coherence gains 6–10 dB over Bell's envelope decoder**, and puts it
  ahead of v3 by 3.4 dB at 12 WPM, 2 dB at 18 and 0.8 dB at 25 (10 %
  level). The longer the element, the more the filter can integrate.
- **At 35 WPM it is 0.5 dB behind v3.** A dit is 34 ms, four 8 ms samples.
  Sampling at 250 Hz instead (`:rate250`, each noise sample counted as half
  a white one) is worse, not better: 25 WPM at −1 dB 0.05 → 0.23, 35 WPM at
  1 dB 0.06 → 0.32. The white-noise filter misreads the correlated noise.
  More likely the keying edges cost here: the bench's 5 ms raised-cosine
  edges and the channel filter are a large share of a 34 ms dit, and the
  model's key is 0 or 1.
- **Little junk around the over:** whole-over CER at 4–8 dB is 0.00–0.05,
  about v3's 0.01–0.05, where envelope Bell has 0.05–0.30. With N0 from
  the band floor the likelihoods are calibrated, and noise rarely
  explains better as a mark.

Fading, payload CER at 24 WPM, 6 / 10 / 20 dB, 50 overs a point:

| Fading | `bell` (envelope) | `coh`, B 0.5 Hz | `coh`, true B | `coh`, bank {0.3, 1, 3} Hz | `v3` |
|---|---|---|---|---|---|
| none | 0.012 / 0.002 / 0.004 | 0.001 / 0.000 / 0.004 | 0.002 / 0.000 / 0.000 | 0.007 / 0.002 / 0.014 | 0.002 / 0.002 / 0.014 |
| ITU good | 0.238 / 0.104 / 0.025 | 0.116 / 0.047 / 0.012 | 0.143 / 0.079 / 0.017 | 0.127 / 0.073 / 0.029 | 0.180 / 0.117 / 0.044 |
| flat 0.1 Hz | 0.261 / 0.146 / 0.025 | 0.150 / 0.068 / 0.017 | 0.180 / 0.081 / 0.022 | 0.160 / 0.108 / 0.046 | 0.215 / 0.143 / 0.042 |
| flat 0.3 Hz | 0.283 / 0.139 / 0.052 | 0.143 / 0.071 / 0.014 | 0.170 / 0.086 / 0.020 | 0.205 / 0.118 / 0.044 | 0.316 / 0.248 / 0.061 |
| ITU moderate | 0.284 / 0.134 / 0.074 | 0.180 / 0.107 / 0.012 | same | 0.234 / 0.129 / 0.036 | 0.402 / 0.316 / 0.080 |
| ITU poor | 0.353 / 0.239 / 0.148 | 0.341 / 0.228 / 0.032 | 0.188 / 0.093 / 0.019 | 0.255 / 0.138 / 0.033 | 0.561 / 0.470 / 0.132 |
| flat 1 Hz | 0.344 / 0.221 / 0.145 | 0.370 / 0.233 / 0.034 | 0.220 / 0.105 / 0.019 | 0.264 / 0.155 / 0.035 | 0.563 / 0.463 / 0.143 |
| flat 3 Hz | 0.511 / 0.410 / 0.294 | 0.656 / 0.632 / 0.194 | 0.279 / 0.119 / 0.020 | 0.299 / 0.119 / 0.020 | 0.665 / 0.569 / 0.187 |

(`coh`, true B: the condition's own spread, 0.05 Hz for none. The bank
{0.1, 0.5, 1.5, 4} Hz is 0.01–0.07 worse than {0.3, 1, 3} on slow
fading and level on fast.)

- **With the spread known, the coherent decoder is level with or better
  than v3 and envelope Bell in every condition** at 6 dB and up: at 10 dB
  0.00–0.12 against v3's 0.00–0.57 and envelope Bell's 0.00–0.41.
- **A fixed 0.5 Hz is best on slow fading** (ITU good to moderate, 0.05–
  0.11 at 10 dB), better than the true spread there: the model also has
  to absorb the AFC's residual frequency error. At 1–3 Hz it fails (3 Hz:
  0.63).
- **The bank {0.3, 1, 3} Hz needs no spread and beats v3 under every
  fading condition from 6 dB**, by 0.04–0.45 at 10 dB, except the slowest
  at 20 dB (flat 0.1 Hz: 0.046 against 0.042); on a steady carrier it is
  level. It pays 0.02–0.05 against the best single spread on slow fading,
  the same cost as the envelope q bank.
- **At 0–3 dB under fading every decoder is poor** (0.3–0.7); coherence
  helps most at 6–15 dB.

Impulses, noise and beating paths (`bench.py impulse` and `noise`
arguments; beating: two fading paths of equal power, `paths:` with
Doppler shifts ±0.5 Hz at 0.1 Hz spread, and ±1 Hz at 0.3 Hz with 1 ms
delay; 24 WPM, 30 overs):

| | `bell` | `coh` 0.5 Hz | `coh` bank {0.3, 1, 3} | `v3` |
|---|---|---|---|---|
| false characters/h, noise only | 417 | 87 | 144 | 3 |
| CER 25 WPM 6 dB, no impulses | 0.006 | 0.000 | 0.001 | 0.007 |
| 1 impulse/s, 40 dB | 0.280 | 0.245 | 0.340 | 0.128 |
| 3 impulses/s, 40 dB | 0.568 | 0.589 | 0.677 | 0.311 |
| 50/s periodic, 30 dB (ignition) | 0.690 | 0.752 | 0.941 | 0.228 |
| false characters/h, noise with 1 impulse/s | 2670 | 1840 | 2280 | 0 |
| beating ±0.5 Hz, CER at 10 dB | 0.219 | 0.197 | 0.131 | 0.421 |
| beating ±1 Hz, CER at 10 dB | 0.312 | 0.515 | 0.140 | 0.539 |

- **Impulses break both Bell variants:** each spike decodes as a dit. With
  Gaussian noise a spike in a space is all but impossible, while a mark of
  uncertain amplitude can absorb it.
- **Noise alone still makes characters** (no idle state): 87–144 an hour
  coherent, 3 for v3.
- **A heavy-tailed noise model helps against impulses, but a decoder on
  one channel cannot tell an impulse from a fast fade.** With
  `:imp<ε>:kappa<κ>` the noise is (1 − ε)·CN(0, 1) + ε·CN(0, κ): a sample
  far outside the Gaussian part scores as weak evidence for either key
  state. 25 WPM 6 dB, 20 overs; ITU poor 24 WPM, 30 overs:

  | | 1/s 40 dB | 10/s 40 dB | 50/s 30 dB | ITU poor 10 dB | 15 dB |
  |---|---|---|---|---|---|
  | `coh` | 0.245 | 0.742 | 0.752 | 0.202 | 0.086 |
  | `:imp0.05:kappa30` | 0.003 | 0.078 | 0.215 | 0.401 | 0.334 |
  | `:imp0.05:kappa30:impscore` | 0.051 | 0.361 | 0.304 | 0.175 | 0.066 |
  | `v3` | 0.128 | 0.709 | 0.228 | 0.419 | 0.195 |

  Weighting the filter update by the impulse posterior (the default once
  ε > 0) removes almost every impulse error, but a fast fade also looks
  like an impulse, the update stops and the filter loses the gain.
  Scoring only (`:impscore`, the filter always takes the Gaussian update)
  keeps fading tracked, and even helps there, at half the impulse gain.
  κ has to match the impulses: 10 suits 30 dB ignition, 1000 suits 40 dB
  spikes, 30–50 covers both. With the spread bank, `:impscore` hurt ITU
  poor (0.24 against 0.14), not yet understood. ε = 0 stays the default,
  so the tables above are Gaussian. An impulse is broadband and lands in
  every channel at once, a fade is one signal's: a wideband blanker before
  the channelizer can tell them apart, where this decoder cannot.
- **Beating paths suit the bank:** the composite gain of two Doppler-
  shifted paths is not an AR(1) of one spread, but its fast classes absorb
  the beat. A spread estimate has to measure how fast the gain actually
  decorrelates, not assume one path's spread.

Realistic impulses (`bench.py impulse2`: receiver-shaped pulses, sferics,
trains, crashes, Class A, clipping; see the bench README). Payload CER at
25 WPM, SNR500 6 dB, 20 overs, and false characters per hour on noise
with the same impulses (v3 / coherent with `:imp0.05:kappa30:impscore`):

| Impulses | `v3` | `bell` | `coh` | `coh` + impulse model | false/h |
|---|---|---|---|---|---|
| none | 0.007 | 0.006 | 0.000 | 0.000 | 0 / 120 |
| `rx` 1/s 40 dB | 0.139 | 0.277 | 0.297 | 0.098 | 0 / 130 |
| sferic 3/s 40 dB | 0.755 | 0.610 | 0.683 | 0.160 | 0 / 760 |
| `rx` 10/s 40 dB | 0.762 | 0.736 | 0.751 | 0.381 | 0 / 3130 |
| ignition 50/s 30 dB | 0.299 | 0.769 | 0.764 | 0.315 | 0 / 9700 |
| rasp 100/s 25 dB | 0.378 | 0.389 | 0.791 | 0.682 | 30 / 2270 |
| crashes 0.2/s 40 dB | 0.229 | 0.135 | 0.162 | 0.067 | 120 / 290 |
| Class A 0.1 / 0.01 | 0.002 | 0.033 | 0.005 | 0.000 | 10 / 80 |
| `rx` 3/s 70 dB, clip 60 | 0.995 | 0.713 | 0.703 | 0.759 | 0 / 5550 |

- **Realistic impulses defeat every decoder:** three sferics a second take
  v3 to 0.76; strong pulses clipped at the ADC leave v3 decoding nothing
  and the Bell variants losing about 0.7.
- **The impulse model makes coherent Bell the most robust on sparse and
  shaped pulses** (sferics 0.16, crashes 0.067), but it decodes hundreds
  to thousands of false characters an hour where v3 makes almost none,
  and it fails on dense power-line rasp.
- No decoder working on one channel handles strong or dense impulses:
  that needs the wideband stage of ALGORITHM.md §5.7.

With that stage, `ImpulseRepair` (`engine/impulse_repair.h`, bench
`--repair`), on the wideband stream before the channelizer (ALGORITHM.md
§5.7 has the method): strong signals are split off by a short-time FFT,
a complex AR(32) model of the rest predicts each sample, a prediction
error 12 dB over the error's median opens a gap, and a gap whose error is
broadband is filled by forward and backward prediction, crossfaded. The
same grid, 6 dB:

| Impulses | `v3` | `bell` | `coh` | `coh` + impulse model | false/h |
|---|---|---|---|---|---|
| none | 0.004 | 0.010 | 0.002 | 0.000 | 0 / 130 |
| `rx` 1/s 40 dB | 0.004 | 0.007 | 0.002 | 0.000 | 0 / 130 |
| sferic 3/s 40 dB | 0.030 | 0.151 | 0.101 | 0.013 | 0 / 120 |
| `rx` 10/s 40 dB | 0.010 | 0.040 | 0.021 | 0.009 | 0 / 100 |
| ignition 50/s 30 dB | 0.002 | 0.005 | 0.002 | 0.000 | 0 / 120 |
| rasp 100/s 25 dB | 0.000 | 0.227 | 0.087 | 0.016 | 0 / 750 |
| crashes 0.2/s 40 dB | 0.020 | 0.055 | 0.040 | 0.011 | 0 / 120 |
| Class A 0.1 / 0.01 | 0.002 | 0.014 | 0.000 | 0.000 | 0 / 70 |
| `rx` 3/s 70 dB, clip 60 | 0.028 | 0.018 | 0.010 | 0.012 | 0 / 110 |

- **Every decoder gains**, v3 most: sferics 0.755 → 0.030, clipped strong
  pulses 0.995 → 0.028. Coherent Bell with the impulse model stays the
  best on nearly every impulse case, and its false characters on
  impulsive noise fall to about its level on plain noise, about 100 an
  hour (v3: 0).
- **On clean signals and fading the stage costs little:** the white-noise
  ladder is level within noise (v3 at 25 WPM, 60 overs: −2 dB 0.593 →
  0.570, 0 dB 0.032 → 0.045, 2 dB 0.007 → 0.010); ITU poor at 15 dB loses
  about 0.02 (v3 0.195 → 0.215), coherent Bell nothing.
- **Why each guard is there.** A strong mark coming on is as unpredictable
  as an impulse, and its error never settles: gaps are capped at 32 ms
  and such a gap is left alone (a gap allowed to run to 64 ms filled the
  start of every strong mark with silence, ITU poor at 15 dB 0.086 →
  0.163). The cap must not be shorter: sferic trains and clipped pulses
  ring for more than 8 ms. And a narrowband error (a carrier keyed on, a
  fade) is never filled, which a test on the gap's own spectrum decides
  without averages, so dense trains are found as well as single pulses.

A crowded band (`--qrm 10|30|60:30:20`: other stations at 10–30 dB SNR500
in other channels), v3 at 6 dB, 20 overs, payload CER without → with the
repair:

| Stations | none | sferic 3/s | ignition 50/s | crashes |
|---|---|---|---|---|
| 0 | 0.007 → 0.004 | 0.755 → 0.030 | 0.299 → 0.002 | 0.229 → 0.020 |
| 10 | 0.004 → 0.009 | 0.752 → 0.106 | 0.306 → 0.005 | 0.229 → 0.053 |
| 30 | 0.002 → 0.002 | 0.750 → 0.158 | 0.305 → 0.035 | 0.229 → 0.086 |
| 60 | 0.001 → 0.024 | 0.748 → 0.093 | 0.298 → 0.161 | 0.251 → 0.068 |

- **The repair still pays in a crowd, but less**: one AR model of the
  weak part cannot follow every station the split leaves in it. With 60
  stations the split sets almost nothing aside (they fill so much of the
  band that the median bin is a station), and a clean crowded band loses
  0.02. Coherent Bell with the impulse model behaves alike (60 stations:
  sferics 0.158 → 0.088, ignition 0.326 → 0.175). A rejected gap pauses
  detection for only 8 ms: a 64 ms pause leaves impulses unrepaired in a
  crowd (30 stations, sferics 0.217 against 0.153 at 10 overs). A larger
  AR model (48, 64) gives no clear gain.
- **The decoder can be told which samples were rebuilt**
  (`SkimDecodeChannel::set_repair_weights()`: per channel sample, the
  share of repaired wideband samples in the channelizer prototype's span
  behind it). With `:repw` coherent Bell scales a sample's log-likelihood
  and Kalman gain by 1 − that share. It trims what the repair leaves, at no
  cost on clean signals or fading. CER at 3 / 6 dB with `--repair`:

  | | sferic 3/s | rasp 100/s | crashes | clipped |
  |---|---|---|---|---|
  | `coh` | 0.056 / 0.101 | 0.208 / 0.125 | 0.031 / 0.039 | 0.006 / 0.014 |
  | `coh:repw` | 0.053 / 0.085 | 0.173 / 0.085 | 0.025 / 0.033 | 0.005 / 0.004 |
  | `coh:imp0.05:kappa30:impscore` | 0.008 / 0.013 | 0.069 / 0.006 | 0.010 / 0.011 | 0.014 / 0.012 |
  | the same with `:repw` | 0.004 / 0.009 | 0.063 / 0.008 | 0.009 / 0.003 | 0.010 / 0.007 |

`:coh` switches (after `bell`): `:spread<B>` (Hz, default 0.5),
`:spreads<x>/<y>/...` a spread class per path (with `:qsw<x>`),
`:match<ms>` the AR(1) match lag (40), `:noafc`, `:rate<Hz>` the sample
rate (125).

## Running it

```sh
cw_engine_bench --cfg bell --wpm 25 --snr 0:10
python3 experiments/cw_bench/bench.py --bin <build>/cw_engine_bench ladder --cfg bell --cfg bell:port --cfg v3
```

Switches, after the name and in any order:

| Switch | What |
|---|---|
| `:port` | morse-wip as published: 20 paths, 5–80 WPM, the `xtrans` sign error, its code tree, both clamps |
| `:listing` | the listing alone: no clamps, 10–60 WPM |
| `:sign` | the `xtrans` sign error alone |
| `:noclamp` | neither clamp |
| `:paths<N>` | the most paths saved |
| `:noif` | no emulation of Bell's IF filter |
| `:wpm<lo>-<hi>` | the speeds a path may move to (default 5–60) |
| `:q<x>` | MODEL's amplitude process noise per 5 ms during a mark (default 0.0001) |
| `:qbank` | four q classes (10⁻⁴, 3·10⁻⁴, 10⁻³, 3·10⁻³) as a hidden state per path, picked by the posterior (`BellConfig::q_bank`) |
| `:qsw<x>` | the probability that a path changes q class at a change of element (default 0.05) |
| `:coh` | the coherent variant ("The coherent variant"), with `:spread<B>`, `:spreads<x>/<y>/...`, `:match<ms>`, `:noafc`, `:imp<ε>`, `:kappa<κ>`, `:impscore`, `:repw` |

## Known limits

- **No idle state.** After 14 dits of pause the model must start a mark,
  so noise before and between overs decodes as characters. Bell tested on
  continuous simulated text. A squelch would have to come from outside.
- **No AFC.** The IF emulation is ±35 Hz around the channel centre.
- **Envelope only.** The amplitude filter sees |z|, with a Gaussian model
  of a Rician envelope. FADING_TRACKER.md explains why a complex filter on
  I/Q does better.
- **Fixed amplitude dynamics.** `MODEL`'s random walk (q = 10⁻⁴ per 5 ms)
  is not tied to the Doppler spread, and its variance shrinks during
  spaces. `:q<x>` sets it by hand; nothing estimates the spread.
