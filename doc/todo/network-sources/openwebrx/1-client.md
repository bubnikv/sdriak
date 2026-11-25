# OpenWebRX+ 1: protocol client

Date: 2026-11-25. Status: implemented; unit tests pass. The live smoke run
passed on two receivers:

| | noradio86 | do1tob |
|-|-----------|--------|
| IQ | ADPCM, 47 882 S/s | int16, 47 993 S/s, RMS 0.31, peak 0.80 |
| FFT | 9 rows/s of 4 096 bins | 50 rows/s of 16 384 bins |

On do1tob the `modes` list came 3.8 s after the config. Part of
[README.md](README.md).
Prerequisite: transport/1–2 (`WSClient` on libcurl, ws and wss).

## Files

* `source_modules/openwebrx_source/src/owrx_client.{h,cpp}`:
  `OpenWebRxClient`.
* `source_modules/openwebrx_source/src/owrx_adpcm.h`: the two decoders.
* `source_modules/openwebrx_source/tests/owrx_client_tests.cpp`, a CMake
  test target as in `ubersdr_source`.

## Connection (openwebrx.md §2)

* **URL.** `ws[s]://host[:port]/ws/`, from the receiver's base URL. 159
  of 420 listed receivers are https.
* **Handshake.** Send `SERVER DE CLIENT client=sdriak type=receiver`.
  Expect `CLIENT DE SERVER server=openwebrx version=v<x.y.z>`, and keep
  the version.
* **Then** send
  `{"type":"connectionproperties","params":{"output_rate":12000,"hd_output_rate":48000}}`.
  48 000 is the `iq` mode's own rate, so the server does not resample.
* **Start IQ:**
  * `{"type":"dspcontrol","action":"start"}`;
  * then `{"type":"dspcontrol","params":{"mod":"iq","offset_freq":W,"low_cut":-24000,"high_cut":24000}}`.
  * **Always send the cuts.** Without them the IQ keeps the profile's
    start-mode filter: one-sided on an LSB profile (measured 26 dB
    down on one half).
* **Retune** sends `{"type":"dspcontrol","params":{"offset_freq":W}}`.
  The server sends nothing back.
* **Profile change** sends
  `{"type":"selectprofile","params":{"profile":"<sdr>|<profile>"}}`. The
  client exposes it, but rate-limiting and warning the user are the
  module's job (step 2).
* **No `setfrequency`.** It needs the owner's magic key, and it moves the
  band for every listener.
* **Refusals.** A 403 on the upgrade (Cloudflare bot protection, measured
  on one of four receivers) reports "this receiver blocks non-browser
  clients". A socket closed before the handshake reply reports the HTTP
  status, or "closed".

## Messages

* **Text** messages are JSON with a `type`:
  * `config`: arrives in several parts; merge them. It carries
    `center_freq`, `samp_rate`, `fft_size`, `audio_compression`,
    `fft_compression`, `waterfall_levels`, `profile_id`, `sdr_id`,
    `start_mod` and `start_offset_freq`.
  * `profiles`: a list of `{id, name}`.
  * `modes`: check that `iq` is listed.
  * `receiver_details`: `receiver_name`, `receiver_location`,
    `session_timeout`.
  * Also `clients`, `smeter`, `backoff`, `sdr_error`,
    `demodulator_error` and `log_message`.
* **Binary** messages start with a type byte:
  * 1: FFT row;
  * 6: HD stereo, which is the IQ: I in channel 1, Q in channel 2;
  * ignore 2, 3 and 4.
* **A `config` with a new `center_freq` or `samp_rate`** (another
  listener changed the profile) goes to the module as a profile event.

## Decoders (`owrx_adpcm.h`)

* **IQ, uncompressed:** little-endian int16 pairs, I then Q, scaled by
  1/32768.
* **IQ, `audio_compression` "adpcm":** stereo IMA ADPCM with sync frames.
  * Each frame is `SYNC`, then four int16: step index and predictor for
    channel 1, then channel 2.
  * Then 1 001 byte pairs: the first byte of a pair is channel 1, the
    second channel 2, each byte low nibble first.
  * The decoder state persists across messages.
  * Measured: 6 000 complex samples per message, decoding cleanly
    (openwebrx.md §5).
* **FFT, `fft_compression` "adpcm":** per row, from a reset state.
  * Decode the nibbles, drop the first 10 values (`COMPRESS_FFT_PAD_N`),
    divide by 100 to get dB.
  * Uncompressed rows are float32.
  * The first message after connecting may hold several rows back to back
    (measured: 5). Each row is (`fft_size` + 10) / 2 bytes; split on that.
* **Tests:**
  * round-trip against a small IMA encoder written in the test;
  * the sync framing, including a frame split across two messages;
  * a multi-row FFT message;
  * a stray byte before `SYNC`, which must be resynchronised.

## Reading without stalls

* **The risk.** The server queues spectrum frames per client, and a full
  queue closes the connection (`mp_send`, openwebrx.md §5).
* **So** the network thread only decodes and hands off. FFT rows go into
  a bounded latest-wins slot, IQ into the source's link. It never blocks
  on the UI.
* **A link backlog is trimmed by the prebuffer.** The socket is never
  paused.

## Keep-alive

* None seen in the protocol: the browser client sends nothing periodic.
  Rely on the websocket's own ping handling.
* `receiver_details.session_timeout` (3 600 s on one receiver) is shown in
  the module.

## Review gate

* Unit tests pass.
* A live smoke run (`owrx_client_tests --live <url>`) on one receiver for
  30 s: handshake, config merged, `iq` listed, IQ at about 48 kS/s, FFT
  rows of `fft_size` bins, clean close. One session, as the shared rules
  require.
