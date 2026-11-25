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
#include "utils/proto/kiwisdr_wf.h"
#include "utils/url.h"

namespace {
    double parseDouble(const std::string& value) {
        std::istringstream iss(value);
        iss.imbue(std::locale::classic());
        double v = 0.0;
        iss >> v;
        return iss ? v : 0.0;
    }

    uint32_t readLE32(const uint8_t* b) {
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }

    int16_t readS16(const uint8_t* b, bool littleEndian) {
        const uint16_t v = littleEndian ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)((b[0] << 8) | b[1]);
        return (int16_t)v;
    }

    // Retune with no NEW_FREQ packet after this long: the server does not
    // set the flag (UberSDR).
    constexpr auto kNewFreqTimeout = std::chrono::seconds(2);
    // Without audio_rate / sample_rate after this long, go live on the
    // defaults rather than wait forever on a server that never sends them.
    constexpr auto kRateTimeout = std::chrono::seconds(5);
    // Not a single message on the krec path after this long: the Kiwi
    // ignores that path (seen on sdr-amradioantennas.com, KiwiSDR 1.902,
    // even with every channel free); the web UI's path is tried instead.
    constexpr auto kKrecSilenceTimeout = std::chrono::seconds(3);
}

int16_t KiwiSDRClient::readBE16(const char* data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    const uint16_t value = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    if (value < 0x8000) {
        return static_cast<int16_t>(value);
    }
    return static_cast<int16_t>(static_cast<int>(value) - 0x10000);
}

bool KiwiSDRClient::parseSndPacket(const std::string& msg, SndPacket& packet, std::vector<dsp::complex_t>& buffer) {
    // "SND", flags, seq (LE32), S-meter (BE16); IQ adds the GPS fields.
    constexpr size_t kRealHeader = 10;
    constexpr size_t kIqHeader = 20;
    if (msg.size() < kRealHeader || msg.compare(0, 3, "SND") != 0) { return false; }
    const auto* b = reinterpret_cast<const uint8_t*>(msg.data());
    packet = SndPacket();
    packet.flags = b[3];
    packet.seq = readLE32(b + 4);
    packet.rssiDbm = 0.1f * (float)(((uint16_t)b[8] << 8) | b[9]) - 127.0f;
    packet.iq = (packet.flags & kFlagIq) != 0;
    packet.newFreq = (packet.flags & kFlagNewFreq) != 0;
    packet.adcOverload = (packet.flags & kFlagAdcOverload) != 0;
    if (packet.flags & kFlagCompressed) { return false; }
    const bool le = (packet.flags & kFlagLittleEndian) != 0;

    if (packet.iq) {
        if (msg.size() < kIqHeader) { return false; }
        packet.lastGpsSolution = b[10];
        packet.gpsSec = readLE32(b + 12);
        packet.gpsNsec = readLE32(b + 16);
        const size_t count = (msg.size() - kIqHeader) / 4;
        buffer.resize(count);
        const uint8_t* s = b + kIqHeader;
        for (size_t k = 0; k < count; k++, s += 4) {
            buffer[k] = dsp::complex_t{ readS16(s, le) / 32767.0f, readS16(s + 2, le) / 32767.0f };
        }
    }
    else {
        const size_t count = (msg.size() - kRealHeader) / 2;
        buffer.resize(count);
        const uint8_t* s = b + kRealHeader;
        for (size_t k = 0; k < count; k++, s += 2) {
            buffer[k] = dsp::complex_t{ readS16(s, le) / 32767.0f, 0.0f };
        }
    }
    packet.samples = buffer.data();
    packet.count = (int)buffer.size();
    return true;
}

