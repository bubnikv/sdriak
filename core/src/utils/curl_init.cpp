#include "curl_init.h"

#include <utils/flog.h>
#include <utils/proxy.h>
#include <version.h>

namespace curl {

namespace {
    bool g_initialized = false;

    const char* tls_backend_name() {
        const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        if (info && info->ssl_version) return info->ssl_version;
        return "(no TLS backend)";
    }
}

void init() {
    if (g_initialized) return;
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        flog::error("curl_global_init failed: {0}", curl_easy_strerror(rc));
        return;
    }
    g_initialized = true;
    flog::info("libcurl: {0} (TLS: {1})", curl_version(), tls_backend_name());
}

void cleanup() {
    if (!g_initialized) return;
    curl_global_cleanup();
    g_initialized = false;
}

CURL* make_easy() {
    CURL* h = curl_easy_init();
    if (!h) return nullptr;

    curl_easy_setopt(h, CURLOPT_USERAGENT, "SDRIAK/" VERSION_STR);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    // Proxy: net::proxy resolves the user's mode (core config "network").
    const net::proxy::Effective proxy = net::proxy::effective();
    switch (proxy.kind) {
    case net::proxy::Effective::Kind::Direct:
        // "" also stops libcurl from reading http_proxy & co.
        curl_easy_setopt(h, CURLOPT_PROXY, "");
        break;
    case net::proxy::Effective::Kind::Proxy:
        curl_easy_setopt(h, CURLOPT_PROXY, proxy.url.c_str());
        curl_easy_setopt(h, CURLOPT_NOPROXY, proxy.noProxy.c_str());
        if (!proxy.user.empty()) {
            curl_easy_setopt(h, CURLOPT_PROXYUSERNAME, proxy.user.c_str());
            curl_easy_setopt(h, CURLOPT_PROXYPASSWORD, proxy.password.c_str());
            curl_easy_setopt(h, CURLOPT_PROXYAUTH, (long)CURLAUTH_ANY);
        }
        break;
    case net::proxy::Effective::Kind::Environment:
        break; // libcurl reads http_proxy / https_proxy / all_proxy / no_proxy
    }

#ifdef _WIN32
    // Schannel fails the whole handshake when the certificate's revocation
    // service is unreachable (proxies, captive portals, mobile links), and
    // the user only sees a TLS error. Best effort still rejects a
    // certificate that is known to be revoked; Mbed-TLS on the other
    // platforms checks no revocation at all.
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_REVOKE_BEST_EFFORT);
#endif

#ifdef __ANDROID__
    // Android ships PEM-encoded CA certs in this directory; libcurl's MbedTLS
    // backend has no other way to find them, and there is no system libcurl to
    // fall back to, so overriding unconditionally is safe here. macOS also
    // needs a trust store pointed out since its MbedTLS switch, but gets it
    // compiled into the bundled build (CURL_CA_BUNDLE in
    // deps/+libcurl/libcurl.cmake) so that a build resolving libcurl to the
    // system copy keeps that curl's own store.
    curl_easy_setopt(h, CURLOPT_CAPATH, "/system/etc/security/cacerts");
#endif

    return h;
}

} // namespace curl
