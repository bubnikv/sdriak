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
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dsp/types.h"

#include "utils/proto/websock.h"

class KiwiWaterfallClient;

// KiwiSDR SND client, speaking the kiwiclient/kiwirecorder ("krec") form of
// the protocol: /<ts>/SND (the web UI's /kiwi/<ts>/SND when a Kiwi does not
// answer that), rates from the server's audio_rate and sample_rate, packets
// parsed by their flags and length. Protocol facts from
// jks-prv/Beagle_SDR_GPS and kiwiclient; see doc/research/kiwisdr-streams.md.
struct KiwiSDRClient {
    using Clock = std::chrono::steady_clock;

    enum Modulation {
        TUNE_IQ = 1,
        TUNE_REAL = 2,           // only real data, -3 .. +3 kHz
    };

    // Who serves the protocol. KiwiCompatible is a server that emulates it,
    // like UberSDR: 240-sample packets, no NEW_FREQ flag, an unrelated
    // waterfall seq. Only the packets decide: Web-888 firmware is KiwiSDR
    // (NEW_FREQ, 512 samples) yet reports an exact sample_rate and up to 13
    // waterfall channels. Unknown until the stream shows one or the other.
    enum class Flavour { Unknown, KiwiSDR, KiwiCompatible };

    // SND header flags (byte 3).
    static constexpr uint8_t kFlagLpfChanged = 0x01;
    static constexpr uint8_t kFlagAdcOverload = 0x02;
    static constexpr uint8_t kFlagNewFreq = 0x04;     // first packet assembled after a retune
    static constexpr uint8_t kFlagIq = 0x08;
    static constexpr uint8_t kFlagCompressed = 0x10;  // ADPCM; never requested (compression=0)
    static constexpr uint8_t kFlagLittleEndian = 0x80;

    static constexpr int kDefaultAudioRate = 12000;   // until the server reports audio_rate
    static constexpr int kKiwiPacketSamples = 512;    // KiwiSDR firmware's IQ packet

    // One SND packet. samples points into the client's buffer and is valid
    // only during the callback.
    struct SndPacket {
        uint8_t flags = 0;
        uint32_t seq = 0;            // +1 per packet
        float rssiDbm = 0.0f;        // 0.1 * S-meter - 127
        bool iq = false;
        bool newFreq = false;
        bool adcOverload = false;
        uint8_t lastGpsSolution = 0; // IQ only: s since the last fix; 0 no GPS, 255 no clock
        uint32_t gpsSec = 0;         // IQ only: GPS second of week of the first sample
        uint32_t gpsNsec = 0;
        const dsp::complex_t* samples = nullptr;
        int count = 0;
    };

    // Parses a binary "SND" message into packet, decoding the samples into
    // buffer: IQ (flag 0x08) after a 20-byte header, real after 10 bytes,
    // int16 big-endian unless flag 0x80. The sample count comes from the
    // length. False for a message that is not SND, too short, or compressed.
    static bool parseSndPacket(const std::string& msg, SndPacket& packet, std::vector<dsp::complex_t>& buffer);

    // Readable text for a server message that refuses or ends the session
    // (too_busy, badp other than 0, down, redirect, monitor,
    // camp_disconnect, ip_limit); empty for any other key.
    static std::string refusalText(const std::string& key, const std::string& value);

    net::websock::WSClient wsClient;
    std::string hostPort;

    // Server waterfall: when set before start(), the session opens W/F on
    // this client first (same ts), waits up to kWaterfallSetupTimeout for
    // its setup or refusal, then opens SND. A refused W/F leaves SND alone.
    // stop() and the end of the session stop it too. speed and cf_kHz are
    // its first wf_speed and view centre.
    KiwiWaterfallClient* waterfall = nullptr;
    int waterfallSpeed = 4;
    double waterfallCfKhz = 15000.0;
    static constexpr std::chrono::seconds kWaterfallSetupTimeout{3};
    // Set once the session is set up: the server has reported sample_rate
    // and the AGC line went out. onConnected fires at the same point.
    std::atomic<bool> connected{false};

private:
    char connectionStatus[100] = "Not connected";
    // Why the server refused or ended the session; shown instead of
    // connectionStatus until the next start().
    std::string refusal;
    mutable std::mutex connectionStatusLock;

