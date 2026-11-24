/*
 * HTTP proxy settings for every libcurl handle (curl::make_easy()).
 *
 * The Windows Internet Settings reader and the ProxyServer list parsing are
 * adapted from lzh173's sdriak-zh (core/src/utils/proxy.cpp, cb338f40,
 * ad77b6f2), which tunnelled the KiwiSDR raw sockets itself. Here libcurl
 * does the proxying, CONNECT tunnels, authentication and no_proxy matching.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */
#pragma once

#include <string>

namespace net::proxy {

    enum class Mode {
        None,   // direct, also ignoring the proxy environment variables
        System, // Windows Internet Settings, else http_proxy/https_proxy/no_proxy
        Manual, // url, credentials and noProxy below
    };

    // The user's choice, persisted in core config ("network" section).
    struct Config {
        Mode mode = Mode::System;
        std::string url;      // "http://host:port"; a bare host:port is taken as http
        std::string user;
        std::string password;
        std::string noProxy;  // comma-separated hosts, domains (.example.com), CIDRs
    };

    // What curl::make_easy() applies to a handle.
    struct Effective {
        enum class Kind {
            Direct,      // CURLOPT_PROXY = "": no proxy, environment ignored
            Proxy,       // CURLOPT_PROXY = url, with credentials and noProxy
            Environment, // nothing set: libcurl reads the proxy variables itself
        };
        Kind kind = Kind::Environment;
        std::string url;
        std::string user;
        std::string password;
        std::string noProxy;
        // Human-readable summary for the "In effect" line and the log.
        std::string description;
    };

    void setConfig(const Config& config);
    Config config();

    // The settings resolved now. System mode reads the Windows Internet
    // Settings on every call, so a change there applies to the next request.
    Effective effective();

    const char* modeName(Mode mode);       // "none" / "system" / "manual"
    Mode modeFromName(const std::string& name); // unknown -> System

    // Exposed for tests and the menu.

    // "host:port", "scheme://host:port", "[v6]:port", credentials and path
    // stripped. False when no valid port is present.
    bool parseHostPort(const std::string& value, std::string& host, int& port);

    // A proxy URL libcurl accepts: a scheme is added when missing ("http://"),
    // an IPv6 host is bracketed. Empty when value does not parse.
    std::string normalizeUrl(const std::string& value);

    // WinINET ProxyServer ("host:port" or "http=h:p;https=h:p;socks=h:p") to
    // one proxy URL: http= first, then https=, then socks= as socks4a://.
    std::string winInetProxyUrl(const std::string& proxyServer);

    // WinINET ProxyOverride ("*.local;192.168.*;<local>") to libcurl's
    // no_proxy syntax: "*.x" -> ".x", "a.b.*" -> CIDR, "<local>" -> the
    // loopback names (libcurl cannot match "any dotless name"). Entries with a
    // '*' elsewhere cannot be expressed and are dropped.
    std::string winInetBypassToNoProxy(const std::string& proxyOverride);
}
