# jprincl/SDRPlusPlus-jp — features worth porting

Review date: 2026-11-24. Reviewed
[`jprincl/SDRPlusPlus-jp`](https://github.com/jprincl/SDRPlusPlus-jp) `master`
at [`c48ba994`](https://github.com/jprincl/SDRPlusPlus-jp/commit/c48ba994) (tag
`v1.7.0-beta`, 2026-11-04), the unmerged branch `module-web_map-v1` at
[`e54fdefb`](https://github.com/jprincl/SDRPlusPlus-jp/commit/e54fdefb)
(2026-08-10), and the merged (since deleted) `test-wefax_decoder` at
[`f01143cd`](https://github.com/jprincl/SDRPlusPlus-jp/commit/f01143cd).
Two detailed windows:

- [`82c7b9c8...9e56994b`](https://github.com/jprincl/SDRPlusPlus-jp/compare/82c7b9c855f6ab2eca1d84a1acafb8154af5078d...9e56994b58f0fd3518c3aca26fa3ef5f4310cfc2)
  (to 2026-08-04): AGC ceiling, FT8/FT4/WSPR, SpyServer VFO+FFT, Android
  background playback, RS41 fix — sections 1–5 below.
- [`9e56994b...c48ba994`](https://github.com/jprincl/SDRPlusPlus-jp/compare/9e56994b58f0fd3518c3aca26fa3ef5f4310cfc2...c48ba994)
  (2026-08-05 → 2026-11-04): WEFAX, SSTV and RTTY decoders, the Frequency
  Info (EiBi/Aoki) module, Android GL texture workaround, Linux window identity
  and AppImage fixes, FT8 `RR73` fix — sections 6–11 below.

`git fetch jp` rejects two of the fork's tags (`nightly`, `v1.4.0-beta`)
because they clash with our own tag names; fetch its tags into a namespace
instead: `git fetch jp '+refs/tags/*:refs/tags/jp/*' --no-tags`.

## State of the fork

- It is a direct fork of us at our `1576b30f` (*core: resolve relative config
  paths against the executable directory*, 2026-07-21) and still has not
  merged any of our later work. Every core/UI port needs adaptation; in
  particular all its modules use `config.acquire()/release()`, which our tree
  no longer has (scoped `Transaction`).
- `master` is 305 commits past the fork point. The second window is 104
  commits, 43 files, `+10,950/−13` (`+7,501` excluding two vendored copies of
  `stb_image_write.h` and screenshots). Core is touched in only four places:
  module default list, GLFW window hints, band-plan strip height, version.
- Activity was a burst 2026-08-05 → 08-10 (v1.4.0 → v1.6.0), then nothing
  until a small Linux-packaging release on 2026-11-04 (v1.7.0).
- **Provenance.** FT8/FT4/WSPR, WEFAX, SSTV and RTTY are all imported from
  [F4JTV/SDRPlusPlus_SwissKnifeEdition](https://github.com/F4JTV/SDRPlusPlus_SwissKnifeEdition)
  (GPL-3.0, an upstream SDR++ fork, last pushed 2026-07-06), which carries
  ~40 decoder modules (ACARS, ADS-B, AIS, APRS, NAVTEX, PSK, Olivia, HFDL,
  VDL2, DRM, …). jp's own contribution to those four is Android robustness
  and a few bug fixes. If we want any of these decoders, the right source is
  F4JTV, with jp's fixes applied on top as separate patches. Frequency Info
  and the web map are jp's own work.
- There are still no decoder tests or recorded vectors, and the repository
  still restricts new issues, so absence of bug reports is not evidence.
- Re-checked 2026-11-24: no new commits since `c48ba994`; the remote branches
  are `master`, `module-web_map-v1` and `v1.7.0-beta-test`.

## What the fork did to our tree, in general

**Shape.** Measured as lines changed from our `1576b30f` to jp `c48ba994`:

| area | lines | nature |
|------|-------|--------|
| `decoder_modules/ft8_decoder` | 8 919 | imported (F4JTV) plus integration; vendored `ft8_lib`, KISS FFT, `wsprd` |
| vendored `stb_image_write.h` (2 copies) | 3 448 | third-party |
| `decoder_modules/sstv_decoder`, `wefax_decoder`, `rtty_decoder` | 5 557 | imported (F4JTV) plus Android fixes |
| `misc_modules/freq_info` | 1 819 | jp's own |
| `source_modules/spyserver_vfo_source` | 1 527 | jp's own (AI-assisted: several "jules" PR branches) |
| `scripts/freqconv.py`, `frequency_manager` | 691 | jp's own |
| `core/src` (24 files) | 268 | mostly rebrand and paths; the external-FFT/VFO hooks; the AGC ceiling plumbing; zoom-slider sizing |
| `.github`, readme, changelog, packaging | about 550 | release machinery |

* **Mostly additive.** The fork adds modules and touches our core in small
  places. Almost nothing of ours was restructured or removed, so very
  little of it conflicts with our later work except the config API
  (`acquire()/release()` → scoped transactions).
* **Where the fixes are.** jp's own fixes are concentrated in the SpyServer
  VFO+FFT source (tuning domain, Invert IQ, gain servo), Android texture
  survival, and Linux packaging.
* **Cadence.** 166 commits in 2026-07, 133 in 2026-08, 6 in 2026-11. Work
  arrives in short bursts, one PR per feature (23 PRs, 19 merged).

## KiwiSDR and networking

Searched for KiwiSDR and network-protocol changes across `master`, both
open branches and all 23 PRs. jp did **no KiwiSDR or network-protocol work
beyond renaming**:

| file | change | relevance |
|------|--------|-----------|
| `core/src/utils/proto/kiwisdr.cpp` | `SERVER DE CLIENT sdr++iak SND` → `sdr++jp` | identity only. jp's KiwiSDR client is ours as of 2026-07-21, with the same defects we have since measured (no TLS, `msg[3] == 0x08` drops flagged packets, hard-coded 12 000 S/s, `/kiwi/` web-UI path); see [kiwisdr-streams.md](../kiwisdr-streams.md) §7–§8. |
| `core/src/utils/curl_init.cpp` | `User-Agent` `SDR++iak/` → `SDR++jp/` | identity only |
| `core/src/server_protocol.h` | `SERVER_PROTOCOL_FORK_ID` `sdrpp-iak` → `sdrpp-jp`, **`SERVER_AUTH_SALT` `sdrpp-iak-server-auth-v1` → `sdrpp-jp-server-auth-v1`** | **interoperability.** The PBKDF2 salt is part of the password hash, so a password-protected SDRIAK server and an SDR++ jp client (or the reverse) cannot authenticate each other, though the magic and versions still match. Unauthenticated connections still interoperate. Nothing to port; worth knowing when users mix the two. |
| `source_modules/spyserver_vfo_source` | new module | the only networking feature: see §4 and [dsp-pipeline.md §10](../../design/dsp-pipeline.md#10-server-side-spectrum-for-network-sources) |
| `misc_modules/web_map` (unmerged branch) | HTTP/SSE plus TCP collector | listens on `0.0.0.0`, no authentication; see "Unmerged branch: web map" |

The KiwiSDR and network improvements worth making come from our own
measurements, not from this fork:

* KiwiSDR: [kiwisdr-streams.md](../kiwisdr-streams.md) §7 (client fixes,
  TLS) and §8.7.
* UberSDR: [ubersdr.md](../ubersdr.md) (a native remote source).
* SpyServer: [todo/network-sources/spyserver/](../../todo/network-sources/spyserver/README.md).

## Bottom line

| Change | Recommendation | Why |
|---|---|---|
| Radiosonde RS41 over-read | **Ported 2026-08-05** (`7d573ccc`) | Tiny Android crash fix against the same pinned dependency. |
| Linux window identity hints (runtime GLFW check) | **Port** | Our focal-built AppImage compiles against GLFW 3.3 headers, so our compile-time check never sets the Wayland app ID even on a 3.4 host. |
| AppImage: maintained `appimagetool` | **Port** | Our Dockerfile still downloads the deprecated AppImageKit tool; the new one embeds the static runtime (no host `libfuse2`). |
| Frequency Info (EiBi + Aoki) | **Use as the starting point for our EiBi module** | Matches `doc/todo/eibi-schedules-module.md` closely and already has the Aoki parser and listener-distance ranking; needs downloader, config migration and overlay coexistence. |
| Android GL-context loss for module textures | **Fix in core, not per module** | Real bug we share (our `ImageDisplay`/`LinePushImage` users too); jp's frame-counter heuristic is a module-local workaround. |
| AGC maximum-gain control | **Port, split from the DSP rewrite** | Small and useful for HF; retain the analytic-signal SSB change for a separate A/B test. |
| FT8/FT4/WSPR decoder | **Worth adapting after tests and provenance audit** | High user value; take from F4JTV plus jp's fixes (incl. `RR73` not a locator). |
| RTTY decoder | **Candidate; evaluate against F4JTV** | Small (≈850 lines), properly locked modem; no test vectors. |
| SSTV (analog) decoder | **Watch** | We have only KG-STV; same unsynchronised GUI/DSP settings and DSP-thread rendering pattern as WEFAX. |
| WEFAX decoder | **Watch; do not port yet** | Now released, but the threading, memory and DSP-thread rendering issues are unchanged. |
| SpyServer VFO+FFT (server-side waterfall) | **Wanted; port through a general core design** | The fork's only server-side waterfall. Its core hooks support one VFO only and couple the network thread to the UI; see `dsp-pipeline.md` §10. |
| SDR# bookmark converter | **Worth taking** | Self-contained Python 3 interoperability tool; add fixtures before adopting. |
| Android bookmark import/export | **Reimplement the file access** | Export scopes are useful; “All files access” is the wrong picker. |
| Android background playback | **Take the requirement, not this patch** | Ignoring `APP_CMD_PAUSE` is not durable background playback. |
| Web map + radiosonde reporter branch | **Skip for now** | Unmerged; bundles 22k-line `httplib.h` and Leaflet, and listens on `0.0.0.0` without authentication by default. |
| Band-plan strip height | **Optional cosmetic** | One line; check against our touch sizing before taking. |
| Waterfall control-column sizing | **Already superseded here** | Our adaptive `fixedCost`/`budgetH` layout is more complete. |
| Rebrand, package IDs, release workflows | **Skip** | Fork identity and maintainer-specific delivery machinery. |
| KiwiSDR / network protocol fixes | **None to port** | Only identity strings changed. The changed auth salt makes password-protected server/client pairs across the two forks incompatible. |

## Ported

### 1. RS41 serial-field over-read — **ported 2026-08-05**

Commit [`6d884b0c`](https://github.com/jprincl/SDRPlusPlus-jp/commit/6d884b0c860476d9c61899308ac95d7d31019554)
fixes `sondedump`'s:

```c
strncpy(dst->serial, status->serial, sizeof(dst->serial)-1);
```

`status->serial` is an eight-byte packed RS41 field without a terminator, while
the destination is 32 bytes. Asking `strncpy` for 31 bytes reads beyond the
source object; Android's `_FORTIFY_SOURCE` detects that and aborts in
`rs41_decode`. The fork copies `sizeof(status->serial)` and relies on the
existing `dst->serial[8] = 0` immediately afterwards.

Our `deps/+sondedump/sondedump.cmake` pins the same revision (`52865556`) and
carries the fix as a dependency patch (`7d573ccc`), using `memcpy` for the
fixed-width source field.

## Worth porting, with adaptation

### 2. AGC maximum-gain ceiling — take the control first

The `v1.3.0-beta` series
([PR merge `468970b5`](https://github.com/jprincl/SDRPlusPlus-jp/commit/468970b586a1b6a33a8b50703e70662b9d91517d))
adds an “AGC Threshold” slider to SSB, DSB, CW, CW-R and AM. Technically this is
a maximum-gain ceiling: 0–120 dB, default 80 dB, mapped to the existing
`loop::AGC::setMaxGain()`. It prevents the AGC from lifting an empty channel's
noise floor without removing our already-ported manual/automatic AGC switch.

The useful part is small: expose `setAGCMaxGain()` from the AM/SSB/CW wrappers,
persist one value per demodulator, and add the slider. Port that independently,
on our scoped config transactions.

The same series also moves SSB AGC from rectified real audio to the complex
analytic envelope. That is a sound idea and should reduce audio-rate envelope
ripple, but it changes every USB/LSB/DSB signal path, buffer ownership, and gain
response. Keep it as a separate patch with recorded-signal A/B tests. While in
this code, fix or explicitly accept the inherited unsynchronised `AGC::getGain()`
GUI read; the DSP thread writes `_gain` concurrently.

### 3. FT8 / FT4 / WSPR module — valuable, not a drop-in

The module landed in [`a991a314`](https://github.com/jprincl/SDRPlusPlus-jp/commit/a991a3142ba33ca3146072b711da577eb0dcb7d6)
from F4JTV's SwissKnifeEdition and was integrated through
[`69cf6993`](https://github.com/jprincl/SDRPlusPlus-jp/commit/69cf6993fd9aecebe49eff62dd94372f9964254a).
It provides:

- a dedicated 3 kHz USB helper VFO and UTC-aligned capture slots;
- FT8 and FT4 through vendored `ft8_lib`/KISS FFT;
- WSPR through a vendored `wsprd` core and FFTW3f;
- a joined worker thread rather than decoding on the DSP thread;
- decode table, TSV logging/snapshots, Maidenhead QTH parsing and distance;
- build integration through our `sdrpp_link_dep` dependency layer.

[`d8ed3707`](https://github.com/jprincl/SDRPlusPlus-jp/commit/d8ed3707) stops
the locator extractor from treating the `RR73` sign-off as grid square RR73;
include that if we port the module.

Do not cherry-pick it unchanged:

1. There are no known-vector tests for any of the three modes. Add prerecorded
   slots and assert decoded text, timing and frequency before trusting on-air
   results. Its FT8/FT4 “SNR” is explicitly a sync-score proxy, not a calibrated
   WSJT-X SNR, and should be labelled as such in the UI.
2. `FT8Engine` has a file-global callsign hash table justified as safe because
   decoding uses one worker, while the module declares unlimited instances and
   gives every instance its own worker. Two instances race on that table.
3. `myQth`, log settings and `scrollToBottom` cross the GUI/worker boundary
   without consistent locking. Snapshot settings into each queued slot or put
   the shared state behind a mutex/atomic protocol.
4. Preserve and audit the individual provenance/licences of `ft8_lib`, KISS FFT
   and the K9AN/K1JT WSPR sources. A single copied GPL-3 text describes the
   combined module but is not a provenance record for the vendored components.
5. Adapt configuration to our transaction API and run every enabled platform
   build. The module defines `_GNU_SOURCE`; its comments claim cross-platform
   support, but there is no module-specific CI or Windows/Android decoder test.

### 4. SpyServer VFO+FFT — coherent but specialised

The initial source module predates the first window. The work in that window
turns it from a proof of concept into a much more serious remote-Airspy source:

- corrects centered/non-centered, converter-offset, sideband and Invert-IQ
  tuning ([`58df2c48`](https://github.com/jprincl/SDRPlusPlus-jp/commit/58df2c483f6d1ae9749e90c9a1d34cf4c32a9ec0),
  [`3139bc50`](https://github.com/jprincl/SDRPlusPlus-jp/commit/3139bc50d6d643b811f49f24f10eb6da40408dbb));
- consumes `CLIENT_SYNC` and reports shared/no-control connections
  ([`24bbf98a`](https://github.com/jprincl/SDRPlusPlus-jp/commit/24bbf98afbdfac22970518eab2646f3ffd83300c));
- sends streaming mode before RF/digital gain and adds manual digital gain;
- adds a raw-IQ peak/rail meter and an automatic fast-attack/slow-release
  digital-gain servo for UInt8/Int16, including an empirical Airspy HF+ UInt8
  baseline ([`a5c5a1d1`](https://github.com/jprincl/SDRPlusPlus-jp/commit/a5c5a1d13989bf3a7590f9be0082dbe1489e2901)
  through [`c813a3a3`](https://github.com/jprincl/SDRPlusPlus-jp/commit/c813a3a3910a1fa1b5306a6af7ddebea4df976f1));
- remembers recent servers and handles old per-device configs defensively.

No SpyServer changes in the second window. It is the only server-side
waterfall in the fork: no other jp source uses the external-FFT path, on any
branch or tag. We want server-side spectrum for our network sources
generally (KiwiSDR W/F first, then SpyServer, then our own server). The
integration, its weaknesses and a general core design are written up in
[dsp-pipeline.md §10](../../design/dsp-pipeline.md#10-server-side-spectrum-for-network-sources).
Take the SpyServer part as a VFO+FFT mode of our `spyserver_source` (keeping
our hardened client), with jp's session order, span rules and gain servo,
rather than as a second module, and test the gain commits together with it,
not one by one.

Before that, replace the fork's core shortcut. It treats the generic
`IQFrontEnd::externalFFTMode` flag as “the source owns the VFO DSP offset” and
therefore suppresses normal `VFOManager` offset propagation for *every* future
external-FFT source. Model that as an explicit source capability instead. Also
test a matrix of USB/LSB/AM, normal/inverted IQ, positive/negative converter
offset, center/edge VFO, and capped/uncapped IQ bandwidth. The servo constants
and HF+ baseline are empirical and should remain per-device policy, not generic
core behavior.

### 6. Linux window identity and AppImage packaging — port both

[`5ab8db59`](https://github.com/jprincl/SDRPlusPlus-jp/commit/5ab8db59) moves
the GLFW window-identity hints from a compile-time `GLFW_VERSION_*` check to a
`glfwGetVersion()` runtime check, defining the hint enums locally when the
headers lack them. That matters to us: `docker_builds/appimage/Dockerfile` is
`ubuntu:focal` (GLFW 3.3 headers), and the AppImage deliberately uses the
host's `libglfw`. Our `core/backends/glfw/backend.cpp` therefore compiles the
`GLFW_WAYLAND_APP_ID` hint out of the AppImage, and on a GLFW 3.4 Wayland host
the shell cannot map the window to `sdriak.desktop`. Take the runtime check
with our `sdriak` ID and keep our `SDRPP_GLFW_UNIX_DESKTOP` guard.

[`695216af`](https://github.com/jprincl/SDRPlusPlus-jp/commit/695216af) and
[`3aa7e354`](https://github.com/jprincl/SDRPlusPlus-jp/commit/3aa7e354):
download `appimagetool` from the maintained `AppImage/appimagetool` project
instead of the deprecated `AppImageKit` (which we still use), `export ARCH`
before packing so it fetches the matching static type-2 runtime, and fail the
build if the generated desktop file lacks `StartupWMClass`. The resulting
AppImage no longer needs `libfuse2` on the host. Our `sdriak.desktop` already
has `StartupWMClass=sdriak`; the guard is cheap insurance.

### 7. Frequency Info (EiBi + Aoki) — seed for our planned EiBi module

`misc_modules/freq_info` ([PR #22 merge `ff64ef20`](https://github.com/jprincl/SDRPlusPlus-jp/commit/ff64ef20),
≈1,820 lines, jp's own work inspired by SDR#'s ListenInfo) is close to
`doc/todo/eibi-schedules-module.md`:

- EiBi CSV and Aoki fixed-width parsers into one frequency-sorted database;
  Windows-1252 → UTF-8; Aoki columns measured from the file's header line;
  Aoki's 1 = Sunday convention handled; malformed lines counted, not fatal.
- Per-frame `queryRange()` uses `lower_bound`/`upper_bound` on the visible span
  with on-air filtering; ties ranked by target area, or by haversine distance
  from a configured listener latitude/longitude for Aoki entries, which carry
  transmitter coordinates.
- Waterfall labels via `onFFTRedraw`/`onInputProcess` with click-to-tune and
  delayed hover tooltips (target, country, language, site, power), a side
  panel with compact mode, and a separate searchable browse window.

It is a much better starting point than writing the module from scratch, but
falls short of our plan in these places:

1. **No downloading.** The user fetches the files and types paths. Our plan
   wants a worker-thread updater with season computation, timeouts, atomic
   replace, cache and EiBi → previous season → Aoki failover.
2. **Parsing on the GUI thread**, including at module construction. Move it to
   the updater thread and swap a `shared_ptr<const vector>` as planned.
3. **Overlay coexistence.** Its input handler does not return early when
   `gui::waterfall.inputHandled` is already set, and its label placement does
   not default to the side opposite `frequency_manager`; adopt the plan's
   bottom-by-default rule and early-return guard (also needed next to `spots`).
4. Config uses `acquire()/release()`; rewrite on our transaction API, and use
   our m_-style members if the code is restructured.
5. Its README notes it is cramped on phones; check `style::touchStyle` sizing.

Keep the module name decision ours (`station_schedules` in the plan); credit
jp for the parsers and ranking in the header comment.

### 8. Android GL-context loss — a shared bug, fix it centrally

On Android, `APP_CMD_TERM_WINDOW` runs `backend::end()`, which destroys the
EGL context, and `INIT_WINDOW` → `doPartialInit()` creates a new one. Our
backend does the same. Every GL texture a module created is then gone, while
the module still holds the old name. The waterfall survives because it
re-specifies its texture with `glTexImage2D` every frame; cached images do not.

jp first tried to keep the context alive and only recreate the EGL surface
([`22214375`](https://github.com/jprincl/SDRPlusPlus-jp/commit/22214375)), but
reverted it the same day after resume-after-exit problems
([`17fc3af6`](https://github.com/jprincl/SDRPlusPlus-jp/commit/17fc3af6)).
The shipped workaround
([`806352de`](https://github.com/jprincl/SDRPlusPlus-jp/commit/806352de),
[`0d634277`](https://github.com/jprincl/SDRPlusPlus-jp/commit/0d634277)) is
per-module: detect a drop in `ImGui::GetFrameCount()` as “context recreated”,
and never call `glGenTextures` again so the module cannot be handed a name the
waterfall is about to rebind. That works, but it depends on two accidents —
the ImGui frame counter resetting with the context, and the waterfall reusing
a stale name without regenerating it.

For us: our `ImageDisplay` (`core/src/gui/widgets/image.cpp`) and
`LinePushImage` widgets, and every decoder using them, are exposed to the
same loss. The durable fix is in core: a backend-owned GL context generation
counter (or an `onContextRecreated` event) that core widgets and modules check
before using a cached texture, regenerating names after a loss, with the
waterfall converted to the same protocol. The context-keeping approach in
`22214375` is the better long-term design if its resume-after-exit failure is
understood; record that failure before retrying it.

The related `GL_UNPACK_ALIGNMENT = 1` fix (preview slanted while saved image
was not) applies only to tightly packed RGB rows of non-multiple-of-four
width; our core image widgets upload RGBA and are unaffected.

## Decoders imported from F4JTV

### 9. RTTY decoder — candidate

`decoder_modules/rtty_decoder` ([PR #21 merge `1f88ca41`](https://github.com/jprincl/SDRPlusPlus-jp/commit/1f88ca41),
≈850 lines): FLDIGI-style Baudot/ITA2 on the complex VFO baseband (USB, LSB,
or FM-discriminated NFM), two boxcar complex correlators on mark/space, UART
framing, LTRS/FIGS with optional USOS, a 128-bin DFT band view and optional
AFC. jp fixed a crash when disabling the decoder with AFC on
([`baaa9a7a`](https://github.com/jprincl/SDRPlusPlus-jp/commit/baaa9a7a)).

Unlike WEFAX/SSTV, the modem's setters and the spectrum view are mutex-guarded.
Weaknesses: boxcar correlation without matched filtering or ATC is noticeably
weaker than fldigi's under selective fading; no recorded test signals;
unlimited instances. Worth taking if RTTY is wanted, from F4JTV with jp's AFC
fix, after a recorded-signal comparison against fldigi.

### 10. SSTV (analog) decoder — watch

`decoder_modules/sstv_decoder` ([PR #20 merge `f43c6d51`](https://github.com/jprincl/SDRPlusPlus-jp/commit/f43c6d51),
≈2,700 lines plus a vendored `stb_image_write.h`): VIS detection, Martin,
Scottie, Robot and PD modes at a 12 kHz internal rate, slant correction with
optional RANSAC, median filter, live texture and image saving. We only have
`kg_sstv_decoder` (Korean digital KG-STV), so this would be new functionality.
It shares the WEFAX pattern: plain `bool` settings written by the GUI thread
while the DSP thread runs `process()`, unlimited instances, no test vectors.

### 11. WEFAX decoder — released, still not ready

WEFAX is now on `master` ([PR #19 merge `2b24f1ef`](https://github.com/jprincl/SDRPlusPlus-jp/commit/2b24f1ef),
released in v1.4.0-beta): the former `test-wefax_decoder` branch plus the
Android texture workaround and the `GL_UNPACK_ALIGNMENT` fix. Features: USB/
LSB/NFM front ends, IOC 576/288, six LPM values, APT start/stop detection,
phasing-pulse regression and optional RANSAC auto-slant, learned/manual slant,
horizontal shift, median filtering, live texture, BMP/PNG/JPEG saving.

The blocking issues are unchanged:

- the GUI thread changes LPM, IOC, slant, APT flags and render state while the
  DSP handler calls `decoder.process()`. Image and raw buffers have mutexes,
  but most control fields (`setAutoStart`, `setAutoSlant`, `setHShiftPixels`,
  `reRenderRequested`, …) are plain unsynchronised members: a data race;
- full re-renders still run from the DSP handler under both large-buffer
  locks, so a setting change can stall the flowgraph;
- at default 120 LPM it allocates two full RGB images plus a capped raw-float
  capture — roughly 70 MB per SSB instance, 118 MB per NFM instance — with
  unlimited instances;
- no attribution beyond F4JTV in the readme, no recorded tests.

Revisit with a worker/snapshot architecture, bounded instance count, and
recorded WEFAX tests. The auto-slant/RANSAC algorithm (shared with SSTV) may
still be worth extracting.

## Useful requirement, incomplete implementation

### 5. Android “Play in background”

[`bee94fae`](https://github.com/jprincl/SDRPlusPlus-jp/commit/bee94faefc40325b4c373f4361fd6faf478e01fd)
adds a persisted checkbox and, when enabled, simply declines to stop the SDR on
`APP_CMD_PAUSE`. It may keep playing while the existing process remains alive,
but Android is free to reclaim a background activity.

For durable background audio, Android documents a `mediaPlayback` foreground
service, its manifest service type and `FOREGROUND_SERVICE_MEDIA_PLAYBACK`
permission; background playback is normally hosted in a media-session service.
See [Foreground service types](https://developer.android.com/develop/background-work/services/fgs/service-types#media)
and [Background playback with a MediaSessionService](https://developer.android.com/media/media3/session/background-playback).
An SDR implementation also needs explicit notification/stop controls, audio
focus and wake-lock policy. Port the user-facing option only as part of that
larger lifecycle design, which should also settle section 8 (keep the EGL
context, or recreate textures). Do not take the three-line pause bypass alone.

## Unmerged branch: web map

`module-web_map-v1` (29 commits, 2026-08-10, not merged or released) adds
`misc_modules/web_map` — an embedded cpp-httplib HTTP/SSE server serving a
Leaflet map, with a TCP JSON-lines collector — and a `MapReporter` in the
radiosonde decoder that pushes positions and trails to it. Not worth taking
in this form: it vendors a 21,773-line `httplib.h` and Leaflet 1.9.4 as C
strings (both permissively licensed, but unrecorded), and both the HTTP and
the TCP ingest listeners default to `0.0.0.0` with no authentication, so any
host on the LAN can inject map points. If we want a map, prefer a loopback
default and an existing network layer. It also carries the same band-plan
height tweak as `master`.

## Other branches and repositories

The fork's remote heads are `master`, `module-web_map-v1`, and
`v1.7.0-beta-test` (two commits behind `master`, its PR #23 source, nothing
unique). `test-wefax_decoder` has been deleted on the remote after merging.

jprincl's other public repositories, none of which carries SDR++ code:

- [`ft8d`](https://github.com/jprincl/ft8d) (fork of `kholia/ft8d`, 25
  commits, 2026-08-24..29): the standalone Fortran FT8 decoder with an
  Airspy HF+ capture wrapper, gaining home-locator distance, sorting,
  per-band logging, grid-aware deduplication, and Cloudlog upload with an API
  key. The Cloudlog/deduplication behaviour is a reference if our FT8 module
  ever grows a logbook upload.
- [`dsd-neo`](https://github.com/jprincl/dsd-neo) branch `console-output-test`
  (16 commits, 2026-08-14..15): a console log screen for the Qt/Android UI of
  the digital-voice decoder. Not relevant to us.
- [`ssmon`](https://github.com/jprincl/ssmon) (Go, 2026-10): a SpyServer
  listener monitor that passively reads SpyServer's directory status reports
  and shows listeners on a web map. A server-operator tool, not client code.

## Earlier features still present

- **`scripts/freqconv.py`** converts SDR++ bookmark JSON to and from SDR#
  `frequencies.xml`, retaining names, groups/lists, frequency, bandwidth, mode
  and notes while explicitly reporting lossy fields and renamed duplicates.
  It is self-contained Python 3 and is worth taking once round-trip fixtures
  cover both schemas, malformed records, duplicate names and every mode.
- **Android frequency-manager I/O** replaces the non-working native file dialog
  with a typed path and adds selected/current/all export scopes. The scopes and
  data handling are useful. Do not copy the requirement for
  `MANAGE_EXTERNAL_STORAGE`: Google Play explicitly treats user-selected file
  import/export as a Storage Access Framework use case and restricts “All files
  access.” See [Google Play's permission policy](https://support.google.com/googleplay/android-developer/answer/10467955).
  Keep our hardened bookmark parsing and scoped config transactions, and add a
  proper Android document picker/create-document bridge instead.

## Already covered or skip

- **Band-plan strip height** (`dd08d8a9`): `CalcTextSize("0").y * 2.5f` →
  text height `+ style::dp(6)`, a thinner strip. Cosmetic; take only after
  checking it against our touch-target sizing.
- **High-DPI waterfall controls** (`e259cfb4`): our current `budgetH`,
  `fixedCost`, touch-size and two/three-slider logic supersedes its three-line
  reserve correction.
- **Radiosonde README-only “crash fixes”** after `6d884b0c`: no additional code
  exists; the dependency patch is the whole fix.
- **Complete `sdrpp-jp` rebrand**, package/app IDs, server magic, user agents,
  icons, desktop-file categories and paths: fork-specific.
- **Release version/tag churn and maintainer signing workflows**: no product
  feature to port.
- **Narrowest two SpyServer decimation stages removed**: a workaround for the
  fork's clipping behavior. If we take the automatic digital-gain servo,
  validate the full stage range before hiding valid bandwidth choices.

## Recommended order

1. Take the two Linux packaging fixes (section 6): runtime GLFW window hints
   and the maintained `appimagetool`. Small, and they fix a real AppImage
   defect of ours.
2. Add the AGC maximum-gain ceiling without the analytic-envelope rewrite.
3. Build the EiBi module from `doc/todo/eibi-schedules-module.md`, seeded with
   jp's `freq_info` parsers, ranking and UI; update that todo to reference it.
4. Fix Android GL-context loss in core before porting any texture-backed
   decoder (section 8).
5. Decide which F4JTV decoders are product goals and review F4JTV directly
   rather than through jp: FT8/FT4/WSPR first (known-slot tests, shared-state
   and provenance fixes), RTTY second; SSTV and WEFAX only after a
   worker/snapshot rewrite.
6. Server-side spectrum for network sources, built on the core design in
   `dsp-pipeline.md` §10.5: KiwiSDR W/F first, then SpyServer VFO+FFT
   (jp's series) as a mode of `spyserver_source`.
