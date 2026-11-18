// ExportSession over a real non-blocking loopback TCP pair. A slow reader puts
// the socket under backpressure; the received stream must still be a whole
// number of packets, each holding consecutive samples. A peer shutdown must be
// noticed by the DSP-side probe.
#include "export_session.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using iq_exporter::ExportSession;

namespace {
    void require(bool condition, const char* message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    struct Pair {
        std::shared_ptr<net::Listener> listener;
        std::shared_ptr<net::Socket> server;
        std::shared_ptr<net::Socket> client;
    };

    // Below the Linux and Windows ephemeral ranges (no self-connect).
    Pair connectPair() {
        Pair p;
        int port = 24450;
        for (; port < 24470 && !p.listener; port++) {
            try { p.listener = net::listen("127.0.0.1", port); }
            catch (const std::exception&) {}
        }
        require(p.listener != nullptr, "could not reserve a loopback test port");
        port--;
        std::thread acc([&] { p.server = p.listener->accept(NULL, 2000); });
        p.client = net::connect("127.0.0.1", port, 2000);
        acc.join();
        require(p.server && p.client, "could not open a loopback connection");
        return p;
    }

    void backpressureKeepsPacketsWhole() {
        Pair p = connectPair();
        const int packetSize = 32768;
        const int perPacket = packetSize / (int)sizeof(dsp::complex_t);
        ExportSession session(p.server, PROTOCOL_TCP_SERVER, SAMPLE_TYPE_FLOAT32, packetSize);

        std::atomic<bool> done{false};
        std::vector<uint8_t> rx;
        std::thread reader([&] {
            uint8_t buf[3000];
            while (true) {
                int n = p.client->recv(buf, sizeof(buf), false, 200);
                if (n <= 0) {
                    if (done) { break; }
                    continue;
                }
                rx.insert(rx.end(), buf, buf + n);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        // re carries a running sample index, im its negation.
        float idx = 0;
        std::vector<dsp::complex_t> block(1000);
        for (int b = 0; b < 2000; b++) {
            for (auto& s : block) { s = { idx, -idx }; idx += 1; }
            session.consume(block.data(), (int)block.size());
        }
        // Let a pending packet drain, then stop the reader.
        for (int i = 0; i < 50; i++) {
            session.consume(block.data(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        done = true;
        reader.join();
        session.close();

        require(!rx.empty(), "nothing was received");
        require(rx.size() % packetSize == 0, "the stream holds a partial packet");
        for (size_t off = 0; off < rx.size(); off += packetSize) {
            const auto* s = (const dsp::complex_t*)&rx[off];
            for (int i = 0; i < perPacket; i++) {
                require(s[i].im == -s[i].re, "a packet is misaligned");
                require(i == 0 || s[i].re == s[i - 1].re + 1, "a packet has an internal gap");
            }
        }
        printf("backpressure: %zu whole packets received\n", rx.size() / packetSize);
    }

    void peerShutdownIsNoticed() {
        Pair p = connectPair();
        ExportSession session(p.server, PROTOCOL_TCP_SERVER, SAMPLE_TYPE_INT16, 1024);
        require(session.isOpen(), "a fresh session is not open");
        p.client->close();
        std::vector<dsp::complex_t> block(16);
        const auto started = std::chrono::steady_clock::now();
        while (session.isOpen() && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
            session.consume(block.data(), (int)block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(!session.isOpen(), "a peer shutdown was not noticed");
    }
}

int main() {
    backpressureKeepsPacketsWhole();
    peerShutdownIsNoticed();
    return 0;
}
