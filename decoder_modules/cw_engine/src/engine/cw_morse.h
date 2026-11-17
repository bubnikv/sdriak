/*
 * cw_morse.h — the CW decoder's character assembly: the Morse table, and the
 * character in flight as the lattice commits its elements.
 *
 * A code is a character's elements as bits under a leading 1, dit = 0 and
 * dah = 1: "A" (.-) is 0b101, "E" (.) 0b10. Up to 7 elements.
 *
 * SDRIAK: upstream keeps the table, morse_nearest() and the code/nelem pair
 * in the decoder state (decode_cw_v2/v3.c); this is that code as a stage,
 * same rules: the lone-E/T rule and SKIM_Vn_NEAREST in take().
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "decode_cw_config.h"

struct CwMorse {
    // The character of a code, 0 when none.
    static char decode(unsigned code);
    // A character's pattern (".-"), nullptr when it has none.
    static const char *pattern(unsigned char c);
    // SKIM_Vn_NEAREST: a code no character has, read as the one character a
    // single element away — first by dropping one element (a sloppy or
    // glitching key adds one: Z35F keyed its 5 with six dits on CQ WW 2012,
    // v2 dropped the char and read "Z3F"), else by swapping one dit/dah. Only
    // a UNIQUE neighbour counts; anything else is 0.
    static char nearest(unsigned code, unsigned nelem);
};

// The character in flight.
class CwAssembler {
public:
    // One committed element. An eighth one drops the character.
    void push(bool dah) {
        if (m_nelem < 7) {
            m_code = (m_nelem ? m_code : 1u) << 1 | (dah ? 1u : 0u);
            m_nelem++;
        } else {
            m_code = 0;
            m_nelem = 0;
        }
    }
    bool empty() const { return m_nelem == 0; }
    void clear() {
        m_code = 0;
        m_nelem = 0;
    }

    // The character in flight — 0 when there is none, it is no character, or
    // it is a lone E/T the clock does not vouch for (fewer than 4 marks, or
    // element error over 0.45: what random noise degenerates into) — and
    // clear. nearest = SKIM_Vn_NEAREST.
    char take(const CwAssemblyConfig &cfg, unsigned marks_committed, double timing_error) {
        char out = 0;
        if (m_nelem && m_code < 256) {
            char c = CwMorse::decode(m_code);
            if (!c && cfg.nearest) { c = CwMorse::nearest(m_code, m_nelem); }
            const bool lone_et = m_nelem == 1 && (marks_committed < 4 || timing_error > 0.45);
            if (c && !lone_et) { out = c; }
        }
        clear();
        return out;
    }

private:
    unsigned m_code = 0;
    unsigned m_nelem = 0;
};
