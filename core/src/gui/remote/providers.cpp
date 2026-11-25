/*
 * The KiwiSDR directory parser is based on code from project SDRPlusPlusBrown
 * (https://github.com/sannysanoff/SDRPlusPlusBrown) originally licensed under
 * the GNU General Public License, version 3 (GPLv3).
 *
 * Modified and extended by qrp73, 2025 (https://github.com/qrp73/SDRPP).
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "providers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>
#include <stdexcept>

#include <utils/flog.h>

namespace remote {

    namespace {

        using nlohmann::json;

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        // A string field, whatever JSON type the directory used for it.
        std::string str(const json& j, const char* key) {
            if (!j.contains(key)) { return {}; }
            const auto& v = j[key];
            if (v.is_string()) { return v.get<std::string>(); }
            if (v.is_number_integer()) { return std::to_string(v.get<long long>()); }
            if (v.is_number()) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", v.get<double>());
                return buf;
            }
            if (v.is_boolean()) { return v.get<bool>() ? "true" : "false"; }
            return {};
        }

        std::optional<double> num(const json& j, const char* key) {
            if (!j.contains(key)) { return std::nullopt; }
            const auto& v = j[key];
            if (v.is_number()) { return v.get<double>(); }
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                std::istringstream ss(s);
                ss.imbue(std::locale::classic());
                double d;
                if (ss >> d) { return d; }
            }
            return std::nullopt;
        }

        bool flag(const json& j, const char* key) {
            return j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
        }

        std::string withThousands(long long v) {
            std::string s = std::to_string(v);
            for (int i = (int)s.size() - 3; i > 0; i -= 3) { s.insert((size_t)i, " "); }
            return s;
        }

        std::string formatRate(double sps) {
            char buf[32];
            if (sps >= 1e6) { std::snprintf(buf, sizeof buf, "%g MS/s", sps / 1e6); }
            else { std::snprintf(buf, sizeof buf, "%g kS/s", sps / 1e3); }
            return buf;
        }

        bool validLatLon(double lat, double lon) {
            if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) { return false; }
            // (0, 0) is where an unset position lands.
            return !(lat == 0.0 && lon == 0.0);
        }

        // Text of an HTML label: tags dropped, entities for the common
        // characters decoded, whitespace collapsed.
        std::string htmlText(const std::string& in) {
            std::string out;
            bool inTag = false;
            for (size_t i = 0; i < in.size(); i++) {
                const char c = in[i];
                if (inTag) {
                    if (c == '>') { inTag = false; out += ' '; }
                    continue;
                }
                if (c == '<') { inTag = true; continue; }
                if (c == '&') {
                    static const std::pair<const char*, char> kEntities[] = {
                        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&nbsp;", ' ' }
                    };
                    bool matched = false;
                    for (const auto& [ent, ch] : kEntities) {
                        const size_t n = std::strlen(ent);
                        if (in.compare(i, n, ent) == 0) {
                            out += ch;
                            i += n - 1;
                            matched = true;
                            break;
                        }
                    }
                    if (matched) { continue; }
                }
                out += c;
            }
            std::string collapsed;
            bool space = true;
            for (char c : out) {
                if (std::isspace((unsigned char)c)) {
                    if (!space) { collapsed += ' '; }
                    space = true;
                }
                else {
                    collapsed += c;
                    space = false;
                }
            }
            while (!collapsed.empty() && collapsed.back() == ' ') { collapsed.pop_back(); }
            return collapsed;
        }

        std::string shorten(const std::string& s, size_t max) {
            if (s.size() <= max) { return s; }
            size_t cut = max;
            // Do not split a UTF-8 sequence.
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) { cut--; }
            return s.substr(0, cut) + "...";
        }

        // =====================================================================
        // KiwiSDR

        constexpr const char* KIWI_URL = "http://rx.linkfanel.net/kiwisdr_com.js";

        // Strips the JS wrapper from kiwisdr_com.js and the trailing comma, so
        // the rest parses as JSON.
        std::string cleanKiwiJs(const std::string& response) {
            const char* BEGIN = "var kiwisdr_com =";
            const char* END = "},\n]\n;";
            auto beginIx = response.find(BEGIN);
            if (beginIx == std::string::npos) { throw std::runtime_error("Invalid response from server"); }
            auto endIx = response.rfind(END);
            const auto contentBeginIx = beginIx + std::strlen(BEGIN);
            if (endIx == std::string::npos || endIx < contentBeginIx) { throw std::runtime_error("Invalid response from server"); }
            std::string out = response.substr(contentBeginIx, endIx - contentBeginIx);
            out += "}]";
            return out;
        }

        std::optional<RemoteEntry> parseKiwiEntry(const json& entry) {
            if (!(entry.contains("gps") && entry.contains("name") && entry.contains("url") && entry.contains("loc") &&
                  entry.contains("snr") && entry.contains("users") && entry.contains("users_max") && entry.contains("offline"))) {
                return std::nullopt;
            }
            if (str(entry, "offline") != "no") { return std::nullopt; }

            const std::string gps = str(entry, "gps");
            double lat = 0.0, lon = 0.0;
            {
                std::stringstream ss(gps);
                ss.imbue(std::locale::classic());
                char discard;
                ss >> discard >> lat >> discard >> lon >> discard;
                if (!ss) {
                    flog::warn("Parsing geo coordinates failed: \"{}\"", gps);
                    return std::nullopt;
                }
            }
            const std::string url = str(entry, "url");
            auto target = parseTarget(url);
            if (!target) { return std::nullopt; }
            if (target->scheme != "http" && target->scheme != "https") { return std::nullopt; }

            RemoteEntry e;
            e.kind = RemoteKind::KiwiSDR;
            e.provider = "kiwisdr.com";
            e.target = *target;
            e.target.path.clear();
            e.id = "kiwisdr.com/" + lower(e.target.hostPort());
            e.name = str(entry, "name");
            e.location = str(entry, "loc");
            e.latLon = std::make_pair(lat, lon);
            e.tlsRequired = e.target.scheme == "https";

            std::optional<std::pair<uint64_t, uint64_t>> band;
            if (entry.contains("bands")) {
                // A malformed bands string leaves the entry without a range
                // rather than dropping it.
                const std::string bandsStr = str(entry, "bands");
                std::stringstream bands(bandsStr);
                bands.imbue(std::locale::classic());
                uint64_t lo = 0, hi = 0;
                char separator = '\0';
                bands >> lo >> separator >> hi;
                bands >> std::ws;
                if (bands.eof() && separator == '-' && lo <= hi) { band = std::make_pair(lo, hi); }
                else { flog::warn("Parsing bands failed: \"{}\"", bandsStr); }
            }
            if (band) { e.ranges.push_back(*band); }

            float maxSnr = 0.0f, secondSnr = 0.0f;
            std::sscanf(str(entry, "snr").c_str(), "%f,%f", &maxSnr, &secondSnr);
            if (maxSnr > 0) { e.snrDb = maxSnr; }
            e.users = std::atoi(str(entry, "users").c_str());
            e.usersMax = std::atoi(str(entry, "users_max").c_str());
            const int extApi = std::atoi(str(entry, "ext_api").c_str());
            e.version = str(entry, "sw_version");

            if (band) { e.details.emplace_back("Band", formatRangeMHz(band->first, band->second)); }
            if (!str(entry, "antenna").empty()) { e.details.emplace_back("ANT", str(entry, "antenna")); }
            if (!str(entry, "sdr_hw").empty()) { e.details.emplace_back("HW", str(entry, "sdr_hw")); }
            if (!e.version.empty()) { e.details.emplace_back("VER", e.version); }
            e.details.emplace_back("QTH", latLonToMaidenhead(lat, lon));
            if (maxSnr > 0) { e.details.emplace_back("SNR", std::to_string((int)maxSnr)); }
            if (*e.usersMax > 0) { e.details.emplace_back("USR", std::to_string(*e.users) + "/" + std::to_string(*e.usersMax)); }
            e.details.emplace_back("EXT API", std::to_string(extApi));
            e.details.emplace_back("URL", url);

            e.flags.push_back("waterfall");
            if (extApi > 0) { e.flags.push_back("ext-api"); }
            if (band && band->second > 32000000ull) { e.flags.push_back("wide"); }
            return e;
        }

        struct KiwiSDRProvider : DirectoryProvider {
            std::string id() const override { return "kiwisdr.com"; }
            std::string name() const override { return "KiwiSDR"; }
            FetchResult fetch(const FetchRequest& request) override {
                auto doc = fetchDocument(KIWI_URL, request, 4 * 1024 * 1024);
                FetchResult r;
                r.notModified = doc.notModified;
                r.etag = doc.etag;
                r.lastModified = doc.lastModified;
                if (!doc.notModified) { r.entries = parseKiwiSDRDirectory(doc.body); }
                return r;
            }
        };

        // =====================================================================
        // SpyServer

        constexpr const char* SPY_URL = "https://airspy.com/directory/status.json";

        struct SpyServerProvider : DirectoryProvider {
            std::string id() const override { return "airspy.com"; }
            std::string name() const override { return "SpyServer"; }
            FetchResult fetch(const FetchRequest& request) override {
                auto doc = fetchDocument(SPY_URL, request, 4 * 1024 * 1024);
                FetchResult r;
                r.notModified = doc.notModified;
                r.etag = doc.etag;
                r.lastModified = doc.lastModified;
                if (!doc.notModified) { r.entries = parseSpyServerDirectory(doc.body, nowUnix()); }
                return r;
            }
        };

        // =====================================================================
        // UberSDR

        constexpr const char* UBER_URL = "https://instances.ubersdr.org/api/instances";

        struct UberSDRProvider : DirectoryProvider {
            std::string id() const override { return "ubersdr.org"; }
            std::string name() const override { return "UberSDR"; }
            FetchResult fetch(const FetchRequest& request) override {
                auto doc = fetchDocument(UBER_URL, request, 8 * 1024 * 1024);
                FetchResult r;
                r.notModified = doc.notModified;
                r.etag = doc.etag;
                r.lastModified = doc.lastModified;
                if (!doc.notModified) { r.entries = parseUberSDRDirectory(doc.body, nowUnix()); }
                return r;
            }
        };

        // =====================================================================
        // Receiverbook

        constexpr const char* RECEIVERBOOK_URL = "https://www.receiverbook.de/map";

        // The JSON array that follows `marker`, matched bracket for bracket
        // with strings skipped.
        std::string extractJsArray(const std::string& text, const std::string& marker) {
            size_t pos = text.find(marker);
            if (pos == std::string::npos) { throw std::runtime_error("No receiver list in the page"); }
            pos = text.find('[', pos + marker.size());
            if (pos == std::string::npos) { throw std::runtime_error("No receiver list in the page"); }
            int depth = 0;
            bool inString = false;
            for (size_t i = pos; i < text.size(); i++) {
                const char c = text[i];
                if (inString) {
                    if (c == '\\') { i++; }
                    else if (c == '"') { inString = false; }
                    continue;
                }
                if (c == '"') { inString = true; }
                else if (c == '[' || c == '{') { depth++; }
                else if (c == ']' || c == '}') {
                    if (--depth == 0) { return text.substr(pos, i - pos + 1); }
                }
            }
            throw std::runtime_error("Receiver list is truncated");
        }

        struct ReceiverbookProvider : DirectoryProvider {
            std::string id() const override { return "receiverbook.de"; }
            std::string name() const override { return "Receiverbook"; }
            FetchResult fetch(const FetchRequest& request) override {
                auto doc = fetchDocument(RECEIVERBOOK_URL, request, 8 * 1024 * 1024);
                FetchResult r;
                r.notModified = doc.notModified;
                r.etag = doc.etag;
                r.lastModified = doc.lastModified;
                if (!doc.notModified) { r.entries = parseReceiverbook(doc.body); }
                return r;
            }
        };

    }

    std::unique_ptr<DirectoryProvider> makeKiwiSDRProvider() { return std::make_unique<KiwiSDRProvider>(); }
    std::unique_ptr<DirectoryProvider> makeSpyServerProvider() { return std::make_unique<SpyServerProvider>(); }
    std::unique_ptr<DirectoryProvider> makeUberSDRProvider() { return std::make_unique<UberSDRProvider>(); }
    std::unique_ptr<DirectoryProvider> makeReceiverbookProvider() { return std::make_unique<ReceiverbookProvider>(); }

    std::vector<RemoteEntry> parseKiwiSDRDirectory(const std::string& body) {
        const json doc = json::parse(cleanKiwiJs(body));
        std::vector<RemoteEntry> out;
        for (const auto& entry : doc) {
            try {
                if (auto e = parseKiwiEntry(entry)) { out.push_back(std::move(*e)); }
            }
            catch (const std::exception& ex) {
                flog::warn("Skipping a KiwiSDR directory entry: {}", ex.what());
            }
        }
        // Low SNR first, so the best stations paint on top.
        std::stable_sort(out.begin(), out.end(), [](const RemoteEntry& a, const RemoteEntry& b) {
            return a.snrDb.value_or(0.0f) < b.snrDb.value_or(0.0f);
        });
        return out;
    }

    std::vector<RemoteEntry> parseSpyServerDirectory(const std::string& body, int64_t fetchedUnix) {
        (void)fetchedUnix;
        const json doc = json::parse(body);
        const json* servers = &doc;
        if (doc.is_object()) {
            if (!doc.contains("servers")) { throw std::runtime_error("No server list"); }
            servers = &doc["servers"];
        }
        if (!servers->is_array()) { throw std::runtime_error("No server list"); }
        std::vector<RemoteEntry> out;
        for (const auto& s : *servers) {
            try {
                if (!s.is_object()) { continue; }
                if (s.contains("online") && s["online"].is_boolean() && !s["online"].get<bool>()) { continue; }
                // ownerEmail and ownerName are never read.
                const std::string host = str(s, "streamingHost");
                const auto port = num(s, "streamingPort");
                if (host.empty() || !port || *port <= 0 || *port > 65535) { continue; }

                RemoteEntry e;
                e.kind = RemoteKind::SpyServer;
                e.provider = "airspy.com";
                e.target.scheme = "tcp";
                e.target.host = host;
                e.target.port = (int)*port;
                e.id = "airspy.com/" + lower(e.target.hostPort());
                const std::string device = str(s, "deviceType");
                std::string desc = htmlText(str(s, "generalDescription"));
                if (lower(desc) == "no description") { desc.clear(); }
                e.name = desc.empty() ? (device.empty() ? e.target.hostPort() : device + " " + e.target.hostPort()) : shorten(desc, 80);
                if (s.contains("antennaLocation") && s["antennaLocation"].is_object()) {
                    const auto lat = num(s["antennaLocation"], "lat");
                    const auto lon = num(s["antennaLocation"], "long");
                    if (lat && lon && validLatLon(*lat, *lon)) {
                        e.latLon = std::make_pair(*lat, *lon);
                        e.location = latLonToMaidenhead(*lat, *lon);
                    }
                }
                const auto users = num(s, "currentClientCount");
                const auto usersMax = num(s, "maxClients");
                if (users) { e.users = (int)*users; }
                if (usersMax) { e.usersMax = (int)*usersMax; }
                const auto lo = num(s, "minimumFrequency");
                const auto hi = num(s, "maximumFrequency");
                if (lo && hi && *hi > *lo && *lo >= 0) { e.ranges.emplace_back((uint64_t)*lo, (uint64_t)*hi); }
                e.version = str(s, "serverVersion");

                if (!device.empty()) { e.details.emplace_back("Device", device); }
                if (desc.size() > 80) { e.details.emplace_back("Description", desc); }
                if (!str(s, "antennaType").empty()) { e.details.emplace_back("Antenna", htmlText(str(s, "antennaType"))); }
                if (!e.ranges.empty()) { e.details.emplace_back("Range", formatRanges(e)); }
                if (const auto rate = num(s, "maximumIQSampleRate")) { e.details.emplace_back("Max IQ rate", formatRate(*rate)); }
                if (const auto bw = num(s, "maximumStreamedBandwidth")) { e.details.emplace_back("Max streamed", withThousands((long long)*bw) + " Hz"); }
                if (users && usersMax) { e.details.emplace_back("Users", std::to_string((int)*users) + "/" + std::to_string((int)*usersMax)); }
                if (const auto limit = num(s, "maxSessionDuration"); limit && *limit > 0) {
                    e.details.emplace_back("Session limit", std::to_string((int)*limit) + " min");
                }
                const bool control = flag(s, "fullControlAllowed");
                e.details.emplace_back("Owner allows control", control ? "yes" : "no");
                if (!e.location.empty()) { e.details.emplace_back("QTH", e.location); }
                if (!e.version.empty()) { e.details.emplace_back("Version", e.version); }
                e.details.emplace_back("Address", e.target.hostPort());

                e.flags.push_back("waterfall");
                if (control) { e.flags.push_back("full-control"); }
                out.push_back(std::move(e));
            }
            catch (const std::exception& ex) {
                flog::warn("Skipping a SpyServer directory entry: {}", ex.what());
            }
        }
        return out;
    }

    std::vector<RemoteEntry> parseUberSDRDirectory(const std::string& body, int64_t fetchedUnix) {
        const json doc = json::parse(body);
        if (!doc.is_object() || !doc.contains("instances") || !doc["instances"].is_array()) {
            throw std::runtime_error("No instance list");
        }
        std::vector<RemoteEntry> out;
        for (const auto& s : doc["instances"]) {
            try {
                if (!s.is_object()) { continue; }
                if (s.contains("is_online") && s["is_online"].is_boolean() && !s["is_online"].get<bool>()) { continue; }

                RemoteEntry e;
                e.kind = RemoteKind::UberSDR;
                e.provider = "ubersdr.org";
                const std::string host = str(s, "host");
                const auto port = num(s, "port");
                if (!host.empty()) {
                    e.target.host = host;
                    e.target.scheme = flag(s, "tls") ? "https" : "http";
                    e.target.port = port ? (int)*port : (e.target.scheme == "https" ? 443 : 80);
                }
                else if (auto t = parseTarget(str(s, "public_url"))) {
                    e.target = *t;
                    e.target.path.clear();
                }
                if (e.target.host.empty() || e.target.port <= 0 || e.target.port > 65535) { continue; }
                e.tlsRequired = e.target.scheme == "https";
                e.id = "ubersdr.org/" + lower(e.target.hostPort());

                const std::string callsign = str(s, "callsign");
                e.name = str(s, "name");
                if (e.name.empty()) { e.name = callsign.empty() ? e.target.host : callsign; }
                e.location = str(s, "location");
                const auto lat = num(s, "latitude");
                const auto lon = num(s, "longitude");
                if (lat && lon && validLatLon(*lat, *lon)) { e.latLon = std::make_pair(*lat, *lon); }

                const auto maxClients = num(s, "max_clients");
                const auto available = num(s, "available_clients");
                if (maxClients) {
                    e.usersMax = (int)*maxClients;
                    if (available) { e.users = std::max(0, (int)*maxClients - (int)*available); }
                }
                if (s.contains("tuning_range") && s["tuning_range"].is_object()) {
                    const auto lo = num(s["tuning_range"], "min_frequency");
                    const auto hi = num(s["tuning_range"], "max_frequency");
                    if (lo && hi && *hi > *lo && *lo >= 0) { e.ranges.emplace_back((uint64_t)*lo, (uint64_t)*hi); }
                }
                if (const auto snr = num(s, "snr_0_30_mhz")) { e.snrDb = (float)*snr; }
                if (const auto age = num(s, "last_report_age_seconds"); age && *age >= 0) { e.lastSeenUnix = fetchedUnix - (int64_t)*age; }
                e.version = str(s, "version");

                std::string modes;
                bool wide = false;
                if (s.contains("public_iq_modes") && s["public_iq_modes"].is_array()) {
                    for (const auto& m : s["public_iq_modes"]) {
                        if (!m.is_string()) { continue; }
                        if (!modes.empty()) { modes += ", "; }
                        modes += m.get<std::string>();
                        if (m.get<std::string>() != "iq48") { wide = true; }
                    }
                }

                if (!callsign.empty()) { e.details.emplace_back("Callsign", callsign); }
                if (!str(s, "maidenhead").empty()) { e.details.emplace_back("QTH", str(s, "maidenhead")); }
                if (!e.ranges.empty()) { e.details.emplace_back("Range", formatRanges(e)); }
                if (!modes.empty()) { e.details.emplace_back("Public IQ", modes); }
                if (e.users && e.usersMax) { e.details.emplace_back("Users", std::to_string(*e.users) + "/" + std::to_string(*e.usersMax)); }
                if (e.snrDb) { e.details.emplace_back("SNR 0-30 MHz", std::to_string((int)*e.snrDb) + " dB"); }
                if (const auto limit = num(s, "max_session_time"); limit && *limit > 0) {
                    e.details.emplace_back("Session limit", std::to_string((int)(*limit / 60)) + " min");
                }
                if (!str(s, "load_status").empty()) { e.details.emplace_back("Load", str(s, "load_status")); }
                std::string features;
                if (flag(s, "cw_skimmer")) { features += "CW skimmer"; }
                if (flag(s, "digital_decodes")) { features += features.empty() ? "digital decodes" : ", digital decodes"; }
                if (flag(s, "tdoa_enabled")) { features += features.empty() ? "TDOA" : ", TDOA"; }
                if (!features.empty()) { e.details.emplace_back("Features", features); }
                if (!e.version.empty()) { e.details.emplace_back("Version", e.version); }
                e.details.emplace_back("URL", e.target.url());

                e.flags.push_back("waterfall");
                if (wide) { e.flags.push_back("wide-iq"); }
                out.push_back(std::move(e));
            }
            catch (const std::exception& ex) {
                flog::warn("Skipping an UberSDR directory entry: {}", ex.what());
            }
        }
        return out;
    }

    bool owrxVersionHasIq(const std::string& version) {
        int parts[3] = { 0, 0, 0 };
        int n = 0;
        const char* p = version.c_str();
        while (*p && n < 3) {
            if (std::isdigit((unsigned char)*p)) {
                parts[n++] = (int)std::strtol(p, (char**)&p, 10);
            }
            else if (n > 0 && *p != '.') {
                break;
            }
            else {
                p++;
            }
        }
        if (n == 0) { return false; }
        if (parts[0] != 1) { return parts[0] > 1; }
        if (parts[1] != 2) { return parts[1] > 2; }
        return parts[2] >= 129;
    }

    std::vector<RemoteEntry> parseReceiverbook(const std::string& body) {
        const json sites = json::parse(extractJsArray(body, "var receivers"));
        if (!sites.is_array()) { throw std::runtime_error("No receiver list"); }
        std::vector<RemoteEntry> out;
        for (const auto& site : sites) {
            try {
                if (!site.is_object() || !site.contains("receivers") || !site["receivers"].is_array()) { continue; }
                std::optional<std::pair<double, double>> latLon;
                if (site.contains("location") && site["location"].is_object()) {
                    const auto& coords = site["location"].value("coordinates", json());
                    if (coords.is_array() && coords.size() >= 2 && coords[0].is_number() && coords[1].is_number()) {
                        const double lon = coords[0].get<double>();
                        const double lat = coords[1].get<double>();
                        if (validLatLon(lat, lon)) { latLon = std::make_pair(lat, lon); }
                    }
                }
                const std::string siteLabel = htmlText(str(site, "label"));
                for (const auto& rx : site["receivers"]) {
                    if (!rx.is_object()) { continue; }
                    const std::string type = str(rx, "type");
                    RemoteKind kind;
                    if (type == "OpenWebRX") { kind = RemoteKind::OpenWebRX; }
                    else if (type == "KiwiSDR") { kind = RemoteKind::KiwiSDR; }
                    else { continue; }
                    const std::string url = str(rx, "url");
                    auto target = parseTarget(url);
                    if (!target || (target->scheme != "http" && target->scheme != "https")) { continue; }

                    RemoteEntry e;
                    e.kind = kind;
                    e.provider = "receiverbook.de";
                    e.target = *target;
                    if (kind == RemoteKind::KiwiSDR) { e.target.path.clear(); }
                    e.tlsRequired = e.target.scheme == "https";
                    e.id = std::string("receiverbook.de/") + kindKey(kind) + "/" + lower(e.target.hostPort()) + lower(e.target.path);
                    const std::string label = htmlText(str(rx, "label"));
                    e.name = shorten(label.empty() ? e.target.host : label, 80);
                    e.location = siteLabel;
                    e.latLon = latLon;
                    e.version = str(rx, "version");

                    if (label.size() > 80) { e.details.emplace_back("Description", label); }
                    if (!e.version.empty()) { e.details.emplace_back("Version", e.version); }
                    if (latLon) { e.details.emplace_back("QTH", latLonToMaidenhead(latLon->first, latLon->second)); }
                    e.details.emplace_back("URL", url);
                    e.flags.push_back("waterfall");
                    if (kind == RemoteKind::OpenWebRX && !owrxVersionHasIq(e.version)) {
                        e.flags.push_back("audio-only");
                        e.details.emplace_back("IQ", e.version.empty() ? "unknown version; needs 1.2.129 or later"
                                                                       : "no iq mode; needs 1.2.129 or later");
                    }
                    out.push_back(std::move(e));
                }
            }
            catch (const std::exception& ex) {
                flog::warn("Skipping a Receiverbook site: {}", ex.what());
            }
        }
        return out;
    }

}
