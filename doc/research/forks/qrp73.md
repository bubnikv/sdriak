# qrp73/SDRPP — features worth porting

Review date: 2026-07-06. Reviewed https://github.com/qrp73/SDRPP against our fork
(local clone used for the diff: `scratchpad\qrp73-sdrpp`).

## What the fork is

qrp73/SDRPP is a **receiver-quality and file-handling focused fork based on SDR++ 1.1.0**
(~2 years behind our 1.2.2-alpha base), still active (last commit May 22, 2026,
~1,655 commits). It adds no hardware we lack — we have far more source modules.
Since it forked at 1.1.0, nothing merges cleanly; everything is an adapt-and-port job.
GPL-3.0, so license-compatible with our fork.

## Worth porting (in priority order)

### 1. Squelch hysteresis + hold — tiny patch, immediate win — **PORTED 2026-07-07**
Their `core/src/dsp/noise_reduction/squelch.h` adds 1 dB hysteresis and a 100 ms hold
before unmuting, and fixes the dB math (upstream/ours uses `10*log10` on an amplitude
mean where `20*log10` is correct). Ours is verbatim upstream with none of that.

Caveat: their counter is a `static int` inside `process()`, which is a bug with
multiple VFOs — make it a member when porting.

Port notes: counter made a per-instance member; the hold is sample-count based
(`Squelch::setSamplerate`, wired from `selectDemodByID`) instead of their "10 blocks
≈ 100 ms" assumption. Behavior change for users: with the `20*log10` fix the same
signal reads twice the dB value, so previously saved squelch levels trigger
differently and need re-adjusting (slider range -100..0 dB still covers it).

### 2. Manual/auto AGC switch for CW/SSB/AM/DSB — small, very relevant to QMX/ham use — **PORTED 2026-07-07**
Each demodulator gets an AGC on/off checkbox plus a gain slider in dB; in auto mode the
slider displays the live AGC gain, in manual mode it sets fixed gain. Clean
implementation: `dsp::demod::SSB/AM/CW` gain `setEnabled`/`setAGCGain`/`getAGCGain`,
plus ~40 lines of UI per demod header (see their
`decoder_modules/radio/src/demodulators/usb.h`). Ours only has attack/decay sliders.

Port notes: covers USB/LSB/DSB/CW/CW-R (checkbox + gain slider) and AM
(Off/Carrier/Audio AGC mode combo, replacing the "Carrier AGC" checkbox; the legacy
`carrierAgc` config key is migrated to `agcMode`). Unlike qrp73, the
`dsp::demod::SSB/AM/CW::init()` signatures were kept unchanged (AGC state is applied
via setters after init) so third-party callers and `vor_receiver` don't break. Attack/
decay sliders are greyed out in manual mode, the gain slider shows live AGC gain in
auto mode, and the last AGC gain carries over when switching to manual.

### 3. file_source format support — self-contained, high interop value — **PORTED 2026-07-07**
Their `source_modules/file_source/src/wavreader.h` (252 lines, drop-in replacement
shape) handles:
- RF64 (>4 GB recordings)
- WAVE_FORMAT_EXTENSIBLE (incl. subformat GUID parsing)
- IEEE float 32/64
- 8/16/24/32-bit PCM
- recovery of broken WAVs with wrong sample counts in the header

Ours is the bare upstream reader (effectively 16-bit PCM only). Matters for playing
back recordings from SDR#, SDR Console, HDSDR, etc.

Port notes: `wavreader.h` adapted with fixes — their `readSamples` remaining-bytes
math over-subtracted `_dataOffset` (and its `size_t < 0` check was dead code); the
parsed ds64/data chunk sizes were discarded (we honor them when plausible and only
fall back to physical file size for recovery, so trailing non-data chunks aren't
played as samples); RIFF odd-size chunk padding handled; truncated headers can't
infinite-loop the chunk scan; `wBlockAlign==0` recovered from channels×bits. Their
per-format worker loops used VLAs and the `0.5d` literal suffix (both GCC-only, we
build MSVC) — replaced by a single worker with per-format converter functions;
mono plays as I=Q like theirs, >2-channel files play channels 0/1. The manual
"Float32 Mode" checkbox is gone (format auto-detected; a format/duration line shows
in the menu instead), file selection is disabled while running (crash fix — deleting
the reader under the worker), and filename frequency parsing now accepts
kHz/MHz/GHz and decimals ("7.100MHz", SDR Console/HDSDR style). Not ported: their
play-position slider, loop checkbox and real-time pacing (our playback is paced by
the DSP sink and always loops, as upstream).

### 4. Recorder: FLAC, MP3 (VBR), and 24-bit PCM containers — medium effort — **FLAC + INT24 PORTED 2026-07-07** (MP3 not adopted)
Contained in `core/src/utils/wav.{h,cpp}` + recorder `main.cpp` +
`cmake/FindFLAC.cmake` / `cmake/Findmp3lame.cmake`. Code is tidy, but it adds
**libFLAC and LAME to our static-deps CI** across Windows/Android/macOS/Linux — real
but manageable work given our deps infrastructure. FLAC for baseband IQ recording is a
genuinely useful storage saver.

