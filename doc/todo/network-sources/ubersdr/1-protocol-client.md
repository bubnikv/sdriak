# UberSDR 1: protocol client

Date: 2026-11-24. Status: implemented and built with the module (plan 2),
not committed.

* **Pinned decoder:** `pcm_v4.hpp` is vendored from commit `29093cde`
  (2026-11-23). Upstream has no v0.1.69 tag, so the file records the
  commit and the upstream SHA-256.
* **Conformance check not run:** upstream's `pcmv4_conformance.cpp` and
  its three fixtures, with the expected hashes from `test/run.sh`, are
  downloaded to the scratchpad but not compiled. Building and running that
  third-party program is waiting for the user's permission.
* **API:** `start()`/`stop()` run the socket on the client's own thread,
  in place of the sketched `openIQ()`.

Part of [README.md](README.md).
Prerequisites: transport/1–2 (TLS: 74 of 77 instances use it).

## Files (new, in `source_modules/ubersdr_source/src/`)

* **`pcm_v4.hpp`:** vendored verbatim from
  `madpsy/ka9q_ubersdr/clients/soapy_driver/pcm_v4.hpp`. It is header-only,
  namespace `ubersdr`, with SSE/AVX2/NEON paths and a generic fallback,
  1 504 lines. Add a header comment naming the origin, the GPL-3.0 licence
  and the commit taken; keep the file otherwise unchanged. It is bit-exact
  against the server encoder: **do not "fix" arithmetic in it**.
* **`ubersdr_client.{h,cpp}`:** `UberSDRClient`, described below.

## Protocol (ubersdr.md §4)

1. **Session id.** A random UUID v4 per source instance, kept across
   reconnects: reconnecting with the same id and no new `/connection` was
   accepted.
2. **Admission:** `POST /connection` with body
   `{"user_session_id": …, "password": … (optional)}`, using
   `net::http::curlRequest` (transport/2).
   * **Fields:** `allowed`, `reason`, `allowed_iq_modes`,
     `max_session_time`/`session_timeout` (0 = unlimited),
     `daily_time_remaining_secs` (−1 = unlimited), `vfos`, `bypassed`,
     `client_ip`.
   * **Status codes:** 403 is a wrong password; `allowed:false` carries a
     reason. Surface both.
3. **IQ socket:**
   `wss://host:port/ws?frequency=<Hz>&mode=<iq48|iq96|iq192|iq384>&user_session_id=<id>&format=pcm-zstd&version=4[&password=…]`.
   * **Primary only.** A second primary `/ws` with the same id **replaces**
     the first: the server closes the old socket within about 10 ms.
     Secondaries are `&vfo=N` (UberSDR 4).
   * **Text frames** are JSON:
     * `status`: `frequency`, `mode`, `sampleRate`, `sessionId`, `info`;
     * `error`: then the server closes; for example "Mode 'iq48' is not
       available on an additional VFO", "This receiver allows 2 VFOs";
     * `pong`.
   * **Binary frames** are `pcm_v4` packets. Use one
     `ubersdr::PCMv4Decoder` per connection: every packet must be decoded,
     even if discarded, or the predictor desynchronises.
   * **Packet header:** `timestamp_nanos` (GPS-grade on most instances;
     the first 3–9 packets carry 0), `sample_rate`, `channels` (2),
     `sample_count` (240 or 360 complex at iq48; 120/360 at iq96;
     240/360 at iq192), `baseband_power` and `noise` (front-end dBFS),
     `profile`, `escape`, `silent`, `shift`.
   * **Samples:** interleaved int16 I/Q, converted to complex float by
     dividing by 32768.
4. **Commands:**
   * `{"type":"tune","frequency":f,"mode":m}`; it is answered by a
     `status` echo;
   * `{"type":"get_status"}`;
   * `{"type":"ping"}` every 30 s, as the official client does.
5. **Back-pressure.** The server buffers rather than drops: after a 5 s
   stall the backlog arrives as a burst with no timestamp gap. The source's
   prebuffer trims it.

## `UberSDRClient` API sketch

```cpp
struct Admission { bool allowed; std::string reason; std::vector<std::string> iqModes;
                   int maxSessionS; int vfos; };
std::optional<Admission> admit(const std::string& baseUrl, const std::string& password);
bool openIQ(const std::string& mode, double freqHz, int vfo = 0);   // network thread inside
void tune(double freqHz, const std::string& mode);                  // any thread
void close();
// Callbacks (network thread):
std::function<void(const PacketInfo&, const dsp::complex_t*, int)> onIQ;  // PacketInfo: ts, rate, power, noise, index in stream
std::function<void(const json&)> onStatus;
std::function<void(const std::string&)> onError;
```

## Review gate

* A standalone check in the scratchpad: a console program feeding a
  recorded packet stream (or UberSDR's
  `clients/hpsdr-go/internal/pcmv4/testdata/*.bin`) through the vendored
  decoder matches the reference output.
* In the tree: compiles with the module (plan 2).
