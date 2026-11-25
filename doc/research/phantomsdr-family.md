# PhantomSDR, NovaSDR, FernSDR and their forks

Date: 2026-11-25. Sources: the repositories below, read at their heads of
that day, the sdr-list.xyz directory, and three short live sessions.

## 1. The family

PhantomSDR (2023) is a wideband WebSDR server: one FFT over the whole
input (up to 30 MHz from an RX-888), a waterfall any client can zoom into,
and audio demodulated on the server from a slice of that FFT. Many
listeners share one receiver. Its descendants:

| line | repository | language, licence | state |
|------|------------|-------------------|-------|
| PhantomSDR | `PhantomSDR/PhantomSDR` | C++, GPL-3.0 | last push 2024-04 |
| PhantomSDR-Plus | `sv1btl/PhantomSDR-Plus` (most active of many forks; also `ny4qphil`, `sv2yj`, `W5TSU`, `f1nsk40260`) | C++ + JS, GPL-3.0 | active, v6.0.0 on the air |
| NovaSDR | `phasor-labs/NovaSDR` | Rust, GPL-3.0 | 0.3.7 on the air; phasor-labs also runs sdr-list.xyz |
| FernSDR | `Steven9101/FernSDR` | C++, AGPL-3.0 | active (2026-11); by the author of PhantomSDR and NovaSDR, which it replaces |
| 22DX-Server, VertexSDR, 9Z SDR+ | not found | | listed on sdr-list.xyz only |

## 2. Directory

`GET https://sdr-list.xyz/api/v1/receivers` returns a JSON array, about
113 entries on 2026-11-25, 109 of them seen that day. Each entry has
`id`, `name`, `hostname`, `port`, `software`, `backend`, `version`,
`users`, `max_users`, `bandwidth`, `center_frequency`,
`range_start_hz`, `range_end_hz`, `grid_locator`, `lat`, `lon`
(`lat_jittered`, `lon_jittered`), `location_city`, `location_country`,
`receiver_count`, `last_seen`. No e-mail.

| `software` | entries | notes |
|------------|---------|-------|
| UberSDR | 77 | the instances.ubersdr.org set |
| PhantomSDR+ | 15 | 10 on v.6.0.0, one each v.3.3.3, 4.2.0, 5.1.0, 5.2.0, one unversioned |
| 22DX-Server | 6 | one host (`sdr.22dx.ru:8901`), six receivers |
| NovaSDR | 4 | 0.3.7 |
| VertexSDR | 3 | one host, three receivers |
| FernSDR | 3 | |
| Web-888, OWRXP (OpenWebRX+), 9Z SDR+ | 1 each | |

A multi-receiver host appears once per receiver with the same
host:port.

## 3. PhantomSDR / NovaSDR protocol

Read from NovaSDR (`crates/novasdr-core/src/protocol.rs`,
`crates/novasdr-server/src/ws/`, `docs/AUDIO.md`), which keeps the
PhantomSDR wire contract.

* Separate websockets for audio, waterfall and events. Each opens with a
  `basic_info` JSON: `sps`, `fft_size`, `fft_result_size`,
  `waterfall_size`, `basefreq`, `total_bandwidth`, `audio_max_sps`,
  `audio_max_fft`, `waterfall_compression`, `audio_compression`,
  `grid_locator`, `defaults {frequency, modulation, l, m, r}`, `markers`.
* Client commands are JSON with a `cmd` tag: `receiver`, `window`
  (`l`, `r`, optional `m`, `level`), `demodulation` (`USB`, `LSB`, `AM`,
  `FM`, `FMC`, `SAM`), `mute`, `squelch`, `agc`, `buffer`, `userid`,
  `chat`. `l`, `m`, `r` are FFT bin indices: the passband edges and the
  carrier.
* Audio: the server cuts the `l..r` slice of the FFT, inverse-transforms
  it, demodulates, applies AGC and codes it. The binary frame is a 40-byte
  header (magic, version, codec 1 = IMA-ADPCM or 2 = Opus, `frame_num`,
  `l`, `m`, `r`, `pwr`) and length-prefixed codec frames. Opus 20 ms
  frames at 8, 12, 16, 24 or 48 kHz. No IQ output.
* Waterfall: CBOR `{frame_num, l, r, data}` per line, inside a zstd
  stream.

## 4. FernSDR protocol

Read from `docs/PROTOCOL.md` and `docs/CODEC.md`.

