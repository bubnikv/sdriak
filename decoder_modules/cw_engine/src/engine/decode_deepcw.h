/*
 * decode_deepcw.h — DeepCW neural CW backend (Conformer + CTC over a
 * 65-bin × 15 ms log-magnitude spectrogram; model by e04, AGPL-3.0-only,
 * https://github.com/e04/deepcw-engine).
 *
 * Implements decode.h on the channelizer's complex baseband: a 20-point
 * Hann DFT per 16 ms gives the model's 12.5 Hz bins straight from the
 * 250 Hz channel (no audio path), the channel's ±5 bins are seated at the
 * tile centre, the rest is zero (offline-verified equal to full audio),
 * each window is peak-normalised (the model is scale-invariant only over a
 * range real IQ falls below). Inference runs on the channel's own frame
 * clock (replays stay deterministic), only on channels whose window shows a
 * keyed line above the floor, and text is committed as soon as a character
 * sits ≥ tail seconds before the window end; the whole ring is re-read
 * every tick (full left context) and a frame cursor keeps anything from
 * being emitted twice.
 *
 * ONNX Runtime is loaded at run time (ort_shim.cpp) and the model file
 * resolved from the config (DeepcwConfig::model_path, SKIM_DEEPCW_MODEL in
 * from_env()) and checked against its pinned sha256 (deepcw_runtime.h);
 * when either is missing or wrong available() says why and the channels
 * track but decode nothing. This backend reads ONLY the v1 model; the v2
 * models are decode_deepcw2.h's.
 *
 * Inference runs on DeepcwConfig::workers (2) threads by default — the
 * engine thread only snapshots a window and later commits the result — or
 * INLINE with sync (SKIM_DEEPCW_SYNC=1), which a replay sets so that it is
 * deterministic (results land in stream order, never a wall-clock race).
 *
 * SDRIAK: compiled only with OPT_BUILD_CW_DEEPCW (CW_ENGINE_DEEPCW is then
 * defined for the engine's consumers). The process-wide singletons of
 * upstream (the tunables, the session, the worker pool, the tail dump)
 * belong to one backend object here, made on a DeepcwConfig;
 * upstream's skim_decode_deepcw_* functions are its methods.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "decode.h"

// The model's contract (deepcw-engine model.onnx.json): input
// [batch, 1, time, 65] = log1p |STFT| at 12.5 Hz per bin, output
// [batch, time, 42] CTC log-probs, classes 0..41, space 40, blank 41.
struct DeepcwModel {
    static constexpr int    kBins    = 65;
    static constexpr int    kCentre  = 32;
    static constexpr int    kClasses = 42;
    static constexpr int    kBlank   = 41;
    static constexpr int    kSpace   = 40;
    static constexpr double kBinHz   = 12.5;
    static constexpr char   kChars[kClasses + 1] = ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
};

// The backend's tunables, upstream's SKIM_DEEPCW_* switches (read once into
// a process-wide struct upstream). Seconds unless noted.
struct DeepcwConfig {
    std::string model_path;      // SKIM_DEEPCW_MODEL: model.onnx ("" = none set;
                                 // upstream: else the user data dir — the
                                 // application chooses its own place)
    std::string ort_library;     // SKIM_ORT_LIB: the runtime library ("" = the
                                 // platform's names, ort_shim.h)
    std::string device = "cpu";  // SKIM_DEEPCW_DEVICE: "cpu" or "cuda"
    bool verify      = true;     // SKIM_DEEPCW_VERIFY (1): refuse a model.onnx that is
                                 // not the pinned v1 file (deepcw_runtime.h
                                 // kDeepcwModelV1; SDRIAK: upstream checks only when
                                 // its fetcher installs the file)
    double win_s     = 10.0;     // ring length = max pending audio
    double tick_s    = 0.0;      // inference cadence per channel (0 = by device:
                                 // 0.5 on CUDA, 1.0 on the CPU)
    double tail_s    = 1.0;      // tail guard: no commit closer to the end
    double minconf_s = 2.0;      // no commit inside the first seconds
    double gate_db   = 6.0;      // line over floor to run inference
    int    threads   = 4;        // ONNX Runtime intra-op threads
    double space_p   = 0.8;      // min posterior for a word gap to count (0 = off)
    bool   sync      = false;    // SKIM_DEEPCW_SYNC=1: inference inline (replays)
    int    workers   = 2;        // async inference threads (1..8)
    int    batch     = 32;       // windows per model run on a worker (1..256)
    int    debug     = 0;        // SKIM_DEEPCW_DEBUG: 1 = ticks, 2 = + frames
    bool   draft     = true;     // SKIM_DEEPCW_DRAFT (1): gray draft tail in the pane
    double draft_min_s = 0.384;  // SKIM_DEEPCW_DRAFT_MIN: no draft closer to the
                                 // window end — the model's reading there is mostly
                                 // wrong (80 m fixture: 11–44 % right under 384 ms,
                                 // 71 % at 384–512, ≥ 85 % beyond)
    double draft_p   = 0.9;      // SKIM_DEEPCW_DRAFT_P: min posterior of a draft
                                 // spike; with the distance bar → 98 % right
    bool   flush     = true;     // SKIM_DEEPCW_FLUSH (1): a TX hold commits the tail
                                 // guard instead of dropping it, and closes the word
                                 // (gh#18); 0 = the behaviour before, for A/B replays
    double flush_pad_s = 0.5;    // SKIM_DEEPCW_FLUSH_PAD: dead air appended to the
                                 // flush window — the band IS dead for us from here
                                 // on, and the model wants right context for the
                                 // last char
    double flush_quiet = 6.0;    // SKIM_DEEPCW_FLUSH_QUIET: dits of silence at the
                                 // live end that prove the last word whole; under it
                                 // the flush stops at the last word gap (a call cut
                                 // mid-word by the operator's TX must not validate:
                                 // OH2X / OH2XX)
    double flush_k   = 4.0;      // SKIM_DEEPCW_FLUSH_K: line over the noise beside
                                 // it that counts as keyed in the silence test
    double flush_p   = 0.5;      // SKIM_DEEPCW_FLUSH_P: min posterior of every spike
                                 // of the last flushed word inside the tail guard
    double only_lo = 0.0, only_hi = 0.0;   // SKIM_DEEPCW_ONLY="lo-hi" kHz (stored
                                 // in Hz): inference only on channels inside
                                 // (measurement runs on the CPU)
    std::vector<std::string> gap_words = {   // SKIM_DEEPCW_GAP_WORDS: short
        "K", "R", "CQ", "DE", "TU", "73", "88", "GL", "GM", "GA",  // words/prosigns
        "GE", "GN", "KN", "SK", "AR", "BK", "UR", "ES", "DX", "RR" // that keep a weak
    };                           // gap before them (a 1–2 char RIGHT piece that is
                                 // one of these is a word, not a torn tail)
    std::string taildump_path;   // SKIM_DEEPCW_TAILDUMP=<file>: measurement

    // Upstream's variables applied to base (default: these defaults),
    // parsed as upstream parses them.
    static DeepcwConfig from_env();
    static DeepcwConfig from_env(const DeepcwConfig &base);
};

// --- the commit rule (pure) --------------------------------------------------
// Upstream's skim_deepcw_commit / _commit_ex and their helpers; the
// tunables they read from the process-wide struct upstream are the rule's.
class DeepcwCommitRule {
public:
    // One emitted character of a greedy CTC collapse: at the frame where its
    // run starts, p = posterior there.
    struct Spike {
        uint8_t  cls;
        uint64_t t;
        float    p;
    };

    // dump = the tail dump (SKIM_DEEPCW_TAILDUMP), nullptr = none.
    explicit DeepcwCommitRule(const DeepcwConfig &cfg, FILE *dump = nullptr)
        : m_space_p(cfg.space_p), m_gap_words(cfg.gap_words), m_debug(cfg.debug), m_dump(dump) {}

    // Greedy CTC collapse of logp[T][42] over abs frames [w0, w0+T).
    std::vector<Spike> ctc_collapse(const float *logp, unsigned T, uint64_t w0) const;

    // The commit rule on a raw CTC log-prob matrix (logp[T][42], window =
    // abs frames [w0, w0+T)): greedy collapse; every spike that sits ≥ tail
    // frames before the window end and beyond the cursor (+ margin frames
    // for characters — a re-placed copy of the last committed character
    // lands within ±2 frames) is appended to out; cursor moves to the last
    // committed spike, last_space squeezes consecutive spaces. Returns the
    // number of committed characters; *conf = their mean posterior. Pure —
    // no state, no model.
    unsigned commit(const float *logp, unsigned T, uint64_t w0, uint64_t &cursor, unsigned tail,
                    unsigned margin, bool &last_space, std::string &out, double *conf) const;

    // The same rule, plus the DRAFT: the spikes INSIDE the tail guard (beyond
    // the cursor/margin, spaces squeezed on from the final text) are the
    // model's current reading of the not-yet-final tail; its RELIABLE PREFIX
    // is appended to draft — the reading is cut at the first spike closer
    // than draft_min frames to the window end or below the draft_p posterior
    // (0 / 0.0 = the whole tail). Display only (the pane shows it dim),
    // re-read at the next tick; the cursor and last_space move only for
    // final characters. nullptr for draft behaves exactly like commit().
    // space_t = frame of the last committed word gap (0 = none): a gap the
    // model placed ON or just before the cursor still passes unless it is a
    // re-read of that committed gap. dump_hz labels the tail dump's lines.
    unsigned commit_ex(const float *logp, unsigned T, uint64_t w0, uint64_t &cursor, unsigned tail,
                       unsigned margin, bool &last_space, uint64_t &space_t, std::string &out,
                       double *conf, std::string *draft, unsigned draft_min, double draft_p,
                       double dump_hz = 0.0) const;

private:
    void drop_weak_tears(std::vector<Spike> &sp, double dump_hz) const;

    double                   m_space_p;
    std::vector<std::string> m_gap_words;
    int                      m_debug;
    FILE                    *m_dump;
};

// --- test hooks (deepcw_test) -------------------------------------------------
struct SkimDeepcwDebug {
    bool     gate       = false;   // last tick's gate verdict
    double   ratio_db   = 0.0;     // line over floor, last gated window
    double   duty       = 0.0;     // keyed duty, last window
    double   wpm        = 0.0;     // dit estimate → WPM (0 = unknown)
    uint64_t frames_abs = 0;       // frames seen
    uint64_t committed  = 0;       // commit cursor (abs frame)
    unsigned ticks      = 0;       // inference runs so far
    bool     dead       = false;   // unusable rate / no runtime
    bool     inflight   = false;   // async: a window is at the workers
    unsigned draft_len  = 0;       // chars of tail-guard draft from the last tick
    bool     pane_open  = false;   // an over region is open in the pane
};

class DeepcwContext;

// The backend: "deepcw", reading the DeepCW v1 model (e04/deepcw-engine
// 8e264d2, sha256 pinned in deepcw_runtime.h). Its channels implement Level,
// ToneOffsetHz, SetFreq, TakePaneOp, Resync, HoldBegin, Noise and
// SetBandNoise; snr_db is SNR in 500 Hz (OK1BR 4db0483, gh#27).
class SkimDeepcwBackend final : public SkimDecodeBackend {
public:
    explicit SkimDeepcwBackend(const DeepcwConfig &cfg);
    ~SkimDeepcwBackend() override;

    const char *name() const override { return "deepcw"; }
    std::unique_ptr<SkimDecodeChannel> channel_new(double sample_rate) const override;
    bool implements(SkimDecodeHook hook) const override;
    bool snr_in_500hz() const override { return true; }   // OK1BR 4db0483 (gh#27)

    // Runtime + model resolvable and loaded (cached; the first call loads).
    // false + *error names what is missing.
    bool available(std::string *error = nullptr) const;

    // The configured model path — informational, not necessarily existing.
    const std::string &model_path() const;

    // "1.21.0 via libonnxruntime.so.1, CUDA:0" once loaded, "" before.
    std::string runtime_info() const;

    // Inference device for the NEXT session: "cpu" (default) or "cuda". A
    // device that is not usable falls back to the CPU — runtime_info says so.
    void set_device(const std::string &device);
    // Drop the loaded session so the next use reloads it (device change,
    // model change). Safe while the pipeline is stopped; workers still
    // finishing a window keep the old session alive until they are done.
    void reset();

    // A channel's state for the gate; nullopt for another backend's channel.
    static std::optional<SkimDeepcwDebug> debug(const SkimDecodeChannel &channel);

private:
    std::shared_ptr<DeepcwContext> m_ctx;   // shared with every channel
};
