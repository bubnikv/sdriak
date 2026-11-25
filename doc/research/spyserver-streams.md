# SpyServer streams: measurements

Date: 2026-11-24. Measured with `scripts/probes/spyserver/` (`spyprobe.py`,
`spyanalyze.py`) for
[../todo/network-sources/spyserver/0-measurements.md](../todo/network-sources/spyserver/0-measurements.md).
The protocol constants are those of our `spyserver_protocol.h`
(protocol 2.0.1700). The servers ran SpyServer 2.0.1921–2.0.1922. jp's
earlier reconstruction is in [../design/dsp-pipeline.md](../design/dsp-pipeline.md)
§10.2. Compare [kiwisdr-streams.md](kiwisdr-streams.md) §8.

## 1. Method

* **Selection.** From `airspy.com/directory/status.json` (222 servers):
  online, `fullControlAllowed`, `maxClients` ≥ 3 with at least 3 free,
  not RTL-SDR. That left 46. A plain TCP connect, one at a time with 1 s
  pauses, reached **15 of the 46**. In a random draw of 10 online servers
  of any type, 2 accepted a connection. The directory lists many servers
  that are not reachable from outside.
* **Sessions.** One at a time, 15–90 s each, with pauses. The probe sends
  `HELLO` as `sdriak-test` and runs `STREAMING_MODE` = FFT+IQ, INT16 IQ and
  UINT8 FFT.
* **Positions.** Every message is logged with the number of IQ samples
  received before it. TCP keeps the order, so a `CLIENT_SYNC` or `PONG` has
  an exact position in the IQ sample stream.

| server | device | `MaximumSampleRate` / `MaximumBandwidth` | IQ | used for |
|--------|--------|------------------------------------------|----|----------|
| `135.23.92.149:5555` (Ottawa) | Airspy HF+ | 912 000 / 780 000 (0.855) | 114 kS/s (stage 3) | all questions; CFRA 580 kHz as the carrier |
| `176.20.129.192:55557` | Airspy One, capped | 750 000 / 600 000 (0.800) | 11 719 S/s (`MinimumIQDecimation` 6) | capped slide limits, scaling, span |

## 2. Results

### 2.1 `CLIENT_SYNC` (Q1)

* **Not sent after `IQ_FREQUENCY`.** In six IQ retunes there was no
  `CLIENT_SYNC`. It is sent after `FFT_FREQUENCY`, `FFT_DECIMATION` and
  `IQ_DECIMATION`, 0.3–1.4 s after the command.
* **The device follows the FFT.** On the HF+, with no other client,
  `DeviceCenterFrequency` became the requested `FFT_FREQUENCY` each time.
  In FFT+IQ mode the device is tuned to the FFT centre.
* **Stale fields.** `IQCenterFrequency` and `FFTCenterFrequency` kept the
  value of the first IQ retune after later changes. Only
  `DeviceCenterFrequency` tracked the device.
* **Limits.** Alone on the HF+, `Min/MaximumIQCenterFrequency` were the
  device's whole range less half the IQ span (49 250 … 30 951 250 Hz at IQ
  stage 3). On the capped server they were
  **device centre ± (`MaximumBandwidth` − 0.8 · IQ rate)/2**, ±295 313 Hz,
  and the device centre stayed fixed at 432.25 MHz whatever
  `FFT_FREQUENCY` said.

### 2.2 Where an IQ retune lands (Q2, Q3)

A steady carrier (CFRA, 580 001.7 Hz from the IQ) at +8.2 kHz, the IQ
retuned by 2 kHz six times, each command followed at once by a `PING`.

| | samples after the command | samples after the `PONG` |
|---|---|---|
| switch | 30 258 – 63 843 (265–560 ms at 114 kS/s) | **+16 … +18** (five retunes) |

* **`PONG` is an in-band marker.** The switched samples begin 16–18
  samples after the position of the `PONG` that answers the setting's
  `PING`. That is the width of the analysis filter, so the switch is at the
  `PONG`.
* **One `PING` got no `PONG`.** The fifth retune's ping was never answered
  (the next pong answered the next ping). A lost pong needs a fallback.
* `PING` with an empty body works; `PONG` has an empty body.

### 2.3 FFT scaling (Q4)

The FFT stage, centre and carrier were held fixed, `FFT_DB_RANGE` and
`FFT_DB_OFFSET` were varied, and each setting was held 6 s with only the
last 2.5 s measured. **Settings take effect one to three seconds late**,
and frames arrive in bursts. With shorter windows the first runs mixed
settings together.

| change (from offset 0, range 127) | HF+ | Airspy One |
|------------------------------------|-----|------------|
| range 100: byte slope (127/100 = 1.27) | 1.32 | 1.10 |
| range 100: floor byte, predicted / measured | — | 29.7 / 29.4 |
| offset +10 dB at range 100: bytes added (255/100 · 10 = 25.5) | +26, +18, +17 | +26, +25 |

The data fit

```
byte = 255 · (dB + offset + range) / range      i.e.   dB = −offset − range · (1 − byte/255)
```

