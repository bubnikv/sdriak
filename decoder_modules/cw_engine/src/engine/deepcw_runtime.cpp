/*
 * deepcw_runtime.cpp — see deepcw_runtime.h.
 *
 * The session load, the inline run and the batch worker are upstream's
 * (skimmer-for-linux decode_deepcw.c: ensure_session, dcw_infer,
 * worker_batch, worker_main); the pin, the shape check and the
 * equal-length batching are SDRIAK's (marked).
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_runtime.h"

#include <filesystem>

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
    m_cfg.workers = std::clamp(m_cfg.workers, 1, 8);
    m_cfg.batch   = std::clamp(m_cfg.batch, 1, 256);
}

DeepcwRuntime::~DeepcwRuntime() {
    {
        std::lock_guard<std::mutex> lk(m_jobs_lock);
        m_stop = true;
    }
    m_jobs_cv.notify_all();
    for (auto &t : m_workers)
        t.join();
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

bool DeepcwRuntime::ensure_session(std::string *error) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_tried) {
        m_tried = true;
        const std::string &model = m_cfg.model_path;
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
                    std::fprintf(stderr, "deepcw: %s, ONNX Runtime %s, model %s, in '%s' out '%s', "
                                         "%d threads, batch %d\n",
                                 m_spec.name, m_rt_info.c_str(), model.c_str(), ss->input_name().c_str(),
                                 ss->output_name().c_str(), m_cfg.threads, m_cfg.batch);
                }
            }
        }
    }
    const bool ok = m_cur != nullptr;
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

void DeepcwRuntime::infer(const float *tile, unsigned T, std::vector<float> &logp, unsigned &To) {
    const int64_t dims[4] = { 1, 1, static_cast<int64_t>(T), m_spec.bins };
    int64_t od[8];
    int ond = 0;
    std::string err;
    logp.clear();
    To = 0;
    std::shared_ptr<const SkimOrtSession> c = session();
    if (!c)
        return;
    if (!c->run(tile, dims, 4, logp, od, ond, &err)) {
        if (!m_infer_warned.exchange(true))
            std::fprintf(stderr, "deepcw: inference failed: %s\n", err.c_str());
        return;
    }
    To = check_output(T, od, ond, 1) ? output_frames(T) : 0;
}

void DeepcwRuntime::workers_ensure() {
    std::call_once(m_workers_once, [this] {
        for (int i = 0; i < m_cfg.workers; i++)
            m_workers.emplace_back([this] { worker_main(); });
    });
}

bool DeepcwRuntime::submit(std::unique_ptr<DeepcwJob> job, bool force) {
    workers_ensure();
    {
        std::lock_guard<std::mutex> lk(m_jobs_lock);
        if (!force && m_jobs.size() > 256) {
            if (!m_behind_warned.exchange(true)) {
                std::fprintf(stderr, "deepcw: inference queue > 256 jobs — workers fall behind, "
                                     "ticks skipped (%d workers)\n", m_cfg.workers);
            }
            return false;
        }
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
            const unsigned T = batch.front()->T;
            for (auto it = m_jobs.begin(); it != m_jobs.end() && batch.size() < static_cast<size_t>(m_cfg.batch);) {
                if ((*it)->T == T) {
                    batch.push_back(std::move(*it));
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
