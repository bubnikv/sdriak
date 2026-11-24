#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

#include <module.h>

namespace net::http {

    struct CurlRequestOptions {
        int connectTimeoutMs = 15000;
        int timeoutMs = 30000;
        int maxRedirects = 5;
        size_t maxBody = 2 * 1024 * 1024;
        std::map<std::string, std::string> headers;
        // Optional cancellation callback. Polled periodically by libcurl while
        // the transfer is in progress; returning true aborts curlGet promptly
        // with std::runtime_error. Lets callers shut down without waiting for
        // timeoutMs.
        std::function<bool()> shouldCancel;
    };

    struct CurlResponse {
        long status = 0;
        std::string body;
        std::string effectiveUrl;
    };

    struct CurlRequest {
        std::string method = "GET"; // "GET", "POST" or another verb
        std::string url;
        std::string body;           // sent when non-empty or method is "POST"
        std::string contentType;    // Content-Type of body, e.g. "application/json"
    };

    // HTTP(S) request implemented by core through libcurl: TLS with the
    // platform trust store, redirects (a POST answered by 301/302/303 is
    // retried as GET). Any HTTP status is returned in CurlResponse::status;
    // transport failures, cancellation and an oversized body throw
    // std::runtime_error. Modules should use this instead of including
    // <curl/curl.h> or linking libcurl directly.
    CurlResponse curlRequest(const CurlRequest& request, const CurlRequestOptions& options = CurlRequestOptions());

    // curlRequest() with method GET.
    CurlResponse curlGet(const std::string& url, const CurlRequestOptions& options = CurlRequestOptions());

    // "scheme://host[:port]" of url, the port only when it is not the
    // scheme's default; empty when url does not parse. After redirects this
    // is how a client learns that http://host:port moved to https://.
    std::string urlOrigin(const std::string& url);

    // The path of url ("/" when it has none); empty when url does not parse.
    std::string urlPath(const std::string& url);

}
