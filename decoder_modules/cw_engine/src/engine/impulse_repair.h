/*
 * impulse_repair.h — impulse noise removed from a wideband complex stream
 * before the channelizer (ALGORITHM.md §5.7, "repair").
 *
 * An impulse is short in the wideband stream and lands in every channel at
 * once; after the channelizer it is smeared over 64 ms of every channel and
 * cannot be told from a fast fade. Here it is still short. A complex AR
 * model of the band, refitted on the repaired history, predicts each sample;
 * carriers and slow structure are predictable, an impulse is not. A sample
 * whose prediction error stands kThreshold above the error's median opens a
 * gap; the gap extends while the samples stay away from the model's
 * extrapolation, plus a guard. The gap is filled by forward prediction from
 * the left and backward prediction from the right, crossfaded: carriers run
 * through it, noise is lost. Nothing is zeroed, so strong carriers are not
 * gated into splatter.
 *
 * One model cannot predict dozens of independently keyed carriers, so
 * strong signals are split off first (as in Linrad): a short-time FFT sets
 * aside the bins that stand far above the median, and the repair works on
 * the rest. A gap is only filled when its error is broadband (spectral
 * flatness): a carrier keyed on or a strong signal fading is narrowband and
 * left alone, however dense real impulses are.
 *
 * Output lags input by latency() samples: the split's frame and the right
 * context a gap needs.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <complex>
#include <vector>

// Durations are in ms, converted at the stream's rate; order, quiet and guard
// are in samples (a pulse rings for a number of samples set by the receiver's
// bandwidth, whatever the rate).
struct ImpulseRepairConfig {
    int    order    = 32;      // AR model order
    double fit_ms   = 128.0;   // the model is fitted on this much history
    double refit_ms = 32.0;    // and refitted this often
    double floor_ms = 256.0;   // the error's median is taken over this much
    double thr_db   = 12.0;    // a gap opens this far over the error's median
    double hold_db  = 5.0;     // and lasts while the error stays this far over it
    int    quiet    = 4;       // samples under hold_db that end a gap
    int    guard    = 4;       // samples added on each side of a gap
    // A gap still away from the model after max_gap_ms is not an impulse (a
    // carrier keyed on, a fade coming up): it is left as it is, and
    // detection pauses for hold_off_ms while the model learns it.
    double max_gap_ms = 32.0;
    double hold_off_ms = 8.0;
    // When more than max_share of recent samples was repaired, the noise is
    // not a few impulses on a quiet floor (e.g. Middleton Class A, where
    // most of the power sits in a tenth of the samples): the threshold rises
    // until the share falls under it again (Linrad's dumb blanker does the
    // same past 20 %).
    double max_share = 0.10;
    // Strong signals split off first (Linrad does the same): one AR model
    // cannot predict dozens of independently keyed carriers, and every
    // keying edge would look like an impulse. A short-time FFT (bins of
    // about split_bin_hz, sqrt-Hann, half overlap) finds the bins whose
    // power, averaged over split_avg_ms, stands split_db over the median
    // bin; those bins and their neighbours bypass the repair, which works on
    // the rest. 0 dB: no split.
    double split_db     = 25.0;
    double split_bin_hz = 31.25;
    double split_avg_ms = 500.0;
    // Only open gaps in frames the split judges broadband (most bins up at
    // once). Off by default: dense impulse trains raise every frame and
    // would never pass; the flatness test below does the job.
    bool   split_gate   = false;
    // A gap whose error spectrum is flatter than this (geometric over
    // arithmetic mean; white noise about 0.56) is an impulse; under it, a
    // narrowband event (a carrier keyed on, a fade) left alone. 0: no test.
    double sfm_min      = 0.2;
};

class ImpulseRepair {
public:
    using Config = ImpulseRepairConfig;

    // rate: the wideband stream's sample rate, Hz.
    explicit ImpulseRepair(double rate, const Config &cfg = Config());

    // n interleaved complex samples in, n out, latency() samples later. in
    // and out may be the same buffer.
    // flags (optional): n bytes, 1 where the output sample was repaired.
    void process(const float *in, float *out, unsigned n, unsigned char *flags = nullptr);

    // Samples between input and output: the repair's look-ahead, plus the
    // split's frame when it is on.
    int    latency() const { return m_look + (m_split_n ? m_split_n : 0); }

    // Output samples so far that were replaced, and gaps filled.
    double repaired() const { return m_repaired; }
    double output() const { return m_output; }
    long   gaps() const { return m_gaps; }
    long   narrow() const { return m_narrow; }       // gaps left alone: narrowband
    long   rejected() const { return m_rejected; }   // gaps left alone: no impulse
    // The split's bins set aside as strong, on average per frame.
    double strong_bins() const { return m_frames ? static_cast<double>(m_strong_bins) / m_frames : 0.0; }
    // The split's frames judged impulsive (broadband), as a share.
    double impulsive_frames() const { return m_frames ? static_cast<double>(m_imp_frames) / m_frames : 0.0; }
    int    look() const { return m_look; }

private:
    using cplx = std::complex<double>;

    // The repair proper, on whatever reaches it (the weak part when split).
    // gate (optional): n bytes, 1 where a gap may open (the split's
    // broadband test); null: everywhere.
    void   core(const float *in, float *out, unsigned n, unsigned char *flags, const unsigned char *gate);
    // The split: n input samples in; appends the weak part to m_weak and the
    // strong part to m_strong, a frame (m_split_n) later.
    void   split(const float *in, unsigned n);
    void   refit();
    cplx   predict_fwd(size_t at) const;    // from the m_order samples before at
    cplx   predict_bwd(size_t at) const;    // from the m_order samples after at
    size_t repair_from(size_t at);          // fill a gap starting at at; its end

    Config m_cfg;
    // The durations in samples at this rate.
    int    m_fit, m_refit, m_max_gap;
    size_t m_hold;
    size_t m_err_window;
    int    m_order, m_look;
    // The working buffer: repaired history, then not yet decided input.
    // m_next indexes the first undecided sample.
    std::vector<cplx> m_buf;
    std::vector<unsigned char> m_rep;       // 1 where m_buf was repaired
    std::vector<unsigned char> m_gate;      // 1 where a gap may open
    size_t m_next = 0;
    size_t m_out_at = 0;                    // the next decided sample to hand out
    std::vector<cplx> m_a;                  // forward predictor: x_t ≈ Σ a_k x_{t−k}
    double m_floor = 0.0;                   // the prediction error power's mean
    std::vector<double> m_err;              // recent prediction error powers
    size_t m_err_at = 0;
    int    m_since_fit = 0;
    bool   m_fitted = false;
    double m_repaired = 0.0, m_output = 0.0;
    // The share control: samples repaired since the last refit, the
    // recent share (EMA over refits), the threshold's extra gain in dB.
    double m_recent_rep = 0.0, m_recent_all = 0.0, m_share = 0.0, m_raise_db = 0.0;
    long   m_gaps = 0, m_rejected = 0, m_narrow = 0;
    size_t m_hold_off = 0;                  // samples left before detection resumes

    // The split (Config::split_db > 0): frame size (0 = off), its window,
    // the input history, the overlap-add accumulator, the per-bin average
    // power, and the weak and strong streams on their way to core().
    int    m_split_n = 0;
    double m_split_alpha = 0.0, m_split_gain = 0.0;
    std::vector<double> m_win, m_avg;
    std::vector<cplx> m_sin, m_ola, m_frame;
    std::vector<unsigned char> m_mask, m_smask;  // strong bins; with neighbours
    bool   m_avg_init = false;
    std::vector<float> m_weak, m_strong;    // interleaved, waiting for core()
    std::vector<unsigned char> m_wgate;     // the weak samples' gate
    bool   m_prev_imp = false;              // the previous frame was impulsive
    std::vector<float> m_strong_delay;      // strong, delayed by the core's look-ahead
    std::vector<float> m_outq;              // combined output not yet handed out
    std::vector<unsigned char> m_flagq;
    long   m_strong_bins = 0, m_frames = 0, m_imp_frames = 0;  // diagnostics
    size_t m_lead_left = 0;                 // start-up zeros still to hand out
};
