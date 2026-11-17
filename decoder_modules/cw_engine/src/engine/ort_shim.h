/*
 * ort_shim.h — ONNX Runtime through its C API, loaded at RUN time.
 *
 * The engine never links ONNX Runtime: this shim loads the library
 * (an explicit path, else SKIM_ORT_LIB through DeepcwConfig, else the
 * platform's names — onnxruntime.dll; libonnxruntime.so.1, Debian's
 * .so.1.21, libonnxruntime.so; libonnxruntime.dylib), asks
 * OrtGetApiBase()->GetApi(ORT_API_VERSION) for the API table the vendored
 * header describes (vendor/onnxruntime, v1.21) and exposes the four things
 * a decode backend needs: open the runtime, load a model, run one float
 * tensor through it, close. A missing library or model is an error string,
 * never an abort — the DeepCW backend then reports "not available".
 *
 * SDRIAK: classes on the standard library (upstream: SkimOrt /
 * SkimOrtSession opaque structs with skim_ort_* functions and GError), and
 * LoadLibrary on Windows beside dlopen. Compiled only with
 * OPT_BUILD_CW_DEEPCW.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

// The C API's types, as onnxruntime_c_api.h declares them (only that file
// includes the header itself).
struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;

// The runtime library and its environment. Kept mapped for the process
// lifetime: the runtime spins worker threads, and unloading the object
// while they wind down has bitten other users of the C API.
class SkimOrt {
public:
    // Load the runtime + create the environment. lib_override may be empty
    // (then the platform's names). nullptr + *error when the library is
    // missing or answers the wrong API version.
    static std::unique_ptr<SkimOrt> open(const std::string &lib_override, std::string *error);
    ~SkimOrt();
    SkimOrt(const SkimOrt &) = delete;
    SkimOrt &operator=(const SkimOrt &) = delete;

    // Runtime version ("1.21.0") and the name or path the loader used.
    const std::string &version() const { return m_version; }
    const std::string &library() const { return m_library; }
    const OrtApi *api() const { return m_api; }
    OrtEnv *env() const { return m_env; }

private:
    SkimOrt() = default;

    const OrtApi *m_api = nullptr;
    OrtEnv       *m_env = nullptr;
    std::string   m_version;
    std::string   m_library;
};

// One loaded model. run() may be called from several threads at once (an
// ONNX Runtime session is thread-safe for Run).
class SkimOrtSession {
public:
    // Load a model (UTF-8 path). intra_threads = ONNX Runtime intra-op
    // threads (0 = its default). device = "cpu" (default) or "cuda" (device
    // 0 through the CUDA execution provider, which the runtime loads
    // itself). A device that cannot be appended is NOT an error: the session
    // falls back to the CPU and device() tells what runs and why.
    // Input/output names are read from the model (index 0 each). ort must
    // outlive the session.
    static std::unique_ptr<SkimOrtSession> create(const SkimOrt &ort, const std::string &model_path,
                                                  int intra_threads, const std::string &device,
                                                  std::string *error);
    ~SkimOrtSession();
    SkimOrtSession(const SkimOrtSession &) = delete;
    SkimOrtSession &operator=(const SkimOrtSession &) = delete;

    // "CPU", "CUDA:0", or "CPU (cuda unavailable: <reason>)".
    const std::string &device() const { return m_device; }
    const std::string &input_name() const { return m_in_name; }
    const std::string &output_name() const { return m_out_name; }

    // Run one float32 tensor (in, dims[nd]) through the model; the output
    // tensor (index 0) is copied into out with its shape in
    // out_dims[0..out_nd) (out_dims must hold ≥ 8).
    bool run(const float *in, const int64_t *dims, int nd, std::vector<float> &out,
             int64_t *out_dims, int &out_nd, std::string *error) const;

private:
    SkimOrtSession() = default;

    const SkimOrt *m_ort     = nullptr;
    OrtSession    *m_session = nullptr;
    OrtMemoryInfo *m_mem     = nullptr;
    std::string    m_in_name;
    std::string    m_out_name;
    std::string    m_device;        // what actually runs (see device())
};
