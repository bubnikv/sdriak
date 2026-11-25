// Remote-receiver browser data layer, offline: target parsing, Maidenhead,
// the OpenWebRX+ version gate, each directory parser on a small sample
// (owner e-mail never kept), and the directory service's cache: a fresh
// cache skips the fetch, 304 keeps the entries, a failure keeps the stale
// list.
// `remote_directory_tests --files <kiwisdr_com.js> <status.json> <instances.json> <receiverbook.html> [<rx-888 devices.json> [<sdr-list receivers.json>]]`
// parses saved real documents instead and prints the counts.
#include <gui/remote/directory.h>
#include <gui/remote/providers.h>
#include <gui/remote/recent.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

    void require(bool cond, const std::string& what) {
        if (!cond) { throw std::runtime_error(what); }
    }

    void targets() {
        auto t = remote::parseTarget("https://Sdr.Example.org:9443/path?x=1");
        require(t && t->scheme == "https" && t->host == "Sdr.Example.org" && t->port == 9443 && t->path == "/path?x=1", "https with port and path");
        require(t->hostKey() == "sdr.example.org", "host key lower-case");
        t = remote::parseTarget("http://kiwi.example:8073");
        require(t && t->port == 8073 && t->path.empty() && t->url() == "http://kiwi.example:8073/", "kiwi url");
        t = remote::parseTarget("https://owrx.example/");
        require(t && t->port == 443 && t->url() == "https://owrx.example/", "default https port left out");
        t = remote::parseTarget("10.0.0.5:5555", "tcp");
        require(t && t->scheme == "tcp" && t->port == 5555 && t->hostPort() == "10.0.0.5:5555", "host:port with default scheme");
        require(!remote::parseTarget("http://:80"), "empty host");
        require(!remote::parseTarget("http://h:99999"), "bad port");
    }

    void maidenhead() {
        auto ll = remote::maidenheadToLatLon("JN79");
        require(ll && std::fabs(ll->first - 49.5) < 1e-9 && std::fabs(ll->second - 15.0) < 1e-9, "JN79 centre");
        ll = remote::maidenheadToLatLon("jn79fx");
        require(ll && remote::latLonToMaidenhead(ll->first, ll->second) == "JN79fx", "6-char round trip");
        require(!remote::maidenheadToLatLon("ZZ00"), "out-of-range field");
        require(!remote::maidenheadToLatLon("JN7"), "odd length");
        const double d = remote::distanceKm({ 50.0, 14.0 }, { 51.5, 0.0 });
        require(d > 980 && d < 1050, "Prague-London distance " + std::to_string(d));
    }

    void owrxVersions() {
        require(remote::owrxVersionHasIq("1.2.129"), "1.2.129");
        require(remote::owrxVersionHasIq("1.2.130"), "1.2.130");
        require(remote::owrxVersionHasIq("v1.3.0"), "v1.3.0");
        require(!remote::owrxVersionHasIq("1.2.128"), "1.2.128");
        require(!remote::owrxVersionHasIq("1.1.999"), "1.1.999");
        require(!remote::owrxVersionHasIq(""), "empty");
    }

    void kiwiParser() {
        const std::string body =
            "var kiwisdr_com =\n[\n"
            "{\"name\":\"Low\",\"url\":\"http://low.example:8073\",\"loc\":\"Here\",\"gps\":\"(50.1, 14.4)\","
            "\"snr\":\"10,8\",\"users\":\"1\",\"users_max\":\"4\",\"offline\":\"no\",\"bands\":\"0-30000000\","
            "\"ext_api\":\"1\",\"email\":\"owner@example.org\"},\n"
            "{\"name\":\"Off\",\"url\":\"http://off.example:8073\",\"loc\":\"x\",\"gps\":\"(1, 2)\",\"snr\":\"1,1\","
            "\"users\":\"0\",\"users_max\":\"4\",\"offline\":\"yes\"},\n"
            "{\"name\":\"High\",\"url\":\"https://high.example\",\"loc\":\"There\",\"gps\":\"(-33.9, 151.2)\","
            "\"snr\":\"30,20\",\"users\":\"8\",\"users_max\":\"8\",\"offline\":\"no\",\"bands\":\"0-62000000\"},\n"
            "]\n;";
        const auto entries = remote::parseKiwiSDRDirectory(body);
        require(entries.size() == 2, "offline entry dropped");
        require(entries[0].name == "Low" && entries[1].name == "High", "low SNR first");
        const auto& low = entries[0];
        require(low.kind == remote::RemoteKind::KiwiSDR && low.target.hostPort() == "low.example:8073", "kiwi target");
        require(low.ranges.size() == 1 && low.ranges[0].second == 30000000ull, "kiwi band");
        require(low.hasFlag("ext-api") && !low.isFull(), "kiwi flags");
        require(entries[1].tlsRequired && entries[1].isFull() && entries[1].hasFlag("wide"), "https, full, wide");
        require(remote::entryToJson(low).dump().find("owner@") == std::string::npos, "e-mail kept");
    }

    void rx888Parser() {
        const std::string body = R"json({"devices":[
            {"url":"http://a.proxy.rx-888.com:8073","name":"OK1ABC - Web-888 | Prague, Czechia","snr":"20,15","users":"2",
             "max_users":"8","antenna":"Loop","status":"active","bands":"0-64000000","gps":"(50.08, 14.42)","uptime":"100"},
            {"url":"http://a.proxy.rx-888.com:8073","name":"OK1ABC - Web-888 | Prague, Czechia","snr":"20,15","users":"2",
             "max_users":"8","status":"active","bands":"0-64000000","gps":"(50.08, 14.42)"},
            {"url":"http://b.example:8074","name":"Busy","snr":"5,5","users":"4","max_users":"4","status":"active",
             "bands":"0-30000000","gps":"(10, 20)"},
            {"url":"http://down.example:8073","name":"Down","snr":"0,0","users":"0","max_users":"4","status":"Inaccessible",
             "bands":"0-30000000","gps":"(1, 2)"},
            {"url":"http://:8073","name":"No host","snr":"0,0","users":"0","max_users":"3","status":"active","gps":"(3, 4)"}]})json";
        const auto entries = remote::parseRx888Directory(body);
        require(entries.size() == 2, "inaccessible, hostless and repeated entries dropped");
        require(entries[0].name == "Busy" && entries[0].isFull(), "low SNR first, full");
        const auto& e = entries[1];
        require(e.kind == remote::RemoteKind::KiwiSDR && e.provider == "rx-888.com", "kiwi kind");
        require(e.location == "Prague, Czechia" && e.usersMax == 8 && e.hasFlag("wide"), "rx-888 fields");
        require(remote::entryKey(e) == "kiwisdr/a.proxy.rx-888.com:8073", "key merges with kiwisdr.com");
    }

    void sdrListParser() {
        const std::string body = R"json([
            {"id":1,"name":"Uber","hostname":"uber.example","port":80,"software":"UberSDR","last_seen":"2026-11-25T17:39:48Z"},
            {"id":2,"name":"Phantom","hostname":"Ph.Example","port":8900,"software":"PhantomSDR+","version":"v.6.0.0",
             "users":11,"max_users":100,"range_start_hz":0,"range_end_hz":30000000,"grid_locator":"JN79",
             "lat":50.0,"lon":14.0,"lat_jittered":50.01,"lon_jittered":14.02,"location_city":"Prague",
             "location_country":"Czechia","last_seen":"2026-11-25T17:39:48Z"},
            {"id":3,"name":"Phantom rx 2","hostname":"ph.example","port":8900,"software":"PhantomSDR+","last_seen":"2026-11-25T17:39:48Z"},
            {"id":4,"name":"Stale","hostname":"old.example","port":80,"software":"PhantomSDR+","last_seen":"2026-11-16T10:00:00Z"},
            {"id":5,"name":"Tls","hostname":"tls.example","port":443,"software":"PhantomSDR+","last_seen":"2026-11-25T12:00:00Z"},
            {"id":6,"name":"Nova","hostname":"nova.example","port":80,"software":"NovaSDR","last_seen":"2026-11-25T17:39:48Z"}])json";
        const auto entries = remote::parseSdrListDirectory(body, 1795629600);  // 2026-11-25T18:00:00Z
        require(entries.size() == 2, "PhantomSDR+ only, repeated host and stale entry dropped");
        const auto& e = entries[0];
        require(e.kind == remote::RemoteKind::KiwiSDR && e.target.url() == "http://Ph.Example:8900/", "kiwi target");
        require(e.lastSeenUnix == 1795628388, "last_seen parsed");
        require(e.latLon && std::fabs(e.latLon->first - 50.01) < 1e-9, "jittered position");
        require(e.location == "Prague, Czechia" && e.users == 11 && e.usersMax == 100 && e.ranges.size() == 1, "sdr-list fields");
        require(remote::entryKey(e) == "kiwisdr/ph.example:8900", "key merges with other KiwiSDR lists");
        require(entries[1].tlsRequired && entries[1].target.scheme == "https", "port 443 is https");
    }

    void spyParser() {
        const std::string body = R"({"servers":[
            {"streamingHost":"1.2.3.4","streamingPort":5555,"online":true,"ownerEmail":"me@example.org","ownerName":"Me",
             "deviceType":"AirspyHF+","generalDescription":"no description","antennaLocation":{"lat":59.3,"long":18.0},
             "currentClientCount":1,"maxClients":4,"minimumFrequency":0,"maximumFrequency":31000000,
             "maximumIQSampleRate":768000,"fullControlAllowed":true,"serverVersion":"2.0.1922"},
            {"streamingHost":"5.6.7.8","streamingPort":5555,"online":false},
            {"streamingHost":"","streamingPort":5555,"online":true}]})";
        const auto entries = remote::parseSpyServerDirectory(body, 1000);
        require(entries.size() == 1, "offline and hostless dropped");
        const auto& e = entries[0];
        require(e.target.scheme == "tcp" && e.target.hostPort() == "1.2.3.4:5555", "spy target");
        require(e.name == "AirspyHF+ 1.2.3.4:5555", "name from device when no description");
        require(e.latLon && e.users == 1 && e.usersMax == 4 && e.hasFlag("full-control"), "spy fields");
        const std::string dump = remote::entryToJson(e).dump();
        require(dump.find("me@example.org") == std::string::npos && dump.find("\"Me\"") == std::string::npos, "owner kept");
    }

    void uberParser() {
        const std::string body = R"({"count":2,"instances":[
            {"name":"SDR with Love","callsign":"N0CALL","location":"Somewhere","latitude":47.9,"longitude":12.8,
             "maidenhead":"JN67jx","host":"sdr.example.de","port":443,"tls":true,"max_clients":20,"available_clients":17,
             "public_iq_modes":["iq48","iq96"],"tuning_range":{"min_frequency":10000,"max_frequency":30000000},
             "snr_0_30_mhz":42,"is_online":true,"last_report_age_seconds":30,"version":"0.1.69"},
            {"name":"Down","host":"down.example","port":80,"is_online":false}]})";
        const auto entries = remote::parseUberSDRDirectory(body, 10000);
        require(entries.size() == 1, "offline instance dropped");
        const auto& e = entries[0];
        require(e.target.url() == "https://sdr.example.de/" && e.tlsRequired, "uber target");
        require(e.users == 3 && e.usersMax == 20, "users from available_clients");
        require(e.lastSeenUnix == 9970 && e.hasFlag("wide-iq") && e.ranges.size() == 1, "uber fields");
    }

    void receiverbookParser() {
        const std::string body = R"(<html><script>
            var receivers = [{"label":"Bedford, England ","location":{"coordinates":[-0.45,52.1],"type":"Point"},
              "receivers":[
                {"label":"New <br> OpenWebRX+ \"]\" in label","version":"1.2.130","url":"http://rr.example:8077/","type":"OpenWebRX"},
                {"label":"Old","version":"1.2.100","url":"https://old.example/owrx/","type":"OpenWebRX"},
                {"label":"Kiwi","url":"http://rr.example:8073/","type":"KiwiSDR"},
                {"label":"Web","url":"http://rr.example:8901/","type":"WebSDR"}]}];
            var other = 1;
            </script></html>)";
        const auto entries = remote::parseReceiverbook(body);
        require(entries.size() == 3, "WebSDR skipped");
        require(entries[0].kind == remote::RemoteKind::OpenWebRX && entries[0].name == "New OpenWebRX+ \"]\" in label", "label text");
        require(entries[0].latLon && std::fabs(entries[0].latLon->first - 52.1) < 1e-9, "GeoJSON lon/lat order");
        require(!entries[0].hasFlag("audio-only") && entries[1].hasFlag("audio-only"), "version gate");
        require(entries[1].target.path == "/owrx/" && entries[1].tlsRequired, "owrx path kept");
        require(entries[2].kind == remote::RemoteKind::KiwiSDR && entries[2].target.path.empty(), "kiwi entry");
        require(remote::entryKey(entries[2]) == "kiwisdr/rr.example:8073", "kiwi key merges with kiwisdr.com");
    }

    // A provider whose answers the test scripts.
    struct FakeProvider : remote::DirectoryProvider {
        std::function<remote::FetchResult(const remote::FetchRequest&)> answer;
        int calls = 0;
        std::string lastEtag;
        std::string id() const override { return "fake"; }
        std::string name() const override { return "Fake"; }
        remote::FetchResult fetch(const remote::FetchRequest& request) override {
            calls++;
            lastEtag = request.etag;
            return answer(request);
        }
    };

    remote::RemoteEntry sampleEntry(const std::string& host) {
        remote::RemoteEntry e;
        e.kind = remote::RemoteKind::SpyServer;
        e.provider = "fake";
        e.target = { "tcp", host, 5555, "" };
        e.id = "fake/" + host;
        e.name = host;
        return e;
    }

    void serviceCache() {
        const auto dir = std::filesystem::temp_directory_path() /
            ("remote_directory_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto makeService = [&](FakeProvider*& out) {
            auto p = std::make_unique<FakeProvider>();
            out = p.get();
            std::vector<std::unique_ptr<remote::DirectoryProvider>> v;
            v.push_back(std::move(p));
            return std::make_unique<remote::DirectoryService>(dir.string(), std::move(v));
        };
        {
            FakeProvider* p = nullptr;
            auto svc = makeService(p);
            p->answer = [](const remote::FetchRequest&) {
                remote::FetchResult r;
                r.entries = { sampleEntry("a.example"), sampleEntry("b.example") };
                r.etag = "\"v1\"";
                return r;
            };
            svc->refreshNow(false);
            require(p->calls == 1 && svc->snapshot().size() == 2, "first fetch");
        }
        {
            // Fresh cache: no request.
            FakeProvider* p = nullptr;
            auto svc = makeService(p);
            p->answer = [](const remote::FetchRequest&) -> remote::FetchResult { throw std::runtime_error("must not fetch"); };
            svc->refreshNow(false);
            require(p->calls == 0 && svc->snapshot().size() == 2, "fresh cache served");
            // Forced: 304 keeps the entries, the validator was sent.
            p->answer = [](const remote::FetchRequest& rq) {
                remote::FetchResult r;
                r.notModified = true;
                r.etag = rq.etag;
                return r;
            };
            svc->refreshNow(true);
            require(p->calls == 1 && p->lastEtag == "\"v1\"" && svc->snapshot().size() == 2, "304 keeps entries");
            // A failure keeps the stale list and reports the error.
            p->answer = [](const remote::FetchRequest&) -> remote::FetchResult { throw std::runtime_error("offline"); };
            svc->refreshNow(true);
            const auto st = svc->status();
            require(svc->snapshot().size() == 2 && st.size() == 1 && st[0].error == "offline", "failure keeps stale list");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string readFile(const char* path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) { throw std::runtime_error(std::string("cannot read ") + path); }
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

int main(int argc, char** argv) {
    if (argc >= 6 && argc <= 8 && std::string(argv[1]) == "--files") {
        try {
            const auto kiwi = remote::parseKiwiSDRDirectory(readFile(argv[2]));
            const auto spy = remote::parseSpyServerDirectory(readFile(argv[3]), remote::nowUnix());
            const auto uber = remote::parseUberSDRDirectory(readFile(argv[4]), remote::nowUnix());
            const auto rb = remote::parseReceiverbook(readFile(argv[5]));
            int owrx = 0, owrxIq = 0, rbKiwi = 0;
            for (const auto& e : rb) {
                if (e.kind == remote::RemoteKind::OpenWebRX) {
                    owrx++;
                    owrxIq += e.hasFlag("audio-only") ? 0 : 1;
                }
                else { rbKiwi++; }
            }
            std::printf("kiwisdr %zu, spyserver %zu, ubersdr %zu, receiverbook %zu (openwebrx %d, %d with iq; kiwisdr %d)\n",
                        kiwi.size(), spy.size(), uber.size(), rb.size(), owrx, owrxIq, rbKiwi);
            if (argc >= 7) { std::printf("rx-888 %zu\n", remote::parseRx888Directory(readFile(argv[6])).size()); }
            if (argc >= 8) { std::printf("sdr-list %zu\n", remote::parseSdrListDirectory(readFile(argv[7]), remote::nowUnix()).size()); }
            return 0;
        }
        catch (const std::exception& e) {
            std::printf("FAIL: %s\n", e.what());
            return 1;
        }
    }

    const std::pair<const char*, void (*)()> tests[] = {
        { "targets", targets },
        { "maidenhead", maidenhead },
        { "openwebrx versions", owrxVersions },
        { "kiwisdr parser", kiwiParser },
        { "rx-888 parser", rx888Parser },
        { "sdr-list parser", sdrListParser },
        { "spyserver parser", spyParser },
        { "ubersdr parser", uberParser },
        { "receiverbook parser", receiverbookParser },
        { "service cache", serviceCache },
    };
    int failed = 0;
    for (const auto& [name, fn] : tests) {
        try {
            fn();
            std::printf("ok   %s\n", name);
        }
        catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", name, e.what());
            failed++;
        }
    }
    return failed ? 1 : 0;
}
