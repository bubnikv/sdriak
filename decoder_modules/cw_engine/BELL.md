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
- **`bell:port` is within 0.1 of `bell`** in every fading cell, and
  worse in most.

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

## Known limits

- **No idle state.** After 14 dits of pause the model must start a mark,
  so noise before and between overs decodes as characters. Bell tested on
  continuous simulated text. A squelch would have to come from outside.
- **No AFC.** The IF emulation is ±35 Hz around the channel centre.
- **Envelope only.** The amplitude filter sees |z|, with a Gaussian model
  of a Rician envelope. FADING_TRACKER.md explains why a complex filter on
  I/Q does better.
- **Fixed amplitude dynamics.** `MODEL`'s random walk (q = 10⁻⁴ per 5 ms)
  is not tied to the Doppler spread, and its variance shrinks during spaces.