std::string KiwiSDRClient::refusalText(const std::string& key, const std::string& value) {
    if (key == "too_busy") {
        return value.empty() ? "server busy: every channel is in use"
                             : "server busy: all " + value + " channels are in use";
    }
    if (key == "badp") {
        // Codes as the KiwiSDR web UI words them.
        if (value.empty() || value == "0") { return {}; }
        if (value == "1") { return "password required: every channel without a password is in use, or the password is wrong"; }
        if (value == "2") { return "server still determining its local address; try again shortly"; }
        if (value == "3") { return "admin connection not allowed from this address"; }
        if (value == "4") { return "no admin password set; admin only from the local network"; }
        if (value == "5") { return "multiple connections from the same address are not allowed"; }
        if (value == "6") { return "database update in progress; try again in a minute"; }
        if (value == "7") { return "another admin connection is already open"; }
        return "server refused the connection (badp=" + value + ")";
    }
    if (key == "down") { return "server is down"; }
    if (key == "redirect") { return value.empty() ? "server redirects elsewhere" : "server redirects to " + value; }
    if (key == "monitor") { return "server busy: connection parked as a monitor"; }
    if (key == "camp_disconnect") { return "server ended the camping session"; }
    if (key == "ip_limit") {
        // "<minutes>,<address>": the address used up its listening time.
        const std::string minutes = value.substr(0, value.find(','));
        return minutes.empty() ? "time limit for this address reached"
                               : "time limit for this address reached (" + minutes + " min)";
    }
    return {};
}

void KiwiSDRClient::setConnectionStatus(const char* status) {
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    snprintf(connectionStatus, sizeof connectionStatus, "%s", status);
}

std::string KiwiSDRClient::getConnectionStatus() const {
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    if (!refusal.empty()) { return "Refused: " + refusal; }
    return connectionStatus;
}

std::string KiwiSDRClient::refusalReason() const {
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    return refusal;
}

bool KiwiSDRClient::adcOverloadRecent() const {
    const int64_t at = lastOverloadTicks.load();
    if (at == 0) { return false; }
    return Clock::now() - Clock::time_point(Clock::duration(at)) < std::chrono::seconds(1);
}

KiwiSDRClient::~KiwiSDRClient() {
    flog::info("KiwiSDRClient: destructor");
    stop();
}

