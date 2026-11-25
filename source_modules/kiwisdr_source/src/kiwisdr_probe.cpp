/*
 * This file is based on code from project SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown)
 * originally licensed under the GNU General Public License, version 3 (GPLv3).
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
#include "kiwisdr_probe.h"

#include <chrono>
#include <thread>

#include <utils/flog.h>
#include "utils/proto/kiwisdr.h"

remote::ProbeResult probeKiwiSDR(const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    remote::ProbeResult result;
    if (entry.target.scheme != "http") {
        result.reason = "TLS required";
        return result;
    }
    const std::string hostPort = entry.target.hostPort();

    // Connect, tune IQ to the middle of the served range, wait up to 5 s for
    // the first SND packet, close.
    KiwiSDRClient client;
    std::atomic<bool> connected{ false };
    std::atomic<bool> disconnected{ false };
    std::atomic<bool> gotSamples{ false };
    std::atomic<int> firstSampleMs{ -1 };
    const auto t0 = Clock::now();
    client.init(hostPort);
    client.onSamples = [&](const dsp::complex_t*, int count) {
        if (count > 0 && !gotSamples.exchange(true)) {
            firstSampleMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        }
    };
    client.onConnected = [&]() {
        connected = true;
        const uint64_t f = entry.ranges.empty() ? 14074000ull : (entry.ranges.front().first + entry.ranges.front().second) / 2;
        client.tune(double(f), KiwiSDRClient::TUNE_IQ);
    };
    client.onDisconnected = [&]() { disconnected = true; };
    client.start();
    while (!cancel && !disconnected && !gotSamples && Clock::now() - t0 < 5s) {
        std::this_thread::sleep_for(50ms);
    }
    const std::string refused = client.refusalReason();
    const auto served = client.getServedRange();
    const int rate = client.audioRate();
    client.stop();
    if (connected) {
        const auto waitStart = Clock::now();
        while (!disconnected && Clock::now() - waitStart < 5s) { std::this_thread::sleep_for(50ms); }
    }

    if (cancel) {
        result.reason = "cancelled";
    }
    else if (!refused.empty()) {
        result.reason = refused;
    }
    else if (gotSamples) {
        result.ok = true;
        if (firstSampleMs >= 0) { result.rttMs = firstSampleMs.load(); }
        result.negotiated.emplace_back("IQ rate", std::to_string(rate) + " S/s");
        if (served) {
            result.negotiated.emplace_back("Served range", remote::formatRangeMHz((uint64_t)std::max<int64_t>(0, served->minHz),
                                                                                  (uint64_t)std::max<int64_t>(0, served->maxHz)));
        }
    }
    else if (connected) {
        result.reason = "connected, but no data within 5 s";
    }
    else {
        result.reason = "could not connect";
    }
    return result;
}
