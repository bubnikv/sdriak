/*
 * decode_bell.cpp — the "bell" backend (see decode_bell.h).
 *
 * Bell's receiver (listing p. 189–190) is a band-pass filter, a rectifier
 * and a 100 Hz three-pole Chebyshev low-pass, sampled every 5 ms. The
 * channelizer already band-limits the channel to about ±62 Hz, so |IQ| is
 * the envelope; a two-sample mean and linear interpolation take it from the
 * channel's rate to 200 Hz.
 *
 * The coherent variant (BellConfig::coherent) skips all of that: it takes
 * every 2nd complex channel sample (8 ms at 250 Hz), divided by the square
 * root of the band floor so the noise has variance 1, and has no IF
 * filter, AGC or clamp.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "decode_bell.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <vector>

BellBackendConfig BellBackendConfig::listing() {
    BellBackendConfig c;
    c.decoder = BellConfig::listing();
    c.agc_clamp = false;
    return c;
}

BellBackendConfig BellBackendConfig::port() {
    BellBackendConfig c;
    c.decoder = BellConfig::port();
    c.agc_clamp = true;
    return c;
}

namespace {

// The coherent decoder keeps every D-th channel sample, D = rate /
// coh_rate_hz: at 250 Hz and 125, every 2nd, 8 ms apart, with nearly white
// noise of the channel's own variance (the passband is about ±62 Hz).
int coherent_decimation(const BellBackendConfig &cfg, double sample_rate) {
    return std::max(1, static_cast<int>(std::lround(sample_rate / cfg.coh_rate_hz)));
}

BellConfig decoder_config(const BellBackendConfig &cfg, double sample_rate) {
    BellConfig c = cfg.decoder;
    if (c.coherent) c.coh_dt_ms = static_cast<float>(1000.0 * coherent_decimation(cfg, sample_rate) / sample_rate);
    return c;
}

// The AFC's periodogram: 4 s of kept samples, a power of two.
int afc_size(double kept_rate) {
    int n = 64;
    while (n < 4.0 * kept_rate) n <<= 1;
    return n;
}

class BellChannel final : public SkimDecodeChannel {
public:
    BellChannel(const BellBackendConfig &cfg, double sample_rate)
        : m_cfg(cfg), m_dec(decoder_config(cfg, sample_rate)), m_step(BellDecoder::kRateHz / sample_rate),
          m_a1(std::pow(0.97, kBellRate / sample_rate)), m_a2(std::pow(0.9, kBellRate / sample_rate)),
          m_decim(coherent_decimation(cfg, sample_rate)),
          m_block(std::max(1, static_cast<int>(0.25 * sample_rate / m_decim))),
          // Kept faster than the ±62 Hz passband needs, neighbouring noise
          // samples correlate; counting each as 1/(rate/125) of a white one
          // keeps the evidence per element right.
          m_noise_scale(std::max(1.0, sample_rate / m_decim / 125.0)),
          m_dec_dt_ms(1000.0 * m_decim / sample_rate),
          m_afcn(afc_size(sample_rate / m_decim)),
          m_ring(m_afcn), m_fft(m_afcn), m_pow(m_afcn) {}

    void set_floor(double noise_pw) override { m_floor = noise_pw; }
    void set_repair_weights(const float *w, unsigned nframes) override { m_w.assign(w, w + nframes); }

    bool process(const float *iq, unsigned nframes, SkimDecode *out) override {
        m_text.clear();
        if (m_cfg.decoder.coherent) {
            for (unsigned k = 0; k < nframes; k++) {
                if (k < m_w.size()) m_wmax = std::max(m_wmax, m_w[k]);
                if (++m_phase < m_decim) continue;
                m_phase = 0;
                coherent_sample(iq[2 * k], iq[2 * k + 1], m_wmax);
                m_wmax = 0.0f;
            }
            m_w.clear();             // the weights belonged to this block
            return emit(out);
        }
        for (unsigned k = 0; k < nframes; k++) {
            float re = iq[2 * k], im = iq[2 * k + 1];
            if (m_cfg.if_filter) {
                m_r1 = m_a1 * m_r1 + (1.0 - m_a1) * re;
                m_i1 = m_a1 * m_i1 + (1.0 - m_a1) * im;
                m_r2 = m_a2 * m_r2 + (1.0 - m_a2) * m_r1;
                m_i2 = m_a2 * m_i2 + (1.0 - m_a2) * m_i1;
                re = static_cast<float>(m_r2);
                im = static_cast<float>(m_i2);
            }
            const float e = std::hypot(re, im);
            const float cur = 0.5f * (e + m_prev_raw);
            m_prev_raw = e;
            // One output each time the 200 Hz clock passes, interpolated
            // between the previous and this input sample.
            m_acc += m_step;
            while (m_acc >= 1.0) {
                // m_acc − m_step is where the clock stood at the previous input.
                const double frac = std::clamp((1.0 - (m_acc - m_step)) / m_step, 0.0, 1.0);
                sample(m_prev + static_cast<float>(frac) * (cur - m_prev));
                m_acc -= 1.0;
            }
            m_prev = cur;
        }
        return emit(out);
    }

    double level() const override {
        return m_cfg.decoder.coherent ? std::sqrt(m_dec.signal_power() * noise()) : m_peak;
    }

    void resync() override {
        m_dec.reset();
        m_acc = 0.0;
        m_phase = 0;
        m_rat = m_rfill = m_since = 0;
        m_afc_lock = false;
        m_afc_hz = m_phi = 0.0;
        m_delay.clear();
        m_delay_w.clear();
    }

    double tone_offset_hz() const override { return m_cfg.decoder.coherent && m_cfg.coh_afc ? afc_hz() : 0.0; }

private:
    // The noise variance per kept sample: the pipeline's band floor, or
    // without one the quietest 0.25 s block of the last 4 s.
    double noise() const {
        if (m_floor > 0.0) return m_floor;
        return m_blocks_min > 0.0 ? m_blocks_min : 1e-12;
    }

    void coherent_sample(float re, float im, float w) {
        m_bacc += static_cast<double>(re) * re + static_cast<double>(im) * im;
        if (++m_bcnt == m_block) {
            m_blocks[m_bat] = m_bacc / m_bcnt;
            m_bat = (m_bat + 1) % kBlocks;
            m_bfill = std::min(m_bfill + 1, kBlocks);
            m_blocks_min = *std::min_element(m_blocks, m_blocks + m_bfill);
            m_bacc = 0.0;
            m_bcnt = 0;
        }
        const double n0 = noise();
        if (m_cfg.coh_afc) {
            // The AFC sees each sample coh_afc_lookahead_s before the
            // decoder, so the first marks of an over are derotated with a
            // locked estimate.
            afc_push(re, im);
            m_delay.emplace_back(re, im);
            m_delay_w.push_back(w);
            const size_t look = static_cast<size_t>(std::lround(m_cfg.coh_afc_lookahead_s * 1000.0 / m_dec_dt_ms));
            if (m_delay.size() <= look) return;
            re = m_delay.front().real();
            im = m_delay.front().imag();
            m_delay.pop_front();
            w = m_delay_w.front();
            m_delay_w.pop_front();
            m_phi = std::remainder(m_phi + 2.0 * kPi * m_afc_hz * m_dec_dt_ms * 1e-3, 2.0 * kPi);
            const double c = std::cos(m_phi), s = std::sin(m_phi);
            const double r2 = re * c + im * s, i2 = im * c - re * s;
            re = static_cast<float>(r2);
            im = static_cast<float>(i2);
        }
        if (!(m_floor > 0.0) && m_bfill == 0) return;       // nothing to scale by yet
        const float s = static_cast<float>(1.0 / std::sqrt(n0 * m_noise_scale));
        m_dec.push_iq(re * s, im * s, m_text, w);
    }

    // AFC: every 0.5 s, the Hann-windowed periodogram of the last m_afcn
    // samples (4 s). A peak kAfcPeak above the spectrum's median is
    // the tone; its frequency, interpolated between bins, is followed with
    // weight kAfcGain. The tone integrates coherently over seconds, so the
    // estimate holds well below the SNR at which characters decode.
    void afc_push(float re, float im) {
        m_ring[m_rat] = {re, im};
        m_rat = (m_rat + 1) % m_afcn;
        m_rfill = std::min(m_rfill + 1, m_afcn);
        if (++m_since < (m_afcn / 8) || m_rfill < m_afcn / 4) return;
        m_since = 0;
        for (int i = 0; i < m_afcn; i++) m_fft[i] = 0.0;
        for (int i = 0; i < m_rfill; i++) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / m_rfill);
            m_fft[i] = w * m_ring[(m_rat - m_rfill + i + m_afcn) % m_afcn];
        }
        fft(m_fft);
        for (int i = 0; i < m_afcn; i++) m_pow[i] = std::norm(m_fft[i]);
        const int k = static_cast<int>(std::max_element(m_pow.begin(), m_pow.end()) - m_pow.begin());
        const double peak = m_pow[k];
        m_sorted = m_pow;
        std::nth_element(m_sorted.begin(), m_sorted.begin() + m_afcn / 2, m_sorted.end());
        if (peak < kAfcPeak * m_sorted[m_afcn / 2]) return;
        const double pm = m_pow[(k + m_afcn - 1) % m_afcn], pp = m_pow[(k + 1) % m_afcn];
        const double den = pm - 2.0 * peak + pp;
        const double d = den < 0.0 ? 0.5 * (pm - pp) / den : 0.0;
        double bin = k + d;
        if (bin >= m_afcn / 2) bin -= m_afcn;
        const double f = bin * 1000.0 / m_dec_dt_ms / m_afcn;
        m_afc_hz = m_afc_lock ? m_afc_hz + kAfcGain * (f - m_afc_hz) : f;
        m_afc_lock = true;
    }

    // In-place radix-2 FFT, size m_afcn.
    static void fft(std::vector<std::complex<double>> &x) {
        const int n = static_cast<int>(x.size());
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / len);
            for (int i = 0; i < n; i += len) {
                std::complex<double> w = 1.0;
                for (int j = 0; j < len / 2; j++) {
                    const std::complex<double> u = x[i + j], v = x[i + j + len / 2] * w;
                    x[i + j] = u + v;
                    x[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    double afc_hz() const { return m_afc_hz; }

    bool emit(SkimDecode *out) {
        if (m_text.empty()) return false;
        std::memset(out, 0, sizeof(*out));
        const size_t n = std::min(m_text.size(), static_cast<size_t>(SkimDecode::kTextMax - 1));
        std::memcpy(out->text, m_text.data(), n);
        out->text[n] = '\0';
        out->confidence = m_dec.best_prob();
        out->freq_offset_hz = 0.0;
        out->speed = m_dec.speed_wpm();
        out->snr_db = 0.0;
        out->pane_own = false;
        return true;
    }

    void sample(float x) {
        // decayavg(avg, in, w) = avg + (in − avg) / w
        m_peak += (x - m_peak) / (x > m_peak ? m_cfg.agc_attack : m_cfg.agc_decay);
        float y = m_peak > 0.0 ? static_cast<float>(x / m_peak) : 0.0f;
        if (m_cfg.agc_clamp) y = std::clamp(y, 0.0f, 1.0f);
        m_dec.push(y, m_text);
    }

    static constexpr double kBellRate = 8000.0;   // the rate of Bell's receiver

    BellBackendConfig m_cfg;
    BellDecoder m_dec;
    const double m_step;          // 200 Hz outputs per input sample
    const double m_a1, m_a2;      // the IF poles at this rate
    double m_r1 = 0.0, m_i1 = 0.0, m_r2 = 0.0, m_i2 = 0.0;
    double m_acc = 0.0;
    float  m_prev_raw = 0.0f, m_prev = 0.0f;
    double m_peak = 0.0;
    std::string m_text;

    // Coherent: the decimation and its phase, the band floor, and the
    // fallback noise estimate (0.25 s block powers, the last kBlocks).
    static constexpr int kBlocks = 16;
    const int m_decim, m_block;
    int    m_phase = 0;
    double m_floor = 0.0;
    double m_bacc = 0.0, m_blocks[kBlocks] = {}, m_blocks_min = 0.0;
    int    m_bcnt = 0, m_bat = 0, m_bfill = 0;
    // AFC: the last m_afcn raw kept samples (4 s), the FFT scratch, the
    // frequency in use and the derotation phase.
    static constexpr double kPi = 3.14159265358979323846;
    // kAfcPeak: the largest of 512–1024 noise-only bins is about 9–10× their
    // median and exceeds 30× with probability ~1e-6.
    static constexpr double kAfcPeak = 30.0, kAfcGain = 0.3;
    const double m_noise_scale;
    const double m_dec_dt_ms;
    const int    m_afcn;
    std::vector<std::complex<double>> m_ring, m_fft;
    std::vector<double> m_pow, m_sorted;
    int    m_rat = 0, m_rfill = 0, m_since = 0;
    bool   m_afc_lock = false;
    double m_afc_hz = 0.0, m_phi = 0.0;
    std::deque<std::complex<float>> m_delay;
    std::deque<float> m_delay_w;             // the repair weight of each delayed sample
    // Repair weights of the current block (set_repair_weights), and the
    // largest since the last kept sample.
    std::vector<float> m_w;
    float m_wmax = 0.0f;
};

}  // namespace

std::unique_ptr<SkimDecodeChannel> SkimBellBackend::channel_new(double sample_rate) const {
    return std::make_unique<BellChannel>(m_cfg, sample_rate);
}
