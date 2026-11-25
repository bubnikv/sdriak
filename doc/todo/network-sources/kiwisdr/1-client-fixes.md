# KiwiSDR 1: client fixes

Date: 2026-11-24. Status: implemented, not committed. Part of
[README.md](README.md). Prerequisite: transport/1–2.

Changes from the steps below:

* **Rate given to the DSP.** `kiwisdr_source` runs the DSP at
  `audioRate()` (the integer `audio_rate`), not `sampleRate()`.
  `RationalResampler` rounds its input rate to an integer, so 11 998.902
  would become a meaningless 11 999. `sampleRate()` is exposed for drift
  tracking later. The rate is applied on the GUI thread from the source's
  `uiTickHandler`.
* **Auth line.** It is `p=` on the krec path, as kiwiclient sends it, and
  `p=#` on `/kiwi/`.
* **Fallback to the web-UI path.** sdr-amradioantennas.com (:8076, and
  :8072 with 0 of 3 users) never answers the krec path. With no message
  within 3 s the client reconnects once on `/kiwi/<ts>/SND`, without
  `onDisconnected` in between. A `monitor` there counts as a refusal.

Tests:

* **Offline.** `websock_tests` covers SND parsing, the refusal wording and
  three fake-Kiwi sessions: the 20 250 rate and the setup order,
  `too_busy`, and the silent krec path. All pass, also under ASan.
* **Live** (`websock_tests --kiwi-client`), all streaming:
  * hra.remoteqth.com (TLS, 11 998.902088, KiwiSDR);
  * sdr-amradioantennas.com :8076 and :8072 (20 250.880923), through the
    fallback;
  * ea8bfk.ddns.net (detected Kiwi-compatible).
* **Not tried:** the GUI, a refusal from a real full Kiwi, and
  `websdr_view`.

## Defects measured on live receivers (kiwisdr-streams.md §8)

| # | defect | evidence |
|---|--------|----------|
| D1 | no TLS | 17 reachable Kiwis are TLS-only; hra.remoteqth.com answers plain `ws://` with `302` |
| D2 | `msg[3] == 0x08` and `size == 2048 + 20` drop packets | every retune's first packet (`0x0c`), the first packet (`0x0d`), every ADC-overload packet; a Kiwi-compatible server's 240-sample packets are all dropped |
| D3 | hard-coded 12 000 S/s and `SET AR OK in=12000` | real rates are 11 998.9 (−91.5 ppm), 20 250 (`rx3.wf3`) and 12 000 exactly (UberSDR) |
| D4 | `/kiwi/<ts>/SND` web-UI path | on a full Kiwi we get parked as `monitor` with no audio; the krec path gets `too_busy` |
| D5 | refusals are invisible | `badp`, `too_busy`, `down`, `redirect`, `monitor` are only logged |

## Steps (`core/src/utils/proto/kiwisdr.{h,cpp}`)

1. **URL (D1, D4).**
   * Base URL from `net::http::probeBaseUrl(host, port, "/VER")`
     (transport/2). `ts` comes from the `/VER` JSON (it may have bit 62
     set; keep it as `uint64_t`).
   * Websocket URL: `ws(s)://host:port/<ts>/SND`, the krec form, without
     `/kiwi/`.
   * Connect with `wsClient.connectAndReceiveLoop(url)`.
2. **Setup order** (kiwiclient's):
   1. On connect: `SET auth t=kiwi p=<password or empty>` (today `p=#`;
      keep `#` only if a password-free Kiwi rejects the empty form),
      `SERVER DE CLIENT sdriak SND`, `SET compression=0`.
   2. On `MSG audio_rate=R`: send `SET AR OK in=R out=48000`.
   3. On `MSG sample_rate=F`: store F as the actual rate. Only then send
      AGC, `SET mod=… freq=…` and keepalive. This is the point where the
      client counts as connected (fire `onConnected`).
3. **SND parsing (D2).**
   * Header: `flags = msg[3]`, `seq = LE32(msg[4..7])`,
     `smeter = BE16(msg[8..9])`.
   * IQ when `flags & 0x08`: 10 bytes of GPS header, then int16 pairs,
     big-endian unless `flags & 0x80`. Sample count = (body − 20) / 4.
   * Real mode (`websdr_view`): count = (body − 10) / 2.
   * No fixed sizes anywhere.
   * Expose:
     * `onPacket(flags, seq, samples, count)`, or extend `onSamples` with
       a struct carrying `newFreq = flags & 0x04` and `seq`;
     * an ADC-overload flag (`0x02`) for a UI indicator;
     * the RSSI (0.1·v − 127);
     * a `seq` gap counter;
     * the GPS fields (`last_gps_solution`, `gpssec`, `gpsnsec`).
4. **Rates (D3).**
   * Replace the public `int IQDATA_FREQUENCY` with
     `double sampleRate() const` (the `MSG sample_rate` value) and
     `int audioRate() const`.
   * `kiwisdr_source`: `core::setInputSampleRate(sampleRate())` and
     `chain.setSampleRate(sampleRate())` once the rate is known. Today
     `menuSelected` hard-codes 12 000; keep 12 000 only as the pre-connect
     placeholder.
   * Passband: `low_cut/high_cut = ±(sampleRate/2 − 100)` instead of
     ±7000.
   * `websdr_view`: replace its uses of `IQDATA_FREQUENCY`
     (`SubWaterfall` construction, `addAudioSamples`) with the getter.
5. **Refusals (D5).**
   * `too_busy`, `badp`, `down`, `redirect`, `monitor`, `camp_disconnect`
     → `onError(reason)` with readable text (kiwiclient's wording for
     `badp` codes 1–7), then close.
   * `kiwisdr_source` shows it in its status line.
   * `monitor` counts as busy, which can no longer happen on the krec
     path, but handle it anyway.
6. **Flavour detection.** Set `flavour = KiwiSDR` or `KiwiCompatible` from
   any of these signs:
   * packet sample count ≠ 512;
   * `sample_rate` exactly equal to `audio_rate`;
   * `wf_chans > 8` (kiwi/2);
   * no `NEW_FREQ` seen within 2 s after a retune.

   UberSDR's Kiwi emulation has no `NEW_FREQ` and an unrelated waterfall
   `seq` (ubersdr.md §2). Plans 2 and 3 branch on the flavour.
7. **Keepalive.** Each websocket sends `SET keepalive` every 5 s (today
   4 s, fine). A W/F connection was dropped after 58 s without its own
   keepalive.

## Review gate (user tests)

* **TLS:** hra.remoteqth.com:8073.
* **Plain and the 20 250 rate:** `sdr-amradioantennas.com:8076` (plain)
  and `:8072` (`rx3.wf3`, check pitch and tuning accuracy).
* **Kiwi-compatible:** `ea8bfk.ddns.net:8073` (UberSDR emulation, 240
  samples).
* **Refusal:** a full Kiwi shows `too_busy` instead of silence.
* **Real mode:** `websdr_view` still works.
