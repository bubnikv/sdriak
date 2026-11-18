// ConnClass / ListenerClass lifetime: close() from a foreign thread wakes a
// blocked reader without releasing the descriptor under it, a closed listener
// frees its port at once, a read handler may close its own connection, and a
// pending read notices a peer that closes while no data flows.
#include <utils/networking.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
    using clock_type = std::chrono::steady_clock;

    void require(bool condition, const char* message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    // Ports below the Linux (32768+) and Windows (49152+) ephemeral ranges, so
    // an outgoing connection can never self-connect on a port under test.
    net::Listener listenOnTestPort(uint16_t& port) {
        for (port = 24420; port < 24440; port++) {
            try { return net::listen("127.0.0.1", port); }
            catch (const std::exception&) {}
        }
        throw std::runtime_error("could not reserve a loopback test port");
    }

    struct Pair {
        net::Listener listener;
        net::Conn server;
        net::Conn client;
        uint16_t port = 0;
    };

    Pair connectPair() {
        Pair p;
        p.listener = listenOnTestPort(p.port);
        std::thread acc([&] { p.server = p.listener->accept(); });
        p.client = net::connect("127.0.0.1", p.port, 2000);
        acc.join();
        require(p.server && p.client, "could not open a loopback connection");
        return p;
    }

    void closeWakesBlockedReader() {
        Pair p = connectPair();
        std::atomic<int> result{1};
        std::thread reader([&] {
            uint8_t buf[16];
            result = p.server->read(sizeof(buf), buf, false);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto started = clock_type::now();
        p.server->close();
        reader.join();
        require(result.load() < 0, "a read blocked across close() did not fail");
        require(clock_type::now() - started < std::chrono::seconds(1),
                "close() from another thread did not wake the blocked reader");
        require(!p.server->isOpen(), "connection still open after close()");
    }

    void closedListenerFreesPort() {
        uint16_t port = 0;
        auto listener = listenOnTestPort(port);
        listener->close();
        // close() itself, not the destructor, must release the port.
        auto again = net::listen("127.0.0.1", port);
        require(again && again->isListening(), "a closed listener kept its port");
        again->close();
        listener.reset();
    }

    // network_sink's health watch: one pending read on a connection the
    // peer never writes to notices the peer closing, with no data flowing.
    void pendingReadSeesSilentClose() {
        Pair p = connectPair();
        require(p.server->setLivenessChecks(10, 5, 3, 20000), "keepalive could not be enabled");
        uint8_t buf[16];
        p.server->readAsync(sizeof(buf), buf, [](int, uint8_t*, void*) {}, NULL, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(p.server->isOpen(), "an idle connection was reported closed");
        p.client->close();
        const auto started = clock_type::now();
        while (p.server->isOpen() && clock_type::now() - started < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(!p.server->isOpen(), "a silent peer close was not noticed");

        net::Conn udp = net::openUDP("127.0.0.1", 0, "127.0.0.1", 9, false);
        require(!udp->setLivenessChecks(10, 5, 3, 20000), "liveness checks accepted a UDP socket");
    }

    struct SelfClose {
        net::Conn conn;
        uint8_t buf[4];
        std::atomic<bool> handled{false};
    };

    void handlerMayCloseItsConnection() {
        Pair p = connectPair();
        SelfClose ctx;
        ctx.conn = std::move(p.server);
        ctx.conn->readAsync(1, ctx.buf, [](int, uint8_t*, void* c) {
            auto* s = (SelfClose*)c;
            s->conn->close(); // runs on the read worker itself
            s->handled = true;
        }, &ctx, true);
        uint8_t byte = 1;
        p.client->write(1, &byte);
        const auto started = clock_type::now();
        while (!ctx.handled && clock_type::now() - started < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(ctx.handled, "the read handler never ran");
        ctx.conn.reset(); // joins the worker from this thread
    }
}

int main() {
    closeWakesBlockedReader();
    closedListenerFreesPort();
    handlerMayCloseItsConnection();
    pendingReadSeesSilentClose();
    return 0;
}