Dependency analysis (2026-07-07): their `wav::Writer` uses only the encode APIs
(`FLAC/stream_encoder.h`, `lame/lame.h`); their three `cmake/Find*.cmake` modules
don't fit our config-package-based `sdrpp_link_dep` flow and are not needed. libogg
is not needed at all — it only serves the Ogg-FLAC container, and the recorder
writes native `.flac` (build libFLAC with `WITH_OGG=OFF`).

FLAC dependency infra landed 2026-07-07: `deps/+flac/` recipe (FLAC 1.5.0,
`WITH_OGG=OFF`, encoder-only; `patch_flac.cmake` drops the unconditional
`find_dependency(Ogg)` from `flac-config.cmake.in` that would break Ogg-less
prefixes), registered `portable:bundled/static distro:system android:bundled/static`
(USAGE core), added to the root dep set, linked PRIVATE into `sdrpp_core`
(`FLAC::FLAC`; static builds carry `FLAC__NO_DLL` as an exported PUBLIC define),
`libflac-dev` added to the deb docker apt list (bookworm+ resolves via CMake
config, bullseye/focal/jammy fall back to `flac.pc`). No CI yml changes needed —
the deps caches key on `deps/+**` content.

Code port (2026-07-07): `wav::Writer` gained the FLAC encoder path and
`SAMP_TYPE_INT24` (both WAV and FLAC); recorder gained the FLAC container option
and Int24, with the sample-type list rebuilt per container (Float32 hidden for
FLAC) and the file extension taken from the writer. Port differences vs qrp73:
`FLAC__StreamEncoder` is held as an opaque `void*` in `wav.h` (it's an
anonymous-struct typedef — NOT forward-declarable — and including FLAC headers
in wav.h would break the PRIVATE core linkage); `SAMP_TYPE_INT24` was appended
at the enum end because the recorder persists sample types by integer value;
the FLAC conversion buffer is allocated once at open (qrp73 allocated a
std::vector per write() call on the DSP thread); int24/FLAC float→int scaling is
done in double — qrp73's float-precision `(8388608.0f - 0.5f)` rounds to 2^23
and wraps full-scale +1.0 samples to negative full-scale in the int24 WAV path;
int24/FLAC float→int scaling uses the symmetric `2^(bits-1) - 1` factor in
double (volk-convention) instead of qrp73's `x * (2^(bits-1) - 0.5) - 0.5`,
which (a) maps exact 0 to −1 LSB — a DC offset on silence — and (b) in the
int24 WAV path computed the scale in float, where `(8388608.0f - 0.5f)`
rounds to 2^23 and wraps full-scale +1.0 samples to negative full scale;
encoder errors are logged once per file, not per block. Existing WAV
uint8/int16/int32/f32 conversions kept upstream volk semantics (unchanged).
Limits: FLAC caps at 1048575 Hz sample rate (libFLAC ≥ 1.4; 655350 on 1.3.x =
Debian bullseye / Ubuntu focal+jammy) and 32-bit samples need libFLAC ≥ 1.4 —
`open()` fails cleanly with a logged error, recorder stays idle.

MP3/LAME not adopted — Opus used instead (2026-07-07). Rather than port qrp73's
MP3/LAME option (LAME 3.100 has no CMake build and Debian's `libmp3lame-dev`
ships no pkg-config/CMake config, so it would need a PATCH_COMMAND-injected
CMakeLists), the lossy-audio recorder uses the royalty-free Opus codec. Both
libopus 1.5.2 and libogg 1.3.6 have clean upstream CMake builds that install
config packages (`Opus::opus`, `Ogg::ogg`) — no recipe patching needed, unlike
FLAC. Registered `portable:bundled/static distro:system android:bundled/static`
(USAGE core), added to the root dep set, linked PRIVATE into `sdrpp_core`,
`libopus-dev`/`libogg-dev` added to the deb docker apt list.

Opus code port (2026-07-07): `wav::Writer` gained `FORMAT_OPUS` — a from-scratch
Ogg-Opus muxer (RFC 7845) in wav.cpp (opus/ogg headers kept out of wav.h behind
an opaque `void* opusState`, same PRIVATE-linkage reasoning as FLAC). We write
the Ogg framing directly against libogg rather than adding libopusenc (which has
no CMake build): OpusHead/OpusTags header packets, `opus_encode_float` on 20 ms
frames, granule/pre-skip bookkeeping in 48 kHz samples, and a one-frame
hold-back so the true last frame carries the EOS flag and a trimmed granule
position (drops encoder delay + final-frame zero padding for sample-accurate
length). Constraints that differ from FLAC/PCM and shape the recorder UI: Opus
only accepts 8/12/16/24/48 kHz and 1–2 channels, so it is **audio-mode only**
(baseband IQ rates are rejected in `open()` and the record button is blocked
with a reason); quality is set by a bitrate slider (16–256 kbps, default 128,
persisted as `opusBitrate`) shown in place of the sample-type combo. Not done:
resampling arbitrary audio rates (e.g. 44.1 kHz) to 48 kHz — unsupported rates
are rejected rather than resampled; and file_source can't play `.opus` back
(neither could it play FLAC) — recordings open in external players.

