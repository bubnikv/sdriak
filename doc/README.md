# Documentation

| Directory | Holds |
|---|---|
| `doc/` | User- and builder-facing documents: how to build, how the release artefacts work |
| `doc/design/` | How this fork is structured, and specs it must conform to |
| `doc/research/` | Findings about the world outside this repository |
| `doc/todo/` | Work intended but not finished |
| `doc/bugs/` | Known defects and the investigations behind them |

## Where a new document goes

The seam that needs a rule is the implementation plan, which is both design and
todo. Use this one:

> **`design/` describes structure that outlives any single task. `todo/` is work
> not yet done.**

So a plan for an unbuilt feature goes in `todo/` — it *is* the future feature.
When the work lands, either delete the plan (git remembers it) or promote the
part that still describes the architecture into `design/`.

Research goes in `research/` even when it drives a decision here; the decision
itself belongs in `design/` or `todo/`. Fork surveys live in
`research/forks/`, indexed by its `README.md`.

## Conventions

- File names are kebab-case, no spaces — they end up in shell commands, URLs
  and `git log --follow` arguments.
- Open with a date, and for `todo/` and `bugs/` a status line
  (`current` / `postponed` / `superseded`), so a stale document is identifiable
  without reading it.
- Link to source files by repository-relative path so the reference survives a
  move of the document itself.

## Index

**Build and release**
- [appimage.md](appimage.md) — Linux AppImage internals and glibc floor
- [build-windows.md](build-windows.md) — MSVC x64 build, presets, configurations

**Design**
- [dsp-pipeline.md](design/dsp-pipeline.md) — source → front end → VFO → decoder → sink: threading, buffers, latency budget per mode
- [volk.md](design/volk.md) — exact input/output aliasing policy, implementation audit, and verification requirements
- [buffer-size-requirements-current.md](design/buffer-size-requirements-current.md) — short current-state summary of the one below: costs, open overflow risks, sizing rules, what blocks lowering the constant
- [buffer-size-requirements.md](design/buffer-size-requirements.md) — what `STREAM_BUFFER_SIZE` must satisfy, producer block sizes, and what it costs (preliminary; audit incomplete)
- [input-source.md](design/input-source.md) — every source module's sample rates, block sizes, wire rates and driver buffers; the front end's input ring (`SampleFrameBuffer`), stall tolerance per driver, and what should replace the ring
- [deps-build-system.md](design/deps-build-system.md) — third-party dependency build
- [iq-file-formats.md](design/iq-file-formats.md) — IQ/baseband record, playback and interchange formats
- [libcurl-integration.md](design/libcurl-integration.md) — libcurl is core-owned; plugins must not link it
- [config-access.md](design/config-access.md) — config access lifetimes and
  porting modules from upstream SDR++

**Research**
- [band-stacking.md](research/band-stacking.md) — what a band-stack register contains, when it is written, what a band boundary is
- [nn-cw-decoders-denoisers.md](research/nn-cw-decoders-denoisers.md) — neural CW decoders and denoisers
- [keyboard-shortcuts.md](research/keyboard-shortcuts.md) — this fork's bindings read from source, compared against gqrx/HDSDR/SDRangel/CubicSDR/Quisk
- [forks/](research/forks/README.md) — fork landscape and per-fork merge reviews

**Todo**
- [android-ui.md](todo/android-ui.md) — Android look-and-feel findings and backlog
- [kiwisdr-ui.md](todo/kiwisdr-ui.md) — KiwiSDR map selector UI/UX
- [band-picker.md](todo/band-picker.md) — F-INP band grid; acceptance checklist still open
- [eibi-schedules-module.md](todo/eibi-schedules-module.md) — EiBi station schedules overlay module
- [cleanup.md](todo/cleanup.md) — mechanical improvements worth doing in passing, with what they actually cost

**Bugs**
- [ui-thread-sync.md](bugs/ui-thread-sync.md) — cross-thread GUI/DSP mutation; full fix postponed
- [state-ownership-bypass.md](bugs/state-ownership-bypass.md) — split module/VFO/UI/config ownership and bypass audit
- [code-review-2026-07.md](bugs/code-review-2026-07.md) — whole-app review at `765bf8a9`
