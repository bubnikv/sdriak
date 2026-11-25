# UberSDR 3: server waterfall (`/ws/user-spectrum`)

Date: 2026-11-24. Status: implemented and built, not committed; not run
against a server or in the GUI.
`source_modules/ubersdr_source/tests/ubersdr_spectrum_tests.cpp` covers the
v2 parser (full and delta frames, the rotation, a broken delta chain) and
core's new `utils/gzip` (zlib, linked PRIVATE into core). Where the code
differs from this plan:

* **Bin centring:** bin k is taken as *centred* at
  `centre + (k − N/2)·bw`, the FFT convention, so
  `startHz = centre − (N/2 + ½)·bw`.
* **First view:** the spectrum socket starts on the server's default view.
  The zoom ladder runs from that view's `totalBandwidth` down to 1 024 ×
  0.5 Hz in steps of √2.
* **View sends:** `zoom` is sent when the bin width changes, `pan` when only
  the centre does. Both come from the UI tick, at most 5 per second, with
  2 s of quiet after a 429.
* **Presentation:** each IQ packet's header timestamp is recorded with its
  link input index (a ring of 512). A frame's timestamp is interpolated
  between them, or extrapolated at the stream rate, and played out with
  `link.playoutTime()`.
* **IQ window:** 90 % of the IQ rate wide; UberSDR 4 places it.
* **Fallback:** when core takes the provider away (no frames for 3 s), the
  source closes its secondaries, retunes the primary to the display centre
  and does not register again until the next session, as kiwisdr_source
  does.

Part of [README.md](README.md).
Prerequisites: UberSDR 2; core 1, 3, 4, 5.

## Protocol (ubersdr.md §3, §4, §4.1)

* **URL:**
  `wss://host:port/ws/user-spectrum?user_session_id=<id>&mode=binary8&version=2[&frequency=<Hz>&bin_bandwidth=<Hz>]`.
  Same session id as the IQ socket.
* **Control replies** are **binary frames holding gzip-compressed JSON**
  (`1f 8b`):
  * `config`: `centerFreq`, `binCount`, `binBandwidth`, `totalBandwidth`,
    …;
  * `pong`;
  * `error` (with `status: 429` on the rate limit).

  Gunzip with zlib (bundled in `deps/+zlib`), or libcurl's, whichever is
  linked into core.
* **Spectrum frames v2:**
  * Header (24 bytes, little-endian): `"SPEC"`, version 2, flags
    (`0x05` full, `0x06` delta), `uint16 seq`, `uint64 timestamp_ns`,
    `uint64 centre_hz`.
  * Full frame: `int16 refCentiDB`, `uint8 stepCentiDB`, then one byte per
    bin; `dB = ref/100 + code·step/100`.
  * Delta frame: a bitmask of ⌈N/8⌉ bytes, then the codes of the set bits,
    relative to the last full frame's scale. Keyframes come at least every
    50 frames.
  * **Bins are in FFT order: rotate by N/2.** After rotation, bin k covers
    `centre + (k − N/2)·binBandwidth`.
  * About 10 frames/s; 1 024 bins down to 0.5 Hz bins; about 5.5 kB/s.
* **Commands** (JSON text):
  * `{"type":"zoom","frequency":f,"binBandwidth":b}` and `pan` (the same
    fields);
  * `set_rate` with a `divisor` of 1–8;
  * `reset`, `get_status`, `ping`.
* **Edges.** The server clamps the view so the span stays inside
  0–30 MHz, and frames report the clamped centre. One request near 0 Hz
  got **no reply at all**, so never wait on a reply.
* **Levels:** bins are front-end dBFS (within 1.7 dB of the IQ header's
  `baseband_power`). The IQ samples carry about 50 dB more gain.

## Steps

1. **`UberSpectrumClient`** (`ubersdr_client.{h,cpp}`): a second
   `WSClient`.
   * Parse the gzip JSON replies and the v2 frames.
   * Keep the current full-frame codes and scale for deltas. On a
     sequence gap, mark the frame stale until the next full frame (do not
     draw from a broken delta chain).
   * Ping every 30 s.
2. **`UberSpectrumProvider : SpectrumProvider`:**
   * `name()`: `"UberSDR"`.
   * `zoomSpans()`: from `totalBandwidth` at the default view (30 MHz)
     down to 1 024 × 0.5 Hz, in steps of ×√2.
   * `requestSpan(s)`: send `zoom` with `frequency = C_hw` and
     `binBandwidth = s/1024`. At most 5 per second; ignore 429 with a
     back-off.
   * Confirmation: on a `config` reply call
     `core::setDisplaySpan(binCount·binBandwidth)`. Frames carry their own
     centre either way.
3. **Frames to core** (network thread):
   * rotate the bins;
   * `startHz = centre − N·bw/2 + tuningOffset`;
   * Invert IQ: reverse the bins and mirror `startHz` around C;
   * `presentAt` from the timestamp: map the frame's `timestamp_ns`
     through the source's record of IQ header timestamps to
     `link.playoutTime(index)`, interpolating between packets. Spectrum
     frames were measured to arrive 26–31 ms earlier than IQ relative to
     their timestamps, so the mapping holds them correctly.
   * `pushSpectrum`.
4. **`tune()`** sends `pan` with the new C as well as the IQ retune.
5. **Levels:** the provider default is `min −130`, `max −60` (front-end
   dBFS). Per-provider storage is core 5.

## Review gate (user tests)

* **Waterfall span:** the waterfall shows 0–30 MHz, and zoom steps
  smoothly down to sub-Hz bins.
* **Pan:** panning near 0 Hz or 30 MHz shows the clamped view correctly.
* **Alignment:** waterfall lines line up with the audio (CW).
* **Gaps:** a forced 5 s stall (suspend the app on Android) recovers with
  no corrupted frames.
