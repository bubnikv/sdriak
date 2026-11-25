# SpyServer 1: client extensions

Date: 2026-11-24. Status: implemented, not committed. The new
`source_modules/spyserver_source/tests/spyserver_client_tests.cpp` (a
loopback fake server, built with `OPT_BUILD_TESTS`) covers the steps below;
there is no live session yet. Where the code differs from this plan:

* `ping(onPong)` takes a callback per `PING`: `answered` with the `PONG`'s
  stream position, or not answered after `kPongTimeout` (1 s). Timeouts are
  checked as messages arrive, which an IQ stream does all the time.
* `PONG`s carry no id, so each answers the oldest outstanding `PING`. If a
  `PONG` is lost while a later `PING` is outstanding, that later `PONG`
  completes the older entry, late. The callers' commit then lands after
  the switch.
* The dB scale used for the conversion is set with `setFftScale()`. FFT
  settings apply one to three seconds late (Phase 0), so frames right
  after a scale change are converted with the new scale.
* In Full IQ mode the only change is the shared-mode status line.

Part of [README.md](README.md).
Prerequisite: SpyServer 0 (for the marker choice).

## File

`source_modules/spyserver_source/src/spyserver_client.{h,cpp}`. Keep the
existing body-size check and chunked `publish()`.

## Steps

1. **Thread-safe writes.** Put `m_writeMtx` around `sendCommand()`. In
   VFO+FFT mode the UI thread sends settings, and the network thread may
   send `PING`s for rule B.
2. **`CLIENT_SYNC`.** Parse it into a `SpyServerClientSync` snapshot under
   a mutex. Expose `canControl()`, the min/max IQ and FFT centres, and the
   reported IQ/FFT centres.
   * Also show the status line "Shared: device centre fixed by another
     client" in **Full IQ** mode; today it ignores `CLIENT_SYNC`.
   * Read `CanControl` from every `CLIENT_SYNC`: a second client joining
     takes control away from the first too (Phase 0).
   * `CLIENT_SYNC` follows FFT and decimation changes, not `IQ_FREQUENCY`.
     Its `IQCenterFrequency`/`FFTCenterFrequency` can be stale; trust
     `DeviceCenterFrequency`.
3. **FFT messages.**
   * `UINT8_FFT`: convert to dB as `−offset − range · (1 − b/255)`
     (Phase 0: a larger offset brightens the bins; jp's sign is reversed). The
     requested `FFT_DB_OFFSET`/`FFT_DB_RANGE` are atomics updated by the
     module. Hand the bins to a callback with the arrival time and the
     **committed FFT geometry** (SpyServer frames carry none).
   * `DINT4_FFT`: log once and ignore. Only UInt8 is ever requested.
4. **Markers (rule B, Phase 0).**
   * The `PONG` that follows the setting's `PING` commits it: the IQ
     switch lands at the `PONG`'s position in the stream (within the
     16–18 samples of the analysis filter, at 114 kS/s).
   * Report the stream position: the sample index of the first IQ sample
     after the `PONG`.
   * A `PING` can go unanswered (one in six in Phase 0): after about 1 s
     without its `PONG`, fall back to rule C.
5. **IQ level meter.** From jp: peak and railed-sample counters on raw
   UInt8/Int16 values (rails 0/255 and ±32767), accumulated per message
   and read-and-reset under a mutex (`readIqMeter()`).
6. **Sample index.** Count published samples, so markers and the
   `PrebufferedLink` (SpyServer 2) share one index.

## Review gate

* In Full IQ mode the client behaves exactly as today, apart from the
  shared-mode status line.
