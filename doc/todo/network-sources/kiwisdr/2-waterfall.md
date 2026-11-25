# KiwiSDR 2: server waterfall (W/F)

Date: 2026-11-24. Status: implemented, not committed; the GUI is untested.
`websock_tests` covers frame parsing, a fake W/F + SND session and a busy
W/F; `--kiwi-waterfall host:port [kHz]` ran live on three receivers
(research §8.7). Where the code differs from this plan:

* The client is `KiwiWaterfallClient` in
  `core/src/utils/proto/kiwisdr_wf.{h,cpp}`. `KiwiSDRClient::waterfall`
  points it at the session.
* Besides the commands below, it sends `SET ident_user=sdriak` and
  `SET maxdb=-10 mindb=-110`, as kiwiclient does. SND sends `ident_user`
  too.
* When the krec W/F gets no answer at all, W/F is retried on
  `/kiwi/<ts>/W/F` (`p=#`), and SND then goes straight to `/kiwi/`.
* `ip_limit` (the address used up its daily time) is a refusal.
* The source runs in `SourceHandler::uiTickHandler`. The IQ window is
  `audio_rate − 200` wide; kiwisdr/3 places it.
* A W/F that fails mid-session is left to core's fallback, which shows the
  notice. The provider registers again only in the next session.
* The "Server waterfall" checkbox is locked while playing, because W/F
  opens with the session. The speed applies live.
* `seq → playout time` uses the packet's first-sample index, counted by
  the source as it pushes samples into the link (kiwisdr/3).

Part of [README.md](README.md).
Prerequisites: KiwiSDR 1; core 1, 3, 4, 5.

## Protocol facts (kiwisdr-streams.md §3, §8)

* **Connection.** A second websocket `…/<ts>/W/F` with the **same `ts`**
  as SND; the server pairs them to one channel. **Open W/F first**: it
  claims a waterfall-capable channel; SND then joins it.
* **Setup message:** `MSG … wf_setup` with `zoom_max`, **`zoom_cap`**
  (requests above it are silently served at the cap), `wf_chans`,
  `wf_cal`, `bandwidth` (30 or 32 MHz) and `wf_fps`.
* **Commands:**
  * `SET wf_speed=1..4` (1/5/13/23 frames/s; zoom 11 tops out at about 6);
  * `SET interp=13`;
  * `SET wf_comp=0|1`;
  * `SET zoom=<z> cf=<kHz>` (baseband: RF − `freq_offset`; servers before
    v1.329 take `start=`);
  * `SET keepalive`.
* **Frame:** `"W/F"`, pad byte, `x_bin` (LE32), `flags|zoom` (LE32),
  `seq` (LE32), then 1 024 bytes.
  * `start = x_bin · bandwidth / (1024 · 2^zoom_max)`;
    `span = bandwidth / 2^zoom`.
  * Bin i covers `[start + i·w, start + (i+1)·w)`, about ±0.25 bin.
  * Value: **dBm = byte − 255 + wf_cal** (checked against the S-meter
    within 1 dB).
  * Frames are clamped inside the band: `cf` is not honoured at the edges.
  * About one frame with the old geometry follows a zoom change.
* **Sync:** `seq` = the SND `seq` of the audio packet leaving when the line
  was computed (median offset 0). Not on Kiwi-compatible servers.
* **Refusal** when no waterfall channel is free: `MSG too_busy`, a silent
  close, or `wf_chans=0` on a web-UI path.

## Steps

1. **`KiwiWaterfallClient`** (new class in `kiwisdr.{h,cpp}`, or new
   `kiwisdr_wf.{h,cpp}`). It owns a `WSClient`:
   * auth and ident (`SERVER DE CLIENT sdriak W/F`), keepalive;
   * on `wf_setup`: store the fields; send `wf_speed` (user setting,
     default 4), `interp=13`, `wf_comp=0`, and `SET zoom` for the current
     view;
   * parse frames into
     `{x_bin, zoom, flags, seq, bytes[1024], arrival}`;
   * expose `available()` (false after a refusal, close or
     `wf_chans == 0`) and the setup fields.
2. **Session order in `KiwiSDRClient::start()`** when the server waterfall
   is enabled:
   1. probe the base URL and `ts`;
   2. open W/F, then wait up to 3 s for `wf_setup` or a refusal;
   3. open SND with the same `ts`.

   If W/F fails, continue with SND alone: today's behaviour, with the
   local FFT.
3. **`KiwiSpectrumProvider : SpectrumProvider`** (in `kiwisdr_source`):
   * `name()`: `"KiwiSDR"`.
   * `zoomSpans()`: `bandwidth / 2^z` for z = 0…`zoom_cap`.
   * `requestSpan(s)`: pick the smallest z with `span_z ≥ s`, then send
     `SET zoom=z cf=(C_hw − freq_offset)/1e3`. Rate-limit to 8 per second
     and compare values, so a send that was held back is retried.
   * The confirmed geometry comes from the frames (`x_bin`, zoom) and the
     `MSG zoom= start=` echo. On a zoom change call
     `core::setDisplaySpan(span_z)`.
   * `available()` forwards the client's state.
4. **Frames to core.** On the W/F network thread:
   * convert to dB;
   * `startHz` = `start + freq_offset + sourceManager.getTuningOffset()`;
   * when Invert IQ is on, reverse the bins and mirror `startHz` around C;
   * presentAt per §5;
   * `iqFrontEnd.pushSpectrum(...)`.
5. **Presentation.**
   * For each SND packet pushed into the `PrebufferedLink`, record
     `seq → link.playoutTime(firstSampleIndex)` in a ring of 256.
   * A frame's `presentAt` is the recorded time for its `seq`.
   * When the `seq` is unknown, or the flavour is `KiwiCompatible`: arrival
     plus `bufferedSamples / sampleRate`.
   * At deep zoom a line lags about 1 s by integration. Accept that.
6. **Source integration** (`kiwisdr_source`):
   * a **"Server waterfall"** checkbox (default on; hidden for
     `websdr_view`, which never uses it);
   * a **speed** combo;
   * on start, when available, register the provider
     (`iqFrontEnd.setSpectrumProvider`), the initial span (zoom 0, or the
     saved zoom) and the window (KiwiSDR 3);
   * on stop or deselect: unregister, `clearDisplaySpan`, `clearIQWindow`.
7. **Later:** `wf_comp=1` with the ADPCM decoder (drop the **first** 10
   decoded samples; kiwiclient drops the last ten, which is a bug); the
   `aper` auto-scale hint for auto-range.

## Review gate (user tests)

* **Waterfall span:** the waterfall shows 0–30 MHz at zoom 0; zooming
  steps down to `zoom_cap` and stays centred on C. At band edges frames
  are partial and filled with the floor.
* **Levels:** a carrier's level matches the S-meter within a few dB.
* **Busy fallback:** on a Kiwi whose waterfall channels are busy, the
  source falls back to the local FFT with a notice, and the audio keeps
  playing.
* **Alignment:** with a 250 ms prebuffer, waterfall lines line up with the
  audio by ear (CW keying).
