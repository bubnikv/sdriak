/*
 * cw_fist.h — the operator's spacing: the learned char/word space centres.
 *
 * Keying is the rigid part of a fist, SPACING is the sloppy one — real
 * operators run char spaces anywhere from 2.5 to 5+ dits and word spaces 5–11,
 * so any fixed char/word boundary misreads somebody (live 2026-07-16:
 * EA1EYL's stretched CQ filed as "C Q" — two words, the CQ marker never
 * matched, the spot never fired). The decoder learns the operator's OWN two
 * space centres from the last kSpaceRing committed spaces: 2-means in the log
 * domain, seeded from the ring's QUANTILES — seeding from the model's own
 * labels would self-reinforce (a stretched char space filed as word space
 * teaches the word centre DOWN, never the char centre up; raw-duration
 * quantiles are label-free). An accepted fit (both clusters populated,
 * word/char ratio ≥ kMinWordToCharRatio) blends into the centres, which move
 * the duration priors, the lattice search windows and the live-emission
 * clocks together. The centres reset with the dit clock — fist forgotten,
 * spacing forgotten.
 *
 * SDRIAK: upstream's fist_* / gaps fields of the decoder state and its
 * fist_learn(), as a class; same arithmetic. Upstream's FIST_* constants are
 * its kName members: FIST_RING kSpaceRing, FIST_MIN_D kMinSpaceDits, FIST_REFIT
 * kRefitEvery, FIST_MIN_FIT kMinSpacesToFit, FIST_SEP kMinWordToCharRatio,
 * FIST_CSP_LO/_HI kCharSpaceMin/Max, FIST_WSP_LO/_HI kWordSpaceMin/Max,
 * FIST_GAIN kFitGain, FIST_CSP0/WSP0 kCharSpaceIdeal/kWordSpaceIdeal.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_debug.h"

class CwFist {
public:
    // The learned character space and word space centres, dits.
    double char_space() const { return m_char_space; }
    double word_space() const { return m_word_space; }

    // Feed one committed space (in dits) and, periodically, refit the two
    // space centres.
    void learn(double d_dits, const CwTrace &tr) {
        if (d_dits < kMinSpaceDits)
            return;
        m_spaces[m_space_head] = static_cast<float>(log(d_dits));
        m_space_head = (m_space_head + 1) % kSpaceRing;
        if (m_nspaces < kSpaceRing) { m_nspaces++; }
        if (++m_since_fit < kRefitEvery || m_nspaces < kMinSpacesToFit)
            return;
        m_since_fit = 0;

        float s[kSpaceRing];
        std::copy_n(m_spaces, m_nspaces, s);
        std::sort(s, s + m_nspaces);
        double c1 = s[m_nspaces / 4];          // label-free quantile seeds
        double c2 = s[(3 * m_nspaces) / 4];
        if (c2 - c1 < 1e-6)
            return;                           // unimodal pile — nothing to learn
        unsigned n1 = 0, n2 = 0;
        for (int it = 0; it < 3; it++) {
            double s1 = 0.0, s2 = 0.0;
            n1 = n2 = 0;
            for (unsigned i = 0; i < m_nspaces; i++) {
                if (fabs(s[i] - c1) <= fabs(s[i] - c2)) {
                    s1 += s[i];
                    n1++;
                } else {
                    s2 += s[i];
                    n2++;
                }
            }
            if (!n1 || !n2)
                return;
            c1 = s1 / n1;
            c2 = s2 / n2;
        }
        if (exp(c2 - c1) < kMinWordToCharRatio || n1 < 3 || n2 < 3)
            return;                           // clusters not credibly two
        const double c = std::clamp(exp(c1), kCharSpaceMin, kCharSpaceMax);
        m_char_space += kFitGain * (c - m_char_space);
        const double wlo = std::max(kWordSpaceMin, 1.6 * m_char_space);
        const double w = std::clamp(exp(c2), wlo, kWordSpaceMax);
        m_word_space += kFitGain * (w - m_word_space);
        CW_DBG(tr, "[fist csp=%.2f wsp=%.2f (fit %.2f/%.2f, %u+%u gaps)]\n",
                m_char_space, m_word_space, exp(c1), exp(c2), n1, n2);
    }

    // Forget the spacing style (with the dit clock). The ring's contents stay;
    // m_nspaces = 0 makes them unread.
    void reset() {
        m_char_space = kCharSpaceIdeal;
        m_word_space = kWordSpaceIdeal;
        m_nspaces = m_space_head = m_since_fit = 0;
    }

private:
    static constexpr unsigned kSpaceRing = 24;      // spaces remembered for the fit
    static constexpr double kMinSpaceDits = 1.6;    // shorter: an element space,
                                                    // not a char/word space
    static constexpr unsigned kRefitEvery = 4;      // refit every this many spaces
    static constexpr unsigned kMinSpacesToFit = 10; // spaces before the first fit
    static constexpr double kMinWordToCharRatio = 1.55;  // word/char centre ratio
                                                         // a fit must reach
    static constexpr double kCharSpaceMin = 2.4;  // the char space centre's range,
                                                  // dits
    static constexpr double kCharSpaceMax = 4.8;
    static constexpr double kWordSpaceMin = 5.0;  // … and the word space centre's
    static constexpr double kWordSpaceMax = 11.0;
    static constexpr double kFitGain = 0.25;      // blend of an accepted fit
    static constexpr double kCharSpaceIdeal = 3.0;  // the seeds: the ITU ideals
    static constexpr double kWordSpaceIdeal = 7.0;

    double   m_char_space = kCharSpaceIdeal;      // char space centre, dits
    double   m_word_space = kWordSpaceIdeal;      // word space centre, dits
    float    m_spaces[kSpaceRing] = {};           // recent spaces ≥ kMinSpaceDits,
                                                  // ln(dits)
    unsigned m_nspaces = 0;                       // entries in the ring
    unsigned m_space_head = 0;                    // the next write
    unsigned m_since_fit = 0;                     // spaces since the last fit
};
