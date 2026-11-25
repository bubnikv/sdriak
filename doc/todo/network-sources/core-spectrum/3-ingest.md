# Core 3: spectrum frame ingest and presentation

Date: 2026-11-24. Status: implemented, not committed; no caller yet.
`core/tests/spectrum_frame_tests.cpp` covers the queue and the mapping.
Where the code differs from this plan:

* The ring and the mapping are header-only in
  `core/src/signal_path/spectrum_frame.h` (`SpectrumQueue`,
  `mapSpectrumFrame`); `IQFrontEnd` owns a queue and forwards to it.
* The nominal bin count is `SpectrumProvider::binCount()`.
* Display bins are filled nearest neighbour by bin centre in every case;
  with equal span and size that is the whole-bin shift.
* There is no synthetic debug provider. The mapping is checked by unit
  tests, and visually once kiwisdr/2 delivers frames.

Part of [README.md](README.md).

## Adds

### Frame and queue (`IQFrontEnd`)

```cpp
struct SpectrumFrame {
    std::vector<float> bins;   // dB, ascending frequency, display domain
    double startHz;            // lower edge of bins[0], display domain (absolute Hz)
    double spanHz;             // width covered by bins
    std::chrono::steady_clock::time_point presentAt{};  // epoch = show at once
};
// Any thread. Never blocks: try_lock on m_spectrumMtx; on contention or a
// full ring the frame is dropped (counted).
bool pushSpectrum(SpectrumFrame&& frame);
// UI thread.
bool popSpectrum(SpectrumFrame& out);   // only frames whose presentAt has passed
```

* **The ring.** 16 preallocated slots. When it is full, drop the oldest.
  `droppedSpectrumFrames()` exposes the drop counter for a debug line.
* **The queue is ordered.** `popSpectrum` stops at the first frame that is
  not yet due.

### Pump (`MainWindow::draw()`, after the source UI tick, before the waterfall draws)

1. **Pop** every frame that is due.
2. **Raw line size.** Set `rawFFTSize` to the provider's nominal bin
   count. Call `waterfall.setRawFFTSize()` only when that count changes:
   it reallocates history under `buf_mtx`, and the UI thread already owns
   that mutex.
3. **Map the frame** `[startHz, startHz+spanHz]` onto the display
   `[C − S/2, C + S/2]`:
   * when the frame spans S, shift by whole bins;
   * otherwise resample by nearest neighbour;
   * fill uncovered bins with −200 dB (KiwiSDR clamps frames at band
     edges; zoom transitions leave frames in flight);
   * drop a frame that overlaps less than half the display.
4. **Write** into `getFFTBuffer()`, then `pushFFT()`.
5. **Display menu.** While a provider is active, FFT size, rate and window
   are stored but do not reallocate (a guard in `updateFFTPath`). The menu
   shows them greyed, with "Spectrum supplied by the source".

### Presentation time

Waterfall lines should appear when the matching audio is heard. The source
computes `presentAt`, because only it knows the protocol's sync key:

* **KiwiSDR:** the SND `seq` echoed in each W/F line.
* **UberSDR:** the server timestamp shared by IQ packets and spectrum
  frames.
* **SpyServer:** no key; use arrival time.

The pattern for all three: when an IQ packet is pushed into the
`PrebufferedLink`, record `key → link.playoutTime(indexOfFirstSample)`. A
frame with key k gets `presentAt` = the recorded time for k, interpolated.
With no key, use arrival time plus the link's current buffered duration.

## Review gate

* Without a provider, the pump does nothing and the waterfall behaves as
  today.
* A debug-only synthetic provider (behind `#ifndef NDEBUG`, removed before
  merge, or kept as a dev menu entry) can push test frames: a moving
  carrier with the frame's start offset by half a span. The mapping and
  fill can then be checked visually.