void KiwiSDRClient::init(const std::string& hostport) {
    this->hostPort = hostport;
    setConnectionStatus("Not connected");

    wsClient.onDisconnected = [&]() {
        // The krec attempt ends silently; the session goes on on /kiwi/.
        if (retryOnWebUiPath) { return; }
        connected = false;
        this->onDisconnected();
        setConnectionStatus("Disconnected");
    };

    // kiwiclient's order: authenticate and identify now; answer audio_rate
    // with SET AR OK; go live (AGC, tune, onConnected) once sample_rate says
    // what the stream really runs at. No password is "p=" on the krec path,
    // as kiwiclient sends it, and "p=#" on the web UI's.
    wsClient.onConnected = [&]() {
        wsUpAt = Clock::now();
        anyMessage = false;
        wsClient.sendString(webUiPath ? "SET auth t=kiwi p=#" : "SET auth t=kiwi p=");
        wsClient.sendString("SERVER DE CLIENT sdriak SND");
        wsClient.sendString("SET ident_user=sdriak");
        wsClient.sendString("SET compression=0");
        setConnectionStatus("Connected, waiting for the server...");
    };
    wsClient.onTextMessage = [&](const std::string& msg) {
        flog::info("TEXT: {}", msg);
    };

    wsClient.onBinaryMessage = [&](const std::string& msg) {
        anyMessage = true;
        if (msg.size() <= 3) { return; }
        if (msg.compare(0, 3, "MSG") == 0) {
            parseMsgFrame(msg);
        }
        else if (msg.compare(0, 3, "SND") == 0) {
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
            SndPacket packet;
            if (parseSndPacket(msg, packet, sndBuffer)) {
                handlePacket(packet);
            }
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
        if (!webUiPath && !anyMessage && wsUpAt != Clock::time_point() && now - wsUpAt > kKrecSilenceTimeout) {
            flog::warn("KiwiSDRClient: {}: no answer on the krec path, trying /kiwi/", hostPort);
            retryOnWebUiPath = true;
            wsClient.stopSocket();
            return;
        }
        if (!connected.load() && refusalReason().empty() && wsUpAt != Clock::time_point() && now - wsUpAt > kRateTimeout) {
            flog::warn("KiwiSDRClient: no sample_rate from the server, assuming {} S/s", audioRate());
            if (!audioRateReported) {
                trySend("SET AR OK in=" + std::to_string(audioRate()) + " out=48000");
            }
            goLive();
        }
    };
}

void KiwiSDRClient::handlePacket(const SndPacket& packet) {
    if (haveSeq) {
        // Forward gaps only (mod 2^32); a repeated or older seq is no loss.
        const uint32_t step = packet.seq - lastSeq;
        if (step > 1 && step < 0x80000000u) { seqGapCount += step - 1; }
    }
    haveSeq = true;
    lastSeq = packet.seq;
    rssi.store(packet.rssiDbm);
    if (packet.adcOverload) {
        lastOverloadTicks.store(Clock::now().time_since_epoch().count());
    }

    if (packet.iq && packet.count != kKiwiPacketSamples) {
        markCompatible("packets are not 512 samples");
    }
    if (packet.newFreq) {
        newFreqSinceTune = true;
        Flavour expected = Flavour::Unknown;
        serverFlavour.compare_exchange_strong(expected, Flavour::KiwiSDR);
    }
    else if (!newFreqSinceTune.load()) {
        const int64_t sent = tuneSentTicks.load();
        if (sent != 0 && Clock::now() - Clock::time_point(Clock::duration(sent)) > kNewFreqTimeout) {
            newFreqSinceTune = true;
            markCompatible("no NEW_FREQ after a retune");
        }
    }

    // Packets of the other mode arrive while a mode switch takes effect.
    if (packet.iq != (currentModulation.load() == TUNE_IQ)) { return; }
    onPacket(packet);
    onSamples(packet.samples, packet.count);
}

void KiwiSDRClient::markCompatible(const char* why) {
    if (serverFlavour.exchange(Flavour::KiwiCompatible) != Flavour::KiwiCompatible) {
        flog::info("KiwiSDRClient: Kiwi-compatible server ({})", why);
    }
}

void KiwiSDRClient::goLive() {
    if (connected.load()) { return; }
    sendAgcLine();
    connected = true;
    setConnectionStatus("Connected, waiting data...");
    if (this->onConnected) {
        onConnected();
    }
}

void KiwiSDRClient::refuse(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(connectionStatusLock);
        if (!refusal.empty()) { return; }
        refusal = reason;
    }
    flog::warn("KiwiSDRClient: {}: {}", hostPort, reason);
    onError(reason);
    wsClient.stopSocket();
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
    // The server clamps the passband to +-(rate/2 - 1); stay just inside.
    const int halfBand = audioRate() / 2 - 100;
    char buf[1024];
    switch (mod) {
    case Modulation::TUNE_IQ:
        snprintf(buf, sizeof buf, "SET mod=iq low_cut=%d high_cut=%d freq=%0.3f", -halfBand, halfBand, serverFreq / 1000.0);
        break;
    case Modulation::TUNE_REAL:
        snprintf(buf, sizeof buf, "SET mod=usb low_cut=0 high_cut=8000 freq=%0.3f", (serverFreq - 3000) / 1000.0);
        break;
    }
    newFreqSinceTune = false;
    tuneSentTicks.store(Clock::now().time_since_epoch().count());
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
        if (eq == std::string::npos) {
            onKeyValue(token, std::string());
            continue;
        }
        onKeyValue(token.substr(0, eq), url::decode(token.substr(eq + 1)));
    }
}

