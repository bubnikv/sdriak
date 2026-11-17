/*
 * cw_clock.h — the element clock: the dit length, its per-mark EMA and the
 * re-lock that jumps it on a speed change.
 *
 * The per-mark EMA glides — a QSO turnaround does not (live 40 m,
 * 2026-07-18: the OTHER op comes back inside the 8 s fist-forget window at
 * his own speed, and the whole first word rides a stale clock; worse, past
 * the 2-dit class boundary the misread marks pull the EMA the WRONG way,
 * self-consistently). So keep a ring of the last raw mark durations and
 * re-run the bootstrap's own clustering over it on every commit: a BIMODAL
 * ring whose low cluster disagrees with the clock by >30 % is a new speed,
 * proven structurally — JUMP, do not glide. A unimodal ring is ambiguous
 * (dit-only text reads clean) and may only jump while the clock visibly
 * fails (timing_error), interpreted as dits — the dah-streak watchdog remains
 * the backstop for the all-dah read. Gate: cw-test "QSO turnaround".
 *
 * SDRIAK: upstream's dit/esp/elem_err/streak/clk_* fields of the decoder
 * state, clk_est(), clock_push(), clock_space() and the DIT/DAH/ESP clock
 * updates of lattice_commit(), as a stage; same arithmetic, same order.
 * The fields are renamed for what they hold (README, "Renamed").
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_debug.h"
#include "decode_cw_config.h"

class CwClock {
public:
    static constexpr auto kRelockRing = 6;     // raw mark/space durations kept for
                                               // the re-lock
    static constexpr auto kRelockHold = 3;     // marks between two clock jumps

    // The unbiased dit, (dit + element space)/2 once the spaces are measured.
    double unbiased_dit() const {
        return m_element_space > 0 ? 0.5 * (m_dit + m_element_space) : m_dit;
    }

    // Empty both re-lock rings (and drop the hold).
    void clear_rings() {
        m_mark_ring_n = m_space_ring_n = m_mark_ring_head = m_space_ring_head = m_relock_hold = 0;
    }

    // A committed mark of d samples (dah: the 3-dit class). ms_db: the
    // discriminator's mark/space level ratio; relock_db (> 0): below it a
    // re-lock to a FASTER clock is refused (SKIM_V3_RELOCK_DB).
    void mark(bool dah, double d, double rate, const CwClockConfig &cfg, double ms_db,
              const CwTrace &tr) {
        const double ideal = (dah ? 3.0 : 1.0) * m_dit;
        m_timing_error += 0.1 * (fabs(d - ideal) / ideal - m_timing_error);
        // Track the clock harder while it is visibly wrong (speed change) —
        // commits arrive a lag behind, so the leisurely steady-state gain
        // would still be converging when the over ends.
        const double a = m_timing_error > 0.20 ? 0.30 : 0.15;
        m_dit += a * ((dah ? d / 3.0 : d) - m_dit);
        m_dit  = std::clamp(m_dit, rate * 0.022, rate * 0.30);
        relock(d, rate, cfg.relock_db, ms_db, tr);
        m_dah_streak = dah ? m_dah_streak + 1 : 0;
        m_dit_streak = dah ? 0 : m_dit_streak + 1;
    }

    // A committed inter-element space of d samples.
    void learn_element_space(double d) {
        m_element_space = m_element_space > 0 ? m_element_space + 0.2 * (d - m_element_space) : d;
        learn_space(d);
    }

    // A committed space of d samples into the witness ring.
    void learn_space(double d) {
        m_space_ring[m_space_ring_head++ % kRelockRing] = static_cast<float>(d);
        if (m_space_ring_n < kRelockRing) { m_space_ring_n++; }
    }

    double   dit() const { return m_dit; }                      // samples per dit;
                                                                // 0 = unlearned
    double   element_space() const { return m_element_space; }  // inter-element
                                                                // space EMA
    double   timing_error() const { return m_timing_error; }    // EMA of
                                                                // |d − ideal|/ideal
    unsigned dah_streak() const { return m_dah_streak; }        // dahs since the last dit
    unsigned dit_streak() const { return m_dit_streak; }        // dits since the last dah

    // The bootstrap's dit (CwBootstrap::learn).
    void set_dit(double dit) { m_dit = dit; }
    // A clock from elsewhere (the oracle's WPM): dit and spaces.
    void impose(double dit) { m_dit = m_element_space = dit; }
    // No clock: the fist forgotten, or the clock lost.
    void forget_dit() { m_dit = 0; }
    // The one-class streaks start over (the clock-lost watchdog).
    void reset_streaks() { m_dah_streak = m_dit_streak = 0; }
    // The space EMA was reading glue, not a fist.
    void drop_element_space() { m_element_space = 0.0; }

private:
    double m_dit = 0;
    double m_element_space = 0;           // The soft edges bias marks LONG and
                                          // spaces SHORT by the same amount;
                                          // (dit + element space)/2 is the
                                          // unbiased dit the reported WPM
                                          // comes from
    double m_timing_error = 0.2;          // neutral until measured
    unsigned m_dah_streak = 0;
    unsigned m_dit_streak = 0;
    float    m_mark_ring[kRelockRing] = {};   // last raw mark durations (samples)
    unsigned m_mark_ring_n = 0, m_mark_ring_head = 0;
    unsigned m_relock_hold = 0;               // marks until the next jump may come
    float    m_space_ring[kRelockRing] = {};  // last raw space durations (witness)
    unsigned m_space_ring_n = 0, m_space_ring_head = 0;

    // Robust cluster mean: tight (max ≤ 1.5×min) = trustworthy speed evidence;
    // one outlier (a torn-dah fragment) may be shed and the rest retried —
    // looser than that is a FIST, not a speed (σ0.2 jitter false-jumped the
    // pane gate), and yields no estimate.
    static bool est_cluster(const float *v, unsigned n, double *est) {
        float s[kRelockRing];
        std::copy_n(v, n, s);
        std::sort(s, s + n);
        unsigned m = n;
        if (s[m - 1] > 1.5f * s[0]) {
            if (n < 4 || s[n - 2] > 1.5f * s[0])
                return false;
            m = n - 1;                         // shed the lone outlier
        }
        *est = std::accumulate(s, s + m, 0.0) / m;
        return true;
    }

    // Push the mark into the ring and jump the clock if the ring proves a new
    // speed (upstream: clock_push).
    void relock(double d, double rate, double relock_db, double ms_db, const CwTrace &tr) {
        m_mark_ring[m_mark_ring_head++ % kRelockRing] = static_cast<float>(d);
        if (m_mark_ring_n < kRelockRing) { m_mark_ring_n++; }
        if (m_relock_hold) { m_relock_hold--; }
        if (m_mark_ring_n < 4 || m_relock_hold || m_dit <= 0)
            return;                   // 4 marks + the space witness already decide
        const auto lohi = std::minmax_element(m_mark_ring, m_mark_ring + m_mark_ring_n);
        const float lo = *lohi.first, hi = *lohi.second;
        double est;
        if (hi >= 2.0f * lo) {                  // dits AND dahs in the ring
            const float mid = 0.5f * (lo + hi);
            float cl[kRelockRing];
            unsigned cnt = 0;
            for (unsigned i = 0; i < m_mark_ring_n; i++) {
                if (m_mark_ring[i] < mid) { cl[cnt++] = m_mark_ring[i]; }
            }
            if (cnt < 3 || !est_cluster(cl, cnt, &est))
                return;
        } else {
            if (!est_cluster(m_mark_ring, m_mark_ring_n, &est))
                return;
            // A unimodal ring is dits-or-dahs ambiguous, and the nasty turnaround
            // reads the new op's dits as PERFECT dahs (timing_error never rises — the
            // live 40 m trap). The SPACES break the tie: element gaps run 1:1 with
            // dits but 1:3 with dahs, so the smallest recent space is the witness.
            if (m_space_ring_n >= 3) {
                // The ELEMENT-GAP class of the space ring: sort, then the smallest
                // class with ≥3 members within 1.5× of its floor. Not the min (a
                // torn dah drops glitch pairs under the real gaps) and not the
                // median (a dah-heavy stretch — "MM DE UB7M" — holds more char gaps
                // than element gaps, the median flips class and the witness jumped
                // a clean 24 WPM clock 3× up; live-replay-caught).
                float sp[kRelockRing];
                std::copy_n(m_space_ring, m_space_ring_n, sp);
                std::sort(sp, sp + m_space_ring_n);
                float witness_space = 0.0f;
                for (unsigned i = 0; i + 2 < m_space_ring_n; i++) {
                    if (sp[i + 2] <= 1.5f * sp[i]) { // three members = a real class
                        double s = 0.0;
                        unsigned c = 0;
                        for (unsigned j = i; j < m_space_ring_n && sp[j] <= 1.5f * sp[i]; j++) {
                            s += sp[j];
                            c++;
                        }
                        witness_space = static_cast<float>(s / c);
                        break;
                    }
                }
                if (witness_space <= 0.0f)
                    return;
                const double r = est / witness_space;
                if (r > 2.2) {
                    est /= 3.0;                       // dahs riding element gaps
                } else if (r >= 1.6) {
                    return;                           // between classes — no verdict
                }
            } else if (m_timing_error <= 0.30) {
                return;
            }
        }
        if (est > 0.80 * m_dit && est < 1.25 * m_dit)
            return;                               // the clock is already right
        // SKIM_V3_RELOCK_DB=<dB>: on a weak signal noise chops marks in two, and
        // the halves make a perfect "faster" cluster — SM7ZFB at 16 WPM and
        // 11 dB re-locked from 17.4 to 12.0 samples and read TMEE EETE ever
        // after (recording 2026-10-16). Below this mark/space level ratio a
        // jump to a FASTER clock is refused; slower stays allowed. Off unless
        // set.
        if (relock_db > 0 && est < m_dit && ms_db < relock_db) {
            CW_DBG(tr, "[clock relock %.1f -> %.1f refused (%.1f dB)]\n", m_dit, est, ms_db);
            return;
        }
        CW_DBG(tr, "[clock relock %.1f -> %.1f (err %.2f, %.1f dB)]\n", m_dit, est,
                m_timing_error, ms_db);
        m_dit      = std::clamp(est, rate * 0.022, rate * 0.30);
        m_timing_error = 0.2;                         // neutral — re-measure
        m_relock_hold = kRelockHold;
    }
};
