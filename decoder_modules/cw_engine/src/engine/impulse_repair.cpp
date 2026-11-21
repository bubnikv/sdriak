/*
 * impulse_repair.cpp — see impulse_repair.h.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "impulse_repair.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kLn2 = 0.6931471805599453;
constexpr double kPi = 3.14159265358979323846;

int samples(double ms, double rate) { return std::max(1, static_cast<int>(std::lround(ms * 1e-3 * rate))); }

// In-place radix-2 FFT; inverse without the 1/N.
void fft(std::vector<std::complex<double>> &x, bool inverse) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1.0;
            for (size_t j = 0; j < len / 2; j++) {
                const std::complex<double> u = x[i + j], v = x[i + j + len / 2] * w;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
}  // namespace

ImpulseRepair::ImpulseRepair(double rate, const Config &cfg)
    : m_cfg(cfg), m_fit(samples(cfg.fit_ms, rate)), m_refit(samples(cfg.refit_ms, rate)),
      m_max_gap(samples(cfg.max_gap_ms, rate)),
      m_hold(cfg.hold_off_ms > 0.0 ? static_cast<size_t>(samples(cfg.hold_off_ms, rate)) : 0),
      m_err_window(static_cast<size_t>(samples(cfg.floor_ms, rate))),
      m_order(std::max(1, cfg.order)),
      // Detection runs `guard` ahead; a gap then needs up to max_gap, the
      // quiet run, the guard again and `order` samples of right context.
      m_look(m_max_gap + 2 * cfg.guard + cfg.quiet + m_order + 8) {
    m_err.reserve(m_err_window);
    if (cfg.split_db > 0.0) {
        int n = 16;
        while (n < rate / cfg.split_bin_hz) n <<= 1;
        m_split_n = n;
        m_win.resize(n);
        for (int i = 0; i < n; i++) m_win[i] = std::sin(kPi * i / n);   // sqrt of a periodic Hann
        m_avg.assign(n, 0.0);
        m_mask.assign(n, 0);
        m_smask.assign(n, 0);
        m_ola.assign(n, cplx());
        m_frame.resize(n);
        m_split_alpha = std::min(1.0, (n / 2) / (rate * cfg.split_avg_ms * 1e-3));
        m_split_gain = std::pow(10.0, cfg.split_db / 10.0);
        m_strong_delay.assign(2 * static_cast<size_t>(m_look), 0.0f);
        m_lead_left = static_cast<size_t>(n);
    }
}

void ImpulseRepair::process(const float *in, float *out, unsigned n, unsigned char *flags) {
    if (!m_split_n) {
        core(in, out, n, flags, nullptr);
        return;
    }
    split(in, n);
    // The weak part through the repair, the strong part delayed by the
    // repair's look-ahead to meet it.
    const unsigned k = static_cast<unsigned>(m_weak.size() / 2);
    if (k) {
        std::vector<float> rep(2 * k);
        std::vector<unsigned char> fl(k);
        core(m_weak.data(), rep.data(), k, fl.data(), m_wgate.data());
        m_strong_delay.insert(m_strong_delay.end(), m_strong.begin(), m_strong.begin() + 2 * k);
        for (unsigned i = 0; i < k; i++) {
            m_outq.push_back(rep[2 * i] + m_strong_delay[2 * i]);
            m_outq.push_back(rep[2 * i + 1] + m_strong_delay[2 * i + 1]);
            m_flagq.push_back(fl[i]);
        }
        m_strong_delay.erase(m_strong_delay.begin(), m_strong_delay.begin() + 2 * k);
        m_weak.clear();
        m_wgate.clear();
        m_strong.clear();
    }
    // The split hands out a hop at a time, but after T inputs it has always
    // produced more than T − m_split_n: m_split_n leading zeros keep the
    // output fed, so the latency is exactly m_split_n + the look-ahead.
    const size_t lead = std::min<size_t>(n, m_lead_left);
    m_lead_left -= lead;
    const size_t take = std::min<size_t>(n - lead, m_flagq.size());
    for (size_t i = 0; i < lead; i++) {
        out[2 * i] = out[2 * i + 1] = 0.0f;
        if (flags) flags[i] = 0;
    }
    for (size_t i = 0; i < take; i++) {
        out[2 * (lead + i)] = m_outq[2 * i];
        out[2 * (lead + i) + 1] = m_outq[2 * i + 1];
        if (flags) flags[lead + i] = m_flagq[i];
    }
    m_outq.erase(m_outq.begin(), m_outq.begin() + 2 * take);
    m_flagq.erase(m_flagq.begin(), m_flagq.begin() + take);
}

// Each frame of m_split_n input samples (sqrt-Hann, half overlap): the
// per-bin power averaged, bins split_db over the median bin and their
// neighbours marked strong, the rest transformed back and overlap-added.
// Once a frame is in, the first hop samples of the accumulator are final:
// that is the weak part, and the input minus it the strong part.
void ImpulseRepair::split(const float *in, unsigned n) {
    const int nn = m_split_n, hop = nn / 2;
    for (unsigned i = 0; i < n; i++) m_sin.emplace_back(in[2 * i], in[2 * i + 1]);
    while (m_sin.size() >= static_cast<size_t>(nn)) {
        for (int i = 0; i < nn; i++) m_frame[i] = m_sin[i] * m_win[i];
        fft(m_frame, false);
        // The broadband test (ALGORITHM.md §5.7, coincidence): an impulse
        // lifts most bins at once, a carrier keyed on a few. The frame is
        // impulsive when over kImpShare of the weak bins stand above
        // kImpRise times their average (noise alone: e^−2 ≈ 13.5 %).
        constexpr double kImpRise = 2.0, kImpShare = 0.3;
        bool imp = false;
        if (m_avg_init) {
            long weak = 0, up = 0;
            for (int b = 0; b < nn; b++) {
                if (m_smask[b]) continue;
                weak++;
                if (std::norm(m_frame[b]) > kImpRise * m_avg[b]) up++;
            }
            imp = weak > 0 && up > kImpShare * static_cast<double>(weak);
        }
        // The average follows each bin, an impulse clipped so it does not
        // blind the test to the next one.
        for (int b = 0; b < nn; b++) {
            const double p = std::norm(m_frame[b]);
            m_avg[b] = m_avg_init ? m_avg[b] + m_split_alpha * (std::min(p, 4.0 * m_avg[b]) - m_avg[b]) : p;
        }
        m_avg_init = true;
        std::vector<double> tmp = m_avg;
        std::nth_element(tmp.begin(), tmp.begin() + nn / 2, tmp.end());
        const double thr = tmp[nn / 2] * m_split_gain;
        for (int b = 0; b < nn; b++) m_mask[b] = m_avg[b] > thr;
        long strong = 0;
        for (int b = 0; b < nn; b++) {
            m_smask[b] = m_mask[b] || m_mask[(b + 1) % nn] || m_mask[(b + nn - 1) % nn];
            if (m_smask[b]) {
                m_frame[b] = 0.0;
                strong++;
            }
        }
        m_strong_bins += strong;
        m_frames++;
        m_imp_frames += imp;
        // The samples handed out now lie in this frame and the one before.
        const unsigned char gate = imp || m_prev_imp;
        m_prev_imp = imp;
        fft(m_frame, true);
        for (int i = 0; i < nn; i++) m_ola[i] += m_frame[i] * (m_win[i] / nn);
        for (int i = 0; i < hop; i++) {
            const cplx weak = m_ola[i], strong_part = m_sin[i] - weak;
            m_weak.push_back(static_cast<float>(weak.real()));
            m_weak.push_back(static_cast<float>(weak.imag()));
            m_strong.push_back(static_cast<float>(strong_part.real()));
            m_strong.push_back(static_cast<float>(strong_part.imag()));
            m_wgate.push_back(gate);
        }
        std::copy(m_ola.begin() + hop, m_ola.end(), m_ola.begin());
        std::fill(m_ola.begin() + (nn - hop), m_ola.end(), cplx());
        m_sin.erase(m_sin.begin(), m_sin.begin() + hop);
    }
}

ImpulseRepair::cplx ImpulseRepair::predict_fwd(size_t at) const {
    cplx s;
    if (!m_fitted || at < static_cast<size_t>(m_order)) return s;
    for (int k = 1; k <= m_order; k++) s += m_a[k] * m_buf[at - k];
    return s;
}

ImpulseRepair::cplx ImpulseRepair::predict_bwd(size_t at) const {
    cplx s;
    if (!m_fitted || at + m_order >= m_buf.size()) return s;
    for (int k = 1; k <= m_order; k++) s += std::conj(m_a[k]) * m_buf[at + k];
    return s;
}

// The AR model by the autocorrelation method on the last fit_len decided
// samples (complex Levinson–Durbin, a little diagonal loading), and the
// floor: the median prediction error power over the last m_err_window, as an
// exponential's mean.
void ImpulseRepair::refit() {
    m_since_fit = 0;
    // The share control (Config::max_share): raise the threshold 1 dB per
    // refit while the recent share is too high, relax it slowly under half.
    if (m_recent_all > 0.0) {
        m_share += 0.25 * (m_recent_rep / m_recent_all - m_share);
        if (m_share > m_cfg.max_share) m_raise_db = std::min(m_raise_db + 1.0, 40.0);
        else if (m_share < 0.5 * m_cfg.max_share) m_raise_db = std::max(m_raise_db - 0.25, 0.0);
    }
    m_recent_rep = m_recent_all = 0.0;
    const size_t n = std::min(static_cast<size_t>(m_fit), m_next);
    if (n > static_cast<size_t>(4 * m_order)) {
        const size_t s0 = m_next - n;
        std::vector<cplx> r(m_order + 1);
        for (int k = 0; k <= m_order; k++) {
            cplx acc;
            for (size_t t = s0 + k; t < m_next; t++) acc += m_buf[t] * std::conj(m_buf[t - k]);
            r[k] = acc / static_cast<double>(n);
        }
        r[0] *= 1.001;
        if (std::abs(r[0]) > 0.0) {
            std::vector<cplx> a(m_order + 1), prev;
            double e = r[0].real();
            for (int m = 1; m <= m_order && e > 0.0; m++) {
                cplx acc = r[m];
                for (int k = 1; k < m; k++) acc -= a[k] * r[m - k];
                const cplx kap = acc / e;
                prev = a;
                a[m] = kap;
                for (int k = 1; k < m; k++) a[k] = prev[k] - kap * std::conj(prev[m - k]);
                e *= 1.0 - std::norm(kap);
            }
            m_a = a;
            m_fitted = true;
        }
    }
    if (m_err.size() >= 256) {
        std::vector<double> tmp = m_err;
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        m_floor = tmp[tmp.size() / 2] / kLn2;
    }
}

// A gap starting at `at` (the first undecided sample): find its end by
// extrapolating the model across it, then fill it from both sides. Returns
// the first sample after the gap.
size_t ImpulseRepair::repair_from(size_t at) {
    const double hold = std::pow(10.0, (m_cfg.hold_db + m_raise_db) / 10.0) * m_floor;
    const size_t limit = std::min(m_buf.size() - m_order - m_cfg.guard - 1, at + m_max_gap);

    // Forward extrapolation, sample by sample, over the gap so far.
    std::vector<cplx> f;
    auto fwd = [&](size_t j) {
        cplx s;
        if (!m_fitted) return s;
        for (int k = 1; k <= m_order; k++) {
            const size_t i = j - k;
            s += m_a[k] * (i >= at ? f[i - at] : m_buf[i]);
        }
        return s;
    };
    size_t end = at, j = at;
    int quiet = 0;
    for (; j < limit; j++) {
        f.push_back(fwd(j));
        const bool loud = std::norm(m_buf[j] - f.back()) > hold;
        if (j >= at + static_cast<size_t>(m_cfg.guard) && !loud) {
            if (++quiet >= m_cfg.quiet) break;
        } else {
            quiet = 0;
            end = j + 1;
        }
    }
    // An impulse decays; a carrier keyed on (or a fade coming up) does not,
    // and stays away from the extrapolation to the end. Leave such samples
    // as they are, and let the model learn the new signal before looking
    // again.
    if (j >= limit) {
        m_hold_off = m_hold;
        m_rejected++;
        return at + 1;
    }
    end = std::min(limit, end + m_cfg.guard);
    while (f.size() < end - at) f.push_back(fwd(at + f.size()));
    const size_t len = end - at;

    // An impulse's error is broadband; a carrier keyed on, or a strong signal
    // fading, leaves a narrowband one. The spectral flatness of the error
    // over the gap (geometric over arithmetic mean of its power spectrum:
    // about 0.56 for white noise, far less for a tone burst) tells them
    // apart, however dense the impulses are. Under sfm_min the gap is left
    // alone and detection skips past it.
    if (m_cfg.sfm_min > 0.0) {
        size_t p = 16;
        while (p < len && p < 256) p <<= 1;
        std::vector<cplx> e(p);
        for (size_t q = 0; q < std::min(len, p); q++) e[q] = m_buf[at + q] - f[q];
        fft(e, false);
        double lg = 0.0, ar = 0.0;
        for (size_t q = 0; q < p; q++) {
            const double pw = std::norm(e[q]) + 1e-30;
            lg += std::log(pw);
            ar += pw;
        }
        const double sfm = std::exp(lg / static_cast<double>(p)) / (ar / static_cast<double>(p));
        if (sfm < m_cfg.sfm_min) {
            m_hold_off = len;
            m_narrow++;
            return at + 1;
        }
    }

    // Backward extrapolation from the right context, and the crossfade.
    std::vector<cplx> b(len);
    for (size_t q = len; q-- > 0;) {
        const size_t jj = at + q;
        cplx s;
        if (m_fitted)
            for (int k = 1; k <= m_order; k++) {
                const size_t i = jj + k;
                s += std::conj(m_a[k]) * (i < end ? b[i - at] : m_buf[i]);
            }
        b[q] = s;
    }
    for (size_t q = 0; q < len; q++) {
        const double w = static_cast<double>(q + 1) / static_cast<double>(len + 1);
        m_buf[at + q] = (1.0 - w) * f[q] + w * b[q];
        m_rep[at + q] = 1;
    }
    m_gaps++;
    m_repaired += static_cast<double>(len);
    m_recent_rep += static_cast<double>(len);
    return end;
}

void ImpulseRepair::core(const float *in, float *out, unsigned n, unsigned char *flags,
                         const unsigned char *gate) {
    for (unsigned i = 0; i < n; i++) {
        m_buf.emplace_back(in[2 * i], in[2 * i + 1]);
        m_rep.push_back(0);
        m_gate.push_back(gate && m_cfg.split_gate ? gate[i] : 1);
    }

    const size_t look = static_cast<size_t>(m_look);
    const size_t ahead = static_cast<size_t>(m_cfg.guard);
    while (m_buf.size() - m_next > look) {
        // Detection runs `guard` samples ahead, so a gap can start with its
        // left guard.
        const size_t j = m_next + ahead;
        const double e2 = std::norm(m_buf[j] - predict_fwd(j));
        if (m_err.size() < m_err_window) m_err.push_back(e2);
        else m_err[m_err_at] = e2;
        m_err_at = (m_err_at + 1) % m_err_window;

        const double thr = std::pow(10.0, (m_cfg.thr_db + m_raise_db) / 10.0);
        const size_t before = m_next;
        if (m_hold_off > 0) {
            m_hold_off--;
            m_next++;
        } else if (m_floor > 0.0 && e2 > thr * m_floor && m_gate[j]) {
            m_next = repair_from(m_next);
        } else {
            m_next++;
        }
        m_recent_all += static_cast<double>(m_next - before);
        if (++m_since_fit >= m_refit) refit();
    }
    // Hand out decided samples in order, look() behind the input.
    for (unsigned i = 0; i < n; i++) {
        cplx v;
        unsigned char rep = 0;
        if (m_out_at < m_next) {
            rep = m_rep[m_out_at];
            v = m_buf[m_out_at++];
        }
        if (flags) flags[i] = rep;
        out[2 * i] = static_cast<float>(v.real());
        out[2 * i + 1] = static_cast<float>(v.imag());
        m_output += 1.0;
    }
    // Keep the history the model needs.
    const size_t keep = static_cast<size_t>(std::max(m_fit, m_order) + 16);
    if (m_out_at > keep + 8192 && m_next > keep + 8192) {
        const size_t drop = std::min(m_out_at, m_next) - keep;
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<long>(drop));
        m_rep.erase(m_rep.begin(), m_rep.begin() + static_cast<long>(drop));
        m_gate.erase(m_gate.begin(), m_gate.begin() + static_cast<long>(drop));
        m_next -= drop;
        m_out_at -= drop;
    }
}
