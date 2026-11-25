# Core 4: display span and server zoom

Date: 2026-11-24. Status: implemented, not committed; no caller yet.
Where the code differs from this plan:

* The ladder state is the pure `ServerZoom` class
  (`core/src/signal_path/server_zoom.h`), owned by `MainWindow`;
  `core/tests/spectrum_frame_tests.cpp` covers wheel, pinch, the 150 ms
  step and the slider.
* The waterfall reaches it through `WaterFall::zoomOverride`, which
  consumes the zoom wheel and pinch while the ladder has more than one rung.
* `setInputSampleRate()` keeps the waterfall's own VFO clamp, as today.
  `clampVFOsToSpan()` runs at init and from `setDisplaySpan` and
  `clearDisplaySpan`.
* There is no debug provider (see core 3), so the review-gate run with a
  3-rung ladder waits for kiwisdr/2.

Part of [README.md](README.md).

## Adds

### Display span owned by the source (`core/src/core.{h,cpp}`, `IQFrontEnd`)

* **`core::setDisplaySpan(double spanHz)`.** It sets an override in
  `IQFrontEnd` and resets zoom: `waterfall.setBandwidth`,
  `setViewBandwidth` and `mainWindow.setViewBandwidthSlider(1.0)`, as
  `setInputSampleRate()` does. `core::clearDisplaySpan()` removes the
  override.
* **`core::setInputSampleRate()`** uses the override, when set, for the
  waterfall bandwidth. It keeps using the effective rate for everything
  else. This replaces jp's call-order dependence and his
  `supportsPostDecimation` flag; the decimation control keeps working.
* **Re-clamp VFOs on every span change.** `main_window.cpp` has a
  "correct the offset of all VFOs so they fit on the screen" block after
  `init()`. Extract it as `MainWindow::clampVFOsToSpan()` and call it from
  both span setters.
* **Server mode.** Both calls are no-ops in `--server` mode.

### Server zoom routing

When `provider->zoomSpans().size() > 1`:

* **Wheel and pinch.** In `waterfall.cpp` (zoom block around line 564):
  instead of changing `viewBandwidth`, accumulate the zoom wheel and, at
  most every 150 ms, step one rung in `zoomSpans()`, then call
  `provider->requestSpan(next)`.
* **Zoom slider** (`main_window.cpp` around line 1048): map its position
  to a rung.
* **The view equals the span.** The local view bandwidth stays at the full
  span (`viewOffset = 0`), so the drawn span is exactly what the server
  computes.
* **Panning** is unchanged: dragging the scale retunes C, and the provider
  re-centres on `tune()`.
* **The confirmed span** comes back from the provider through
  `core::setDisplaySpan()`, when the server confirms it (KiwiSDR echo,
  UberSDR `config`, SpyServer marker).

### History on a span change

Add `WaterFall::resetHistory()`: it clears the stored raw lines (filled
with −200 dB) under `buf_mtx`. `core::setDisplaySpan` calls it whenever
the span changes. Resampling history into the new geometry is a later
refinement.

## Review gate

* With no provider, wheel, pinch and the slider behave exactly as today.
* With the debug provider from core 3 declaring a 3-rung ladder: the
  wheel steps the rungs, the waterfall history clears per rung, and VFOs
  stay inside the span.
