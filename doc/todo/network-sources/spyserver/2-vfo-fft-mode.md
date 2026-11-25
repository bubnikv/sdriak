# SpyServer 2: VFO+FFT mode in the source module

Date: 2026-11-24. Status: implemented, not committed; GUI untested.
Where the code differs from this plan:

* `vfoIqRateId` starts at the widest stage at or below 192 kS/s, not at
  0, which would be the device's full rate.
* In VFO+FFT mode `tune()` sends `FFT_FREQUENCY` together with
  `IQ_FREQUENCY`. The device follows the FFT centre, and the IQ must stay
  inside the device's band (Phase 0). SpyServer 3 adds the rate limit.
* The link stays in bypass: there is no buffer control yet.
* The digital-gain keys are created, but their UI comes with the servo in
  SpyServer 4. Both modes still send `computeDigitalGain()`.
* The Advanced settings are shown in VFO+FFT mode: FFT dB offset (0–100),
  dB range (10–150) and the spectrum calibration (±60 dB, stored for
  SpyServer 3).

Part of [README.md](README.md).
Prerequisites: SpyServer 1; core 1.

## File

`source_modules/spyserver_source/src/main.cpp`

## Config

New per-device keys, created with `ensure()` next to `sampleRateId`,
`sampleBitDepthId` and `gainId`:

| key | default | meaning |
|-----|---------|---------|
| `mode` | `"iq"` | `"iq"` or `"vfo_fft"` |
| `vfoIqRateId` | 0 | narrow IQ stage, from `MinimumIQDecimation` |
| `fftZoomId` | 0 | FFT stage, from 0 |
| `fftDbOffset` / `fftDbRange` | 0 / 150 | server FFT scaling |
| `fftCalDb` | 0 | our calibration offset |
| `digitalGainAuto` | true | gain servo in `vfo_fft` (SpyServer 4) |
| `digitalGainAutoIq` | false | gain servo in `iq` (SpyServer 4) |

Switching mode never rewrites the other mode's keys.

## Rate lists (built in `pollConnection()`)

* **Full IQ:** unchanged.
* **Narrow IQ:** the same rule, with its own index.
* **FFT stages:** 0…`DecimationStageCount`, span `MaximumBandwidth / 2^i`
  (Phase 0). Pixels: the requested `FFT_DISPLAY_PIXELS` is delivered
  exactly; frames come at the server's display rate.
* **Keep the narrowest IQ stages,** which jp dropped. Test them with the
  servo instead.

## Pipeline

Route `stream` through a `PrebufferedLink` with default target 0 ms
(bypass). The marker API (core 2) then works in both modes, and the user
can add jitter buffering on poor links.

## Start in `vfo_fft` mode (jp's order; credit it)

1. IQ format, decimation, frequency.
2. FFT format UInt8, dB offset, dB range, `FFT_DISPLAY_PIXELS` = 2048,
   frequency, decimation.
3. `STREAMING_MODE = FFT_IQ`, then `GAIN`, then digital gain (SpyServer 4).
4. `STREAMING_ENABLED`.
5. Then:
   * `core::setInputSampleRate(narrowRate)`;
   * register the provider and `core::setDisplaySpan(fftSpan)`
     (SpyServer 3);
   * `vfoManager.setIQWindow(W0, usableWidth)` (SpyServer 4).

## Stop, deselect and mode switch

* **Stop and deselect:** unregister the provider, `clearDisplaySpan()`,
  `clearIQWindow()`, then `setInputSampleRate` for the mode's own rate.
* **Mode switch:** only while stopped.
* **Server mode:** the mode combo is hidden in `--server` mode, which is
  forced to `"iq"`.

## UI

```
Mode            [ Full IQ / VFO + server waterfall ]   (disabled while running)
  Full IQ:      Samplerate […]
  VFO+FFT:      IQ bandwidth […]
Sample bit depth […]   Gain [slider]
IQ digital gain  NN dB  [x] Auto   [manual slider]
▸ Advanced: FFT dB offset, FFT dB range, Spectrum calibration
Status: Connected (device) [Shared …]
```

## Review gate

* Full IQ is unchanged, and so is its config (apart from keys added by
  `ensure()`).
* VFO+FFT streams narrow IQ, which a radio can demodulate at C. The
  waterfall work comes in SpyServer 3.
