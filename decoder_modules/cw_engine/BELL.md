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
| SPDTR | 172 | speeds 5–80 WPM | the listing: 10–60 WPM |
| PATH, LIKHD | 173–175 | same | same |
| KALFIL, MODEL | 176–178 | same | same |
| PROBP | 178–179 | returns with P stale when every node scores 0 | restarts the decoder (the listing divides by 0) |
| SPROB | 179–180 | same | same |
| SAVEP | 180–183 | normalises with P(1) included | same as morse-wip. The listing starts the final sum at 0 and leaves P(1) out (line 40800), a slip in the listing |
| TRELIS | 184–186 | trellis zeroed, so the first 1 s indexes path 0, out of bounds; `ndel` read uninitialised | the listing's `DATA PTHTRL/5000*5/, LMDSAV/5000*5/` |
| TRANSL | 186–188 | its own: a code tree turns element pairs into characters. Z has no dah child, so `,` is unreadable; Q's dit child points at `0`; when one trellis step settles several letters, each is written over the last in the same buffer | the tree, with Z and Q fixed, appending every letter |
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

`bell:listing` turns both off.

## Measurements

A standalone run of the backend: synthetic keyed carrier at 250 Hz
complex baseband, complex white noise, no fading, tone at the channel
centre. Each run is 1 s of noise, 14 words drawn from contest traffic
(CQ, DE, 5NN, calls), then 2.5 s of noise; 8 seeds per cell.

**Main copy** CER counts only the decoded text against the sent text
(Levenshtein; characters decoded from the noise before and after the
over are not counted). **Total** CER counts them too.

| WPM | SNR500 | main copy, morse-wip | main copy, corrected | total, morse-wip | total, corrected | total, listing (no clamps) |
|---|---|---|---|---|---|---|
| 20 | +10 dB | 0.037 | 0.021 | 0.086 | 0.080 | 0.271 |
| 20 | +3 dB | 0.086 | 0.037 | 0.148 | 0.099 | 0.370 |
| 20 | 0 dB | 0.312 | 0.226 | 0.417 | 0.324 | 0.583 |
| 25 | +10 dB | 0.071 | 0.013 | 0.108 | 0.104 | 0.136 |
| 25 | +3 dB | 0.093 | 0.030 | 0.130 | 0.127 | 0.175 |
| 25 | 0 dB | 0.376 | 0.168 | 0.484 | 0.272 | 0.404 |
| 32 | +10 dB | 0.020 | 0.013 | 0.056 | 0.087 | 0.096 |
| 32 | +3 dB | 0.076 | 0.040 | 0.121 | 0.137 | 0.126 |
| 32 | 0 dB | 0.316 | 0.233 | 0.365 | 0.341 | 0.343 |

- **The sign error is the damaging bug.** It costs most at 25 WPM, where
  the 5 ms grid never lands on the nominal lengths. At 20 WPM, the speed
  morse-wip starts its paths at, the grid lands exactly on them and the bug
  stays hidden for paths at that speed. Its signature is words run
  together (`OK1RRDL5ABC`, `G3XYZRST`), the "missed word spaces" AG1LE's
  testers reported.
- **Taken alone, the sign error raises total CER 1.4–1.7×** at 20 and 25
  WPM (listing plus `:sign` against the listing, 12 seeds: 25 WPM 0 dB
  0.54 → 0.74, +3 dB 0.19 → 0.28).
- **Most of what is left is junk before the over.** That comes from the
  missing idle state (below), not from the port.
- **Without the IF emulation** both versions decode noise as text: CER
  above 1 at 6 dB SNR500.
- **20 paths against 25** makes no consistent difference on the main
  copy. The listing's 25 stay.

Not measured yet: fading, the `cw_bench` channelizer and the gates.
These numbers come from the standalone harness, not from `cw_engine_bench`.

## Running it

```sh
cw_engine_bench --cfg bell --wpm 25 --snr 0:10
python3 experiments/cw_bench/bench.py --bin <build>/cw_engine_bench ladder --cfg bell --cfg bell:port --cfg v3
```

Switches, after the name and in any order:

| Switch | What |
|---|---|
| `:port` | morse-wip as published: 20 paths, 5–80 WPM, the `xtrans` sign error, both clamps |
| `:listing` | the listing alone: no clamps |
| `:sign` | the `xtrans` sign error alone |
| `:noclamp` | neither clamp |
| `:paths<N>` | the most paths saved |
| `:noif` | no emulation of Bell's IF filter |

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
