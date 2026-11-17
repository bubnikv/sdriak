/*
 * cw_lattice.h — the semi-Markov Viterbi lattice over marks and spaces.
 *
 * Every sample adds its mark/space log-likelihood ratio to a cumulative
 * ring; a segment ending at t scores the evidence since its start plus a
 * duration prior. Marks (Dit, Dah) and spaces (ElementSpace, CharSpace,
 * WordSpace) alternate by construction. The best path's stable prefix — segments that ended more
 * than a lag ago — commits; the committed segments drive the clock, the
 * fist and the character assembly (the caller's on_segment).
 *
 * Upstream's W/WMASK are CwLatticeRings::kSize/kMask (aliased in
 * CwLattice), NEG_INF CwLattice::kNoScore, SEG_NAMES/SEG_IDEAL/SEG_ALPHA/
 * SEG_COST CwSegmentPriors::kSegment[].name/.ideal_dits/.prior_weight and
 * kSegmentCost;
 * the SEG_* segment types the enum class CwSeg.
 *
 * SDRIAK: upstream's struct Lattice, the active/commit_t/commit_kind
 * fields of the decoder state, lattice_step(), lattice_head(), the
 * traceback + renormalization of lattice_commit(), and seg_ideal(),
 * dur_pen(), dur_pen_memo(), pen_sync(), seg_window(), as a stage. The
 * priors read a CwSegmentPriors (the clock, the fist and the edge switch) the
 * decoder builds; same arithmetic, same order.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

// --- lattice geometry ---------------------------------------------------------

// Segment types. Marks and spaces alternate by construction.
enum class CwSeg : uint8_t { Dit, Dah, ElementSpace, CharSpace, WordSpace };
// The type's row in the per-type tables (priors, names, the duration memo).
constexpr int  seg_index(CwSeg s) { return static_cast<int>(s); }
constexpr bool is_mark(CwSeg s) { return s == CwSeg::Dit || s == CwSeg::Dah; }

// The lattice's rings (allocated lazily — noise channels never pay).
struct CwLatticeRings {
    static constexpr auto kSize = 2048;        // samples a ring holds (power of 2)
    static constexpr auto kMask = (kSize - 1);

    double   llr_sum[kSize];                // cumulative llr, llr_sum[t & kMask]
    // best score of a path whose last segment (a mark / a space) ends at t
    float    best_mark_end[kSize], best_space_end[kSize];
    uint16_t mark_len[kSize], space_len[kSize];     // traceback: duration
    CwSeg    mark_type[kSize], space_type[kSize];   // traceback: segment type
    // duration_prior() memo: the prior depends on (type, d) and the clock/fist
    // triple only, and the step asks for the same few hundred values on
    // every sample — one log() each was a third of the replay CPU. An entry
    // is valid while its stamp equals prior_generation.
    double   prior_memo[5][kSize / 2 + 1];
    uint32_t prior_stamp[5][kSize / 2 + 1];
    uint32_t prior_generation;
    // the priors the memo was filled at
    double   memo_dit, memo_char_space, memo_word_space, memo_element_space;
    bool     memo_edge;
};

// What the duration priors and the search windows read: the clock, the
// fist centres, the edge model switch and the dah ceiling.
struct CwSegmentPriors {
    // Duration prior: −α·ln²(d/ideal). Ideals are 1/3/1/3/7 dits; α is tight
    // for elements (keying is the rigid part of a fist) and loose for word
    // spaces (operators breathe).
    struct SegInfo {
        const char *name;                   // the debug trace's (SDRIAK: was a
                                            // compound literal, not C++)
        double      ideal_dits;             // ideal duration, dits
        double      prior_weight;           // α
    };
    static constexpr SegInfo kSegment[5] = {   // by seg_index()
        { "DIT", 1.0, 10.0 },
        { "DAH", 3.0, 10.0 },
        { "esp", 1.0, 10.0 },
        { "CSP", 3.0, 6.0 },
        { "WSP", 7.0, 2.5 },
    };
    // Per-segment fixed cost: an MDL-style anti-chatter term — a boundary must
    // pay for itself in evidence.
    static constexpr auto kSegmentCost = 0.5;

    double dit;                           // samples per dit
    double element_space;                 // inter-element space EMA (0 = none)
    double timing_error;                  // the clock's timing error
    double char_space, word_space;        // fist centres, dits
    double dah_max;                       // SKIM_V3_DAHMAX, dits
    bool   edge;                          // the edge model acts

    // Ideal duration of a segment type in samples.
    double ideal_length(CwSeg type) const {
        const double k = type == CwSeg::CharSpace   ? char_space
                         : type == CwSeg::WordSpace ? word_space
                                                    : kSegment[seg_index(type)].ideal_dits;
        if (!edge || element_space <= 0)
            return k * dit;
        const double tu = 0.5 * (dit + element_space);
        const double dl = std::clamp(0.5 * (dit - element_space), -0.4 * tu, 0.4 * tu);
        return k * tu + (is_mark(type) ? dl : -dl);
    }

    // Duration prior in the log domain. The char/word space ideals ride the
    // fist model.
    double duration_prior(CwSeg type, double d) const {
        double r = log(d / ideal_length(type));
        return -kSegment[seg_index(type)].prior_weight * r * r - kSegmentCost;
    }

    // The durations (samples) a segment of this type may take — the
    // lattice's search window, shared with the word re-read
    // (SKIM_V3_RESCORE) so both score the same hypotheses.
    //
    // Mark ranges OVERLAP on purpose: a hard dit/dah cutoff at a stale dit
    // clock misfiles every element after a speed change (28 WPM dahs = 1.9×
    // an 18 WPM dit — the classifier lock-up the first build hit). Inside
    // the overlap the duration priors decide, and they move with the clock.
    //
    // ESP's floor sits at 0.7 dits: a real inter-element space is never
    // shorter (0.85 at −15 % jitter), but a sub-dit flutter dropout inside a
    // dash is — without the floor the lattice happily reads the dropout as
    // an inter-element space and splits the dash (dot-dot for a dash).
    // EXCEPT while the element clock is visibly wrong (timing_error high — a
    // speed change in progress): a stale slow dit makes 0.7·T LONGER than
    // the new true spaces and the lattice cannot separate elements at all.
    // The loose floor lets it re-lock; steady copy restores the strict one.
    // CSP/WSP windows ride the fist centres (seeds reproduce the old fixed
    // 1.6–5.5 / 4.0–17: 1.833·3 = 5.5, 0.571·7 = 4.0). The overlap between
    // CSP hi and WSP lo survives any centre pair — the priors decide there.
    // WSP's ceiling reaches past the break length so a lattice seeded
    // mid-pause can always bridge the remaining quiet to the first real
    // mark.
    void search_window(CwSeg type, int *dlo, int *dhi) const {
        const double T = dit;
        // SKIM_V3_DAHMAX: the dah window's ceiling in dits (default 5.0; 6.0
        // was tried with the CQ WW 2012 set — after a turnaround to half the
        // speed the new op's dahs run 5.6 old dits — and left with it,
        // v0.6.0).
        const double dah_hi = dah_max;
        double lo, hi;
        switch (type) {
        case CwSeg::Dit: lo = 0.60 * T; hi = 2.6 * T;    break;
        case CwSeg::Dah: lo = 1.6 * T;  hi = dah_hi * T; break;
        case CwSeg::ElementSpace: {
            // With the edge model the ESP floor sits under the SPACE ideal, not
            // the mark-biased dit.
            const double Tk = edge ? ideal_length(CwSeg::ElementSpace) : T;
            lo = (timing_error > 0.20 ? 0.50 : 0.70) * Tk;
            hi = 2.6 * T;
            break;
        }
        case CwSeg::CharSpace: lo = 1.6 * T; hi = 1.833 * char_space * T; break;
        default:               lo = 0.571 * word_space * T; hi = 17.0 * T; break;
        }
        *dlo = std::max(2, static_cast<int>(lo + 0.5));
        *dhi = std::min(static_cast<int>(hi + 0.5), CwLatticeRings::kSize / 2);
    }
};

class CwLattice {
public:
    static constexpr auto kSize = CwLatticeRings::kSize;   // the rings' samples
    static constexpr auto kMask = CwLatticeRings::kMask;
    static constexpr auto kNoScore = (-1e30f);  // no path ends here

    bool     active() const { return m_active; }      // seeded and stepping
    uint64_t committed_until() const { return m_committed_until; }  // the last
                                                                    // committed boundary
    uint64_t t0() const { return m_t0; }              // the seed: llr_sum valid
                                                      // from here
    // The rings, for the word re-read and the call reader (null before the
    // first start()).
    const CwLatticeRings *rings() const { return m_rings.get(); }
    // No lattice until the next start().
    void stop() { m_active = false; }

    // Seed a fresh lattice at sample t: either a mark or a space may open.
    void start(uint64_t t) {
        if (!m_rings) { m_rings = std::make_unique<CwLatticeRings>(); }
        CwLatticeRings *L = m_rings.get();
        for (unsigned i = 0; i < kSize; i++) {
            L->best_mark_end[i] = kNoScore;
            L->best_space_end[i] = kNoScore;
        }
        m_committed_until = t;
        m_committed_kind  = Boundary::Either;       // first segment: mark OR space
        L->best_mark_end[t & kMask]  = 0.0f;
        L->best_space_end[t & kMask] = 0.0f;
        L->mark_len[t & kMask]  = 0;                // seed boundaries stop the traceback
        L->space_len[t & kMask] = 0;
        L->llr_sum[t & kMask]   = 0.0;
        m_t0 = t;
        m_active = true;
    }

    // The sample t's llr into the cumulative ring.
    void push_llr(uint64_t t, double llr) {
        m_rings->llr_sum[t & kMask] = m_rings->llr_sum[(t - 1) & kMask] + llr;
    }

    // The duration prior through the memo — the same arithmetic, computed
    // once per (type, d) while the clock and fist stay put. A miss computes
    // it from priors, whatever the memo's generation.
    double duration_prior(const CwSegmentPriors &priors, CwSeg type, int d) {
        CwLatticeRings *L = m_rings.get();
        if (L->prior_stamp[seg_index(type)][d] != L->prior_generation) {
            L->prior_memo[seg_index(type)][d]  = priors.duration_prior(type, d);
            L->prior_stamp[seg_index(type)][d] = L->prior_generation;
        }
        return L->prior_memo[seg_index(type)][d];
    }

    // The duration memo follows the clock and the fist: a new generation
    // whenever one of them moved.
    void sync_prior_memo(const CwSegmentPriors &priors) {
        CwLatticeRings *L = m_rings.get();
        if (L->prior_generation == 0 || L->memo_dit != priors.dit ||
            L->memo_char_space != priors.char_space || L->memo_word_space != priors.word_space ||
            L->memo_element_space != priors.element_space || L->memo_edge != priors.edge) {
            L->prior_generation++;                // 0 never matches a stamp: zeroed
            if (L->prior_generation == 0) { L->prior_generation = 1; }
            L->memo_dit           = priors.dit;
            L->memo_char_space    = priors.char_space;
            L->memo_word_space    = priors.word_space;
            L->memo_element_space = priors.element_space;
            L->memo_edge          = priors.edge;
        }
    }

    // One Viterbi step for the sample at t (llr already in the ring).
    void step(uint64_t t, const CwSegmentPriors &priors) {
        CwLatticeRings *L = m_rings.get();
        const double  ct  = L->llr_sum[t & kMask];
        const uint64_t cmt = m_committed_until;
        sync_prior_memo(priors);

        // Mark ends at t: preceded by a space (or seed) boundary at t−d.
        float  best_m = kNoScore;
        uint16_t bd = 0;
        CwSeg    bt = CwSeg::Dit;
        static const CwSeg MTYPES[2] = { CwSeg::Dit, CwSeg::Dah };
        for (int k = 0; k < 2; k++) {
            int dlo, dhi;
            priors.search_window(MTYPES[k], &dlo, &dhi);
            for (int d = dlo; d <= dhi; d++) {
                if (t < static_cast<uint64_t>(d) || t - d < cmt)
                    break;                        // d grows — only older from here
                if (t - d == cmt && m_committed_kind == Boundary::Mark)
                    continue;                     // two marks may not touch
                float prev = L->best_space_end[(t - d) & kMask];
                if (prev <= kNoScore)
                    continue;
                double ev = ct - L->llr_sum[(t - d) & kMask];
                float  sc = prev + static_cast<float>(ev + duration_prior(priors, MTYPES[k], d));
                if (sc > best_m) {
                    best_m = sc;
                    bd = static_cast<uint16_t>(d);
                    bt = MTYPES[k];
                }
            }
        }
        L->best_mark_end[t & kMask] = best_m;
        L->mark_len[t & kMask]      = bd;
        L->mark_type[t & kMask]     = bt;

        // Space ends at t: preceded by a mark (or seed) boundary at t−d.
        float best_s = kNoScore;
        bd = 0;
        bt = CwSeg::Dit;
        static const CwSeg STYPES[3] = { CwSeg::ElementSpace, CwSeg::CharSpace, CwSeg::WordSpace };
        for (int k = 0; k < 3; k++) {
            int dlo, dhi;
            priors.search_window(STYPES[k], &dlo, &dhi);
            for (int d = dlo; d <= dhi; d++) {
                if (t < static_cast<uint64_t>(d) || t - d < cmt)
                    break;
                if (t - d == cmt && m_committed_kind == Boundary::Space)
                    continue;                     // two spaces may not touch
                float prev = L->best_mark_end[(t - d) & kMask];
                if (prev <= kNoScore)
                    continue;
                double ev = -(ct - L->llr_sum[(t - d) & kMask]);
                float  sc = prev + static_cast<float>(ev + duration_prior(priors, STYPES[k], d));
                if (sc > best_s) {
                    best_s = sc;
                    bd = static_cast<uint16_t>(d);
                    bt = STYPES[k];
                }
            }
        }
        L->best_space_end[t & kMask] = best_s;
        L->space_len[t & kMask]      = bd;
        L->space_type[t & kMask]     = bt;
    }

    // Best full-path hypothesis "now" (sample t, clock dit): a boundary at τ
    // plus the in-progress segment (τ, t] scored with optimistic (zero)
    // duration penalty.
    void best_head(uint64_t t, double dit, uint64_t *out_tau, bool *out_mark_ended) const {
        const CwLatticeRings *L = m_rings.get();
        const double  ct = L->llr_sum[t & kMask];
        const int back = std::min(static_cast<int>(17.0 * dit), static_cast<int>(t - m_committed_until));
        double best = -1e30;
        uint64_t tau = m_committed_until;
        bool mark_ended = m_committed_kind != Boundary::Space;
        for (int d = 0; d <= back; d++) {
            uint64_t at = t - d;
            double ev = ct - L->llr_sum[at & kMask];         // llr sum over (τ, t]
            if (L->best_space_end[at & kMask] > kNoScore) {   // space ended → mark running
                double sc = L->best_space_end[at & kMask] + ev;
                if (sc > best) { best = sc; tau = at; mark_ended = false; }
            }
            if (L->best_mark_end[at & kMask] > kNoScore) {    // mark ended → space running
                double sc = L->best_mark_end[at & kMask] - ev;
                if (sc > best) { best = sc; tau = at; mark_ended = true; }
            }
        }
        *out_tau = tau;
        *out_mark_ended = mark_ended;
    }

    // Commit the stable prefix of the best path (segments ending ≤ t − lag,
    // lag = lag_dits·dit; 0 commits everything). Oldest first, each segment
    // goes to on_segment(type, d, start, end) before committed_until moves past
    // it.
    template <class OnSegment>
    void commit(uint64_t t, double dit, double lag_dits, OnSegment &&on_segment) {
        if (!m_active || t <= m_committed_until)
            return;
        CwLatticeRings *L = m_rings.get();
        uint64_t tau;
        bool mark_ended;
        best_head(t, dit, &tau, &mark_ended);

        // Walk the backpointers down to committed_until, collecting segments.
        uint16_t segs_d[64];
        CwSeg    segs_t[64];
        unsigned   n = 0;
        uint64_t at = tau;
        bool in_mark = mark_ended;
        while (at > m_committed_until && n < 64) {
            uint16_t d  = in_mark ? L->mark_len[at & kMask] : L->space_len[at & kMask];
            CwSeg    ty = in_mark ? L->mark_type[at & kMask] : L->space_type[at & kMask];
            if (d == 0)
                break;                                 // seed boundary reached
            segs_d[n] = d;
            segs_t[n] = ty;
            n++;
            at -= d;
            in_mark = !in_mark;                      // alternation
        }

        // Oldest first, commit those whose END is out of the revision window.
        const uint64_t lag = static_cast<uint64_t>(lag_dits * dit);
        const uint64_t stable = t > lag ? t - lag : 0;
        uint64_t end = m_committed_until;
        bool committed_any = false;
        for (unsigned i = n; i > 0; i--) {
            end += segs_d[i - 1];
            if (end > stable)
                break;
            const CwSeg ty = segs_t[i - 1];
            on_segment(ty, static_cast<double>(segs_d[i - 1]), end - segs_d[i - 1], end);
            m_committed_until = end;
            m_committed_kind  = is_mark(ty) ? Boundary::Mark : Boundary::Space;
            committed_any = true;
        }

        // Renormalize scores once they grow large (float precision).
        if (committed_any) {
            float base = m_committed_kind == Boundary::Mark ? L->best_mark_end[m_committed_until & kMask]
                                                            : L->best_space_end[m_committed_until & kMask];
            if (fabsf(base) > 1e5f) {
                for (unsigned i = 0; i < kSize; i++) {
                    if (L->best_mark_end[i] > kNoScore) { L->best_mark_end[i] -= base; }
                    if (L->best_space_end[i] > kNoScore) { L->best_space_end[i] -= base; }
                }
            }
        }
    }

private:
    // What kind of boundary the committed prefix ends in. Either is the fresh
    // seed: both a mark and a space may open the lattice (a restart can land
    // mid-pause, and the first real mark may be many dits away).
    enum class Boundary : uint8_t { Mark, Space, Either };

    bool     m_active = false;
    uint64_t m_committed_until = 0;
    Boundary m_committed_kind = Boundary::Mark;  // boundary kind at m_committed_until
    uint64_t m_t0 = 0;
    std::unique_ptr<CwLatticeRings> m_rings;
};
