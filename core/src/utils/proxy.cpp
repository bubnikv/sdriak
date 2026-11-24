/*
 * HTTP proxy settings. See proxy.h; the WinINET reader and ProxyServer
 * parsing are adapted from lzh173's sdriak-zh.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */
#include "proxy.h"

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

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
}