    // Last user-requested tune; remembered so we can re-issue the SET freq
    // command after the server reports its hardware-converter offset.
    std::atomic<double> currentFrequency{0.0};

    // Server-reported hardware-converter offset (Hz). Subtracted from the
    // user-requested frequency before being sent. Updated by MSG/freq_offset.
    std::atomic<int64_t> serverFrequencyOffset{0};

    // Server-reported ADC passband width (Hz), from the MSG/bandwidth field.
    // 0 = not yet reported. Combined with serverFrequencyOffset this yields
    // the tunable dial span [freq_offset, freq_offset + bandwidth]. The
    // 'center_freq' MSG is deliberately ignored: it is the cosmetic ADC
    // passband center, not part of the tuning span.
    std::atomic<int64_t> serverBandwidth{0};

    // audio_rate of the last server that reported one: the stream's nominal
    // rate (12000, 20250 on rx3.wf3). Kept across sessions, so a reconnect
    // does not switch the DSP back to the default and forth again.
    std::atomic<int> audioRateHz{kDefaultAudioRate};
    // This session's sample_rate: the true rate from the corrected ADC
    // clock, e.g. 11998.902088. 0 until reported.
    std::atomic<double> sampleRateHz{0.0};
    std::atomic<Flavour> serverFlavour{Flavour::Unknown};
    std::atomic<float> rssi{-127.0f};
    std::atomic<uint64_t> seqGapCount{0};
    // Clock::now() of the last packet with the ADC-overload flag, in ticks;
    // 0 = none this session.
    std::atomic<int64_t> lastOverloadTicks{0};

    // Receive thread only.
    std::map<std::string, std::string> keyValues; // MSG key=value tokens
    bool audioRateReported = false;
    bool haveSeq = false;
    uint32_t lastSeq = 0;
    Clock::time_point wsUpAt;
    std::vector<dsp::complex_t> sndBuffer;
    bool webUiPath = false;        // this connection is on /kiwi/<ts>/SND
    bool anyMessage = false;       // the server sent something on this connection
    bool retryOnWebUiPath = false; // the krec path stayed silent; reconnect on /kiwi/

    // Retune bookkeeping for the NEW_FREQ flavour check: when the last tune
    // command went out (ticks, 0 = none yet) and whether a NEW_FREQ packet
    // has arrived since.
    std::atomic<int64_t> tuneSentTicks{0};
    std::atomic<bool> newFreqSinceTune{true};

public:
    // AGC configuration; persisted across reconnects and re-sent on
    // (re)connect or live via setAgc(). Bundled because the six fields
    // are always read and written as one coherent snapshot — the SET line
    // sent to the server must reflect a single, consistent setAgc() call.
    struct AgcSettings {
        bool enabled = false;       // matches pre-merge default (AGC off)
        bool hang = false;
        int  thresholdDb = -100;
        int  slopeDb = 6;
        int  decayMs = 1000;
        int  manualGainDb = 50;
    };

private:
    AgcSettings agc;
    mutable std::mutex agcMutex;

public:
    Clock::time_point lastPing;
    std::atomic<bool> running{false};
    std::atomic<Modulation> currentModulation{TUNE_IQ};
    // deque, not vector: front entries are popped on every packet, and a
    // vector::erase(begin) shift would be O(n) per packet.
    std::deque<Clock::time_point> times;

    std::function<void()> onConnected = []() {};
    std::function<void()> onDisconnected = []() {};
    // Fired on hard failures (handshake, connect, /VER lookup) and when the
    // server refuses the session (refusalText()) — separate from
    // onDisconnected so a tester or UI can distinguish "never came up" from
    // "came up and then went away". The wsClient still fires onDisconnected
    // too, so handlers don't need to subscribe to both.
    std::function<void(const std::string&)> onError = [](const std::string&) {};
    std::thread looperThread;
    // Every packet of the tuned mode, with its header; then onSamples with
    // the same samples.
    std::function<void(const SndPacket&)> onPacket = [](const SndPacket&) {};
    std::function<void(const dsp::complex_t*, int)> onSamples = [](const dsp::complex_t*, int) {};

