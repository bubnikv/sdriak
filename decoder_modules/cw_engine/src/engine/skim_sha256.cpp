/*
 * skim_sha256.cpp — see skim_sha256.h. Part of SDRIAK. GPL-3.0-or-later.
 */
#include "skim_sha256.h"

#include <filesystem>
#include <fstream>

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

void SkimSha256::reset() {
    static constexpr uint32_t kInit[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::copy(std::begin(kInit), std::end(kInit), m_h);
    m_fill = 0;
    m_bytes = 0;
}

void SkimSha256::block(const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
               (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3];
    uint32_t e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d;
    m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
}

void SkimSha256::update(const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    m_bytes += len;
    if (m_fill) {
        const size_t take = std::min(len, sizeof(m_buf) - m_fill);
        std::memcpy(m_buf + m_fill, p, take);
        m_fill += take;
        p += take;
        len -= take;
        if (m_fill < sizeof(m_buf))
            return;
        block(m_buf);
        m_fill = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        block(p);
    if (len) {
        std::memcpy(m_buf, p, len);
        m_fill = len;
    }
}

SkimSha256::Digest SkimSha256::finish() {
    const uint64_t bits = m_bytes * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (m_fill != 56)
        update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    Digest d;
    for (int i = 0; i < 8; i++) {
        d[4 * i]     = static_cast<uint8_t>(m_h[i] >> 24);
        d[4 * i + 1] = static_cast<uint8_t>(m_h[i] >> 16);
        d[4 * i + 2] = static_cast<uint8_t>(m_h[i] >> 8);
        d[4 * i + 3] = static_cast<uint8_t>(m_h[i]);
    }
    reset();
    return d;
}

std::string SkimSha256::hex(const Digest &d) {
    static const char kHex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < d.size(); i++) {
        s[2 * i] = kHex[d[i] >> 4];
        s[2 * i + 1] = kHex[d[i] & 15];
    }
    return s;
}

std::string SkimSha256::of(std::string_view data) {
    SkimSha256 h;
    h.update(data.data(), data.size());
    return hex(h.finish());
}

std::optional<std::string> SkimSha256::of_file(const std::string &path, std::string *error) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        if (error)
            *error = "cannot read " + path;
        return std::nullopt;
    }
    SkimSha256 h;
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n > 0)
            h.update(buf.data(), static_cast<size_t>(n));
    }
    if (f.bad()) {
        if (error)
            *error = "read error on " + path;
        return std::nullopt;
    }
    return hex(h.finish());
}
