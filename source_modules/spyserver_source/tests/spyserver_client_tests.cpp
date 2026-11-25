// SpyServerClientClass against a loopback fake server: CLIENT_SYNC and
// CanControl, the UInt8 FFT dB mapping, PING/PONG stream positions and the
// PONG timeout, the IQ meter and the published-sample count.
#include <spyserver_client.h>
#include <utils/networking.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    int failures = 0;
    void require(bool ok, const std::string& what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            failures++;
        }
    }

    // Ports below the ephemeral ranges, apart from the core tests' blocks.
    net::Listener listenOnTestPort(uint16_t& port) {
        for (port = 24560; port < 24580; port++) {
            try { return net::listen("127.0.0.1", port); }
            catch (const std::exception&) {}
        }
        throw std::runtime_error("could not reserve a loopback test port");
    }

    void put32(std::vector<uint8_t>& m, uint32_t v) {
        for (int i = 0; i < 4; i++) { m.push_back((uint8_t)(v >> (8 * i))); }
    }

    std::vector<uint8_t> message(uint32_t type, const std::vector<uint8_t>& body, uint32_t flags = 0) {
        std::vector<uint8_t> m;
        put32(m, SPYSERVER_PROTOCOL_VERSION);
        put32(m, type | (flags << 16));
        put32(m, 0);
        put32(m, 0);
        put32(m, (uint32_t)body.size());
        m.insert(m.end(), body.begin(), body.end());
        return m;
    }

    std::vector<uint8_t> words(std::initializer_list<uint32_t> v) {
        std::vector<uint8_t> b;
        for (uint32_t x : v) { put32(b, x); }
        return b;
    }

    // count IQ samples, Int16; the first I value is `first`, the rest small.
    std::vector<uint8_t> iq16(int count, int16_t first) {
        std::vector<uint8_t> b;
        for (int i = 0; i < count; i++) {
            const int16_t iv = (i == 0) ? first : (int16_t)100;
            const int16_t qv = (int16_t)-100;
            b.push_back((uint8_t)(iv & 0xff));
            b.push_back((uint8_t)((uint16_t)iv >> 8));
            b.push_back((uint8_t)(qv & 0xff));
            b.push_back((uint8_t)((uint16_t)qv >> 8));
        }
        return b;
    }

    bool readExact(net::Conn& c, uint8_t* buf, int n) {
        int got = 0;
        while (got < n) {
            const int r = c->read(n - got, buf + got);
            if (r <= 0) { return false; }
            got += r;
        }
        return true;
    }

    struct FakeServer {
        net::Listener listener;
        uint16_t port = 0;
        std::thread thread;
        std::mutex mtx;
        std::vector<std::pair<uint32_t, uint32_t>> settings;
        std::atomic<int> pings{0};

        FakeServer() {
            listener = listenOnTestPort(port);
            thread = std::thread([this] { run(); });
        }
        ~FakeServer() {
            listener->close();
            thread.join();
        }

        void send(net::Conn& c, const std::vector<uint8_t>& m) { c->write((int)m.size(), (uint8_t*)m.data()); }

        void run() {
            net::Conn c;
            try { c = listener->accept(); }
            catch (...) { return; }
            if (!c) { return; }
            uint8_t hdr[8];
            // HELLO
            if (!readExact(c, hdr, 8)) { return; }
            uint32_t size;
            std::memcpy(&size, hdr + 4, 4);
            std::vector<uint8_t> body(size);
            if (size && !readExact(c, body.data(), (int)size)) { return; }
            send(c, message(SPYSERVER_MSG_TYPE_DEVICE_INFO,
                            words({ SPYSERVER_DEVICE_AIRSPY_HF, 0x1234, 912000, 780000, 8, 0, 8, 500, 31000000, 16, 0, 0 })));
            send(c, message(SPYSERVER_MSG_TYPE_CLIENT_SYNC,
                            words({ 1, 5, 580000, 580000, 580000, 49250, 30951250, 390500, 30610000 })));
            for (;;) {
                if (!readExact(c, hdr, 8)) { break; }
                uint32_t cmd;
                std::memcpy(&cmd, hdr, 4);
                std::memcpy(&size, hdr + 4, 4);
                body.assign(size, 0);
                if (size && !readExact(c, body.data(), (int)size)) { break; }
                if (cmd == SPYSERVER_CMD_SET_SETTING && size == 8) {
                    uint32_t setting, value;
                    std::memcpy(&setting, body.data(), 4);
                    std::memcpy(&value, body.data() + 4, 4);
                    std::lock_guard lck(mtx);
                    settings.emplace_back(setting, value);
                }
                else if (cmd == SPYSERVER_CMD_PING) {
                    const int n = ++pings;
                    if (n == 1) {
                        // 100 samples (one railed), PONG, 50 samples, a
                        // UInt8 FFT frame, then a sync without control.
                        send(c, message(SPYSERVER_MSG_TYPE_INT16_IQ, iq16(100, 32767)));
                        send(c, message(SPYSERVER_MSG_TYPE_PONG, {}));
                        send(c, message(SPYSERVER_MSG_TYPE_INT16_IQ, iq16(50, 100)));
                        send(c, message(SPYSERVER_MSG_TYPE_UINT8_FFT, { 0, 255, 51, 255 }));
                        send(c, message(SPYSERVER_MSG_TYPE_CLIENT_SYNC,
                                        words({ 0, 5, 580000, 580000, 580000, 502500, 697500, 580000, 580000 })));
                    }
                    else {
                        // Unanswered: IQ keeps coming past the timeout.
                        for (int k = 0; k < 13; k++) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            send(c, message(SPYSERVER_MSG_TYPE_INT16_IQ, iq16(10, 100)));
                        }
                    }
                }
            }
            c->close();
        }
    };
}

