# SDRconnect: a source on SDRplay's WebSocket API

Date: 2026-11-25. Status: **not to implement.** SDRconnect has no public
server directory, and public SDRconnect servers serve their clients over
SDRconnect's own unpublished protocol, not this API; the WebSocket API
reaches only a user's own SDRconnect, where `sdrplay_source` already
drives the RSP. Kept for reference. Part of
[../README.md](../README.md). The API as documented:
[../../../research/sdrconnect.md](../../../research/sdrconnect.md).

A new `sdrconnect_source` that talks to a running SDRconnect (GUI or
headless) over its WebSocket API on port 5454: device control through
properties, IQ from binary payload 2, optionally the spectrum from
payload 3 through the core spectrum provider.

## Why

* **The API is published and vendor-supported,** unlike SDRconnect's own
  client/server protocol and SDR-Console's.
* **It reaches every RSP** SDRconnect drives, local or (open question 5)
  remote, on every platform SDRconnect runs on, without the SDRplay API
  library in our build.
* **No public directory exists.** Users enter a host and port; the
  module records them in Recent through the remote registry.

## Steps

| step | prerequisites | content |
|------|---------------|---------|
| 0 measurements | an SDRconnect install | answer the open questions with a probe, no product code |
| 1 client | 0, transport/1 | `SDRconnectClient` in core |
| 2 source module | 1 | `sdrconnect_source` with IQ and device control |
| 3 server spectrum | 2, core-spectrum/1–5, and 0 showing a span wider than the IQ | `SDRconnectSpectrumProvider` |

### 0. Measurements

A Python probe in `scripts/probes/sdrconnect/`, modelled on
`scripts/probes/spyserver/`, against SDRconnect headless on this machine:

* **Device:** an RSP if one is at hand; otherwise SDRconnect as a client
  of a public SDRconnect server, which also answers question 5. One
  session at a time, short, as with every public server.
* **Questions:** the five in
  [sdrconnect.md](../../../research/sdrconnect.md) §5, plus:
  * the IQ message size and whether it changes with `device_sample_rate`;
  * how fast `property_changed` follows `set_property` for
    `device_center_frequency`, and where in the IQ stream the retune
    lands (the commit rule for a later IQ window, as spyserver/0 found);
  * the binary rate at 2, 6 and 10 MS/s over loopback and over a LAN;
  * what happens with two WebSocket clients: both streams, one
    `can_control`?
* **Output:** the answers added to `sdrconnect.md`, and a decision on step 3.

### 1. Client

`core/src/utils/proto/sdrconnect.{h,cpp}`, on `WSClient` (libcurl, so
proxy and TLS work unchanged):

* JSON envelope in and out; a property cache filled from
  `property_changed` and `get_property_response`, read with const
  getters, never blocking the caller.
* Binary demultiplexing by payload type; type 2 (or 5 for the secondary
  tuner) to an `onIq(const int16_t*, int frames)` callback, converted to
  `complex_t` in the module; type 3 (or 6) to `onSpectrum`.
* Connect: open the socket, `get_property` for `api_version`,
  `valid_devices`, `active_device`, `can_control`, the rate, LO and gain
  ranges; select the device; then `iq_stream_enable`.
* Reconnect with backoff, as `UberSDRClient`; close cleanly on stop.
* Unknown properties and event types are ignored, so API 1.0.4+ servers
  keep working.
* Tests in `core/tests/`: a fake SDRconnect on `net::` loopback
  (envelope, property cache, binary demux, reconnect).

### 2. Source module

`source_modules/sdrconnect_source`, `include(${SDRPP_MODULE_CMAKE})`, in
the root option list and the Android module list:

* Menu: host and port (default 5454), Recent combo, device combo from
  `valid_devices`, tuner (primary/secondary on an RSPduo), sample rate,
  LNA state, antenna; read-only when `can_control` is false, with the
  reason shown.
* `tune()` sets `device_center_frequency`; the rate list comes from
  step 0's answer (writable since 1.0.7).
* The IQ path goes through the module's prebuffer into its stream, as
  `ubersdr_source` does; no network thread touches the GUI.
* Remote registry: a handler with a probe (connect, `api_version`,
  `valid_devices`, close) and `recordRecent` on connect; no directory
  provider.

### 3. Server spectrum

Only if step 0 shows the spectrum spans more than the IQ the link can
carry (for example the full device rate while IQ is decimated, or the
Compact and IQ Lite network modes). Then a provider on the core-spectrum
hooks: frame geometry from `device_center_frequency` and the measured
span, levels from `spectrum_base` and `spectrum_ref_level`, the local FFT
as fallback.

## Open decisions

* **Headless install for step 0.** SDRconnect is closed-source; the
  probe needs it installed here (Windows) or on a Linux host. The user
  decides where.
* **The examples' licence** is not stated. Read them for facts only.
