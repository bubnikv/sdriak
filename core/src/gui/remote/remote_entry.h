#pragma once
// One receiver of any networked source, as the remote-receiver browser
// lists it. Directory providers produce these; source modules consume them
// through their RemoteHandler. Plan: doc/todo/remote-receiver-browser.md.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

namespace remote {

    enum class RemoteKind { KiwiSDR, UberSDR, SpyServer, OpenWebRX, SDRIAKServer, RtlTcp, NetSDR, Hermes, Other };

    constexpr RemoteKind kAllKinds[] = { RemoteKind::KiwiSDR, RemoteKind::UberSDR, RemoteKind::SpyServer,
                                         RemoteKind::OpenWebRX, RemoteKind::SDRIAKServer, RemoteKind::RtlTcp,
                                         RemoteKind::NetSDR, RemoteKind::Hermes, RemoteKind::Other };

    // Display name, e.g. "KiwiSDR", "OpenWebRX+".
    const char* kindName(RemoteKind kind);
    // Config/cache key, e.g. "kiwisdr", "openwebrx".
    const char* kindKey(RemoteKind kind);
    std::optional<RemoteKind> kindFromKey(const std::string& key);
    // Native-first order when one host offers several protocols: a server's
    // own protocol before the KiwiSDR emulation it may also offer.
    int kindRank(RemoteKind kind);

    // What a source needs to connect.
    struct RemoteTarget {
        std::string scheme; // "http", "https", "tcp"
        std::string host;
        int port = 0;
        std::string path; // optional, starts with '/'

        // scheme://host:port/path; the port is left out when it is the
        // scheme's default (80 for http, 443 for https).
        std::string url() const;
        // host:port
        std::string hostPort() const;
        // Lower-case host: the key de-duplication groups by.
        std::string hostKey() const;
    };

    // Parse "http[s]://host[:port][/path]" (or "host:port" with
    // defaultScheme). Returns nullopt when there is no host.
    std::optional<RemoteTarget> parseTarget(const std::string& url, const std::string& defaultScheme = "http");

    struct RemoteEntry {
        std::string id;       // stable: provider + host:port (or directory id)
        RemoteKind kind = RemoteKind::Other;
        std::string provider; // "kiwisdr.com", "rx-888.com", "sdr-list.xyz", "ubersdr.org", "airspy.com", "receiverbook.de", "recent"
        std::string name;
        std::string location;
        // Latitude, longitude in degrees. Absent: list view only.
        std::optional<std::pair<double, double>> latLon;
        RemoteTarget target;
        std::optional<int> users;
        std::optional<int> usersMax;
        std::vector<std::pair<uint64_t, uint64_t>> ranges; // served frequency ranges, Hz
        std::optional<float> snrDb;
        std::optional<int64_t> lastSeenUnix;
        std::string version;  // server software version, when listed
        bool tlsRequired = false;
        bool favourite = false;
        std::vector<std::pair<std::string, std::string>> details; // panel rows, already formatted
        std::vector<std::string> flags; // "full-control", "wide-iq", "ext-api", "waterfall", "audio-only"

        bool hasFlag(const std::string& flag) const;
        bool isFull() const { return users && usersMax && *usersMax > 0 && *users >= *usersMax; }
        // users/usersMax in [0, 1], or nullopt when the directory gives no capacity.
        std::optional<float> occupancy() const;
        bool covers(uint64_t hz) const;
        // Plain-text detail value, empty when absent.
        std::string detail(const std::string& key) const;
    };

    nlohmann::json entryToJson(const RemoteEntry& entry);
    std::optional<RemoteEntry> entryFromJson(const nlohmann::json& j);

    // Compact MHz range, e.g. "0-30 MHz" or "0.015-32 MHz".
    std::string formatRangeMHz(uint64_t startHz, uint64_t endHz);
    std::string formatRanges(const RemoteEntry& entry);

    // Maidenhead locator (2, 4 or 6 characters) to the centre of its square,
    // latitude/longitude in degrees. nullopt when it does not parse.
    std::optional<std::pair<double, double>> maidenheadToLatLon(const std::string& locator);
    // 6-character locator, empty when out of range.
    std::string latLonToMaidenhead(double lat, double lon);
    // Great-circle distance in km.
    double distanceKm(std::pair<double, double> a, std::pair<double, double> b);

}