* One websocket at `/ws`. Text frames are JSON control messages; binary
  frames start with a type byte: `0x01` audio, `0x02` waterfall line,
  `0x03` meter (26 bytes).
* `welcome` lists the bands, each with `center`, `low`, `high`,
  `sample_rate`, `max_bandwidth`, `listeners`, `running`, `on_air`, and
  the server limits (`max_waterfall_width` 4096, `max_waterfall_fps` 30,
  `max_users`).
* `tune {band, freq, mode, low, high}`, modes `usb lsb cw cwl am sam nfm
  dsb` (and `wfm` on wide bands); `viewport {low, high, width, fps,
  codec}` sets the waterfall span in Hz; `audio {rate, bitrate, frames,
  noise_margin}`; `dsp {agc, nr, notches, squelch, ...}`.
* Every waterfall line carries its own low and high edge in Hz (doubles)
  and width in bins, then a coded row (WFC1 to WFC5: zero runs,
  predictors, 1 or 2 dB steps, range coding).
* Audio is demodulated on the server and coded with NAC, FernSDR's own
  MDCT codec (NAC, NAC2, NAC3), bitstream specified in `CODEC.md`. The
  audio rate is the band rate over a power of two and need not be whole
  (7812.5 Hz from 64 MS/s). No IQ output.
* `GET /api/status` gives the site, bands and listener counts as JSON.

## 5. PhantomSDR-Plus: the KiwiSDR bridge

`sv1btl/PhantomSDR-Plus` `src/kiwi_bridge.h` (925 lines, in the
repository since its first commit, 2026-09-20) serves the KiwiSDR
protocol beside the native one:

* SND on `/kiwi/<ts>/SND` and `/<ts>/SND`, W/F on the matching W/F
  paths. `SET mod=<mode> low_cut= high_cut= freq=` retunes; `SET zoom=
  cf=` and `SET zoom= start=` move the waterfall.
* W/F frames carry `x_bin = 0` and `zoom = 0` in every header,
  whatever zoom and centre the bridge serves (`KiwiWfEncoder::send`), so a
  client that places lines by their header draws a zoomed line across the
  whole band.
* SND frames are Kiwi-shaped: 512 samples, flags `0x08` for IQ (with the
  10-byte zero GPS header) or `0x00` for audio, big-endian PCM, the
  S-meter in the header. No `NEW_FREQ` flag.
* `mod=iq` gives 12 kHz IQ only when the operator enables `[server] iq`;
  otherwise it falls back to USB audio.
* Retuning SND works for real (RX-888 direct-sampling) inputs only; an IQ
  input does not retune through the bridge.

**Live, 2026-11-25,** `phantomsdr.no-ip.org:8900` (PhantomSDR+ v.6.0.0,
11/100 users), with `websock_tests`:

| item | result |
|------|--------|
| `/status` | `<html><body>Kiwi bridge OK</body></html>`, not the Kiwi key=value page |
| `/VER` | 404; `KiwiSDRClient` falls back to a local timestamp |
| SND | `audio_rate=12000`, `sample_rate=12000.000000`; IQ arrived after 2.1 s, no seq gaps; flavour Kiwi-compatible after the 2 s no-`NEW_FREQ` timeout |
| W/F | `zoom_max 11`, `zoom_cap 11`, `wf_chans 3`, `wf_cal 0`, `bandwidth 30000000`; 304 frames, every one labelled zoom 0 over 0–30 MHz, including those after `SET zoom=8 cf=14074`: the header bug above |

## 6. What this means for us

* **PhantomSDR+ with the bridge needs no new source.** `kiwisdr_source`
  connects and gets IQ where the operator enabled it. The sdr-list.xyz
  provider lists them as KiwiSDR-kind entries (the probe tells IQ from
  audio-only). Because of the header bug, `kiwisdr_source` keeps a
  session at zoom 0 when no frame header leaves zoom 0, x_bin 0 within
  2 s and 10 frames of a zoom request; the bridge's zoom-0 waterfall
  covers the whole band. Writing the served zoom and `x_bin` into the
  header would fix it in the bridge.
* **NovaSDR, FernSDR and older PhantomSDR+** give demodulated audio
  only, in Opus, IMA-ADPCM or NAC, plus a waterfall. A source for them
  would deliver audio, not IQ, into a pipeline built on IQ. Together they
  are about 10 public receivers. Not worth a source now.
* **The UberSDR entries** duplicate instances.ubersdr.org; an sdr-list
  provider keeps only the kinds no other directory covers.
