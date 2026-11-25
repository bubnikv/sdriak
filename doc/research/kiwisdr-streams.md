# KiwiSDR audio and waterfall streams

Date: 2026-11-24.

This note covers how a KiwiSDR delivers IQ audio and its waterfall: how the
connections pair, how frames are laid out, how the server-side zoom works,
and what the server gives a client to keep the two streams in step. It also
covers what that means for the server-side spectrum design in
[dsp-pipeline.md §10](../design/dsp-pipeline.md#10-server-side-spectrum-for-network-sources)
and [todo/network-sources/](../todo/network-sources/README.md).

**Sources.** Everything below is read from code, not from documentation:

* the KiwiSDR server, [`jks-prv/Beagle_SDR_GPS`](https://github.com/jks-prv/Beagle_SDR_GPS)
  `master` at `efb38e2b`: `rx/rx_sound.{h,cpp}`, `rx/rx_sound_cmd.cpp`,
  `rx/rx_waterfall.{h,cpp}`, `rx/rx_server.cpp`, `main.cpp`, `kiwi.h`;
* the reference Python client, [`jks-prv/kiwiclient`](https://github.com/jks-prv/kiwiclient)
  `master`: `kiwi/client.py`, `kiwirecorder.py`;
* our client: `core/src/utils/proto/kiwisdr.{h,cpp}` and
  `source_modules/kiwisdr_source/src/main.cpp`.

---

## 1. Connections

* **One websocket per stream.** A client opens `…/<ts>/SND` for audio and
  IQ, and `…/<ts>/W/F` for the waterfall. The path prefix tells the server
  what kind of client it is (`rx_server.cpp`):

  | path | server's view |
  |------|---------------|
  | `/<ts>/<stream>` | kiwiclient/kiwirecorder ("krec"): the API for non-browser clients |
  | `/kiwi/<ts>/<stream>` | **the Kiwi web UI** (`isKiwi_UI`) |
  | `/no_wf/<ts>/<stream>` | the web UI without a waterfall |
  | `/wb/<ts>/<stream>` | wideband firmware |
  | `ws/` + any of the above | the same, marked as a websocket |

  **We use `/kiwi/<ts>/SND`, so the server treats us as the web UI.** That
  changes channel allocation and refusal behaviour (see below).
* **Pairing by timestamp.** Two connections with the same `<ts>` are linked
  to the **same receiver channel**: `conn->other`, and `csnd->zoom` is set
  from the waterfall. The IP address must match too, unless `ts` has bit 62
  set (`NEW_TSTAMP_SPACE`, added for VPNs that spread one client over
  several addresses). A second connection of the same type with the same
  `ts` is rejected as a duplicate.
* **Each connection is set up on its own.** Both need `SET auth t=kiwi p=…`
  and an ident (`SERVER DE CLIENT <name> <type>`). Both must send
  `SET keepalive` at least every `KEEPALIVE_SEC` = 60 s (20 s before
  authentication). Waterfall commands also reset the paired audio
  connection's inactivity timer.
* **Waterfall-capable channels are scarce.** The firmware build decides
  how many receiver channels have hardware waterfall (`main.cpp`):

  | firmware | rx channels | waterfall channels | audio rate |
  |----------|-------------|--------------------|------------|
  | `RX4_WF4` | 4 | 4 | `SND_RATE_4CH` |
  | `RX8_WF2` | 8 | 2 | `SND_RATE_8CH` |
  | `RX3_WF3` | 3 | 3 | `SND_RATE_3CH` |
  | `RX14_WF0` | 14 | 0 | `SND_RATE_14CH` |
  | `WB` | 1 | 1 | `SND_RATE_WB` |

  Channels `0 … wf_chans−1` carry the waterfall. The server reports
  `rx_chans`, `wf_chans` and `wf_chans_real` in the `wf_setup` message.

* **Channel allocation for a krec client** (`/<ts>/…`):
  * **SND opened alone** is placed by `RX_COUNT_NO_WF_FIRST`: on a channel
    **without** a waterfall, when one is free. That leaves the waterfall
    channels to browser users. v1.902 did not behave this way (§8.1).
  * **W/F that arrives after its SND**, on a channel numbered
    `≥ wf_chans`, is closed without any message ("case 2" in
    `rx_server.cpp`).
  * **W/F opened first** gets any free channel (`RX_COUNT_ALL`), or
    `MSG too_busy=<rx_chans>` when every waterfall channel is taken
    ("case 1"). The SND connection that follows with the same `ts` then
    joins that channel.
* **Channel allocation for the web UI** (`/kiwi/…`, what we send today):
  * SND is placed by `RX_COUNT_ALL`, so it may take a waterfall channel.
  * A W/F on a channel without hardware waterfall is **accepted**, with no
    error. The server then sends `wf_chans=0` in `wf_setup` and computes an
    **audio FFT**: the spectrum of the audio passband only (bins 256–768),
    not a wide view.
  * When every channel is busy, the first web-UI connection can be turned
    into a `STREAM_MONITOR` (camp/queue) connection instead of being
    refused.
* **Recommendation: switch to the krec form** `/<ts>/SND` and `/<ts>/W/F`.
  * It is the API meant for programs like ours.
  * Its refusals are explicit.
  * With the server waterfall off, it leaves the waterfall channels to
    browser users.
  * With the server waterfall on, **open W/F first, then SND**. Fall back
    to SND only plus the local FFT on `too_busy`, on a silent close, or (if
    we keep the web-UI form) when `wf_setup` reports `wf_chans=0`.
  * The monitor/camp behaviour of the web-UI form should be checked before
    deciding.

## 2. The SND stream (IQ mode)

**Setup.**

1. The server sends `MSG audio_init=<isLocal> audio_rate=<snd_rate>`.
2. The client answers `SET AR OK in=<audio_rate> out=<…>`.
3. The server sends `MSG sample_rate=<frate>`, with six decimals. This is
   the **true** rate, derived from the corrected ADC clock, and is not
   exactly 12 000. kiwiclient waits for this message before it starts
   (`_setup_rx_params()`) and tracks drift against it.

**Tuning.**

* `SET mod=iq low_cut=<Hz> high_cut=<Hz> freq=<kHz>` (with an optional
  `param=`).
* The server clamps the passband to ±(`frate`/2 − 1).
* A change of frequency or mode sets `change_freq_mode`.

**Packet layout** (`snd_pkt_iq_t`):

| bytes | field | notes |
|-------|-------|-------|
| 0–2 | `"SND"` | |
| 3 | flags | `0x01` LPF changed, `0x02` ADC overflow, `0x04` **NEW_FREQ**, `0x08` MODE_IQ, `0x10` compressed (never in IQ mode), `0x20` restart, `0x40` squelched, `0x80` little-endian |
| 4–7 | `seq` | little-endian `u32`; +1 per packet; "waterfall syncs to this sequence number" |
| 8–9 | S-meter | big-endian `u16`; RSSI = 0.1·v − 127 dBm |
| 10 | `last_gps_solution` | seconds since the last GPS fix; 0 = no GPS, 255 = no clock |
| 11 | pad | |
| 12–15 | `gpssec` | GPS seconds of week, of the packet's first sample |
| 16–19 | `gpsnsec` | nanoseconds |
| 20… | IQ | `FASTFIR_OUTBUF_SIZE` = 512 complex `s16`, big-endian unless flag `0x80` |

So one packet is 512 samples, about 42.7 ms at 12 kHz.

**In-band markers the server provides.**

* **`NEW_FREQ`** is set on the **first packet assembled after** a retune
  command is processed. It is the retune marker the SpyServer plan's
  Phase 0 has to look for. Here it exists in the protocol.
* **`seq`** gives a continuous packet count, so gaps and drops are visible.
* **GPS timestamps** give absolute sample timing on GPS-equipped Kiwis.

**What our client does with this today** (`kiwisdr.cpp`):

* **It drops flagged packets.** It accepts an IQ packet only if
  `msg.size() == 2048 + 20 && msg[3] == 0x08`. Any other flag bit drops
  the whole packet: NEW_FREQ (so the first packet after every retune is
  lost), ADC overflow (so every packet during overload is lost), LPF
  change, or little-endian. The test should be `flags & 0x08`, and the
  size should come from the body length.
* **It hard-codes the rate.** It assumes 12 000 (`IQDATA_FREQUENCY`,
  `SET AR OK in=12000`) and ignores `audio_rate` and `sample_rate`. The true
  rate differs by tens of ppm, and the `Prebuffer` absorbs the difference by
  dropping or priming. `RX3_WF3` Kiwis run a different audio rate entirely.
* **It ignores most of the header.** `seq`, the S-meter and the GPS fields
  are never parsed.

These are independent of the waterfall work and worth fixing first
(§6, step 0).

## 3. The W/F stream

### 3.1 Setup and commands

**Setup.** On connect, the server sends:

* `MSG center_freq=<ui_srate/2> bandwidth=<ui_srate> adc_clk_nom=…`
* `MSG wf_fft_size=1024 wf_fps=23 wf_fps_max=23 zoom_max=<MAX_ZOOM>
  rx_chans=<n> wf_chans=<n> wf_chans_real=<n> wf_cal=<dB> wf_setup`

`ui_srate` is the receiver span, 30 MHz on most Kiwis; kiwiclient takes
`bandwidth` for its `MAX_FREQ` (32 MHz units exist). Read `zoom_max` from
this message: kiwiclient assumes 14. Newer servers also send `zoom_cap`,
the deepest zoom actually served (§8.3).

**Commands** (`rx_waterfall.cpp`):

| command | effect |
|---------|--------|
| `SET zoom=<z> cf=<kHz>` | zoom level 0…`zoom_max`, centre frequency; server v1.329 and later |
| `SET zoom=<z> start=<counter>` | the older form; `start` in units of `HZperStart` = `ui_srate / (1024 << MAX_ZOOM)`, about 1.79 Hz on a 30 MHz Kiwi |
| `SET wf_speed=<0…4>` | 0 off, then 1, 5, 13, 23 frames/s (odd rates, chosen so periodic signals do not beat with the frame rate) |
| `SET wf_comp=<0/1>` | IMA ADPCM, about 2:1 |
| `SET interp=<n>` | how FFT bins are reduced to 1 024 pixels: max, min, last, drop, CMA, +10 for CIC compensation; the Kiwi UI default is 13 (drop + CIC) |
| `SET maxdb=<n> mindb=<n>` | the client's colour range; used for auto-aperture reports, does **not** change the encoding (§3.2) |
| `SET aper=… algo=… param=…` | server-side auto-scaling; the server answers `MSG maxdb=…`, `MSG mindb=…` |
| `SET wf_window_func=<n>` | Hanning, Hamming, Blackman-Harris, none |
| `SET send_dB=<0/1>` | |
| `SET keepalive` | as on SND |

Every accepted zoom or start change is echoed back as
`MSG zoom=<z> start=<counter>`.

### 3.2 Geometry and values

* **Span.** `ui_srate / 2^zoom`, always **1 024 bins**. On a 30 MHz Kiwi
  that runs from 30 MHz at zoom 0 (29.3 kHz per bin) to 1.83 kHz at zoom 14
  (1.79 Hz per bin).
* **Position.** `start` is clamped to `[0, MAX_START(zoom)]`. The window
  therefore never leaves 0 … `ui_srate`. Near a band edge the frame is
  **not centred** on the requested `cf`.
* **Bin frequency.** Bin `i` starts at
  `(start + i · 2^(MAX_ZOOM − zoom)) · HZperStart` (line 930). Spectral
  inversion is undone server-side.
* **Values.** Each bin is `(u8)(clamp(dB, −200, 0) − 1)`. kiwiclient
  decodes it as `dBm = byte − 255 + wf_cal`, where `wf_cal` comes from
  `MSG wf_cal` and defaults to −13 on servers before v1.550. The scale is
  **absolute dBm in 1 dB steps**. It is not a client-chosen range, unlike
  SpyServer's `FFT_DB_OFFSET`/`FFT_DB_RANGE`.

### 3.3 Frame format (`wf_pkt_t`)

| bytes | field | notes |
|-------|-------|-------|
| 0–2 | `"W/F"` | then one pad byte (kiwiclient skips `body[0]`) |
| 4–7 | `x_bin_server` | the `start` counter this frame was computed at |
| 8–11 | `flags_x_zoom_server` | low 16 bits: zoom; `0x10000` compressed; `0x20000` no-sync (not yet sent by the server) |
| 12–15 | `seq` | **the SND `seq` of the audio packet going out when this line was computed** |
| 16… | bins | 1 024 bytes; or ADPCM of a 10-byte pad plus 1 024 bytes, which kiwiclient decodes and trims |

**After a zoom or pan**, the server can keep echoing the previous
`start`/zoom for a few frames (`flush_wf_pipe`) while the FFT pipeline
drains. So every frame states the geometry it was really computed at.

### 3.4 Timing per line

* **Integration time grows with zoom.** One line integrates
  `WF_C_NSAMPS · 2^(zoom−1) / f_ADC` of signal, with `WF_C_NSAMPS` = 8 192
  and `f_ADC` ≈ 66.7 MHz: about 0.12 ms at zoom 0–1, 63 ms at zoom 10, and
  **about 1 s at zoom 14**.
* **Overlapped sampling.** When that time exceeds two frame periods, the
  server switches to overlapped sampling to hold the frame rate.
  Consecutive deep-zoom lines then share most of their samples. They look
  smooth, but they lag about 1 s behind the audio.
* **Frame rate.** At most 23 frames/s, which is about 23 kB/s uncompressed.

## 4. Synchronisation

The Kiwi defines a single sync point: the SND `seq`.

* **Audio.** Each SND packet carries `seq`, and each W/F line carries the
  `seq` of the SND packet leaving at the moment the line was computed. A
  client lines a waterfall row up with the audio it heard by matching the
  two.
* **Retunes.** `NEW_FREQ` marks the first SND packet after a retune. The
  FIR state in that packet still holds about one filter length of the old
  frequency.
* **Waterfall geometry.** It is in every frame (`x_bin_server`, zoom) and
  is echoed by `MSG zoom= start=`.
* **Absolute time** (where GPS is present): `gpssec`/`gpsnsec` per SND
  packet.

Nothing marks the moment a W/F zoom takes effect, other than the echoed
geometry. None is needed, because each frame describes itself.

## 5. KiwiSDR compared with SpyServer

| aspect | SpyServer (`todo/network-sources/spyserver/`) | KiwiSDR |
|--------|------------------------------------------|---------|
| connections | one TCP connection, mode `FFT_IQ` | two websockets paired by `ts`; **W/F must open first**; W/F can be refused while SND succeeds |
| spectrum width | `MaximumBandwidth / 2^stage`, centred where the client asks | `ui_srate / 2^zoom`, **clamped inside 0 … `ui_srate`**, so not always centred |
| bins | client-chosen (`FFT_DISPLAY_PIXELS`) | always 1 024 |
| resolution | fixed per stage; zoom means changing the stage | **zoom is the only way to get resolution**: 29 kHz per bin at zoom 0 is useless for tuning |
| levels | client-chosen offset and range, mapping reconstructed | absolute dBm in 1 dB steps, plus `wf_cal` |
| frame geometry | not in the frame | **in every frame** |
| IQ width | server decimation stages, kHz to MHz | 12 kHz (or the firmware's audio rate), one window per channel |
| IQ retune marker | to be found (Phase 0) | `NEW_FREQ` flag in the SND header |
| audio–waterfall sync | none | SND `seq` echoed in each W/F line |
| line latency | short | up to about 1 s at deep zoom |
| rate | server-configured | 1 / 5 / 13 / 23 frames/s, client-chosen |

## 6. What the architecture must provide

These requirements go into the core pieces of
[todo/network-sources/core-spectrum/](../todo/network-sources/core-spectrum/README.md), so that
the KiwiSDR adapter fits them without changing core again.

1. **A frame states its own geometry.** A `SpectrumFrame` carries `startHz`
   and `spanHz` (equivalently a centre), measured, not requested. The pump
   maps the frame onto the display span. Coverage may be **partial**:
   frames clamped at a band edge, or frames in flight during a pan. Fill the
   uncovered bins with the floor. The SpyServer adapter fills in the
   geometry it last committed; the Kiwi adapter takes it from
   `x_bin_server` and the zoom.
2. **Server zoom is a first-class interface, not an extra.**
   * The provider exposes discrete zoom levels: a list of spans.
   * Core routes zoom input there instead of to local view zoom: the zoom
     slider, the wheel over the frequency scale, pinch on Android.
   * A pan (drag of the scale) becomes a centre request.
   * The display span then changes with every zoom step, so the waterfall
     needs **history handling on a span change**. Either clear the history,
     or resample the stored lines into the new geometry, as the Kiwi web UI
     does approximately. Clearing is acceptable for a first version.
   * For SpyServer the same interface maps onto `FFT_DECIMATION` stages,
     which promotes the SpyServer plan's Phase 3 into the shared design.
3. **The display centre is decoupled from the IQ centre.** This is already
   in the plan as the IQ window `W`. For Kiwi, every IQ window move is a
   `SET mod=iq … freq=`, the window is 12 kHz wide, and the hysteresis rule
   is essential: SSB fits, and so does a short drag.
4. **Commit on `NEW_FREQ`.** The Kiwi adapter commits a pending `W` when an
   SND packet with `NEW_FREQ` arrives, on the network thread and in stream
   order, then hands it to the UI tick. The plan's "Phase 0 rule A/B/C"
   choice becomes per adapter.
5. **Presentation delay.**
   * Audio reaches the speaker after the `Prebuffer` (250 ms by default
     for Kiwi; dsp-pipeline.md §2.1) plus the rest of the pipeline.
     Waterfall lines arrive without that delay, so the waterfall runs ahead
     of the audio.
   * The frame carries the source's stream position: the SND `seq`, mapped
     by the adapter to a sample index. The pump holds each frame until the
     front end has consumed that position.
   * Until the sample counters of dsp-pipeline.md §9 item 1 exist, the
     adapter estimates the position: present the frame at its arrival time
     plus the prebuffer's current fill. `PrebufferedLink` would expose that
     fill.
   * At deep zoom the line itself lags by its integration time (§3.4). Do
     not try to hide that.
6. **Levels per provider.** Kiwi bins are absolute dBm, SpyServer bins use
   a server range, and the local FFT is dBFS. The waterfall's min/max (and
   the sticky auto-range) are stored **per spectrum provider**. A provider
   may also supply server auto-scale hints (Kiwi `MSG maxdb`/`mindb` with
   `aper`).
7. **The provider can be unavailable.** It must survive refusal at
   connect time and loss mid-session (a W/F close while SND stays up) by
   switching back to the local FFT, all on the UI thread.
8. **One IQ window per Kiwi channel.** Extra VFOs outside it are marked out
   of window and muted. A second Kiwi channel per VFO costs a user slot on
   a shared public receiver and is not worth doing.

## 7. A KiwiSDR adapter, in order

0. **Fix the client's SND handling, independently of the waterfall** (§1,
   §2, §8):
   * add TLS (`wss://`) to `core/src/utils/proto/websock`. Kiwis behind
     HTTPS redirect plain websockets, and our client cannot connect to
     them at all (§8.1);
   * decide the URL form (krec `/<ts>/SND` recommended, §1);
   * test `flags & 0x08`, and take the sample count from the body length;
   * honour `audio_rate` in `SET AR OK`;
   * use `MSG sample_rate` for `core::setInputSampleRate()` and for the
     `Prebuffer` clock;
   * parse `seq`, and log gaps;
   * keep the S-meter value for a future signal readout.
1. **A W/F client.** It is a second `WSClient` with its own receive thread.
   * **Connect:** the same `ts` as SND, opened **before** SND; auth, ident,
     keepalive.
   * **Setup:** `SET wf_speed=4`, `SET interp=13`, `SET wf_comp=0`
     (compression can follow later). Then `SET zoom=<z> cf=<kHz>`, minus
     `freq_offset`, as SND does.
   * **Each frame:** decode `byte − 255 + wf_cal` and call
     `pushSpectrum()` with startHz/spanHz from `x_bin_server` and the zoom,
     plus `seq`.
   * **Refusal:** handle `too_busy` and silent closes (§1).
2. **Source integration.**
   * A **"Server waterfall"** checkbox: on by default, hidden in
     `--server` mode.
   * Zoom levels for the provider interface.
   * The IQ window policy, with a 12 kHz usable width set by the passband.
   * Commit on `NEW_FREQ`.
   * Presentation delay from the prebuffer fill.
3. **Later:** `wf_comp` ADPCM decoding; server auto-aperture as an
   auto-range hint; history resampling on zoom; sample-exact window
   switching (the plan's Phase 4).

---

## 8. Measurements

Run on 2026-11-24 against `hra.remoteqth.com:8073` (OK1HRA, KiwiSDR 1
v1.902, firmware mode `rx8.wf3`, so 8 channels with 3 waterfalls; no GPS
fix), with a standalone Python probe (stdlib websocket plus numpy) that
identified itself as `sdriak-test`. Reference carrier: TDF Allouis
162 kHz, measured from the IQ at 162 000.36 Hz.

Where a measurement contradicts §1–§4, which were read from `master`
`efb38e2b`, the measurement wins: v1.902 is newer than that source.

### 8.1 Connection

| test | result |
|------|--------|
| plain `ws://` on 8073 | **`302 Found`**, a redirect to `https://`. This Kiwi serves TLS itself (Mongoose 7.14). Our `websock` has no TLS, so **our KiwiSDR source cannot connect to this receiver at all**. |
| `/VER` | `{"maj":1,"min":902,"ts":4611686213435808288}`. The `ts` has bit 62 set, the `NEW_TSTAMP_SPACE` form (pairing without matching IP addresses). |
| channel, krec SND alone | channel **1** while another user held 0; channel 0 when the Kiwi was empty. Both are waterfall channels, so the `RX_COUNT_NO_WF_FIRST` rule read in §1 did not apply here. |
| channel, `/kiwi/` SND alone | channel 0 (empty Kiwi) |
| SND first, then W/F, empty Kiwi | accepted: the W/F paired on channel 0 and frames flowed. Refusal when every waterfall channel is busy was not provoked; that would have needed three slots of a public receiver. |
| W/F first, then SND | paired: W/F reported `rx_chan=1`, and the waterfall `seq` tracks the SND `seq` (§8.3) |
| `wf_setup` | `zoom_max=14`, **`zoom_cap=11`**, `wf_chans=3`, `wf_chans_real=3`, `wf_cal=−13`, `wf_fps=23`, `wf_fft_size=1024`, `bandwidth=30000000`, and **`wf_share=1`** and `firmware_sel=5`, neither of which is in the source read for §1–§3 |
| keepalive | W/F without its own keepalive was closed after **58 s** while SND kept sending keepalives. Each connection needs its own. |
| after close | the channel showed as used in `/users` for a few seconds, then was freed |

### 8.2 SND

| test | result |
|------|--------|
| rates | `audio_rate=12000`, `sample_rate=11998.902088` (−91.5 ppm against our hard-coded 12000). A quiet 150 s stream arrived at 11 998.78 S/s, within 10 ppm of the message, with one network stall of about 0.5 s. |
| packets | always 512 samples, 2 068 bytes, big-endian, no `seq` gaps in 4 318 packets |
| flags | `0x0d` (IQ + NEW_FREQ + LPF) on the first packet, `0x0c` (IQ + NEW_FREQ) on every retune, `0x08` otherwise. **Our `msg[3] == 0x08` test dropped exactly those 10 packets**, the first one and every retune. |
| retune command to `NEW_FREQ` packet arrival | 150–670 ms. The longer times came early in the session, while waterfall commands were also being sent. |
| where the switch happens | abruptly, within 24 samples and with no amplitude dip, **330–360 samples into the `NEW_FREQ` packet** (27–30 ms), across four retunes of ±2 kHz. A coarser first run gave 340–560. The flag marks the packet that contains the switch. The offset inside it is nearly constant here. |
| GPS fields | all zero (this Kiwi has no GPS fix) |
| arrival rate during a busy session | 0.35 % below `sample_rate` over 185 s with eight retunes and many zoom changes, against 10 ppm when quiet. Most likely a backlog draining after the start; it fits the falling command latency. Not explained further. |

### 8.3 W/F

| test | result |
|------|--------|
| geometry echo | `MSG zoom=… start=…` arrives 100–650 ms after the command. The first frame at the new zoom arrives 75–480 ms after it, and at most one frame with the old geometry follows the echo. |
| clamping | zoom 3, `cf` = 100 kHz → `start` = 0, centre 1.875 MHz. Zoom 3, `cf` = 29.95 MHz → `start` 26.25 MHz. Zoom 5, `cf` = 162 kHz → `start` 0. Frames are clamped inside the band, and the requested centre is not honoured there. |
| zoom above the cap | zoom 14 requested → **served at zoom 11** (`zoom_cap`), with no error, only the echo. The zoom levels offered must stop at `zoom_cap`, not `zoom_max`. |
| `x_bin_server` | `start = x_bin · ui_srate / (1024 · 2^zoom_max)`, confirmed: zoom 10, `x_bin` 82 399 → 147.34 kHz |
| bin convention | bin `i` covers `[start + i·w, start + (i+1)·w)`, to about ±0.25 bin: carriers at positions 616.86, 459.6, 651.8 and 721.8 peaked in bins 616, 459, 651 and 721 at zooms 8–11 |
| levels | `byte − 255 + wf_cal` = −58.0 dBm on the carrier, against an S-meter reading of −57.4 dBm: **absolute dBm, within 1 dB**. Noise −116 dBm per 14.3 Hz bin. |
| frame rate, zoom 0 | `wf_speed` 1/2/3/4 → 1.0 / 5.0 / 12.4 / 20.6 frames/s |
| frame rate, zoom 11 | 1.1 / 4.8 / 6.0 / 5.8 frames/s: the deep zoom caps at about 6, from the ~0.13 s integration per line |
| waterfall `seq` against SND `seq` at arrival | median 0 packets at every zoom, range −13…+4. A waterfall line and the SND packet with the same `seq` arrive together, give or take a few packets. |

### 8.4 Survey of the public list

**Method.** The list was taken from `rx.linkfanel.net/kiwisdr_com.js` (a
mirror of kiwisdr.com/public) on 2026-11-24: 842 receivers, all listed as
active.

* **`/VER` probe.** One plain-HTTP `GET /VER` per receiver, following
  redirects, plus HTTPS when plain failed. This uses no user slot. One
  sweep ran 48 probes in parallel. That was too aggressive: afterwards
  `proxy.kiwisdr.com` timed out on its port-80 front for every proxied
  receiver. All later probes were sequential.
* **SND sessions.** Nine short (about 6 s) SND-only sessions, one at a
  time, connecting exactly as our client does: `/kiwi/<ts>/SND`, plain
  `ws://` where possible, the same commands.

**Reachability:**

| group | count | result |
|-------|-------|--------|
| direct hosts, plain HTTP | 416 of 441 | `/VER` answers; 290 on 8073, the rest on 8071–8080, 80 and assorted ports, some after a redirect |
| direct hosts, TLS only | 16 | 15 on 443, 1 on 8073 (hra.remoteqth.com, §8.1, is unlisted and also TLS-only). **Our client cannot reach these.** |
| direct, unreachable | 9 | timeouts, DNS failures, 403/530 |
| `*.proxy.kiwisdr.com` | 401 | the listed URL has no port, and port 80 answers `307` to `:8073`. Port 80 timed out after the parallel sweep; `bern.proxy.kiwisdr.com:8073` answered `/VER` over plain HTTP when tried alone. |

Firmware modes among free direct receivers: `rx8.wf3` 343, `rx4.wf4` 36,
`rx3.wf3` 15, `rx8.wf2` 5. Almost all run v1.902.

**SND sessions, connecting as our client does:**

| receiver | result |
|----------|--------|
| `sdr-amradioantennas.com:8076` (`rx8.wf2`) | normal: 512-sample packets, 11 998.902 S/s, first packet `0x0d` |
| `sdr-amradioantennas.com:8072` (`rx3.wf3`) | **`audio_rate=20250`, `sample_rate=20250.88`**. Our client answers `in=12000` and treats the stream as 12 kS/s, so the IQ plays at the wrong rate. |
| `ea8bfk.ddns.net:8073` (listed as `rx8.wf3`, 40 users) | **980-byte packets (240 samples), `sample_rate=12000.000000` exactly**: a Kiwi-compatible server, not KiwiSDR firmware. Our client's `size == 2048 + 20` check would drop every packet, so it plays silence. |
| `86.254.7.95:8073` (`rx4.wf4`) | `chan_no_pwd=0`, then **`MSG monitor`** and no audio. Every public channel was busy, and the `/kiwi/` (web UI) path got us parked as a monitor. Our client would show "connected" and play nothing. |
| `bern.proxy.kiwisdr.com:8073` | `badp=1` and a close: no password-free channel free (`chan_no_pwd=3`, all busy). Our client should report this. |
| `kiwi.x26.de`, `swl-lippi-2.stbach.de` | plain HTTP: `/VER` returns a redirect page, so our client fails. **`wss://…:443` works normally.** |

### 8.5 Second batch, on a handful of receivers

**Method.** Run on 2026-11-24.

* **Choosing receivers.** Eight were picked from the list by property and
  re-checked with `/status`. Five had free slots.
* **Gentleness.** One session at a time, after a `/status` check that the
  receiver would keep at least two free slots, with 15 s between sessions.
* **Assignment.** The pan test (A5) went to a randomly drawn receiver
  (seed `1795500812`: `82.139.82.247`). Tests that need a particular
  property went to the receiver that has it.

| test | receiver | result |
|------|----------|--------|
| B4 TLS | `kiwi.770net.de:8073`, `kiwi.x26.de:443`, `swl-lippi-2.stbach.de:443` | all TLS 1.3 with **valid Let's Encrypt certificates**, so normal certificate verification can stay on in our TLS code |
| A2 krec path on a full receiver | `86.254.7.95` (4/4) | **`MSG too_busy=4`, no `monitor`.** The krec path gets an explicit refusal where the `/kiwi/` path got parked (§8.4). |
| A1 W/F refusal | `sdr-amradioantennas.com:8078` (`rx8.wf2`, 6/8) | **inconclusive.** The handshakes succeeded, then no message at all in 18 s on either connection, and the web pages timed out: the receiver looked overloaded. Not retried. |
| A3 switch offset at 20 250 S/s | `162.199.177.108` (`rx3.wf3`, WWV 5 MHz as the carrier) | the last old-frequency sample came 400–500 samples into the `NEW_FREQ` packet (20–25 ms; one outlier at 1 421). The "first stable new" detector failed on WWV's AM, so this is less precise than §8.2. In time, it is close to the 28 ms measured at 12 kS/s. |
| A4 fast retunes | same | 20 commands at 15/s → 16 flagged packets. The server applies almost every command, and several can fall inside one 25 ms packet. |
| A6 back-pressure | same | reading paused for 2 s and 5 s: **no `seq` gaps, no loss**. The server buffers and delivers the backlog as a burst (about 98 packets within 0.3 s of resuming). A stalled client therefore gains latency, which only a dropping `Prebuffer` removes (dsp-pipeline.md §2.1). |
| B5 limits from `load_cfg` | same | `inactivity_timeout_mins=120`, `ip_limit_mins=3600`, `n_camp=4`, `chan_no_pwd=0` |
| B2 converter offset | `g8ure.ddns.net:8076` (`freq_offset` = 45 066.565 kHz, 50 MHz) | **W/F `cf` is in the baseband domain.** `cf` = RF centre or RF + 1 MHz clamps to the top of the band (start 29.06 MHz), while `cf` = 15 MHz baseband is centred correctly. The client subtracts `freq_offset` for W/F as it does for SND. |
| B3 32 MHz span | `82.139.82.247` (`bandwidth=32000000`, `zoom_cap=14`) | the DCF77 carrier lands at bin 462.85 using the reported bandwidth, and the peak was in bin 462. A 30 MHz assumption predicts 547.5. **Take `bandwidth` from the message.** This receiver serves zoom 14 (no cap). |
| C1 GPS timing | same, 60 s | `gpssec`/`gpsnsec` give 11 998.934918 S/s against a reported `sample_rate` of 11 998.934928: −0.0008 ppm, residual RMS 0.001 samples, monotonic. **On GPS Kiwis the timestamps are sample-exact**, and the reported rate is the GPS-corrected one. |
| A5 fast panning | `82.139.82.247`, W/F only | 30 `cf` steps at 10/s → 30 echoes (no merging). 116 frames at 23 frames/s with 20 distinct starts. The final geometry arrived 0.49 s after the last command. |
| B1 the 40-channel receiver | `ea8bfk.ddns.net:8073` (EA8BFK, Fuerteventura) | **Not KiwiSDR firmware: [UberSDR](https://github.com/madpsy/ka9q_ubersdr)** (Go, on [ka9q-radio](https://github.com/ka9q/ka9q-radio)). It implements the KiwiSDR protocol (`kiwi_websocket.go`, `kiwi_waterfall.go`, `kiwi_adpcm.go`), serves the Kiwi web UI, registers in the kiwisdr.com list (`kiwisdr_registration.go`), and reports `sdr_hw` as "a verbatim copy of a real KiwiSDR 2" value. Clues on the wire: no `Server` header (Go `net/http`, where Kiwi sends `KiwiSDR_x/Mongoose`), `sample_rate` exactly 12000.000000, 240-sample SND packets, `rx_chans`/`wf_chans`=40, `wf_cal=−3`. Differences that matter to us: **its SND flags are only stereo `0x08`, compressed `0x10` and squelch `0x40`, with no `NEW_FREQ`**, so there is no in-band retune marker. **Its W/F `seq` is its own frame counter** (`wfSequence`), unrelated to the SND `seq`. The W/F header layout (`x_bin`, flags in bits 16+, zoom) matches KiwiSDR. |

### 8.6 Third batch: switch offset, retune latency, waterfall compression

**Method.** Run on 2026-11-24 over a **mobile-phone internet link**.
WebSocket ping round trips were 55–670 ms with spikes, so latency figures
include a jittery access link. Receivers were drawn at random from the
public list by property (seed `1795502286`), with sequential checks and
one session at a time.

| test | receiver | result |
|------|----------|--------|
| W/F refusal (A1) | 10 random `rx8.wf2`/`rx8.wf3` Kiwis | **still untested.** `/users` timed out (12 s) on all ten while `/status` answered, so `/users` cannot serve as a gate on many Kiwis. Detecting the refusal at run time (§1) is the only option left. |
| switch offset, Kiwi 2 | `df6dbf.ddns.net` (12 kS/s, DCF77 77.5 kHz as the carrier) | **inconclusive.** DCF77's amplitude keying (to 15 % every second) confuses the instantaneous-frequency detector. The two clean retunes switched 335 and 348 samples into the `NEW_FREQ` packet, matching Kiwi 1 (§8.2). |
| switch offset, `rx3.wf3` | `ams.dedyn.io` (20 251.07 S/s, TDF 162 000.01 Hz) | six clean retunes, each switching **within 23–24 samples**, at **419, 496, 511, 431, 477 and 540 samples** into the `NEW_FREQ` packet: 20.7–26.7 ms, mean about 23.6 ms. **The last one fell into the following packet.** |
| retune latency on a real Kiwi | same | `NEW_FREQ` packet arrived 102–435 ms after the command (RTT 61–667 ms at the time) |
| retune latency, UberSDR | `websdr.heppen.be` (UberSDR, 50 users, 240-sample packets, TDF) | the switch packet arrived **60–122 ms** after the command in five of six retunes and 384 ms once (RTT 67–422 ms). The switch was abrupt (24 samples). **Flags were `0x08` only: no `NEW_FREQ` at any point.** |
| waterfall ADPCM | `df6dbf.ddns.net`, `ams.dedyn.io` | compressed frames are **517 bytes against 1 024** (−50 %). **Drop the first 10 decoded samples:** the pad precedes the data (`ADPCM_PAD` in `wf_pkt_t`). Decoded values are the byte values: mean absolute difference from plain frames 0.85–1.1 dB, RMS 2.1 dB, against 1.45–1.49 dB between two plain runs. kiwiclient's `samples[:len-10]` drops the **last** 10 instead, which shifts every bin by 10 (RMS 3.9–4.1 dB) and looks like a kiwiclient bug. |

### 8.7 Fourth batch: the server waterfall client

**Method.** Run on 2026-11-24 with `websock_tests --kiwi-waterfall`: W/F
first, then SND on the same `ts`; zoom 0, then zoom 8 around the tuned
frequency. One session at a time after a `/status` check, 15–20 s apart.

| receiver | result |
|----------|--------|
| `82.139.82.247` | `MSG ip_limit=120,127.0.0.1`, then a close of SND right after `sample_rate`: this address had used up its 120 min per day. W/F delivered zoom-0 frames until the session ended. The client now reports `ip_limit` as a refusal. |
| `sdr-amradioantennas.com:8072` (`rx3.wf3`) | the krec W/F path stays silent, as the krec SND path does (§8.4). On `/kiwi/<ts>/W/F` (`p=#`) the setup arrives (`wf_chans=3`, `wf_cal=-10`), and SND on `/kiwi/` pairs with it. Zoom 8 around 77.5 kHz: start 18.906 kHz, span 117.188 kHz, so centred on the request. About 6 frames/s at zoom 8. |
| `ams.dedyn.io` (32 MHz) | krec W/F and SND: `bandwidth=32000000`, `wf_chans=3`, about 20 frames/s at zoom 0, zoom 8 around 162 kHz from 99.499 kHz over 125 kHz. |

### 8.8 Fifth batch: Web-888

**Method.** Run on 2026-11-25 on `ra3vlz-sdr.ru:8073` (Web-888, 0–60 MHz
in the rx-888.com list, 1 of 12 users): `/status`, then one raw SND
session that retunes once and logs the SND flags, then
`websock_tests --kiwi-client` and `--kiwi-waterfall`, one at a time.

| item | result |
|------|--------|
| identity | `/status`: `sdr_hw=KiwiSDR v2026.205 …`, `sw_version=Web888_v2026.205`; `/VER` `{"maj":2026,"min":205}`; SND `MSG version_maj=2026 version_min=205`. The firmware is KiwiSDR-derived with date-based versions. |
| SND | `audio_rate=12000`, `sample_rate=12000.000000` exactly; 512-sample IQ packets; first packet `0x0d`; **`NEW_FREQ` (`0x0c`) about 236 ms after a retune**, as on KiwiSDR. |
| W/F | `wf_chans 13`, `zoom_max 11`, `zoom_cap 11`, `wf_cal −13`, `bandwidth 30720000`. Zoom 0 spans 0–30.72 MHz; zoom 8 around 14 074 kHz starts at 14 014 kHz over 120 kHz, centred on the request. |
| reachability | of three receivers the rx-888.com list showed as `active`, one timed out and one had no backend behind `proxy.rx-888.com` (HTTP 404 on every path). |

An exact `sample_rate` and more than 8 waterfall channels therefore do not
mark a Kiwi-compatible server: Web-888 shows both and is KiwiSDR firmware.

### 8.9 What this changes

* **TLS is a prerequisite.** Without `wss://`, this Kiwi, and any Kiwi
  behind HTTPS, is out of reach: 17 of the 432 reachable direct receivers
  in §8.4. It is step 0 of §7 and independent of the waterfall.
* **Rate and packet size must come from the server.** Use `audio_rate`
  (20 250 on `rx3.wf3`) and `sample_rate`, and take the sample count from
  the body length, since Kiwi-compatible servers send 240-sample packets.
* **Surface refusals.** `badp`, `too_busy`, `down`, `redirect` and
  `monitor` must reach the UI. The krec path avoids being parked as a
  monitor, and gets `too_busy` instead (§8.5).
* **Take geometry and offsets from the server.** Use `bandwidth` (32 MHz
  units exist) and `zoom_cap`. W/F `cf` and SND `freq` are both in the
  baseband domain, so subtract `freq_offset` from both.
* **Detect Kiwi-protocol servers that are not Kiwis.** UberSDR (§8.5)
  lists itself as a KiwiSDR 2 but has no `NEW_FREQ` flag and no SND-linked
  waterfall `seq`. Detect it by its packets (non-512 sample counts, no
  `NEW_FREQ` after a retune), not by `sdr_hw`, the rate or the channel
  count (§8.8). For those servers, commit IQ-window
  moves on a timeout after the command, and present frames by arrival time.
* **Don't trust `seq` blindly.** A Kiwi-compatible server numbers its
  waterfall frames independently. Use `seq` for presentation only while
  W/F and SND `seq` advance together; otherwise fall back to arrival time
  plus the prebuffer fill.
* **After a stall the server delivers a backlog burst, not a gap.** The
  `Prebuffer` must trim it, or latency stays high.
* **Commit rule, refined.** On real Kiwis the switch lies 20–30 ms after
  the start of the `NEW_FREQ` packet. At 20 250 S/s it can spill into the
  next packet. Commit at the flagged packet's start plus 24 ms, as a time
  rather than a sample count. Accept about ±5 ms of error, or mute the
  affected VFO for that one packet.
* **Without `NEW_FREQ`** (UberSDR), commit when no marker has come within
  `max(150 ms, 1.5 × measured RTT)` of the command. In practice the switch
  arrived within one RTT.
* **Waterfall compression is worth enabling on slow links.** It halves the
  waterfall bandwidth for about 1 dB of mean error. Decode with the pad at
  the front.
* **GPS Kiwis give sample-exact time.** That is a possible basis for exact
  IQ-window switching (plan Phase 4) and for drift correction.
* **Zoom levels: 0 … `zoom_cap`.** The server clamps larger requests to
  the cap silently.
* **Commit rule.** Switch the IQ window at the start of the `NEW_FREQ`
  packet plus a fixed offset. 340 samples measured here; verify on other
  Kiwis before hard-coding it, and make it an adapter constant. Using the
  packet start alone errs by about 28 ms.
* **Presentation delay by `seq` works.** The adapter records the SND sample
  index of each `seq`. A waterfall line is shown when the front end has
  consumed that index, or until the counters exist, at arrival plus the
  prebuffer fill.
* **Geometry.** Map frames with the left-edge rule. Expect one stale frame
  after every zoom change; the echo in the frame header handles it.
* **Channel allocation** did not follow the `master` source. Opening W/F
  first is still correct, because it is what forces a waterfall channel.
  The refusal path stays untested.
