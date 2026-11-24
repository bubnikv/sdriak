/*
 * HTTP proxy settings. See proxy.h; the ProxyServer and ProxyOverride
 * parsing are adapted from lzh173's sdriak-zh.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */
#include "proxy.h"

// Before <windows.h>: WinSock2 must come ahead of winsock.h.
#include <utils/networking.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <utils/base64.h>
#include <utils/flog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#ifdef __ANDROID__
#include <android_backend.h>
#endif

namespace net::proxy {

    namespace {
        std::mutex g_mtx;
        Config g_config;

        std::string trim(const std::string& in) {
            size_t begin = 0;
            size_t end = in.size();
            while (begin < end && std::isspace((unsigned char)in[begin])) { begin++; }
            while (end > begin && std::isspace((unsigned char)in[end - 1])) { end--; }
            return in.substr(begin, end - begin);
        }

        std::string lower(std::string in) {
            for (auto& c : in) { c = (char)std::tolower((unsigned char)c); }
            return in;
        }

        std::vector<std::string> split(const std::string& s, char sep) {
            std::vector<std::string> out;
            size_t pos = 0;
            while (pos <= s.size()) {
                const size_t next = s.find(sep, pos);
                out.push_back(s.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
                if (next == std::string::npos) { break; }
                pos = next + 1;
            }
            return out;
        }

        // The URL without credentials, for descriptions and logs.
        std::string redact(const std::string& url) {
            const size_t scheme = url.find("://");
            const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
            const size_t at = url.find('@', start);
            const size_t slash = url.find('/', start);
            if (at == std::string::npos || (slash != std::string::npos && at > slash)) { return url; }
            return url.substr(0, start) + url.substr(at + 1);
        }

        const char* envProxy(std::string& name) {
            for (const char* n : { "https_proxy", "HTTPS_PROXY", "http_proxy", "all_proxy", "ALL_PROXY" }) {
                const char* v = std::getenv(n);
                if (v && *v) {
                    name = n;
                    return v;
                }
            }
            return nullptr;
        }

#ifdef _WIN32
        std::wstring widen(const std::string& s) {
            if (s.empty()) { return {}; }
            const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring out(needed, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), needed);
            return out;
        }

        std::string narrow(const wchar_t* w) {
            if (!w || !*w) { return {}; }
            const int len = (int)wcslen(w);
            const int needed = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
            std::string out(needed, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), needed, nullptr, nullptr);
            return out;
        }

        // The Internet Settings of the current connection, as Internet
        // Options shows them. server is set only while "Use a proxy server" is
        // on; autoDetect is "Automatically detect settings" (WPAD).
        struct WinInet {
            std::string server;
            std::string override;
            std::string autoConfigUrl;
            bool autoDetect = false;
        };

        WinInet readWinInet() {
            WinInet w;
            WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie = {};
            if (!WinHttpGetIEProxyConfigForCurrentUser(&ie)) { return w; }
            w.autoDetect = ie.fAutoDetect != FALSE;
            w.autoConfigUrl = narrow(ie.lpszAutoConfigUrl);
            w.server = narrow(ie.lpszProxy);
            w.override = narrow(ie.lpszProxyBypass);
            for (LPWSTR p : { ie.lpszAutoConfigUrl, ie.lpszProxy, ie.lpszProxyBypass }) {
                if (p) { GlobalFree(p); }
            }
            return w;
        }

        std::string winHttpErrorText(DWORD err) {
            switch (err) {
            case ERROR_WINHTTP_AUTODETECTION_FAILED: return "no PAC script found (WPAD)";
            case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT: return "cannot download the PAC script";
            case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT: return "the PAC script failed";
            case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "URL scheme not supported";
            default: return "WinHTTP error " + std::to_string(err);
            }
        }

        // One session for the process: WinHTTP keeps the downloaded script
        // with it.
        HINTERNET pacSession() {
            static HINTERNET session = [] {
                HINTERNET s = WinHttpOpen(L"SDRIAK", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
                // Bounds the script download.
                if (s) { WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000); }
                return s;
            }();
            return session;
        }

        struct PacAnswer {
            bool ok = false;
            std::string proxyUrl; // empty: DIRECT
            std::string error;
        };