### 5. Level meter replacing SNR meter — small-medium — **PORTED 2026-07-07**
`ImGui::LevelMeter(level, levelMax, snr, …)` (their
`core/src/gui/widgets/snr_meter.{h,cpp}`) gives audio level + peak-hold + numeric
dB/SNR readout instead of the bare SNR bar. Nice operator-facing improvement; needs
main_window wiring.

Port notes: `SNRMeter`/`GetSNRMeterMinWidth` removed (main_window was the only
caller) and replaced by `LevelMeter`/`GetLevelMeterMinWidth`, adapted to our
`style::dp()` scaling and min-width font/scale cache. The level source is the
`strength` output of `calculateVFOSignalInfo` that our `pushFFT` previously
discarded; peak hold is a 10-frame ring buffer member in `WaterFall` (their
`std::queue` copy-per-frame approach not taken). Differences from qrp73: the
numeric readout (peak level, SNR below) is right-aligned *inside* the widget
instead of overflowing 25 px past it, the readout column is reserved in the
min-width so the top-bar layout adapts, non-finite level/levelMax/snr draw an
empty meter with a `--.- dB` readout (used when no VFO is selected — theirs
drew a full green bar in that case, `LevelMeter(0,0,0)`), and the `-90` tick
label is clamped to the widget's left edge.

### 6. FFT window functions + unity gain — small — **PORTED 2026-07-07**
Adds Blackman-Harris-4/7, Blackman-Nuttall, Nuttall, Hamming, Hann, cosine as separate
headers under `core/src/dsp/window/`, with unity-gain normalization so FFT amplitude
readings are calibrated, plus a 1M FFT size option. Easy port into `iq_frontend` +
display menu.

Port notes: our 1.2.x base already had all window headers except
`blackman_harris7.h` (Albrecht 7-term, the only new DSP file) — the gap was that
`IQFrontEnd` only exposed 3 of them. `IQFrontEnd::FFTWindow` was extended to 7
entries (Rectangular, Hamming, Hann, Blackman, Nuttall, BH4, BH7 — same set and
UI order as qrp73; Blackman-Nuttall deliberately not exposed, it duplicates
Nuttall/BH4) and window generation was unified into `IQFrontEnd::genFFTWindow`
instead of adopting their `dsp::window::createWindow`, fixing two bugs along the
way: their centered-normalization loop overruns the buffer by one float when the
size is odd (`_nzFFTSize` can be odd), and our `init()` filled the rectangular
window with zeros and skipped the DC-centering factor. Unity-gain normalization:
window sum scaled to 1, volk power-spectrum normalization factor changed from
`_fftSize` to 1.0 — this also fixes upstream's level sag with zero padding at
high FFT rates. Hamming coefficients changed to the exact equiripple values.
Config: `fftWindow` now stores the window *name* (a saved index would have
changed meaning); legacy integer configs are migrated on load (0→Rectangular,
1→Blackman, 2→Nuttall). The 1M FFT size option was NOT adopted (commented out in
display.cpp): the waterfall stores rawFFTSize × waterfallHeight floats — >1 GiB
at 1M bins — and its malloc/realloc paths don't check for allocation failure
(realloc returning NULL would also leak the old buffer); revisit if that path is
ever hardened. Behavior change: displayed FFT levels jump ~+9 dB with the
default Nuttall window (changelogged).

## Not worth it / already covered

- **KiwiSDR map browser** (~1,000 lines: geomap + earcut) — we already have an
  equivalent from the SDRPPBrown port (now the remote-receiver browser in `gui/remote/`, used by our
  `websdr_view` module). Skip.
- **SDL2 / OpenGL ES headless backend** (RPi KMS-DRM/Wayland) — significant platform
  work, irrelevant to our Windows/Android focus.
- **hpsdr_source** — we have hermes_source and our fork targets QMX; niche unless
  someone asks.
- **RTL-SDR "sample conversion fix"** — it's `-127.5` vs upstream's `-127.4` offset;
  negligible.
- **7-segment frequency font, SDR#/SpectraVue waterfall palettes, antialiasing
  setting** — cosmetics; the palettes are trivial config files if ever wanted.
- **Waterfall zoom / mouse-click-leak fixes** — real fixes, but entangled with their
  1.1.0-era waterfall which has diverged from our 1.2.x one; extracting them needs
  commit archaeology for uncertain payoff. Check per-fix whether upstream 1.2.x already
  fixed the same issues.
- **frequency_manager LO save** — we've modified frequency_manager ourselves; their
  change would conflict and the feature is minor.

## Suggested first step

Port the squelch + manual AGC pair — best value-to-effort in the list.