    static int16_t readBE16(const char* data);

    void setConnectionStatus(const char* status);
    std::string getConnectionStatus() const;

    virtual ~KiwiSDRClient();

    // The stream's nominal rate (audio_rate), kDefaultAudioRate until a
    // server has reported one. This is the rate to run the DSP at.
    int audioRate() const { return audioRateHz.load(); }
    // The true rate (sample_rate), audioRate() until reported.
    double sampleRate() const {
        const double sr = sampleRateHz.load();
        return sr > 0.0 ? sr : (double)audioRate();
    }
    Flavour flavour() const { return serverFlavour.load(); }
    // Hardware-converter offset (freq_offset), Hz; 0 until reported.
    int64_t frequencyOffsetHz() const { return serverFrequencyOffset.load(); }
    float rssiDbm() const { return rssi.load(); }
    // Packets missing from the seq count this session.
    uint64_t seqGaps() const { return seqGapCount.load(); }
    // A packet in the last second carried the ADC-overload flag.
    bool adcOverloadRecent() const;
    // Why the server refused the session, empty when it did not.
    std::string refusalReason() const;

    void init(const std::string& hostport);

    void tune(double freq, Modulation mod);

    // Apply new AGC settings. Stored regardless of connection state (so the
    // values persist across reconnects); sent to the server immediately if
    // currently connected. Out-of-range numeric inputs are clamped.
    void setAgc(AgcSettings s);

    // Coherent snapshot of the current AGC settings.
    AgcSettings getAgc() const;

    struct ServedRange {
        int64_t minHz;   // freq_offset
        int64_t maxHz;   // freq_offset + bandwidth
    };
    // Live tunable dial span derived from the server's reported freq_offset
    // and bandwidth, or nullopt until the server has reported its bandwidth.
    std::optional<ServedRange> getServedRange() const;

private:
    // Fire-and-forget command send. tune() and setAgc() run on the GUI
    // thread and can race the socket dying on a flaky link; a failed
    // command must not throw into the GUI thread (uncaught => terminate).
    // The receive loop notices the dead socket and tears the session down.
    bool trySend(const std::string& cmd);

    // Build the SET freq command, applying the server-reported hardware
    // offset. Used both by tune() and by the freq_offset MSG handler when it
    // re-issues the last user-requested tune.
    void sendTuneCommand(double freq, Modulation mod);

    void sendAgcLine();

    // Tokenize a "MSG key1=v1 key2=v2 ..." text frame from the binary channel
    // and dispatch each pair through onKeyValue. Values are URL-encoded; a
    // token without '=' ("MSG monitor") is a key with an empty value.
    void parseMsgFrame(const std::string& msg);

    void onKeyValue(const std::string& key, const std::string& value);

    // Receive thread: one parsed SND packet.
    void handlePacket(const SndPacket& packet);

    // Receive thread: the session is set up (sample_rate arrived, or the
    // server never sent the rates); send AGC, mark connected, onConnected.
    void goLive();

    // Receive thread: the server refused or ended the session.
    void refuse(const std::string& reason);

    void markCompatible(const char* why);

    static int64_t parseKhzToHz(const std::string& value);

    // Synchronously GET http://host:port/VER and return its 'ts' field.
    // KiwiSDR uses this value in the websocket path (/<ts>/SND) to pair a
    // client's connections; some servers reject paths whose timestamp
    // drifts too far from their own clock. Falls back to the local epoch ms
    // if the fetch or parse fails — local-clock ms is still unique enough
    // for session disambiguation.
    // origin receives the server's http(s)://host[:port] after redirects
    // (the scheme probe), or http://host:port when the fetch fails.
    uint64_t fetchServerTimestamp(const std::string& host, int port, std::string& origin);

    void resetSessionState();

public:
    void stop();
    void start();
};