void KiwiSDRClient::onKeyValue(const std::string& key, const std::string& value) {
    auto it = keyValues.find(key);
    if (it != keyValues.end() && it->second == value) {
        return;
    }
    keyValues[key] = value;
    const std::string refused = refusalText(key, value);
    if (!refused.empty()) {
        refuse(refused);
        return;
    }
    if (key == "audio_rate") {
        const int rate = (int)std::lround(parseDouble(value));
        if (rate > 0) {
            audioRateHz.store(rate);
            audioRateReported = true;
            trySend("SET AR OK in=" + std::to_string(rate) + " out=48000");
        }
    }
    else if (key == "sample_rate") {
        const double rate = parseDouble(value);
        if (rate > 0.0) {
            sampleRateHz.store(rate);
            flog::info("KiwiSDRClient: {}: audio_rate {}, sample_rate {}", hostPort, audioRate(), std::to_string(rate));
            // KiwiSDR derives it from the corrected ADC clock, never exact.
            if (audioRateReported && rate == (double)audioRate()) {
                markCompatible("sample_rate equals audio_rate");
            }
            goLive();
        }
    }
    else if (key == "wf_chans") {
        // KiwiSDR firmware has at most 8 waterfall channels.
        if (std::atoi(value.c_str()) > 8) { markCompatible("more than 8 waterfall channels"); }
    }
    else if (key == "freq_offset") {
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
        const double hz = parseDouble(value);
        if (hz > 0.0) {
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
    sampleRateHz.store(0.0);
    serverFlavour.store(Flavour::Unknown);
    rssi.store(-127.0f);
    seqGapCount.store(0);
    lastOverloadTicks.store(0);
    audioRateReported = false;
    haveSeq = false;
    lastSeq = 0;
    wsUpAt = Clock::time_point();
    webUiPath = false;
    anyMessage = false;
    retryOnWebUiPath = false;
    tuneSentTicks.store(0);
    newFreqSinceTune = true;
    std::lock_guard<std::mutex> lock(connectionStatusLock);
    refusal.clear();
}

void KiwiSDRClient::stop() {
    if (running.load() || looperThread.joinable()) {
        setConnectionStatus("Disconnecting..");
    }
    running = false;
    wsClient.stopSocket();
    if (waterfall) { waterfall->stop(); }
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
            const std::string tsPath = std::to_string(ts);
            // Set when the krec W/F got no answer at all: the Kiwi ignores
            // the krec path, so SND goes to the web UI's at once.
            bool sndOnWebUi = false;
            if (waterfall && running.load()) {
                // W/F first: it claims a waterfall-capable channel, which
                // the SND connection with the same ts then joins.
                const auto cancel = [this] { return !running.load(); };
                waterfall->start(origin + "/" + tsPath + "/W/F", waterfallSpeed, 0, waterfallCfKhz);
                bool ready = waterfall->waitReady(kWaterfallSetupTimeout, cancel);
                if (!ready && !waterfall->serverAnswered() && running.load()) {
                    flog::warn("KiwiSDRClient: {}: no answer on the krec W/F path, trying /kiwi/", hostPort);
                    sndOnWebUi = true;
                    waterfall->start(origin + "/kiwi/" + tsPath + "/W/F", waterfallSpeed, 0, waterfallCfKhz, true);
                    ready = waterfall->waitReady(kWaterfallSetupTimeout, cancel);
                }
                if (!ready) {
                    flog::warn("KiwiSDRClient: {}: no server waterfall ({}), audio only",
                               hostPort, waterfall->unavailableReason());
                    waterfall->stop();
                }
            }
            // origin is http(s)://host[:port]; WSClient maps it to ws(s).
            // The krec path first, not the web UI's /kiwi/<ts>/SND: a full
            // Kiwi refuses it with too_busy instead of parking it as a
            // monitor.
            if (sndOnWebUi) {
                webUiPath = true;
                wsClient.connectAndReceiveLoop(origin + "/kiwi/" + tsPath + "/SND");
            }
            else {
                wsClient.connectAndReceiveLoop(origin + "/" + tsPath + "/SND");
            }
            if (retryOnWebUiPath) {
                retryOnWebUiPath = false;
                webUiPath = true;
                // reset() before reading running: a stop() that comes later
                // still finds its stopSocket() in effect.
                wsClient.reset();
                if (running.load()) {
                    wsClient.connectAndReceiveLoop(origin + "/kiwi/" + tsPath + "/SND");
                }
                else {
                    wsClient.stopSocket();
                    this->onDisconnected();
                }
            }
            if (waterfall) { waterfall->stop(); }
            flog::info("x.connectAndReceiveLoop exited.");
            setConnectionStatus("Disconnected");
            connected = false;
            running = false;
        }
        catch (const std::exception& e) {
            flog::error("KiwiSDRClient: Exception: {}", e.what());
            if (waterfall) { waterfall->stop(); }
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
            if (waterfall) { waterfall->stop(); }
            setConnectionStatus("Error: unknown exception");
            connected = false;
            running = false;
            onError("unknown exception");
        }
    });
}
