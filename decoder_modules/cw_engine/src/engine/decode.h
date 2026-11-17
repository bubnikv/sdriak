/*
 * decode.h — decode backend interface (mode-agnostic).
 *
 * A backend receives one narrow, decimated COMPLEX baseband channel (interleaved
 * float I/Q, as produced by the channelizer) and emits decoded characters with a
 * confidence, the channel's audio/carrier offset, and a speed (WPM for CW, baud
 * for RTTY/PSK). CW, RTTY and PSK31/63 are each a backend implementing this.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"


// One decode event on a channel.
struct SkimDecode {
    static constexpr auto kTextMax = 64;

    char    text[kTextMax];             // newly decoded characters (UTF-8/ASCII)
    double  confidence;                 // 0.0 .. 1.0
    double  freq_offset_hz;             // signal offset within the channel
    double  speed;                      // WPM (CW) or baud (RTTY/PSK)
    double  snr_db;                     // estimated SNR of the trace. A backend
                                        // whose snr_in_500hz() is true reports
                                        // SNR in 500 Hz: key-down carrier power
                                        // over the noise density × 500 Hz — the
                                        // dB of CW Skimmer and of RBN spots
                                        // (OK1BR 4db0483, gh#27)
    bool pane_own;                  // backend owns the PANE view of this
                                    // call's text: the pipeline must not
                                    // append text[] to the pane (the pane
                                    // ops carry it); extractor/spot/log
                                    // paths are unaffected
};

// --- pane ops (phase B: draft → final hybrid pane) --------------------------
// While the neural reader streams on a solid channel, the backend COMPOSES
// the pane view of the current over: v2's per-element draft shows live and
// every reader word-commit rewrites it in place. The ops are full-state
// (SET carries the over's whole text), so a dropped op self-heals at the
// next one. Display only — nothing here may ever reach the extractor.
// (SDRIAK: upstream's SKIM_PANE_OP_APPEND/OPEN/SET/CLOSE.)
enum class SkimPaneOpKind {
    Append,     // plain pane append (e.g. the over separator)
    Open,       // erase `erase` bytes of already-shown draft,
                // open the over region, append text
    Set,        // replace the open over region with text
    Close,      // final replace; the region seals
};

// SDRIAK: the texts are std::string (upstream: malloc'd char *, fresh NULL
// when there is none — here empty).
struct SkimPaneOp {
    SkimPaneOpKind kind = SkimPaneOpKind::Append;
    unsigned  erase = 0;         // OPEN only: bytes of shown draft to take back
    unsigned  final_len = 0;     // prefix of text[] that is reader-final (bytes);
                                 // the rest is live draft (dim in the UI)
    std::string text;          // over text, or the APPEND payload
    std::string fresh;         // newly final chars ("" = none) — the decode
                               // log's increment (never re-logs a SET)
};

// The optional hooks of a channel (SDRIAK: upstream's NULL function
// pointers). A backend that leaves one out inherits a neutral default — the
// same result a caller got by skipping the NULL — and reports it absent from
// SkimDecodeBackend::implements(), for a caller whose behaviour depends on
// the hook existing (e.g. a TX hold keeps feeding zero frames only to
// backends with hold_begin).
enum class SkimDecodeHook {
    Level,
    ToneOffsetHz,
    SetFreq,
    SetFloor,
    TakeAuxText,
    TakePaneOp,
    Resync,
    HoldBegin,
    HeldPair,
    SetNoiseBw,     // OK1BR 4db0483 (gh#27)
    Noise,          // OK1BR 4db0483 (gh#27)
    SetBandNoise,   // OK1BR 4db0483 (gh#27)
};

// One channel's decoder: what upstream keeps as an opaque per-channel state
// plus the table functions that take it. SDRIAK: an object with virtual
// methods, created by SkimDecodeBackend::channel_new() and freed by its
// destructor (upstream: channel_free).
class SkimDecodeChannel {
public:
    virtual ~SkimDecodeChannel() = default;

    // Feed one block of interleaved complex baseband (nframes I/Q pairs).
    // Returns true and fills *out when characters were decoded this block.
    virtual bool process(const float *iq, unsigned nframes, SkimDecode *out) = 0;

    // Current peak signal level, linear envelope units (comparable across
    // channels of one bank). The pipeline arbitrates adjacent-channel ghosts
    // with it: only the strongest channel of a splatter group may report.
    // Optional (absent = backend cannot tell; the default reads 0).
    virtual double level() const { return 0.0; }

    // Current in-channel tone offset estimate (Hz, EMA) — lets the pipeline
    // recognise that two OVERLAPPING channels are tracking the same physical
    // tone (a signal midway decodes in both at equal level and doubles every
    // character). Optional.
    virtual double tone_offset_hz() const { return 0.0; }

    // Absolute frequency label for this channel/slot (Hz) — the pipeline
    // refreshes it each block before process(). Diagnostics only (run dumps
    // carry real frequencies); the decode path never depends on it. Optional.
    virtual void set_freq(double /*freq_hz*/) {}

    // The band's noise floor around this channel (snr_meter.h: the mean
    // power of a noise-only sample of the channel, linear; NAN = not measured
    // yet), refreshed by the pipeline about once a second before process().
    // A backend that calibrates its likelihoods or reports an SNR against the
    // band floor takes it here (v3). Optional.
    virtual void set_floor(double /*noise_pw*/) {}

    // Pending DISPLAY-ONLY text (returns it and clears; "" when none —
    // SDRIAK: upstream returns a malloc'd string or NULL). The pipeline shows
    // it (pane, decode log) but must NEVER feed it to the callsign extractor:
    // aux text may come from a lexically primed model that can hallucinate
    // plausible callsigns from noise — live-caught 2026-07-16, a phantom
    // "EI55ISI" station row minted from a neural re-read of channel babble.
    // Optional.
    virtual std::string take_aux_text() { return std::string(); }

    // Pop one pending pane op (fills op, returns true) or false when the
    // queue is empty. Same hallucination rule as aux text: pane + decode log
    // only, NEVER the extractor. Optional. (A reference, not a return value:
    // a caller reusing one SkimPaneOp keeps its strings' buffers.)
    virtual bool take_pane_op(SkimPaneOp & /*op*/) { return false; }

    // Resynchronize after a gap the channel did NOT see (the pipeline's TX
    // hold swallows blocks while the operator's own TX deafens the band):
    // drop mid-character framer state and sub-bit
    // accumulators, KEEP acquisition/tracking — the channel must decode the
    // answering station from its first characters. Optional.
    virtual void resync() {}

    // The TX hold BEGINS: the audio ends here, for seconds. A backend that
    // commits behind the live edge (DeepCW's tail guard) reads what it holds
    // to its end NOW — the station the operator is answering finished its
    // over a moment ago, and waiting for right context would wait out the
    // whole transmission and then lose the tail to resync (gh#18). While the
    // hold lasts the pipeline keeps calling process() with ZERO frames, so
    // the flushed text (and its pane ops) drain through the normal dispatch.
    // A backend without the hook is left alone through the hold, as before.
    // Optional.
    virtual void hold_begin() {}

    // An FSK backend: while the channel COPIES a pair — acquired and its
    // squelch open — the pair's centre offset in the channel (Hz); nullopt
    // otherwise.
    // The pipeline publishes it (rtty_pairs.h) so the band's CW decoders can
    // stay off the RTTY station. Optional.
    virtual std::optional<double> held_pair() const { return std::nullopt; }

    // --- the band's noise, CW Skimmer's way -------------------------------
    // From OK1BR/skimmer-for-linux 4db0483 (2026-11-07, gh#27: "Spots: the dB
    // is SNR in 500 Hz"), which OK1HRA's fork does not have. (v3's
    // set_floor() above is OK1HRA's own band floor; the two coexist.)

    // The equivalent noise bandwidth (Hz) of the stream this channel is fed
    // — the channelizer channel's or a narrow slot's filter — so a backend
    // that measures the noise of its WHOLE input can state snr_db in 500 Hz.
    // A backend never told assumes its input noise fills the sample rate. A
    // backend that measures its noise per spectral bin (DeepCW) needs no
    // telling. Optional.
    virtual void set_noise_bw(double /*enbw_hz*/) {}

    // This channel's noise statistic over the last second, in the backend's
    // own units — every channel's, a station's included, so the pipeline
    // can take the MEDIAN over the bank (a station's channel reads high and
    // is discarded by the median; a mean would rise with every strong
    // station in a contest). 0 = no estimate yet. Optional.
    virtual double noise() const { return 0.0; }

    // That median handed back, with the noise bandwidth of the channels it
    // was taken on, so the backend refers its signal to the band's noise
    // density instead of the noise it can see between the elements of a
    // strong station (filter ringing fills those gaps). A backend never told
    // falls back to its own channel's noise. Optional.
    virtual void set_band_noise(double /*noise*/, double /*enbw_hz*/) {}
};

// A decode backend: a stateless singleton that names the mode, says which
// optional hooks its channels implement, and creates channels. SDRIAK: a
// class (upstream: the same table of function pointers; the per-channel
// ones are SkimDecodeChannel's methods here).
class SkimDecodeBackend {
public:
    virtual ~SkimDecodeBackend() = default;

    // "cw", "rtty", "psk31" ...
    virtual const char *name() const = 0;

    // A decoder for a channel sampled at sample_rate.
    virtual std::unique_ptr<SkimDecodeChannel> channel_new(double sample_rate) const = 0;

    // Whether this backend's channels implement an optional hook.
    virtual bool implements(SkimDecodeHook /*hook*/) const { return false; }

    // true: snr_db is SNR in 500 Hz as described at SkimDecode::snr_db, and
    // the pipeline may complete it (add back what the channel filter took
    // off a tone away from the channel centre). false: the backend's own
    // scale, passed through untouched. (OK1BR 4db0483, gh#27.)
    virtual bool snr_in_500hz() const { return false; }
};
