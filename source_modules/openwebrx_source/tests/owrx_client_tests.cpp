// OpenWebRX client: both ADPCM framings (round trip against an IMA encoder
// written here, sync frames split across messages, a stray byte before the
// sync word, multi-row FFT messages), the config merge and profile-change
// detection, version and URL parsing.
//
//   owrx_client_tests                 unit tests, no network
//   owrx_client_tests --live <url>    one 30 s session on a receiver
#include "owrx_client.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    int failures = 0;
    void require(bool ok, const std::string& what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            failures++;
        }
    }

    // IMA encoder mirroring the decoder's state: the nibble is chosen with
    // the current step, then the state advances as the decoder's does.
    int encodeNibble(owrx::adpcm::Channel& ch, int sample) {
        int diff = sample - ch.predictor;
        int nib = 0;
        if (diff < 0) {
            nib = 8;
            diff = -diff;
        }
        int step = ch.step;
        for (int mask = 4; mask; mask >>= 1) {
            if (diff >= step) {
                nib |= mask;
                diff -= step;
            }
            step >>= 1;
        }
        ch.decode(nib);
        return nib;
    }

    void put16(std::vector<uint8_t>& m, int v) {
        m.push_back((uint8_t)(v & 0xFF));
        m.push_back((uint8_t)((v >> 8) & 0xFF));
    }

    // A stereo sync stream of `frames` frames; ref gets what the decoder
    // must produce for each channel.
    std::vector<uint8_t> stereoStream(int frames, std::vector<int16_t>& refA, std::vector<int16_t>& refB) {
        owrx::adpcm::Channel enc[2];
        enc[0].step = enc[1].step = owrx::adpcm::kStepTable[0];
        std::vector<uint8_t> out;
        int n = 0;
        for (int f = 0; f < frames; f++) {
            out.insert(out.end(), { 'S', 'Y', 'N', 'C' });
            put16(out, enc[0].index);
            put16(out, enc[0].predictor);
            put16(out, enc[1].index);
            put16(out, enc[1].predictor);
            for (int p = 0; p < owrx::StereoSyncAdpcm::kPairsPerFrame; p++) {
                uint8_t bytes[2];
                for (int c = 0; c < 2; c++) {
                    int nibs[2];
                    for (int h = 0; h < 2; h++) {
                        const double t = (double)(n + h);
                        const double x = c == 0 ? 9000.0 * std::cos(0.01 * t) : 9000.0 * std::sin(0.01 * t);
                        nibs[h] = encodeNibble(enc[c], (int)x);
                        (c == 0 ? refA : refB).push_back((int16_t)enc[c].predictor);
                    }
                    bytes[c] = (uint8_t)(nibs[0] | (nibs[1] << 4));
                }
                out.push_back(bytes[0]);
                out.push_back(bytes[1]);
                n += 2;
            }
        }
        return out;
    }

    void testStereoWhole() {
        std::vector<int16_t> refA, refB, a, b;
        const auto s = stereoStream(3, refA, refB);
        owrx::StereoSyncAdpcm dec;
        dec.decode(s.data(), s.size(), a, b);
        require(a == refA && b == refB, "stereo: whole stream decodes to the encoder's reconstruction");
        require(a.size() == (size_t)3 * 2 * owrx::StereoSyncAdpcm::kPairsPerFrame, "stereo: 2002 samples per channel and frame");
        // The reconstruction follows the input closely after a short warm-up.
        double maxErr = 0.0;
        for (size_t i = 200; i < a.size(); i++) {
            maxErr = std::max(maxErr, std::fabs((double)a[i] - 9000.0 * std::cos(0.01 * (double)i)));
        }
        require(maxErr < 600.0, "stereo: IMA reconstruction tracks a slow cosine");
    }

    void testStereoSplit() {
        std::vector<int16_t> refA, refB;
        const auto s = stereoStream(2, refA, refB);
        // Every split point class: inside the sync word, the header, a pair,
        // and between pairs.
        for (size_t cut : { (size_t)2, (size_t)7, (size_t)12, (size_t)13, (size_t)1000, (size_t)2013, s.size() - 1 }) {
            std::vector<int16_t> a, b;
            owrx::StereoSyncAdpcm dec;
            dec.decode(s.data(), cut, a, b);
            dec.decode(s.data() + cut, s.size() - cut, a, b);
            require(a == refA && b == refB, "stereo: split at " + std::to_string(cut));
        }
    }

    void testStereoResync() {
        std::vector<int16_t> refA, refB, a, b;
        auto s = stereoStream(2, refA, refB);
        // A stray byte, and a partial "SY", before the stream.
        std::vector<uint8_t> m = { 0x42, 'S', 'Y', 0x00 };
        m.insert(m.end(), s.begin(), s.end());
        owrx::StereoSyncAdpcm dec;
        dec.decode(m.data(), m.size(), a, b);
        require(a == refA && b == refB, "stereo: stray bytes before SYNC are skipped");
        // "SSYNC": a repeated first letter still finds the word.
        std::vector<uint8_t> m2 = { 'S' };
        m2.insert(m2.end(), s.begin(), s.end());
        a.clear();
        b.clear();
        owrx::StereoSyncAdpcm dec2;
        dec2.decode(m2.data(), m2.size(), a, b);
        require(a == refA && b == refB, "stereo: SSYNC resynchronises");
    }

    // One FFT row: kPad padding values, then the bins in centi-dB.
    std::vector<uint8_t> fftRow(const std::vector<float>& db, std::vector<float>& ref) {
        owrx::adpcm::Channel enc;
        std::vector<int> values(owrx::FftAdpcm::kPad, 0);
        for (float d : db) { values.push_back((int)std::lround(d * 100.0f)); }
        if (values.size() & 1) { values.push_back(0); }
        std::vector<uint8_t> out;
        for (size_t i = 0; i < values.size(); i += 2) {
            const int lo = encodeNibble(enc, values[i]);
            if (i >= (size_t)owrx::FftAdpcm::kPad && ref.size() < db.size()) { ref.push_back((float)enc.predictor / 100.0f); }
            const int hi = encodeNibble(enc, values[i + 1]);
            if (i + 1 >= (size_t)owrx::FftAdpcm::kPad && ref.size() < db.size()) { ref.push_back((float)enc.predictor / 100.0f); }
            out.push_back((uint8_t)(lo | (hi << 4)));
        }
        return out;
    }

    void testFft() {
        const int size = 64;
        owrx::OpenWebRxClient c;
        c.handleText(R"({"type":"config","value":{"fft_size":64,"fft_compression":"adpcm","audio_compression":"none"}})");
        c.handleText(R"({"type":"config","value":{"center_freq":7100000,"samp_rate":1024000,"sdr_id":"rtl","profile_id":"40m"}})");
        std::vector<std::vector<float>> got;
        c.onFftRow = [&got](const float* bins, int n) { got.emplace_back(bins, bins + n); };
        // Five rows back to back, as the first message after connecting holds.
        std::vector<uint8_t> msg = { 1 };
        std::vector<std::vector<float>> refs;
        for (int r = 0; r < 5; r++) {
            std::vector<float> db(size), ref;
            for (int k = 0; k < size; k++) { db[(size_t)k] = -100.0f + 10.0f * (float)r + 20.0f * std::sin(0.2f * (float)k); }
            const auto row = fftRow(db, ref);
            require(row.size() == owrx::FftAdpcm::rowBytes(size), "fft: row length (fft_size + 10) / 2");
            msg.insert(msg.end(), row.begin(), row.end());
            refs.push_back(ref);
        }
        c.handleBinary(msg.data(), msg.size());
        require(got.size() == 5, "fft: five rows from one message");
        for (size_t r = 0; r < got.size() && r < refs.size(); r++) {
            require(got[r] == refs[r], "fft: row " + std::to_string(r) + " decodes from a reset state");
            float maxErr = 0.0f;
            for (int k = 20; k < size; k++) {
                maxErr = std::max(maxErr, std::fabs(got[r][(size_t)k] - (-100.0f + 10.0f * (float)r + 20.0f * std::sin(0.2f * (float)k))));
            }
            require(maxErr < 2.0f, "fft: row " + std::to_string(r) + " within 2 dB of the input");
        }

        // float32 rows.
        owrx::OpenWebRxClient f;
        f.handleText(R"({"type":"config","value":{"fft_size":4,"fft_compression":"none","center_freq":1,"samp_rate":2}})");
        std::vector<float> rows = { -1, -2, -3, -4, -5, -6, -7, -8 };
        std::vector<uint8_t> fm = { 1 };
        fm.resize(1 + rows.size() * 4);
        std::memcpy(fm.data() + 1, rows.data(), rows.size() * 4);
        int n = 0;
        f.onFftRow = [&n](const float* bins, int count) { n += (count == 4 && bins[0] == (n == 0 ? -1.0f : -5.0f)) ? 1 : 100; };
        f.handleBinary(fm.data(), fm.size());
        require(n == 2, "fft: two float32 rows");
    }

    void testIqPlain() {
        owrx::OpenWebRxClient c;
        c.handleText(R"({"type":"config","value":{"audio_compression":"none"}})");
        std::vector<uint8_t> m = { 6 };
        put16(m, 16384);
        put16(m, -16384);
        put16(m, -32768);
        put16(m, 0);
        std::vector<dsp::complex_t> got;
        c.onIQ = [&got](const dsp::complex_t* s, int n) { got.insert(got.end(), s, s + n); };
        c.handleBinary(m.data(), m.size());
        require(got.size() == 2 && got[0].re == 0.5f && got[0].im == -0.5f && got[1].re == -1.0f && got[1].im == 0.0f,
                "iq: int16 I then Q, scaled by 1/32768");

        // ADPCM IQ across two messages.
        owrx::OpenWebRxClient a;
        a.handleText(R"({"type":"config","value":{"audio_compression":"adpcm"}})");
        std::vector<int16_t> refA, refB;
        const auto s = stereoStream(2, refA, refB);
        std::vector<dsp::complex_t> iq;
        a.onIQ = [&iq](const dsp::complex_t* p, int n) { iq.insert(iq.end(), p, p + n); };
        const size_t cut = 3001;
        std::vector<uint8_t> m1 = { 6 }, m2 = { 6 };
        m1.insert(m1.end(), s.begin(), s.begin() + (std::ptrdiff_t)cut);
        m2.insert(m2.end(), s.begin() + (std::ptrdiff_t)cut, s.end());
        a.handleBinary(m1.data(), m1.size());
        a.handleBinary(m2.data(), m2.size());
        bool ok = iq.size() == refA.size();
        for (size_t i = 0; ok && i < iq.size(); i++) {
            ok = iq[i].re == (float)refA[i] / 32768.0f && iq[i].im == (float)refB[i] / 32768.0f;
        }
        require(ok, "iq: ADPCM channel 1 is I, channel 2 is Q, state kept across messages");
    }

    void testConfigMerge() {
        owrx::OpenWebRxClient c;
        int calls = 0, changes = 0;
        owrx::OpenWebRxClient::Config last;
        c.onConfig = [&](const owrx::OpenWebRxClient::Config& cfg, bool changed) {
            calls++;
            changes += changed ? 1 : 0;
            last = cfg;
        };
        c.handleText("CLIENT DE SERVER server=openwebrx version=v1.2.131");
        require(c.serverVersion() == "v1.2.131", "handshake: version kept");
        c.handleText(R"({"type":"config","value":{"fft_size":4096,"fft_compression":"adpcm"}})");
        c.handleText(R"({"type":"config","value":{"waterfall_levels":{"min":-88.0,"max":-20.0}}})");
        require(calls == 0, "config: incomplete parts are not reported");
        c.handleText(R"({"type":"config","value":{"samp_rate":2000000,"center_freq":145000000,"profile_id":"2m","sdr_id":"rtl","start_mod":"nfm"}})");
        require(calls == 1 && changes == 0, "config: the first complete config is not a change");
        require(last.fftSize == 4096 && last.levelMin == -88.0f && last.levelMax == -20.0f && last.profileKey() == "rtl|2m",
                "config: parts merged");
        c.handleText(R"({"type":"config","value":{"waterfall_levels":{"min":-90.0,"max":-30.0}}})");
        require(calls == 2 && changes == 0, "config: levels alone are no profile change");
        c.handleText(R"({"type":"config","value":{"center_freq":7100000,"samp_rate":1024000,"profile_id":"40m"}})");
        require(changes == 1 && last.centerFreq == 7100000.0, "config: a new centre is a profile change");

        c.handleText(R"({"type":"modes","value":[{"modulation":"nfm","name":"FM"},{"modulation":"iq","name":"IQ"}]})");
        require(c.hasIqMode(), "modes: iq listed");
        c.handleText(R"({"type":"profiles","value":[{"id":"rtl|40m","name":"40 m"},{"id":"rtl|2m","name":"2 m"}]})");
        require(c.profiles().size() == 2 && c.profiles()[1].name == "2 m", "profiles parsed");
        c.handleText(R"({"type":"receiver_details","value":{"receiver_name":"X","receiver_location":"Y","session_timeout":3600}})");
        require(c.details().sessionTimeoutS == 3600 && c.details().name == "X", "receiver_details parsed");
        c.handleText(R"({"type":"clients","value":4})");
        require(c.clients() == 4, "clients parsed");
    }

    void testVersionAndUrl() {
        using C = owrx::OpenWebRxClient;
        require(C::versionSupportsIq("v1.2.129") && C::versionSupportsIq("v1.2.131") && C::versionSupportsIq("1.3.0"),
                "version: 1.2.129 and later");
        require(!C::versionSupportsIq("v1.2.128") && !C::versionSupportsIq("v1.2.2") && !C::versionSupportsIq("v1.1.0") &&
                    !C::versionSupportsIq(""),
                "version: earlier refused");
        require(C::socketUrl("https://host.example/") == "wss://host.example/ws/", "url: https");
        require(C::socketUrl("host.example:8073") == "ws://host.example:8073/ws/", "url: bare host:port");
        require(C::socketUrl("http://h:8073/#freq=7100000,mod=lsb") == "ws://h:8073/ws/", "url: fragment dropped");
        require(C::socketUrl("https://h/sdr/") == "wss://h/sdr/ws/", "url: path kept");
        require(C::socketUrl("wss://h/ws/") == "wss://h/ws/", "url: socket URL kept");
    }

    // One session: handshake, config, iq listed, IQ rate, FFT rows, close.
    int live(const std::string& url) {
        owrx::OpenWebRxClient c;
        std::atomic<bool> handshake{false}, config{false};
        std::atomic<uint64_t> samples{0}, rows{0}, badRows{0};
        std::atomic<int> fftSize{0};
        std::mutex errMtx;
        std::string error;
        c.onHandshake = [&] { handshake = true; };
        c.onConfig = [&](const owrx::OpenWebRxClient::Config& cfg, bool changed) {
            fftSize = cfg.fftSize;
            config = true;
            if (changed) { std::printf("profile changed: %.0f Hz, %.0f S/s\n", cfg.centerFreq, cfg.sampRate); }
        };
        // IQ level: the server's AGC and limiter put the peak near 0.8.
        std::atomic<float> peak{0.0f};
        std::atomic<double> power{0.0};
        c.onIQ = [&](const dsp::complex_t* s, int n) {
            float pk = peak.load();
            double pw = 0.0;
            for (int i = 0; i < n; i++) {
                pk = std::max(pk, std::max(std::fabs(s[i].re), std::fabs(s[i].im)));
                pw += (double)s[i].re * s[i].re + (double)s[i].im * s[i].im;
            }
            peak = pk;
            power = power.load() + pw;
            samples += (uint64_t)n;
        };
        c.onFftRow = [&](const float*, int n) {
            rows++;
            if (n != fftSize.load()) { badRows++; }
        };
        c.onError = [&](const std::string& e) {
            std::lock_guard lck(errMtx);
            error = e;
            std::printf("error: %s\n", e.c_str());
        };
        c.start(url);
        const auto t0 = std::chrono::steady_clock::now();
        while (!config.load() && c.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto cfg = c.config();
        std::printf("handshake %s, version %s, config %s: centre %.0f Hz, rate %.0f, fft %d, audio %s, fft %s\n",
                    handshake ? "ok" : "MISSING", c.serverVersion().c_str(), config ? "complete" : "MISSING", cfg.centerFreq,
                    cfg.sampRate, cfg.fftSize, cfg.audioCompression.c_str(), cfg.fftCompression.c_str());
        // Modes and profiles follow the config.
        const auto t1 = std::chrono::steady_clock::now();
        while ((c.modes().empty() || c.profiles().empty()) && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        std::printf("after %.1f s: %zu modes (iq %s), %zu profiles, %d clients, supported version %s\n", waited, c.modes().size(),
                    c.hasIqMode() ? "listed" : "MISSING", c.profiles().size(), c.clients(),
                    owrx::OpenWebRxClient::versionSupportsIq(c.serverVersion()) ? "yes" : "no");
        if (!config.load() || !c.hasIqMode()) {
            c.stop();
            return 1;
        }
        c.startIq(0.0);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const uint64_t s0 = samples.load();
        const auto m0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(28));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - m0).count();
        const double rate = (double)(samples.load() - s0) / secs;
        std::printf("IQ %.0f S/s by arrival, %llu FFT rows (%.1f/s), %llu of the wrong size, still connected: %s\n", rate,
                    (unsigned long long)rows.load(), (double)rows.load() / 30.0, (unsigned long long)badRows.load(),
                    c.connected() ? "yes" : "no");
        std::printf("IQ RMS %.3f, peak %.2f\n", std::sqrt(power.load() / (double)std::max<uint64_t>(1, samples.load())), peak.load());
        c.stop();
        std::printf("closed\n");
        return (rate > 0.95 * 48000.0 && rows.load() > 0 && badRows.load() == 0) ? 0 : 1;
    }
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--live") { return live(argv[2]); }
    testStereoWhole();
    testStereoSplit();
    testStereoResync();
    testFft();
    testIqPlain();
    testConfigMerge();
    testVersionAndUrl();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("owrx_client_tests: all passed\n");
    return 0;
}
