# UberSDR: directory, protocols, and what it would take to support

Date: 2026-11-24.

[UberSDR](https://ubersdr.org/) ([`madpsy/ka9q_ubersdr`](https://github.com/madpsy/ka9q_ubersdr),
GPL-3.0, Go, v0.1.69) is a multi-user web SDR built on
[ka9q-radio](https://github.com/ka9q/ka9q-radio). Its instances typically
run RX888-class front ends sampling the whole 0–30 MHz band at 64.8 MS/s,
and allow 10–100 users each.

We first met it as `ea8bfk.ddns.net` in the KiwiSDR list
([kiwisdr-streams.md §8.5](kiwisdr-streams.md#85-second-batch-on-a-handful-of-receivers)).
That is UberSDR's optional KiwiSDR emulation, which reports itself as a
KiwiSDR 2.

## 1. Directory

**Endpoint.** `GET https://instances.ubersdr.org/api/instances` returns
JSON `{count, instances[]}`, about 500 kB. It is the same endpoint the
project's own Python client uses (`clients/python/public_instances_display.py`).
On 2026-11-24 it listed **77 instances, 76 online**.

**Fields useful to a map** (per instance):

| field | meaning |
|-------|---------|
| `latitude`, `longitude`, `maidenhead`, `location`, `country_code` | position |
| `callsign`, `name` | label |
| `host`, `port`, `tls`, `public_url` | connection: **74 of 77 use TLS** |
| `max_clients`, `available_clients` | capacity (20 is typical, up to 100) |
| `public_iq_modes` | IQ widths open to the public: `iq48` on all, `iq96`–`iq384` on a few |
| `tuning_range` | `min_frequency`, `max_frequency`, `spectrum_span_hz`, `input_samprate` |
| `snr_0_30_mhz`, `snr_1_8_30_mhz`, `noise_floor`, `antenna_connected` | quality |
| `cw_skimmer`, `digital_decodes`, `tdoa_enabled`, `gps_enabled` | features |
| `is_online`, `last_seen`, `version`, `load_status` | health |
| `ssb_predictions` | per-band propagation predictions (bulky) |

**Overlap with the KiwiSDR list.** Only **3 of the 77** also appear in the
kiwisdr.com list: those with the Kiwi emulation enabled. In the KiwiSDR
list they show up as "KiwiSDR 2" entries with 20–100 users.

## 2. Ways in

| route | width | status |
|-------|-------|--------|
| **KiwiSDR emulation** (`kiwi_websocket.go`) | 12 kS/s SND, plus the 1 024-bin Kiwi waterfall | **opt-in**: 4 of 4 randomly drawn directory-only instances answered `/VER` and `/status` with `404`. Even where enabled it differs from KiwiSDR: no `NEW_FREQ` flag, an independent waterfall `seq`, 240-sample packets. |
| **Native WebSocket API** (`pcm_v4` compressed PCM; IQ modes `iq48`/`96`/`192`/`384`; spectrum from radiod) | 48–384 kHz IQ | the project's own route. The `/connection` precheck reports `allowed_iq_modes` per session. A C++ decoder exists in `clients/soapy_driver/pcm_v4.hpp`, with conformance tests. |
| **HPSDR bridge** (`ubersdr-hpsdr`, Go; also a C version) | 48–384 kHz per DDC, 1–10 DDCs | runs **locally** and presents an UberSDR as a Hermes Lite 2. Not pursued: we want a direct remote connection. |
| **SoapySDR driver** (`SoapyUberSDR`) | 48–384 kHz | our `soapy_source` could use it where Soapy plug-ins are available |
| TCI | 48–384 kHz | the Python TCI server in `clients/python`; we have no TCI source |

## 3. Waterfall streaming (native API)

UberSDR streams a per-user spectrum on its own websocket,
`/ws/user-spectrum` (`user_spectrum_websocket.go`, `user_spectrum_v2.go`;
`main.go` registers it next to `/ws` for audio and IQ). This is separate
from the Kiwi emulation.

* **Session.** The query string must carry `user_session_id` (a UUID),
  which ties it to the user's audio/IQ session. Optional `frequency=` and
  `bin_bandwidth=` set the initial view, and `version=2` selects the
  compact format. Connections are rate-limited per IP, and an IP or
  password bypass list exists.
* **Control** (JSON text messages): `zoom` / `pan` with `frequency` and
  `binBandwidth`, `set_rate` (poll divisor 1–8), `reset`, `get_status`,
  `ping`. Status replies carry `frequency`, `binCount` and `binBandwidth`.
* **Zoom is continuous.** It is set as a bin bandwidth, not as
  power-of-two steps. The centre is clamped to the receiver's range. For
  deep zoom the server shrinks `binCount` (down to 256) instead of going
  below a minimum bin bandwidth. The default view is a **shared** channel
  for all users; zooming or panning moves the session onto a **private**
  radiod channel.
* **Frames (v2, binary):** 24-byte header `"SPEC"`, version 2, flags
  (`0x05` full, `0x06` delta), **`uint16` sequence**, **`uint64` timestamp
  in ns**, **`uint64` centre frequency**.
  * A full frame adds `refCentiDB int16`, `stepCentiDB uint8` and one byte
    per bin: `dB = ref/100 + code × step/100`, steps of 0.25–2.55 dB
    (default 0.5).
  * A delta frame is a bitmask of changed bins plus their codes, relative
    to the last full frame.
  * A keyframe comes at least every 50 frames, so a dropped delta heals
    within about 5 s at 10 frames/s.
  * Measured by the project: about 5.2 kB/s for 1 024 bins at 10 frames/s.
  * **Bins are in FFT order** (DC first, negative frequencies in the upper
    half) and must be rotated by N/2 for display (§4.1).
* **Version 1** (the default when no version is given): full or delta
  frames with float32 or uint8 values, a 22-byte header with a timestamp in
  ms and the centre frequency, and no sequence number.

**How it fits our server-side spectrum design**
([dsp-pipeline.md §10](../design/dsp-pipeline.md#10-server-side-spectrum-for-network-sources)):

* **Geometry.** Every frame states its centre frequency, and the span
  follows from the last status (`binCount × binBandwidth`). The pump can
  place every frame exactly, including frames in flight during a pan.
* **Time.** Each frame carries a server capture timestamp (ns), which is a
  better presentation key than KiwiSDR's `seq`. It can be aligned with the
  IQ stream if that stream carries comparable timestamps (not yet checked).
* **Gaps.** The sequence number reveals lost frames, and keyframes heal
  them.
* **Levels.** The scale travels with each full frame, in dB relative to the
  receiver's spectrum scale (the code comments speak of dBFS). That is
  close to our local FFT's convention, so per-provider levels may barely
  differ.
* **Zoom.** Continuous zoom maps directly onto the zoom interface (plan
  §1.4a). The provider can offer a fine ladder of spans instead of Kiwi's
  powers of two.
* **Cost.** About 5 kB/s for the waterfall, plus 48–384 kHz of IQ.

## 4. Measurements of the native API

**Method.** Run on 2026-11-24 over a mobile-phone internet link
(round-trip time 65–680 ms, with spikes).

* **Receivers.** Three were drawn at random (seed `1795503658`) from the
  directory snapshot: online, TLS, at least 8 of 20 or more client slots
  free, `load_status=ok`, in Europe so TDF 162 kHz can serve as the test
  carrier. They were `oe6add`, `on6ure-tel` and `on6ure-btl`, all
  `*.tunnel.ubersdr.org:443`, v0.1.69.
* **Gentleness.** One session at a time, 63 s each, 20 s apart.
* **Sequence.** `GET /api/description`, then `POST /connection`, then
  `/ws` in `iq48`, then `/ws/user-spectrum?mode=binary8&version=2`. After
  a baseline, ping ×3 on each socket, 4 IQ retunes of ±2 kHz, 4 spectrum
  view changes, one burst of 5 pans at 5/s, and a `reset`.
* **Analysis.** Raw frames were recorded and decoded offline with the
  project's `pcm_v4.py`, in pure Python. My own v2 spectrum decoder ran
  with no delta-length mismatches.

**Admission** (`POST /connection {user_session_id}`), identical on all three:
`allowed: true`, `allowed_iq_modes: ["iq48"]`, `session_timeout` and
`max_session_time` 3600 s, `daily_time_remaining_secs: -1` (unlimited),
`vfos: 2`, `bypassed: false`.

**IQ (`iq48`, `format=pcm-zstd&version=4`):**

| quantity | oe6add | on6ure-tel | on6ure-btl |
|----------|--------|------------|------------|
| packet sizes (complex samples) | 240, 360 | 240, 360 | 240, 360 |
| packets without a timestamp at the start | 3 | 9 | 3 |
| rate from header timestamps (S/s) | 48 000.0012 | 48 000.2811 | 47 999.9243 |
| timestamp residual RMS | **461 ns** | 409 µs | 14 µs |
| timestamp jumps above 1 ms | 0 | 0 | 0 |
| bytes/s (lossless predictive codec) | 107 k | 135 k | 144 k |
| TDF carrier: offset, SNR, level | +6000.0 Hz, 67 dB, −30 dBFS | +6000.0 Hz, 67 dB, −27 dBFS | +6000.0 Hz, 12 dB, −65 dBFS |

* **Timestamps.** Every packet after the first few carries the timestamp of
  its first sample (ns since the epoch, "GPS-synchronised" in the decoder's
  terms). It is consistent to under a microsecond on one receiver and under
  half a millisecond on all three.
* **Status echo.** A `tune` (`{"type":"tune","frequency":…,"mode":"iq48"}`)
  is answered by a JSON `status` message with the new frequency, 70–1 117 ms
  after the command; usually 100–300 ms, the outliers coming from the
  mobile link.
* **The retune is marked in stream order.** In all 8 retunes the detector
  resolved, on three receivers, the frequency switched **at sample 224 (one
  case 223) of a 240-sample packet, two packets after the `status` echo**.
  The transition took 31–34 samples, which is the analysis filter's length,
  so the switch itself is abrupt. The other 4 were not resolved: weak
  carrier on `on6ure-btl` (SNR 12 dB), and two on `on6ure-tel`. Command to
  switch-packet arrival was 131–410 ms, with two outliers at 0.67 and
  1.12 s.

**Spectrum (`/ws/user-spectrum`, v2 `binary8`):**

| quantity | all three |
|----------|-----------|
| frame rate | 10.09–10.12 frames/s |
| bins | 1 024, at every zoom tested (100, 20 and 5 Hz bins) |
| default view | `centerFreq` 15 MHz, `binBandwidth` 29 296.875 Hz, 30 MHz span (shared channel) |
| traffic | 5.3–5.7 kB/s; full frames about 1 051 B, deltas about 545 B; a full frame every ~30 frames |
| `seq` gaps | 0 |
| quantisation step | 0.29–0.35 dB |
| control replies | **binary frames that are gzip-compressed JSON** (`1f 8b`): `config` with `centerFreq`, `binCount`, `binBandwidth`, `totalBandwidth`; and `pong` |
| view change → `config` reply | 68–874 ms |
| view change → first frame with the new centre | 24–880 ms |
| 5 pans at 5/s | no `429`, all applied |
| ping RTT | 66–202 ms |

**Alignment.** IQ packets and spectrum frames carry timestamps on the same
server clock. Relative to its timestamp, a spectrum frame arrives **26–31 ms
sooner than an IQ packet** on every receiver: the IQ path has that much more
delay. The spectrum's arrival jitter against its timestamps was about
100 ms, set by the mobile link.

**Not established.** Spectrum dB against IQ dBFS. The IQ channel is scaled
(the carrier reads −27 to −30 dBFS in IQ while the zoomed spectrum peak was
about −67 dB), so the two scales differ by a gain that still needs
measuring with the IQ channel's gain settings in view. `iq96` and above
were not offered to the public on these three.

### 4.1 Second batch: VFOs, back-pressure, retunes, levels, edges

**Method.** Run on 2026-11-24 over the same mobile link. Receivers were
drawn at random (seeds `1795504713`, `1795505097`), excluding those already
used: `stuey3d`, `pd2rps`, `rocksdr` and `hb3xdc`, all `*.tunnel.ubersdr.org`,
20/20 or 19/20 free. `oe6add` was reused for the level measurement because
it has a strong TDF carrier. One session at a time, 19–50 s each, 20 s
apart. `max_session_time` was 0 (unlimited), 3600 or 7200 s depending on
the receiver.

**VFOs** (`server.audio_vfos`; `vfos: 2` in the admission reply):

* **One IQ stream per id.** A second `/ws` with the same `user_session_id`
  and no `vfo=` **replaces** the first: the server closed the earlier
  socket within about 10 ms, twice.
* **Secondary VFOs** are `/ws?…&vfo=N` (N = 1 … `vfos`−1), per
  `session_secondary_audio.go`:
  * They are audio sessions that are **not counted as users**.
  * They live as long as the primary, plus 10 s of grace.
  * They take any demodulated mode, or **plain `iq`: 12 kHz, never
    `iq48`+.**
  * Measured: `vfo=1&mode=iq` streamed 12 000.00 S/s IQ in 240-sample
    packets (DCF77 at DC, 40 dB SNR) alongside an `iq48` primary.
* **Refusals arrive after the upgrade**, as a JSON `error` followed by a
  close:
  * `vfo=2` on a 2-VFO receiver → `"This receiver allows 2 VFOs"`;
  * `iq48` on a secondary (at connect, or in a `tune` message) →
    `"Mode 'iq48' is not available on an additional VFO"`.

**Back-pressure** (client stopped reading):

| socket | pause | result |
|--------|-------|--------|
| IQ primary (`hb3xdc`) | 2 s, 5 s | **no timestamp gap (max 0.01 ms): everything was buffered** and arrived as a burst (215 and 388 packets within 0.3 s of resuming). No lasting latency once drained. The secondary carried on. |
| spectrum (`stuey3d`) | 5 s | no `seq` gap; 53 frames arrived at once on resume. The server's drop-on-slow-client path was not reached at this rate. |

**Retunes** (`pd2rps`, iq48, TDF):

* **Single retunes:** all **10 of 10** resolved. Each switched **two
  packets after the `status` echo, at sample 233–234 of a 240-sample
  packet** (224 on `oe6add`, `on6ure-tel` and `on6ure-btl`), with a
  transition of 13–15 samples. The two-packet rule holds on four
  receivers. The in-packet sample is per-server, within a band near the
  end of the packet, and every switch so far landed in a 240-sample
  packet. Echo latency was 62–687 ms.
* **Drag:** 15 `tune` commands at 15/s produced **15 echoes, all
  applied**, no rate limiting, and the final frequency was correct.
* **Far jumps:** to 7 MHz, to 14 MHz and back were all echoed (85–595 ms).
* **Reconnect:** a new `/ws` with the same `user_session_id` and **no new
  `/connection`** was accepted and streamed.

**Spectrum bins are in FFT order.** The server says, in `user_spectrum.go`:
"Data is in FFT order: [positive freqs (DC to +Nyquist), negative freqs
(-Nyquist to DC)]". The project's Python client unwraps with
`raw[N/2:] + raw[:N/2]`. Measured: at a 162 kHz centre, TDF sits in bins 0
and 1023; LW stations at 153 kHz and 135.4 kHz sit at bins 574 (20 Hz
bins) and 758 (100 Hz bins), i.e. `(k − N) × binBandwidth` below the
centre. **Clients must rotate every frame by N/2.**

**Levels** (`oe6add`, TDF):

* **Carrier:** **−80 dB in the spectrum at 5, 20 and 100 Hz bins alike**,
  so the scale is per-line amplitude. The noise floor per bin rises with
  bin width (−137, −131, −126 dB), as power per bin should.
* **The spectrum is on the header's scale.** Spectrum power summed over the
  48 kHz IQ window: −63.1 dB. The IQ packet header's `baseband_power`:
  −64.8 dBFS. **They agree within 1.7 dB**, so the spectrum is in
  front-end dBFS, the same scale as `baseband_power` and `noise`.
* **The IQ samples carry extra gain:** mean IQ power was −15.2 dBFS,
  about 50 dB above `baseband_power`, with the carrier at −25 dBFS. That
  gain can be computed from every packet (sample power minus
  `baseband_power`), so IQ and waterfall levels can be reconciled with no
  calibration step.

**Spectrum edges and rate** (`rocksdr`):

* **Fine zoom:** 1 024 bins were kept down to **0.5 Hz bins** (512 Hz
  span); no bin-count reduction seen yet.
* **Band edges are clamped** so the whole span stays inside 0–30 MHz. A
  1 kHz-bin view asked at 10 kHz came back centred at 512 kHz (`config`
  and frames agree); one asked at 29.995 MHz came back at 29.488 MHz.
* **One request was ignored:** 100 Hz bins centred at 1 kHz got no
  `config` reply and the view stayed unchanged. A client must not wait for
  a reply that may never come.
* **`set_rate`:** divisor 4 → 2.5 frames/s; divisor 1 → 9.75 frames/s.

### 4.2 Final session: retune ordering, secondary VFO, wide IQ

**Method.** `dig647.tunnel.ubersdr.org`, drawn at random (seed `1795505684`)
from the three European instances that offer `iq96`+ publicly with
`load_status=ok`. Admission: `allowed_iq_modes` `iq48` … `iq384`,
`max_session_time` 0. One 63 s session over the mobile link.

**Primary retunes** (`iq48`, 20 retunes 1.2 s apart, all resolved):

| echo → switch packet | count | position of the switch |
|----------------------|-------|------------------------|
| 2 packets | 17 | sample 232–235 of a 240-sample packet |
| 5 packets | 2 | sample 233–234 of a 240-sample packet |
| 3 packets | 1 | **sample 1 of a 360-sample packet**: the first switch seen in a 360 packet; transition 56 samples, less clean |

The **position in the server's block timing is stable**: near the end of a
240-sample packet, or right at the start of a 360-sample packet. The
**`status` echo's place in the stream varies by a few packets**: the text
message and the binary packets do not share one strict order.

**Secondary retunes** (`vfo=1`, plain `iq`, 12 000.00 S/s, 240-sample
packets, TDF at +1.5 kHz): **6 of 6 switched at sample 233 of a
240-sample packet**, and the echo arrived together with that packet or one
packet later.

**Wide IQ** (8 s each; the new `/ws` replaced the previous primary):

| mode | rate from timestamps | packet sizes | traffic | arrival |
|------|----------------------|--------------|---------|---------|
| `iq96` | 95 999.95 S/s | 120, 360 | 285 kB/s | real time |
| `iq192` | 191 999.99 S/s | 240, 360 | **517 kB/s (≈ 4.1 Mbit/s)** | **7 % behind real time on the mobile link** |

Both are lossless: profile 0, no reduced-depth shift. TDF was at 57–58 dB
SNR in both.

### What this means for an `ubersdr_source`

* **Retune commit.** The `status` echo with the new frequency marks the
  switch to within a few packets. On the primary it fell 2 packets later
  in 17 of 20 retunes and 3–5 in the rest; on a secondary, in the same
  packet or one before. The switch itself sits at a stable point in the
  server's block timing: near the end of a 240-sample packet (sample 224
  or 233–235, fixed per server), or at the start of a 360-sample packet.
  * Treat the 2–5 packets (about 10–25 ms at 48 kS/s) after the echo as
    the switch zone.
  * Either mute or crossfade the affected VFO across it, or detect the
    exact boundary as the first 240-sample packet end (or 360-sample
    packet start) in the zone.
  * The marker is still in-band and tighter than KiwiSDR's `NEW_FREQ`.
* **IQ width against the link.** `iq96` (285 kB/s) kept up over a mobile
  link; `iq192` (517 kB/s) fell 7 % behind. Default to `iq48`, and offer
  wider modes only when the measured arrival rate keeps pace.
* **Multiple VFOs.** One wide `iq48` primary, plus up to `vfos`−1
  secondaries in 12 kHz plain `iq` (`/ws?vfo=N`), which are not counted as
  users. This maps onto our IQ-window design: the main VFO in the wide
  window, other VFOs in their own 12 kHz streams when they fall outside it.
* **Spectrum order.** Rotate each frame by N/2 (FFT order on the wire).
* **Levels.** The spectrum is in front-end dBFS. The IQ gain over that is
  computable from each packet (`baseband_power` against the sample power).
* **Stalls.** The server buffers both IQ and spectrum rather than
  dropping, so after a stall the client gets a backlog burst. The
  prebuffer must trim it, as for KiwiSDR.
* **Presentation.** Hold each spectrum frame until the IQ sample with the
  same timestamp is played. No sequence mapping or prebuffer estimate is
  needed; the measured constant offset is 26–31 ms.
* **Geometry.** `config` gives span and bin width, and each frame gives
  its centre. Expect up to about 0.9 s on a poor link between a command
  and the first frame at the new geometry.
* **Frame handling.** Binary frames must be gunzipped when they start
  `1f 8b`. The `SPEC` frames are not compressed.
* **Bandwidth.** About 110–145 kB/s for iq48 IQ plus about 5.5 kB/s of
  waterfall.

## 5. Assessment

* **For the map:** the directory is easier to consume than the KiwiSDR one
  (plain JSON, explicit TLS and capacity). Merging it is cheap, but only
  worthwhile if a marker can lead to a working connection. Kiwi emulation
  covers a few percent of instances, so it cannot be that connection.
* **For a source:** a native `ubersdr_source`, connecting remotely to
  `/connection`, `/ws` (IQ) and `/ws/user-spectrum`, would be the first
  network source of ours to deliver 48–384 kHz of IQ **plus a
  self-describing server waterfall with continuous zoom** from a shared
  public receiver. That is between KiwiSDR's 12 kHz and a full SpyServer, without
  the slot scarcity. It needs TLS in our websocket layer, which the
  KiwiSDR work needs anyway.
