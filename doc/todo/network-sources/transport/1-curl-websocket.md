# Transport 1: `WSClient` on libcurl's WebSocket API

Date: 2026-11-24. Status: implemented. `core/tests/websock_tests` covers
the protocol paths on loopback (also under ASan); `websock_tests --live
<url> [--kiwi]` reached hra.remoteqth.com over wss:// and a plain
KiwiSDR on Windows. Android and macOS are not yet tested. Part of
[README.md](README.md).

## Goal

`net::websock::WSClient` keeps its public API and gains `wss://`. TLS,
redirects and (later) proxies come from libcurl. The callers do not change
behaviour: `KiwiSDRClient` (`core/src/utils/proto/kiwisdr.cpp`) and,
through it, `kiwisdr_source` and `websdr_view`.

## Files

* `core/src/utils/proto/websock.{h,cpp}`: rewrite the transport, keep the
  API. Use Edit, and keep the existing line endings: zh's version of this
  change rewrote the licence header as whitespace churn.
* `core/src/utils/curl_init.{h,cpp}`: use `make_easy()` for the handle, so
  the user agent, CA path (Android) and later the proxy are shared.

## API

The existing API stays:

* `connectAndReceiveLoop(host, port, path)`, `sendString`, `sendBinary`,
  `stopSocket`, `reset`, `isStopped`;
* the callbacks `onTextMessage`, `onBinaryMessage`, `onConnected`,
  `onDisconnected`, `onReceiveLoopTick`.

Add:

```cpp
// url is ws://, wss://, http:// (treated as ws) or https:// (treated as wss).
void connectAndReceiveLoop(const std::string& url);
// Final URL after redirects, available from onConnected.
std::string effectiveUrl() const;
```

The `host`/`port`/`path` overload builds `ws://host:port/path` and calls the
URL overload.

## Implementation

1. **Connect.**
   * `curl::make_easy()`, then `CURLOPT_URL`, `CURLOPT_CONNECT_ONLY = 2L`
     (the WebSocket upgrade), `CURLOPT_CONNECTTIMEOUT_MS` = the current
     `HANDSHAKE_TIMEOUT_MS` (10 s), `CURLOPT_NOSIGNAL = 1`.
   * Run `curl_easy_perform()`. Read `CURLINFO_RESPONSE_CODE` and
     `CURLINFO_EFFECTIVE_URL`.
2. **Redirects.**
   * libcurl refuses any non-101 answer to the upgrade before it resolves
     a redirect: it does not follow even with `CURLOPT_FOLLOWLOCATION`, and
     `CURLINFO_REDIRECT_URL` stays empty (`http.c`, "Refused WebSocket
     upgrade"). So `WSClient` sets `FOLLOWLOCATION = 0`, reads `Location`
     with `curl_easy_header()` and resolves it against the request URL.
   * hra.remoteqth.com:8073 answers `ws://…/kiwi/<ts>/SND` with
     `302 → https://hra.remoteqth.com:8073/`, the site root. A redirect to
     the root keeps the endpoint path and query.
   * Map `http→ws` and `https→wss`, keep the path's case (the current code
     preserves it), and retry, at most 5 times, as now.
3. **Receive loop** (the current thread model: the caller's thread runs
   the loop):
   * Get the socket with `CURLINFO_ACTIVESOCKET`, then wait on it with
     `poll`/`select` for up to 100 ms, so `onReceiveLoopTick` keeps its
     cadence.
   * Under `m_curlMtx`, call `curl_ws_recv()` repeatedly until
     `CURLE_AGAIN`.
   * Reassemble fragments with `curl_ws_meta()`: `bytesleft`, and the
     flags `CURLWS_TEXT`, `CURLWS_BINARY`, `CURLWS_CONT`, `CURLWS_CLOSE`.
     Deliver complete messages to `onTextMessage`/`onBinaryMessage` outside
     the mutex.
   * Ping/pong: libcurl answers PINGs itself; do not send our own pong.
   * A CLOSE frame or an error ends the loop: `onDisconnected`.
4. **Send.**
   * `sendString`/`sendBinary` take `m_curlMtx` and call `curl_ws_send()`
     until all bytes are out, handling `CURLE_AGAIN` with a short wait.
     libcurl handles must never be used from two threads at once; the mutex
     makes that true.
   * Errors are logged, not thrown into the GUI thread. That is the
     existing `trySend` contract in `KiwiSDRClient`.
5. **Stop.** `stopSocket()` sets the stop flag. The loop exits within one
   poll slice and does `curl_easy_cleanup()` on its own thread. No handle is
   touched after cleanup.
6. **Remove** the raw-socket code paths: framing, masking and upgrade
   parsing. libcurl does all of them.

## Review gate

1. The diff keeps the public API; `kiwisdr.cpp` and `websdr_view`
   compile unchanged.
2. The user's tests:
   * a plain KiwiSDR (for example `sdr-amradioantennas.com:8076`) streams
     as before;
   * hra.remoteqth.com:8073 (TLS only) connects, either through the
     redirect or by taking `https://` from transport/2;
   * stop/start ten times with no hang and no crash;
   * Android: one plain and one TLS connection (CA path).
