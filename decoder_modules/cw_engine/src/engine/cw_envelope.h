/*
 * cw_envelope.h — the CW decoder's envelope front end, one channel sample at
 * a time: |IQ|, its 3-tap moving average (what the squelch, the keying-duty
 * test and the dit bootstrap read), and the soft path's envelope — the same
 * average, or with SKIM_Vn_NARROW the carrier-centred narrow one: derotate by
 * the decoder's tone-offset estimate and low-pass (2nd-order Butterworth, fc)
 * before |·|, so a 25-35 WPM signal is read in a third of the 125 Hz channel
 * — less noise, less of the neighbours' splatter.
 *
 * SDRIAK: upstream computes this inline at the top of the decoder's sample
 * loop; this is that code as a stage, with the same float/double operations
 * in the same order. v3's matched front end (fe_*) stays in the decoder: the
 * gate, the bootstrap, the pre-roll and the lattice commit read its state.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "decode_cw_config.h"

class CwEnvelope {
public:
    struct Sample {
        float mag;    // |IQ|
        float raw;    // its 3-tap moving average (the raw channel's envelope)
        float soft;   // the soft path's envelope: the narrow one, or raw
    };

    // For a channel at rate Hz; narrow_fc = SKIM_Vn_NARROW (off when ≤ 0 or
    // ≥ 0.45·rate).
    void init(double rate, const CwEnvelopeConfig &cfg) {
        const double narrow_fc = cfg.narrow_fc;
        m_rate = rate;
        m_narrow_on = narrow_fc > 0 && narrow_fc < 0.45 * rate;
        if (m_narrow_on) {
            const double k = tan(SKIM_PI * narrow_fc / rate), q2 = sqrt(2.0);
            const double nrm = 1.0 / (1.0 + q2 * k + k * k);
            m_b0 = k * k * nrm;
            m_b1 = 2.0 * m_b0;
            m_b2 = m_b0;
            m_a1 = 2.0 * (k * k - 1.0) * nrm;
            m_a2 = (1.0 - q2 * k + k * k) * nrm;
        }
    }

    // One complex sample; tone_offset_hz = the decoder's current tone-offset
    // estimate, which the narrow path derotates by.
    Sample push(float i, float q, double tone_offset_hz) {
        const float mag = sqrtf(i * i + q * q);
        float narrow_mag = mag;
        if (m_narrow_on) {
            m_derotate_phase -= 2.0 * SKIM_PI * tone_offset_hz / m_rate;
            if (m_derotate_phase > SKIM_PI || m_derotate_phase < -SKIM_PI) {
                m_derotate_phase = remainder(m_derotate_phase, 2.0 * SKIM_PI);
            }
            const double c = cos(m_derotate_phase), sn = sin(m_derotate_phase);
            const double xin[2] = { i * c - q * sn, i * sn + q * c };
            double yo[2];
            for (int r = 0; r < 2; r++) {
                yo[r] = m_b0 * xin[r] + m_b1 * m_x[r][0] + m_b2 * m_x[r][1] -
                        m_a1 * m_y[r][0] - m_a2 * m_y[r][1];
                m_x[r][1] = m_x[r][0];
                m_x[r][0] = xin[r];
                m_y[r][1] = m_y[r][0];
                m_y[r][0] = yo[r];
            }
            narrow_mag = static_cast<float>(sqrt(yo[0] * yo[0] + yo[1] * yo[1]));
        }
        m_ma[m_n % 3] = mag;
        m_narrow_ma[m_n % 3] = narrow_mag;
        m_n++;
        const float raw = (m_ma[0] + m_ma[1] + m_ma[2]) / static_cast<float>(std::min(m_n, 3u));
        const float soft = m_narrow_on ? (m_narrow_ma[0] + m_narrow_ma[1] + m_narrow_ma[2]) /
                                             static_cast<float>(std::min(m_n, 3u))
                                       : raw;
        return { mag, raw, soft };
    }

private:
    double   m_rate = 250.0;
    bool     m_narrow_on = false;           // the narrow path
    double   m_b0 = 0, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;   // its biquad
    double   m_derotate_phase = 0.0;        // the narrow path's derotation
    double   m_x[2][2] = {}, m_y[2][2] = {};  // biquad history, [I/Q][n-1, n-2]
    float    m_ma[3] = {};                  // |IQ| moving average
    float    m_narrow_ma[3] = {};           // the narrow envelope's
    unsigned m_n = 0;                       // samples pushed
};
