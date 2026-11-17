/*
 * channelizer.h — wideband IQ → N narrow COMPLEX baseband channels.
 *
 * A 2×-oversampled polyphase filter bank splits the wideband TCI IQ stream
 * into M = in_rate/chan_bw_hz uniformly spaced, decimated complex channels
 * (output rate = 2·chan_bw_hz). Complex (phase-preserving) from day one
 * because RTTY/PSK backends need phase, not just magnitude.
 * Prototype filter by WDSP's fir_bandpass; the M-point FFT is fftw3f.
 *
 * Threading contract: push() and read() are single-threaded (the engine
 * pipeline thread). No internal locking.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

struct SkimChannelizerState;

// SDRIAK: a class (upstream: an opaque struct and skim_channelizer_*
// functions — new/new_ex are the create/create_ex factories, free the
// destructor, the rest the methods of the same names).
class SkimChannelizer {
public:
    // Create a channelizer for input at in_rate Hz with chan_bw_hz channel
    // spacing. in_rate/chan_bw_hz must be an even integer (the hop is M/2).
    // Empty on invalid geometry.
    static std::unique_ptr<SkimChannelizer> create(double in_rate, double chan_bw_hz) {
        return create_ex(in_rate, chan_bw_hz, 0.0, 0);
    }

    // Extended geometry: passband_hz = one-sided prototype cutoff (0 = the
    // default chan_bw/2), taps_per_branch = prototype length in branches
    // (0 = the default 8). A mode whose signal is WIDER than its channel
    // spacing (RTTY: ±85 Hz tones + keying sidebands) opens the passband up
    // to the output Nyquist (chan_bw_hz) so a station anywhere between two
    // channel centres still lands with BOTH tones in one channel; the longer
    // prototype buys back the alias rejection the wide cutoff spends.
    static std::unique_ptr<SkimChannelizer> create_ex(double in_rate, double chan_bw_hz,
                                                      double passband_hz,
                                                      unsigned taps_per_branch);
    ~SkimChannelizer();
    SkimChannelizer(const SkimChannelizer &) = delete;
    SkimChannelizer &operator=(const SkimChannelizer &) = delete;

    // Number of output channels (= M). Channel 0 is the stream centre; channels
    // 1..M/2 sit above it, M/2+1..M-1 below (negative offsets).
    unsigned count() const;

    // Per-channel output sample rate (= 2·chan_bw_hz).
    double out_rate() const;

    // Signed offset of channel chan's centre from the stream centre, in Hz.
    double offset_hz(unsigned chan) const;

    // Push one block of interleaved input IQ (nframes I/Q pairs, true
    // orientation as delivered by the TCI client).
    void push(const float *iq, unsigned nframes);

    // Drain up to max_frames of channel chan's complex baseband into iq
    // (interleaved I/Q). Returns frames copied. Each channel buffers ~8 s;
    // unread output beyond that overwrites oldest (see dropped()).
    unsigned read(unsigned chan, float *iq, unsigned max_frames);

    // Total frames lost to ring overwrites since creation (all channels).
    uint64_t dropped() const;

private:
    explicit SkimChannelizer(std::unique_ptr<SkimChannelizerState> ch);
    std::unique_ptr<SkimChannelizerState> m_ch;    // the state (channelizer.cpp)
};
