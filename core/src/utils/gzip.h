#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace gzip {
    // Data starts with the gzip magic (1f 8b).
    bool isGzip(const void* data, size_t len);

    // Inflate a gzip (or zlib) stream into out. False, with out cleared, on
    // a corrupt or truncated stream or one that would exceed maxOut bytes.
    bool gunzip(const void* data, size_t len, std::string& out, size_t maxOut = 4 * 1024 * 1024);
}
