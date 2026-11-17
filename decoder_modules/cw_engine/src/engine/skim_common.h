/*
 * skim_common.h — the standard headers the engine is written against.
 *
 * SDRIAK: upstream's engine files include <glib.h>; here the engine is
 * plain C++17 on the standard library, and this header is what they include
 * in its place: the standard headers they share, and π (G_PI upstream).
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

constexpr double SKIM_PI = 3.1415926535897932384626433832795028841971693993751;

// BSD strlcpy/strlcat (g_strlcpy/g_strlcat upstream): copy/append into a
// fixed buffer of dst_size bytes, always NUL-terminated when dst_size > 0,
// truncating; return the length the result would have had untruncated.
inline size_t skim_strlcpy(char *dst, const char *src, size_t dst_size) {
    const size_t n = std::strlen(src);
    if (dst_size) {
        const size_t c = n < dst_size - 1 ? n : dst_size - 1;
        std::memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

inline size_t skim_strlcat(char *dst, const char *src, size_t dst_size) {
    size_t d = 0;
    while (d < dst_size && dst[d]) { d++; }
    const size_t n = std::strlen(src);
    if (d == dst_size)
        return d + n;
    const size_t c = n < dst_size - d - 1 ? n : dst_size - d - 1;
    std::memcpy(dst + d, src, c);
    dst[d + c] = '\0';
    return d + n;
}
