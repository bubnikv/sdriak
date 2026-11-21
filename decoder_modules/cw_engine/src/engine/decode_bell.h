/*
 * decode_bell.h — E. L. Bell's 1977 Bayesian Morse decoder as a decode
 * backend ("bell"), for comparison on the bench (BELL.md).
 *
 * Per channel: complex baseband (250 Hz for the CW bank) → |IQ| → resampled
 * to Bell's 200 Hz → a peak AGC (Bell's NOISE constants assume a signal near
 * unit amplitude; his simulation fixed the level, the channelizer does not)
 * → BellDecoder (bell/bell_decoder.h): NOISE, then PROCES on every sample.
 * Text is emitted as TRELIS settles it, up to 1 s behind the input.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "decode.h"
#include "bell/bell_decoder.h"

struct BellBackendConfig {
    BellConfig decoder;
    // Bell's IF: his band-pass is two resonators at 1 kHz with pole radii
    // 0.97 and 0.9 at 8 kHz (listing p. 189, CK2 and C2), about ±35 Hz.
    // NOISE's constants are fitted to the envelope that filter leaves; on
    // the channelizer's wider channel the noise envelope moves faster, its
    // minimum falls, and NOISE reads the noise far too low. On: the same
    // two poles as complex one-pole low-passes at the channel's rate.
    bool   if_filter  = true;
    // The AGC on the 200 Hz envelope: the peak follows a rise with this
    // weight and a fall with the second (decayavg(), as morse-wip's -agc).
    double agc_attack = 10.0;     // samples: 50 ms
    double agc_decay  = 800.0;    // samples: 4 s
    // Clamp the normalised envelope to [0, 1], as morse-wip's -agc does: the
    // AGC's rise lags a mark's onset, and the overshoot reads as a jump in
    // amplitude.
    bool   agc_clamp  = true;

    // The coherent variant (decoder.coherent) needs the tone near 0 Hz: its
    // AR(1) fading model cannot follow a steady rotation. The AFC finds the
    // tone in the periodogram of the last 4 s, every 0.5 s, and derotates
    // the samples by the frequency it holds. It sees each sample
    // coh_afc_lookahead_s before the decoder does (added latency).
    bool   coh_afc    = true;
    double coh_afc_lookahead_s = 2.0;
    // The coherent decoder's sample rate: the channel's rate divided by an
    // integer. 125 Hz (8 ms) keeps the noise close to white; faster rates
    // count each noise sample as rate/125 of a white one.
    double coh_rate_hz = 125.0;

    // The gate (PLAN.md R1): v3's carrier detector on the channel's raw IQ
    // (CwCarrierDetector: a carrier gate_db over the floor for gate_s), and
    // the decoder's text passed only while a carrier was seen within the
    // last gate_hold_s: the decoder's own delay (the AFC's look-ahead and
    // the trellis) plus a margin, so the over's last characters still pass.
    bool   gate = false;
    double gate_db = 6.0;
    double gate_s = 1.0;
    double gate_hold_s = 4.0;
    // The coarse AFC (PLAN.md R2): the detector's carrier (v3's AFC)
    // derotates the channel's samples at their full rate, before the
    // decimation to coh_rate_hz, so a tone near the channel's edge does not
    // sit on the decimated rate's fold; the periodogram AFC follows what is
    // left. It fixes the edge (62.5 Hz: 0.70 → 0.04 CER) but the estimate's
    // moves from hop to hop disturb every path's gain filter: about 1 dB at
    // 12 WPM, 0.3 dB at 25 on a centred tone. Holding it, stepping it,
    // setting it once per over, smoothing it, or a 250 Hz decoder instead
    // all did worse. Off by default.
    bool   coarse_afc = false;
    // The spread measured on the station (PLAN.md R2; CwSpreadEstimator, as
    // v3's K3) and shared by all paths, in place of coh_spread: runs of
    // samples the decoder holds key-down with probability over 0.9 are its
    // marks; the paths take it calibrated (calibrated_spread() in
    // decode_bell.cpp: 1.3x under 0.5 Hz, at least 0.3 Hz, 2.5x as steep
    // above). One spread class only (coh_spreads empty).
    bool   measure_spread = false;

    // The listing alone: BellConfig::listing(), no AGC clamp.
    static BellBackendConfig listing();
    // morse-wip's behaviour: BellConfig::port() and the AGC clamp.
    static BellBackendConfig port();
};

class SkimBellBackend final : public SkimDecodeBackend {
public:
    explicit SkimBellBackend(const BellBackendConfig &cfg = BellBackendConfig()) : m_cfg(cfg) {}

    const char *name() const override { return "bell"; }
    std::unique_ptr<SkimDecodeChannel> channel_new(double sample_rate) const override;
    bool implements(SkimDecodeHook hook) const override { return hook == SkimDecodeHook::Level; }

private:
    BellBackendConfig m_cfg;
};
