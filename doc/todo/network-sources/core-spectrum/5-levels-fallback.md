# Core 5: per-provider levels, fallback, wideband consumers

Date: 2026-11-24. Status: implemented, not committed; no caller yet.
Where the code differs from this plan:

* `MainWindow::spectrumProviderTick()` runs each UI tick before the zoom
  tick and the frame pump. It falls back, and it switches levels whenever
  the provider's `name()` changes (or "local" without a provider).
* `SpectrumProvider` gains `unavailableReason()` and `defaultLevels()`,
  both with defaults.
* `WaterfallAutoRange` no longer writes config. It calls the `persist`
  callback that `MainWindow` passes to `init()`.
* The legacy `"min"`, `"max"` and `"waterfallAutoRange"` keys seed
  `waterfallLevels.local` once and are no longer written.
* The narrow-IQ warning is `ImGui::NarrowIQNotice()`
  (`core/src/gui/widgets/narrow_iq_notice.h`), shown by the recorder and
  the IQ exporter in baseband mode and by the scanner.
* The CW skimmer has no module menu in the tree yet (`cw_engine` holds only
  the engine and its tests), so it has no warning.
* The fallback notice stays in the source menu until a provider registers
  again.

Part of [README.md](README.md).

## Adds

### Waterfall levels per provider (`main_window.cpp`, `waterfall_autorange.*`)

* **Why.** The scales differ by tens of dB:
  * the local FFT reads in dBFS;
  * KiwiSDR bins are absolute dBm (`byte − 255 + wf_cal`);
  * UberSDR bins are front-end dBFS;
  * SpyServer bins use a server-chosen offset and range.
* **Storage.** Today `fftMin`/`fftMax` and the sticky auto-range state are
  global config keys (`"min"`, `"max"`). Store them per provider in a core
  config section `waterfallLevels[<provider name or "local">]`, with
  fields `min`, `max` and `sticky`.
* **On provider change** (`setSpectrumProvider`): save the current set,
  load the new provider's set, or seed it from a default the provider
  declares. Then call `autoRange.init(...)` again.
* **Migration.** The existing `"min"`/`"max"` become the `"local"` entry
  the first time.

### Fallback (`IQFrontEnd` / `MainWindow`)

* **Trigger.** On each UI tick, if the registered provider reports
  `available() == false`, call `setSpectrumProvider(nullptr)`,
  `clearDisplaySpan()` and `vfoManager.clearIQWindow()`, and restore the
  local levels.
* **The source decides** when to register again (for example, a KiwiSDR
  W/F reconnect).
* **Notice.** Show a one-line status in the source menu: "Server waterfall
  unavailable: <reason>".

### Consumers that need the whole band

* **Signal.** `IQFrontEnd::isIQNarrow()` and `Event<bool> onNarrowIQChanged`,
  fired by `setIQWindow` and `clearIQWindow`.
* **Warn, don't refuse.** The baseband recorder (`misc_modules/recorder`),
  `iq_exporter`, the scanner and the CW skimmer (`decoder_modules/cw_engine`)
  show "IQ is a narrow server window (W ± width/2); baseband covers only
  that". They keep working.
* **The scanner** reads `acquireLatestFFT()`, so it sees the server
  spectrum. Its VFO retunes make the window follow. Test it.

## Review gate

* Switching between a local source and a provider source keeps each one's
  levels.
* Pulling the provider (debug provider toggled unavailable) restores the
  local FFT within one frame, with no stuck span.
