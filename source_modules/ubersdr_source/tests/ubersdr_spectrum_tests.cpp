// UberSpectrumClient's v2 frame parser (full and delta frames, the N/2
// rotation out of FFT order, a broken delta chain after a seq gap) and core's
// gunzip, which the spectrum socket's control replies need.
#include "ubersdr_client.h"
#include <utils/gzip.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
    int failures = 0;
    void require(bool ok, const std::string& what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            failures++;
        }
    }

    void put16(std::vector<uint8_t>& m, uint16_t v) {
        m.push_back((uint8_t)v);
        m.push_back((uint8_t)(v >> 8));
    }
    void put64(std::vector<uint8_t>& m, uint64_t v) {
        for (int i = 0; i < 8; i++) { m.push_back((uint8_t)(v >> (8 * i))); }
    }

    std::vector<uint8_t> header(uint8_t flags, uint16_t seq, uint64_t ns, uint64_t centre) {
        std::vector<uint8_t> m = { 'S', 'P', 'E', 'C', 2, flags };
        put16(m, seq);
        put64(m, ns);
        put64(m, centre);
        return m;
    }

    // Full frame, codes 0..n-1 in wire (FFT) order, ref -150.00 dB, step 0.50 dB.
    std::vector<uint8_t> fullFrame(uint16_t seq, int n) {
        auto m = header(0x05, seq, 1000, 7000000);
        put16(m, (uint16_t)(int16_t)-15000);
        m.push_back(50);
        for (int i = 0; i < n; i++) { m.push_back((uint8_t)i); }
        return m;
    }

    // Delta: wire bin `bin` gets `code`.
    std::vector<uint8_t> deltaFrame(uint16_t seq, int n, int bin, uint8_t code) {
        auto m = header(0x06, seq, 2000, 7000000);
        std::vector<uint8_t> mask((size_t)(n + 7) / 8, 0);
        mask[(size_t)bin >> 3] |= (uint8_t)(1u << (bin & 7));
        m.insert(m.end(), mask.begin(), mask.end());
        m.push_back(code);
        return m;
    }

    bool approxEqual(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}

int main() {
    ubersdr::UberSpectrumClient c;
    ubersdr::UberSpectrumClient::Frame f;
    constexpr int n = 16;

    auto full = fullFrame(10, n);
    require(c.parseFrame(full.data(), full.size(), f), "full frame parsed");
    require(f.count == n && f.seq == 10 && f.timestampNanos == 1000 && f.centreHz == 7000000.0, "full frame header");
    // Rotated: display bin 0 is wire bin n/2 (the most negative frequency),
    // display bin n/2 is wire bin 0 (DC).
    require(approxEqual(f.binsDb[0], -150.0f + 0.5f * (n / 2)), "rotation: lowest bin");
    require(approxEqual(f.binsDb[n / 2], -150.0f), "rotation: DC at n/2");
    require(approxEqual(f.binsDb[n - 1], -150.0f + 0.5f * (n / 2 - 1)), "rotation: highest bin");

    auto delta = deltaFrame(11, n, 0, 100);
    require(c.parseFrame(delta.data(), delta.size(), f), "delta frame parsed");
    require(approxEqual(f.binsDb[n / 2], -150.0f + 50.0f), "delta updates the DC bin");
    require(approxEqual(f.binsDb[0], -150.0f + 0.5f * (n / 2)), "delta keeps the other bins");

    // seq 12 lost: the chain is broken until a full frame.
    auto late = deltaFrame(13, n, 1, 7);
    require(!c.parseFrame(late.data(), late.size(), f), "delta after a gap rejected");
    auto again = deltaFrame(14, n, 1, 7);
    require(!c.parseFrame(again.data(), again.size(), f), "chain stays broken until a full frame");
    auto full2 = fullFrame(15, n);
    require(c.parseFrame(full2.data(), full2.size(), f), "full frame heals the chain");

    std::vector<uint8_t> junk = { 'S', 'P', 'E', 'C', 1, 5 };
    require(!c.parseFrame(junk.data(), junk.size(), f), "short or v1 frame rejected");

    // gzip -c of {"type":"config","binCount":1024}, mtime 0.
    const uint8_t gz[] = { 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xab, 0x56, 0x2a, 0xa9, 0x2c, 0x48,
                           0x55, 0xb2, 0x52, 0x4a, 0xce, 0xcf, 0x4b, 0xcb, 0x4c, 0x57, 0xd2, 0x51, 0x4a, 0xca, 0xcc, 0x73,
                           0xce, 0x2f, 0xcd, 0x2b, 0x51, 0xb2, 0x32, 0x34, 0x30, 0x32, 0xa9, 0x05, 0x00, 0x97, 0x34, 0xef,
                           0x7e, 0x21, 0x00, 0x00, 0x00 };
    std::string text;
    require(gzip::isGzip(gz, sizeof gz), "gzip magic");
    require(gzip::gunzip(gz, sizeof gz, text) && text == "{\"type\":\"config\",\"binCount\":1024}", "gunzip: '" + text + "'");
    require(!gzip::gunzip(gz, sizeof gz - 10, text) && text.empty(), "truncated gzip rejected");
    require(!gzip::gunzip(gz, sizeof gz, text, 8), "size limit");

    if (failures == 0) { std::printf("ubersdr_spectrum_tests: all passed\n"); }
    return failures == 0 ? 0 : 1;
}
