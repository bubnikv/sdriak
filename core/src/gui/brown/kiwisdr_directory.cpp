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
#define IMGUI_DEFINE_MATH_OPERATORS
#ifdef _WIN32
#define _WINSOCKAPI_ // stops windows.h including winsock.h
#endif

#include "kiwisdr_directory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>

#include <core.h>
#include <utils/flog.h>
#include "utils/proto/http.h"
#include "utils/proto/kiwisdr.h"
#include "utils/url.h"
#include <gui/brown/geomap.h>


namespace {

    constexpr const char* DIRECTORY_HOST = "rx.linkfanel.net";
    constexpr const char* DIRECTORY_PATH = "/kiwisdr_com.js";
    constexpr const char* CACHE_FILENAME = "kiwisdr_source.receiverlist.json";
    constexpr auto CACHE_TTL = std::chrono::hours(1);
    constexpr size_t MAX_RESPONSE_SIZE = 4 * 1024 * 1024;

    // Strips the JS wrapper from the kiwisdr_com.js response and rewrites
    // the trailing comma so the result is parseable as JSON.
    std::string cleanJsResponse(const std::string& response) {
        const char* BEGIN = "var kiwisdr_com =";
        const char* END = "},\n]\n;";
        auto beginIx = response.find(BEGIN);
        if (beginIx == std::string::npos) {
            throw std::runtime_error("Invalid response from server");
        }
        auto endIx = response.rfind(END);
        const auto contentBeginIx = beginIx + std::strlen(BEGIN);
        if (endIx == std::string::npos || endIx < contentBeginIx) {
            throw std::runtime_error("Invalid response from server");
        }
        std::string out = response.substr(contentBeginIx, endIx - contentBeginIx);
        out += "}]";
        return out;
    }

