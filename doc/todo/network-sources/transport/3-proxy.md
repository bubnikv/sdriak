# Transport 3: proxy settings for all networked sources

Date: 2026-11-24. Status: steps 1–4 implemented (`core/src/utils/proxy.*`,
the "network" config section, `gui/menus/network.cpp`, applied in
`curl::make_easy()`; websockets always tunnel with CONNECT). Through an
in-process authenticating proxy (`core/tests/test_http_proxy.h`) the
KiwiSDR list, `/VER` and a TLS KiwiSDR stream work (`curl_http_tests
--directory --via-test-proxy`, `websock_tests --kiwi-client
hra.remoteqth.com:8073 --via-test-proxy`). Step 5 implemented:
`net::proxy::connect()` (utils/networking.h) and `connectSocket()`
(utils/net.h, `proxy_socket.h`) tunnel with CONNECT and Basic auth, match
`noProxy` themselves, refuse SOCKS; "Through the proxy" checkbox in the
SpyServer and SDRIAK server sources (config key `viaProxy`, default off);
covered by `curl_http_tests` and `proxy_socket_tests`, not tried against
a real SpyServer. Step 6 implemented: `MainActivity.getSystemProxy()`
(`ConnectivityManager.getDefaultProxy()`, else the `http.proxyHost`
properties) through `backend::getSystemProxy()`, cached 3 s; the C++
compiles with the NDK (syntax check), the Kotlin was not compiled (no JDK
on the build machine) and it is untested on a device. Windows PAC
implemented: system mode reads the settings with
`WinHttpGetIEProxyConfigForCurrentUser`; when a PAC script
(`AutoConfigURL`) is set or "Automatically detect settings" (WPAD) is on,
`WinHttpGetProxyForUrl` answers per target (`effective(url)`,
`make_easy(url)`, `tcpRoute(host, port)` as `https://host:port/`), cached
5 min per target, a failed script 1 min; on failure the static proxy, then
the environment applies. The first proxy of a PAC list is used; a redirect
libcurl follows to another host keeps the answer for the first. Covered by
`curl_http_tests` (a script served on loopback); not tried on a network
with a real PAC or WPAD server, and no NTLM/Negotiate proxy auth with the
logged-on user's credentials. Not done: UberSDR (not built).
libcurl matches `noProxy` itself, so zh's `bypasses()` is not ported.
Part of [README.md](README.md). Prerequisite: transport/1–2.

## Goal

One core network setting decides how every libcurl handle reaches the
internet:

* **no proxy;**
* **system proxy:** libcurl's environment handling, plus the Windows
  Internet Settings (WinINET);
* **manual,** with optional credentials.

It applies to the KiwiSDR, UberSDR and directory traffic. A CONNECT tunnel
for raw-TCP sources is optional.

## Source of the code

lzh173's `core/src/utils/proxy.{h,cpp}` in sdriak-zh (`cb338f40`,
`ad77b6f2`, `0b5967c7`); see
[../../../research/forks/zh.md](../../../research/forks/zh.md) §2. Take
the WinINET reader, the host:port parser and the bypass matcher, with
credit. Replace zh's hand-written CONNECT for websockets with libcurl's
proxy support, now that the websocket runs on libcurl.

## Steps

1. **Core settings.** Add a "Network" section to core config with these
   keys:

   | key | values |
   |-----|--------|
   | `proxyMode` | `"none"`, `"system"`, `"manual"`; default `"system"` |
   | `proxyUrl` | `http://host:port` |
   | `proxyUser` | user name |
   | `proxyPassword` | password, stored in plain text (say so in the UI) |
   | `noProxy` | comma-separated hosts |

   Add a small "Network" menu in `core/src/gui/menus/` showing the mode,
   the fields, and a line "In effect: …". Desktop and Android both get it.
2. **`net::proxy` in core.** From zh:
   * `systemSettings()`: WinINET on Windows (`ProxyEnable`, `ProxyServer`
     with per-scheme lists, `ProxyOverride`), otherwise the environment.
   * `bypasses()`.
   * `effective()`: the settings resolved by mode.
3. **Apply it in `curl::make_easy()`.**
   * `system` on Windows: `CURLOPT_PROXY` from WinINET, and
     `CURLOPT_NOPROXY` from `ProxyOverride` converted (`;`→`,`,
     `<local>`→ plain host names).
   * `system` elsewhere: set nothing, so libcurl reads
     `http_proxy`/`https_proxy`/`no_proxy`/`all_proxy` itself.
   * `manual`: `CURLOPT_PROXY`, `CURLOPT_PROXYUSERPWD` and
     `CURLOPT_NOPROXY`.
   * `none`: `CURLOPT_PROXY = ""` (disables the environment too).
4. **Fix zh's gaps on the way.** libcurl brackets IPv6 CONNECT authorities
   itself. Credentials are supported. `no_proxy` and `https_proxy` are
   honoured. The KiwiSDR-only checkbox and the `kiwisdr_proxy_addr` key
   are not needed.
5. **Optional: raw TCP.** Add `net::proxy::connect()` (zh's CONNECT
   tunnel, plus `Proxy-Authorization: Basic …` when credentials are set,
   plus `[v6]` bracketing) for `spyserver_source` and `sdriak_server_source`.
   Many proxies only allow CONNECT to 443, so this stays off unless the
   user enables it per source.
6. **Android.** Read the system proxy through JNI
   (`ConnectivityManager.getDefaultProxy()`) for `system` mode. This can be
   a later step; until then the environment applies, which is usually
   empty on Android.

## Review gate

* Through an authenticating proxy (for example a local squid with basic
  auth): the KiwiSDR map list, `/VER`, a TLS KiwiSDR stream and an UberSDR
  session (once that exists) all work.
* With `none`, nothing changes from today.
