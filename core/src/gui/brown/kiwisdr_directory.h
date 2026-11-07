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
#pragma once

#include <atomic>
#include <cstdint>
#include <imgui.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ServerEntry {
    struct FrequencyBand {
        uint64_t startHz = 0;
        uint64_t endHz = 0;
    };

    ImVec2 gps;
    std::string name;
    std::string loc;
    std::string url;
    std::string antenna;
    std::string sdrHardware;   // "sdr_hw" field of kiwisdr_com.js
    std::string swVersion;     // KiwiSDR firmware version
    std::string qth;           // 6-char Maidenhead grid square derived from gps
    float maxSnr = 0.0f;
    float secondSnr = 0.0f;
    int users = 0;
    int usersmax = 0;
    int extApi = 0;
    std::optional<FrequencyBand> band;
    bool selected = false;
};

/**
 * Fetches and caches the public KiwiSDR receiver list from rx.linkfanel.net.
 * The HTTP fetch runs on a background thread; the UI thread polls for the
 * parsed result via takeIfReady().
 */
class KiwiSDRDirectoryClient {
public:
    explicit KiwiSDRDirectoryClient(std::string root);
    ~KiwiSDRDirectoryClient();

    KiwiSDRDirectoryClient(const KiwiSDRDirectoryClient&) = delete;
    KiwiSDRDirectoryClient& operator=(const KiwiSDRDirectoryClient&) = delete;

    /// Idempotent: starts a background fetch the first time it is called and
    /// after that does nothing until the result has been delivered.
    void requestRefresh();

    bool isLoading() const;
    std::string errorMessage() const;

    /// Returns the parsed result the first time it is ready and resets the
    /// internal state; subsequent calls return std::nullopt.
    std::optional<std::vector<ServerEntry>> takeIfReady();

private:
    void fetchThreadFn();

    const std::string root;
    std::thread fetchThread;
    mutable std::mutex stateMutex;
    bool loading = false;
    bool fetchAttempted = false;
    std::string errorMsg;
    std::optional<std::vector<ServerEntry>> result;
};

/**
 * Probes a KiwiSDR server: connects, tunes 14.074 MHz IQ, waits up to 5 s
 * for the first IQ packet, reports OK / NOT OK. Status text is observable
 * from the UI thread via the snapshot accessors.
 */
class KiwiSDRTester {
public:
    KiwiSDRTester() = default;
    ~KiwiSDRTester();

    KiwiSDRTester(const KiwiSDRTester&) = delete;
    KiwiSDRTester& operator=(const KiwiSDRTester&) = delete;

    struct OkServer {
        std::string hostPort;
        std::string loc;
        // Served frequency range from the directory 'bands' field, if known.
        // Carried back so the source can constrain the tuning UI to what the
        // server actually receives.
        std::optional<ServerEntry::FrequencyBand> band;
    };

    /// Start probing the given URL. No-op if a probe is already in flight.
    void start(std::string url, std::string loc, std::optional<ServerEntry::FrequencyBand> band);

    /// Request cancellation of any in-flight probe.
    void cancel();

    bool isInProgress() const;
    std::string statusText() const;
    std::string errorText() const;

    /// Returns the most recently successfully-tested server, if any.
    /// Persists across calls until a new test starts.
    std::optional<OkServer> lastOk() const;

private:
    void testThreadFn(std::string url, std::string loc, std::optional<ServerEntry::FrequencyBand> band);

    std::thread testThread;
    mutable std::mutex stateMutex;
    bool inProgress = false;
    std::atomic<bool> cancelRequested{false};
    std::string status;
    std::string error;
    std::optional<OkServer> lastOkServer;
};
