# SpyServer 3: server waterfall provider

Date: 2026-11-24. Status: implemented, not committed; GUI and live
untested. Where the code differs from this plan:

* **Confirmation:** a view send is committed by the first `CLIENT_SYNC`
  after it (`SpyServerClientClass::syncCount()`), or after 3 s. The plan
  used the `PONG`. In Phase 0 the sync came 0.3–1.4 s after FFT changes,
  about when they took effect, while a `PONG` answers at once. The commit
  runs on the UI tick, which also calls `core::setDisplaySpan`.
* **FFT centre:** in VFO+FFT mode `tune()` sends only `IQ_FREQUENCY`. The
  FFT centre follows from the UI tick, at most 8 sends per second.
* **Clamping:** requests are clamped to the last `CLIENT_SYNC`'s FFT
  centre limits, because the server ignores out-of-range requests.
* **IQ window:** a fixed window at W = 0 is set, with width
  `IQ rate × MaximumBandwidth / MaximumSampleRate`. VFOs outside it are
  marked and muted until SpyServer 4 moves it.
* **Stored stage:** the committed stage is stored as `fftZoomId`.
* **No helper:** there is no "Match Full IQ levels" helper. The
  calibration slider is ±60 dB.

Part of [README.md](README.md).
Prerequisites: SpyServer 2; core 3, 4, 5.

## `SpyServerSpectrumProvider : SpectrumProvider` (in `spyserver_source`)

* **`name()`:** `"SpyServer"`.
* **`zoomSpans()`:** the FFT stage spans, widest first.
* **`requestSpan(s)`:** pick the narrowest stage with `span ≥ s`; send
  `FFT_DECIMATION` and `FFT_FREQUENCY = C_hw`. Rate-limit to 8 per second
  and compare values (jp's fix: a send that was held back is retried).
* **Confirmed geometry.** Frames carry no geometry, so the client tags each
  frame with the **committed** FFT centre and stage. A request is
  committed by its `PONG` (rule B); under rule C, after the timeout. FFT
  settings take effect one to three seconds after the command (Phase 0),
  so the `PONG` alone may come early: confirm a span change from the
  frames' statistics or from the `CLIENT_SYNC` that follows FFT changes.
  * On commit: `core::setDisplaySpan(span)`.
  * Frames that arrive before the commit keep the old geometry. The pump
    maps them, so a pan shows no wrongly placed lines.
* **`available()`:** false when the connection drops, or when the server
  stops sending FFT frames for 3 s.

## Frames to core (network thread)

* **Scale.** `dB = −offset − range · (1 − b/255)` (Phase 0), plus
  `fftCalDb`.
* **Geometry.** Bin n/2 is centred on the FFT centre (Phase 0), so
  `startHz = committedCentre − span/2 − span/(2n) + tuningOffset` for the
  lower edge of bin 0.
* **Invert IQ.** When on, reverse the bins and mirror `startHz` around C.
* **Presentation.** `presentAt` = arrival + the link's buffered duration.
  There is no sync key: SpyServer has no timestamps.
* `pushSpectrum`.

## Levels

* **Server range.** `FFT_DB_OFFSET`/`FFT_DB_RANGE` set the 8-bit
  quantisation window (150 dB over 256 steps = 0.6 dB per step). Keep them
  under "Advanced".
* **Calibration slider.** `fftCalDb`, ±40 dB.
* **"Match Full IQ levels" helper (optional):**
  1. in Full IQ mode, remember the median of the local spectrum
     (`acquireLatestFFT`) at this centre;
  2. in VFO+FFT, compare it with the server spectrum's median over the
     same span and set `fftCalDb`.

  This matches noise floors, not carriers, because noise per bin depends
  on bin width.
* **Storage** is per provider (core 5).

## Review gate (user tests)

* **Devices:** Airspy R2 (uncapped and capped) and HF+.
* **Display:** the waterfall shows the stage span; zoom steps through the
  stages; pan and Invert IQ display correctly.
* **Levels:** after calibration, the waterfall levels are comparable to
  Full IQ.
