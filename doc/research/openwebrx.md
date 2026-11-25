# OpenWebRX / OpenWebRX+: directory, protocol, and what a source would get

Date: 2026-11-25. Read from the OpenWebRX+ source
([luarvique/openwebrx](https://github.com/luarvique/openwebrx) at
`0b91fb5d`, version 1.2.131), upstream OpenWebRX `develop`
([jketterl/openwebrx](https://github.com/jketterl/openwebrx)) for
comparison, one fetch each of Receiverbook's list and map pages, and three
live sessions (§5).

## 1. Directory: Receiverbook

* **Where.** <https://www.receiverbook.de/> replaced sdr.hu as the
  OpenWebRX directory. It also lists KiwiSDRs and PA3FWM WebSDRs.
* **Machine-readable list.** No documented API, but `/map` embeds the
  whole list as a JSON literal, `var receivers = [...]`:
  * one entry per site, with `label` and `location` (GeoJSON point);
  * each site has `receivers: [{label, url, type, version}]`, with `type`
    one of `OpenWebRX`, `KiwiSDR`, `WebSDR`.
* **Size on 2026-11-25:** 1 226 sites and 1 242 receivers: 724 KiwiSDR,
  420 OpenWebRX and 98 WebSDR. 159 of the OpenWebRX URLs are https.
* **Versions** of the 420 OpenWebRX entries:
  * about 390 run OpenWebRX+ (1.2.x with a patch number of 10 or more);
  * about 30 run upstream (1.2.2, 1.2.1, 1.3.0-dev and older);
  * **195 run 1.2.129 or later**, the versions with the IQ mode (§3).
* The list page itself is paginated HTML (`/?page=N`). The map page is
  one request of about 380 kB.

## 2. Protocol (both flavours)

* **Socket.** A websocket at `/ws/`. The client sends
  `SERVER DE CLIENT client=<name> type=receiver`, and the server answers
  `CLIENT DE SERVER server=openwebrx version=v1.2.131`.
* **Then** the client sends
  `{"type":"connectionproperties","params":{"output_rate":…,"hd_output_rate":…}}`,
  and `{"type":"dspcontrol","action":"start","params":{…}}`.
* **Client-settable DSP parameters** (validated server-side):
  * `mod` and `offset_freq` (Hz from the profile centre);
  * `low_cut` and `high_cut` (passband);
  * `output_rate`, `hd_output_rate`, `squelch_level`, `nr_enabled`,
    `nr_threshold`, plus the secondary demodulator's.
  * `audio_compression` and `fft_compression` are the server's settings
    and cannot be changed by the client.
* **Tuning.** Within a profile the client only moves `offset_freq`; the
  SDR's centre is the profile's.
  * `{"type":"selectprofile","params":{"profile":"<sdr>|<profile>"}}`
    switches band.
  * `setfrequency` moves the hardware only where the owner allows it, and
    then only with the "magic key".
* **Bot ban.** Each profile change within 10 s of the previous one adds
  `10 − Δt` to a score, and a score of 30 bans the client for 12 hours
  (`bot_ban_enabled`). A client must never step through profiles quickly.
* **Text messages** are JSON: `config` (`center_freq`, `samp_rate`,
  `fft_size`, `audio_compression`, `fft_compression`, waterfall levels and
  more), `profiles`, `modes`, `bookmarks`, `smeter`, `clients`, errors and
  chat.
* **Binary messages** carry a one-byte type:
  * 1: FFT;
  * 2: audio, at `output_rate`;
  * 3: secondary FFT;
  * 4: HD audio, at `hd_output_rate`;
  * 6: HD stereo (OpenWebRX+ only).
* **Spectrum (type 1).** The server's shared FFT over the whole profile
  span (`samp_rate` around `center_freq`), `fft_size` bins:
  * already swapped into display order;
  * log power plus a fixed −70 dB, averaged over 10 frames;
  * at the owner's `fft_fps`;
  * float32 per bin, or with `fft_compression` "adpcm", ADPCM with
    `COMPRESS_FFT_PAD_N` padding samples and values in hundredths of a dB.
* **Spectrum limits.** One view for everybody on that SDR: no per-user
  zoom and no frame timestamps. The waterfall can only be aligned with the
  audio by arrival.
* **Audio.** int16, mono or stereo, or ADPCM with sync markers
  (`AdpcmEncoder(sync=True)`) when `audio_compression` is "adpcm".

## 3. IQ: OpenWebRX+ 1.2.129 and later only

* **The mode.** OpenWebRX+ 1.2.129 added `mod: "iq"`, "direct IQ
  streaming mode using stereo path" (`IqStream` in
  `csdr/chain/analog.py`). Upstream OpenWebRX has no such mode, so it is
  audio only.
* **What it sends:**
  * complex float at a fixed 48 kS/s around `offset_freq`;
  * through an AGC, then resampled to the client's `hd_output_rate` when
    that differs;
  * limited to ±1 and converted to complex int16;
  * sent as type 6, ADPCM-coded when the server compresses audio.
* **Consequences for a source:**
  * **Width.** 48 kHz of IQ at most, a window like KiwiSDR's 12 kHz but
    four times wider.
  * **Levels.** The AGC means the IQ amplitude says nothing about signal
    level; the waterfall's dB come from the server FFT.
  * **Quality.** With ADPCM the IQ is 4-bit lossy. 1.2.130 "fixed ADPCM
    decoding second channel" in the browser client, so 1.2.129 servers may
    be fine on the wire.
  * **Passband.** The mode's default is `Bandpass(0, 48000)`, but the IQ
    measured two-sided (§5).

## 4. Fit with our network-sources design

* **It fits** the server-waterfall model built for KiwiSDR, SpyServer and
  UberSDR (`doc/todo/network-sources/`):
  * the profile's FFT is a `SpectrumProvider` with one fixed span (a
    single-step zoom ladder);
  * the 48 kHz IQ is the IQ window at C + `offset_freq`;
  * a window move is a `dspcontrol` `offset_freq` change.
* **Differences** from what is already built:
  * no stream-position marker for a retune: the commit must be timed by
    arrival, as SpyServer's fallback rule does;
  * no frame timestamps, so frames are presented by arrival;
  * band changes are profile switches, which must be rate-limited hard
    because of the bot ban;
  * upstream servers and OpenWebRX+ before 1.2.129 give audio only, which
    our IQ sources cannot use. They would need an audio-input mode, or a
    "needs OpenWebRX+ 1.2.129" refusal.
* **Directory.** The `/map` JSON gives URL, type, version and position,
  enough for the remote-receiver browser
  ([todo/remote-receiver-browser.md](../todo/remote-receiver-browser.md)).
  It also covers KiwiSDRs not in the KiwiSDR list.

## 5. Measurements (2026-11-25)

**Method.**
* Four OpenWebRX+ 1.2.131 https receivers were drawn at random (seed
  `1795600001`) from the Receiverbook map data, and tried in draw order,
  one session at a time with 20 s pauses.
* Each session lasted about 50 s on one socket, with no profile change:
  * `mod=iq` with the mode's default passband;
  * then ±24 kHz cuts;
  * then `offset_freq` +10 kHz, 0, −10 kHz and 0, 4 s apart.
* Probes: `scripts/probes/openwebrx/`.

**Receivers.**
* **websdr.frogden.org:** Cloudflare answered the websocket upgrade with
  **403**. OpenWebRX+ has no Origin check of its own, so this is the
  CDN's bot protection; a native client cannot reach such receivers.
* **receiver.i140201mb.net** (Italy): on an ADS-B profile, 1 090 MHz,
  4 MS/s, with another listener present.
* **websdr.do1tob.de** (Germany): on 2 m, 145 MHz, 2 MS/s.
* **noradio86.ddns.net:8073** (Poitiers, France), for the retune timing:
  drawn (seed `1795600002`) from the OpenWebRX+ receivers of 1.2.129 or
  later whose labels say HF. It was on MW (1.5 MHz, 1.024 MS/s,
  RTL-SDR, start mode LSB), with 4 listeners. The probe tuned onto a
  steady carrier at −41.75 kHz, which stood 14 dB over the FFT floor.
  The first try used a 15 dB carrier threshold and closed after 4 s; the
  second used 10 dB.

**Results** of the first two sessions:

| quantity | i140201mb | do1tob |
|----------|-----------|--------|
| `audio_compression` / `fft_compression` | none / adpcm | none / adpcm |
| IQ rate by arrival | 47 947 S/s | 48 080 S/s |
| IQ messages | 3 000 complex int16 samples (12 000 B, I then Q) | same |
| IQ level (AGC) | RMS 0.10, peak 0.80 | RMS 0.32, peak 0.80 |
| FFT | 4 096 bins, 9.0 rows/s | 16 384 bins, about 50 rows/s |
| FFT dB (median) | −100 | −64 |

* **Always send the cuts.** Without `low_cut`/`high_cut`, the IQ is
  filtered by the profile's start mode, not by the `iq` mode's
  `Bandpass(0, 48000)`:
  * on noradio86 (start mode LSB) the positive half sat 26 dB below the
    negative one;
  * on the other two the halves were equal (start modes ADS-B, with no
    filter, and NFM, ±4 kHz).
* **With ±24 kHz cuts** the IQ is two-sided on all three: flat to
  ±16 kHz and 5–7 dB down at ±24 kHz, with a bump of 2–9 dB at DC.
* **The peak of 0.80 on both channels** looks like the `Limit` stage:
  the IQ is clipped there by design.
* **FFT messages.** One row per message. The first message after
  connecting can carry a backlog of several rows (five here), each with
  its own 10-sample ADPCM pad.
* **Back-pressure.** Spectrum frames go through a bounded queue
  (`mp_send`), and a full queue *closes the client*. A slow reader is
  disconnected rather than throttled.
* **ADPCM IQ** (noradio86, `audio_compression` "adpcm"):
  * 6 000 complex samples per message, 47 958 S/s by arrival;
  * decodes cleanly with the client's stereo sync scheme;
  * the carrier stood 25–31 dB over the floor in 4 096-point spectra;
  * peak 0.86.
* **Retune latency** (noradio86, four `offset_freq` steps of ±10 kHz,
  carrier tracked in 1 024-sample blocks): the IQ moved in a message
  arriving **167, 246, 200 and 181 ms after the command**.
  * That is over this machine's link, so it includes the round trip.
  * **The server sends nothing in reply** to a `dspcontrol` change: no
    echo and no config, only the periodic smeter, cpuusage and
    temperature. A client can only commit by arrival, as SpyServer's
    no-PONG rule does: commit at the first block that arrives some 150 ms
    after the command, and mute across the uncertainty (about 100 ms).
* **The carrier's IQ position** was −234 Hz rather than 0. The 250 Hz
  bins of the 4 096-point FFT over 1.024 MHz leave a tune that far off.
  It is not a protocol offset.

**Still open:**
* whether owners can disable the `iq` mode;
* the retune latency on a fast link, without the round trip;
* how the switch looks inside the stream: a hard cut, or a filter
  transient.
