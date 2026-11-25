# Core: server-side spectrum, IQ window and server zoom

Date: 2026-11-24. Status: current. Part of [../README.md](../README.md).

The core architecture that lets a networked source supply the waterfall and
own the position of its IQ window. Every piece is a **no-op until a source
uses it**: with no provider registered, behaviour must be byte-for-byte
today's.

Design rationale: [../../../design/dsp-pipeline.md](../../../design/dsp-pipeline.md)
§10. It explains why jp's hooks (one VFO, a flag with two meanings,
`buf_mtx` taken on the network thread) are not copied.

## Pieces

| plan | adds | used by |
|------|------|---------|
| [1-capability.md](1-capability.md) | `SpectrumProvider` interface, provider registration on `IQFrontEnd`, `SourceHandler::uiTickHandler`, `SourceManager::getTuningOffset()`, `IQFrontEnd::getInvertIQ()` | all |
| [2-iq-window.md](2-iq-window.md) | IQ window offset in `VFOManager` (one place sets the DSP offset), out-of-window mark and mute, stream-position commit via `PrebufferedLink` markers, the shared window-placement policy | all |
| [3-ingest.md](3-ingest.md) | `SpectrumFrame` queue (never blocks the producer), UI-thread pump, geometry mapping, presentation time from the prebuffer's playout clock | all |
| [4-span-zoom.md](4-span-zoom.md) | display span owned by the provider, server-zoom ladder routed from the wheel, pinch and slider, waterfall history reset on a span change | all |
| [5-levels-fallback.md](5-levels-fallback.md) | waterfall levels per provider, fallback to the local FFT, warnings for consumers that need the whole band, Display-menu state | all |

Implement in that order. Stop for review after each.

## Invariants (keep them true in every step)

1. **Threads.** The UI thread is the only writer of waterfall state,
   `WaterfallVFO`s and the `VFOManager` map. Network and DSP threads touch
   only atomics, the frame queue, or marker callbacks that write atomics.
2. **The display centre is C** (`sourceManager.tune`). With server zoom
   the view always spans the provider's span centred on C. Panning
   retunes C exactly as dragging the scale does today.
3. **The DSP offset of every VFO is `centerOffset − W`.** It is computed
   in one function. W = 0 whenever no source manages a window.
4. **Providers deliver frames in the display domain,** ascending
   frequency, with any FFT-order rotation, Invert IQ and converter offset
   already applied.
5. **A network thread never waits on the UI,** and the UI never waits on
   a network thread.
