/*
 * skim_sha256.h — SHA-256 (FIPS 180-4), for pinning model files.
 *
 * SDRIAK's: upstream (skimmer-for-linux headless/deepcw_model.c) checks the
 * DeepCW model with GLib's g_compute_checksum_for_data(G_CHECKSUM_SHA256);
 * the engine has no GLib, so this is a small self-contained implementation.
 * Compiled only with OPT_BUILD_CW_DEEPCW.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

#include <array>

class SkimSha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    SkimSha256() { reset(); }
    void reset();
    void update(const void *data, size_t len);
    Digest finish();                    // the object is reset afterwards

    // Lowercase hex of a digest.
    static std::string hex(const Digest &d);
    // Hex digest of a whole string / file (UTF-8 path); nullopt + *error when
    // the file cannot be read.
    static std::string of(std::string_view data);
    static std::optional<std::string> of_file(const std::string &path, std::string *error = nullptr);

private:
    void block(const uint8_t *p);

    uint32_t m_h[8];
    uint8_t  m_buf[64];
    size_t   m_fill = 0;
    uint64_t m_bytes = 0;
};
