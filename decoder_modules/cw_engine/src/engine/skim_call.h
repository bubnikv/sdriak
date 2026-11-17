/*
 * skim_call.h — a callsign (or a call-like word) as a value.
 *
 * Up to kMaxLength characters inline and NUL-terminated, no heap: the engine
 * keeps calls by the dozen in fixed arrays (candidates, near-call lists, hit
 * memory) and copies them freely. Assigning a longer string truncates it,
 * as upstream's g_strlcpy into a 16-byte buffer did.
 *
 * SDRIAK: upstream passes char[16] buffers (SKIM_CALLSIGN_NEAR_LEN,
 * CALL_BUF_LEN) and compares and copies them with strcmp()/g_strlcpy().
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

class SkimCall {
public:
    static constexpr size_t kCapacity = 16;               // bytes, terminator included
    static constexpr size_t kMaxLength = kCapacity - 1;

    SkimCall() = default;
    explicit SkimCall(std::string_view s) { assign(s); }

    // s fits without truncation.
    static bool fits(std::string_view s) { return s.size() <= kMaxLength; }

    // s, truncated to kMaxLength characters.
    void assign(std::string_view s) {
        const size_t n = std::min(s.size(), kMaxLength);
        std::memcpy(m_s, s.data(), n);
        m_s[n] = '\0';
    }
    // One more character; false (and the call unchanged) when it is full.
    bool push_back(char c) {
        const size_t n = size();
        if (n >= kMaxLength)
            return false;
        m_s[n] = c;
        m_s[n + 1] = '\0';
        return true;
    }
    void clear() { m_s[0] = '\0'; }

    const char *c_str() const { return m_s; }
    std::string_view view() const { return std::string_view(m_s, size()); }
    size_t size() const { return std::strlen(m_s); }
    bool empty() const { return m_s[0] == '\0'; }
    char operator[](size_t i) const { return m_s[i]; }

    friend bool operator==(const SkimCall &a, const SkimCall &b) { return a.view() == b.view(); }
    friend bool operator!=(const SkimCall &a, const SkimCall &b) { return !(a == b); }
    friend bool operator==(const SkimCall &a, std::string_view b) { return a.view() == b; }
    friend bool operator!=(const SkimCall &a, std::string_view b) { return !(a == b); }
    friend bool operator==(const SkimCall &a, const char *b) { return a.view() == b; }
    friend bool operator!=(const SkimCall &a, const char *b) { return !(a == b); }

private:
    char m_s[kCapacity] = {};
};
