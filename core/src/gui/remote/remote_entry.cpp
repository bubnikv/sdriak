#include "remote_entry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace remote {

    namespace {
        struct KindInfo {
            RemoteKind kind;
            const char* name;
            const char* key;
            int rank;
        };
        constexpr KindInfo kKinds[] = {
            { RemoteKind::UberSDR, "UberSDR", "ubersdr", 0 },
            { RemoteKind::OpenWebRX, "OpenWebRX+", "openwebrx", 1 },
            { RemoteKind::SpyServer, "SpyServer", "spyserver", 2 },
            { RemoteKind::SDRIAKServer, "SDRIAK Server", "sdriak_server", 3 },
            { RemoteKind::RtlTcp, "RTL-TCP", "rtl_tcp", 4 },
            { RemoteKind::NetSDR, "RFspace", "netsdr", 5 },
            { RemoteKind::Hermes, "Hermes", "hermes", 6 },
            { RemoteKind::KiwiSDR, "KiwiSDR", "kiwisdr", 7 },
            { RemoteKind::Other, "Other", "other", 8 },
        };

        const KindInfo& info(RemoteKind kind) {
            for (const auto& k : kKinds) {
                if (k.kind == kind) { return k; }
            }
            return kKinds[sizeof(kKinds) / sizeof(kKinds[0]) - 1];
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        int defaultPort(const std::string& scheme) {
            if (scheme == "https" || scheme == "wss") { return 443; }
            if (scheme == "http" || scheme == "ws") { return 80; }
            return 0;
        }
    }

    const char* kindName(RemoteKind kind) { return info(kind).name; }
    const char* kindKey(RemoteKind kind) { return info(kind).key; }
    int kindRank(RemoteKind kind) { return info(kind).rank; }

    std::optional<RemoteKind> kindFromKey(const std::string& key) {
        for (const auto& k : kKinds) {
            if (key == k.key) { return k.kind; }
        }
        return std::nullopt;
    }

    std::string RemoteTarget::url() const {
        std::string out = scheme.empty() ? std::string("http") : scheme;
        out += "://" + host;
        if (port > 0 && port != defaultPort(out.substr(0, out.find(':')))) { out += ":" + std::to_string(port); }
        out += path.empty() ? std::string("/") : path;
        return out;
    }

    std::string RemoteTarget::hostPort() const {
        return host + ":" + std::to_string(port);
    }

    std::string RemoteTarget::hostKey() const {
        return lower(host);
    }

    std::optional<RemoteTarget> parseTarget(const std::string& urlIn, const std::string& defaultScheme) {
        std::string url = urlIn;
        while (!url.empty() && std::isspace((unsigned char)url.back())) { url.pop_back(); }
        size_t start = 0;
        while (start < url.size() && std::isspace((unsigned char)url[start])) { start++; }
        url = url.substr(start);

        RemoteTarget t;
        size_t pos = url.find("://");
        if (pos != std::string::npos) {
            t.scheme = lower(url.substr(0, pos));
            url = url.substr(pos + 3);
        }
        else {
            t.scheme = defaultScheme;
        }
        const size_t slash = url.find_first_of("/?#");
        std::string authority = slash == std::string::npos ? url : url.substr(0, slash);
        if (slash != std::string::npos) {
            t.path = url.substr(slash);
            if (t.path[0] != '/') { t.path = "/" + t.path; }
            if (t.path == "/") { t.path.clear(); }
        }
        // user:pass@ is never part of a directory URL; drop it.
        const size_t at = authority.rfind('@');
        if (at != std::string::npos) { authority = authority.substr(at + 1); }
        const size_t colon = authority.rfind(':');
        t.port = defaultPort(t.scheme);
        if (colon != std::string::npos && authority.find(']') == std::string::npos) {
            const std::string portStr = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
            if (!portStr.empty()) {
                char* end = nullptr;
                const long p = std::strtol(portStr.c_str(), &end, 10);
                if (!end || *end || p <= 0 || p > 65535) { return std::nullopt; }
                t.port = (int)p;
            }
        }
        if (authority.empty()) { return std::nullopt; }
        t.host = authority;
        return t;
    }

    bool RemoteEntry::hasFlag(const std::string& flag) const {
        return std::find(flags.begin(), flags.end(), flag) != flags.end();
    }

    std::optional<float> RemoteEntry::occupancy() const {
        if (!users || !usersMax || *usersMax <= 0) { return std::nullopt; }
        return std::clamp((float)*users / (float)*usersMax, 0.0f, 1.0f);
    }

    bool RemoteEntry::covers(uint64_t hz) const {
        for (const auto& [lo, hi] : ranges) {
            if (hz >= lo && hz <= hi) { return true; }
        }
        return false;
    }

    std::string RemoteEntry::detail(const std::string& key) const {
        for (const auto& [k, v] : details) {
            if (k == key) { return v; }
        }
        return {};
    }

    nlohmann::json entryToJson(const RemoteEntry& e) {
        nlohmann::json j;
        j["id"] = e.id;
        j["kind"] = kindKey(e.kind);
        j["provider"] = e.provider;
        j["name"] = e.name;
        j["location"] = e.location;
        if (e.latLon) { j["lat"] = e.latLon->first; j["lon"] = e.latLon->second; }
        j["scheme"] = e.target.scheme;
        j["host"] = e.target.host;
        j["port"] = e.target.port;
        if (!e.target.path.empty()) { j["path"] = e.target.path; }
        if (e.users) { j["users"] = *e.users; }
        if (e.usersMax) { j["usersMax"] = *e.usersMax; }
        if (!e.ranges.empty()) {
            nlohmann::json r = nlohmann::json::array();
            for (const auto& [lo, hi] : e.ranges) { r.push_back({ lo, hi }); }
            j["ranges"] = r;
        }
        if (e.snrDb) { j["snr"] = *e.snrDb; }
        if (e.lastSeenUnix) { j["lastSeen"] = *e.lastSeenUnix; }
        if (!e.version.empty()) { j["version"] = e.version; }
        if (e.tlsRequired) { j["tls"] = true; }
        if (!e.details.empty()) {
            nlohmann::json d = nlohmann::json::array();
            for (const auto& [k, v] : e.details) { d.push_back({ k, v }); }
            j["details"] = d;
        }
        if (!e.flags.empty()) { j["flags"] = e.flags; }
        return j;
    }

    std::optional<RemoteEntry> entryFromJson(const nlohmann::json& j) {
        try {
            if (!j.is_object()) { return std::nullopt; }
            RemoteEntry e;
            const auto kind = kindFromKey(j.value("kind", std::string()));
            if (!kind) { return std::nullopt; }
            e.kind = *kind;
            e.id = j.value("id", std::string());
            e.provider = j.value("provider", std::string());
            e.name = j.value("name", std::string());
            e.location = j.value("location", std::string());
            if (j.contains("lat") && j.contains("lon")) { e.latLon = std::make_pair(j["lat"].get<double>(), j["lon"].get<double>()); }
            e.target.scheme = j.value("scheme", std::string("http"));
            e.target.host = j.value("host", std::string());
            e.target.port = j.value("port", 0);
            e.target.path = j.value("path", std::string());
            if (e.target.host.empty() || e.id.empty()) { return std::nullopt; }
            if (j.contains("users")) { e.users = j["users"].get<int>(); }
            if (j.contains("usersMax")) { e.usersMax = j["usersMax"].get<int>(); }
            if (j.contains("ranges") && j["ranges"].is_array()) {
                for (const auto& r : j["ranges"]) {
                    if (r.is_array() && r.size() == 2) { e.ranges.emplace_back(r[0].get<uint64_t>(), r[1].get<uint64_t>()); }
                }
            }
            if (j.contains("snr")) { e.snrDb = j["snr"].get<float>(); }
            if (j.contains("lastSeen")) { e.lastSeenUnix = j["lastSeen"].get<int64_t>(); }
            e.version = j.value("version", std::string());
            e.tlsRequired = j.value("tls", false);
            if (j.contains("details") && j["details"].is_array()) {
                for (const auto& d : j["details"]) {
                    if (d.is_array() && d.size() == 2) { e.details.emplace_back(d[0].get<std::string>(), d[1].get<std::string>()); }
                }
            }
            if (j.contains("flags") && j["flags"].is_array()) {
                for (const auto& f : j["flags"]) {
                    if (f.is_string()) { e.flags.push_back(f.get<std::string>()); }
                }
            }
            return e;
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::string formatRangeMHz(uint64_t startHz, uint64_t endHz) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g-%g MHz", startHz / 1e6, endHz / 1e6);
        return buf;
    }

    std::string formatRanges(const RemoteEntry& e) {
        std::string out;
        for (const auto& [lo, hi] : e.ranges) {
            if (!out.empty()) { out += ", "; }
            out += formatRangeMHz(lo, hi);
        }
        return out;
    }

    std::optional<std::pair<double, double>> maidenheadToLatLon(const std::string& locIn) {
        const std::string loc = lower(locIn);
        if (loc.size() != 2 && loc.size() != 4 && loc.size() != 6) { return std::nullopt; }
        if (loc[0] < 'a' || loc[0] > 'r' || loc[1] < 'a' || loc[1] > 'r') { return std::nullopt; }
        double lon = (loc[0] - 'a') * 20.0 - 180.0;
        double lat = (loc[1] - 'a') * 10.0 - 90.0;
        double lonSize = 20.0, latSize = 10.0;
        if (loc.size() >= 4) {
            if (!std::isdigit((unsigned char)loc[2]) || !std::isdigit((unsigned char)loc[3])) { return std::nullopt; }
            lon += (loc[2] - '0') * 2.0;
            lat += (loc[3] - '0') * 1.0;
            lonSize = 2.0;
            latSize = 1.0;
        }
        if (loc.size() == 6) {
            if (loc[4] < 'a' || loc[4] > 'x' || loc[5] < 'a' || loc[5] > 'x') { return std::nullopt; }
            lon += (loc[4] - 'a') * (2.0 / 24.0);
            lat += (loc[5] - 'a') * (1.0 / 24.0);
            lonSize = 2.0 / 24.0;
            latSize = 1.0 / 24.0;
        }
        return std::make_pair(lat + latSize / 2.0, lon + lonSize / 2.0);
    }

    std::string latLonToMaidenhead(double latitude, double longitude) {
        if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) { return {}; }
        const double lon = longitude + 180.0;
        const double lat = latitude + 90.0;
        const int lonField = std::min(17, (int)(lon / 20.0));
        const int latField = std::min(17, (int)(lat / 10.0));
        const int lonSquare = std::min(9, (int)((lon - lonField * 20.0) / 2.0));
        const int latSquare = std::min(9, (int)(lat - latField * 10.0));
        const int lonSub = std::min(23, (int)((lon - lonField * 20.0 - lonSquare * 2.0) * 12.0));
        const int latSub = std::min(23, (int)((lat - latField * 10.0 - latSquare * 1.0) * 24.0));
        char buf[7];
        std::snprintf(buf, sizeof buf, "%c%c%d%d%c%c", 'A' + lonField, 'A' + latField, lonSquare, latSquare,
                      'a' + lonSub, 'a' + latSub);
        return buf;
    }

    double distanceKm(std::pair<double, double> a, std::pair<double, double> b) {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kEarthRadiusKm = 6371.0;
        const double la1 = a.first * kPi / 180.0, la2 = b.first * kPi / 180.0;
        const double dLat = la2 - la1;
        const double dLon = (b.second - a.second) * kPi / 180.0;
        const double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                         std::cos(la1) * std::cos(la2) * std::sin(dLon / 2) * std::sin(dLon / 2);
        return 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(h)));
    }

}
