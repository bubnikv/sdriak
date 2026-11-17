/*
 * ort_shim.cpp — ONNX Runtime C API behind run-time loading (see ort_shim.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "ort_shim.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "onnxruntime_c_api.h"

namespace {

// --- the platform's loader (SDRIAK: upstream is dlopen only) ---------------

#ifdef _WIN32
std::wstring widen(const std::string &s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string last_error() {
    const DWORD code = GetLastError();
    char *msg = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string s = n && msg ? std::string(msg, n) : "error " + std::to_string(code);
    if (msg)
        LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    return s;
}

void *lib_open(const std::string &name, std::string *why) {
    // A path: its own directory first for the runtime's companions
    // (onnxruntime_providers_shared.dll); a bare name: the usual search, the
    // executable's directory first.
    const bool path = name.find_first_of("/\\") != std::string::npos;
    HMODULE h = LoadLibraryExW(widen(name).c_str(), nullptr, path ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (!h)
        *why = last_error();
    return reinterpret_cast<void *>(h);
}

void *lib_sym(void *lib, const char *sym) {
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(lib), sym));
}

void lib_close(void *lib) { FreeLibrary(static_cast<HMODULE>(lib)); }
#else
void *lib_open(const std::string &name, std::string *why) {
    void *h = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        *why = e ? e : "?";
    }
    return h;
}

void *lib_sym(void *lib, const char *sym) { return dlsym(lib, sym); }

void lib_close(void *lib) { dlclose(lib); }
#endif

// The names tried after an explicit one.
std::vector<std::string> lib_names() {
#if defined(_WIN32)
    return { "onnxruntime.dll" };
#elif defined(__APPLE__)
    // SDRIAK: the release archive's and Homebrew's names; dlopen of a bare
    // name does not search Homebrew's prefix.
    return { "libonnxruntime.dylib", "libonnxruntime.1.dylib",
             "/opt/homebrew/lib/libonnxruntime.dylib", "/usr/local/lib/libonnxruntime.dylib" };
#else
    // Debian and Ubuntu name the soname after the minor release
    // (libonnxruntime1.21 ships only libonnxruntime.so.1.21) — the release
    // the vendored header is, so its API is there for certain.
    return { "libonnxruntime.so.1", "libonnxruntime.so.1.21", "libonnxruntime.so" };
#endif
}

// Turn an OrtStatus into an error string (and release it). true = there was one.
bool take_status(const OrtApi *api, OrtStatus *st, std::string *error, const char *what) {
    if (!st)
        return false;
    if (error)
        *error = std::string(what) + ": " + api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return true;
}

// Append the CUDA execution provider (device 0). false + *why on failure —
// typically the provider library is not installed (a CPU-only runtime) or
// the driver is missing; the caller then stays on the CPU.
bool append_cuda(const OrtApi *api, OrtSessionOptions *opt, std::string *why) {
#if defined(__linux__) && !defined(__ANDROID__)
    // Arch's onnxruntime-cuda 1.29 provider library references cuDNN
    // symbols but does not list libcudnn.so.9 as NEEDED (only cudart and
    // cublas), so its load fails with "undefined symbol:
    // cudnnGetConvolutionBackwardDataAlgorithm_v7" although the installed
    // cuDNN 9.26 exports it (checked with nm). Bringing cuDNN into the
    // process first, with global symbol visibility, lets the provider
    // resolve; harmless where the provider is linked correctly, and a
    // missing cuDNN is reported by the provider load itself below.
    static const bool cudnn_once = [] {
        if (!dlopen("libcudnn.so.9", RTLD_NOW | RTLD_GLOBAL)) {
            const char *e = dlerror();
            std::fprintf(stderr, "ort: libcudnn.so.9 not preloadable (%s)\n", e ? e : "?");
        }
        return true;
    }();
    (void)cudnn_once;
#endif
    OrtCUDAProviderOptionsV2 *co = nullptr;
    OrtStatus *st = api->CreateCUDAProviderOptions(&co);
    if (st) {
        *why = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        return false;
    }
    const char *keys[] = { "device_id" };
    const char *vals[] = { "0" };
    st = api->UpdateCUDAProviderOptions(co, keys, vals, 1);
    if (st) {
        *why = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        api->ReleaseCUDAProviderOptions(co);
        return false;
    }
    st = api->SessionOptionsAppendExecutionProvider_CUDA_V2(opt, co);
    api->ReleaseCUDAProviderOptions(co);
    if (st) {
        *why = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        return false;
    }
    return true;
}

// SDRIAK: the session options both of upstream's CreateSession attempts
// set (the fallback ignored the statuses; so does strict = false here).
bool set_options(const OrtApi *api, OrtSessionOptions *opt, int intra_threads, bool strict,
                 std::string *error) {
    auto failed = [&](OrtStatus *st, const char *what) {
        if (strict)
            return take_status(api, st, error, what);
        if (st)
            api->ReleaseStatus(st);
        return false;
    };
    if (intra_threads > 0 && failed(api->SetIntraOpNumThreads(opt, intra_threads), "SetIntraOpNumThreads"))
        return false;
    if (failed(api->SetInterOpNumThreads(opt, 1), "SetInterOpNumThreads"))
        return false;
    if (failed(api->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel"))
        return false;
    OrtStatus *st = api->SetSessionLogSeverityLevel(opt, 3);   // errors only
    if (st)
        api->ReleaseStatus(st);                         // cosmetic — ignore
    return true;
}

OrtStatus *create_session(const OrtApi *api, OrtEnv *env, const std::string &model_path,
                          OrtSessionOptions *opt, OrtSession **sess) {
#ifdef _WIN32
    // ORTCHAR_T is wchar_t on Windows.
    return api->CreateSession(env, widen(model_path).c_str(), opt, sess);
#else
    return api->CreateSession(env, model_path.c_str(), opt, sess);
#endif
}

} // namespace

std::unique_ptr<SkimOrt> SkimOrt::open(const std::string &lib_override, std::string *error) {
    std::vector<std::string> cands;
    if (!lib_override.empty())
        cands.push_back(lib_override);
    for (auto &n : lib_names())
        cands.push_back(n);

    void *dl = nullptr;
    std::string used, tried;
    for (size_t i = 0; i < cands.size() && !dl; i++) {
        std::string why;
        dl = lib_open(cands[i], &why);
        if (dl)
            used = cands[i];
        else
            tried += (i ? ", " : "") + cands[i] + " (" + why + ")";
    }
    if (!dl) {
        if (error)
            *error = "ONNX Runtime library not found — tried " + tried;
        return nullptr;
    }

    using GetApiBase = decltype(&OrtGetApiBase);
    auto get_base = reinterpret_cast<GetApiBase>(lib_sym(dl, "OrtGetApiBase"));
    if (!get_base) {
        if (error)
            *error = used + " has no OrtGetApiBase";
        lib_close(dl);
        return nullptr;
    }
    const OrtApiBase *base = get_base();
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api) {
        if (error)
            *error = used + " (version " + base->GetVersionString() +
                     ") does not provide ONNX Runtime API " + std::to_string(ORT_API_VERSION);
        lib_close(dl);
        return nullptr;
    }
    OrtEnv *oenv = nullptr;
    OrtStatus *st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "skimmer", &oenv);
    if (take_status(api, st, error, "CreateEnv")) {
        lib_close(dl);
        return nullptr;
    }
    std::unique_ptr<SkimOrt> o(new SkimOrt());
    o->m_api     = api;
    o->m_env     = oenv;
    o->m_version = base->GetVersionString();
    o->m_library = used;
    return o;   // the library stays mapped (see the class)
}

SkimOrt::~SkimOrt() {
    if (m_env)
        m_api->ReleaseEnv(m_env);
}

std::unique_ptr<SkimOrtSession> SkimOrtSession::create(const SkimOrt &o, const std::string &model_path,
                                                       int intra_threads, const std::string &device,
                                                       std::string *error) {
    const OrtApi *api = o.api();
    OrtSessionOptions *opt = nullptr;
    OrtStatus *st = api->CreateSessionOptions(&opt);
    if (take_status(api, st, error, "CreateSessionOptions"))
        return nullptr;
    std::string dev_label;
    std::string dev = device;
    std::transform(dev.begin(), dev.end(), dev.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (dev == "cuda") {
        std::string why;
        if (append_cuda(api, opt, &why))
            dev_label = "CUDA:0";
        else
            dev_label = "CPU (cuda unavailable: " + why + ")";
    } else {
        dev_label = "CPU";
    }
    if (!set_options(api, opt, intra_threads, true, error)) {
        api->ReleaseSessionOptions(opt);
        return nullptr;
    }

    OrtSession *sess = nullptr;
    st = create_session(api, o.env(), model_path, opt, &sess);
    api->ReleaseSessionOptions(opt);
    if (st && dev_label.rfind("CUDA", 0) == 0) {
        // The provider appended but the session refused (no usable GPU, a
        // driver/library mismatch): say so and build the CPU session.
        dev_label = std::string("CPU (cuda unavailable: ") + api->GetErrorMessage(st) + ")";
        api->ReleaseStatus(st);
        st = api->CreateSessionOptions(&opt);
        if (take_status(api, st, error, "CreateSessionOptions"))
            return nullptr;
        set_options(api, opt, intra_threads, false, nullptr);
        st = create_session(api, o.env(), model_path, opt, &sess);
        api->ReleaseSessionOptions(opt);
    }
    if (take_status(api, st, error, "CreateSession"))
        return nullptr;

    OrtAllocator *alloc = nullptr;
    st = api->GetAllocatorWithDefaultOptions(&alloc);
    if (take_status(api, st, error, "GetAllocatorWithDefaultOptions")) {
        api->ReleaseSession(sess);
        return nullptr;
    }
    char *in_name = nullptr, *out_name = nullptr;
    st = api->SessionGetInputName(sess, 0, alloc, &in_name);
    if (take_status(api, st, error, "SessionGetInputName")) {
        api->ReleaseSession(sess);
        return nullptr;
    }
    st = api->SessionGetOutputName(sess, 0, alloc, &out_name);
    if (take_status(api, st, error, "SessionGetOutputName")) {
        alloc->Free(alloc, in_name);
        api->ReleaseSession(sess);
        return nullptr;
    }
    OrtMemoryInfo *mem = nullptr;
    st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem);
    if (take_status(api, st, error, "CreateCpuMemoryInfo")) {
        alloc->Free(alloc, in_name);
        alloc->Free(alloc, out_name);
        api->ReleaseSession(sess);
        return nullptr;
    }
    std::unique_ptr<SkimOrtSession> s(new SkimOrtSession());
    s->m_ort      = &o;
    s->m_session  = sess;
    s->m_mem      = mem;
    s->m_in_name  = in_name;
    s->m_out_name = out_name;
    s->m_device   = dev_label;
    alloc->Free(alloc, in_name);
    alloc->Free(alloc, out_name);
    return s;
}

SkimOrtSession::~SkimOrtSession() {
    const OrtApi *api = m_ort->api();
    if (m_session)
        api->ReleaseSession(m_session);
    if (m_mem)
        api->ReleaseMemoryInfo(m_mem);
}

bool SkimOrtSession::run(const float *in, const int64_t *dims, int nd, std::vector<float> &out,
                         int64_t *out_dims, int &out_nd, std::string *error) const {
    const OrtApi *api = m_ort->api();
    size_t count = 1;
    for (int i = 0; i < nd; i++)
        count *= static_cast<size_t>(dims[i]);

    OrtValue *iv = nullptr;
    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        m_mem, const_cast<float *>(in), count * sizeof(float), dims, static_cast<size_t>(nd),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv);
    if (take_status(api, st, error, "CreateTensorWithDataAsOrtValue"))
        return false;
    const char *in_names[1]  = { m_in_name.c_str() };
    const char *out_names[1] = { m_out_name.c_str() };
    OrtValue *ov = nullptr;
    st = api->Run(m_session, nullptr, in_names, &iv, 1, out_names, 1, &ov);
    api->ReleaseValue(iv);
    if (take_status(api, st, error, "Run"))
        return false;

    OrtTensorTypeAndShapeInfo *info = nullptr;
    st = api->GetTensorTypeAndShape(ov, &info);
    if (take_status(api, st, error, "GetTensorTypeAndShape")) {
        api->ReleaseValue(ov);
        return false;
    }
    size_t ond = 0;
    st = api->GetDimensionsCount(info, &ond);
    if (take_status(api, st, error, "GetDimensionsCount")) {
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(ov);
        return false;
    }
    ond = std::min<size_t>(ond, 8);
    st = api->GetDimensions(info, out_dims, ond);
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (take_status(api, st, error, "GetDimensions")) {
        api->ReleaseValue(ov);
        return false;
    }
    size_t ocount = 1;
    for (size_t i = 0; i < ond; i++)
        ocount *= static_cast<size_t>(out_dims[i]);
    float *data = nullptr;
    st = api->GetTensorMutableData(ov, reinterpret_cast<void **>(&data));
    if (take_status(api, st, error, "GetTensorMutableData")) {
        api->ReleaseValue(ov);
        return false;
    }
    out.assign(data, data + ocount);
    out_nd = static_cast<int>(ond);
    api->ReleaseValue(ov);
    return true;
}