int main() {
    FakeServer server;
    dsp::stream<dsp::complex_t> out;
    std::atomic<uint64_t> drained{0};
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop) {
            const int n = out.read();
            if (n < 0) { break; }
            drained += (uint64_t)n;
            out.flush();
        }
    });

    auto client = spyserver::connect("127.0.0.1", server.port, &out, 3000);
    require(client != nullptr, "connect");
    if (!client) {
        stop = true;
        out.stopReader();
        reader.join();
        return 1;
    }
    require(client->waitForDevInfo(3000), "device info");
    require(client->devInfo.MaximumBandwidth == 780000, "device info fields");

    SpyServerClientSync sync{};
    const auto t0 = Clock::now();
    while (!client->clientSync(sync) && Clock::now() - t0 < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(client->clientSync(sync) && sync.CanControl == 1 && sync.MaximumIQCenterFrequency == 30951250, "first CLIENT_SYNC");
    require(client->canControl(), "canControl after the first sync");
    require(client->syncCount() == 1, "sync count");

    std::mutex mtx;
    std::vector<float> fft;
    uint64_t fftIndex = 0;
    client->setFftScale(10, 100);
    client->onFft = [&](const spyserver::SpyServerClientClass::FftFrame& f) {
        std::lock_guard lck(mtx);
        fft.assign(f.bins, f.bins + f.count);
        fftIndex = f.iqIndex;
    };

    std::atomic<int> pongs{0};
    std::atomic<bool> firstAnswered{false};
    std::atomic<uint64_t> firstIndex{0};
    std::atomic<bool> secondDone{false}, secondAnswered{true};
    client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, 582000);
    client->ping([&](bool answered, uint64_t index) {
        firstAnswered = answered;
        firstIndex = index;
        pongs++;
    });
    auto waitFor = [](auto pred, int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (!pred() && Clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return pred();
    };
    require(waitFor([&] { return pongs.load() == 1; }, 3000), "PONG callback");
    require(firstAnswered && firstIndex == 100, "PONG position " + std::to_string(firstIndex.load()));
    require(waitFor([&] { return !client->canControl(); }, 3000), "CanControl from a later sync");
    require(waitFor([&] { std::lock_guard lck(mtx); return !fft.empty(); }, 3000), "FFT frame");
    {
        std::lock_guard lck(mtx);
        // dB = -offset - range * (1 - b/255): offset 10, range 100.
        require(fft.size() == 4 && std::fabs(fft[0] + 110.0f) < 1e-4f && std::fabs(fft[1] + 10.0f) < 1e-4f &&
                    std::fabs(fft[2] + 90.0f) < 1e-4f,
                "FFT dB mapping");
        require(fftIndex == 150, "FFT frame position " + std::to_string(fftIndex));
    }
    require(client->samplesPublished() == 150, "samples published " + std::to_string(client->samplesPublished()));
    const auto meter = client->readIqMeter();
    require(meter.values == 300 && meter.railed == 1 && meter.peak > 0.99f, "IQ meter");
    require(client->readIqMeter().values == 0, "IQ meter reset");

    client->ping([&](bool answered, uint64_t) {
        secondAnswered = answered;
        secondDone = true;
    });
    require(waitFor([&] { return secondDone.load(); }, 3000), "PONG timeout callback");
    require(!secondAnswered, "unanswered PING reported as answered");

    {
        std::lock_guard lck(server.mtx);
        require(!server.settings.empty() && server.settings[0].first == SPYSERVER_SETTING_IQ_FREQUENCY &&
                    server.settings[0].second == 582000,
                "setting on the wire");
    }

    client->close();
    stop = true;
    out.stopReader();
    reader.join();
    client.reset();
    if (failures == 0) { std::printf("spyserver_client_tests: all passed\n"); }
    return failures == 0 ? 0 : 1;
}
