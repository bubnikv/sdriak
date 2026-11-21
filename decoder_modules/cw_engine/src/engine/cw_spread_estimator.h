/*
 * cw_spread_estimator.h — the station's Doppler spread, measured on the
 * keying the lattice committed (FADING_TRACKER.md K3).
 *
 * CwGainTracker models the channel gain as an AR(1) whose correlation
 * matches a Gaussian Doppler spectrum of spread B. A fixed B is a
 * compromise: 0.5 Hz integrates more on a steady signal, 1 Hz is needed for
 * fast fading and for the AFC's residual rotation. Here B is measured.
 *
 * Inside a committed mark the matched filter's coherent sum is u = g + n.
 * Pairs of such samples, in the same mark or in earlier ones, at least one
 * sum length apart (so their noise is independent), estimate the gain's
 * autocorrelation:
 *
 *     ρ(τ) = E[u_t · conj(u_{t−τ})] / (E|u|² − σ²)
 *
 * and for a Gaussian Doppler spectrum −ln|ρ(τ)| = π²B²τ²/2. B is the least
 * squares fit through the origin in τ² over the lags where 0.2 ≤ |ρ| ≤
 * 0.95; if the gain stays correlated at every lag measured, B is bounded by
 * the longest one. The sums forget slowly (kForgetSeconds), so the estimate
 * follows a channel that changes and survives the gaps between overs.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <vector>

#include "skim_common.h"

class CwSpreadEstimator {
    static constexpr auto kRingSize = 2048u;     // samples kept by lattice time (8 s at 250 Hz)
    static constexpr auto kMaxLagSeconds = 0.8;  // the longest lag measured
    static constexpr auto kBinSeconds = 0.04;    // lags pooled in bins this wide
    static constexpr auto kForgetSeconds = 20.0; // the sums' memory, in mark time
    static constexpr auto kMinPairs = 200.0;     // pairs a bin needs to count
    static constexpr auto kMinB = 0.2, kMaxB = 5.0;

public:
    void init(double rate) {
        m_rate = rate;
        m_bin = std::max(1, static_cast<int>(kBinSeconds * rate));
        m_nbins = static_cast<int>(kMaxLagSeconds * rate) / m_bin + 1;
        m_r.assign(m_nbins, std::complex<double>());
        m_c.assign(m_nbins, 0.0);
        m_ring.assign(kRingSize, std::complex<float>());
        m_valid.assign(kRingSize, 0);
    }

    // The sum u at lattice time t (and whether there was one).
    void push(uint64_t t, float re, float im, bool valid) {
        m_ring[t % kRingSize] = std::complex<float>(re, im);
        m_valid[t % kRingSize] = valid ? 1 : 0;
        m_t = t;
    }

    // A committed mark (start, end]; L the sum length, noise σ² of u. Its
    // interior samples, away from the matched filter's ramps, pair with the
    // recent marks' samples.
    void mark(uint64_t start, uint64_t end, unsigned L, double noise) {
        if (m_r.empty() || L == 0 || noise <= 0 || end <= start || m_t < end || m_t - start >= kRingSize) return;
        const uint64_t a = start + L / 2 + 1, b = end > L / 2 ? end - L / 2 : 0;
        if (b <= a) return;
        // forget in proportion to the mark time added
        const double keep = std::exp(-static_cast<double>(b - a) / (kForgetSeconds * m_rate));
        for (int i = 0; i < m_nbins; i++) { m_r[i] *= keep; m_c[i] *= keep; }
        m_pow *= keep;
        m_pn *= keep;
        m_noise = noise;
        const int maxlag = m_nbins * m_bin - 1;
        for (uint64_t t = a; t <= b; t++) {
            if (!m_valid[t % kRingSize]) continue;
            const std::complex<double> ut(m_ring[t % kRingSize]);
            m_pow += std::norm(ut);
            m_pn += 1.0;
            // pairs with earlier interior samples, this mark's and the history's
            for (const auto &h : m_hist) {
                const int64_t lag = static_cast<int64_t>(t) - static_cast<int64_t>(h.first);
                if (lag < static_cast<int64_t>(L) || lag > maxlag) continue;
                const int bin = static_cast<int>(lag / m_bin);
                m_r[bin] += ut * std::conj(std::complex<double>(h.second));
                m_c[bin] += 1.0;
            }
            m_hist.push_back({t, m_ring[t % kRingSize]});
        }
        while (!m_hist.empty() && m_hist.front().first + static_cast<uint64_t>(maxlag) < b) m_hist.pop_front();
        refit();
    }

    // The measured spread, Hz; 0 until enough has been seen.
    double spread() const { return m_b; }
    double residual_hz() const { return m_df; }   // the gain's mean rotation, Hz

private:
    void refit() {
        if (m_pn < kMinPairs) return;
        const double s = m_pow / m_pn - m_noise;   // the gain's power
        if (s <= 0) return;
        double num = 0, den = 0, longest = 0, pnum = 0, pden = 0;
        bool any = false, all_high = true;
        for (int i = 0; i < m_nbins; i++) {
            if (m_c[i] < kMinPairs) continue;
            const std::complex<double> rho = m_r[i] / (m_c[i] * s);
            const double tau = (i + 0.5) * m_bin / m_rate;
            const double mag = std::abs(rho);
            longest = tau;
            if (mag < 0.95) all_high = false;
            if (mag < 0.2 || mag > 0.95) continue;
            const double y = -std::log(mag);
            num += tau * tau * y;
            den += tau * tau * tau * tau;
            pnum += tau * std::arg(rho);         // the rotation: arg ρ = 2π δf τ
            pden += tau * tau;
            any = true;
        }
        if (any && den > 0) {
            m_b = std::clamp(std::sqrt(2.0 * num / den) / SKIM_PI, kMinB, kMaxB);
            m_df = pnum / pden / (2.0 * SKIM_PI);
        } else if (all_high && longest > 0) {
            // correlated at every lag measured: −ln 0.95 at the longest at most
            m_b = std::clamp(std::sqrt(2.0 * -std::log(0.95)) / (SKIM_PI * longest), kMinB, kMaxB);
        }
    }

    double m_rate = 250.0;
    int    m_bin = 10, m_nbins = 0;
    std::vector<std::complex<double>> m_r;   // Σ u_t conj(u_{t−τ}) by lag bin
    std::vector<double> m_c;                 // … pairs counted
    double m_pow = 0, m_pn = 0;              // Σ |u|², samples
    double m_noise = 0;
    double m_b = 0, m_df = 0;
    uint64_t m_t = 0;
    std::vector<std::complex<float>> m_ring;
    std::vector<unsigned char> m_valid;
    std::deque<std::pair<uint64_t, std::complex<float>>> m_hist;   // recent interior samples
};
