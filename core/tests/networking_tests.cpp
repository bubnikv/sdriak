// ConnClass / ListenerClass lifetime: close() from a foreign thread wakes a
// blocked reader without releasing the descriptor under it, a closed listener
// frees its port at once, writeSome() never blocks, and a read handler may
// close its own connection.
#include <utils/networking.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

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

    void writeSomeNeverBlocks() {
        Pair p = connectPair();
        std::vector<uint8_t> chunk(65536, 0x5a);
        const auto started = clock_type::now();
        size_t queued = 0;
        int sent = 0;
        while ((sent = p.server->writeSome((int)chunk.size(), chunk.data())) > 0) {
            queued += sent;
            require(clock_type::now() - started < std::chrono::seconds(5),
                    "the send buffer never filled");
        }
        require(sent == 0, "a full send buffer was reported as an error");
        require(p.server->isOpen(), "a full send buffer closed the connection");

        // Drain what was queued; the connection must accept data again.
        std::vector<uint8_t> sink(queued);
        require(p.client->read((int)queued, sink.data(), true) == (int)queued,
                "the peer did not receive every queued byte");
        require(p.server->writeSome(16, chunk.data()) == 16,
                "writeSome() did not resume after the peer drained");
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
    writeSomeNeverBlocks();
    handlerMayCloseItsConnection();
    return 0;
}
