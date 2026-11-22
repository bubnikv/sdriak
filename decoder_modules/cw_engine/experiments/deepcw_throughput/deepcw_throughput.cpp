/*
 * deepcw_throughput — CPU throughput of the DeepCW models over many
 * channels: how many keyed channels one core carries, and at which batch,
 * worker and thread counts (README.md beside this file).
 *
 * Two modes, both on synthetic tiles of one steady-state window length:
 *  - raw:     W threads share one session (I intra-op threads) and run
 *             batches of B equal-length windows back to back — the model's
 *             own rate, the runtime's queue not involved;
 *  - runtime: N channels keep one window each in flight through
 *             DeepcwRuntime (its job queue, its workers, its equal-length
 *             batching) — the engine's path, with its clamps (workers 1..8,
 *             batch 1..256).
 * Each configuration reports windows/s and the process CPU time it used, so
 * the core cost of one window is measured, spinning threads included. At
 * one tick per second, one window per second is one keyed channel.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "engine/skim_common.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include "engine/decode_deepcw2.h"
#include "engine/deepcw_runtime.h"
#include "engine/ort_shim.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define DCWT_HAVE_FTZ 1
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ---- the model's cost, counted from its layer shapes (DEEPCW_EXPLAINER.html §4)

struct ModelShape {
    const char *key;
    const DeepcwModelSpec *spec;
    int d, ff, blocks, kernel, heads;
    int proj;               // features into the first linear layer
    double front_per_frame; // conv MACs per frame the front end emits
    bool front_strided;     // the first conv halves time
    int populated_lo, populated_hi;   // tile bins that carry the channel
};

const ModelShape kShapes[] = {
    { "v1", &kDeepcwModelV1, 192, 384, 6, 17, 6, 864,
      32.0 * 33 * 9 + 64.0 * 17 * 288 + 96.0 * 9 * 576, false, 27, 37 },
    { "wide", &kDeepcwModelV2, 192, 384, 6, 17, 6, 864,
      32.0 * 33 * 9 + 64.0 * 17 * 288 + 96.0 * 9 * 576, true, 27, 37 },
    { "narrow", &kDeepcwModelV2Narrow, 64, 128, 3, 15, 4, 24,
      12.0 * 2 * 9 + 24.0 * 1 * 108, true, 0, 2 },
};

double window_macs(const ModelShape &m, unsigned T) {
    const double Te = std::ceil(static_cast<double>(T) / m.spec->time_stride);
    const double d = m.d;
    const double blk = 2.0 * (d * m.ff * 2.0) + 3.0 * d * d + d * d + 2.0 * d * d + d * m.kernel + d * d;
    const double front = m.front_per_frame * (m.front_strided ? Te : static_cast<double>(T));
    return front + Te * (m.proj * d + m.blocks * blk + d * 42.0) + m.blocks * 2.0 * Te * Te * d;
}

// ---- process CPU time

double cpu_seconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0.0;
    auto s = [](const FILETIME &f) {
        return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7;
    };
    return s(k) + s(u);
#else
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
#endif
}

void set_ftz() {
#ifdef DCWT_HAVE_FTZ
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// ---- synthetic tiles: Rayleigh noise on the populated bins, a keyed line
// on the centre one (random Morse-like keying), normalised as the backend
// normalises them (v2: noise_q90_log1p over the populated bins; v1: window
// peak → 30, log1p). Throughput does not depend on the values beyond
// keeping them in the model's range.

std::vector<float> make_tile(const ModelShape &m, unsigned T, double snr_db, std::mt19937 &rng) {
    const int bins = m.spec->bins;
    std::vector<float> tile(static_cast<size_t>(T) * bins, 0.0f);
    std::uniform_real_distribution<double> U(1e-12, 1.0);
    const int centre = (m.populated_lo + m.populated_hi) / 2;
    const double amp = std::pow(10.0, snr_db / 20.0);
    const unsigned dit = 3;   // 16 ms frames: ~24 WPM
    bool key = false;
    unsigned left = 0;
    for (unsigned t = 0; t < T; t++) {
        if (left == 0) {
            key = !key;
            const unsigned n = key ? (U(rng) < 0.5 ? 1 : 3) : (U(rng) < 0.6 ? 1 : (U(rng) < 0.7 ? 3 : 7));
            left = n * dit;
        }
        left--;
        for (int b = m.populated_lo; b <= m.populated_hi; b++) {
            double v = std::sqrt(-2.0 * std::log(U(rng)));   // Rayleigh, σ = 1
            if (key && b == centre)
                v += amp;
            tile[static_cast<size_t>(t) * bins + b] = static_cast<float>(v);
        }
    }
    if (m.spec->time_stride == 1) {
        const float peak = *std::max_element(tile.begin(), tile.end());
        const float g = peak > 0.0f ? 30.0f / peak : 1.0f;
        for (float &v : tile)
            v = std::log1p(v * g);
    } else {
        std::vector<float> src;
        src.reserve(static_cast<size_t>(T) * (m.populated_hi - m.populated_lo + 1));
        for (unsigned t = 0; t < T; t++) {
            for (int b = m.populated_lo; b <= m.populated_hi; b++)
                src.push_back(tile[static_cast<size_t>(t) * bins + b]);
        }
        deepcw2_normalize(tile.data(), tile.size(), src.data(), src.size());
    }
    return tile;
}

// ---- options

std::vector<int> parse_list(const char *s) {
    std::vector<int> v;
    while (s && *s) {
        char *end = nullptr;
        const long x = std::strtol(s, &end, 10);
        if (end == s)
            break;
        if (x > 0)
            v.push_back(static_cast<int>(x));
        s = *end == ',' ? end + 1 : end;
    }
    return v;
}

struct Options {
    std::string model_key = "narrow";
    std::string model_path;
    std::string ort_lib;
    bool verify = true;
    bool ftz = false;
    bool raw = true, runtime = true;
    double window_s = 7.0;
    double duration_s = 3.0;
    double snr_db = 6.0;
    int pool = 64;
    int channels = 300;
    std::vector<int> batches = { 1, 8, 32, 64, 128 };
    std::vector<int> threads = { 1, 2, 4 };
    std::vector<int> workers;   // empty = 1, 2, 4, … up to the hardware threads
};

void usage() {
    std::printf(
        "deepcw_throughput [options]\n"
        "  --model narrow|wide|v1   model contract (default narrow)\n"
        "  --path FILE              model file (default: SKIM_DEEPCW2_NARROW_MODEL,\n"
        "                           SKIM_DEEPCW2_MODEL or SKIM_DEEPCW_MODEL by contract)\n"
        "  --ort LIB                ONNX Runtime library (default: SKIM_ORT_LIB, else\n"
        "                           the platform's names)\n"
        "  --noverify               skip the sha256 pin (a transformed model)\n"
        "  --mode raw|runtime|both  (default both)\n"
        "  --window S               window length in seconds (default 7: deepcw2's\n"
        "                           steady state; v1 reads its 10 s ring)\n"
        "  --duration S             timed seconds per configuration (default 3)\n"
        "  --batch L                raw: batch sizes, comma list (default 1,8,32,64,128);\n"
        "                           runtime: DeepcwRuntimeConfig::batch values\n"
        "  --threads L              intra-op threads per session (default 1,2,4)\n"
        "  --workers L              worker threads (default 1,2,4,… to the hardware)\n"
        "  --channels N             runtime: channels with a window in flight (300)\n"
        "  --snr DB                 keyed line over the noise in the tiles (6)\n"
        "  --ftz                    flush denormals to zero on the calling threads\n");
}

bool parse(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--model") { const char *v = next(); if (!v) return false; o.model_key = v; }
        else if (a == "--path") { const char *v = next(); if (!v) return false; o.model_path = v; }
        else if (a == "--ort") { const char *v = next(); if (!v) return false; o.ort_lib = v; }
        else if (a == "--noverify") o.verify = false;
        else if (a == "--ftz") o.ftz = true;
        else if (a == "--mode") {
            const char *v = next(); if (!v) return false;
            const std::string m = v;
            o.raw = m == "raw" || m == "both";
            o.runtime = m == "runtime" || m == "both";
            if (!o.raw && !o.runtime) return false;
        }
        else if (a == "--window") { const char *v = next(); if (!v) return false; o.window_s = std::atof(v); }
        else if (a == "--duration") { const char *v = next(); if (!v) return false; o.duration_s = std::atof(v); }
        else if (a == "--snr") { const char *v = next(); if (!v) return false; o.snr_db = std::atof(v); }
        else if (a == "--channels") { const char *v = next(); if (!v) return false; o.channels = std::max(1, std::atoi(v)); }
        else if (a == "--batch") { o.batches = parse_list(next()); if (o.batches.empty()) return false; }
        else if (a == "--threads") { o.threads = parse_list(next()); if (o.threads.empty()) return false; }
        else if (a == "--workers") { o.workers = parse_list(next()); if (o.workers.empty()) return false; }
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

std::string env_or(const char *name) {
    const char *v = std::getenv(name);
    return v && v[0] ? std::string(v) : std::string();
}

// ---- one result line

struct Result {
    double windows = 0, wall = 0, cpu = 0;
};

void print_header(const char *mode) {
    std::printf("\n%-8s %7s %7s %7s %10s %8s %9s %10s %10s\n", mode, "threads", "workers", "batch",
                "windows/s", "cores", "ms/window", "GMAC/s/core", "ch/core");
}

void print_result(const char *mode, int threads, int workers, int batch, const Result &r, double macs) {
    const double wps = r.windows / r.wall;
    const double cores = r.cpu / r.wall;
    const double cpu_per_window = r.windows > 0 ? r.cpu / r.windows : 0.0;   // core-seconds
    std::printf("%-8s %7d %7d %7d %10.1f %8.2f %9.2f %10.2f %10.1f\n", mode, threads, workers, batch, wps,
                cores, 1e3 * cpu_per_window, cpu_per_window > 0 ? macs / cpu_per_window / 1e9 : 0.0,
                cpu_per_window > 0 ? 1.0 / cpu_per_window : 0.0);
}

// ---- raw mode

bool check_shape(const ModelShape &m, unsigned T, int64_t batch, const int64_t *od, int ond) {
    const int64_t To = (T + m.spec->time_stride - 1) / m.spec->time_stride;
    return ond == 3 && od[0] == batch && od[1] == To && od[2] == m.spec->classes;
}

// The same window alone and inside a batch of equal-length windows must
// read the same (DeepcwRuntime's batching rule): the max |Δ| of the logits.
double batch_consistency(const SkimOrtSession &s, const ModelShape &m, const std::vector<std::vector<float>> &pool,
                         unsigned T) {
    const size_t row = static_cast<size_t>(T) * m.spec->bins;
    const int64_t d1[4] = { 1, 1, static_cast<int64_t>(T), m.spec->bins };
    std::vector<float> one, many;
    int64_t od[8];
    int ond = 0;
    std::string err;
    if (!s.run(pool[0].data(), d1, 4, one, od, ond, &err))
        return -1.0;
    const int64_t B = std::min<int64_t>(8, static_cast<int64_t>(pool.size()));
    std::vector<float> in(static_cast<size_t>(B) * row);
    for (int64_t i = 0; i < B; i++)
        std::copy_n(pool[static_cast<size_t>(i)].data(), row, in.data() + static_cast<size_t>(i) * row);
    const int64_t dB[4] = { B, 1, static_cast<int64_t>(T), m.spec->bins };
    if (!s.run(in.data(), dB, 4, many, od, ond, &err) || one.size() * static_cast<size_t>(B) != many.size())
        return -1.0;
    double worst = 0.0;
    for (size_t i = 0; i < one.size(); i++)
        worst = std::max(worst, static_cast<double>(std::fabs(one[i] - many[i])));
    return worst;
}

Result run_raw(const SkimOrtSession &s, const ModelShape &m, const std::vector<std::vector<float>> &pool,
               unsigned T, int workers, int batch, double duration, bool ftz, bool &shape_ok) {
    const size_t row = static_cast<size_t>(T) * m.spec->bins;
    std::atomic<bool> go{ false }, stop{ false }, bad{ false };
    std::atomic<long long> done{ 0 };
    std::atomic<int> ready{ 0 };
    std::vector<std::thread> th;
    for (int w = 0; w < workers; w++) {
        th.emplace_back([&, w] {
            if (ftz)
                set_ftz();
            std::vector<float> in(static_cast<size_t>(batch) * row), out;
            for (int i = 0; i < batch; i++) {
                const auto &src = pool[static_cast<size_t>(w * batch + i) % pool.size()];
                std::copy_n(src.data(), row, in.data() + static_cast<size_t>(i) * row);
            }
            const int64_t dims[4] = { batch, 1, static_cast<int64_t>(T), m.spec->bins };
            int64_t od[8];
            int ond = 0;
            std::string err;
            // warm-up: the first run of a shape allocates and plans
            if (!s.run(in.data(), dims, 4, out, od, ond, &err) || !check_shape(m, T, batch, od, ond))
                bad = true;
            ready++;
            while (!go.load())
                std::this_thread::yield();
            while (!stop.load()) {
                if (!s.run(in.data(), dims, 4, out, od, ond, &err)) {
                    bad = true;
                    break;
                }
                done += batch;
            }
        });
    }
    while (ready.load() < workers)
        std::this_thread::yield();
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    go = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(duration));
    stop = true;
    for (auto &t : th)
        t.join();
    Result r;
    r.wall = std::chrono::duration<double>(Clock::now() - t0).count();
    r.cpu = cpu_seconds() - c0;
    r.windows = static_cast<double>(done.load());
    shape_ok = !bad.load();
    return r;
}

// ---- runtime mode

Result run_runtime(const Options &o, const ModelShape &m, const std::vector<std::vector<float>> &pool, unsigned T,
                   int threads, int workers, int batch, bool &ok) {
    DeepcwRuntimeConfig cfg;
    cfg.model_path  = o.model_path;
    cfg.ort_library = o.ort_lib;
    cfg.threads     = threads;
    cfg.workers     = workers;
    cfg.batch       = batch;
    cfg.verify      = o.verify;
    DeepcwRuntime rt(*m.spec, cfg);
    std::string err;
    ok = rt.ensure_session(&err);
    if (!ok) {
        std::fprintf(stderr, "runtime: %s\n", err.c_str());
        return {};
    }
    const unsigned To = rt.output_frames(T);
    std::vector<std::shared_ptr<DeepcwMailbox>> boxes(static_cast<size_t>(o.channels));
    std::vector<bool> inflight(boxes.size(), false);
    for (auto &b : boxes)
        b = std::make_shared<DeepcwMailbox>();
    auto submit = [&](size_t c) {
        auto job = std::make_unique<DeepcwJob>();
        job->box  = boxes[c];
        job->tile = pool[c % pool.size()];
        job->T    = T;
        job->w0   = c;
        if (rt.submit(std::move(job), false))
            inflight[c] = true;
    };
    long long done = 0, failed = 0;
    auto reap = [&]() {
        for (size_t c = 0; c < boxes.size(); c++) {
            if (!inflight[c])
                continue;
            DeepcwMailbox &b = *boxes[c];
            std::lock_guard<std::mutex> lk(b.lock);
            if (!b.ready)
                continue;
            b.ready = false;
            inflight[c] = false;
            if (b.T == To)
                done++;
            else
                failed++;
        }
    };
    // warm-up: one round of every channel (each worker plans its shapes)
    for (size_t c = 0; c < boxes.size(); c++)
        submit(c);
    for (auto t = Clock::now(); Clock::now() - t < std::chrono::seconds(60);) {
        reap();
        if (std::none_of(inflight.begin(), inflight.end(), [](bool b) { return b; }))
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    done = failed = 0;
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    const auto until = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration_s));
    while (Clock::now() < until) {
        for (size_t c = 0; c < boxes.size(); c++) {
            if (!inflight[c])
                submit(c);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        reap();
    }
    Result r;
    r.wall = std::chrono::duration<double>(Clock::now() - t0).count();
    r.cpu = cpu_seconds() - c0;   // includes this polling thread (small)
    r.windows = static_cast<double>(done);
    // let the in-flight jobs finish before the runtime goes
    for (auto t = Clock::now(); Clock::now() - t < std::chrono::seconds(60);) {
        reap();
        if (std::none_of(inflight.begin(), inflight.end(), [](bool b) { return b; }))
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (failed)
        std::fprintf(stderr, "runtime: %lld windows came back empty\n", failed);
    ok = failed == 0;
    return r;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }
    const ModelShape *m = nullptr;
    for (const ModelShape &s : kShapes) {
        if (o.model_key == s.key)
            m = &s;
    }
    if (!m) {
        std::fprintf(stderr, "unknown model %s (narrow, wide, v1)\n", o.model_key.c_str());
        return 2;
    }
    if (o.model_path.empty()) {
        o.model_path = env_or(m->spec == &kDeepcwModelV2Narrow ? "SKIM_DEEPCW2_NARROW_MODEL"
                              : m->spec == &kDeepcwModelV2     ? "SKIM_DEEPCW2_MODEL"
                                                               : "SKIM_DEEPCW_MODEL");
    }
    if (o.ort_lib.empty())
        o.ort_lib = env_or("SKIM_ORT_LIB");
    if (o.model_path.empty()) {
        std::fprintf(stderr, "no model file: --path or the model's SKIM_* variable\n");
        return 2;
    }
    if (o.verify) {
        const std::string why = deepcw_model_mismatch(*m->spec, o.model_path);
        if (!why.empty()) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 2;
        }
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    if (o.workers.empty()) {
        for (unsigned w = 1; w <= hw; w *= 2)
            o.workers.push_back(static_cast<int>(w));
        if (o.workers.back() != static_cast<int>(hw))
            o.workers.push_back(static_cast<int>(hw));
    }

    const double fps = 250.0 / 4.0;   // the channel front end: 4 samples of 250 Hz
    unsigned T = static_cast<unsigned>(std::lround(o.window_s * fps));
    T -= T % static_cast<unsigned>(m->spec->time_stride);
    const double macs = window_macs(*m, T);

    std::mt19937 rng(12345);
    std::vector<std::vector<float>> pool;
    for (int i = 0; i < o.pool; i++)
        pool.push_back(make_tile(*m, T, o.snr_db, rng));

    std::printf("deepcw_throughput: %s, %s\n", m->spec->name, o.model_path.c_str());
    std::printf("window %.2f s = %u frames, %.1f MMAC per window (counted), %u hardware threads%s\n",
                T / fps, T, macs / 1e6, hw, o.ftz ? ", FTZ/DAZ on" : "");
    std::printf("columns: cores = process CPU time / wall time; ms/window = CPU per window;\n"
                "         ch/core = keyed channels one core carries at one 1 s tick each\n");

    std::string err;

    struct Best { double cpw = 0; int threads = 0, workers = 0, batch = 0; const char *mode = ""; } best;
    auto keep = [&](const char *mode, int t, int w, int b, const Result &r) {
        if (r.windows <= 0)
            return;
        const double cpw = r.cpu / r.windows;
        if (best.cpw == 0 || cpw < best.cpw)
            best = { cpw, t, w, b, mode };
    };
    int rc = 0;

    if (o.raw) {
        // Its own environment, gone before the runtime mode opens the
        // process-wide one (deepcw_runtime.cpp shared_ort: one per process).
        std::unique_ptr<SkimOrt> ort = SkimOrt::open(o.ort_lib, &err);
        if (!ort) {
            std::fprintf(stderr, "ONNX Runtime: %s\n", err.c_str());
            return 2;
        }
        std::printf("ONNX Runtime %s via %s\n", ort->version().c_str(), ort->library().c_str());
        print_header("raw");
        for (int t : o.threads) {
            std::unique_ptr<SkimOrtSession> s = SkimOrtSession::create(*ort, o.model_path, t, "cpu", &err);
            if (!s) {
                std::fprintf(stderr, "session (%d threads): %s\n", t, err.c_str());
                return 2;
            }
            if (t == o.threads.front()) {
                const double diff = batch_consistency(*s, *m, pool, T);
                std::printf("batch consistency: max |logit(alone) - logit(in a batch of 8)| = %.3g\n", diff);
            }
            for (int w : o.workers) {
                if (static_cast<unsigned>(w * t) > 2 * hw)
                    continue;   // oversubscribed beyond any use
                for (int b : o.batches) {
                    bool shape_ok = true;
                    const Result r = run_raw(*s, *m, pool, T, w, b, o.duration_s, o.ftz, shape_ok);
                    if (!shape_ok) {
                        std::fprintf(stderr, "raw: a run failed or returned the wrong shape\n");
                        rc = 1;
                    }
                    print_result("raw", t, w, b, r, macs);
                    keep("raw", t, w, b, r);
                    std::fflush(stdout);
                }
            }
        }
    }

    if (o.runtime) {
        std::printf("\nruntime: %d channels, one window each in flight (DeepcwRuntime clamps workers "
                    "to 1..8, batch to 1..256)\n", o.channels);
        print_header("runtime");
        for (int t : o.threads) {
            for (int w : o.workers) {
                if (w > 8 || static_cast<unsigned>(w * t) > 2 * hw)
                    continue;
                for (int b : o.batches) {
                    bool ok = true;
                    const Result r = run_runtime(o, *m, pool, T, t, w, b, ok);
                    if (!ok)
                        rc = 1;
                    if (r.wall <= 0.0)
                        continue;   // no session
                    print_result("runtime", t, w, b, r, macs);
                    keep("runtime", t, w, b, r);
                    std::fflush(stdout);
                }
            }
        }
    }

    if (best.cpw > 0) {
        std::printf("\nbest: %s, %d threads x %d workers, batch %d: %.2f ms CPU per window, "
                    "%.1f keyed channels per core; %d channels need %.2f cores\n",
                    best.mode, best.threads, best.workers, best.batch, 1e3 * best.cpw, 1.0 / best.cpw,
                    o.channels, o.channels * best.cpw);
    }
    return rc;
}
