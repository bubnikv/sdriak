#include "kiwisdr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>
#include <stdexcept>

#include <json.hpp>

#include "utils/flog.h"
#include "utils/proto/curl_http.h"
#include "utils/url.h"

int16_t KiwiSDRClient::readBE16(const char* data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    const uint16_t value = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    if (value < 0x8000) {
        return static_cast<int16_t>(value);
    }
    return static_cast<int16_t>(static_cast<int>(value) - 0x10000);
}

void KiwiSDRClient::setConnectionStatus(const char* status) {
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    snprintf(connectionStatus, sizeof connectionStatus, "%s", status);
}

std::string KiwiSDRClient::getConnectionStatus() const {
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    return connectionStatus;
}

KiwiSDRClient::~KiwiSDRClient() {
    flog::info("KiwiSDRClient: destructor");
    stop();
}

void KiwiSDRClient::init(const std::string& hostport) {
    this->hostPort = hostport;
    setConnectionStatus("Not connected");

    wsClient.onDisconnected = [&]() {
        connected = false;
        this->onDisconnected();
        setConnectionStatus("Disconnected");
    };

    wsClient.onConnected = [&]() {
        // x.sendString("SET mod=usb low_cut=300 high_cut=2700 freq=14100.000");
        wsClient.sendString("SET auth t=kiwi p=#");
        wsClient.sendString("SET AR OK in=" + std::to_string(IQDATA_FREQUENCY) + " out=48000");
        //            x.sendString("SET mod=am low_cut=-4900 high_cut=4900 freq=119604.33");
        wsClient.sendString("SERVER DE CLIENT sdriak SND");
        wsClient.sendString("SET compression=0");
        sendAgcLine();
        connected = true;
        if (this->onConnected) {
            onConnected();
        }
        setConnectionStatus("Connected, waiting data...");
        //            wsClient.sendString("SET mod=iq low_cut=-5000 high_cut=5000 freq=14074.000");
    };
    wsClient.onTextMessage = [&](const std::string& msg) {
        flog::info("TEXT: {}", msg);
    };

    wsClient.onBinaryMessage = [&](const std::string& msg) {
        std::string start = "???";
        if (msg.size() > 3) {
            start = msg.substr(0, 3);
        }
        if (start == "MSG") {
            parseMsgFrame(msg);
        }
        else if (start == "SND") {
            using namespace std::chrono_literals;
            auto ctm = Clock::now();
            times.emplace_back(ctm);
            int lastSecondCount = 0;
            for (int q = times.size() - 1; q >= 0; q--) {
                if (times[q] < ctm - 1s) {
                    break;
                }
                lastSecondCount++;
            }
            while (!times.empty() && times.front() < ctm - 2s) {
                times.pop_front();
            }
            char status[100];
            snprintf(status, sizeof status, "Receiving. %d KB/sec (%d)", (lastSecondCount * ((int)msg.size())) / 1024, lastSecondCount);
            setConnectionStatus(status);
            int IQ_HEADER_SIZE = 20;
            int REAL_HEADER_SIZE = 10;
            const Modulation modulation = currentModulation.load();
            if (modulation == TUNE_REAL && msg.size() == 1024 + REAL_HEADER_SIZE) { // REAL data
                const char* samples = msg.data() + REAL_HEADER_SIZE;
                dsp::complex_t decoded[512];
                for (int z = 0; z < 512; z++) {
                    const int16_t sample = readBE16(samples + (z * 2));
                    decoded[z] = dsp::complex_t{ sample / 32767.0f, 0.0f };
                }
                onSamples(decoded, 512);
                snprintf(status, sizeof status, "Cont Recv. %d KB/sec (%d)", (lastSecondCount * ((int)msg.size())) / 1024, lastSecondCount);
                setConnectionStatus(status);
            }
            if (modulation == TUNE_IQ && msg.size() == 2048 + IQ_HEADER_SIZE && msg[3] == 0x08) { // IQ data
                const char* samples = msg.data() + IQ_HEADER_SIZE;
                dsp::complex_t decoded[512];
                for (int z = 0; z < 512; z++) {
                    const char* iqsample = samples + (z * 4);
                    const int16_t i = readBE16(iqsample);
                    const int16_t q = readBE16(iqsample + 2);
                    decoded[z] = dsp::complex_t{ i / 32767.0f, q / 32767.0f };
                }
                onSamples(decoded, 512);
                //                    flog::info("{} Got sound: bytes={} , {} samples, buflen now = {} (erased {})", (int64_t)currentTimeMillis(), msg.size(), (msg.size() - HEADER_SIZE) / 4, buflen, erased);
            }
        }
        else {
            if (msg.size() >= 70) {
                char buf[100];
                for (int q = 3; q < 30; q++) {
                    snprintf(buf, sizeof buf, "%02x ", (unsigned char)msg[q]);
                    start += buf;
                }
                start += "... ";
                for (int q = -20; q < 0; q++) {
                    snprintf(buf, sizeof buf, "%02x ", (unsigned char)msg[msg.size() - 1 + q]);
                    start += buf;
                }
            }
//                flog::info("=> BIN: {} bytes: {}", (int64_t)msg.size(), start);
        }
    };
    using namespace std::chrono_literals;
    lastPing = Clock::now();
    // Fires on every receive-loop tick, including recv timeouts during a
    // stall — the keepalive must keep flowing even when no data arrives or
    // the server drops the session as inactive. A send failure here throws
    // and tears the session down via the receive loop, which is correct:
    // if the keepalive can't be sent, the connection is gone.
    wsClient.onReceiveLoopTick = [&]() {
        auto now = Clock::now();
        if (now - lastPing > 4s) {
            wsClient.sendString("SET keepalive");
            lastPing = now;
        }
    };
}

