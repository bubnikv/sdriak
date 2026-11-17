/*
 * cw_tone.h — the tone offset from the phase slope.
 *
 * On solid mark samples the phase advance between consecutive IQ samples is
 * the tone's offset from the channel centre; a 0.01 EMA of it is the
 * reported frequency and the narrow path's derotation.
 *
 * SDRIAK: upstream's prev_i/prev_q/foff_hz fields and the phase-slope block
 * of the decoder's sample loop, as a stage; the caller decides which
 * samples measure (the decoder's mark bar).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

class CwTone {
public:
    // One IQ sample at rate; measure: it is a solid mark sample.
    void update(float i, float q, bool measure, double rate) {
        if (measure) {
            const double dr = static_cast<double>(i) * m_prev_i + static_cast<double>(q) * m_prev_q;
            const double di = static_cast<double>(q) * m_prev_i - static_cast<double>(i) * m_prev_q;
            if (dr != 0.0 || di != 0.0) {
                const double f = atan2(di, dr) * rate / (2.0 * SKIM_PI);
                m_offset_hz += 0.01 * (f - m_offset_hz);
            }
        }
        m_prev_i = i;
        m_prev_q = q;
    }

    // The tone offset from the channel centre, Hz.
    double offset_hz() const { return m_offset_hz; }
    // … taken from elsewhere: the oracle's carrier, v3's AFC.
    void set_offset_hz(double hz) { m_offset_hz = hz; }

private:
    double m_prev_i = 0, m_prev_q = 0;    // the previous IQ sample
    double m_offset_hz = 0;
};