    std::string fetchDirectoryJson(const std::string& cachePath) {
        auto status = std::filesystem::status(cachePath);
        if (std::filesystem::exists(status)) {
            const auto last_write_time = std::filesystem::last_write_time(cachePath);
            auto last_write_sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            if (std::chrono::system_clock::now() - last_write_sys < CACHE_TTL) {
                std::ifstream ifs(cachePath);
                return std::string((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
            }
        }

        // Bound DNS + TCP connect like the 5 s response timeout below, so a
        // dead directory host can't pin the fetch worker for minutes.
        auto controlSock = net::connect(DIRECTORY_HOST, 80, 5000);
        auto controlHttp = net::http::Client(controlSock);

        net::http::RequestHeader rqhdr(net::http::METHOD_GET, DIRECTORY_PATH, DIRECTORY_HOST);
        controlHttp.sendRequestHeader(rqhdr);
        net::http::ResponseHeader rshdr;
        controlHttp.recvResponseHeader(rshdr, 5000);

        flog::debug("Response from {}: {}", DIRECTORY_HOST, rshdr.getStatusString());

        std::vector<uint8_t> data(16 * 1024);
        std::string response;
        response.reserve(256 * 1024);
        while (true) {
            auto len = controlSock->recv(data.data(), data.size());
            if (len < 1) {
                break;
            }
            if (response.size() + len > MAX_RESPONSE_SIZE) {
                controlSock->close();
                throw std::runtime_error("Server response is too large");
            }
            response.append(reinterpret_cast<char*>(data.data()), len);
        }
        controlSock->close();

        std::string cleaned = cleanJsResponse(response);

        if (FILE* toSave = fopen(cachePath.c_str(), "wt")) {
            fwrite(cleaned.c_str(), 1, cleaned.size(), toSave);
            fclose(toSave);
        }

        return cleaned;
    }

    std::optional<ServerEntry> parseServerEntry(const json& entry) {
        if (!(entry.contains("gps") && entry.contains("name") && entry.contains("url") &&
              entry.contains("loc") &&
              entry.contains("snr") && entry.contains("users") && entry.contains("users_max") &&
              entry.contains("offline"))) {
            return std::nullopt;
        }
        if (entry["offline"].get<std::string>() != "no") {
            return std::nullopt;
        }

        const std::string gps_str = entry["gps"].get<std::string>();
        geomap::GeoCoordinates geo = {0.0, 0.0};
        std::stringstream ss(gps_str);
        ss.imbue(std::locale::classic());          // force '.' as decimal separator
        char discard;
        ss >> discard >> geo.latitude >> discard >> geo.longitude >> discard;
        if (!ss) {
            flog::warn("Parsing geo coordinates failed: \"{}\"", gps_str);
            return std::nullopt;
        }

        ServerEntry e;
        e.gps = geomap::geoToCartesian(geo).toImVec2();
        e.qth = geomap::geoToMaidenhead(geo);
        e.name = entry["name"].get<std::string>();
        e.loc = entry["loc"].get<std::string>();
        e.url = entry["url"].get<std::string>();
        if (entry.contains("antenna")) {
            e.antenna = entry["antenna"].get<std::string>();
        }
        if (entry.contains("sdr_hw")) {
            e.sdrHardware = entry["sdr_hw"].get<std::string>();
        }
        if (entry.contains("sw_version")) {
            e.swVersion = entry["sw_version"].get<std::string>();
        }
        if (entry.contains("bands")) {
            // Best-effort: a malformed bands string is logged but does not
            // cause the whole entry to be dropped — the server can still
            // appear on the map without a usable frequency-band hint.
            const std::string bands_str = entry["bands"].get<std::string>();
            std::stringstream bands(bands_str);
            bands.imbue(std::locale::classic());
            ServerEntry::FrequencyBand band;
            char separator = '\0';
            bands >> band.startHz >> separator >> band.endHz;
            bands >> std::ws;
            if (bands.eof() && separator == '-' && band.startHz <= band.endHz) {    
                e.band = band;
            }
            else {
                flog::warn("Parsing bands failed: \"{}\"", bands_str);
            }
        }
        sscanf(entry["snr"].get<std::string>().c_str(), "%f,%f", &e.maxSnr, &e.secondSnr);
        e.users = atoi(entry["users"].get<std::string>().c_str());
        e.usersmax = atoi(entry["users_max"].get<std::string>().c_str());
        if (entry.contains("ext_api")) {
            e.extApi = atoi(entry["ext_api"].get<std::string>().c_str());
        }
        return e;
    }

}


// === KiwiSDRDirectoryClient ===

KiwiSDRDirectoryClient::KiwiSDRDirectoryClient(std::string root)
    : root(std::move(root)) {}

KiwiSDRDirectoryClient::~KiwiSDRDirectoryClient() {
    if (fetchThread.joinable()) {
        fetchThread.join();
    }
}

void KiwiSDRDirectoryClient::requestRefresh() {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (fetchAttempted) {
            return;
        }
        fetchAttempted = true;
        loading = true;
    }
    if (fetchThread.joinable()) {
        fetchThread.join();
    }
    fetchThread = std::thread(&KiwiSDRDirectoryClient::fetchThreadFn, this);
}

bool KiwiSDRDirectoryClient::isLoading() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return loading;
}

std::string KiwiSDRDirectoryClient::errorMessage() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return errorMsg;
}

std::optional<std::vector<ServerEntry>> KiwiSDRDirectoryClient::takeIfReady() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!result) {
        return std::nullopt;
    }
    auto out = std::move(*result);
    result.reset();
    return out;
}

