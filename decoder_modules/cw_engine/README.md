# cw_engine — the CW skimmer engine, shared by the CW decoder modules

A static library linked into SDRIAK's CW decoder modules: the wideband
skimmer and the single-VFO reader. It holds the CW decode backends of
**skimmer-for-linux**: the semi-Markov Viterbi decoders v2 and v3, the
callsign validator, extractor and MASTER.SCP dictionary they use, and the
polyphase channelizer that splits a wideband IQ stream into the decoders'
250 Hz channels. Optionally (`OPT_BUILD_CW_DEEPCW`, below) it also holds
two neural decoders on e04's DeepCW models and ONNX Runtime: upstream's
DeepCW backend (v1 model) and a v2 branch beside it (`DeepCW.md`). Upstream's
classical v1 decoder (`decode_cw.c`) and its neural over re-reader
(`cw_reader.c`) are not part of SDRIAK.

## Origin

| | |
|---|---|
| Upstream | [ok1hra/skimmer-for-linux](https://github.com/ok1hra/skimmer-for-linux), the headless fork by Dan Pech, OK1HRA, of [OK1BR/skimmer-for-linux](https://github.com/OK1BR/skimmer-for-linux) by Richard Fakenberg, OK1BR |
| Pinned commit | `03beb5b330bd4f6e57667ada51cf6012817eab50` (release v0.8.0) |
| Licence | GPL-3.0-or-later, as upstream |

The decoders were tuned and measured by OK1HRA against CW Skimmer Server
(VE3NEA) on live air and on hours of recorded IQ. The measured record is
upstream's `docs/CW-V3.md`, `docs/LIVE-AIR.md` and `docs/CQWW2012.md`.

## Layout

The sources keep upstream's files, functions, names and layout, so an
upstream change can be followed file by file. They are **C++17 on the
standard library**: each upstream `.c` is a `.cpp` and GLib is replaced by
its standard counterparts (below). The one restructured file is the decoder:
its state and functions are split into stages (`engine/cw_*.h`), each
holding upstream's fields and function bodies; the map under "Following
upstream" says where each upstream function lives, and "Renamed" which
upstream names are spelled out here.

```
src/engine/    = upstream src/engine/ (the decoder subset, see below)
src/*_test.cpp = upstream src/*_test.c (the decoders' offline gates)
src/sdriak     = SDRIAK's own: what upstream takes from WDSP
```

| File | What |
|---|---|
| `engine/decode.h` | the backend interface: one complex baseband channel in, text + confidence + offset + WPM + SNR out |
| `engine/decode_cw_viterbi.cpp` | v2 and v3, one implementation (upstream: `decode_cw_v2.c` and its near-copy `decode_cw_v3.c`; the file is v3's). v2: soft-decision semi-Markov Viterbi (QSB, fist model, clock re-lock, pre-roll replay, narrow 45 Hz envelope, MASTER.SCP word re-read). v3: v2 verbatim for strong stations, plus the weak-signal view (carrier detector + AFC, coherent half-dit matched filter, calibrated LLR), the audio-confirmed call reader and three clock fixes. The two are presets of `CwDecoderConfig`. The file holds the channel (`CwState`: the stages below and the over-level state — pause, carrier, break, marks), the backend, and the orchestration: `process()` and `soft_step()` as sequences of per-sample steps, the commit's `on_segment()`, emission, the pre-roll replay |
| `engine/decode_cw.h` | the v2 and v3 backends: `skim_decode_cw_v2_new(cfg, dict)` / `_v3_new(cfg, dict)` |
| `engine/cw_envelope.h` | SDRIAK's: `CwEnvelope`, the decoder's envelope front end — per sample \|IQ\|, its 3-tap average and the soft path's envelope (the carrier-centred narrow one with `SKIM_Vn_NARROW`) |
| `engine/cw_morse.h`, `engine/cw_morse.cpp` | SDRIAK's: `CwMorse` (the Morse table: code → character, character → pattern, the nearest-character fallback) and `CwAssembler` (the character in flight: push an element, take the character with the lone-E/T rule) |
| `engine/cw_levels.h` | the decoder's stage `CwLevels`: v1's level trackers, envelope squelch and keying-duty test |
| `engine/cw_schmitt.h` | `CwSchmitt` (v1's key detector with the glitch fold; the dit bootstrap's and the run dump's) and `CwBootstrap` (the first-marks dit clustering) |
| `engine/cw_clock.h` | `CwClock`: the dit clock, its per-mark EMA, the element-space EMA and the speed-change re-lock |
| `engine/cw_fist.h` | `CwFist`: the operator's learned character/word gap centres |
| `engine/cw_tone.h` | `CwTone`: the tone offset from the phase slope |
| `engine/cw_lattice.h` | `CwSegmentPriors` (duration priors and search windows) and `CwLattice` (the semi-Markov Viterbi: llr ring, step, best head, traceback commit with an `on_segment` callback) |
| `engine/cw_discriminator.h` | `CwDiscriminator`: v2's µ_mark/µ_space trackers, the span + Rayleigh llr, the edge smear and the weak-edge switch |
| `engine/cw_live.h` | `CwLive`: live emission, the char/word thresholds on the fist centres |
| `engine/cw_frontend.h` | `CwFrontEnd`: v3's front end, of five parts — `CwIqRing` (the raw I/Q ring), `CwCarrierDetector` (spectrum, floor, candidate state, AFC), `CwMatchedFilter` (derotator, low-pass, half-dit coherent sum), `CwPowerStats` (σ0², σ1², presence, the calibrated llr and SNR500), `CwMatchedLevels` (the matched-amplitude trackers) |
| `engine/cw_preroll.h` | `CwPreroll`: the pre-roll rings and the search for the over's head |
| `engine/cw_align.h`, `engine/cw_align.cpp` | forced alignment of calls: `call_segments()`, `CwCallAlignment` (the keyword search's duration tables, free reading and call reading); `CwReadView`, what the readers see of the decoder |
| `engine/cw_reread.h`, `engine/cw_reread.cpp` | `CwWordReread`: the word in flight and its MASTER.SCP re-read (`SKIM_V3_RESCORE`), the oracle's combine |
| `engine/cw_callreader.h`, `engine/cw_callreader.cpp` | `CwCallReader` (v3's audio-confirmed call reader) and `CwKwsProbe` (the oracle's kws probe) |
| `engine/cw_rundump.h` | `CwRunDumpSink`, the run dump's two files (`SKIM_CW_DUMP_RUNS`), and `CwRunDump`, a channel's keyer on it |
| `engine/cw_debug.h` | `CwTrace`, a channel's debug trace (`SKIM_CWn_DEBUG`), and `CW_DBG(trace, …)` |
| `engine/callsign.cpp` | callsign validation, the per-channel extractor (`SkimCallsignConfig`: its switches), the MASTER.SCP dictionary (`SkimCallsignDict`) and its near-call index, `SkimCallsignDictSource` (the slot a dictionary is published into) |
| `engine/channelizer.cpp` | 2×-oversampled polyphase filter bank: M = in_rate / spacing complex channels at 2 × spacing (125 Hz spacing → 250 Hz, the CW geometry), fftw3f |
| `engine/oracle.cpp` | `SkimOracle`, measurement only (`SkimOracle::from_env()`, null unless `SKIM_ORACLE` is set); kept because v2/v3 call into it |
| `engine/skim_call.h` | `SkimCall`, a callsign as a value: 15 characters inline, no heap, compared with `==` |
| `engine/skim_common.h` | SDRIAK's: the standard headers the engine shares (in place of `<glib.h>`), `SKIM_PI`, `skim_strlcpy`/`skim_strlcat` |
| `engine/decode_cw_config.h` | SDRIAK's: `CwDecoderConfig`, the decoders' tunables, and `cw_config_from_env()` |
| `engine/decode_deepcw.h`, `engine/decode_deepcw.cpp` | optional (`OPT_BUILD_CW_DEEPCW`): the DeepCW backend `SkimDeepcwBackend` ("deepcw"): per channel a 20-point Hann DFT to the model's 12.5 Hz bins, a 10 s ring, the keyed-line gate, the peak-normalised 65-bin tile, Conformer + CTC inference inline or batched on worker threads, the sliding-window commit rule with the gray draft (`DeepcwCommitRule`), the TX-hold flush; `DeepcwConfig` (upstream's `SKIM_DEEPCW_*`, `DeepcwConfig::from_env()`) |
| `engine/decode_deepcw2.h`, `engine/decode_deepcw2.cpp` | optional, SDRIAK's: the DeepCW v2 backend `SkimDeepcw2Backend` ("deepcw2", narrow "deepcw2n"): the v2 contract (log1p over a noise level, output stride 2, the 65-bin and the 3-bin model) on the v1 backend's front end, streamed by e04's posterior stitcher (`DeepcwStitcher`, ported from MIT code); the wide tile's noise level is the populated bins' median (`noise_mode`; upstream's 90th percentile fails on a channel tile under fading); `model_v1` runs the v1 model through the same stitcher, for comparison; `DeepcwV2Config` (`SKIM_DEEPCW2_*`) |
| `engine/deepcw_runtime.h`, `engine/deepcw_runtime.cpp` | optional, SDRIAK's: `DeepcwRuntime`, one model contract (`DeepcwModelSpec`: v1, v2, v2 narrow) behind ONNX Runtime — the session with its sha256 pin and output-shape check, the inference workers batching equal-length windows |
| `engine/skim_sha256.h`, `engine/skim_sha256.cpp` | optional, SDRIAK's: `SkimSha256`, SHA-256 for the model pin |
| `engine/ort_shim.h`, `engine/ort_shim.cpp` | optional: ONNX Runtime's C API loaded at run time (`SkimOrt`, the library and environment; `SkimOrtSession`, one model on the CPU or CUDA) |
| `../vendor/onnxruntime/` | optional: the MIT C API header of ONNX Runtime 1.21 (`VENDOR.md`) |

The decoders expect **250 Hz complex baseband** with the tone within the
±62.5 Hz channel, before any AGC: the output of a 125 Hz-spaced channelizer
channel, or a VFO of that rate.

The decoders' tunables are a `CwDecoderConfig` (`engine/decode_cw_config.h`):
one field per upstream `SKIM_V2_*` / `SKIM_V3_*` / `SKIM_CW2_*` / `SKIM_CW3_*`
switch, with upstream's defaults, grouped by consumer: `diag` (debug and llr
traces, run dump, kws dump), `envelope`, `assembly`, `lattice`, `clock`, `llr`
(v2's llr views), `rescore` (the word re-read), and under `v3` the v3-only
`robust_clock`, `front` (the front end), `weak` (the decoder in the weak view)
and `reader` (the call reader). Each stage takes its own part
(`CwDiscriminator::llr(…, cfg.llr)`, `CwFrontEnd::init(…, cfg.v3.front, …)`). A backend owns a config and each channel
takes a copy, deriving at its own sample rate what depends on it (the
narrow envelope's biquad). `cw_config_from_env()` applies upstream's
variables to a preset, parsed as upstream parses them, so every switch
still works as upstream documents it.

Nothing in the engine reads state from hidden memory; whoever owns the
decoders creates what they read and hands it to them:

- the known-call dictionary: a `SkimCallsignDictSource` the application owns
  and publishes `SkimCallsignDict::load()` tables into (any thread, any time),
  given to each backend (`skim_decode_cw_v3_new(cfg, dict)`) and extractor
  (`SkimCallsignExtractor(dict, cfg)`); a channel takes one snapshot per
  `process()` call, an extractor one per `feed()` / `best()` / `top()`;
- the oracle: `SkimOracle::from_env()`, once, into `CwDecoderConfig::diag.oracle`;
- the run dump: `CwRunDumpSink::from_env()`, once, into `diag.dump` (one sink
  per path: two would write over each other);
- the extractor's switches: a `SkimCallsignConfig` (`SkimCallsignConfig::from_env()`
  for upstream's `SKIM_CALL_*`);
- the debug trace: each channel's `CwTrace`, handed to the stages that print.

Not here yet: the band SNR meter, the pipeline (ghost arbitration,
frequency lock, station table) and the spot feed. Those belong to the
skimmer module.

## DeepCW (optional)

`OPT_BUILD_CW_DEEPCW` (default OFF, needs `OPT_BUILD_CW_ENGINE`) compiles
the DeepCW backends into the library and defines `CW_ENGINE_DEEPCW` for
their consumers. With the option off nothing of them, and nothing of ONNX
Runtime, is compiled. `DeepCW.md` has the comparison with the upstreams
(e04's two repositories, OK1BR, OK1HRA), the model contracts and what is
not measured yet.

- **Backends.** `SkimDeepcwBackend` ("deepcw") reads the v1 model with
  OK1BR/OK1HRA's commit rule; `SkimDeepcw2Backend` ("deepcw2" / "deepcw2n")
  reads the v2 wide or narrow model with e04's stitcher. They share the
  front end and the runtime, so they can run side by side on one channel.
- **How they compare** (`DeepCW.md`, "Synthetic bench"): in noise the three
  models are equal and 2.5–5 dB better than v3; under fast fading and
  multipath v1 is clearly the best (2–4× fewer errors than v2 at 0–10 dB),
  and the v1 model reads as well through e04's stitcher as through its own
  commit rule. On the CPU v2 narrow costs ~1/17 of v1.
- **Runtime.** ONNX Runtime is never linked. `engine/ort_shim.cpp` loads
  it when the first channel is made (`LoadLibrary` / `dlopen`) and asks for
  API version 21 (ONNX Runtime ≥ 1.21): the config's `ort_library`, else
  `onnxruntime.dll` (Windows: the executable's directory first);
  `libonnxruntime.so.1`, `.so.1.21`, `.so` (Linux, Android);
  `libonnxruntime.dylib`, `.1.dylib` and Homebrew's prefixes (macOS). One
  environment serves every backend. The build needs only the vendored
  header. `device = "cuda"` appends the CUDA execution provider and falls
  back to the CPU, saying why, when it cannot.
- **Models**, at the config's `model_path`, each checked against its sha256
  before it is loaded (`deepcw_runtime.h`; `verify = false` /
  `SKIM_DEEPCW_VERIFY=0` / `SKIM_DEEPCW2_VERIFY=0` skips it). None is
  bundled; SDRIAK has no fetcher (upstream's `src/headless/deepcw_model.c`
  is not taken).

  | Backend | File | sha256 | Licence |
  |---|---|---|---|
  | deepcw | `model.onnx` at deepcw-engine `8e264d2` | `ef120799…fe02` | AGPL-3.0-only |
  | deepcw2 | `model.onnx` at deepcw-engine `79104fb` | `cd5ee381…73ae` | MIT (e04) |
  | deepcw2n | `model_narrow.onnx` at deepcw-engine `79104fb` | `fc80c4ba…46e5` | MIT (e04) |

  A file of another contract is refused and named, and every run's output
  shape is checked against the contract (v2's output has half the frames).
- **Unavailable** (no runtime, no model, wrong model): `available()` says
  why; channels still run the DSP half (level, offset, gate) and decode
  nothing.
- **SNR.** Both report `snr_db` as SNR in 500 Hz (`snr_in_500hz()`), OK1BR's
  gh#27 (`4db0483`), with the band's noise through `noise()` /
  `set_band_noise()` (decode.h).
- **State.** A backend owns what upstream keeps process-wide: the tunables,
  the runtime (`reset()`, `set_device()`), the worker pool and the tail
  dump. Channels share it; a channel destroyed with a window at the workers
  leaves the result to be dropped (upstream refcounts the channel state
  instead).

The measured record behind the v1 design (evaluation, cost, the commit
rule, the draft, word gaps, the TX-hold flush) is OK1BR's `docs/DEEPCW.md`.

## What upstream takes from elsewhere

| Upstream | In SDRIAK | |
|---|---|---|
| WDSP `fir_bandpass()` (vendor/wdsp), the channelizer's prototype low-pass | `sdriak/fir_bandpass.cpp` | WDSP's windowed sinc on `dsp::window::blackmanHarris(i, N − 1)`, which is WDSP's 4-term Blackman-Harris term for term (the N − 1 period, not `dsp::taps::windowedSinc`'s N, is what keeps the taps upstream's). Window 0 (BH4) only, real or complex taps, returned as a `std::vector<double>` (WDSP: a `malloc`'d block the caller `free()`s), declared in `sdriak/fir_bandpass.h`; no impulse cache |
| `fftw_lock.c` (a private mutex) | `std::lock_guard` on core's `dsp::buffer::fftwMutex()` in `engine/channelizer.cpp` | engine planning is serialised against core and every other module |

## GLib → the standard library

| Upstream (GLib) | Here |
|---|---|
| `gboolean`/`TRUE`/`FALSE`, `guint`, `guint64`, `gsize`, `gpointer` … | `bool`/`true`/`false`, `unsigned`, `uint64_t`, `size_t`, `void *` … |
| `MIN` / `MAX` / `CLAMP` / `ABS`, `G_N_ELEMENTS`, `G_PI` | `std::min` / `std::max` / `std::clamp` / `std::abs`, `std::size`, `SKIM_PI` (every `std::clamp` has `lo ≤ hi`, checked site by site) |
| `g_new0(State, 1)` / `g_free` of decoder, extractor and channelizer state | `new State()` (value-initialised: zeroed as before) / `delete` |
| state buffers (`g_new0(float, n)`), the lattice | `std::vector<float>`, `std::unique_ptr<Lattice>` |
| scratch buffers in the word re-read and the call reader | `std::vector`, freed by scope |
| `g_once_init_enter`/`leave` | `std::call_once` |
| `GString` | `std::string` |
| dictionary: `GHashTable` set, deletion index, `GPtrArray`s under a `GRWLock` | one immutable `SkimCallsignDict` per load (`std::unordered_set<std::string>` of calls, an `std::unordered_map` deletion index of `const char *` into it, sorted `std::vector<std::string>`s), published into a `SkimCallsignDictSource` as `std::shared_ptr<const SkimCallsignDict>` with `std::atomic_store` and read with `std::atomic_load`: a reload never blocks a lookup, and the old table dies with its last reader |
| `skim_callsign_dict_load(path, GError **)` | `SkimCallsignDict::load(path, std::string *error = nullptr)` (UTF-8 path) → a table, `nullptr` on failure; the owner publishes it |
| `char *skim_callsign_dict_release()` (caller frees) | `const char *SkimCallsignDict::release() const` |
| `skim_callsign_dict_has()` / `_near()` / `_size()` | `SkimCallsignDict::has()` / `near()` / `size()` |
| `g_getenv`, `g_ascii_strtod`, `g_ascii_formatd`, `g_printerr`, `g_error` | `std::getenv`, `std::strtod`, `std::snprintf`, `std::fprintf(stderr, …)`, `fprintf` + `std::abort` — the locale-independent GLib forms are plain C here because SDRIAK never changes the C locale |
| `g_ascii_isdigit` … `toupper` | `std::isdigit` … `std::toupper` on `unsigned char` (the `"C"` locale) |
| tests: `GRand` | `std::mt19937` — the same MT19937 with the same seeding — and GLib's `g_rand_double` / `g_rand_int_range` draws on it, so every synthetic signal is upstream's sample for sample |
| tests: `GArray`, `GThread`, `g_atomic_*`, temp files | `std::vector<float>`, `std::thread`, `std::atomic<int>`, `std::filesystem::temp_directory_path` |

The near-call index is built in `unordered_set` iteration order (GLib's
hash order upstream; unspecified in both). It only orders candidates that
the re-read then scores, so it matters only when more than 24 calls lie
within two edits of a word.

## C++ style

Across all sources, upstream's C spellings are C++ ones:

| Upstream | Here |
|---|---|
| `typedef struct {…} X;`, `typedef enum {…} X;` | `struct X {…};`, `enum X {…};` |
| `#define NAME value` constants | `constexpr auto NAME = value;` — `auto` keeps the literal's type (`int`, `unsigned`, `double`, `float`, `const char *`, `char`); every expression value was parenthesised, so no use changes meaning |
| `NULL`, `f(void)`, C-style casts | `nullptr`, `f()`, `static_cast` / `reinterpret_cast` / `const_cast` |
| `<math.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>` | `<cmath>`, `<cstdio>`, `<cstdlib>`, `<cstring>` |
| `qsort` with a comparator | `std::sort` |
| hand-written insertion sorts, min/max and sum loops, pointer and value swaps, duplicate searches, `memcpy` / `memmove` of arrays | `std::sort`, `std::minmax_element`, `std::accumulate`, `std::swap`, `std::any_of`, `std::copy` / `std::copy_n` |
| `memset` of a record or of filter state | `= {}` / `std::fill_n` |
| a table or file set up once under a `std::once_flag` | a function-local `static const` (thread-safe one-time initialisation since C++11) |
| `fe_select()` (quickselect) | `std::nth_element` |
| `(void)x;` for an unused parameter | an unnamed parameter, `[[maybe_unused]]` |
| a state struct and its free functions `f(S *s, …)` | a class with `f` as its method (`s->` dropped); data private where nothing outside reads it, read through `const` getters where something does |
| file-scope constants private to one stage (`FIST_*`) | `static constexpr` members named for their meaning (`kMinGapDits`) |

Naming: private and protected data members take an `m_` prefix (`m_char_space`);
constants that are class members are `kName`.

Left as a macro: `CW_DBG` (a variadic `printf` wrapper). Headers use `#pragma
once`, as the rest of SDR++ does. Resources are owned: the channelizer's FFTW
buffers and plan are `std::unique_ptr`s with FFTW deleters, and the planner
lock is a `std::lock_guard`.

## Other local edits

Each is marked `SDRIAK:` in the source.

| File | Edit | Why |
|---|---|---|
| `engine/callsign.cpp` | an explicit cast where C converts `memchr`'s `void *` implicitly | C++ has no implicit `void*` conversion |
| `engine/cw_lattice.h` | the debug trace's `(const char *[]){…}[ty]` is `SEG_NAMES[ty]` | compound literals are not C++ |
| `engine/decode.h` | the interface is two classes (upstream: one struct of function pointers taking an opaque `void *state`): `SkimDecodeBackend` (`name()`, `channel_new()`, `implements(SkimDecodeHook)`) and `SkimDecodeChannel`, the per-channel decoder `channel_new()` returns as a `std::unique_ptr`, with `process()` and the optional hooks as virtual methods (neutral defaults; `channel_free` is its destructor). `take_aux_text()` returns a `std::string` and `SkimPaneOp` holds `std::string`s ("" = none) where upstream hands out `malloc`'d `char *` | idiomatic C++, no manual ownership |
| `engine/decode_cw_viterbi.cpp` | **v2 and v3 merged**: upstream's `decode_cw_v3.c` with `Cw3State`/`Cw3Backend`/`CW3_DBG` named `CwState`/`CwBackend`/`CW_DBG`, and its three unconditional clock fixes (no lattice without a clock, the stall watchdog, the carrier cleared with the fist) behind `CwDecoderConfig::v3.robust_clock`; the pre-roll's 512-sample cap applies with the front end only. Every other v3 change already sits behind `fe_on` / `WEAKV()`, so the v2 preset (front end, call reader, robust clock off) runs exactly upstream v2's code paths. `decode_cw_v2.c` itself is not taken | one decoder, two presets |
| `engine/decode_cw_viterbi.cpp`, `engine/cw_*.h`, `engine/cw_*.cpp` | **the decoder split into stages**. Each stage is a class holding a group of upstream's state fields (private, `m_` + upstream's name, or a name for what it holds: "Renamed" below) with upstream's function bodies as its methods; the decoder reads a stage through `const` getters (`clock.dit()`, `levels.env_hi()`, `lattice.active()`, …) and changes it through methods named for the intent (`clock.impose()` — the oracle's clock, `clock.forget_dit()`, `levels.force_open()`, `preroll.fence()`, `reader.due()`, …); a result the caller used to read back from fields comes back from the call (`CwSchmitt::step()` hands out the confirmed run, `CwBootstrap::learn()` returns the dit); arithmetic and order of operations are unchanged, and what a stage reads of the rest of the decoder comes in as arguments (`CwClock &`, `CwSegmentPriors`, `CwReadView`, the pre-roll ring) or is copied at `init()`. Blocks of `process()` and `soft_step()` are step methods of the decoder (`clock_watchdog`, `front_end_step`, `gate_step`, `dead_air_now`, `dead_air`, `tone_step`, `bootstrap_step`; `carrier_watch`, `live_emit`), and `lattice_commit()`'s per-segment body is `on_segment()`. `lat_t0` is `CwLattice::t0`; the unused `cont_space` is dropped. The gates' output is byte-identical to the unsplit decoder | stages a module or a test can use on their own, a decoder that reads as its algorithm |
| `engine/decode_cw_viterbi.cpp` | `CwState` (upstream `Cw2State` / `Cw3State`) derives from `SkimDecodeChannel`: upstream's `cw3_process`, `cw3_level`, … are its methods (`CwState::process` …), and so are upstream's static functions on the state (`soft_step`, `emit`, `lattice_start`, …: private methods, `st->` dropped); upstream's `cw3_new` body is its constructor, so `channel_new()` is a `make_unique`. Its own fields are private, `m_` + upstream's name (`st->t` is `m_t`). The `SkimDecode *out, unsigned *pos` pair they thread is a `CwText` cursor (`put()`, `fits()`; upstream's `emit_str` is `CwText::put`), and `WEAKV(st)` is `weak_view()`. `CwBackend` keeps `name()`, `implements()` and `channel_new()` | the channel is an object |
| `engine/cw_frontend.h` | `noise_refresh()` (upstream `fe_noise_refresh()`): the VLA `float v[st->pw_size]` is a `std::vector` | C++ has no VLAs |
| `engine/decode_cw_viterbi.cpp` | the neural reader removed: its `#include`, state fields, `RrRun`, `rr_reader()` and the `rr_*`/`hy_*`/`ov_*` pane-composition functions, their calls, `take_pane_op`; `emit()` keeps no pane draft; the run keyer runs for the run dump (`SKIM_CW_DUMP_RUNS`) only | `cw_reader.c` is not part of SDRIAK. With `SKIM_CW_READER` unset upstream ran none of it, so decoding is unchanged |
| `engine/decode_cw.h` | `skim_decode_cw()` (v1) removed | v1 is not part of SDRIAK |
| `engine/callsign.h`, `engine/callsign.cpp`, the gates | `SkimCallsignExtractor` is a class: `skim_callsign_extractor_new`/`_free` are its constructor/destructor (`new`/`delete`), `_reset`, `_feed`, `_best`/`_best_ex` (one `best()`, below), `_hearings`, `_addressed`, `_top`, `_set_now`, `_last_heard` its methods of the same names, defined where upstream has the functions, bodies unchanged . Its data is upstream's struct renamed `SkimCallsignExtractorState`, held by a `std::unique_ptr` (pimpl); upstream's static helpers on it (`run_clear`, `cand_add`, `run_close`, `take_token`, `best_pick`, …) are its methods (`x->` dropped), defined where upstream has them. Likewise `cand_calling(c)` is `Cand::calling()`, `near_add`/`near_del`/`near_ext` are `Near::add`/`del`/`ext`, and `dict_index()` is `SkimCallsignDict::build_index()` (its local `del` is `key`: the dictionary's map is `del` too); `skim_callsign_extract()` is the static `SkimCallsignExtractor::extract()` | the extractor is a class |
| `engine/channelizer.h`, `engine/channelizer.cpp`, `cw_test.cpp` | `SkimChannelizer` is a class: `skim_channelizer_new`/`_new_ex` are the static factories `create`/`create_ex` (a `std::unique_ptr`, empty on invalid geometry where upstream returned NULL), `_free` the destructor, `_count`, `_out_rate`, `_offset_hz`, `_push`, `_read`, `_dropped` its methods, defined where upstream has the functions, bodies unchanged (the one-line `create` inline in the class). Its data is upstream's struct renamed `SkimChannelizerState` (pimpl), and `hop()` is its method (its `M`/`P` locals, copies of the members, dropped) | the channelizer is a class |
| `engine/decode_cw_viterbi.cpp`, `cw_test.cpp` | every switch read (`std::getenv` into a function-local static at first use) reads `st->cfg`; the getters keep their names and take `st`; the narrow biquad and the edge-weak ratios are computed per channel at `channel_new`; the backends own a config (`Cw2Backend(cfg)`), and `QSB_TRACE` in the gate runs a traced twin backend instead of setting `SKIM_CW2_TRACE` | tunables are data, set per backend; no process-wide caches |
| `cw_test.cpp` | v1's runs, prints and two v1 checks removed; where v1 ran ahead of v2 on the shared generator, `rng_burn()` takes the same draws | v2 and v3 still see exactly upstream's noise, so upstream's pass thresholds hold |
| `engine/decode_deepcw.cpp` | `DcwState` is the channel class `DeepcwChannel` (upstream's `dcw_*` functions on it are its methods, fields `m_` + upstream's name); `DcwTune` is `DeepcwConfig`, `skim_decode_deepcw_*()` are `SkimDeepcwBackend`'s methods, `skim_deepcw_commit`/`_ex` are `DeepcwCommitRule::commit`/`commit_ex` (the tunables they read are the rule's, the tail dump's label `g_dump_hz` a parameter); the session refcount is a `std::shared_ptr`; the job queue is a `std::deque` under a condition variable; jobs deliver into a per-channel mailbox object | no process-wide state; a backend per configuration |
| `engine/deepcw_runtime.cpp` (upstream's session and workers, out of `decode_deepcw.c`) | a worker whose model run fails, or returns an unexpected shape, delivers an empty result to every channel of the batch | upstream delivers nothing, and those channels wait for their window for good (one job in flight per channel) |
| `engine/deepcw_runtime.cpp` | the model file's sha256 is checked before every load; every run's output shape must be the contract's | upstream checks the file only when its fetcher installs it, and takes `min(T, od[1])` frames: a v2 model would be read as v1 without an error |
| `engine/deepcw_runtime.cpp` | a worker batches only windows of one length (upstream: zero-padded to the longest) | the model's GroupNorm and unmasked attention see the padding, so a short window read differently batched than alone |
| `engine/decode_deepcw.cpp`, `engine/decode.h` | OK1BR `4db0483` (gh#27, after OK1HRA forked): SNR in 500 Hz, `noise()` / `set_band_noise()` / `set_noise_bw()`, `snr_in_500hz()`; the band floor EMA removed; marked `OK1BR 4db0483` | OK1HRA's SNR read ~8 dB high |
| `engine/decode_deepcw.cpp` | no default model path (upstream: `~/.local/share/skimmer-for-linux/models/deepcw/model.onnx`) | the application chooses where the model lives |
| `engine/ort_shim.cpp` | `LoadLibrary` on Windows (the model path widened to UTF-16, `ORTCHAR_T`), the macOS library names; the cuDNN preload on Linux only | upstream is Linux only |
| `deepcw_test.cpp` | the two checks through the whole offline pipeline (pane routing, the TX hold's station report) removed | the pipeline is not part of the engine |

Not taken from upstream: `decode_cw.c` (v1), `cw_reader.c` and `.h` (the
neural re-reader), `fftw_lock.c` (a private GMutex; the channelizer
locks core's mutex instead) and `headless/deepcw_model.c` (the model
fetcher: GLib + libcurl).

## Following upstream

The files map one to one, but upstream's diffs speak GLib, so they are
ported by hand rather than applied:

```sh
# in the upstream clone: what changed since the pinned commit
git diff 03beb5b <new> -- src/engine/decode.h src/engine/decode_cw.h \
    src/engine/decode_cw_v2.c src/engine/decode_cw_v3.c \
    src/engine/callsign.[ch] src/engine/oracle.[ch] \
    src/engine/channelizer.[ch] src/engine/fftw_lock.[ch] \
    src/engine/decode_deepcw.[ch] src/engine/ort_shim.[ch] \
    src/cw_test.c src/call_test.c src/deepcw_test.c
# in an e04/deepcw-engine clone (decode_deepcw2's contract and stitcher):
git diff 79104fb <new> -- model.onnx.json model_narrow.onnx.json \
    examples/python/decode_morse.py
```

Each hunk lands in the same function of the `.cpp` of the same name,
written with the table above; both `decode_cw_v2.c` and `decode_cw_v3.c`
land in the decoder, in the stage that holds the function:

| Upstream (`decode_cw_v3.c`) | SDRIAK |
|---|---|
| `cw3_process`, `cw3_new` (the constructor), `soft_step`, `preroll_replay` (the replay), `lattice_start`, `lattice_flush`, `emit*`, `word_close` (emission) | `decode_cw_viterbi.cpp`: `CwState`'s methods (`process()` blocks are its step methods) |
| `rd_*`, the run keyer | `cw_rundump.h`: `CwRunDumpSink` (the files), `CwRunDump` (the keyer) |
| `skim_decode_cw_v2()` / `_v3()` (singletons) | not taken: `skim_decode_cw_vN_new(cw_config_from_env(cw_preset_vN(), N), dict)` |
| `skim_oracle_modes()` / `_time()` / `_at()` / `_decoys()` | `SkimOracle::modes()` / `time()` / `at()` / `decoys()` |
| out-pointer results: `skim_oracle_at(…, &hit)`, `skim_callsign_extractor_best(x, out, n)` / `_best_ex(x, out, n, &cq)`, `_top(x, &cand)`, `skim_callsign_dict_inspect(data, len, &info)`, `held_pair(&hz)`, the pause predicate's `&solid` | return values: `std::optional<SkimOracleHit> at()`, `SkimCallsignBest best()` (call, score, `cq_context`), `std::optional<SkimCallsignCand> top()`, `SkimCallsignDictInfo SkimCallsignDict::inspect(std::string_view)`, `std::optional<double> held_pair()`, `CwState::DeadAir dead_air_now()`; byte ranges are `std::string_view` (`SkimCallsignDict::parse`); `take_pane_op(SkimPaneOp &)` keeps an out-parameter (by reference) so a reused op keeps its string buffers |
| C API constants and free functions: `SKIM_CALLSIGN_SPOT_THRESHOLD`, `SKIM_CALLSIGN_NEAR_LEN`, `SKIM_CALLSIGN_FIX_OPEN`/`_CLOSE`, `skim_callsign_is_valid()`, `skim_callsign_strip_fix()`, `skim_callsign_dict_inspect()`, `skim_callsign_extract()`, `SKIM_DECODE_TEXT_MAX`, `SKIM_PANE_OP_*`, `SKIM_ORACLE_GATE` … `_KWS` | members of their classes: `SkimCallsignExtractor::kSpotThreshold`, `SkimCall::kCapacity`, `SkimCallsign::kFixOpen`/`kFixClose`, `SkimCallsign::is_valid()`, `SkimCallsign::strip_fix()`, `SkimCallsignDict::inspect()`, `SkimCallsignExtractor::extract()` (returns a `SkimCallsignBest`), `SkimDecode::kTextMax`, `enum class SkimPaneOpKind { Append, Open, Set, Close }`, `SkimOracle::kGate`, `kClock`, `kFreq`, `kGhost`, `kSoftGate` (gatesoft), `kCombine`, `kKws`; `skim_callsign_config_from_env()` is `SkimCallsignConfig::from_env()` |
| `char call[16]` / `char (*out)[SKIM_CALLSIGN_NEAR_LEN]` buffers with `strcmp()`/`g_strlcpy()`: the extractor's candidates, `tu_call`/`prev_tok`/`run_hit`, `skim_callsign_dict_near()`'s output, the oracle's stations and decoys, the call reader's candidates, tokens and hit memory, the word re-read's word and its correction | `SkimCall` (truncating `assign()` as `g_strlcpy` did, `push_back()`, `==`, `c_str()`, `view()`); `near()` fills a `SkimCall *`; scratch buffers that build or join text stay `char` arrays |
| `cw_dbg()` / `CW3_DBG(…)` | `CwTrace::on()` / `CW_DBG(trace, …)` |
| `xk_tu_strict()`, `cqwords_on()`, `fused_on()`, `glue_on()`, `despace_on()`, `tu_mode`, `exch_on` | `SkimCallsignConfig::tu_strict`, `cqwords`, `fused`, `glue`, `despace`, `tu`, `exch` |
| `lattice_commit` | `CwLattice::commit` (traceback) + `on_segment()` (the per-segment switch) |
| `lattice_step`, `lattice_head`, `pen_sync`, `dur_pen_memo`, `seg_ideal`, `dur_pen`, `seg_window` | `cw_lattice.h`: `CwLattice::step`, `best_head`, `sync_prior_memo`, `duration_prior`; `CwSegmentPriors::ideal_length`, `duration_prior`, `search_window` |
| `clock_push`, `clock_space`, `clk_est`, the DIT/DAH/ESP clock updates | `cw_clock.h` |
| `fist_learn` | `cw_fist.h` |
| the level-tracker block, the bootstrap block, the run keyer, the tone block | `cw_levels.h`, `cw_schmitt.h`, `cw_tone.h` |
| the µ trackers, the llr views, the edge-weak switch | `cw_discriminator.h` |
| the live-emission thresholds | `cw_live.h` |
| `fe_hop` | `cw_frontend.h`: `CwCarrierDetector::hop` (+ `CwFrontEnd::hop`, the backfill and view at birth) |
| `fe_sample` | `CwMatchedFilter::power` (derotate, low-pass, sum) + `CwPowerStats::update` (σ1², presence, σ0² refresh) |
| `fe_noise_refresh`, `fe_llr`, `fe_snr500` | `CwPowerStats` |
| `fe_track`, `fe_backfill`, `fe_fft`, `fe_select` | `CwMatchedLevels::track`, `CwFrontEnd::backfill`, `CwCarrierDetector::fft`, `std::nth_element` (inline at its two uses) |
| the weak/strong view switch in `cw3_process` | `CwFrontEnd::update_view` |
| `preroll_replay` (the onset search) | `cw_preroll.h` |
| `rs_*`, `oc_*`, `word_close` (the re-read) | `cw_reread.cpp` |
| `kws_read`, `kr_*`, `kws_eval`, `kws_push` | `cw_callreader.cpp` |
| `rs_segments` | `cw_align.cpp`: `call_segments` |
| `KwsDur` with `kws_dur`, `kws_dur_free`, `kws_free`, `kws_call` | `cw_align.cpp`: the class `CwCallAlignment` (its constructor, the vectors' destructor, `free_reading()`, `align_call()`) |

Upstream's constants are `static constexpr` members of the class that uses
them, named for their meaning (the public interface's `SKIM_*` names are
upstream's):

| Upstream | SDRIAK |
|---|---|
| `LAG_DITS`, `LAG_DITS_FAST`, `BREAK_DITS`, `BREAK_MARK`, `STALL_S` | `CwState::kCommitLagDits`, `kFastCommitLagDits`, `kOverBreakDits`, `kOverBreakMark`, `kStallSeconds` |
| `CLK_RING`, `CLK_HOLD` | `CwClock::kRelockRing`, `kRelockHold` |
| `FIST_*` | `CwFist::k*` (mapped in `cw_fist.h`) |
| `LLR_K`, `LLR_MAX` | `CwDiscriminator::kLlrGain`, `kLlrMax` |
| `W`, `WMASK`, `NEG_INF` | `CwLatticeRings::kSize`, `kMask` (aliased in `CwLattice`), `CwLattice::kNoScore` |
| `SEG_NAMES`, `SEG_IDEAL`, `SEG_ALPHA`, `SEG_COST` | `CwSegmentPriors::kSegment[]` (`.name`, `.ideal_dits`, `.prior_weight`, one row per type), `kSegmentCost` |
| `FE_*` | the parts of `cw_frontend.h` (mapped there); `FE_RING_S` is `CwFrontEnd::kRingSeconds` |
| `RS_MAX_SEG`, `RS_MAX_CAND`, `KWS_SKIP` | `MAX_CALL_SEGMENTS`, `MAX_NEAR_CALLS`, `CwCallAlignment::kUnreachable` |
| `RS_LEAD_DITS` | `CwWordReread::kLeadDits` |
| `KR_TEXT`, `KR_HITS`, `KR_WIN_S`, `KR_EVERY_S`, `KR_SLOTS`, `KR_CANDS`, `KR_NEAR`, `KR_HOLD_S`, `KR_SAY_S`, `KR_CQ_S` | `CwCallReader::kTextChars`, `kHitSlots`, `kWindowSeconds`, `kEverySeconds`, `kSlotsPerDit`, `kMaxCandidates`, `kMaxNearControls`, `kHoldSeconds`, `kRepeatSeconds`, `kCqSeconds` |
| `KWS_WIN_S`, `KWS_HOP_S`, `KWS_NEAR`, `KWS_DECOYS` | `CwKwsProbe::kWindowSeconds`, `kHopSeconds`, `kNearCalls`, `kDecoys` |
| `MAX_CAND`, `CQ_WINDOW`, `RUN_MAX`, `RUN_PARTS`, `CAND_STALE_TOKENS`, `FUSED_MAX`, `OVER_MAX` | `SkimCallsignExtractorState::kMaxCandidates`, `kCqWindowTokens`, `kMaxRunFragments`, `kMaxTornParts`, `kStaleAfterTokens`, `kMaxFusedLength`, `kMaxOverBytes` |
| `SINGLE_LETTER`, `CALL_NEAR_MAX`, `CALL_MAX`, `TOK_MAX` | `ITU_SINGLE_LETTER_SERIES`, `NEAR_QUERY_BUF_LEN`, `CALL_BUF_LEN`, `TOKEN_BUF_LEN` (file scope in `callsign.cpp`) |
| `TAPS_PER_BRANCH`, `RING_CAP` | `SkimChannelizerState::kTapsPerBranch`, `kRingFrames` |
| the segment types `SEG_DIT`, `SEG_DAH`, `SEG_ESP`, `SEG_CSP`, `SEG_WSP` | `enum class CwSeg : uint8_t { Dit, Dah, ElementSpace, CharSpace, WordSpace }` (`cw_lattice.h`; the debug trace keeps upstream's `DIT`/`DAH`/`esp`/`CSP`/`WSP`); tables index with `seg_index()`, `SEG_DIT || SEG_DAH` tests are `is_mark()` |
| the boundary kinds `K_MARK`, `K_SPACE`, `K_EITHER` | `CwLattice::Boundary::Mark` / `Space` / `Either` (private) |
| the kws classes `KT_DIT` … `KT_CSPL`, `KT_N` | `CwCallAlignment::kDit`, `kDah`, `kElementSpace`, `kCharSpace`, `kWordSpace`, `kLongCharSpace`, `kKinds` (private); `CwCallAlignment::kind_of()` maps a `CwSeg` |

### Renamed

Upstream names its state with two- to four-letter abbreviations. Here a
field, method or local the decoder reads across stages is named for what it
holds; the rest keep upstream's name (`m_` + it). The left column is
upstream's name as the stage member had it (`m_` dropped; the front end's
fields sit in upstream's decoder state with an `fe_` prefix, the call
reader's with `kr_`). Morse's own terms stay: dit, dah, mark, space, and
"space" throughout — element space, character space, word space — where
upstream mixes in "gap".

| Upstream | SDRIAK |
|---|---|
| **segments and priors** (`cw_lattice.h`) | |
| `SEG_ESP`, `SEG_CSP`, `SEG_WSP` | `CwSeg::ElementSpace`, `CharSpace`, `WordSpace` |
| what `seg_ideal()`, `dur_pen()` and `seg_window()` read of the state | `CwSegmentPriors`, built by `CwState::segment_priors()`; `CwReadView::priors` |
| … `esp`, `elem_err`, the fist centres | `element_space`, `timing_error`, `char_space`, `word_space` |
| `seg_ideal`, `dur_pen`, `seg_window` | `ideal_length()`, `duration_prior()`, `search_window()` |
| **lattice** (`cw_lattice.h`) | |
| `cum` | `llr_sum` (the cumulative llr) |
| `bm`, `bs` | `best_mark_end`, `best_space_end` |
| `bd_m`, `bd_s`, `bt_m`, `bt_s` | `mark_len`, `space_len`, `mark_type`, `space_type` (the back-pointers) |
| `pen`, `pen_at`, `pen_gen`, `pen_dit`, `pen_csp`, `pen_wsp`, `pen_esp`, `pen_edge` | `prior_memo`, `prior_stamp`, `prior_generation`, `memo_dit`, `memo_char_space`, `memo_word_space`, `memo_element_space`, `memo_edge` |
| `dur_pen_memo`, `pen_sync`, `lattice_head` | `CwLattice::duration_prior()`, `sync_prior_memo()`, `best_head()` |
| `commit_t`, `commit_kind` | `committed_until()` (`m_committed_until`), `m_committed_kind` |
| **clock** (`cw_clock.h`) | |
| `esp`, `elem_err` | `element_space()`, `timing_error()` (the assembler's and the run dump's `elem_err` arguments too) |
| the unbiased dit, (dit + esp)/2 | `unbiased_dit()` |
| `clk`, `clk_n`, `clk_head`, `clk_hold` | `m_mark_ring`, `m_mark_ring_n`, `m_mark_ring_head`, `m_relock_hold` |
| `clk_sp`, `clk_sp_n`, `clk_sp_head` | `m_space_ring`, `m_space_ring_n`, `m_space_ring_head` |
| the ESP and CSP/WSP clock updates, `clock_space` | `learn_element_space()`, `learn_space()` |
| the esp drop of the collapsed-space watchdog | `drop_element_space()` |
| **fist** (`cw_fist.h`) | |
| `gaps`, `ngaps`, `gap_head`, `gap_fresh` | `m_spaces`, `m_nspaces`, `m_space_head`, `m_since_fit` |
| `FIST_RING`, `FIST_MIN_D`, `FIST_MIN_FIT` | `kSpaceRing`, `kMinSpaceDits`, `kMinSpacesToFit` |
| **bootstrap** (`cw_schmitt.h`) | |
| `boot`, `nboot` | `m_mark_lens`, `m_nmarks` |
| **discriminator** (`cw_discriminator.h`) | |
| `mu_m`, `mu_s` | `mark_level()`, `space_level()` |
| `cont_mark`, `k_m_dn`, `k_s` | `m_mark_run`, `m_mark_release`, `m_space_gain` |
| the edge-weak switch, `ew_r` | `update_edge_model()`, `CwState::m_edge_weak_ratio` |
| **envelope** (`cw_envelope.h`, `cw_tone.h`) | |
| the sample loop's `m`, `ms` | `CwEnvelope::Sample::raw`, `soft`; in the decoder `env`, `soft_env` |
| the `nb_*` fields (the narrow path) | `m_narrow_on`, `m_narrow_ma`, `m_derotate_phase` |
| `foff_hz` | `CwTone::m_offset_hz`; `CwEnvelope::push()`'s `tone_offset_hz` |
| **v3 front end** (`cw_frontend.h`) | |
| the decoder's `fe_*` state | `CwState::m_front`; its parts `m_detector`, `m_matched`, `m_power` |
| the sample loop's `mr` | `matched_amp` |
| `mf_hi`, `mf_lo` | `matched_hi()`, `matched_lo()` |
| `n0`, `pmark`, `pk` | `m_noise_power` (σ0²), `m_mark_power` (σ1²), `m_mark_peak` |
| `pres`, `pres_neg`, `pres_on` | `m_presence`, `m_absent_run`, `m_keep_presence` |
| `pw`, `pw_size`, `pw_head`, `pw_due` | `m_power_ring`, `m_power_ring_size`, `m_power_ring_head`, `m_since_refresh` |
| `p_fast`, `k_pf` | not taken: written, never read |
| `mab`, `ma_sr`, `ma_si`, `ma_len`, `ma_cap`, `ma_pos`, `ma_used` | `m_sum_ring`, `m_sum_re`, `m_sum_im`, `m_sum_len`, `m_sum_cap`, `m_sum_pos`, `m_sum_used` |
| `rot_re`, `rot_im`, `rot_n`, `lp` | `m_phasor_re`, `m_phasor_im`, `m_phasor_n`, `m_lowpass` |
| `fc_hz`, `prom`, `nb_local`, `det_run`, `det_bar`, `det_s` | `m_carrier_hz`, `m_prominence`, `m_local_floor`, `m_hops_over_bar`, `m_detect_bar`, `m_detect_seconds` |
| `cand`, `cand_t`, `spec`, `spec_hops`, `hop_n`, `noise_chan` | `m_candidate`, `m_last_detection`, `m_spectrum`, `m_spectrum_hops`, `m_since_hop`, `m_band_floor` (`band_floor()`) |
| `fe_hop`'s `kspan`, `kown`, `kp`, `pk`, `nb` | `span_bins`, `own_bins`, `peak_bin`, `peak`, `floor` |
| **call reader and alignment** (`cw_callreader.*`, `cw_align.*`) | |
| `kr_hit` (`call`, `at`, `said`, `dc`, `n`) | `m_confirmed` (`CwCallReader::Confirmed`: `call`, `last_read`, `last_sent`, `best_score`, `reads`) |
| `kr_q`, `kr_next`, `kr_cq_t` | `m_fix_phrase`, `m_next_read`, `m_calling_seen` |
| `kr_shape`, `kr_add`, `kr_add_near`, `kr_score` | `looks_like_call`, `add_candidate`, `add_near_calls`, `score_call` |
| `kws` (the probe's ring), `kws_n` | `CwKwsProbe::m_ring`, `m_written` |
| `kws_call`'s and `kws_free`'s `c`, `P`, `S`, `gm`, `gse`, `gl`, `gsc`, `f`, `g` | `llr_sum`, `prefix`, `suffix`, `end_mark`, `end_element_space`, `end_long_silence`, `end_char_space`, `row`, `next_row` |
| `KwsDur`'s `pen`, `dlong`, `pen_long` | `Durations::prior`, `m_long_silence`, `m_long_silence_prior` |
| **tunables** (`decode_cw_config.h`; the variables keep their names) | |
| `SKIM_Vn_RESCORE`'s `fit`, `sep`, `ev`, `SKIM_V3_RS_EV_WEAK` | `rescore.max_deficit`, `min_margin`, `min_evidence`, `min_evidence_weak` |
| `SKIM_V3_KWS_DC`, `SKIM_V3_KWS_SEP` | `v3.reader.min_score`, `min_margin` |
| **decoder state** (`decode_cw_viterbi.cpp`) | |
| `pre`, `boot`, `brk_out`, `marks_ok`, `mark_t` | `m_preroll`, `m_bootstrap`, `m_break_sent`, `m_marks_committed`, `m_last_mark_t` |
| **oracle** (`oracle.h`) | |
| `Iv`, `iv` | `Interval`, `m_intervals` |

Inside a stage, upstream's `st->field` is the member `m_field` (or its
"Renamed" name) and its other reads are the method's arguments. A change upstream makes to both copies is
ported once. A v3-only change goes in as v3 has it if it sits behind the
weak view or the front end, and behind a `CwDecoderConfig` flag otherwise
(as `v3.robust_clock` does), so the v2 preset stays v2. A v2-only change gets
a flag the v3 preset leaves off. Changes to the neural-reader parts of v2/v3
are dropped; a change to `fftw_lock.c` is mirrored in `engine/channelizer.cpp`, a
change to the WDSP calls in `src/sdriak`. Then update the pinned commit above.

Ported C gets the same C++ spellings. clang-tidy (shipped with Visual
Studio's LLVM component) does the casts, `nullptr`, `(void)` lists and C
headers; the `typedef`s, `#define` constants and `qsort` are by hand:

```sh
clang-tidy -fix -header-filter='cw_engine[/\\]src' \
  -checks='-*,modernize-use-nullptr,modernize-redundant-void-arg,modernize-deprecated-headers,google-readability-casting' \
  src/engine/<file>.cpp -- -std=c++17 -Isrc -I<sdriak>/core/src -I<fftw include>
```

## Gates

With `OPT_BUILD_TESTS=ON`, `ctest -R cw_engine` runs upstream's
`skimmer-cw-test` (v2 and v3 on synthetic CW: clean copy, jitter, QSB,
speed changes, squelch on noise, the channelizer end to end, the fist
model, the weak ladder, the call reader) and `skimmer-call-test`
(validation, extraction, the dictionary and its swap under concurrent
readers). With `OPT_BUILD_CW_DEEPCW=ON` also upstream's
`skimmer-deepcw-test` (SDRIAK's model-pin section with the SHA-256
vectors, the commit rule, the draft and word gaps on synthetic CTC
matrices, the DSP half on synthetic baseband, and — only when
`SKIM_ORT_LIB` / `SKIM_DEEPCW_MODEL` point at a runtime and the model —
the model's copy, device fallback, the TX-hold flush and no phantom text
on noise) and SDRIAK's `deepcw2_test` (the v2 normalisation, the stitcher
on synthetic CTC streams, the DSP half, and the v2 models with
`SKIM_DEEPCW2_MODEL` / `SKIM_DEEPCW2_NARROW_MODEL`); ctest reports them
skipped without a model. ctest runs them inline (`SKIM_DEEPCW_SYNC=1`,
`SKIM_DEEPCW2_SYNC=1`); run by hand without it, they also check the
batched worker path.
