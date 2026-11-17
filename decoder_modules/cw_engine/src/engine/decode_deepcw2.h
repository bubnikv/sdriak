/*
 * decode_deepcw2.h — DeepCW v2 neural CW backend: e04/deepcw-engine's v2
 * models (c18fce5, 2026-10-24; MIT since 79104fb) on the channelizer's
 * complex baseband, streamed with the model author's own posterior
 * stitcher.
 *
 * An independent branch beside decode_deepcw.h (v1, OK1HRA/OK1BR's
 * engine), so the two can be compared on the same input. What v2 changes
 * against v1, and how this backend follows it (DeepCW.md has the record):
 *
 *  - Model contract (model.onnx.json): `noise_q90_log1p` input — each
 *    window's magnitudes over a noise level (the 90th percentile over
 *    sqrt(−2 ln 0.1), floored at peak·1e-3 and 1e-5), then log1p — and
 *    `output_time_stride` 2: one output frame per two input frames. Two
 *    models: model.onnx (65 bins, 400–1200 Hz) and model_narrow.onnx (3
 *    bins, 787.5–812.5 Hz, ~0.2 M parameters, "for multi-channel
 *    decoding").
 *  - Streaming: upstream's PosteriorStitcher (examples/python/
 *    decode_morse.py): windows of at most 12 s every 1 s, 4 s of left
 *    context before the frozen point, the log-probs of every window
 *    covering a frame averaged with smoothstep edge weights after a ±1
 *    frame dilation, greedy CTC on the average, characters committed once
 *    they end before the 2 s freeze guard, and a blank run of ≥ 2.01 s
 *    read as a word gap. Ported as DeepcwStitcher (MIT, e04).
 *
 * Kept from the v1 backend, for the comparison: the front end (20-point
 * Hann DFT per 16 ms on the 250 Hz channel, the ±5 channel bins), the
 * keyed-line gate, level / offset / WPM, SNR in 500 Hz (OK1BR gh#27), the
 * one-word-per-call output and the gray pane draft (here: the stitcher's
 * pending text). Not kept: v1's commit rule and its word-gap heuristics,
 * the reliable-prefix draft cut and the TX-hold flush's live-end trim and
 * whole-word test — hold_begin() runs the stitcher's final window instead.
 *
 * Tile, wide model: the 11 channel bins at 27..37, the rest zero, the noise
 * level from the populated bins' median (DeepcwV2Config::noise_mode). The
 * model's own rule — the 90th percentile — assumes most cells are noise, as
 * they are in 65 bins of audio; in an 11-bin channel tile a keyed line lifts
 * it, and under fast fading the fades then read as below the noise
 * (cw_bench: twice the CER at +20 dB; DeepCW.md). Narrow model: the line bin
 * of the window and its two neighbours, normalised by the model's rule (it
 * was trained on 3-bin tiles).
 *
 * Measured (DeepCW.md, "Synthetic bench"): in noise both v2 models equal
 * v1; under fast fading and multipath they read 2–4× worse than v1 at
 * 0–10 dB. The v1 model run through this backend's stitcher (model_v1)
 * reads as well as v1 with its own commit rule, so the gap is the v2
 * models', not the stitcher's.
 *
 * Compiled only with OPT_BUILD_CW_DEEPCW.
 *
 * Part of SDRIAK. GPL-3.0-or-later; DeepcwStitcher is ported from MIT code,
 * see decode_deepcw2.cpp.
 */
#pragma once

#include "decode.h"
#include "decode_deepcw.h"

