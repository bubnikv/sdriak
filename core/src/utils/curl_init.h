#pragma once

#include <curl/curl.h>

#include <string>

namespace curl {
    // Process-wide libcurl init/teardown. Call init() exactly once early in
    // startup (before any thread spawns or any HTTP/WS work) and cleanup()
    // exactly once at shutdown. Plugins must not call either.
    void init();
    void cleanup();

    // Allocates a CURL easy handle with shared defaults applied: User-Agent,
    // follow-redirects, no SIGPIPE/SIGALRM, the proxy net::proxy resolves
    // for url (a PAC script answers per host; see net::proxy::effective), on
    // Windows best-effort revocation checks, and on Android the OS trust store
    // via CURLOPT_CAPATH. Caller owns the handle and must curl_easy_cleanup()
    // it. Returns nullptr on allocation failure.
    CURL* make_easy(const std::string& url = std::string());
}
