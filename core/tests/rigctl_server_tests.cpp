#include <utils/proto/rigctl.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
    void require(bool condition, const char* message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    std::shared_ptr<net::rigctl::Server> startServer(int& port) {
        for (port = 45400; port < 45500; port++) {
            try {
                return net::rigctl::listen("127.0.0.1", port);
            }
            catch (const std::exception&) {
                // Another local process may own an individual test port.
            }
        }
        throw std::runtime_error("could not reserve a loopback test port");
    }
}

int main() {
    int port = 0;
    auto server = startServer(port);

    constexpr double initialFrequency = 7100000.25;
    constexpr double changedFrequency = 14200000.5;
    double frequency = initialFrequency;
    std::mutex frequencyMtx;

    server->onGetFreq([&](double& value) {
        std::lock_guard lck(frequencyMtx);
        value = frequency;
        return 0;
    });
    server->onSetFreq([&](double value) {
        std::lock_guard lck(frequencyMtx);
        frequency = value;
        return 0;
    });

    auto client = net::rigctl::connect("127.0.0.1", port, 2000);
    require(client && client->isOpen(), "could not connect to the Rigctl test server");
    require(std::abs(client->getFreq() - initialFrequency) < 0.001,
            "get frequency returned the wrong value");
    require(client->setFreq(changedFrequency) == 0,
            "set frequency returned an error or consumed a stale response");
    {
        std::lock_guard lck(frequencyMtx);
        require(std::abs(frequency - changedFrequency) < 0.001,
                "set frequency did not reach the registered handler");
    }

    const auto stopStarted = std::chrono::steady_clock::now();
    server->stop();
    const auto stopTime = std::chrono::steady_clock::now() - stopStarted;
    require(stopTime < std::chrono::seconds(3),
            "stopping with an active client did not terminate promptly");
    require(!server->listening(), "server still reports listening after stop");

    // stop() is part of both explicit shutdown and the destructor path.
    server->stop();
    client->close();
    return 0;
}