void KiwiSDRClient::tune(double freq, Modulation mod) {
    currentFrequency.store(freq);
    currentModulation.store(mod);
    if (connected.load()) {
        sendTuneCommand(freq, mod);
    }
}

void KiwiSDRClient::setAgc(AgcSettings s) {
    s.thresholdDb  = std::clamp(s.thresholdDb,  -130, 0);
    s.slopeDb      = std::clamp(s.slopeDb,      0, 10);
    s.decayMs      = std::clamp(s.decayMs,      20, 5000);
    s.manualGainDb = std::clamp(s.manualGainDb, 0, 120);
    {
        std::lock_guard<std::mutex> lock(agcMutex);
        agc = s;
    }
    if (connected) {
        sendAgcLine();
    }
}

KiwiSDRClient::AgcSettings KiwiSDRClient::getAgc() const {
    std::lock_guard<std::mutex> lock(agcMutex);
    return agc;
}

bool KiwiSDRClient::trySend(const std::string& cmd) {
    try {
        wsClient.sendString(cmd);
        return true;
    }
    catch (const std::exception& e) {
        flog::warn("KiwiSDRClient: dropped command '{}': {}", cmd, e.what());
        return false;
    }
}

void KiwiSDRClient::sendTuneCommand(double freq, Modulation mod) {
    const double serverFreq = freq - static_cast<double>(serverFrequencyOffset.load());
    char buf[1024];
    switch (mod) {
    case Modulation::TUNE_IQ:
        snprintf(buf, sizeof buf, "SET mod=iq low_cut=-7000 high_cut=7000 freq=%0.3f", serverFreq / 1000.0);
        break;
    case Modulation::TUNE_REAL:
        snprintf(buf, sizeof buf, "SET mod=usb low_cut=0 high_cut=8000 freq=%0.3f", (serverFreq - 3000) / 1000.0);
        break;
    }
    trySend(buf);
}

void KiwiSDRClient::sendAgcLine() {
    // Take a snapshot under the mutex so the SET line on the wire
    // always reflects a single, coherent setAgc() — never a mix of
    // before/after values from an in-flight update on another thread.
    AgcSettings snap = getAgc();
    char buf[256];
    snprintf(buf, sizeof buf, "SET agc=%d hang=%d thresh=%d slope=%d decay=%d manGain=%d",
             snap.enabled ? 1 : 0,
             snap.hang ? 1 : 0,
             snap.thresholdDb,
             snap.slopeDb,
             snap.decayMs,
             snap.manualGainDb);
    trySend(buf);
}

void KiwiSDRClient::parseMsgFrame(const std::string& msg) {
    std::istringstream iss(msg);
    std::string token;
    iss >> token;  // consume the "MSG" prefix
    while (iss >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        onKeyValue(token.substr(0, eq), url::decode(token.substr(eq + 1)));
    }
}

void KiwiSDRClient::onKeyValue(const std::string& key, const std::string& value) {
    auto it = keyValues.find(key);
    if (it != keyValues.end() && it->second == value) {
        return;
    }
    keyValues[key] = value;
    if (key == "freq_offset") {
        serverFrequencyOffset.store(parseKhzToHz(value));
        // Re-issue the last user-requested tune so the displayed
        // frequency stays correct once the offset is known. Skip if
        // no tune has happened yet (currentFrequency still default).
        const double last = currentFrequency.load();
        if (last != 0.0 && connected.load()) {
            sendTuneCommand(last, currentModulation.load());
        }
    }
    else if (key == "bandwidth") {
        // Reported in Hz (e.g. "30000000", or "32000000" on wideband Kiwis).
        std::istringstream iss(value);
        iss.imbue(std::locale::classic());
        double hz = 0.0;
        iss >> hz;
        if (iss && hz > 0.0) {
            serverBandwidth.store(static_cast<int64_t>(std::llround(hz)));
        }
    }
}

std::optional<KiwiSDRClient::ServedRange> KiwiSDRClient::getServedRange() const {
    const int64_t bw = serverBandwidth.load();
    if (bw <= 0) {
        return std::nullopt;
    }
    const int64_t foff = serverFrequencyOffset.load();
    return ServedRange{ foff, foff + bw };
}

