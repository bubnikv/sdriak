#include "tcp_probe.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <utils/networking.h>
#include <utils/proxy.h>

namespace remote {

    ProbeResult probeTcp(const RemoteEntry& entry, const std::atomic<bool>& cancel, int expectBytes, bool viaProxy) {
        using Clock = std::chrono::steady_clock;
        ProbeResult result;
        if (entry.target.port <= 0 || entry.target.port > 65535) {
            result.reason = "bad port";
            return result;
        }
        const auto t0 = Clock::now();
        net::Conn conn;
        try {
            const auto port = (uint16_t)entry.target.port;
            conn = viaProxy ? net::proxy::connect(entry.target.host, port, 3000) : net::connect(entry.target.host, port, 3000);
        }
        catch (const std::exception& e) {
            result.reason = e.what();
            return result;
        }
        if (!conn || !conn->isOpen()) {
            result.reason = "refused";
            return result;
        }
        result.rttMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (expectBytes <= 0) {
            conn->close();
            result.ok = !cancel;
            if (cancel) { result.reason = "cancelled"; }
            return result;
        }

        // read() has no timeout: read on a helper thread and close the
        // connection to release it.
        std::vector<uint8_t> buf((size_t)expectBytes);
        std::atomic<int> got{ -1 };
        std::thread reader([&] { got = conn->read(expectBytes, buf.data()); });
        while (!cancel && got.load() < 0 && Clock::now() - t0 < std::chrono::seconds(3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        conn->close();
        reader.join();
        if (cancel) {
            result.reason = "cancelled";
        }
        else if (entry.kind == RemoteKind::RtlTcp && (got.load() < 12 || std::memcmp(buf.data(), "RTL0", 4) != 0)) {
            result.reason = "expected an RTL0 greeting from rtl_tcp";
        }
        else if (got.load() >= expectBytes) {
            result.ok = true;
            std::string magic;
            for (int i = 0; i < 4 && i < expectBytes; i++) {
                const char c = (char)buf[(size_t)i];
                magic += (c >= 32 && c < 127) ? c : '.';
            }
            result.negotiated.emplace_back("First bytes", magic);
        }
        else {
            result.reason = "connected, but the server sent nothing";
        }
        return result;
    }

}
