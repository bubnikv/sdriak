# SDRconnect WebSocket API

Date: 2026-11-25. Sources: `SDRconnect_WebSocket_API.pdf` 1.0.3
(api.sdrplay.com, last modified 2026-06-15; the copy under
sdrplay.com/wp-content/uploads/2026/08/ is the same 1.0.3),
`SDRconnect_Release_Notes.pdf` (1.0.9 build d34bf923c, 15 June 2026), and
sdrplay.com/websocket-api. Nothing here is measured yet.

## 1. What it is

SDRconnect is SDRplay's receiver application for RSP devices. It runs
with its GUI or as a headless binary; both expose the same WebSocket API.
SDRconnect also has its own client/server mode for remote RSPs (network
modes Full IQ, IQ Lite, Compact; login port selectable since 1.0.4).
That protocol is not published. The WebSocket API is the published one.

| version | date | API |
|---------|------|-----|
| 1.0.6 | 2026-01-20 | WebSocket API introduced ("module integration and application automation") |
| 1.0.7 | 2026-02-09 | experimental headless binary; open devices by serial and display name; more properties; `device_sample_rate` writable |
| 1.0.8 | 2026-03-25 | API 1.0.2: RSPduo dual-tuner mode, more properties |
| 1.0.9 | 2026-06-15 | API 1.0.3: read-only `api_version`; a fix for macOS with "WebSocket server enabled" |

SDRplay publishes C# examples (`MinimalExample`, `AudioPlayer`,
`RawIqWriter`, `SDRconnectCtl`, 1.0.3) and a JavaScript example (1.0.2)
as downloads with no stated licence.

## 2. Transport

* WebSocket, port 5454 (SDRplay's page). Whether it binds to every
  interface, whether the port can be changed, and how "WebSocket server
  enabled" is set on the headless binary are not documented.
* No authentication is described.
* Text messages: one JSON object each. Binary messages: a 2-byte
  little-endian payload type, then the payload.

## 3. Text messages

Envelope: `event_type`, `property`, `value` (always a string), optional
`device` (`primary` or `secondary`; primary when absent).

From SDRconnect: `property_changed` (pushed on any change) and
`get_property_response`.

To SDRconnect:

| `event_type` | value |
|--------------|-------|
| `set_property`, `get_property` | the property value, or none |
| `iq_stream_enable`, `audio_stream_enable`, `spectrum_enable` | `"true"` / `"false"` |
| `device_stream_enable` | `"true"` / `"false"`: start or stop the device |
| `selected_device` | device index |
| `selected_device_serial`, `selected_device_name` | serial or display name, optionally `:<network mode>` (Full IQ, IQ Lite, Compact) |
| `start_recording`, `stop_recording` | `iq`, `audio`, `compressed_audio` |
| `apply_device_profile` | profile name |
| `set_primary_device_enable`, `set_secondary_device_enable` | filter events and binary data per tuner |

Properties (read-only marked RO):

* tuning and device: `device_center_frequency` (Hz), `device_sample_rate`
  (Hz, double), `device_vfo_frequency` (Hz), `lna_state` with
  `lna_state_min`/`lna_state_max` (RO), `started` (RO), `overload` (RO),
  `can_control` (RO), `valid_antennas` (RO), `active_antenna`,
  `valid_devices` (RO), `active_device` (RO), `api_version` (RO);
* demodulator: `demodulator` (AM, USB, LSB, CW, SAM, NFM, WFM),
  `filter_bandwidth`, `demod_max_bandwidth` (RO), low-cut per family,
  squelch, AGC, noise reduction, NFM de-emphasis, WFM stereo, RDS fields;
* audio: `audio_volume_percent`, `audio_mute`, `audio_limiters`,
  `audio_filter`;
* spectrum: `spectrum_ref_level`, `spectrum_base` (the display's top and
  bottom, dB);
* measurements: `signal_power`, `signal_snr` (RO).

## 4. Binary payloads

| type | payload |
|------|---------|
| 1 | primary: int16 stereo audio, 48 kHz, LRLR |
| 2 | primary: int16 interleaved IQ |
| 3 | primary: uint8 spectrum bins, scaled to the visible range |
| 4, 5, 6 | the same for the secondary tuner |

## 5. Open questions (to measure)

1. The IQ stream's rate: the device rate (`device_sample_rate`), or a
   decimated VFO band? Is there a block size or timestamp?
2. The spectrum: bin count, line rate, span (the device rate, or a zoomed
   view?), and the dB mapping (`spectrum_base` at 0, `spectrum_ref_level`
   at 255?).
3. Bind address, port option and the enable switch on the headless
   binary.
4. `can_control` when several clients connect: does a second client get
   read-only access?
5. Behaviour when SDRconnect is itself a client of a remote SDRconnect
   server: does the WebSocket API stream that remote device's IQ? If so,
   one local SDRconnect is the bridge to every SDRconnect server,
   whatever its network protocol.