// Upstream's stitcher constants (decode_morse.py), seconds of real time.
struct DeepcwStitchConfig {
    double max_segment_s   = 12.0;   // MAX_SEGMENT_SECONDS
    double min_pending_s   = 2.0;    // MIN_PENDING_SECONDS: the first window
    double step_s          = 1.0;    // STEP_SECONDS: inference cadence
    double left_context_s  = 4.0;    // STITCH_LEFT_CONTEXT_SECONDS
    double freeze_guard_s  = 2.0;    // STITCH_FREEZE_GUARD_SECONDS
    double left_ramp_s     = 2.0;    // STITCH_LEFT_RAMP_SECONDS
    double right_ramp_s    = 1.0;    // STITCH_RIGHT_RAMP_SECONDS
    int    dilation        = 1;      // STITCH_DILATION_FRAMES
    double word_boundary_s = 2.01;   // WORD_BOUNDARY_BLANK_HOPS: 134 hops of 15 ms
    double min_view_weight = 1e-3;   // MIN_VIEW_WEIGHT
    double uncovered_log_prob = -30.0;   // UNCOVERED_LOG_PROB
};

struct DeepcwV2Config {
    std::string model_path;      // SKIM_DEEPCW2_MODEL ("" = none set)
    std::string ort_library;     // SKIM_ORT_LIB
    std::string device = "cpu";  // SKIM_DEEPCW2_DEVICE
    bool   narrow  = false;      // SKIM_DEEPCW2_NARROW=1: model_narrow.onnx (3 bins)
    bool   verify  = true;       // SKIM_DEEPCW2_VERIFY (1): the sha256 pin
    int    threads = 4;          // SKIM_DEEPCW2_THREADS
    int    workers = 2;          // SKIM_DEEPCW2_WORKERS (1..8)
    int    batch   = 32;         // SKIM_DEEPCW2_BATCH (1..256)
    bool   sync    = false;      // SKIM_DEEPCW2_SYNC=1: inference inline (replays)
    int    debug   = 0;          // SKIM_DEEPCW2_DEBUG
    bool   gate    = true;       // SKIM_DEEPCW2_GATE (1): infer only on a keyed line
                                 // (v1's gate; upstream's example infers always)
    double gate_db = 6.0;        // SKIM_DEEPCW2_GATE_DB
    bool   draft   = true;       // SKIM_DEEPCW2_DRAFT (1): pending text in the pane
    // SKIM_DEEPCW2_NOISE, wide model only — the noise level of the tile:
    //   "median" the populated bins' median over the Rayleigh median factor
    //            1.1774 (default: holds while up to half the cells are
    //            signal; the 90th percentile does not — a strong line lifts
    //            it, and under fast fading the fades then read as below the
    //            noise: 2× the CER at +20 dB in cw_bench, DeepCW.md);
    //   "q90"    upstream's rule (the 90th percentile ÷ 2.146) over the 11
    //            populated bins, as the web app does for shifted tiles;
    //   "fill"   the 54 empty bins filled with Rayleigh noise at the median
    //            level, then noise_q90_log1p over the whole 65-bin tile, as
    //            the reference normalises real audio.
    std::string noise_mode = "median";
    // SKIM_DEEPCW2_V1=1: an experiment, not a v2 backend — the v1 model
    // (DeepcwModelSpec v1: stride 1, SKIM_DEEPCW_MODEL) on this backend's
    // stitcher, with the v1 backend's tile (peak-normalised to 30, log1p).
    // Against "deepcw" (the same model, OK1HRA's commit rule) it isolates
    // what the stitcher does; name "deepcw2v1".
    bool   model_v1 = false;
    double win_ring_s = 14.0;    // the channel's frame ring (≥ max_segment_s)
    DeepcwStitchConfig stitch;   // SKIM_DEEPCW2_STEP, SKIM_DEEPCW2_WIN (max_segment_s)

    static DeepcwV2Config from_env();
    static DeepcwV2Config from_env(const DeepcwV2Config &base);
};

// noise_q90_log1p in place on x[n], the noise level taken over src[m]
// (src = x for a fully populated tile). Upstream: model.onnx.json
// "normalization", decode_morse.py normalize_spectrogram().
void deepcw2_normalize(float *x, size_t n, const float *src, size_t m);

