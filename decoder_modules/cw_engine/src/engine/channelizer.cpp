/*
 * channelizer.c — 2×-oversampled polyphase filter bank (M2).
 *
 * Geometry: M = in_rate/chan_bw channels, prototype lowpass of K·M taps
 * (WDSP fir_bandpass, Blackman-Harris 4-term, cutoff ±chan_bw/2 by default,
 * DC gain normalised to 1), hop H = M/2 → output rate 2·chan_bw per channel.
 * The _ex constructor opens the cutoff (up to the output Nyquist) and
 * lengthens the prototype — the RTTY bank runs 250 Hz spacing with a
 * ±225 Hz passband so both FSK tones of a station anywhere between two
 * channel centres stay inside one channel (see channelizer.h).
 *
 * Per hop: the newest K·M input frames are folded branch-wise,
 *   acc[m] = Σ_k hist[P−1−(kM+m)] · h[kM+m],      m = 0..M−1,
 * then an unnormalised BACKWARD M-point FFT (fftw3f) evaluates all channels
 * at once: channel c ⇔ +c·chan_bw (c ≤ M/2), c−M below centre — matching the
 * true-orientation IQ the TCI client delivers. A centred unit tone comes out
 * with unit amplitude (Σh = 1 lands whole in its bin).
 *
 * Oversampling phase fix: hopping by H = M/2 rotates channel c by e^{jπc}
 * between hops, so on odd hops odd channels are negated — without it every
 * odd channel's baseband would sit at ±chan_bw instead of near DC.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "channelizer.h"

#include <fftw3.h>
#include <dsp/buffer/fftw.h>
#include <cmath>
#include <cstdlib>
#include <type_traits>
#include <cstring>

// WDSP's fir_bandpass (SDRIAK: on the core dsp library, returning the taps
// as a std::vector — upstream declares the vendored WDSP's here).
#include "sdriak/fir_bandpass.h"

// FFTW's planner is NOT thread-safe: fftwf_plan_* and fftwf_destroy_plan share
// global planner state; only fftwf_execute on distinct plans may run
// concurrently. Six pipelines (skimmer-headless, one per band) building their
// channel banks on six engine threads at the first IQ block crashed inside
// fftwf's planner (2026-09-29), so every plan create/destroy takes a lock.
//
// SDRIAK: the lock is CORE's, dsp::buffer::fftwMutex() (core/src/dsp/buffer/
// fftw.h, exported from sdrpp_core): the one every FFTW planner call in the
// process takes, core's and every module's. A lock private to the engine would
// serialise the engine against itself only. Upstream's fftw_lock.c (a private
// GMutex and skim_fftw_lock()/skim_fftw_unlock()) is not part of SDRIAK.
struct FftwFree {
    void operator()(fftwf_complex *p) const { fftwf_free(p); }
};
struct FftwPlanDestroy {
    void operator()(fftwf_plan p) const {
        const std::lock_guard<std::mutex> lock(dsp::buffer::fftwMutex());
        fftwf_destroy_plan(p);
    }
};

struct SkimChannelizerState {
    static constexpr auto kTapsPerBranch = 8;  // prototype taps per polyphase branch
    static constexpr auto kRingFrames = 4096;  // output ring, frames a channel
                                               // (≈ 8 s at 2×250 Hz)

    unsigned  M = 0;             // channels
    unsigned  H = 0;             // hop = M/2
    unsigned  K = 0;             // taps per branch
    unsigned  P = 0;             // prototype taps = K·M
    double in_rate = 0;
    double chan_bw = 0;

    std::vector<float> proto;  // P real taps, Σ = 1
    std::vector<float> hist;   // P complex frames, newest at index P−1
    unsigned  staged = 0;        // frames accumulated toward the next hop
    unsigned  hop_odd = 0;       // parity of the running hop counter

    std::unique_ptr<fftwf_complex[], FftwFree> fft_in;    // M frames each
    std::unique_ptr<fftwf_complex[], FftwFree> fft_out;
    // (declared after the buffers: destroyed before them)
    std::unique_ptr<std::remove_pointer_t<fftwf_plan>, FftwPlanDestroy> plan;

    std::vector<float> ring;   // M · kRingFrames complex frames
    std::vector<unsigned> wr;  // per-channel write index
    std::vector<unsigned> cnt; // per-channel valid frames (≤ kRingFrames)
    uint64_t  dropped = 0;

    // One hop: the polyphase branches, the FFT, the M outputs into the rings.
    void hop();
};

std::unique_ptr<SkimChannelizer> SkimChannelizer::create_ex(double in_rate, double chan_bw_hz,
                                                            double passband_hz,
                                                            unsigned taps_per_branch) {
    if (in_rate <= 0 || chan_bw_hz <= 0)
        return nullptr;
    double md = in_rate / chan_bw_hz;
    unsigned  M  = static_cast<unsigned>(md + 0.5);
    if (fabs(md - static_cast<double>(M)) > 1e-6 || M < 8 || M > 16384 || (M & 1))
        return nullptr;
    if (passband_hz <= 0) { passband_hz = chan_bw_hz / 2.0; }
    // The output Nyquist is chan_bw (2× oversampled): a cutoff at or past it
    // cannot be anti-aliased by any prototype length.
    if (passband_hz >= chan_bw_hz)
        return nullptr;
    unsigned K = taps_per_branch ? taps_per_branch : SkimChannelizerState::kTapsPerBranch;
    if (K < 2 || K > 64)
        return nullptr;

    auto ch = std::make_unique<SkimChannelizerState>();
    ch->M       = M;
    ch->H       = M / 2;
    ch->K       = K;
    ch->P       = K * M;
    ch->in_rate = in_rate;
    ch->chan_bw = chan_bw_hz;

    // Prototype lowpass ±passband_hz, real taps (rtype 0), BH4 (wintype 0).
    const std::vector<double> hd = fir_bandpass(static_cast<int>(ch->P), -passband_hz, passband_hz,
                                                in_rate, 0, 0, 1.0);
    if (hd.empty())
        return nullptr;
    double sum = 0.0;
    for (unsigned i = 0; i < ch->P; i++) { sum += hd[i]; }
    ch->proto.resize(ch->P);
    for (unsigned i = 0; i < ch->P; i++) { ch->proto[i] = static_cast<float>(hd[i] / sum); }

    ch->hist.assign(2 * static_cast<size_t>(ch->P), 0.0f);
    ch->fft_in.reset(fftwf_alloc_complex(M));
    ch->fft_out.reset(fftwf_alloc_complex(M));
    {
        const std::lock_guard<std::mutex> lock(dsp::buffer::fftwMutex());
        ch->plan.reset(fftwf_plan_dft_1d(static_cast<int>(M), ch->fft_in.get(), ch->fft_out.get(),
                                         FFTW_BACKWARD, FFTW_ESTIMATE));
    }
    ch->ring.assign(static_cast<size_t>(M) * SkimChannelizerState::kRingFrames * 2, 0.0f);
    ch->wr.assign(M, 0);
    ch->cnt.assign(M, 0);
    return std::unique_ptr<SkimChannelizer>(new SkimChannelizer(std::move(ch)));
}

SkimChannelizer::SkimChannelizer(std::unique_ptr<SkimChannelizerState> ch) : m_ch(std::move(ch)) {}

// Out of line: SkimChannelizerState is complete only here.
SkimChannelizer::~SkimChannelizer() = default;

unsigned SkimChannelizer::count() const { return m_ch->M; }

double SkimChannelizer::out_rate() const {
    const SkimChannelizerState *ch = m_ch.get();
    return 2.0 * ch->chan_bw;
}

double SkimChannelizer::offset_hz(unsigned chan) const {
    const SkimChannelizerState *ch = m_ch.get();
    int c = static_cast<int>(chan);
    if (chan > ch->M / 2) { c -= static_cast<int>(ch->M); }
    return static_cast<double>(c) * ch->chan_bw;
}

// One hop over the newest P frames of history.
void SkimChannelizerState::hop() {
    for (unsigned m = 0; m < M; m++) {
        float ar = 0.0f, ai = 0.0f;
        for (unsigned k = 0; k < K; k++) {
            const unsigned tap = k * M + m;
            const unsigned idx = P - 1 - tap;         // x[s − tap]
            const float w   = proto[tap];
            ar += w * hist[2 * idx];
            ai += w * hist[2 * idx + 1];
        }
        fft_in[m][0] = ar;
        fft_in[m][1] = ai;
    }
    fftwf_execute(plan.get());

    const unsigned odd = hop_odd;
    hop_odd ^= 1;
    for (unsigned c = 0; c < M; c++) {
        float yr = fft_out[c][0], yi = fft_out[c][1];
        if (odd && (c & 1)) { yr = -yr; yi = -yi; }
        float *slot = ring.data() + (static_cast<size_t>(c) * SkimChannelizerState::kRingFrames + wr[c]) * 2;
        slot[0] = yr;
        slot[1] = yi;
        wr[c] = (wr[c] + 1) % SkimChannelizerState::kRingFrames;
        if (cnt[c] < SkimChannelizerState::kRingFrames) {
            cnt[c]++;
        } else {
            dropped++;                          // overwrote the oldest frame
        }
    }
}

void SkimChannelizer::push(const float *iq, unsigned nframes) {
    SkimChannelizerState *ch = m_ch.get();
    const unsigned H = ch->H, P = ch->P;
    while (nframes) {
        unsigned take = std::min(nframes, H - ch->staged);
        // Slide history left as the new frames stage in at the tail.
        memmove(ch->hist.data(), ch->hist.data() + 2 * take,
                (2 * static_cast<size_t>(P - take)) * sizeof(float));
        memcpy(ch->hist.data() + 2 * (P - take), iq, 2 * static_cast<size_t>(take) * sizeof(float));
        iq         += 2 * take;
        nframes    -= take;
        ch->staged += take;
        if (ch->staged == H) {
            ch->staged = 0;
            ch->hop();
        }
    }
}

unsigned SkimChannelizer::read(unsigned chan, float *iq, unsigned max_frames) {
    SkimChannelizerState *ch = m_ch.get();
    if (chan >= ch->M)
        return 0;
    unsigned n = std::min(max_frames, ch->cnt[chan]);
    const float *base = ch->ring.data() + static_cast<size_t>(chan) * SkimChannelizerState::kRingFrames * 2;
    unsigned rd = (ch->wr[chan] + SkimChannelizerState::kRingFrames - ch->cnt[chan]) % SkimChannelizerState::kRingFrames;
    for (unsigned i = 0; i < n; i++) {
        iq[2 * i]     = base[2 * rd];
        iq[2 * i + 1] = base[2 * rd + 1];
        rd = (rd + 1) % SkimChannelizerState::kRingFrames;
    }
    ch->cnt[chan] -= n;
    return n;
}

uint64_t SkimChannelizer::dropped() const {
    const SkimChannelizerState *ch = m_ch.get();
    return ch->dropped;
}
