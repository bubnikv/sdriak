/*
 * cw_schmitt.h — v1's key detector: a Schmitt trigger on an envelope with a
 * 3-sample glitch fold, and the dit bootstrap that learns the clock from the
 * first marks it sees.
 *
 * SDRIAK: upstream runs this trigger twice inline in the decoder's sample
 * loop — the dit bootstrap (key/run/pend_*) and the run dump's keyer
 * (rd_*), the same code on the same thresholds. This is it once, with the
 * same operations in the same order. A confirmed run comes back through
 * step()'s out-parameter (upstream reads pend_key/pend_len after it).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

// Key on above lo + 0.55·(hi − lo), off below lo + 0.30·(hi − lo). A run
// shorter than kGlitch samples is folded back into the run before it.
class CwSchmitt {
public:
    static constexpr double kGlitch = 3.0;

    // A finished run: key down (a mark) or up, its length in samples.
    struct Run {
        bool   key = false;
        double len = 0.0;
    };

    // One sample x against the levels lo/hi. true: a run is confirmed, in
    // `run`; it stays pending until settle().
    bool step(double x, double lo, double hi, Run &run) {
        const double span = hi - lo;
        const double thr  = lo + (m_key ? 0.30 : 0.55) * span;
        const bool want = x > thr;
        if (want != m_key) {
            if (m_pend_valid) {
                m_run += m_pend_len;          // glitch — fold it back
                m_key  = want;
                m_pend_valid = false;
            } else {
                m_pend_key   = m_key;
                m_pend_len   = m_run;
                m_pend_valid = true;
                m_key = want;
                m_run = 0.0;
            }
        }
        m_run += 1.0;
        if (m_pend_valid && m_run > kGlitch) {
            run.key = m_pend_key;
            run.len = m_pend_len;
            return true;
        }
        return false;
    }

    void settle() { m_pend_valid = false; }

    // Dead air: no key, no run, nothing pending.
    void idle() {
        m_key = false;
        m_run = 0;
        m_pend_valid = false;
    }

private:
    bool   m_key = false;               // the key now
    double m_run = 0.0;                 // samples in the current run
    bool   m_pend_valid = false;        // a finished run waits to be confirmed
    bool   m_pend_key = false;          // … its key
    double m_pend_len = 0.0;            // … and length
};

// The dit bootstrap: v1's Schmitt + first-marks clustering. Until the
// lattice has a clock, the first kMarks mark runs teach it one: with dits
// and dahs among them (max ≥ 2·min), the mean of the short cluster; with
// kMarks of one kind, the shortest.
class CwBootstrap {
public:
    static constexpr unsigned kMarks = 8;

    // The trigger (see CwSchmitt::step).
    bool step(double x, double lo, double hi, CwSchmitt::Run &run) { return m_sch.step(x, lo, hi, run); }
    void settle() { m_sch.settle(); }
    void idle() { m_sch.idle(); }

    // The confirmed run is a mark long enough to count.
    static bool mark_ready(const CwSchmitt::Run &run) {
        return run.key && run.len >= CwSchmitt::kGlitch;
    }

    // Take a mark of len samples; returns the dit (samples, clamped to 4..55
    // WPM at rate) once the marks decide, `dit` as it was otherwise.
    double learn(double len, double rate, double dit) {
        if (m_nmarks < kMarks) { m_mark_lens[m_nmarks++] = len; }
        const auto lohi = std::minmax_element(m_mark_lens, m_mark_lens + m_nmarks);
        const double lo = *lohi.first, hi = *lohi.second;
        if (m_nmarks >= 4 && hi >= 2.0 * lo) {
            double mid = (lo + hi) / 2.0, sum = 0.0;
            unsigned cnt = 0;
            for (unsigned b = 0; b < m_nmarks; b++) {
                if (m_mark_lens[b] < mid) { sum += m_mark_lens[b]; cnt++; }
            }
            return std::clamp(sum / cnt, rate * 0.022, rate * 0.30);
        }
        if (m_nmarks == kMarks) {
            return std::clamp(lo, rate * 0.022, rate * 0.30);
        }
        return dit;
    }

    unsigned marks() const { return m_nmarks; }    // collected (0 = from scratch)
    void forget() { m_nmarks = 0; }

private:
    CwSchmitt m_sch;
    double    m_mark_lens[kMarks] = {};
    unsigned  m_nmarks = 0;
};