// Upstream's PosteriorStitcher on absolute output frames.
class DeepcwStitcher {
public:
    struct Span {
        uint8_t cls;
        int64_t start, end;     // output frames, inclusive
    };
    struct LaneText {
        std::string text;       // no leading/trailing space; single inner spaces
        bool prepend_space = false;
    };
    struct Take {
        std::optional<LaneText> committed;
        LaneText pending;
        // SDRIAK (upstream reports neither): mean posterior of the committed
        // characters, and their extent for a WPM estimate.
        double   confidence = 0.0;
        unsigned letters = 0;
        int64_t  first = -1, last = -1;
    };

    DeepcwStitcher(const DeepcwStitchConfig &cfg, double frames_per_second);

    int64_t context_start_frame() const;
    // The start of the window that ends at output frame end_frame.
    int64_t window_start_frame(int64_t end_frame) const;
    // logits[count][42] of the window starting at output frame start.
    void add_window(int64_t start, const float *logits, unsigned count, bool is_final);
    Take take(bool final);
    // SDRIAK: the stream restarts at output frame `frame` after a gap the
    // channel did not see (TX hold); text after it starts a new word.
    void restart(int64_t frame);

    int64_t covered_end() const { return m_covered_end; }
    int64_t frozen_end() const { return m_frozen_end; }
    int64_t committed_end() const { return m_committed_end; }

private:
    enum class Symbol { None, Char, Space };

    std::vector<float> dilated_log_probs(const float *logits, unsigned count) const;
    std::vector<float> read_scores(int64_t start, int64_t end) const;
    std::vector<Span> decode_greedy(const std::vector<float> &scores, int64_t frames,
                                    std::vector<int> &argmax) const;
    static LaneText to_lane_text(const std::vector<Span> &spans, size_t from, size_t to, bool prepend);
    void ensure_rows(int64_t end_frame);
    void drop_rows_before(int64_t frame);

    DeepcwStitchConfig m_cfg;
    double  m_fps;
    int64_t m_left_context_frames, m_freeze_guard_frames, m_word_boundary_frames, m_max_segment_frames;

    int64_t m_origin = 0;               // SDRIAK: the stream's first frame (0 upstream)
    int64_t m_row_base = 0;
    std::vector<float> m_sums;          // [rows][42]
    std::vector<float> m_weights;       // [rows]
    int64_t m_covered_end = 0, m_frozen_end = 0, m_committed_end = 0;
    Symbol  m_previous = Symbol::None;
    int64_t m_blank_frames = 0;
    bool    m_has_committed = false;
};

struct SkimDeepcw2Debug {
    bool     gate = false;
    double   ratio_db = 0.0, duty = 0.0, wpm = 0.0;
    uint64_t frames_abs = 0;            // input frames seen
    int64_t  covered_end = 0, frozen_end = 0, committed_end = 0;   // output frames
    unsigned ticks = 0;                 // model runs
    bool     dead = false, inflight = false, pane_open = false;
    unsigned draft_len = 0;
    int      line_bin = -1;             // narrow: the centre of the last tile (0..10)
};

class DeepcwV2Context;

// The backend: "deepcw2" (wide) or "deepcw2n" (narrow). Its channels
// implement Level, ToneOffsetHz, SetFreq, TakePaneOp, Resync, HoldBegin,
// Noise and SetBandNoise; snr_db is SNR in 500 Hz.
class SkimDeepcw2Backend final : public SkimDecodeBackend {
public:
    explicit SkimDeepcw2Backend(const DeepcwV2Config &cfg);
    ~SkimDeepcw2Backend() override;

    const char *name() const override;
    std::unique_ptr<SkimDecodeChannel> channel_new(double sample_rate) const override;
    bool implements(SkimDecodeHook hook) const override;
    bool snr_in_500hz() const override { return true; }

    bool available(std::string *error = nullptr) const;
    const std::string &model_path() const;
    std::string runtime_info() const;
    void set_device(const std::string &device);
    void reset();

    static std::optional<SkimDeepcw2Debug> debug(const SkimDecodeChannel &channel);

private:
    std::shared_ptr<DeepcwV2Context> m_ctx;
};
