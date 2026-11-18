/*
 * cw_frontend.h — v3's front end: the carrier detector + AFC, the matched
 * filter on the derotated I/Q, and the calibrated likelihoods.
 *
 * SDRIAK: upstream's fe_* fields of the decoder state and its fe_fft(),
 * fe_select() (std::nth_element at its two uses), fe_noise_refresh(),
 * fe_track(), fe_sample(), fe_backfill(), fe_hop(), fe_llr() and
 * fe_snr500(), as a stage of five parts by purpose:
 *
 *   CwIqRing           the raw I/Q ring (the detector's and the backfill's)
 *   CwCarrierDetector  spectrum, floor, candidate state, carrier (AFC): fe_hop
 *   CwMatchedFilter    derotator, low-pass, half-dit coherent sum: fe_sample's
 *                      first half
 *   CwPowerStats       σ0² / σ1² / presence on the matched power: fe_sample's
 *                      second half, fe_noise_refresh, fe_llr, fe_snr500
 *   CwMatchedLevels    the matched-amplitude trackers: fe_track
 *
 * Upstream's FE_* constants are the parts' kName members: FE_N kFftSize,
 * FE_HOP kHopFrames, FE_SUMW2 kHannPowerSum, FE_SPAN_HZ kSearchSpanHz,
 * FE_SPEC_S kSpectrumAvgSeconds, FE_AFC_S kCarrierAvgSeconds, FE_LOST_S
 * kLostAfterSeconds (the detector); FE_LP_HZ kLowPassHz, FE_MA_CAP
 * kSumCapacity, FE_MA_DFLT kDefaultSumLength, FE_MA_MAX kMaxSumLength (the
 * filter); FE_PW_S kNoiseWindowSeconds, FE_PW_EVERY kNoiseRefreshSeconds,
 * FE_LLR_MAX kMaxLlr, FE_SIG_MIN kMinMarkToNoise, FE_R_CAP kMaxMarkToNoise,
 * FE_PK_S kPeakReleaseSeconds, FE_MK_S kMarkAvgSeconds, FE_KEY_P
 * kKeyedShare, FE_PRES_S kPresenceAvgSeconds (the power statistics);
 * FE_RING_S CwFrontEnd::kRingSeconds. FE_PW_Q and FE_DEAD_S, unused
 * upstream (the dead-air time is SKIM_V3_DEAD_S), are not taken.
 *
 * CwFrontEnd holds them, the weak/strong view and the backfill. The bodies
 * are upstream's, each in the part that owns its data; what a part reads of
 * another comes in as an argument. Arithmetic and order are unchanged.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <cmath>

#include "skim_common.h"
#include "cw_clock.h"
#include "cw_debug.h"
#include "cw_preroll.h"
#include "decode_cw_config.h"

// v3 front end (docs/CW-V3.md).
// Every channel runs a cheap carrier detector on a 64-point spectrum of its
// 250 Hz stream (2 s EMA, CFAR against the band floor); a detected carrier
// makes the channel a CANDIDATE: the raw I/Q ring of the last 6 s is
// derotated onto the carrier, low-passed and summed coherently over half a
// dit (the matched filter for on-off keying), and the lattice reads
// calibrated log-likelihood ratios of that statistic against the measured
// noise power — not v2's heuristic on the channel's wide envelope.

// --- the raw I/Q ring ----------------------------------------------------------

// CwFrontEnd::kRingSeconds of the channel's raw I/Q (the pre-roll's
// depth): the detector's spectrum source, and what the backfill runs the
// matched path over.
class CwIqRing {
public:
    void init(unsigned size) {
        m_size = size;
        m_iq.assign(static_cast<size_t>(2 * m_size), 0.0f);
    }

    // One raw frame in.
    void push(float i, float q) {
        const unsigned imask = m_size - 1;
        m_iq[2 * (m_head & imask)]     = i;
        m_iq[2 * (m_head & imask) + 1] = q;
        m_head++;
    }

    unsigned head() const { return m_head; }        // frames written
    unsigned size() const { return m_size; }        // frames held (power of 2)
    // Frame number f (a write count, f < head()).
    float i(unsigned f) const { return m_iq[2 * (f & (m_size - 1))]; }
    float q(unsigned f) const { return m_iq[2 * (f & (m_size - 1)) + 1]; }

private:
    std::vector<float> m_iq;              // 2 floats a frame
    unsigned m_size = 0;
    unsigned m_head = 0;
};

// --- the carrier detector + AFC ------------------------------------------------

class CwCarrierDetector {
    static constexpr auto kFftSize = 64;          // detector FFT: bins of rate/64 Hz
    static constexpr auto kHopFrames = 64;        // frames per spectrum (256 ms)
    static constexpr auto kHannPowerSum = (3.0 * kFftSize / 8.0);  // Σ hann²: white
                                                                   // noise power per bin
    static constexpr auto kSearchSpanHz = 70.0;   // carrier search, ± the channel
                                                  // centre
    static constexpr auto kSpectrumAvgSeconds = 2.0;  // the spectrum's EMA
    static constexpr auto kCarrierAvgSeconds = 1.0;   // the carrier estimate's EMA
    static constexpr auto kLostAfterSeconds = 3.0;    // a candidate without its
                                                      // carrier this long is lost

public:
    enum class Event { None, Born, Lost };

    void init(const CwFrontEndConfig &cfg) {
        m_detect_bar     = pow(10.0, cfg.det_db / 10.0);
        m_detect_seconds = cfg.det_s;
        m_band_floor     = NAN;
    }

    // set_floor(): the band's noise power at this channel.
    void set_floor(double noise_pw) { m_band_floor = noise_pw; }
    double band_floor() const { return m_band_floor; }

    bool   candidate() const { return m_candidate; }    // a carrier is detected here
    double carrier_hz() const { return m_carrier_hz; }  // the AFC, channel-relative

    // After each raw frame (the ring holds `head` frames): true when a hop is
    // due — every kHopFrames, once a full FFT's worth is in.
    bool hop_due(unsigned head) {
        if (++m_since_hop >= kHopFrames && head >= kFftSize) {
            m_since_hop = 0;
            return true;
        }
        return false;
    }

    // A candidate by decree (the oracle's gate), at ring frame `now`.
    void force(unsigned now) {
        m_candidate = true;
        m_last_detection = now;
    }

    // One detector hop: the spectrum of the last kFftSize frames (Hann) into the
    // EMA; the strongest bin within ±kSearchSpanHz against the floor decides the
    // candidate state and steers the carrier estimate. Born: the channel just
    // became a candidate; Lost: its carrier is gone.
    Event hop(const CwIqRing &ring, double rate, uint64_t t, const CwTrace &tr) {
        struct Window { float w[kFftSize]; };
        static const Window hann = [] {
            Window h{};
            for (unsigned k = 0; k < kFftSize; k++) { h.w[k] = 0.5f - 0.5f * static_cast<float>(cos(2.0 * SKIM_PI * k / kFftSize)); }
            return h;
        }();
        const float *win = hann.w;
        float re[kFftSize], im[kFftSize];
        for (unsigned k = 0; k < kFftSize; k++) {
            const unsigned f = ring.head() - kFftSize + k;
            re[k] = ring.i(f) * win[k];
            im[k] = ring.q(f) * win[k];
        }
        fft(re, im);
        const unsigned  warm = static_cast<unsigned>(kSpectrumAvgSeconds * rate / kHopFrames);
        const double a = m_spectrum_hops < warm ? 1.0 / (m_spectrum_hops + 1)
                                            : static_cast<double>(kHopFrames) / (kSpectrumAvgSeconds * rate);
        for (unsigned k = 0; k < kFftSize; k++) {
            const float p = re[k] * re[k] + im[k] * im[k];
            m_spectrum[k] += static_cast<float>(a * (p - m_spectrum[k]));
        }
        m_spectrum_hops++;
        if (m_spectrum_hops < 4)
            return Event::None;
        // the floor per bin: the band's (set_floor) or this spectrum's median
        // The floor per bin over the carrier search span only: outside the
        // channel's passband the bins are filtered down and a median over all
        // of them would call plain in-band noise a carrier. The band floor
        // (set_floor) when there is one — a channel full of one station cannot
        // lift it; its in-band density is rate/ENBW (2/0.851) of a white one.
        const int span_bins = static_cast<int>(kSearchSpanHz * kFftSize / rate);
        // A carrier belongs to the channel whose centre is nearest (±¼ of the
        // 2×-oversampled rate): the overlapping neighbour sees it too, and two
        // candidates on one station only cost a second lattice and a ghost.
        const int own_bins = static_cast<int>(0.25 * kFftSize + 0.5);
        // … and never under this channel's own median there: on a crowded
        // stretch the channel's noise (splatter, weak QRM) sits dB above the
        // band's quiet 20th percentile and every bin would pass.
        if ((m_spectrum_hops & 3) == 0 || m_local_floor <= 0) {   // every 4th hop
            float sv[kFftSize];
            unsigned ns = 0;
            for (int k = -span_bins; k <= span_bins; k++) { sv[ns++] = m_spectrum[static_cast<unsigned>((k + kFftSize) % kFftSize)]; }
            std::nth_element(sv, sv + ns / 2, sv + ns);
            m_local_floor = sv[ns / 2];
        }
        double floor = m_local_floor;
        if (m_band_floor > 0 && !std::isnan(m_band_floor)) {
            floor = std::max(floor, m_band_floor * kHannPowerSum * 2.0 / 0.851);
        }
        if (floor <= 0)
            return Event::None;
        int   peak_bin = 0;
        double peak = -1.0;
        for (int k = -span_bins; k <= span_bins; k++) {
            const unsigned ix = static_cast<unsigned>((k + kFftSize) % kFftSize);
            if (m_spectrum[ix] > peak) {
                peak = m_spectrum[ix];
                peak_bin = k;
            }
        }
        // not ours: the neighbour's carrier, or its skirt on our edge
        if (std::abs(peak_bin) > own_bins) { peak = 0.0; }
        const double prominence = peak / floor;
        m_prominence = prominence;
        const double bar = m_detect_bar;
        if (prominence >= bar) {
            if (m_hops_over_bar < UINT_MAX) { m_hops_over_bar++; }
        } else {
            m_hops_over_bar = 0;
        }
        if (prominence >= 0.5 * bar) {
            // the carrier: a parabola through the log spectrum round the peak
            const double lm = log(static_cast<double>(m_spectrum[(peak_bin - 1 + kFftSize) % kFftSize]) + 1e-30);
            const double l0 = log(peak + 1e-30);
            const double lp = log(static_cast<double>(m_spectrum[(peak_bin + 1 + kFftSize) % kFftSize]) + 1e-30);
            const double den = lm - 2.0 * l0 + lp;
            const double dlt = den < 0 ? std::clamp(0.5 * (lm - lp) / den, -0.5, 0.5) : 0.0;
            const double f = (peak_bin + dlt) * rate / kFftSize;
            if (!m_candidate && m_hops_over_bar <= 1) {
                m_carrier_hz = f;
            } else {
                m_carrier_hz += (kHopFrames / (kCarrierAvgSeconds * rate)) * (f - m_carrier_hz);
            }
            m_last_detection = ring.head();
        }
        const unsigned need = std::max(1u, static_cast<unsigned>(m_detect_seconds * rate / kHopFrames));
        if (!m_candidate && m_hops_over_bar >= need) {
            m_candidate = true;
            CW_DBG(tr, "[candidate: carrier %+.1f Hz, %.1f dB over the floor, t=%lu]\n",
                    m_carrier_hz, 10.0 * log10(prominence), (unsigned long)t);
            return Event::Born;
        }
        if (m_candidate && ring.head() - m_last_detection > static_cast<unsigned>(kLostAfterSeconds * rate)) {
            m_candidate = false;
            m_hops_over_bar = 0;
            CW_DBG(tr, "[candidate lost, t=%lu]\n", (unsigned long)t);
            return Event::Lost;
        }
        return Event::None;
    }

private:
    // In-place radix-2 FFT of kFftSize complex points: no FFTW planner, no lock, a
    // few thousand flops per hop.
    static void fft(float *re, float *im) {
        struct Twiddles { float re[kFftSize / 2], im[kFftSize / 2]; };
        static const Twiddles tw = [] {
            Twiddles w{};
            for (unsigned k = 0; k < kFftSize / 2; k++) {
                w.re[k] = static_cast<float>(cos(-2.0 * SKIM_PI * k / kFftSize));
                w.im[k] = static_cast<float>(sin(-2.0 * SKIM_PI * k / kFftSize));
            }
            return w;
        }();
        const float *twr = tw.re, *twi = tw.im;
        for (unsigned i = 1, j = 0; i < kFftSize; i++) {
            unsigned bit = kFftSize >> 1;
            for (; j & bit; bit >>= 1) { j ^= bit; }
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (unsigned len = 2; len <= kFftSize; len <<= 1) {
            const unsigned step = kFftSize / len;
            for (unsigned i = 0; i < kFftSize; i += len) {
                for (unsigned k = 0; k < len / 2; k++) {
                    const float wr = twr[k * step], wi = twi[k * step];
                    const unsigned a = i + k, b = a + len / 2;
                    const float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }

    unsigned m_since_hop = 0;             // frames since the last hop
    double   m_detect_bar = 0;            // SKIM_V3_DET_DB, linear
    double   m_detect_seconds = 0;        // SKIM_V3_DET_S
    float    m_spectrum[kFftSize] = {};   // EMA power spectrum
    unsigned m_spectrum_hops = 0;         // hops in the spectrum so far
    double   m_band_floor = 0;            // set_floor(): channel noise power
    double   m_prominence = 0;            // detector prominence, linear
    double   m_local_floor = 0;           // the span's median bin, cached
    unsigned m_hops_over_bar = 0;         // hops over the bar in a row
    bool     m_candidate = false;         // a carrier is detected here
    unsigned m_last_detection = 0;        // ring frame of the last detection
    double   m_carrier_hz = 0;            // the carrier (AFC), channel-relative
};

// --- the matched filter -----------------------------------------------------------

// One raw frame through the candidate path: derotate onto the carrier, the
// low-pass, the coherent sum over half a dit.
class CwMatchedFilter {
    static constexpr auto kLowPassHz = 60.0;      // complex low-pass ahead of the sum
    static constexpr auto kSumCapacity = 64;      // the sum's ring, frames (½ dit ≤ 38)
    static constexpr auto kDefaultSumLength = 5;  // before the clock: ~30 WPM
    static constexpr auto kMaxSumLength = 10;     // full matched gain down to ~15 WPM

public:
    void init() {
        m_sum_ring.assign(static_cast<size_t>(2 * kSumCapacity), 0.0f);
        m_sum_cap = kSumCapacity;
        m_sum_len = kDefaultSumLength;
        m_phasor_re = 1.0;
    }

    // A fresh start (the backfill).
    void reset() {
        m_phasor_re = 1.0;
        m_phasor_im = 0.0;
        m_phasor_n  = 0;
        m_lowpass = {};
        std::fill_n(m_sum_ring.begin(), 2 * m_sum_cap, 0.0f);
        m_sum_pos  = 0;
        m_sum_used = 0;
    }

    // The sum's length L, frames.
    unsigned length() const { return m_sum_len; }

    // One raw frame, the carrier at fc_hz: the matched power r² = |u|².
    double power(float i, float q, double fc_hz, double rate, const CwClock &clk) {
        const double w  = -2.0 * SKIM_PI * fc_hz / rate;
        const double cw = cos(w), sw = sin(w);
        const double nr = m_phasor_re * cw - m_phasor_im * sw;
        const double ni = m_phasor_re * sw + m_phasor_im * cw;
        m_phasor_re = nr;
        m_phasor_im = ni;
        if ((++m_phasor_n & 255) == 0) {
            const double g = 1.0 / sqrt(nr * nr + ni * ni);
            m_phasor_re *= g;
            m_phasor_im *= g;
        }
        double x[2] = { i * nr - q * ni, i * ni + q * nr };
        static double b0, b1, b2, a1, a2, for_rate;
        if (for_rate != rate) {
            const double k = tan(SKIM_PI * kLowPassHz / rate), q2 = sqrt(2.0);
            const double nrm = 1.0 / (1.0 + q2 * k + k * k);
            b0 = k * k * nrm;
            b1 = 2.0 * b0;
            b2 = b0;
            a1 = 2.0 * (k * k - 1.0) * nrm;
            a2 = (1.0 - q2 * k + k * k) * nrm;
            for_rate = rate;
        }
        for (int r = 0; r < 2; r++) {
            const double y = b0 * x[r] + b1 * m_lowpass.x[r][0] + b2 * m_lowpass.x[r][1] -
                             a1 * m_lowpass.y[r][0] - a2 * m_lowpass.y[r][1];
            m_lowpass.x[r][1] = m_lowpass.x[r][0];
            m_lowpass.x[r][0] = x[r];
            m_lowpass.y[r][1] = m_lowpass.y[r][0];
            m_lowpass.y[r][0] = y;
            x[r] = y;
        }
        // The sum's length follows the UNBIASED dit, (dit + element space) / 2: the sum
        // itself stretches marks and shrinks spaces by about its own length, so
        // a length taken from the marks alone fed back on itself — longer sum,
        // longer marks, slower clock, longer sum — until the clock sat at its
        // 4 WPM floor and the channel never read again (rec 2, 40 m: 23 channels
        // dead for hours). A stale element space (another station's) is ignored.
        if (clk.dit() > 0) {
            const double u = clk.element_space() > 0.25 * clk.dit() && clk.element_space() < 1.25 * clk.dit()
                                 ? 0.5 * (clk.dit() + clk.element_space()) : clk.dit();
            m_sum_len = static_cast<unsigned>(std::clamp(lround(0.5 * u), 2L,
                                      std::min<long>(kMaxSumLength, static_cast<long>(m_sum_cap) - 1)));
        }
        // the running sum: one frame in, the one ma_len back out; summed afresh
        // when the length changes and every 1024 frames (float drift)
        const unsigned out_ix = (m_sum_pos + m_sum_cap - m_sum_len) % m_sum_cap;
        if (m_sum_used == m_sum_len && (m_phasor_n & 1023) != 0) {
            m_sum_re += x[0] - m_sum_ring[2 * out_ix];
            m_sum_im += x[1] - m_sum_ring[2 * out_ix + 1];
            m_sum_ring[2 * m_sum_pos]     = static_cast<float>(x[0]);
            m_sum_ring[2 * m_sum_pos + 1] = static_cast<float>(x[1]);
            m_sum_pos = (m_sum_pos + 1) % m_sum_cap;
        } else {
            m_sum_ring[2 * m_sum_pos]     = static_cast<float>(x[0]);
            m_sum_ring[2 * m_sum_pos + 1] = static_cast<float>(x[1]);
            m_sum_pos = (m_sum_pos + 1) % m_sum_cap;
            m_sum_re = m_sum_im = 0.0;
            for (unsigned k = 0; k < m_sum_len; k++) {
                const unsigned ix = (m_sum_pos + m_sum_cap - 1 - k) % m_sum_cap;
                m_sum_re += m_sum_ring[2 * ix];
                m_sum_im += m_sum_ring[2 * ix + 1];
            }
            m_sum_used = m_sum_len;
        }
        const double sr = m_sum_re / m_sum_len, si = m_sum_im / m_sum_len;
        return sr * sr + si * si;
    }

private:
    double   m_phasor_re = 0, m_phasor_im = 0;  // derotator phasor
    unsigned m_phasor_n = 0;              // frames derotated (renormalise, and
                                          // the MA's periodic re-sum)
    struct {
        double x[2][2], y[2][2];            // [I/Q][n-1, n-2]
    } m_lowpass = {};                     // kLowPassHz biquad history
    std::vector<float> m_sum_ring;        // the coherent sum's ring, 2 floats a frame
    double   m_sum_re = 0, m_sum_im = 0;  // its running sum over m_sum_len
    unsigned m_sum_used = 0;              // the length that sum is over
    unsigned m_sum_len = 0;               // L, frames
    unsigned m_sum_cap = 0;               // the ring's frames
    unsigned m_sum_pos = 0;               // the ring's next write
};

// --- the power statistics -----------------------------------------------------

// On the matched power r²: σ0² (the noise, a percentile), σ1² (the
// station's key-down power) and the presence log-odds; the likelihoods and
// the SNR they calibrate. L: the matched filter's length.
class CwPowerStats {
    static constexpr auto kNoiseWindowSeconds = 4.0;   // r² ring for the noise
                                                       // percentile
    static constexpr auto kNoiseRefreshSeconds = 0.5;  // σ0² re-measured this often
    static constexpr auto kMaxLlr = 4.0;          // per-sample llr clamp, after the
                                                  // 1/L weight
    static constexpr auto kMinMarkToNoise = 2.0;  // σ1² ≥ this × σ0² (0 dB in the
                                                  // filter)
    // σ1²/σ0² as the LLR sees it, at most: ln R/(R−1) = ¼ puts the zero
    // crossing at HALF the key-down amplitude — where the half-dit MA crosses a
    // rectangular edge, so marks and spaces keep their true length. Stronger
    // stations would push the crossing down onto the MA's ramps (dits fuse);
    // weaker ones keep the calibrated ratio, the crossing rises above ½.
    static constexpr auto kMaxMarkToNoise = 9.0;
    static constexpr auto kPeakReleaseSeconds = 2.0;  // mark power: the held peak's
                                                      // decay
    static constexpr auto kMarkAvgSeconds = 0.25; // … and the mean of the marks
                                                  // under it
    static constexpr auto kKeyedShare = 0.45;     // a keyed carrier's share of mark
                                                  // time (the presence mixture)
    static constexpr auto kPresenceAvgSeconds = 1.0;  // the presence log-odds' EMA

public:
    void init(double rate) {
        unsigned pn = 1;
        while (pn < static_cast<unsigned>(kNoiseWindowSeconds * rate)) { pn <<= 1; }
        m_power_ring_size = pn;
        m_power_ring.assign(static_cast<size_t>(m_power_ring_size), 0.0f);
    }

    // A fresh start (the backfill).
    void reset() {
        m_power_ring_head = m_since_refresh = 0;
        m_noise_power = m_mark_peak = m_mark_power = m_presence = 0.0;
        m_absent_run = 0;
    }

    double   noise() const { return m_noise_power; }            // σ0²; 0 = not measured
    unsigned absent_run() const { return m_absent_run; } // samples with presence
                                                       // < 0 in a row

    // One matched power r²; presence: keep the presence statistic. band_floor:
    // the band floor (set_floor) that bounds σ0².
    void update(double r2, double rate, unsigned L, bool presence, double band_floor) {
        // σ1²: the station's key-down power, held through its spaces and past
        // the end of an over (snr_meter's mark: the mean of samples within 6 dB
        // of a decaying peak) — a σ1² that collapsed onto the noise in a pause
        // would read noise as marks.
        m_mark_peak = r2 > m_mark_peak ? r2 : m_mark_peak + (r2 - m_mark_peak) / (kPeakReleaseSeconds * rate);
        if (r2 > 0.25 * m_mark_peak) { m_mark_power += (r2 - m_mark_power) / (kMarkAvgSeconds * rate); }
        // Is a keyed carrier here at all? The mixture log-odds of the sample
        // (marks a kKeyedShare share of the time) against noise alone, one
        // independent sample per MA length, averaged over kPresenceAvgSeconds. It falls
        // below zero within a second of the last mark — v2's envelope squelch,
        // but soft and calibrated.
        if (m_noise_power > 0 && presence) {
            const double s0 = m_noise_power, s1 = std::max(m_mark_power, kMinMarkToNoise * s0);
            const double lr = std::clamp(log(s0 / s1) + r2 * (1.0 / s0 - 1.0 / s1), -5.0, 5.0);
            const double term = log((1.0 - kKeyedShare) + kKeyedShare * exp(lr)) / L;
            m_presence += (term - m_presence) / (kPresenceAvgSeconds * rate);
            m_absent_run = m_presence < 0 ? m_absent_run + 1 : 0;
        }
        m_power_ring[m_power_ring_head++ & (m_power_ring_size - 1)] = static_cast<float>(r2);
        if (++m_since_refresh >= static_cast<unsigned>(kNoiseRefreshSeconds * rate) || (m_noise_power <= 0 && m_power_ring_head >= 64)) {
            m_since_refresh = 0;
            noise_refresh(L, band_floor);
        }
    }

    // The sample's log-likelihood ratio mark : space on the matched power r².
    // Exponential power under both hypotheses — noise σ0² (the measured
    // percentile) and noise plus the station's key-down power σ1² (the mean
    // mark power under the held peak; a Rayleigh-fading mark, marginalised):
    // llr = ln(σ0²/σ1²) + r²·(1/σ0² − 1/σ1²). It shrinks to nothing as
    // σ1 → σ0 (the duration priors take over on a weak channel) and the 1/L
    // weight pays for the MA's correlation — L frames ≈ one independent
    // sample.
    double llr(float r, unsigned L) const {
        if (m_noise_power <= 0)
            return 0.0;
        const double s1 = std::max(m_mark_power, kMinMarkToNoise * m_noise_power);
        const double s0 = std::max(m_noise_power, s1 / kMaxMarkToNoise);
        const double x  = std::min(static_cast<double>(r) * r, 4.0 * s1);
        const double l  = log(s0 / s1) + x * (1.0 / s0 - 1.0 / s1);
        return std::clamp(l / L, -kMaxLlr, kMaxLlr);
    }

    // SNR500: key-down power over the noise in 500 Hz — the MA's noise is in
    // rate/L Hz. The scale CW Skimmer Server reports on (≈ +1 dB).
    double snr500(double rate, unsigned L) const {
        if (m_noise_power <= 0)
            return 0.0;
        const double sig = std::max(m_mark_power - m_noise_power, 0.01 * m_noise_power);
        return 10.0 * log10(sig / m_noise_power) + 10.0 * log10((rate / L) / 500.0);
    }

private:
    // σ0²: a low percentile of r² over the last kNoiseWindowSeconds — the noise power
    // of the matched statistic on THIS channel (a keyed station leaves its
    // spaces there). The band floor (set_floor) bounds it: a channel whose
    // every sample is signal cannot teach itself a high floor.
    void noise_refresh(unsigned L, double band_floor) {
        const unsigned n = std::min(m_power_ring_head, m_power_ring_size);
        if (n < 64)
            return;
        std::vector<float> v_v(static_cast<size_t>(m_power_ring_size));   // SDRIAK: was a VLA — C++ has none
        float *v = v_v.data();
        for (unsigned k = 0; k < n; k++) { v[k] = m_power_ring[(m_power_ring_head - n + k) & (m_power_ring_size - 1)]; }
        // Noise power through the MA is exponential (complex Gaussian). Start
        // from the 10th percentile (−ln 0.9 of the mean) — low enough to sit in
        // the noise of a channel that is keyed half the time — then refine on
        // the samples under 3 means: a truncated exponential's mean is
        // 0.843 of the full one. Marks above the cut cannot lift it.
        const unsigned q10 = static_cast<unsigned>(0.10 * (n - 1));
        std::nth_element(v, v + q10, v + n);
        double mu = v[q10] / 0.1054;
        for (int it = 0; it < 4 && mu > 0; it++) {
            const float cut = static_cast<float>(3.0 * mu);
            double sum = 0.0;
            unsigned  cnt = 0;
            for (unsigned k = 0; k < n; k++) {
                if (v[k] < cut) {
                    sum += v[k];
                    cnt++;
                }
            }
            if (cnt < 16)
                break;
            mu = (sum / cnt) / 0.843;
        }
        if (band_floor > 0 && !std::isnan(band_floor)) {
            // the channel's noise through the MA: ENBW rate/L of the channel's
            // 0.851 × spacing (snr_meter.h), spacing = rate/2
            const double expect = band_floor * 2.0 / (0.851 * L);
            mu = std::clamp(mu, 0.7 * expect, 1.5 * expect);
        }
        m_noise_power = mu;
    }

    std::vector<float> m_power_ring;      // r² ring, kNoiseWindowSeconds
    unsigned m_power_ring_size = 0, m_power_ring_head = 0;
    unsigned m_since_refresh = 0;         // samples since σ0² was measured
    double   m_noise_power = 0;           // σ0²: the noise power of r²
    double   m_mark_peak = 0;             // r²'s held peak
    double   m_mark_power = 0;            // σ1²: the mean mark power under it
    double   m_presence = 0;              // EMA log-odds: keyed carrier : noise
    unsigned m_absent_run = 0;            // samples with presence < 0 in a row
};

// --- the matched-amplitude trackers -------------------------------------------

// v1's envelope dynamics on the matched amplitude: the bootstrap's Schmitt
// and the pre-roll's bars read these in v3.
class CwMatchedLevels {
public:
    // The level trackers' constants (cw_levels.h) at rate.
    void init(double rate) {
        m_k_hi_rel = 1.0 - exp(-1.0 / (rate * 0.8));
        m_k_lo     = 1.0 - exp(-1.0 / (rate * 0.3));
        reset();
    }
    void reset() { m_hi = m_lo = -1.0; }

    double hi() const { return m_hi; }    // peak, instant attack
    double lo() const { return m_lo; }    // mean of the samples under mid

    void track(float r) {
        if (m_hi < 0) { m_hi = m_lo = r; }
        m_hi = r > m_hi ? r : m_hi + m_k_hi_rel * (r - m_hi);
        if (r < 0.5 * (m_lo + m_hi)) { m_lo += m_k_lo * (r - m_lo); }
    }

private:
    double m_k_hi_rel = 0, m_k_lo = 0;
    double m_hi = 0, m_lo = 0;
};

// --- the front end -------------------------------------------------------------

class CwFrontEnd {
public:
    // The raw I/Q ring and the matched pre-roll: a candidate the detector took
    // a second to be sure of still decodes from its first mark.
    static constexpr auto kRingSeconds = 6.0;

    // The front end of a channel at rate whose pre-roll ring holds ring_size
    // frames (upstream: channel_new's fe_on block). Off until init().
    void init(double rate, const CwFrontEndConfig &cfg, unsigned ring_size) {
        m_on      = true;
        m_rate    = rate;
        m_weak_db = cfg.weak_db;
        m_levels.init(rate);
        m_detector.init(cfg);
        m_ring.init(ring_size);
        m_power.init(rate);
        m_matched.init();
    }

    bool   on() const { return m_on; }            // SKIM_V3_FRONT != v2
    bool   weak() const { return m_weak; }        // below SKIM_V3_WEAK_DB: the
                                                  // lattice reads the calibrated
                                                  // matched LLR; above it v2's
                                                  // own view, verbatim
    bool   candidate() const { return m_detector.candidate(); }
    double carrier_hz() const { return m_detector.carrier_hz(); }
    double noise() const { return m_power.noise(); }
    unsigned absent_run() const { return m_power.absent_run(); }
    double matched_hi() const { return m_levels.hi(); }  // matched-amplitude trackers
    double matched_lo() const { return m_levels.lo(); }
    void   set_floor(double noise_pw) { m_detector.set_floor(noise_pw); }

    // One raw frame into the I/Q ring; true: a detector hop is due.
    bool push_iq(float i, float q) {
        m_ring.push(i, q);
        return m_detector.hop_due(m_ring.head());
    }

    // One raw frame through the candidate path. Returns |u|, the matched
    // amplitude; keeps the power statistics the likelihoods are calibrated on.
    float sample(float i, float q, const CwClock &clk) {
        const double r2 = m_matched.power(i, q, m_detector.carrier_hz(), m_rate, clk);
        m_power.update(r2, m_rate, m_matched.length(), m_keep_presence || (m_on && m_weak), m_detector.band_floor());
        return static_cast<float>(sqrt(r2));
    }

    void track(float r) { m_levels.track(r); }

    // The channel just became a candidate. The detector took a second or so to
    // be sure, and the pre-roll ring must hold the MATCHED statistic for the
    // head of the over: run the front end over the raw ring up to (not
    // including) the current frame, writing the ring the pre-roll replays and
    // warming the trackers.
    void backfill(const CwClock &clk, CwPreroll &pre) {
        const unsigned pre_head = pre.head(), pre_size = pre.size();
        m_matched.reset();
        m_keep_presence = true;
        m_power.reset();
        m_levels.reset();
        unsigned have = std::min(m_ring.head(), m_ring.size());
        have = std::min(have, std::min(pre_head + 1, pre_size));
        for (unsigned k = have; k > 1; k--) {                 // frames H−have … H−2
            const unsigned f = m_ring.head() - k;
            const float r = sample(m_ring.i(f), m_ring.q(f), clk);
            track(r);
            pre.set_matched(pre_head - (k - 1), r);
        }
        m_keep_presence = false;
    }

    // A candidate by decree (the oracle's gate), backfilled at once.
    void force_candidate(const CwClock &clk, CwPreroll &pre) {
        m_detector.force(m_ring.head());
        backfill(clk, pre);
    }

    // One detector hop (push_iq() said it is due); a new candidate is
    // backfilled and takes its view from its SNR.
    void hop(uint64_t t, const CwClock &clk, CwPreroll &pre, const CwTrace &tr) {
        if (m_detector.hop(m_ring, m_rate, t, tr) == CwCarrierDetector::Event::Born) {
            backfill(clk, pre);
            if (m_power.noise() > 0) {
                m_weak = snr500() < m_weak_db + 1.5;
                CW_DBG(tr, "[%s view at birth: %.1f dB]\n", m_weak ? "weak" : "strong", snr500());
            }
        }
    }

    // The view between overs (the caller's call: mid-over the lattice would
    // read two likelihood scales in one word): weak under SKIM_V3_WEAK_DB,
    // strong again 3 dB above it.
    void update_view(uint64_t t, const CwTrace &tr) {
        if ((m_ring.head() & 15) == 0 && m_power.noise() > 0) {
            const double snr = snr500(), bar = m_weak_db;
            if (!m_weak && snr < bar) {
                m_weak = true;
                CW_DBG(tr, "[weak view: %.1f dB, t=%lu]\n", snr, (unsigned long)t);
            } else if (m_weak && snr > bar + 3.0) {
                m_weak = false;
                CW_DBG(tr, "[strong view: %.1f dB, t=%lu]\n", snr, (unsigned long)t);
            }
        }
    }

    double llr(float r) const { return m_power.llr(r, m_matched.length()); }
    double snr500() const { return m_power.snr500(m_rate, m_matched.length()); }

private:
    bool   m_on = false;
    bool   m_weak = false;
    bool   m_keep_presence = false;       // keep the presence statistic (the
                                          // backfill)
    double m_rate = 0;
    double m_weak_db = 0;                 // SKIM_V3_WEAK_DB

    CwIqRing          m_ring;
    CwCarrierDetector m_detector;
    CwMatchedFilter   m_matched;
    CwPowerStats      m_power;
    CwMatchedLevels   m_levels;
};
