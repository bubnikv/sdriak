/*
 * deepcw_runtime.cpp — see deepcw_runtime.h.
 *
 * The session load, the inline run and the batch worker are upstream's
 * (skimmer-for-linux decode_deepcw.c: ensure_session, dcw_infer,
 * worker_batch, worker_main); the pin, the shape check and the
 * equal-length batching are SDRIAK's (marked), as is the fused kernel's
 * path (deepcw_narrow_fused.h) and its check against ONNX Runtime.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_runtime.h"

#include <filesystem>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define DCW_RT_X86 1
#endif

#include "deepcw_narrow_fused.h"
#include "ort_shim.h"
#include "skim_sha256.h"

namespace {

// SDRIAK: ONNX Runtime allows one environment per process, so the backends
// share one SkimOrt (the first open decides the library).
std::shared_ptr<SkimOrt> shared_ort(const std::string &lib, std::string *error) {
    static std::mutex lock;
    static std::weak_ptr<SkimOrt> cache;
    std::lock_guard<std::mutex> lk(lock);
    if (auto o = cache.lock())
        return o;
    std::shared_ptr<SkimOrt> o = SkimOrt::open(lib, error);
    cache = o;
    return o;
}

const DeepcwModelSpec *known_spec(const std::string &sha) {
    for (const DeepcwModelSpec *s : { &kDeepcwModelV1, &kDeepcwModelV2, &kDeepcwModelV2Narrow }) {
        if (sha == s->sha256)
            return s;
    }
    return nullptr;
}

} // namespace

// SDRIAK: the pin (upstream: skim_deepcw_model_check in the headless
// fetcher, GLib's checksum; here before every load).
std::string deepcw_model_mismatch(const DeepcwModelSpec &spec, const std::string &path) {
    std::string err;
    const std::optional<std::string> sha = SkimSha256::of_file(path, &err);
    if (!sha)
        return err;
    if (*sha == spec.sha256)
        return std::string();
    std::string why = path + " is not " + spec.name + " (sha256 " + sha->substr(0, 8) +
                      "…, expected " + std::string(spec.sha256).substr(0, 8) + "…)";
    if (const DeepcwModelSpec *other = known_spec(*sha))
        why += std::string(": it is ") + other->name + ", which another DeepCW backend reads";
    else
        why += std::string(": the pinned file is ") + spec.source;
    return why;
}

DeepcwRuntime::DeepcwRuntime(const DeepcwModelSpec &spec, const DeepcwRuntimeConfig &cfg)
    : m_spec(spec), m_cfg(cfg) {
    // SDRIAK: 0 = auto (resolved when the workers start: it depends on the
    // device and the engine); otherwise 1 .. the hardware threads (upstream
    // and the earlier port: 1..8)
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    m_cfg.threads = std::max(1, m_cfg.threads);
    m_cfg.workers = m_cfg.workers <= 0 ? 0 : std::min(m_cfg.workers, hw);
    m_cfg.batch   = m_cfg.batch <= 0 ? 0 : std::min(m_cfg.batch, 256);
}

DeepcwRuntime::~DeepcwRuntime() {
    {
        std::lock_guard<std::mutex> lk(m_jobs_lock);
        m_stop = true;
    }
    m_jobs_cv.notify_all();
    for (auto &t : m_workers)
        t.join();
    const FusedCheck c = fused_check();
    if (c.windows)
        std::fprintf(stderr, "deepcw: fused check, %s: %llu windows, max |logp - ONNX Runtime| %.2e "
                             "(%.2e where logp > -10), %llu windows with another greedy path\n",
                     m_spec.name, static_cast<unsigned long long>(c.windows), c.max_abs, c.max_abs_live,
                     static_cast<unsigned long long>(c.greedy_diff));
}

DeepcwRuntime::FusedCheck DeepcwRuntime::fused_check() const {
    std::lock_guard<std::mutex> lk(m_check_lock);
    return m_check;
}

std::shared_ptr<const DeepcwNarrowFused> DeepcwRuntime::fused() const {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_fused;
}

std::shared_ptr<const SkimOrtSession> DeepcwRuntime::session() const {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_cur;
}

void DeepcwRuntime::set_device(const std::string &device) {
    std::lock_guard<std::mutex> lk(m_lock);
    m_device = device;
    std::transform(m_device.begin(), m_device.end(), m_device.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

void DeepcwRuntime::reset() {
    std::shared_ptr<const SkimOrtSession> old;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        old = std::move(m_cur);
        m_cur.reset();
        m_fused.reset();
        m_tried = false;
        m_load_err.clear();
        m_rt_info.clear();
    }
    // old dies here, or with the last worker still running a window on it
}

std::string DeepcwRuntime::runtime_info() const {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_rt_info;
}

// The ONNX Runtime session for model (m_lock held).
bool DeepcwRuntime::load_ort(const std::string &model) {
    std::error_code ec;
    if (model.empty()) {
        m_load_err = "no model file set";
    } else if (!std::filesystem::is_regular_file(std::filesystem::u8path(model), ec)) {
        m_load_err = "model file not found: " + model;
    } else if (std::string why = m_cfg.verify ? deepcw_model_mismatch(m_spec, model) : std::string();
               !why.empty()) {
        m_load_err = why;   // SDRIAK: the pin
    } else {
        if (!m_ort)
            m_ort = shared_ort(m_cfg.ort_library, &m_load_err);
        if (m_ort) {
            const std::string dev = !m_device.empty() ? m_device
                                  : (!m_cfg.device.empty() ? m_cfg.device : "cpu");
            std::shared_ptr<const SkimOrtSession> ss =
                SkimOrtSession::create(*m_ort, model, m_cfg.threads, dev, &m_load_err);
            if (ss) {
                m_cur = ss;
                m_rt_info = m_ort->version() + " via " + m_ort->library() + ", " + ss->device();
                std::fprintf(stderr, "deepcw: %s, ONNX Runtime %s, model %s, in '%s' out '%s', %d threads\n",
                             m_spec.name, m_rt_info.c_str(), model.c_str(), ss->input_name().c_str(),
                             ss->output_name().c_str(), m_cfg.threads);
            }
        }
    }
    return m_cur != nullptr;
}

bool DeepcwRuntime::ensure_session(std::string *error) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_tried) {
        m_tried = true;
        if (m_cfg.fused_path.empty()) {
            load_ort(m_cfg.model_path);
        } else if (std::string(m_spec.sha256) != kDeepcwModelV2Narrow.sha256) {
            // SDRIAK: the fused kernel is the narrow model's alone
            m_load_err = std::string("the fused kernel runs ") + kDeepcwModelV2Narrow.name + " only, not " + m_spec.name;
        } else if (DeepcwNarrowFused::Isa isa = DeepcwNarrowFused::Isa::kAuto;
                   !DeepcwNarrowFused::isa_from_name(m_cfg.fused_isa.empty() ? "auto" : m_cfg.fused_isa, &isa)) {
            m_load_err = "unknown fused-kernel encoder '" + m_cfg.fused_isa + "' (auto, avx2, sse, neon, generic)";
        } else if (std::shared_ptr<const DeepcwNarrowFused> f =
                       DeepcwNarrowFused::load(m_cfg.fused_path, m_cfg.verify, &m_load_err, isa)) {
            if (!m_cfg.fused_check || load_ort(m_cfg.model_path)) {
                m_fused = f;
                const std::string ort = m_rt_info;
                m_rt_info = std::string("fused kernel (") + f->isa() + ")";
                if (m_cfg.fused_check)
                    m_rt_info += ", checked against ONNX Runtime " + ort;
                std::fprintf(stderr, "deepcw: %s, %s, weights %s\n", m_spec.name, m_rt_info.c_str(),
                             m_cfg.fused_path.c_str());
            } else {
                m_load_err = "fused check: " + m_load_err;
            }
        }
    }
    const bool ok = m_fused != nullptr || m_cur != nullptr;
    if (!ok && error)
        *error = m_load_err;
    return ok;
}

// SDRIAK: the output must be [batch, ceil(T / stride), classes]. Upstream
// takes min(T, od[1]) frames, which silently reads a stride-2 model's
// output as stride 1.
bool DeepcwRuntime::check_output(unsigned T, const int64_t *od, int ond, int64_t batch) {
    const bool ok = ond == 3 && od[0] == batch && od[1] == static_cast<int64_t>(output_frames(T)) &&
                    od[2] == m_spec.classes;
    if (!ok && !m_shape_warned.exchange(true)) {
        std::fprintf(stderr, "deepcw: %s: %u input frames gave output [%lld, %lld, %lld], expected "
                             "[%lld, %u, %d] — not this model's contract\n",
                     m_spec.name, T, ond > 0 ? static_cast<long long>(od[0]) : -1LL,
                     ond > 1 ? static_cast<long long>(od[1]) : -1LL, ond > 2 ? static_cast<long long>(od[2]) : -1LL,
                     static_cast<long long>(batch), output_frames(T), m_spec.classes);
    }
    return ok;
}

bool DeepcwRuntime::run_one(const float *tile, unsigned T, std::vector<float> &logp) {
    logp.clear();
    const int64_t dims[4] = { 1, 1, static_cast<int64_t>(T), m_spec.bins };
    int64_t od[8];
    int ond = 0;
    std::string err;
    std::shared_ptr<const SkimOrtSession> c = session();
    if (std::shared_ptr<const DeepcwNarrowFused> f = fused()) {
        if (T == 0)
            return false;
        const unsigned To = output_frames(T);
        logp.resize(static_cast<size_t>(To) * m_spec.classes);
        f->run(tile, T, logp.data());
        std::vector<float> ref;
        if (!m_cfg.fused_check || !c || !c->run(tile, dims, 4, ref, od, ond, &err) ||
            !check_output(T, od, ond, 1) || ref.size() != logp.size())
            return true;
        double d = 0.0, live = 0.0;
        bool greedy_same = true;
        for (unsigned t = 0; t < To; t++) {
            const float *a = logp.data() + static_cast<size_t>(t) * m_spec.classes;
            const float *b = ref.data() + static_cast<size_t>(t) * m_spec.classes;
            for (int k = 0; k < m_spec.classes; k++) {
                const double e = std::fabs(a[k] - b[k]);
                d = std::max(d, e);
                if (b[k] > -10.0f)
                    live = std::max(live, e);
            }
            greedy_same = greedy_same && std::max_element(a, a + m_spec.classes) - a ==
                                             std::max_element(b, b + m_spec.classes) - b;
        }
        std::lock_guard<std::mutex> lk(m_check_lock);
        m_check.windows++;
        m_check.max_abs = std::max(m_check.max_abs, d);
        m_check.max_abs_live = std::max(m_check.max_abs_live, live);
        m_check.greedy_diff += greedy_same ? 0 : 1;
        return true;
    }
    if (!c)
        return false;
    if (!c->run(tile, dims, 4, logp, od, ond, &err)) {
        if (!m_infer_warned.exchange(true))
            std::fprintf(stderr, "deepcw: inference failed: %s\n", err.c_str());
        return false;
    }
    return check_output(T, od, ond, 1);
}

void DeepcwRuntime::infer(const float *tile, unsigned T, std::vector<float> &logp, unsigned &To) {
    To = run_one(tile, T, logp) ? output_frames(T) : 0;
}

void DeepcwRuntime::workers_ensure() {
    std::call_once(m_workers_once, [this] {
        // SDRIAK: auto workers and batch, from deepcw_throughput on an
        // i5-13400 (DeepCW.md "Runtime defaults")
        const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        bool cuda = false, narrow_ort = false;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            const std::string dev = !m_device.empty() ? m_device : m_cfg.device;
            cuda = dev == "cuda" && !m_fused;
            narrow_ort = !m_fused && std::string(m_spec.sha256) == kDeepcwModelV2Narrow.sha256;
        }
        int w = m_cfg.workers;
        if (w == 0)
            w = cuda ? 2 : narrow_ort ? std::clamp(hw - 2, 1, 8) : std::max(1, hw - 2);
        m_workers_n = w;
        m_batch_n = m_cfg.batch > 0 ? m_cfg.batch : (cuda ? 32 : 1);
        std::fprintf(stderr, "deepcw: %s: %d workers%s, batch %d%s\n", m_spec.name, w,
                     m_cfg.workers == 0 ? " (auto)" : "", m_batch_n.load(), m_cfg.batch == 0 ? " (auto)" : "");
        for (int i = 0; i < w; i++)
            m_workers.emplace_back([this] { worker_main(); });
    });
}

bool DeepcwRuntime::submit(std::unique_ptr<DeepcwJob> job, bool force) {
    workers_ensure();
    {
        // SDRIAK: the queue holds at least kQueueJobs windows, more while
        // their tiles fit kQueueBytes (upstream: 256 jobs) — every keyed
        // channel has one window in flight at most, so with hundreds of
        // narrow channels 256 refused windows the workers had time for
        const size_t bytes = job->tile.size() * sizeof(float);
        std::lock_guard<std::mutex> lk(m_jobs_lock);
        if (!force && m_jobs.size() >= kQueueJobs && m_jobs_bytes + bytes > kQueueBytes) {
            if (!m_behind_warned.exchange(true)) {
                std::fprintf(stderr, "deepcw: inference queue full (%zu jobs, %zu MB) — workers fall behind, "
                                     "ticks skipped (%d workers)\n", m_jobs.size(), m_jobs_bytes >> 20,
                             m_workers_n.load());
            }
            return false;
        }
        m_jobs_bytes += bytes;
        m_jobs.push_back(std::move(job));
    }
    m_jobs_cv.notify_one();
    return true;
}

namespace {

void deliver(DeepcwMailbox &box, std::vector<float> logp, unsigned To, uint64_t w0) {
    std::lock_guard<std::mutex> lk(box.lock);
    box.logp  = std::move(logp);
    box.T     = To;
    box.w0    = w0;
    box.ready = true;
}

} // namespace

// A batch of windows of one length through the model in ONE run: one
// launch for N channels is what a GPU wants; on the CPU it costs the same
// per channel as N single runs. A failed run delivers an empty result, so
// the channels take their next window (SDRIAK: upstream delivers nothing
// and those channels wait for good).
void DeepcwRuntime::worker_batch(std::vector<std::unique_ptr<DeepcwJob>> &jobs) {
    const unsigned T = jobs.front()->T;
    if (fused()) {
        // SDRIAK: the fused kernel runs one window at a time
        std::vector<float> logp;
        for (auto &j : jobs) {
            if (j->box->dying.load())
                continue;
            const bool ok = run_one(j->tile.data(), j->T, logp);
            deliver(*j->box, ok ? std::move(logp) : std::vector<float>(), ok ? output_frames(j->T) : 0, j->w0);
        }
        return;
    }
    const size_t N = jobs.size();
    const size_t row = static_cast<size_t>(T) * m_spec.bins;
    std::vector<float> in(N * row);
    for (size_t i = 0; i < N; i++)
        std::copy_n(jobs[i]->tile.data(), row, in.data() + i * row);
    const int64_t dims[4] = { static_cast<int64_t>(N), 1, static_cast<int64_t>(T), m_spec.bins };
    int64_t od[8];
    int ond = 0;
    std::vector<float> out;
    std::string err;
    std::shared_ptr<const SkimOrtSession> c = session();
    const bool ok = c && c->run(in.data(), dims, 4, out, od, ond, &err);
    c.reset();
    in = {};
    if (!ok && !err.empty() && !m_infer_warned.exchange(true))
        std::fprintf(stderr, "deepcw: inference failed: %s\n", err.c_str());
    const bool shape_ok = ok && check_output(T, od, ond, static_cast<int64_t>(N));
    const unsigned To = output_frames(T);
    const size_t per = static_cast<size_t>(To) * m_spec.classes;
    for (size_t i = 0; i < N; i++) {
        DeepcwJob &j = *jobs[i];
        if (j.box->dying.load())
            continue;
        if (!shape_ok) {
            deliver(*j.box, {}, 0, j.w0);
            continue;
        }
        const float *src = out.data() + i * per;
        deliver(*j.box, std::vector<float>(src, src + per), To, j.w0);
    }
}

void DeepcwRuntime::worker_main() {
#if defined(DCW_RT_X86)
    // SDRIAK: denormals flushed (FTZ, DAZ) on the worker: with one intra-op
    // thread ONNX Runtime computes on it, and a denormal costs ~100 cycles
    _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
    const size_t max_batch = static_cast<size_t>(std::max(1, m_batch_n.load()));
    std::vector<std::unique_ptr<DeepcwJob>> batch;
    for (;;) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lk(m_jobs_lock);
            m_jobs_cv.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop)
                return;
            // SDRIAK: the oldest job and every queued job of ITS length
            // (upstream: the first `batch` jobs, zero-padded to the longest).
            batch.push_back(std::move(m_jobs.front()));
            m_jobs.pop_front();
            m_jobs_bytes -= batch.back()->tile.size() * sizeof(float);
            const unsigned T = batch.front()->T;
            for (auto it = m_jobs.begin(); it != m_jobs.end() && batch.size() < max_batch;) {
                if ((*it)->T == T) {
                    batch.push_back(std::move(*it));
                    m_jobs_bytes -= batch.back()->tile.size() * sizeof(float);
                    it = m_jobs.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // drop windows whose channel died meanwhile
        batch.erase(std::remove_if(batch.begin(), batch.end(),
                                   [](const std::unique_ptr<DeepcwJob> &j) { return j->box->dying.load(); }),
                    batch.end());
        if (!batch.empty())
            worker_batch(batch);
    }
}
