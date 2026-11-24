/*
 * HTTP proxy settings for every libcurl handle (curl::make_easy()).
 *
 * The ProxyServer list and ProxyOverride parsing are adapted from lzh173's
 * sdriak-zh (core/src/utils/proxy.cpp, cb338f40,
 * ad77b6f2), which tunnelled the KiwiSDR raw sockets itself. Here libcurl
 * does the proxying, CONNECT tunnels, authentication and no_proxy matching.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace net {
    class ConnClass;
    typedef std::unique_ptr<ConnClass> Conn; // utils/networking.h
}

namespace net::proxy {

    enum class Mode {
        None,   // direct, also ignoring the proxy environment variables
        System, // Windows Internet Settings (PAC, WPAD, static), the Android system proxy, else http_proxy/https_proxy/no_proxy
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

    // The settings resolved now for a request to url (http, https, ws, wss).
    // System mode reads the Windows Internet Settings on every call, so a
    // change there applies to the next request. When they name a PAC script
    // or "Automatically detect settings" is on, WinHTTP evaluates the script
    // for url's scheme, host and port (cached 5 min per target, a failure
    // 1 min); the static proxy applies when that fails. Without url the
    // script is not run and the description says what it falls back to.
    Effective effective(const std::string& url = std::string());

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

    // Raw TCP (SpyServer, SDRIAK server), which libcurl does not carry.

    // How a raw TCP connection to host goes: direct, or through an HTTP proxy
    // with CONNECT. Resolved like effective("https://host:port/"), port 443
    // when not given; in Environment mode from
    // all_proxy / https_proxy / http_proxy and no_proxy, credentials taken
    // from the URL. error is set for a proxy CONNECT cannot use (SOCKS).
    struct TcpRoute {
        bool direct = true;
        std::string proxyHost;
        int proxyPort = 0;
        std::string user;
        std::string password;
        std::string description;
        std::string error;
    };
    TcpRoute tcpRoute(const std::string& host, int port = 0);

    // TCP to host:port along tcpRoute(host). A tunnel sends CONNECT with
    // Proxy-Authorization: Basic when credentials are set and waits for a 2xx
    // within timeoutMs (10 s when negative). Throws std::runtime_error like
    // net::connect(). Many proxies allow CONNECT only to port 443, so sources
    // call this only when the user enabled it for them.
    net::Conn connect(const std::string& host, uint16_t port, int timeoutMs);
    // connectSocket() in utils/proxy_socket.h is the same for the utils/net.h
    // socket; the two socket libraries cannot share a translation unit.

    namespace detail {
        // The CONNECT request for authority ("host:port", "[v6]:port").
        std::string connectRequest(const TcpRoute& route, const std::string& authority);
        std::string authorityOf(const std::string& host, int port);
        // Throws unless head is a complete 2xx answer to CONNECT.
        void checkConnectReply(const std::string& head, bool timedOut, int budgetMs,
                               const TcpRoute& route, const std::string& authority);

        // What a PAC script is asked about url: "scheme://host:port/" with
        // ws -> http, wss -> https, credentials, path and query dropped.
        // Empty for other schemes and for no URL.
        std::string pacTarget(const std::string& url);

#ifdef _WIN32
        // FindProxyForURL(target) through WinHTTP, uncached: the script at
        // autoConfigUrl and/or the one WPAD finds. True with proxyUrl empty
        // for DIRECT, true with the first proxy as a URL, false with error.
        bool evaluatePac(const std::string& autoConfigUrl, bool autoDetect, const std::string& target,
                         std::string& proxyUrl, std::string& error);
#endif
    }

    // True when a libcurl-syntax no_proxy list says host is reached
    // directly: "*", a host or domain (also its subdomains, a leading dot
    // optional), an IPv4 CIDR, an exact address.
    bool noProxyMatches(const std::string& noProxy, const std::string& host);

    // WinINET ProxyOverride ("*.local;192.168.*;<local>") to libcurl's
    // no_proxy syntax: "*.x" -> ".x", "a.b.*" -> CIDR, "<local>" -> the
    // loopback names (libcurl cannot match "any dotless name"). Entries with a
    // '*' elsewhere cannot be expressed and are dropped.
    std::string winInetBypassToNoProxy(const std::string& proxyOverride);
}