void KiwiSDRDirectoryClient::fetchThreadFn() {
    std::vector<ServerEntry> parsed;
    std::string failure;
    try {
        const std::string cachePath = root + "/" + CACHE_FILENAME;
        std::string jsonText = fetchDirectoryJson(cachePath);
        json doc = json::parse(jsonText);

        for (const auto& entry : doc) {
            if (auto e = parseServerEntry(entry)) {
                parsed.push_back(std::move(*e));
            }
        }
        flog::info("Parsed {} servers", parsed.size());

        std::sort(parsed.begin(), parsed.end(), [](const ServerEntry& a, const ServerEntry& b) {
            return a.maxSnr < b.maxSnr;
        });
    }
    catch (const std::exception& e) {
        failure = e.what();
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    loading = false;
    if (!failure.empty()) {
        errorMsg = failure;
    }
    else {
        result = std::move(parsed);
    }
}


// === KiwiSDRTester ===

KiwiSDRTester::~KiwiSDRTester() {
    cancelRequested = true;
    if (testThread.joinable()) {
        testThread.join();
    }
}

void KiwiSDRTester::start(std::string url, std::string loc, std::optional<ServerEntry::FrequencyBand> band) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (inProgress) {
            return;
        }
        inProgress = true;
        status = "Testing server " + url + " ...";
        error.clear();
        lastOkServer.reset();
    }
    cancelRequested = false;
    if (testThread.joinable()) {
        testThread.join();
    }
    testThread = std::thread(&KiwiSDRTester::testThreadFn, this, std::move(url), std::move(loc), std::move(band));
}

void KiwiSDRTester::cancel() {
    cancelRequested = true;
}

bool KiwiSDRTester::isInProgress() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return inProgress;
}

std::string KiwiSDRTester::statusText() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return status;
}

std::string KiwiSDRTester::errorText() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return error;
}

std::optional<KiwiSDRTester::OkServer> KiwiSDRTester::lastOk() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return lastOkServer;
}

void KiwiSDRTester::testThreadFn(std::string url, std::string loc, std::optional<ServerEntry::FrequencyBand> band) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    auto setStatus = [this](const std::string& s) {
        std::lock_guard<std::mutex> lock(stateMutex);
        status = s;
    };
    auto isCanceled = [this]() {
        return cancelRequested.load();
    };

    KiwiSDRClient testClient;
    std::atomic<bool> plannedDisconnect{false};
    std::atomic<bool> connectedFlag{false};
    std::atomic<bool> disconnectedFlag{false};
    std::atomic<bool> sampleReceived{false};
    try {
        auto parsedUrl = url::parseHttpHostPort(url);
        if (parsedUrl) {
            const std::string hostPort = parsedUrl->host + ":" + std::to_string(parsedUrl->port);
            setStatus("Testing server " + hostPort + "...");
            testClient.init(hostPort);
            testClient.onSamples = [&](const dsp::complex_t*, int count) {
                if (count > 0) {
                    sampleReceived = true;
                    plannedDisconnect = true;
                }
            };
            testClient.onConnected = [&]() {
                connectedFlag = true;
                setStatus("Connected to server " + hostPort + " ...");
                const uint64_t testFrequency = band ? (band->startHz + band->endHz) / 2 : 14074000;
                testClient.tune(double(testFrequency), KiwiSDRClient::TUNE_IQ);
            };
            testClient.onDisconnected = [&, hostPort, loc, url]() {
                disconnectedFlag = true;
                std::lock_guard<std::mutex> lock(stateMutex);
                if (plannedDisconnect.load()) {
                    status = "Got some data. Server OK: " + url;
                    lastOkServer = OkServer{hostPort, loc, band};
                }
                else {
                    status = "Disconnect, no data. Server NOT OK: " + url;
                }
            };
            testClient.start();
            auto begin = Clock::now();
            while (true) {
                if (isCanceled()) break;
                if (disconnectedFlag.load()) break;
                std::this_thread::sleep_for(100ms);
                if (sampleReceived.load()) {
                    break;
                }
                if (Clock::now() - begin > 5s) break;
            }
            testClient.stop();
            if (isCanceled()) {
                setStatus("Server test canceled");
            }
            else if (connectedFlag.load()) {
                auto disconnectWaitStart = Clock::now();
                while (!disconnectedFlag.load() && !isCanceled() && Clock::now() - disconnectWaitStart < 5s) {
                    std::this_thread::sleep_for(100ms);
                }
                flog::info("Disconnected ok");
            }
            else {
                setStatus("Could not connect to server: " + url);
                std::this_thread::sleep_for(1s);
            }
        }
        else {
            setStatus("Unsupported URL scheme (only http:// is supported): " + url);
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(stateMutex);
        status = "Server test error";
        error = e.what();
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    inProgress = false;
}
