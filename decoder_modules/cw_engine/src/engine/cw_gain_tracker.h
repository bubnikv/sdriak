/*
 * cw_gain_tracker.h — v3's fading tracker, step K2 of FADING_TRACKER.md.
 *
 * The channel's complex gain g on the matched filter's coherent sum u:
 *
 *     u_t = k_t · g_t + n_t        k_t: keying 0/1, n_t: CN(0, σ²)
 *     g_t = a · g_{t−1} + v_t      v_t: CN(0, (1 − a²) P)
 *
 * a complex AR(1) whose correlation matches a Gaussian Doppler spectrum of
 * spread B (= 2σ_f) at kMatchSeconds; σ² and the key-down power come from
 * CwPowerStats. The keying is not known here (the lattice decides it 4–8
 * dits later), so the update is the soft data association of
 * FADING_TRACKER.md §3: weighted by the probability that the key is down,
 * from the observation's own likelihood ratio and the keying duty, and
 * collapsed to one Gaussian. It updates once per sum length L, where the
 * running sum's noise is independent; it predicts every frame.
 *
 * The evidence for the lattice is the likelihood ratio of key down against
 * key up given the predicted gain (FADING_TRACKER.md §4):
 *
 *     llr = ln CN(u; µ, σ² + V) − ln CN(u; 0, σ²)
 *
 * a matched filter against the prediction when the gain is well known,
 * the Rayleigh energy statistic when it is not (V → P). Both hypotheses
 * carry an impulse term, ε·CN(0, κσ²), in the score only, its share ε and
 * power κ learned from the channel as a two-component mixture would learn
 * them; the state update
 * takes every observation (one channel cannot tell an impulse from a quick
 * fade; impulses are repaired before the channelizer, ALGORITHM.md §5.7).
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <algorithm>
#include <cmath>

#include "skim_common.h"

class CwGainTracker {
    static constexpr auto kMatchSeconds = 0.04;   // AR(1) matched to the Gaussian
                                                  // correlation at this lag
    static constexpr auto kKeyedShare = 0.45;     // the key-down prior
    static constexpr auto kMaxLlr = 4.0;          // per-frame clamp, after the 1/L
                                                  // weight (as CwPowerStats::llr)
    static constexpr auto kMinMarkToNoise = 1.0;  // the gain's power ≥ this × σ²
    static constexpr auto kImpulseSeconds = 4.0;  // the impulse term's rate and
                                                  // power: averaged over this long
    static constexpr auto kMinEps = 0.001, kMaxEps = 0.3;
    static constexpr auto kMinKappa = 4.0, kMaxKappa = 1e4;
    // The spread follows the station's SNR500 between these: a weak signal
    // keeps the narrow spread (the AFC's residual rotation is lost in the
    // noise, a tighter model integrates more), a strong one the wide one
    // (that rotation would read as a fade the model is too sure of).
    static constexpr auto kNarrowDb = 3.0, kWideDb = 10.0;

public:
    // rate: frames per second; spread_hz: B on a strong signal, narrow_hz on
    // a weak one (0: B throughout); eps, kappa: the impulse term (eps 0:
    // none); adapt: learn eps and kappa from the channel, starting from the
    // values given.
    // ratio: the score's noise is at least the predicted mark power over this
    // (0: none).
    void init(double rate, double spread_hz, double narrow_hz, double eps, double kappa, bool adapt, double ratio) {
        m_ratio = ratio;
        m_rate = rate;
        m_wide_hz = spread_hz;
        m_config_hz = spread_hz;
        m_narrow_hz = narrow_hz > 0 ? narrow_hz : spread_hz;
        m_a = coefficient(m_wide_hz);
        m_eps = eps;
        m_kappa = kappa;
        m_adapt = adapt && eps > 0;
        m_imp_share = eps;
        m_imp_power = eps * kappa;
        reset();
    }

    // The spread measured on the station (CwSpreadEstimator), in place of
    // the configured one.
    // A weak station takes it as it is; a strong one at least the configured
    // spread, which absorbs the AFC's wandering residual (the spread then
    // follows the SNR between kNarrowDb and kWideDb, as with narrow_hz).
    void set_spread(double b) {
        m_narrow_hz = b;
        m_wide_hz = std::max(m_config_hz, b);
    }
    double spread() const { return m_wide_hz; }
    // The gain's mean rotation, Hz (the AFC's residual, measured on the
    // station): predicted, not left to the spread.
    void set_rotation(double hz) {
        m_rotate = true;
        m_rot_re = std::cos(2.0 * SKIM_PI * hz / m_rate);
        m_rot_im = std::sin(2.0 * SKIM_PI * hz / m_rate);
    }

    // A fresh gain; the impulse term, a property of the band, is kept.
    void reset() {
        m_mu_re = m_mu_im = 0.0;
        m_rel = 1.0;
        m_phase = 0;
    }

    // One frame's coherent sum u (re, im), L frames long; noise: σ², the
    // noise power of u; mark: the mean key-down power of u. Returns the
    // frame's llr, 1/L weighted and clamped as the lattice reads it.
    // snr_db: the station's SNR500, which sets the spread.
    double step(double re, double im, unsigned L, double noise, double mark, double snr_db) {
        if (noise <= 0)
            return 0.0;
        if (m_phase == 0 && m_narrow_hz != m_wide_hz) {
            const double t = std::clamp((snr_db - kNarrowDb) / (kWideDb - kNarrowDb), 0.0, 1.0);
            m_a = coefficient(m_narrow_hz + t * (m_wide_hz - m_narrow_hz));
        }
        const double P = std::max(mark - noise, kMinMarkToNoise * noise);
        // predict one frame
        if (m_rotate) {
            const double r = m_a * (m_mu_re * m_rot_re - m_mu_im * m_rot_im);
            m_mu_im = m_a * (m_mu_re * m_rot_im + m_mu_im * m_rot_re);
            m_mu_re = r;
        } else {
            m_mu_re *= m_a;
            m_mu_im *= m_a;
        }
        m_rel = m_a * m_a * m_rel + (1.0 - m_a * m_a);
        // the score against the prediction
        const double var = m_rel * P;
        // The score's noise: at least the predicted mark power over m_ratio,
        // so a strong station's score crosses zero near half its predicted
        // amplitude, not on the matched filter's ramps (as CwPowerStats'
        // kMaxMarkToNoise); a tracked fade lowers the prediction and the
        // floor with it.
        const double mu2 = m_mu_re * m_mu_re + m_mu_im * m_mu_im;
        const double sn = m_ratio > 0 ? std::max(noise, (mu2 + var) / m_ratio) : noise;
        const double s1 = sn + var;
        const double dr = re - m_mu_re, di = im - m_mu_im;
        const double z2 = re * re + im * im;
        const double g1 = -std::log(s1) - (dr * dr + di * di) / s1;
        const double g0 = -std::log(sn) - z2 / sn;
        double l1 = g1, l0 = g0, lk = 0.0;
        if (m_eps > 0) {
            const double sk = m_kappa * sn;
            lk = std::log(m_eps) - std::log(sk) - z2 / sk;
            l1 = logaddexp(std::log1p(-m_eps) + g1, lk);
            l0 = logaddexp(std::log1p(-m_eps) + g0, lk);
        }
        const double llr = l1 - l0;
        // the update, once per independent sum
        if (++m_phase >= L) {
            m_phase = 0;
            if (m_adapt) {
                // The impulse term's share and power, as a two-component
                // mixture learns them: r, how far the impulse component
                // explains this sum (against noise or a mark at the keying
                // duty), averaged; its power, the r-weighted mean of |u|².
                const double lm = logaddexp(std::log(kKeyedShare) + g1, std::log(1.0 - kKeyedShare) + g0);
                const double r = std::exp(lk - logaddexp(std::log1p(-m_eps) + lm, lk));
                const double w = L / (kImpulseSeconds * m_rate);
                m_imp_share += w * (r - m_imp_share);
                m_imp_power += w * (r * z2 / noise - m_imp_power);
                m_eps = std::clamp(m_imp_share, kMinEps, kMaxEps);
                if (m_imp_share > 0) { m_kappa = std::clamp(m_imp_power / m_imp_share, kMinKappa, kMaxKappa); }
            }
            const double lo = std::clamp(llr + std::log(kKeyedShare / (1.0 - kKeyedShare)), -30.0, 30.0);
            const double p = 1.0 / (1.0 + std::exp(-lo));
            const double k = var / s1;
            const double ur = k * dr, ui = k * di;          // the mark update's step
            m_mu_re += p * ur;
            m_mu_im += p * ui;
            const double v1 = p * (1.0 - k) * var + (1.0 - p) * var + p * (1.0 - p) * (ur * ur + ui * ui);
            m_rel = std::min(v1 / P, 1.0);
        }
        return std::clamp(llr / L, -kMaxLlr, kMaxLlr);
    }

    double gain_power() const { return m_mu_re * m_mu_re + m_mu_im * m_mu_im; }
    double relative_variance() const { return m_rel; }
    double impulse_share() const { return m_eps; }
    double impulse_kappa() const { return m_kappa; }

private:
    // The AR(1) coefficient per frame for spread b.
    double coefficient(double b) const {
        const double rho = std::exp(-0.5 * SKIM_PI * SKIM_PI * b * b * kMatchSeconds * kMatchSeconds);
        return std::pow(rho, 1.0 / (kMatchSeconds * m_rate));
    }

    static double logaddexp(double x, double y) {
        const double m = std::max(x, y);
        return m + std::log(std::exp(x - m) + std::exp(y - m));
    }

    double   m_a = 1.0;                   // AR(1) coefficient per frame
    double   m_rate = 250.0;
    bool     m_rotate = false;            // set_rotation()
    double   m_rot_re = 1.0, m_rot_im = 0.0;
    double   m_wide_hz = 1.0, m_narrow_hz = 1.0;  // the spread, strong and weak
    double   m_config_hz = 1.0;           // B as configured
    double   m_ratio = 0.0;               // the score's noise ≥ (|µ|² + V) / this
    double   m_eps = 0.0, m_kappa = 30.0; // the impulse term
    bool     m_adapt = false;             // … learned
    double   m_imp_share = 0.0;           // its share, averaged
    double   m_imp_power = 0.0;           // r·|u|²/σ², averaged
    double   m_mu_re = 0.0, m_mu_im = 0.0;  // the gain's mean
    // Its variance relative to the gain's power P: P is learned while the
    // gain is tracked (from nothing, at the start of an over), and the
    // uncertainty follows it.
    double   m_rel = 1.0;
    unsigned m_phase = 0;                 // frames since the last update
};
