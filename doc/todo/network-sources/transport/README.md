# Transport: TLS, HTTPS and proxy for networked sources

Date: 2026-11-24. Status: current. Part of [../README.md](../README.md).

## Why

* **The websocket has no TLS.** `core/src/utils/proto/websock.{h,cpp}`
  (`net::websock::WSClient`) runs on raw TCP sockets. It follows HTTP
  redirects but cannot speak TLS, so every receiver behind HTTPS is
  unreachable:
  * 17 of the reachable public KiwiSDRs, including hra.remoteqth.com,
    which answers plain `ws://` with `302 → https://`;
  * 74 of 77 UberSDR instances.
* **Two raw-socket HTTP fetches have the same problem:**
  * the KiwiSDR `/VER` fetch (`net::http::get` in
    `core/src/utils/proto/kiwisdr.cpp`);
  * the directory fetch (`net::http::Client` in
    `core/src/gui/brown/kiwisdr_directory.cpp`).
* **No proxy.** None of these honours a proxy, so users behind a mandatory
  proxy (corporate networks, China) cannot reach any receiver.

## Decision: use libcurl for TLS, HTTP and WebSocket transport

**What we already ship.** Our bundled libcurl 8.22:

* builds with **WebSockets mandatory**: `core/CMakeLists.txt` fails the
  configure without `curl_ws_send`, and `deps/+libcurl/libcurl.cmake` sets
  `CURL_DISABLE_WEBSOCKETS=OFF`;
* has the platform's TLS backend: Schannel on Windows, Mbed-TLS on
  Android, macOS and bundled Linux;
* already has its trust stores wired up in `core/src/utils/curl_init.cpp`
  (`make_easy()`).

**What it gives us:**

* TLS with the right trust store on every platform;
* HTTP CONNECT proxies, with authentication and the
  `http_proxy`/`https_proxy`/`no_proxy` environment;
* redirects.

**Rejected:** writing our own TLS layer under the raw sockets. Mbed-TLS is
not even built on Windows, and we would re-implement trust-store, SNI and
proxy handling that libcurl already does.

**What stays raw TCP:** raw-TCP protocols (SpyServer, rtl_tcp, NetSDR,
SDRIAK server) stay on `utils/networking`. An HTTP-CONNECT path for them is
optional (§3).

## Sub-plans

| plan | content | gate |
|------|---------|------|
| [1-curl-websocket.md](1-curl-websocket.md) | `WSClient` moved onto `curl_ws_*`, public API kept, `ws://` and `wss://` URLs | the KiwiSDR source connects to a TLS Kiwi (hra.remoteqth.com) and to a plain one, with no behaviour change otherwise |
| [2-http.md](2-http.md) | `curlRequest()` (GET/POST, headers, body) and the scheme probe; migrate the `/VER` and directory fetches | the KiwiSDR directory and `/VER` work over HTTPS and behind redirects |
| [3-proxy.md](3-proxy.md) | core network settings (no proxy / system / manual, with credentials), WinINET reader (from zh), applied to every libcurl handle; optional CONNECT for raw TCP | KiwiSDR and UberSDR work through an authenticating HTTP proxy |
