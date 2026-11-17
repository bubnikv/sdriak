/*
 * cw_reread.h — SKIM_V3_RESCORE: the finished word re-read against the
 * dictionary.
 *
 * Live air against CW Skimmer Server (skimmer-compare, 2026-10-15): of the
 * stations R spotted and L missed, a quarter were MISREAD — a token one or
 * two edits from the call sat in L's text — and two thirds of L's busts
 * were calls cut short. The lattice decides every element on its own and
 * the extractor only ever sees the letters; the evidence that one more dah
 * was there, or that "EAAOY" fits a 6 better than an A, dies in between.
 *
 * So when a word closes, the calls MASTER.SCP holds within two edits of it
 * are laid onto the very samples the word came from: each call's exact
 * element sequence, aligned by the lattice's own semi-Markov search (the
 * same llr ring, duration priors and windows, a free space before and
 * after), against the best FREE reading of the same span. A call that fits
 * nearly as well as the free reading (≤ max_deficit) and clearly better
 * than every other reading, the word as read included (≥ min_margin),
 * replaces the word — for the extractor alone (callsign.h FIX): the pane
 * and the log keep what was heard, the feed gate judges the corrected read
 * like any other. Only dictionary calls are ever written in: an unknown call
 * is never invented.
 *
 * SKIM_V3_RESCORE="fit,sep,ev" = max_deficit, min_margin, min_evidence
 * (log-likelihood units, as the lattice scores; min_evidence per sample;
 * unset or "1" = the defaults, on since v0.6.0; "0" = off).
 * SKIM_V3_RESCORE_DEBUG=1 prints every re-read.
 *
 * SDRIAK: upstream's wd_* / oc_* fields of the decoder state, rs_forced(),
 * rs_free(), rs_reread(), oc_span(), oc_word() and the state half of
 * word_close(), as a stage (cw_reread.cpp); the bodies read the decoder
 * through a CwReadView. Upstream's wd_* fields are m_word*, oc_* m_oracle_*,
 * oc_span()/oc_word() oracle_span()/oracle_word().
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_align.h"
#include "skim_call.h"

struct SkimOracleHit;

class CwWordReread {
public:
    static constexpr auto kLeadDits = 2.0;     // free space searched before a word

    // No word in flight.
    bool empty() const { return m_word.empty(); }
    // The word in flight starts at sample t (its first mark).
    void set_start(uint64_t t) { m_word_t0 = t; }

    // A fresh lattice at sample t: no word in flight.
    void start(uint64_t t) {
        m_word_prev = t;
        m_word.clear();
        m_word_over = false;
    }

    // One decoded character (not a space) whose audio ended at t_end.
    void push_char(char c, uint64_t t_end) {
        if (!m_word.push_back(c)) { m_word_over = true; }
        m_word_t1 = t_end;
    }

    // The word in flight is done (its evidence ends at B): re-read it — true
    // with the dictionary call that replaces it in alt — and start the next
    // word.
    bool close(const CwReadView &v, const CwRescoreConfig &cfg, uint64_t B, SkimCall &alt) {
        bool fix = false;
        if (!m_word.empty()) {
            oracle_word(v, B);
            if (!m_word_over) {
                m_oracle_prev = m_word;
                m_oracle_prev_t0 = m_word_t0;
            } else {
                m_oracle_prev.clear();
            }
            fix = reread(v, cfg, B, alt);
            m_word_prev = m_word_t1;
        }
        m_word.clear();
        m_word_over = false;
        return fix;
    }

private:
    // Re-read the word that just closed; true with the call in `alt` when a
    // dictionary call replaces it.
    bool reread(const CwReadView &v, const CwRescoreConfig &cfg, uint64_t B, SkimCall &alt);

    // Best score of seg[0..K) laid on (A, A+N]: a free leading space, the
    // sequence, a free trailing space. f/g: scratch, N + 1 floats each.
    double forced(const CwReadView &v, uint64_t A, unsigned N, const CwSeg *seg,
                  unsigned K, float *f, float *g);
    // The best reading of (A, A+N] as ONE word — the bound every forced
    // reading of the span stays under.
    double free_reading(const CwReadView &v, uint64_t A, unsigned N, float *lead,
                        float *gm, float *gs);

    // SKIM_ORACLE_MODE=combine — measurement only (cw_reread.cpp).
    void oracle_span(const CwReadView &v, const SkimOracleHit &oh, uint64_t A,
                 uint64_t B, const char *word, int k);
    void oracle_word(const CwReadView &v, uint64_t B);

private:
    SkimCall m_word;                      // chars emitted since the last gap
    bool     m_word_over = false;         // longer than m_word holds: no
                                          // re-read
    uint64_t m_word_t0 = 0;               // start of its first mark (samples)
    uint64_t m_word_t1 = 0;               // end of its last mark
    uint64_t m_word_prev = 0;             // end of the previous word's marks
    uint64_t m_oracle_prev_t0 = 0;        // SKIM_ORACLE_MODE=combine: the word
                                          // before, for torn calls
    SkimCall m_oracle_prev;
    bool     m_oracle_loose = false;      // re-read spans: a character gap may
                                          // run word-gap long (combine only)

};