int64_t KiwiSDRClient::parseKhzToHz(const std::string& value) {
    // KiwiSDR reports frequencies in kHz with up to 3 decimal places
    // (1 Hz precision). Use the classic locale to avoid surprises with
    // comma-as-decimal locales.
    std::istringstream iss(value);
    iss.imbue(std::locale::classic());
    double khz = 0.0;
    iss >> khz;
    if (!iss) return 0;
    return static_cast<int64_t>(std::llround(khz * 1000.0));
}

// /VER doubles as the scheme probe. A Kiwi behind TLS answers http:// with a
// redirect (hra.remoteqth.com: 302 to its https site root), so the request
// follows redirects and origin becomes the final URL's scheme://host[:port];
// when the redirect lands somewhere other than /VER, /VER is asked again on
// that origin. origin stays http://host:port when the probe fails, and the
// websocket then follows the redirect itself.
uint64_t KiwiSDRClient::fetchServerTimestamp(const std::string& host, int port, std::string& origin) {
    using namespace std::chrono;
    const uint64_t fallback = static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    const bool bracket = host.find(':') != std::string::npos && host.front() != '[';
    origin = "http://" + (bracket ? "[" + host + "]" : host) + ":" + std::to_string(port);
    try {
        net::http::CurlRequestOptions options;
        options.connectTimeoutMs = 5000;
        options.timeoutMs = 5000;
        options.maxRedirects = 5;
        // A redirect to the site root brings the web page along.
        options.maxBody = 1024 * 1024;
        options.headers["Accept"] = "application/json, text/plain, */*";
        options.shouldCancel = [this] { return !running.load(); };

        auto response = net::http::curlGet(origin + "/VER", options);
        const std::string finalOrigin = net::http::urlOrigin(response.effectiveUrl);
        if (!finalOrigin.empty()) {
            origin = finalOrigin;
        }
        if (net::http::urlPath(response.effectiveUrl) != "/VER") {
            flog::info("KiwiSDRClient: /VER moved to {}, asking {}/VER", response.effectiveUrl, origin);
            response = net::http::curlGet(origin + "/VER", options);
        }
        const int status = static_cast<int>(response.status);
        if (status != 200) {
            throw std::runtime_error("KiwiSDRClient: /VER HTTP status " + std::to_string(status));
        }

        auto j = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.contains("ts")) {
            const auto& ts = j["ts"];
            if (ts.is_number_unsigned()) {
                return ts.get<uint64_t>();
            }
            if (ts.is_string()) {
                try { return std::stoull(ts.get<std::string>()); } catch (...) {}
            }
        }
        throw std::runtime_error("KiwiSDRClient: /VER JSON missing ts");
    }
    catch (const std::exception& e) {
        flog::warn("KiwiSDRClient: /VER fetch failed, using local timestamp fallback: {}", e.what());
    }
    catch (...) {
        flog::warn("KiwiSDRClient: /VER fetch failed, using local timestamp fallback: unknown");
    }
    return fallback;
}

void KiwiSDRClient::resetSessionState() {
    keyValues.clear();
    times.clear();
    serverFrequencyOffset.store(0);
    serverBandwidth.store(0);
}

void KiwiSDRClient::stop() {
    if (running.load() || looperThread.joinable()) {
        setConnectionStatus("Disconnecting..");
    }
    running = false;
    wsClient.stopSocket();
    if (looperThread.joinable()) {
        if (looperThread.get_id() != std::this_thread::get_id()) {
            looperThread.join();
        }
    }
    connected = false;
    setConnectionStatus("Disconnected.");
}

void KiwiSDRClient::start() {
    if (running.load()) { return; }
    if (looperThread.joinable()) {
        looperThread.join();
    }
    setConnectionStatus("Connecting..");
    resetSessionState();
    running = true;
    connected = false;
    wsClient.reset();
    looperThread = std::thread([this]() {
        flog::info("calling x.connectAndReceiveLoop..");
        try {
            auto parsed = url::splitHostPort(hostPort);
            if (!parsed) {
                throw std::runtime_error("KiwiSDRClient: malformed host:port: " + hostPort);
            }
            std::string origin;
            const uint64_t ts = fetchServerTimestamp(parsed->host, parsed->port, origin);
            // origin is http(s)://host[:port]; WSClient maps it to ws(s).
            wsClient.connectAndReceiveLoop(origin + "/kiwi/" + std::to_string(ts) + "/SND");
            flog::info("x.connectAndReceiveLoop exited.");
            setConnectionStatus("Disconnected");
            connected = false;
            running = false;
        }
        catch (const std::exception& e) {
            flog::error("KiwiSDRClient: Exception: {}", e.what());
            char status[100];
            snprintf(status, sizeof status, "Error: %s", e.what());
            setConnectionStatus(status);
            connected = false;
            running = false;
            onError(e.what());
        }
        catch (...) {
            // Anything escaping this thread calls std::terminate and kills
            // the whole app — swallow and report instead.
            flog::error("KiwiSDRClient: unknown exception in network thread");
            setConnectionStatus("Error: unknown exception");
            connected = false;
            running = false;
            onError("unknown exception");
        }
    });
}
