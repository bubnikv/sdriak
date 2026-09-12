# libcurl integration

Binding rule for plugin authors. Split out of the old `doc/Forks.txt`, where it
had been appended below an unrelated fork survey.

libcurl is the sanctioned HTTPS/WebSocket implementation dependency in core.
Plugins must not include <curl/curl.h>, add find_package(CURL), or link libcurl
directly. They should use the HTTP/WebSocket wrappers exported by sdrpp_core
instead. Static-linking libcurl into multiple DSOs in the same process is a known
source of nondeterministic TLS failures (curl has process-global state for TLS
callbacks, mutex tables, PRNG).

Per-platform TLS backend table:
    Platform   libcurl source             TLS backend         Cert store
    --------   -------------------------  ------------------  ----------------------------
    Windows    bundled (deps/+libcurl)    Schannel            Windows Cert Store
    macOS      bundled (+mbedtls dep)     MbedTLS             /etc/ssl/cert.pem (built in)
    Linux      system OR bundled          OpenSSL             /etc/ssl/certs
    Android    bundled (+mbedtls dep)     MbedTLS             /system/etc/security/cacerts

libcurl is owned by the deps system (deps/+libcurl/libcurl.cmake). Its
classification (deps/cmake/DepClassification.cmake) defaults to `system`
on the distro profile and `bundled` on portable/android profiles. Override
per-build with the standard dep knobs:
    -DSDRPP_DEP_FORCE_BUNDLED=libcurl  # AppImage/Flatpak or old distros
    -DSDRPP_DEP_FORCE_SYSTEM=libcurl   # use distro libcurl on a portable build

System libcurl must be ≥ 8.5 with WebSocket support compiled in
(curl_ws_send must exist). core/CMakeLists.txt probes the symbol after
linking and configure fails cleanly otherwise.

No CA bundle is ever shipped. On Android, curl_init.cpp sets CURLOPT_CAPATH to
the OS trust store via curl::make_easy(). macOS needs a store named too, since
the MbedTLS backend that replaced Secure Transport (removed upstream in curl
8.15.0) has none of its own, but it is compiled into the bundled build
(CURL_CA_BUNDLE=/etc/ssl/cert.pem) rather than forced at runtime: a macOS build
that resolves libcurl to the system copy must keep whatever store that curl was
built against. /etc/ssl/cert.pem is a static bundle rather than the Keychain, so
CAs a user installs into the Keychain are not honoured by the bundled build —
switching macOS to OpenSSL + USE_APPLE_SECTRUST is the way back to native trust
if that ever matters. Core networking wrappers use the handle factory so plugins
never have to touch libcurl directly.

The bundled recipe tracks current upstream curl (see deps/+libcurl/libcurl.cmake
for the pin). It must never be pinned to 8.11.1, the single release carrying
CVE-2025-0665: its threaded resolver double-closes the eventfd it uses to signal
resolve completion, so on 64-bit targets every DNS lookup frees an fd number
another thread may already own. Android's fdsan turns that into a process
abort.

curl_global_init/cleanup are called from sdrpp_main() in core. Plugins must
not call them.
