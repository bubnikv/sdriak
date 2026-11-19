/*
 * decode_bell.cpp — the "bell" backend (see decode_bell.h).
 *
 * Bell's receiver (listing p. 189–190) is a band-pass filter, a rectifier
 * and a 100 Hz three-pole Chebyshev low-pass, sampled every 5 ms. The
 * channelizer already band-limits the channel to about ±62 Hz, so |IQ| is
 * the envelope; a two-sample mean and linear interpolation take it from the
 * channel's rate to 200 Hz.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "decode_bell.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

class BellChannel final : public SkimDecodeChannel {
public:
    BellChannel(const BellBackendConfig &cfg, double sample_rate)
        : m_cfg(cfg), m_dec(cfg.decoder), m_step(BellDecoder::kRateHz / sample_rate),
          m_a1(std::pow(0.97, kBellRate / sample_rate)), m_a2(std::pow(0.9, kBellRate / sample_rate)) {}

    bool process(const float *iq, unsigned nframes, SkimDecode *out) override {
        m_text.clear();
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

    double level() const override { return m_peak; }

    void resync() override {
        m_dec.reset();
        m_acc = 0.0;
    }

private:
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
};

}  // namespace

std::unique_ptr<SkimDecodeChannel> SkimBellBackend::channel_new(double sample_rate) const {
    return std::make_unique<BellChannel>(m_cfg, sample_rate);
}
