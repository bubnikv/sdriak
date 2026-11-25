# Networked sources with server-side waterfall: master plan

Date: 2026-11-24. Status: transport/1–3 implemented and committed;
kiwisdr/1–3 and core-spectrum/1–5 implemented, not committed; spyserver/0
measured (doc/research/spyserver-streams.md); spyserver/1–4 and ubersdr/1–4 implemented, not
committed. Every step is implemented; none of the uncommitted ones has
been through the GUI review gates yet.

This folder holds independent, self-contained implementation plans for an
implementation agent. Each sub-plan lists its own prerequisites, files,
steps and review gate. Read this page first: it sets the shared rules, the
vocabulary and the order.

## Goal

Three networked sources deliver a **server-side waterfall**: the spectrum
computed by the remote receiver over a span much wider than the IQ the link
carries. Each source keeps an option to fall back to the local FFT.

| source | IQ today | server spectrum | plan |
|--------|----------|-----------------|------|
| KiwiSDR (`kiwisdr_source`) | 12 kS/s (or 20.25 kS/s) | W/F websocket, 1 024 bins, zoom 0…`zoom_cap` | [kiwisdr/](kiwisdr/README.md) |
| UberSDR (new `ubersdr_source`) | 48–384 kS/s, plus 12 kS/s secondaries | `/ws/user-spectrum` v2, 1 024 bins, continuous zoom | [ubersdr/](ubersdr/README.md) |
| SpyServer (`spyserver_source`) | full IQ, or narrow IQ in VFO+FFT mode | FFT stream in `FFT_IQ` mode, 2 048 bins, decimation stages | [spyserver/](spyserver/README.md) |

Two shared foundations come first:

| plan | what |
|------|------|
| [transport/](transport/README.md) | TLS, HTTP proxy, HTTPS requests for every networked source: `WSClient` moved onto libcurl's WebSocket API |
| [core-spectrum/](core-spectrum/README.md) | the core architecture: spectrum capability, IQ window with stream-position commit, frame ingest, display span and server zoom, levels and fallback |

Related but separate: [../remote-receiver-browser.md](../remote-receiver-browser.md)
(one map/list browser over the KiwiSDR, UberSDR and SpyServer directories).

## Order and dependencies

```
transport/1 curl-websocket ─┬─ transport/2 http ─── transport/3 proxy
                            │
                            ├─ kiwisdr/1 client-fixes ──────────────┐
                            └─ ubersdr/1 protocol-client ──────────┐│
core-spectrum/1 capability ─ 2 iq-window ─ 3 ingest ─ 4 span-zoom ─ 5 levels-fallback
                                 │             │           │              │
              spyserver/0 measurements ─ spyserver/1 client ─ 2 mode ─ 3 waterfall ─ 4 window+gain
                                 │             │           │              │
                            kiwisdr/2 waterfall ─ kiwisdr/3 iq-window     │
                            ubersdr/2 source ─ ubersdr/3 waterfall ─ ubersdr/4 vfos
```

Recommended sequence. Each step is a reviewable unit:

1. **transport/1–2.** It unlocks every TLS receiver (17 of 432 reachable
   KiwiSDRs, 74 of 77 UberSDRs) and is the single biggest user-visible fix.
2. **kiwisdr/1.** The stream defects measured on real receivers: dropped
   flagged packets, the hard-coded rate, the web-UI path, silent
   refusals.
3. **spyserver/0** can run any time. It is measurement only and needs no
   code in the tree.
4. **core-spectrum/1–5,** in order, each a no-op until a source uses it.
5. **kiwisdr/2–3.** First server waterfall end to end: the largest gain,
   from 12 kHz to 30 MHz.
6. **spyserver/1–4.** No TLS needed.
7. **ubersdr/1–4.** Needs transport and core-spectrum.
8. **transport/3** (proxy) can go in any time after transport/1–2.

## Shared rules for every sub-plan

- **Builds.** Build and run the affected targets and tests in the agent's
  own trees (Windows: `build-msvc-x64-bench`, ASan
  `build-msvc-x64-asan`), never in the user's Visual Studio trees.
- **No commits** unless the user asks. Stop for review at each plan's
  gate. During review, show diffs and wait rather than reworking unasked.
- **Style.** New classes use `m_`-prefixed private members, const getters,
  `kName` constants, and one-line accessors in the class body. Existing
  code keeps its style.
- **Config.** Use the scoped transaction API (`config.read()`,
  `config.edit().section(...)`, `ensure()`, `tryGet()`). `acquire()` and
  `release()` do not exist.
- **Module CMake.** New modules use `include(${SDRPP_MODULE_CMAKE})`. A new
  module also goes into the hard-coded Android list in
  `core/src/core.cpp` (`#ifdef __ANDROID__`) and into the root
  `CMakeLists.txt` option list.
- **Comments and docs describe the current code.** No "previously",
  "fixed" or revision narration.
- **Attribution.** Credit borrowed knowledge or code in a header comment:
  * jp's SpyServer VFO+FFT findings;
  * lzh173's proxy layer (sdriak-zh);
  * UberSDR's `pcm_v4.hpp` (GPL-3.0, madpsy/ka9q_ubersdr);
  * KiwiSDR protocol facts from `jks-prv/Beagle_SDR_GPS` and kiwiclient.

  Our tree is GPL-3.0; all of these are compatible.
- **Public servers are third-party property.** Any live test is one
  session at a time, short, with pauses, on receivers with free slots, and
  never a parallel sweep.
- **Thread rules.** The UI thread owns the waterfall, the `WaterfallVFO`s
  and `VFOManager` bookkeeping. Network threads never call into the GUI,
  never take `buf_mtx`, and never block on the UI. The audio/IQ DSP path
  only ever sees atomics or lock-free hand-offs from control code.

## Vocabulary

| term | meaning |
|------|---------|
| **display domain** | frequencies as the waterfall shows them: hardware frequency plus `SourceManager`'s tuning offset (converters), with Invert IQ applied |
| **C** | display centre: `gui::waterfall.getCenterFrequency()`, the frequency the source was last `tune()`d to |
| **S** | display span: the waterfall's whole bandwidth (`WaterFall::getBandwidth()`) |
| **IQ window** | the slice of spectrum the source's IQ stream actually carries |
| **W** | IQ window offset: the window's centre relative to C, in the display domain. 0 for every source today |
| **window width** | usable (alias-free) width of the IQ window |
| **spectrum provider** | a source-owned object that supplies waterfall frames and server zoom (core-spectrum/1) |
| **frame** | one waterfall line from a provider, stating its own start frequency, span and presentation time |
| **marker** | a stream position (input sample index) at which something must take effect, here an IQ-window move (core-spectrum/2) |
| **playout time** | the `steady_clock` time a sample is expected to leave the source's prebuffer |

## Research behind these plans

- [../../design/dsp-pipeline.md](../../design/dsp-pipeline.md) §10: jp's
  SpyServer integration and its weaknesses; the general design.
- [../../research/kiwisdr-streams.md](../../research/kiwisdr-streams.md):
  the KiwiSDR protocol, read from the server source and measured on 20+
  receivers.
- [../../research/ubersdr.md](../../research/ubersdr.md): the UberSDR
  directory, native API, measurements on 8 instances.
- [../../research/forks/jp.md](../../research/forks/jp.md) §4 and
  [../../research/forks/zh.md](../../research/forks/zh.md) §2: borrowed
  code.
