/*
 * cw_levels.h — v1's level trackers, envelope squelch and keying-duty test,
 * one envelope sample at a time.
 *
 * env_hi is an instant-attack peak with a 0.8 s release, env_lo the mean of
 * the samples under the midpoint (0.3 s); the gate opens when the peak
 * stands 4× over the floor and shuts under 2.6×; keying is on while more
 * than 15 % of the samples (0.7 s EMA) sit in the top 30 % of the span, off
 * under 7 %.
 *
 * SDRIAK: upstream's "level trackers + squelch + keying-duty — v1 verbatim"
 * block of the decoder's sample loop, as a stage; same operations, same
 * order. The oracle's gate overrides are force_open() / soft_open().
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

class CwLevels {
public:
    void init(double rate) {
        m_k_hi_rel = 1.0 - exp(-1.0 / (rate * 0.8));
        m_k_lo     = 1.0 - exp(-1.0 / (rate * 0.3));
        m_k_ph     = 1.0 - exp(-1.0 / (rate * 0.7));
    }

    // One envelope sample (the raw channel's 3-tap average).
    void update(float m) {
        if (m_env_hi < 0) { m_env_hi = m_env_lo = m; }
        m_env_hi = m > m_env_hi
                       ? m
                       : m_env_hi + m_k_hi_rel * (m - m_env_hi);
        if (m < 0.5 * (m_env_lo + m_env_hi)) {
            m_env_lo += m_k_lo * (m - m_env_lo);
        }
        if (!m_gate_open) {
            m_gate_open = m_env_hi > 4.0 * m_env_lo + 1e-6;
        } else if (m_env_hi < 2.6 * m_env_lo + 1e-6) {
            m_gate_open = false;
        }
        const double span0 = m_env_hi - m_env_lo;
        m_p_high += m_k_ph *
                    ((m > m_env_lo + 0.7 * span0 ? 1.0 : 0.0) - m_p_high);
        if (!m_keying) {
            m_keying = m_p_high > 0.15;
        } else if (m_p_high < 0.07) {
            m_keying = false;
        }
    }

    // The oracle's gate (SKIM_ORACLE, measurement only): open and keying.
    void force_open() {
        m_gate_open = true;
        m_keying = true;
    }
    // The oracle's soft gate: the envelope bar at 2×, the keying-duty bar at
    // its off level.
    void soft_open() {
        m_gate_open = m_env_hi > 2.0 * m_env_lo + 1e-6;
        m_keying = m_keying || m_p_high > 0.07;
    }

    double env_hi() const { return m_env_hi; }    // peak
    double env_lo() const { return m_env_lo; }    // floor
    bool   gate_open() const { return m_gate_open; }  // the envelope squelch
    bool   keying() const { return m_keying; }    // keying duty seen

private:
    double m_env_hi = -1.0, m_env_lo = -1.0;      // < 0 = no sample yet
    double m_k_hi_rel = 0, m_k_lo = 0, m_k_ph = 0;
    bool   m_gate_open = false;
    double m_p_high = 0.0;                        // EMA of samples in the top 30 %
    bool   m_keying = false;
};
