/*
 * cw_preroll.h — the pre-roll rings (squelch attack fix, 2026-07-19) and the
 * search for the over's head the replay starts from.
 *
 * The last ~2 s (v3: CwFrontEnd::kRingSeconds) of soft-path envelope
 * samples — and with v3's front end the matched amplitude beside them —
 * written on EVERY
 * sample, dead air included. When the channel wakes with a usable dit clock
 * (reopen from pause, or the moment the bootstrap learns the clock) the
 * ring is replayed through the soft path, so the head the gate delay and
 * the bootstrap consumed still decodes. pre_from fences the replayable
 * window: audio older than the last pause entry (or a watchdog reboot) was
 * already decoded or squelched — replaying it would duplicate text.
 *
 * SDRIAK: upstream's pre/pre3/pre_* fields of the decoder state and the
 * search half of preroll_replay(), as a stage; the replay itself (the soft
 * path) stays in the decoder.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_debug.h"

class CwPreroll {
public:
    // size samples (a power of two); matched: v3's matched ring as well.
    void init(unsigned size, bool matched) {
        m_size = size;
        m_env.assign(static_cast<size_t>(m_size), 0.0f);
        if (matched) { m_matched.assign(static_cast<size_t>(m_size), 0.0f); }
    }

    bool     empty() const { return m_env.empty(); }
    unsigned size() const { return m_size; }
    unsigned head() const { return m_head; }       // samples written

    // The sample's matched amplitude (before push(), at the same head).
    void put_matched(float mr) { m_matched[m_head & (m_size - 1)] = mr; }
    // … written later, at write count `at` (the front end's backfill).
    void set_matched(unsigned at, float r) { m_matched[at & (m_size - 1)] = r; }
    // The sample's soft-path envelope; the head moves on.
    void push(float ms) { m_env[m_head++ & (m_size - 1)] = ms; }
    // Everything so far was decoded or squelched: a replay may only reach
    // back to here.
    void fence() { m_from = m_head; }

    // The samples at write count `at`.
    float env(unsigned at) const { return m_env[at & (m_size - 1)]; }
    float matched(unsigned at) const { return m_matched[at & (m_size - 1)]; }

    // The over's head in the last `have` samples of the ring (the matched one
    // when `use_matched`): true with the replay starting `start` samples into
    // them; use_matched: search the matched ring. thr: the mark bar; bar: the head's mark-over-quiet ratio; cap:
    // the most samples looked back; dit, rate: the clock; t, tr: the trace's.
    bool find_head(bool use_matched, unsigned cap, float thr, double bar, double dit, double rate,
                   uint64_t t, const CwTrace &tr, unsigned *have_out, unsigned *start_out) const {
        const unsigned size = m_size;
        const float *ring = use_matched ? m_matched.data() : m_env.data();
        unsigned have = std::min(m_head, size);
        have = std::min(have, m_head - m_from);
        have = std::min(have, cap);
        const unsigned qmin    = static_cast<unsigned>(std::max(2.0 * dit, 0.10 * rate));
        const unsigned marklen = static_cast<unsigned>(std::max(3.0, 0.55 * dit));
        if (have < qmin + marklen)
            return false;
        // Onset: the first above-threshold run ≥ marklen (a real mark, not a
        // noise pop) whose start follows ≥ qmin of quiet. qmin ≥ 2 dits keeps a
        // mid-character element gap from ever qualifying — a replay may only
        // start at a char/word boundary or dead air, so no partial character can
        // be minted.
        unsigned quiet = 0, run = 0, run_at = 0, onset = 0;
        bool found = false;
        for (unsigned k = 0; k < have && !found; k++) {
            const float v = ring[(m_head - have + k) & (size - 1)];
            if (v > thr) {
                if (run == 0) { run_at = k; }
                run++;
                if (run >= marklen && quiet >= qmin) {
                    onset = run_at;
                    found = true;
                }
            } else {
                if (run) { quiet = 0; }                  // a pop ended — quiet anew
                quiet++;
                run = 0;
            }
        }
        if (!found)
            return false;
        // Weak heads keep the old behaviour. The seeded µ midpoint reflects the
        // WAKE moment; the head of a fading-in over sits under it and reads TORN
        // (YOTA fixture: 12 dB RV6HV re-read as "ST AEV6HV" where the old code
        // stayed quiet — and the mutation cost the station its table entry). A
        // mutated head near the extractor is dearer than a missing one, and the
        // clip class this fix hunts was measured on strong runners (R3OM 33 dB).
        // The bar is measured on the RING, onset-run level against the quiet
        // level before it — the envelope ratio at wake time is useless here: the
        // gate opens ON the rising edge, so it reads ~4-5× by construction
        // whatever the station's strength (fixture-measured). 5.6× on the ring
        // means keeps every strong-channel gain and blocks the 12 dB tear.
        {
            double qsum = 0.0, msum = 0.0;
            unsigned  qn = 0, mn = 0;
            for (unsigned k = onset > qmin ? onset - qmin : 0; k < onset; k++) {
                const float v = ring[(m_head - have + k) & (size - 1)];
                if (v <= thr) {
                    qsum += v;
                    qn++;
                }
            }
            for (unsigned k = onset; k < have && mn < 2 * marklen; k++) {
                const float v = ring[(m_head - have + k) & (size - 1)];
                if (v > thr) {
                    msum += v;
                    mn++;
                }
            }
            if (!qn || !mn || msum * static_cast<double>(qn) < bar * qsum * static_cast<double>(mn))
                return false;
        }
        const unsigned brk = static_cast<unsigned>(14.0 * dit);
        quiet = 0;
        for (unsigned k = onset; k < have; k++) {
            const float v = ring[(m_head - have + k) & (size - 1)];
            quiet = v > thr ? 0 : quiet + 1;
            if (quiet >= brk)
                return false;
        }
        const unsigned lead = std::min(static_cast<unsigned>(2.0 * dit), onset);
        CW_DBG(tr, "[preroll replay %u samples (onset -%u) at t=%lu]\n",
                have - (onset - lead), have - onset, (unsigned long)t);
        *have_out  = have;
        *start_out = onset - lead;
        return true;
    }

private:
    std::vector<float> m_env;             // the soft path's envelope
    std::vector<float> m_matched;         // v3: the matched amplitude
    unsigned m_size = 0;                  // power of two
    unsigned m_head = 0;                  // total writes; index = & (size-1)
    unsigned m_from = 0;                  // replay window fence (write count)
};