        // FindProxyForURL is evaluated per host, and with WPAD the lookup
        // alone can take seconds, so answers are kept: an answer for 5 min
        // per target, a failure for 1 min for the whole script, so that a
        // missing WPAD server costs one lookup and not one per request.
        PacAnswer cachedPac(const WinInet& w, const std::string& target) {
            using Clock = std::chrono::steady_clock;
            struct Entry {
                PacAnswer answer;
                Clock::time_point at;
            };
            static std::mutex mtx;
            static std::map<std::string, Entry> answers;
            static std::map<std::string, Entry> failures;

            const std::string script = w.autoConfigUrl + (w.autoDetect ? "\n+wpad" : "\n");
            const std::string key = script + "\n" + target;
            const auto now = Clock::now();
            {
                std::lock_guard<std::mutex> lock(mtx);
                const auto f = failures.find(script);
                if (f != failures.end() && now - f->second.at < std::chrono::minutes(1)) { return f->second.answer; }
                const auto a = answers.find(key);
                if (a != answers.end() && now - a->second.at < std::chrono::minutes(5)) { return a->second.answer; }
            }

            // Evaluated without the lock: a slow lookup does not hold up
            // requests that are already answered.
            PacAnswer answer;
            answer.ok = detail::evaluatePac(w.autoConfigUrl, w.autoDetect, target, answer.proxyUrl, answer.error);
            flog::info("proxy: PAC for {}: {}", target,
                       !answer.ok ? "failed, " + answer.error : answer.proxyUrl.empty() ? std::string("DIRECT") : answer.proxyUrl);

            std::lock_guard<std::mutex> lock(mtx);
            if (answers.size() > 256) { answers.clear(); }
            (answer.ok ? answers[key] : failures[script]) = Entry{ answer, Clock::now() };
            return answer;
        }
#endif

#ifdef __ANDROID__
        // MainActivity.getSystemProxy() over JNI; effective() runs for every
        // libcurl handle and once a second in the Network menu.
        std::string androidSystemProxy() {
            static std::mutex mtx;
            static std::string cached;
            static std::chrono::steady_clock::time_point at;
            static bool have = false;
            std::lock_guard<std::mutex> lock(mtx);
            const auto now = std::chrono::steady_clock::now();
            if (!have || now - at > std::chrono::seconds(3)) {
                cached = backend::getSystemProxy();
                at = now;
                have = true;
            }
            return cached;
        }
#endif
    }

    const char* modeName(Mode mode) {
        switch (mode) {
        case Mode::None: return "none";
        case Mode::Manual: return "manual";
        case Mode::System: default: return "system";
        }
    }

    Mode modeFromName(const std::string& name) {
        if (name == "none") { return Mode::None; }
        if (name == "manual") { return Mode::Manual; }
        return Mode::System;
    }

    void setConfig(const Config& config) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_config = config;
    }

    Config config() {
        std::lock_guard<std::mutex> lock(g_mtx);
        return g_config;
    }

    bool parseHostPort(const std::string& valueIn, std::string& host, int& port) {
        std::string value = trim(valueIn);
        if (value.empty()) { return false; }

        const auto schemeEnd = value.find("://");
        if (schemeEnd != std::string::npos) { value = value.substr(schemeEnd + 3); }
        const auto slash = value.find('/');
        if (slash != std::string::npos) { value = value.substr(0, slash); }
        const auto at = value.rfind('@');
        if (at != std::string::npos) { value = value.substr(at + 1); }

        const auto colon = value.rfind(':');
        if (colon == std::string::npos) { return false; }
        // "[::1]" alone has its last colon inside the brackets.
        if (value.front() == '[' && value.find(']') > colon) { return false; }

        const std::string portStr = value.substr(colon + 1);
        if (portStr.empty() || portStr.size() > 5) { return false; }
        for (char c : portStr) {
            if (!std::isdigit((unsigned char)c)) { return false; }
        }
        const int parsedPort = std::atoi(portStr.c_str());
        if (parsedPort <= 0 || parsedPort > 65535) { return false; }

        host = trim(value.substr(0, colon));
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        if (host.empty()) { return false; }
        port = parsedPort;
        return true;
    }

    std::string normalizeUrl(const std::string& valueIn) {
        const std::string value = trim(valueIn);
        std::string host;
        int port = 0;
        if (!parseHostPort(value, host, port)) { return {}; }
        std::string scheme = "http";
        const auto schemeEnd = value.find("://");
        if (schemeEnd != std::string::npos) { scheme = lower(value.substr(0, schemeEnd)); }
        const bool v6 = host.find(':') != std::string::npos;
        return scheme + "://" + (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    }

    std::string winInetProxyUrl(const std::string& proxyServerIn) {
        const std::string proxyServer = trim(proxyServerIn);
        if (proxyServer.find('=') == std::string::npos) { return normalizeUrl(proxyServer); }
        std::string http, https, socks;
        for (const std::string& entry : split(proxyServer, ';')) {
            const auto eq = entry.find('=');
            if (eq == std::string::npos) { continue; }
            const std::string scheme = lower(trim(entry.substr(0, eq)));
            const std::string target = trim(entry.substr(eq + 1));
            if (scheme == "http") { http = target; }
            else if (scheme == "https") { https = target; }
            else if (scheme == "socks") { socks = target; }
        }
        if (!http.empty()) { return normalizeUrl(http); }
        if (!https.empty()) { return normalizeUrl(https); }
        // WinINET speaks SOCKS4 to a socks= proxy; let it resolve names.
        if (!socks.empty()) { return normalizeUrl("socks4a://" + socks); }
        return {};
    }

    std::string winInetBypassToNoProxy(const std::string& proxyOverride) {
        std::vector<std::string> out;
        auto add = [&out](const std::string& e) {
            for (const auto& have : out) {
                if (have == e) { return; }
            }
            out.push_back(e);
        };
        for (std::string entry : split(proxyOverride, ';')) {
            entry = trim(entry);
            if (entry.empty()) { continue; }
            if (lower(entry) == "<local>") {
                add("localhost");
                add("127.0.0.1");
                add("::1");
                continue;
            }
            if (entry == "*") {
                add("*");
                continue;
            }
            if (entry.rfind("*.", 0) == 0 && entry.find('*', 1) == std::string::npos) {
                add(entry.substr(1)); // ".example.com": the domain and its subdomains
                continue;
            }
            const size_t star = entry.find('*');
            if (star == std::string::npos) {
                add(entry);
                continue;
            }
            // "10.*", "192.168.*", "192.168.1.*" -> CIDR.
            if (star == entry.size() - 1 && entry.size() >= 2 && entry[entry.size() - 2] == '.') {
                const std::vector<std::string> octets = split(entry.substr(0, entry.size() - 2), '.');
                bool numeric = !octets.empty() && octets.size() <= 3;
                for (const auto& o : octets) {
                    numeric = numeric && !o.empty() && o.size() <= 3 &&
                              o.find_first_not_of("0123456789") == std::string::npos && std::atoi(o.c_str()) <= 255;
                }
                if (numeric) {
                    std::string cidr;
                    for (size_t i = 0; i < 4; i++) {
                        if (i) { cidr += "."; }
                        cidr += i < octets.size() ? octets[i] : "0";
                    }
                    add(cidr + "/" + std::to_string(8 * octets.size()));
                }
            }
            // Other globs ("*foo*", "10.*.1.1") have no no_proxy form.
        }
        std::string joined;
        for (const auto& e : out) {
            if (!joined.empty()) { joined += ","; }
            joined += e;
        }
        return joined;
    }

    Effective effective(const std::string& url) {
        const Config c = config();
        Effective e;
        switch (c.mode) {
        case Mode::None:
            e.kind = Effective::Kind::Direct;
            e.description = "direct (no proxy)";
            return e;

        case Mode::Manual: {
            e.url = normalizeUrl(c.url);
            if (e.url.empty()) {
                e.kind = Effective::Kind::Direct;
                e.description = c.url.empty() ? "direct (no manual proxy set)"
                                              : "direct (cannot parse proxy '" + redact(c.url) + "')";
                return e;
            }
            e.kind = Effective::Kind::Proxy;
            e.user = c.user;
            e.password = c.password;
            e.noProxy = c.noProxy;
            e.description = e.url + (c.user.empty() ? "" : " as " + c.user) +
                            (c.noProxy.empty() ? "" : ", not for " + c.noProxy);
            return e;
        }

        case Mode::System:
        default:
            break;
        }

#ifdef __ANDROID__
        {
            // "host:port\nexclusion,exclusion"; the exclusions use the same
            // globs as ProxyOverride ("*.example.com", "192.168.*").
            const std::string sys = androidSystemProxy();
            const size_t nl = sys.find('\n');
            e.url = normalizeUrl(sys.substr(0, nl));
            if (!e.url.empty()) {
                std::string exclusions = nl == std::string::npos ? std::string() : sys.substr(nl + 1);
                for (char& ch : exclusions) {
                    if (ch == ',') { ch = ';'; }
                }
                e.kind = Effective::Kind::Proxy;
                e.noProxy = winInetBypassToNoProxy(exclusions);
                e.description = "Android proxy " + e.url + (e.noProxy.empty() ? "" : ", not for " + e.noProxy);
                return e;
            }
        }
#endif
#ifdef _WIN32
        // Internet Options order: automatic configuration (WPAD, then the
        // PAC script) first, the static proxy when it has no answer.
        const WinInet w = readWinInet();
        std::string pac;
        if (w.autoDetect || !w.autoConfigUrl.empty()) {
            pac = !w.autoConfigUrl.empty() ? "PAC script " + redact(w.autoConfigUrl) : std::string("auto-detected PAC (WPAD)");
            if (w.autoDetect && !w.autoConfigUrl.empty()) { pac = "auto-detected PAC (WPAD) or " + pac; }
            const std::string target = detail::pacTarget(url);
            if (!target.empty()) {
                const PacAnswer answer = cachedPac(w, target);
                if (answer.ok) {
                    // The answer is for this host; a redirect that libcurl
                    // follows to another host keeps it.
                    e.kind = answer.proxyUrl.empty() ? Effective::Kind::Direct : Effective::Kind::Proxy;
                    e.url = answer.proxyUrl;
                    e.description = "Windows " + pac + ": " + (e.url.empty() ? std::string("DIRECT") : e.url) + " for " + target;
                    return e;
                }
                pac += " failed (" + answer.error + ")";
            }
        }
        // Without a URL, as for the menu's "In effect" line, the automatic
        // configuration is named in front of what it falls back to.
        const std::string pacPrefix = pac.empty() ? "" : "Windows " + pac + (url.empty() ? " per host" : "") + ", else ";
        if (!w.server.empty()) {
            e.url = winInetProxyUrl(w.server);
            if (!e.url.empty()) {
                e.kind = Effective::Kind::Proxy;
                e.noProxy = winInetBypassToNoProxy(w.override);
                e.description = pacPrefix + "Windows proxy " + e.url + (e.noProxy.empty() ? "" : ", not for " + e.noProxy);
                return e;
            }
        }
#else
        const std::string pacPrefix;
#endif
        e.kind = Effective::Kind::Environment;
        std::string name;
        if (const char* v = envProxy(name)) {
            e.description = pacPrefix + "environment " + name + "=" + redact(v);
        }
        else {
            e.description = pacPrefix + "direct (no system proxy)";
        }
        return e;
    }

    namespace detail {
        std::string pacTarget(const std::string& url) {
            const size_t sep = url.find("://");
            if (sep == std::string::npos || sep == 0) { return {}; }
            std::string scheme = lower(url.substr(0, sep));
            if (scheme == "ws") { scheme = "http"; }
            else if (scheme == "wss") { scheme = "https"; }
            else if (scheme != "http" && scheme != "https") { return {}; }
            const size_t end = url.find_first_of("/?#", sep + 3);
            std::string authority = url.substr(sep + 3, end == std::string::npos ? std::string::npos : end - (sep + 3));
            const size_t at = authority.rfind('@');
            if (at != std::string::npos) { authority = authority.substr(at + 1); }
            if (authority.empty()) { return {}; }
            return scheme + "://" + lower(authority) + "/";
        }

#ifdef _WIN32
        bool evaluatePac(const std::string& autoConfigUrl, bool autoDetect, const std::string& target,
                         std::string& proxyUrl, std::string& error) {
            proxyUrl.clear();
            error.clear();
            HINTERNET session = pacSession();
            if (!session) {
                error = winHttpErrorText(GetLastError());
                return false;
            }
            WINHTTP_AUTOPROXY_OPTIONS options = {};
            std::wstring configUrl;
            if (autoDetect) {
                options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
                options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
            }
            if (!autoConfigUrl.empty()) {
                options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
                configUrl = widen(autoConfigUrl);
                options.lpszAutoConfigUrl = configUrl.c_str();
            }
            if (!options.dwFlags) {
                error = "no PAC script configured";
                return false;
            }
            // Credentials of the logged-on user for a script server that
            // asks for them.
            options.fAutoLogonIfChallenged = TRUE;

            WINHTTP_PROXY_INFO info = {};
            const std::wstring wtarget = widen(target);
            if (!WinHttpGetProxyForUrl(session, wtarget.c_str(), &options, &info)) {
                error = winHttpErrorText(GetLastError());
                return false;
            }
            const std::string list = narrow(info.lpszProxy);
            const bool named = info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            if (info.lpszProxy) { GlobalFree(info.lpszProxy); }
            if (info.lpszProxyBypass) { GlobalFree(info.lpszProxyBypass); }
            if (!named || list.empty()) { return true; } // DIRECT

            // "PROXY a:1; PROXY b:2" arrives as "a:1;b:2"; libcurl takes one
            // proxy, the first.
            const std::string first = trim(list.substr(0, list.find_first_of("; \t")));
            proxyUrl = winInetProxyUrl(first);
            if (proxyUrl.empty()) {
                error = "cannot parse the PAC answer '" + list + "'";
                return false;
            }
            return true;
        }
#endif
    }

    namespace {
        std::string percentDecode(const std::string& s) {
            std::string out;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                    std::isxdigit((unsigned char)s[i + 2])) {
                    out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else {
                    out.push_back(s[i]);
                }
            }
            return out;
        }

        // "user:pass" between "://" and '@' of a proxy URL.
        void urlCredentials(const std::string& url, std::string& user, std::string& password) {
            const size_t scheme = url.find("://");
            const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
            const size_t at = url.find('@', start);
            const size_t slash = url.find('/', start);
            if (at == std::string::npos || (slash != std::string::npos && at > slash)) { return; }
            const std::string cred = url.substr(start, at - start);
            const size_t colon = cred.find(':');
            user = percentDecode(cred.substr(0, colon));
            password = colon == std::string::npos ? std::string() : percentDecode(cred.substr(colon + 1));
        }

        bool parseIPv4(const std::string& s, uint32_t& out) {
            const std::vector<std::string> parts = split(s, '.');
            if (parts.size() != 4) { return false; }
            out = 0;
            for (const auto& p : parts) {
                if (p.empty() || p.size() > 3 || p.find_first_not_of("0123456789") != std::string::npos) { return false; }
                const int v = std::atoi(p.c_str());
                if (v > 255) { return false; }
                out = (out << 8) | (uint32_t)v;
            }
            return true;
        }

        const char* firstEnv(std::initializer_list<const char*> names) {
            for (const char* n : names) {
                const char* v = std::getenv(n);
                if (v && *v) { return v; }
            }
            return nullptr;
        }
    }

    bool noProxyMatches(const std::string& noProxy, const std::string& hostIn) {
        std::string host = lower(trim(hostIn));
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') { host = host.substr(1, host.size() - 2); }
        uint32_t hostIp = 0;
        const bool hostIsIPv4 = parseIPv4(host, hostIp);
        for (std::string entry : split(noProxy, ',')) {
            entry = lower(trim(entry));
            if (entry.empty()) { continue; }
            if (entry == "*") { return true; }
            const size_t slash = entry.find('/');
            if (slash != std::string::npos) {
                uint32_t net = 0;
                const int bits = std::atoi(entry.c_str() + slash + 1);
                if (hostIsIPv4 && parseIPv4(entry.substr(0, slash), net) && bits >= 0 && bits <= 32) {
                    const uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
                    if ((hostIp & mask) == (net & mask)) { return true; }
                }
                continue;
            }
            if (entry.front() == '.') { entry = entry.substr(1); }
            if (entry.size() >= 2 && entry.front() == '[' && entry.back() == ']') { entry = entry.substr(1, entry.size() - 2); }
            if (host == entry) { return true; }
            if (!hostIsIPv4 && host.size() > entry.size() &&
                host.compare(host.size() - entry.size(), entry.size(), entry) == 0 &&
                host[host.size() - entry.size() - 1] == '.') {
                return true;
            }
        }
        return false;
    }

    TcpRoute tcpRoute(const std::string& host, int port) {
        TcpRoute r;
        // A PAC script is asked what it does for https to host.
        const Effective e = effective("https://" + detail::authorityOf(host, port > 0 ? port : 443) + "/");
        std::string url;
        std::string noProxy;
        switch (e.kind) {
        case Effective::Kind::Direct:
            r.description = "direct";
            return r;
        case Effective::Kind::Proxy:
            url = e.url;
            noProxy = e.noProxy;
            r.user = e.user;
            r.password = e.password;
            break;
        case Effective::Kind::Environment: {
            // libcurl's order for a non-HTTP transfer; http_proxy only in
            // lower case, as libcurl reads it.
            const char* v = firstEnv({ "all_proxy", "ALL_PROXY", "https_proxy", "HTTPS_PROXY", "http_proxy" });
            if (!v) {
                r.description = "direct";
                return r;
            }
            url = v;
            const char* np = firstEnv({ "no_proxy", "NO_PROXY" });
            noProxy = np ? np : "";
            urlCredentials(url, r.user, r.password);
            break;
        }
        }
        if (noProxyMatches(noProxy, host)) {
            r.description = "direct (no proxy for " + host + ")";
            return r;
        }
        const std::string scheme = lower(url.substr(0, url.find("://") == std::string::npos ? 0 : url.find("://")));
        if (!scheme.empty() && scheme != "http") {
            r.error = "a " + scheme + " proxy cannot tunnel raw TCP (" + redact(url) + ")";
            return r;
        }
        if (!parseHostPort(url, r.proxyHost, r.proxyPort)) {
            r.error = "cannot parse proxy '" + redact(url) + "'";
            return r;
        }
        r.direct = false;
        r.description = "CONNECT through " + redact(normalizeUrl(url)) + (r.user.empty() ? "" : " as " + r.user);
        return r;
    }

    namespace detail {
        // The CONNECT request for host:port along route.
        std::string connectRequest(const TcpRoute& route, const std::string& authority) {
            std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
            if (!route.user.empty()) {
                const std::string cred = route.user + ":" + route.password;
                request += "Proxy-Authorization: Basic " +
                           base64::encode(reinterpret_cast<const uint8_t*>(cred.data()), cred.size()) + "\r\n";
            }
            return request + "\r\n";
        }

        std::string authorityOf(const std::string& host, int port) {
            const bool v6 = host.find(':') != std::string::npos && host.front() != '[';
            return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
        }

        // Throws unless head is a complete 2xx answer to CONNECT.
        void checkConnectReply(const std::string& head, bool timedOut, int budgetMs,
                               const TcpRoute& route, const std::string& authority) {
            if (timedOut) {
                throw std::runtime_error("proxy: no answer to CONNECT " + authority + " within " + std::to_string(budgetMs) + " ms");
            }
            if (head.find("\r\n\r\n") == std::string::npos) {
                throw std::runtime_error("proxy: connection closed during CONNECT " + authority);
            }
            const std::string statusLine = head.substr(0, head.find("\r\n"));
            const size_t sp = statusLine.find(' ');
            const int status = sp == std::string::npos ? 0 : std::atoi(statusLine.c_str() + sp + 1);
            if (status == 407) {
                throw std::runtime_error("proxy: " + route.proxyHost + " requires authentication (" + statusLine + ")");
            }
            if (status < 200 || status > 299) {
                throw std::runtime_error("proxy: CONNECT " + authority + " refused: " + statusLine);
            }
            flog::info("proxy: tunnel to {} {}", authority, route.description);
        }
    }

    net::Conn connect(const std::string& host, uint16_t port, int timeoutMs) {
        const TcpRoute route = tcpRoute(host, port);
        if (!route.error.empty()) { throw std::runtime_error("proxy: " + route.error); }
        if (route.direct) { return net::connect(host, port, timeoutMs); }

        const int budgetMs = timeoutMs < 0 ? 10000 : timeoutMs;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        net::Conn conn = net::connect(route.proxyHost, (uint16_t)route.proxyPort, budgetMs);
        if (!conn) { throw std::runtime_error("proxy: could not connect to " + route.proxyHost); }

        const std::string authority = detail::authorityOf(host, port);
        std::string request = detail::connectRequest(route, authority);
        if (!conn->write((int)request.size(), reinterpret_cast<uint8_t*>(request.data()))) {
            throw std::runtime_error("proxy: sending CONNECT to " + route.proxyHost + " failed");
        }

        // ConnClass::read() has no timeout: a watchdog closes the socket at
        // the deadline, which wakes the read. One byte at a time, so nothing
        // the server sends through the tunnel is consumed here.
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        bool timedOut = false;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lock(mtx);
            if (!cv.wait_until(lock, deadline, [&] { return done; })) {
                timedOut = true;
                conn->close();
            }
        });
        std::string head;
        uint8_t ch = 0;
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
            if (conn->read(1, &ch, true) != 1) { break; }
            head.push_back((char)ch);
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
        }
        cv.notify_all();
        watchdog.join();

        detail::checkConnectReply(head, timedOut, budgetMs, route, authority);
        return conn;
    }
}
