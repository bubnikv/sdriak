/*
 * cw_discriminator.h — v2's soft discriminator: the mark/space level
 * trackers µ_mark/µ_space and the per-sample log-likelihood ratio the
 * lattice reads (v3's weak view reads CwFrontEnd::llr instead).
 *
 * SDRIAK: upstream's mu_m/mu_s/k_m_dn/k_s/cont_mark/smear/edge_now fields
 * of the decoder state and the tracker, llr and edge-weak blocks of
 * soft_step(), as a stage; same operations, same order.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "decode_cw_config.h"

class CwDiscriminator {
public:
    // Observation model: llr = kLlrGain·(env − mid)/(µm − µs), clamped to
    // ±kLlrMax (the oracle's kws probe reads the same scale).
    static constexpr auto kLlrGain = 5.0;
    static constexpr auto kLlrMax = 2.5;

    // µ_mark rides QSB down within one fade edge (0.35 s release); µ_space
    // follows the below-mid mean a touch faster than env_lo.
    void init(double rate) {
        m_mark_release = 1.0 - exp(-1.0 / (rate * 0.35));
        m_space_gain   = 1.0 - exp(-1.0 / (rate * 0.25));
    }

    // A fresh lattice: the trackers start on the envelope's peak and floor.
    void seed(double env_hi, double env_lo) {
        m_mark_level  = env_hi;
        m_space_level = env_lo;
        m_mark_run    = 0;
    }

    // One soft-path envelope sample; returns the midpoint it was judged
    // against.
    //
    // µ_mark is a FAST decaying peak (instant attack, ~0.35 s release): a
    // QSB trough pulls it down within one fade edge, so faded marks stay
    // above the midpoint and keep their positive evidence — the fix for
    // "the fade ate my dits". The µ_s·1.5 floor stops a long pause from
    // collapsing the midpoint into the noise. µ_space is the below-mid
    // mean. (An early conditional-update µ_mark got STUCK whenever a fade
    // dropped marks below the old midpoint — nothing above mid ever
    // updated it again; the decaying peak cannot stick.)
    double update(float env) {
        m_mark_level = env > m_mark_level ? static_cast<double>(env)
                                          : m_mark_level + m_mark_release * (env - m_mark_level);
        m_mark_level = std::max(m_mark_level, std::max(1.5 * m_space_level, 1e-12));
        const double mid = 0.5 * (m_mark_level + m_space_level);
        if (env > mid) {
            m_mark_run++;
        } else {
            // Only CLEARLY space-like samples may teach µ_space. A fade's edge
            // throws mark samples just under the midpoint — letting them in
            // drags µ_s (and the midpoint) up under the whole trough until every
            // faded mark reads as space (the mid-word break cascade).
            if (env < m_space_level + 0.25 * (m_mark_level - m_space_level)) {
                m_space_level += m_space_gain * (env - m_space_level);
            }
            m_mark_run = 0;
        }
        return mid;
    }

    // The sample's llr mark : space (env against mid, from update()).
    //
    // Two llr views, averaged:
    //  - span: where does m sit between µ_space and µ_mark? Follows QSB,
    //    but cannot tell a −18 dB notch inside a dash from a real space.
    //  - Rayleigh: how plausible is m as pure noise? A notch bottoms out
    //    4–5× ABOVE the noise floor — "not noise" — while a real space
    //    sits ON it. Anchored to µ_s, blind to fading.
    // The average lets a sub-dit notch pass with ~zero evidence (duration
    // priors then carry the dash through), yet keeps real spaces at full
    // negative weight and low-level QRM in spaces near zero.
    double llr(float env, double mid, const CwLlrConfig &cfg) const {
        const double x = (env - mid) / (m_mark_level - m_space_level);
        const double span_llr = std::clamp(kLlrGain * x, -kLlrMax, kLlrMax);
        // Zero crossing at z ≈ 2.8: Rayleigh noise clears that only ~2 % of
        // samples (no sustained runs — no phantom dits minted from the tail
        // after an over), while a −18 dB notch (z ≈ 5) and QSB-trough marks
        // (z ≈ 7) sit far in the positive.
        const double z = env / std::max(m_space_level / 1.2533, 1e-12);
        const double ray_llr =
            z <= 1.0 ? -kLlrMax
                     : std::clamp(3.3 * (0.5 * z * z - log(z) - 2.89),
                             -kLlrMax, kLlrMax);
        // SKIM_V3_RAYFADE="r0,r1[,wmin]" (4,10 was the CQ WW 2012 choice; off
        // by default since v0.6.0, with the edge model and for the same
        // reason): the Rayleigh view's weight fades from ½ to wmin (0) as
        // µ_mark/µ_space climbs from r0 to r1. Anchored to the noise floor, it
        // calls everything that is "not noise" a mark — on a strong signal
        // that includes the decaying tail of every element and the QRM filling
        // the gaps, so marks grow, element spaces shrink under the 0.7-dit ESP
        // floor and two dits fuse into a dah (CQ WW 2012, HG5C at 30 dB read
        // "HGBC"; 40 m recall +9 points). The same view is what carries a
        // sub-dit flutter notch through a dash, so the fade only acts as far
        // as the signal's edges are measurably smeared (SKIM_V3_RAYSMEAR) — a
        // crisp signal keeps the notch protection.
        const bool   rf_on = cfg.rayfade;                  // "r0,r1[,wmin]"
        const double rf_r0 = cfg.rayfade_r0, rf_r1 = cfg.rayfade_r1,
                     rf_wmin = cfg.rayfade_wmin;
        double w_ray = 0.5;
        if (rf_on) {
            const double ratio = m_mark_level / std::max(m_space_level, 1e-12);
            double wf = rf_wmin + (0.5 - rf_wmin) *
                                      std::clamp((rf_r1 - ratio) / (rf_r1 - rf_r0), 0.0, 1.0);
            // SKIM_V3_RAYSMEAR="s0,s1": the fade applies only as far as this
            // signal's edges are measurably smeared — the edge model's
            // (dit − element space)/(dit + element space), averaged over
            // committed elements. Slow edges are what turn "not noise" into
            // longer marks; a crisp signal keeps the full view and with it
            // the notch protection.
            const bool   rs_on = cfg.raysmear;                // "s0,s1"
            const double rs_s0 = cfg.raysmear_s0, rs_s1 = cfg.raysmear_s1;
            if (rs_on) {
                const double k = std::clamp((m_smear - rs_s0) / (rs_s1 - rs_s0), 0.0, 1.0);
                wf = 0.5 - (0.5 - wf) * k;
            }
            w_ray = wf;
        }
        // SKIM_V3_RAYCAP=1: below the span midpoint the Rayleigh view may
        // CANCEL the span's negative evidence (a notch inside a dash scores
        // ~0, the duration priors carry the dash) but never turn it into mark
        // evidence — an element's decaying tail and the QRM in a gap sit just
        // as far above the noise as a notch does, and must not lengthen the
        // mark.
        const bool rc_on = cfg.raycap;
        const double ray_v = rc_on && span_llr < 0 ? std::min(ray_llr, -span_llr) : ray_llr;
        return (1.0 - w_ray) * span_llr + w_ray * ray_v;
    }

    // A committed inter-element space of d samples at clock dit: the edge
    // m_smear (dit - d)/(dit + d) into its EMA.
    void learn_smear(double dit, double d) {
        if (dit > 0) {
            const double sm = (dit - d) / (dit + d);
            m_smear += 0.1 * (sm - m_smear);
        }
    }

    // SKIM_V3_EDGE_WEAK: the edge model on while µ_mark/µ_space sits under
    // ratio[0], off again above ratio[1] (ratio[0] ≤ 0: off).
    void update_edge_model(const double *ratio) {
        if (ratio[0] > 0) {
            const double r = m_mark_level / std::max(m_space_level, 1e-12);
            if (!m_edge_now && r < ratio[0]) {
                m_edge_now = true;
            } else if (m_edge_now && r > ratio[1]) {
                m_edge_now = false;
            }
        }
    }
    double   mark_level() const { return m_mark_level; }    // µ_mark
    double   space_level() const { return m_space_level; }  // µ_space
    unsigned mark_run() const { return m_mark_run; }        // samples continuously
                                                            // above the midpoint
    bool     edge_now() const { return m_edge_now; }        // SKIM_V3_EDGE_WEAK: weak
                                                            // enough for the edge model
    // Forget the edge smear (with the dit clock).
    void     reset_smear() { m_smear = 0.0; }

private:
    double   m_mark_level = 0, m_space_level = 0;   // µ_mark / µ_space trackers
    bool     m_edge_now = false;          // SKIM_V3_EDGE_WEAK: this channel is
                                          // weak enough for the edge model
    double   m_smear = 0;                 // EMA of (mark - space)/(mark+space)
                                          // over committed DIT/ESP pairs
    double   m_mark_release = 0;          // µ_mark's per-sample release gain
    double   m_space_gain = 0;            // µ_space's per-sample EMA gain
    unsigned m_mark_run = 0;              // samples continuously above mid
};
