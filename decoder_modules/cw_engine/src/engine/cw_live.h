/*
 * cw_live.h — live emission (v1's 2.2/5.5-dit rule, model-driven).
 *
 * A space in progress on the best path emits the finished char/word
 * WITHOUT waiting for the closing mark — the last char of an over has no
 * closing mark. The live clocks ride the fist centres: the char fires at
 * 0.867·char_space (the old 2.6 dits at the 3.0 seed), the word at
 * 1.2·√(char_space·word_space) (the old 5.5 at the 3/7 seeds) — a stretched
 * fist gets slower live text instead of premature word splits. The class
 * remembers which space boundary fired, so its eventual CharSpace/WordSpace
 * commit does not repeat it.
 *
 * SDRIAK: upstream's char_live/word_live/live_had_char/live_t fields of the
 * decoder state and the thresholds of soft_step()'s live block, as a stage.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_fist.h"

class CwLive {
public:
    // A new space will come: nothing has fired for it.
    void clear() { m_char_live = m_word_live = false; }

    // A fresh lattice.
    void restart() {
        m_char_live = m_word_live = false;
        m_had_char = false;
    }

    // The committed space starting at start already went out live.
    bool lived(uint64_t start) const { return m_char_live && start == m_t; }

    // sp samples of space at clock dit: the char in flight is done.
    bool char_due(double sp, const CwFist &fist, double dit) const {
        return !m_char_live && sp > 0.867 * fist.char_space() * dit;
    }
    void fire_char(uint64_t tau, bool had) {
        m_char_live = true;
        m_t         = tau;
        m_had_char  = had;
    }

    // … and the word with it.
    bool word_due(double sp, const CwFist &fist, double dit) const {
        return m_char_live && m_had_char && !m_word_live &&
               sp > 1.2 * sqrt(fist.char_space() * fist.word_space()) * dit;
    }
    void fire_word() { m_word_live = true; }

    bool had_char() const { return m_had_char; }    // the live char held elements
    bool word_live() const { return m_word_live; }  // the word space went out too

private:
    bool     m_char_live = false;         // the char in flight went out live
    bool     m_word_live = false;         // … and the word space after it
    bool     m_had_char = false;          // the live char held elements
    uint64_t m_t = 0;                     // the space boundary that fired
};
