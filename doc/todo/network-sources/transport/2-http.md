# Transport 2: HTTPS requests and the scheme probe

Date: 2026-11-24. Status: implemented. `core/tests/curl_http_tests`
covers GET/POST/redirect/cancel on loopback (also under ASan);
`websock_tests --kiwi-client` streamed IQ from hra.remoteqth.com (TLS) and
sdr-amradioantennas.com:8076 (plain); `curl_http_tests --directory`
loaded 844 servers. Two changes from the steps below: there is no separate
`probeBaseUrl()` (the `/VER` request is the probe, step 2), and the
directory stays on `http://` (step 4). Part of [README.md](README.md).

## Goal

Every HTTP request a networked source makes goes through libcurl, which
gives TLS, redirects and the proxy (transport/3). It must also support
POST with a JSON body, because UberSDR's `POST /connection` needs one.

## Files

* `core/src/utils/proto/curl_http.{h,cpp}`
* `core/src/utils/proto/kiwisdr.cpp` (`fetchServerTimestamp`)
* `core/src/gui/brown/kiwisdr_directory.cpp` (directory fetch)

## Steps

1. **Generalise the request call.** Add next to `curlGet`:

   ```cpp
   struct CurlRequest {
       std::string method = "GET";     // "GET" or "POST"
       std::string url;
       std::string body;               // POST body
       std::string contentType;        // e.g. "application/json"
   };
   SDRPP_EXPORT CurlResponse curlRequest(const CurlRequest& req,
                                         const CurlRequestOptions& options = CurlRequestOptions());
   ```

   * `curlGet` becomes a wrapper around `curlRequest`.
   * `CurlResponse` already carries `effectiveUrl` after redirects.
2. **Probe the scheme.** `urlOrigin(url)` and `urlPath(url)` in
   `curl_http.h`; the request a client makes anyway is the probe:
   * `fetchServerTimestamp` GETs `http://host:port/VER` with redirects
     followed and takes the origin (scheme, host, port) of `effectiveUrl`.
   * hra.remoteqth.com:8073 redirects to its site root,
     `https://hra.remoteqth.com:8073/`; when the final path is not `/VER`,
     `/VER` is asked again on that origin.
   * The websocket then opens on the same origin (`https` → `wss`), with
     no redirect of its own. A separate `probeBaseUrl()` would cost every
     connect a second request.
3. **Migrate `/VER`.** `KiwiSDRClient::fetchServerTimestamp` moves to
   `curlRequest` on the probed base URL. Keep the local-clock fallback.
4. **Migrate the directory fetch.** `kiwisdr_directory.cpp` moves from
   `net::http::Client` to `curlGet("http://rx.linkfanel.net/kiwisdr_com.js")`:
   that host serves no TLS (port 443 refused, 2026-11-24). The destructor
   cancels a running fetch through `shouldCancel`.
5. **Revocation best effort on Windows.** In `curl::make_easy()`
   (`core/src/utils/curl_init.cpp`) set
   `CURLOPT_SSL_OPTIONS = CURLSSLOPT_REVOKE_BEST_EFFORT`.
   * **Why:** Schannel fails the whole handshake when it cannot reach the
     certificate's revocation service, which is common behind proxies,
     captive portals and on mobile links. The user only sees a TLS error.
   * **Still safe:** best effort still rejects a certificate that is
     actually revoked.
   * **Other platforms:** Mbed-TLS does no revocation checking, so
     Windows stays at least as strict as the rest.
   * **Scope:** it applies to every handle from `make_easy()`, including
     today's `curlGet()` users.
6. **Leave the rest.** `net::http` (raw) stays for local LAN protocols
   (`spectran_http_source`).

## Review gate

* The KiwiSDR map loads its list.
* `/VER` succeeds on a TLS Kiwi and on a plain one.
* No new direct `curl.h` includes in modules: they keep using the core
  wrappers.
