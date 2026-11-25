# Core 1: spectrum capability and source UI tick

Date: 2026-11-24. Status: implemented, not committed; no caller yet. The
frame queue does not exist until core 3, so `setSpectrumProvider` only
unbinds and rebinds `fftIn`. The whole bench tree builds (every source
module against the new `SourceHandler`), and ctest passes apart from the
four libcorrect entries that never run in this build. Part of
[README.md](README.md).

## Adds

### `core/src/signal_path/spectrum_provider.h` (new)

```cpp
// A source that supplies the waterfall itself (server-side spectrum).
// All methods are called on the UI thread.
class SpectrumProvider {
public:
    virtual ~SpectrumProvider() = default;
    // Stable key, e.g. "KiwiSDR"; used for per-provider waterfall levels.
    virtual std::string name() const = 0;
    // Nominal bins per frame: the waterfall's raw line size (core 3).
    virtual int binCount() const = 0;
    // Server zoom ladder: display spans in Hz, widest first. One entry
    // means "no server zoom" (local view zoom stays active).
    virtual std::vector<double> zoomSpans() const = 0;
    // Ask for a view of span `spanHz` centred on the display centre C.
    // The provider quantises, rate-limits and sends; the confirmed span
    // arrives through core::setDisplaySpan() (core 4).
    virtual void requestSpan(double spanHz) = 0;
    // True while the server spectrum is usable; false asks core to fall
    // back to the local FFT (core 5).
    virtual bool available() const = 0;
};
```

### `IQFrontEnd` (`core/src/signal_path/iq_frontend.{h,cpp}`)

* `void setSpectrumProvider(SpectrumProvider* p)`: UI thread; `nullptr`
  restores the local FFT.
  * **Non-null:** unbind `fftIn` from `split` (no FFT work, no splitter
    copy) and empty the frame queue.
  * **Null:** rebind `fftIn` and run `updateFFTPath(true)`.
* `SpectrumProvider* spectrumProvider() const`
* `bool getInvertIQ() const`, a getter for the existing `conjugate` state.
  It replaces jp's `extern sourcemenu::invertIQ`.

### `SourceManager` (`core/src/signal_path/source.{h,cpp}`)

* `double getTuningOffset() const`: providers need it to convert hardware
  frequencies to the display domain.
* `SourceHandler::uiTickHandler`: `void (*)(void* ctx)`, default
  `nullptr`.
  * `SourceManager::uiTick()` calls it for the selected source.
  * `MainWindow::draw()` calls `uiTick()` once per frame, right after
    `vfoManager.updateFromWaterfall()` (main_window.cpp, around line 368).
  * This is where a source runs its window policy and reads VFO geometry,
    on the thread that owns it. It replaces jp's 40 ms poll thread.
* Sources assign their handler fields one by one. Adding a defaulted
  member must not break any source, so check all `SourceHandler`
  initialisations compile.

### `--server` mode

`setSpectrumProvider` and the UI tick are GUI-only. In server mode,
sources never register a provider. Document that in the header.

## Review gate

* No caller yet: the diff only adds declarations and the tick call.
* `uiTickHandler` is null for every existing source.