**A larger `FFT_DB_OFFSET` brightens the bins.** jp's
`dB = offset − range·(1 − b/255)` has the offset's sign reversed. The scale
is relative (the server's own dB): it says nothing about dBm or dBFS. A
negative offset sent as `uint32` is not interpreted as negative.

### 2.4 FFT span and bins (Q5)

Each stage's FFT frame was fitted against the spectrum of the IQ taken at
the same time and centre, over span = k · `MaximumSampleRate`/2^n and a
shift in bins.

| stage | HF+: best k (BW/SR = 0.8553) | shift | r |
|-------|-----------------------------|-------|---|
| 0 | 0.8550 | −0.75 | 0.993 |
| 1 | 0.8525 | −0.25 | 0.982 |
| 2 | 0.8550 | −0.50 | 0.978 |
| 3 | 0.8550 | −0.50 | 0.986 |
| 4 | 0.8550 | −0.75 | 0.926 |
| 5 | 0.8550 | −0.50 | 0.766 |

* **The span is `MaximumBandwidth / 2^n`**, as jp assumed. The Airspy One
  fits were weaker (r 0.55–0.78, k 0.76–0.83 around its 0.800), because
  the 11.7 kHz IQ on UHF noise has few features to fit.
* **Bin convention.** The shift of about −0.5 bin means **bin n/2 is
  centred on the FFT centre frequency**: bin i is centred at
  `centre + (i − n/2) · span/n`.

### 2.5 Pixels and frame rate (Q6)

* `FFT_DISPLAY_PIXELS` of 1 000, 2 048 and 4 096 were each delivered
  exactly.
* Frames/s on the HF+ were 13.8–15.4, against the directory's
  `displayFPS` of 15: the server sends at its configured display rate.

### 2.6 How far the IQ can move (Q7)

On the HF+, with the device at 534.4 kHz (FFT stage 0), the IQ was placed
at the device centre + k · `MaximumSampleRate`. Each placement's IQ
spectrum was compared with the FFT, and its peaks with the 10 kHz
medium-wave channel raster.

| k | inside `MaximumBandwidth`/2? | IQ |
|---|---|---|
| 0, 0.30, 0.40 | yes | matches the FFT (r 0.78–0.97); peaks on 10 kHz channels |
| 0.45 | just past | peaks 4.4 kHz off the channel raster: **aliases** |
| 0.55, 0.75 | no | off-raster junk, or nothing |

The server reports nothing when this happens. **The IQ is usable only
within device centre ± (`MaximumBandwidth` − IQ alias-free width)/2.** On
a capped server `CLIENT_SYNC` gives exactly that range. When alone, it
gives the whole tuning range, because a client in control could move the
device. In FFT+IQ mode, though, the device moves with `FFT_FREQUENCY`, not
with `IQ_FREQUENCY`.

### 2.7 Shared mode (Q8)

A second connection while the first was streaming:

* **Both lose control.** The second client got `CanControl = 0`, and the
  first client's next `CLIENT_SYNC` also said `CanControl = 0`.
* **Guest limits.** The guest's IQ limits were its own FFT window:
  FFT centre ± FFT span/2 (502 500 … 697 500 Hz around 600 000 at FFT
  stage 2). Its FFT centre followed its IQ request.
* **Out-of-range requests are ignored, not clamped.** `IQCenterFrequency`
  kept the last accepted value.
* **Gain.** The guest's `GAIN` had no effect.

### 2.8 Gain order (Q9)

Two sessions on the HF+, digital gain 9 (3 dB per IQ stage), sent before
and after `STREAMING_MODE`:

| order | IQ message gain flags | RMS | clipped |
|-------|----------------------|-----|---------|
| gain before `STREAMING_MODE` | 9 | 21 407 | 0.002 % |
| gain after | 9 | 18 194 | 0 |

The same gain arrived both ways, and the 1.4 dB difference is within
medium-wave fading. **No over-drive from the order was seen** on this
server. jp's ordering costs nothing, so keep it.

### 2.9 Other observations

* `SequenceNumber` is not contiguous per `StreamType`: about 300–500 gaps
  per minute. This was not investigated further. Do not use it for loss
  detection until it is understood.

## 3. What this changes for the plans

* **Commit rule for SpyServer 4: B, with C as fallback.** After each
  `IQ_FREQUENCY`, send `PING`. The switch is at the stream position of the
  matching `PONG`, within the filter width. Commit there with a short mute.
  If no `PONG` arrives within about 1 s, fall back to rule C. Rule A
  (`CLIENT_SYNC`) is out, because no sync follows an IQ change.
* **FFT scaling:** `dB = −offset − range · (1 − b/255)`.
* **Span:** `MaximumBandwidth / 2^n`, with bin n/2 centred on the FFT
  centre.
* **IQ slide limits:** the device's alias-free band, device centre
  ± (`MaximumBandwidth` − 0.8 · IQ rate)/2. In FFT+IQ mode the device
  centre is the FFT centre, so a window move beyond that has to move the
  FFT, which redraws the waterfall. Intersect with `CLIENT_SYNC`'s limits.
* **Settings lag.** FFT settings (`FFT_DB_*`, `FFT_DECIMATION`,
  `FFT_FREQUENCY`) apply one to three seconds late. The waterfall span
  change must follow the frames, the confirming `CLIENT_SYNC`
  (`DeviceCenterFrequency`) or a `PONG`, not the command.
* **Shared mode** removes control from the first client too, and silently
  ignores out-of-range requests. The source must read `CanControl` from
  every `CLIENT_SYNC`, not only the first.
