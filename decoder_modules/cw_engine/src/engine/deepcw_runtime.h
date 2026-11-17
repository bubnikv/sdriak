/*
 * deepcw_runtime.h — a DeepCW model behind ONNX Runtime: the session, the
 * model pin, and the inference workers, shared by the DeepCW backends
 * (decode_deepcw.h: v1, decode_deepcw2.h: v2).
 *
 * SDRIAK's: upstream (skimmer-for-linux decode_deepcw.c) keeps the session,
 * the job queue and the workers as process-wide statics of the one backend.
 * Here one DeepcwRuntime per backend holds them, for one model contract
 * (DeepcwModelSpec), so the v1 and v2 backends can run side by side. What
 * it adds to upstream:
 *  - the model file is checked against the spec's pinned sha256 before it
 *    is loaded (upstream checks it only when the headless fetcher installs
 *    it), and a known DeepCW file of another contract is named;
 *  - every run's output shape is checked against the contract (the time
 *    stride), so a model of another version can never be read as this one;
 *  - a worker batches only windows of EQUAL length. Upstream zero-pads a
 *    batch to its longest window, but the model's front end normalises over
 *    the whole window (GroupNorm as InstanceNormalization, time included)
 *    and its self-attention has no mask, so padding changes how the real
 *    frames are read.
 * Compiled only with OPT_BUILD_CW_DEEPCW.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

class SkimOrt;
class SkimOrtSession;

// One model contract (e04/deepcw-engine model*.onnx.json) and its pinned file.
struct DeepcwModelSpec {
    const char *name;           // for messages
    int         bins;           // input frequency bins
    int         classes;        // CTC classes (blank 41, space 40)
    int         time_stride;    // input frames per output frame
    const char *sha256;         // the pinned model.onnx
    const char *source;         // where it is published
};

// e04/deepcw-engine 8e264d2 (2026-06-16) model.onnx: AGPL-3.0-only.
inline constexpr DeepcwModelSpec kDeepcwModelV1 = {
    "DeepCW v1 (deepcw-engine 8e264d2)", 65, 42, 1,
    "ef120799457bca042d4690944f0faf93268eb4654e7f50f28784ad63bdc1fe02",
    "https://github.com/e04/deepcw-engine/blob/8e264d243bbd4467bd19f3f28292219405b47e0e/model.onnx" };
// e04/deepcw-engine c18fce5 "v2" (2026-10-24) model.onnx: MIT since 79104fb.
inline constexpr DeepcwModelSpec kDeepcwModelV2 = {
    "DeepCW v2 (deepcw-engine c18fce5)", 65, 42, 2,
    "cd5ee38106348ad86a8f2d40ef1ce7dccea0d7f7fbfd0f2dee0fd1a8a28773ae",
    "https://github.com/e04/deepcw-engine/blob/79104fbe1ea0abd09eb27c6ebf9c6bab364d0d7f/model.onnx" };
// e04/deepcw-engine c18fce5 model_narrow.onnx (3 bins, 787.5–812.5 Hz): MIT.
inline constexpr DeepcwModelSpec kDeepcwModelV2Narrow = {
    "DeepCW v2 narrow (deepcw-engine c18fce5)", 3, 42, 2,
    "fc80c4baec72d756af1dbce9b70357a731b0b1a6b8306815bcfc61b6403c46e5",
    "https://github.com/e04/deepcw-engine/blob/79104fbe1ea0abd09eb27c6ebf9c6bab364d0d7f/model_narrow.onnx" };

struct DeepcwRuntimeConfig {
    std::string model_path;      // "" = none set
    std::string ort_library;     // "" = the platform's names (ort_shim.h)
    std::string device = "cpu";  // "cpu" or "cuda"
    int  threads = 4;            // ONNX Runtime intra-op threads
    int  workers = 2;            // async inference threads (1..8)
    int  batch   = 32;           // equal-length windows per run (1..256)
    bool verify  = true;         // check the file against the spec's sha256
};

// Where a job delivers its result. The channel reads it on the engine
// thread; a channel destroyed with a job in flight marks it dying and the
// worker drops the result.
struct DeepcwMailbox {
    std::atomic<bool>  dying{ false };
    std::mutex         lock;
    bool               ready = false;  // a result waits
    std::vector<float> logp;           // logp[To][classes]
    unsigned           T  = 0;         // output frames (0 = the run failed)
    uint64_t           w0 = 0;         // the job's w0, returned as given
};

struct DeepcwJob {
    std::shared_ptr<DeepcwMailbox> box;
    std::vector<float>             tile;   // [T][bins]
    unsigned                       T  = 0; // input frames
    uint64_t                       w0 = 0;
};

class DeepcwRuntime {
public:
    DeepcwRuntime(const DeepcwModelSpec &spec, const DeepcwRuntimeConfig &cfg);
    ~DeepcwRuntime();
    DeepcwRuntime(const DeepcwRuntime &) = delete;
    DeepcwRuntime &operator=(const DeepcwRuntime &) = delete;

    const DeepcwModelSpec &spec() const { return m_spec; }
    const DeepcwRuntimeConfig &cfg() const { return m_cfg; }

    // Runtime + model resolvable, pinned and loaded (cached; the first call
    // loads). false + *error names what is missing or wrong.
    bool ensure_session(std::string *error);
    void set_device(const std::string &device);
    void reset();
    std::string runtime_info() const;

    // Output frames the contract gives for T input frames.
    unsigned output_frames(unsigned T) const {
        return (T + static_cast<unsigned>(m_spec.time_stride) - 1) / static_cast<unsigned>(m_spec.time_stride);
    }

    // One window through the model (inline path). To = 0: nothing came back.
    void infer(const float *tile, unsigned T, std::vector<float> &logp, unsigned &To);
    // Queue a window for the workers. false = the queue is too long and the
    // window was not queued (never for force).
    bool submit(std::unique_ptr<DeepcwJob> job, bool force);

private:
    std::shared_ptr<const SkimOrtSession> session() const;
    bool check_output(unsigned T, const int64_t *od, int ond, int64_t batch);
    void workers_ensure();
    void worker_main();
    void worker_batch(std::vector<std::unique_ptr<DeepcwJob>> &jobs);

    const DeepcwModelSpec m_spec;
    DeepcwRuntimeConfig   m_cfg;

    mutable std::mutex m_lock;
    std::shared_ptr<SkimOrt> m_ort;                     // under m_lock
    std::shared_ptr<const SkimOrtSession> m_cur;        // under m_lock
    std::string m_load_err;
    bool        m_tried = false;
    std::string m_rt_info;
    std::string m_device;                               // set_device(); "" = the config's

    std::once_flag m_workers_once;
    std::mutex     m_jobs_lock;
    std::condition_variable m_jobs_cv;
    std::deque<std::unique_ptr<DeepcwJob>> m_jobs;      // under m_jobs_lock
    bool           m_stop = false;                      // under m_jobs_lock
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_behind_warned{ false };
    std::atomic<bool> m_infer_warned{ false };
    std::atomic<bool> m_shape_warned{ false };
};

// The pin check alone: "" when path is the spec's file, else why not (a
// known DeepCW file of another contract is named).
std::string deepcw_model_mismatch(const DeepcwModelSpec &spec, const std::string &path);
