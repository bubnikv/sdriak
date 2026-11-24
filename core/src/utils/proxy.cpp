/*
 * HTTP proxy settings. See proxy.h; the WinINET reader and ProxyServer
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
        std::string readRegistryString(HKEY key, const wchar_t* name) {
            DWORD type = 0;
            DWORD size = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) { return {}; }
            if (type != REG_SZ && type != REG_EXPAND_SZ) { return {}; }
            std::wstring wbuf(size / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)wbuf.data(), &size) != ERROR_SUCCESS) { return {}; }
            while (!wbuf.empty() && wbuf.back() == L'\0') { wbuf.pop_back(); }
            if (wbuf.empty()) { return {}; }
            const int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(), nullptr, 0, nullptr, nullptr);
            std::string out(needed, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(), out.data(), needed, nullptr, nullptr);
            return out;
        }

        bool readRegistryDword(HKEY key, const wchar_t* name, DWORD& value) {
            DWORD type = 0;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS) { return false; }
            return type == REG_DWORD;
        }

        struct WinInet {
            bool enabled = false;
            std::string server;
            std::string override;
            std::string autoConfigUrl;
        };

        WinInet readWinInet() {
            WinInet w;
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                              0, KEY_READ, &key) != ERROR_SUCCESS) {
                return w;
            }
            DWORD enable = 0;
            w.enabled = readRegistryDword(key, L"ProxyEnable", enable) && enable;
            w.server = readRegistryString(key, L"ProxyServer");
            w.override = readRegistryString(key, L"ProxyOverride");
            w.autoConfigUrl = readRegistryString(key, L"AutoConfigURL");
            RegCloseKey(key);
            return w;
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

    Effective effective() {
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

#ifdef _WIN32
        const WinInet w = readWinInet();
        if (w.enabled && !w.server.empty()) {
            e.url = winInetProxyUrl(w.server);
            if (!e.url.empty()) {
                e.kind = Effective::Kind::Proxy;
                e.noProxy = winInetBypassToNoProxy(w.override);
                e.description = "Windows proxy " + e.url + (e.noProxy.empty() ? "" : ", not for " + e.noProxy);
                return e;
            }
        }
        const std::string pacNote = w.autoConfigUrl.empty() ? "" : "; Windows PAC script is not supported";
#else
        const std::string pacNote;
#endif
        e.kind = Effective::Kind::Environment;
        std::string name;
        if (const char* v = envProxy(name)) {
            e.description = "environment " + name + "=" + redact(v) + pacNote;
        }
        else {
            e.description = "direct (no system proxy)" + pacNote;
        }
        return e;
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

    TcpRoute tcpRoute(const std::string& host) {
        TcpRoute r;
        const Effective e = effective();
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
        const TcpRoute route = tcpRoute(host);
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
