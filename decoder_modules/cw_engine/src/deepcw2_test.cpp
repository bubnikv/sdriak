/*
 * deepcw2_test — offline gate for the DeepCW v2 backend (decode_deepcw2.h).
 *
 * Sections: (N) the noise_q90_log1p normalisation; (A) the posterior
 * stitcher on synthetic CTC streams — one final window, overlapping
 * jittered windows with nothing doubled or lost, the blank-run word gap,
 * restart; (B) the DSP half on synthetic baseband (rate rejection, gate,
 * the narrow tile's line bin); (C) the v2 models — only when
 * SKIM_DEEPCW2_MODEL (and, for the narrow model, SKIM_DEEPCW2_NARROW_MODEL)
 * resolve with SKIM_ORT_LIB; exit 77 = SKIP otherwise.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "engine/skim_common.h"

#include <chrono>
#include <random>
#include <thread>

#include "engine/decode.h"
#include "engine/decode_deepcw2.h"

namespace {

int fails, checks;
void check(const char *what, bool ok) {
    checks++;
    if (!ok)
        fails++;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

constexpr int NC = DeepcwModel::kClasses;
constexpr int BLANK = DeepcwModel::kBlank;
constexpr int SPACE = DeepcwModel::kSpace;

int cls_of(char c) {
    for (int i = 0; i < NC - 1; i++) {
        if (DeepcwModel::kChars[i] == c)
            return i;
    }
    return -1;
}

// Log-probs [T][42]: blank everywhere, a character spike where asked.
std::vector<float> blank_stream(size_t T) {
    std::vector<float> m(T * NC);
    for (size_t t = 0; t < T; t++) {
        for (int c = 0; c < NC; c++)
            m[t * NC + c] = std::log(0.05f / 41.0f);
        m[t * NC + BLANK] = std::log(0.95f);
    }
    return m;
}

void spike(std::vector<float> &m, size_t t, int cls, float p = 0.95f) {
    for (int c = 0; c < NC; c++)
        m[t * NC + c] = std::log((1.0f - p) / 41.0f);
    m[t * NC + cls] = std::log(p);
}

// "text" as two-frame spikes every `step` frames; ' ' = a space spike.
size_t write_text(std::vector<float> &m, size_t t0, size_t step, const char *text) {
    size_t t = t0;
    for (const char *s = text; *s; s++, t += step) {
        const int c = *s == ' ' ? SPACE : cls_of(*s);
        spike(m, t, c);
        spike(m, t + 1, c);
    }
    return t;
}

// Stream a global log-prob matrix through the stitcher the way the channel
// does: a window every `step` frames ending at the current frame, shifted
// by `jitter` frames on odd windows, then the final window.
std::string stream(DeepcwStitcher &st, const std::vector<float> &g, size_t T, int64_t step, int jitter,
                   std::string *pending_seen = nullptr) {
    std::string text;
    bool has = false;
    auto emit = [&](const DeepcwStitcher::Take &t) {
        if (t.committed && !t.committed->text.empty()) {
            text += (t.committed->prepend_space && has ? " " : "") + t.committed->text;
            has = true;
        }
        if (pending_seen && !t.pending.text.empty())
            *pending_seen = t.pending.text;
    };
    int k = 0;
    for (int64_t end = 2 * step; end <= static_cast<int64_t>(T); end += step, k++) {
        const int64_t s = st.window_start_frame(end);
        std::vector<float> w(g.begin() + s * NC, g.begin() + end * NC);
        if (jitter && (k & 1)) {   // the same spikes a frame or two later
            std::vector<float> sh = blank_stream(static_cast<size_t>(end - s));
            for (int64_t f = 0; f + jitter < end - s; f++)
                std::copy_n(w.begin() + f * NC, NC, sh.begin() + (f + jitter) * NC);
            w = sh;
        }
        st.add_window(s, w.data(), static_cast<unsigned>(end - s), false);
        emit(st.take(false));
    }
    const int64_t s = st.context_start_frame();
    st.add_window(s, g.data() + s * NC, static_cast<unsigned>(static_cast<int64_t>(T) - s), true);
    emit(st.take(true));
    return text;
}

// ---- synthetic complex baseband (deepcw_test.cpp's) --------------------------
double rand_double(std::mt19937 &r) {
    for (;;) {
        double v = r() * 2.3283064365386962890625e-10;
        v = (v + r()) * 2.3283064365386962890625e-10;
        if (v < 1.0)
            return v;
    }
}

const char *morse(char c) {
    static const char *M[] = { "A.-", "B-...", "C-.-.", "D-..", "E.", "F..-.", "G--.",
        "H....", "I..", "J.---", "K-.-", "L.-..", "M--", "N-.", "O---", "P.--.", "Q--.-",
        "R.-.", "S...", "T-", "U..-", "V...-", "W.--", "X-..-", "Y-.--", "Z--..",
        "0-----", "1.----", "2..---", "3...--", "4....-", "5.....", "6-....", "7--...",
        "8---..", "9----.", nullptr };
    for (int i = 0; M[i]; i++) {
        if (M[i][0] == c)
            return M[i] + 1;
    }
    return "";
}

std::vector<float> keyer(const char *text, double wpm, double rate, int lead, int trail) {
    std::vector<float> e;
    const double dit = 1.2 / wpm;
    auto put = [&](double n, float v) {
        for (int i = 0; i < static_cast<int>(std::llround(n * dit * rate)); i++)
            e.push_back(v);
    };
    put(lead, 0.0f);
    for (const char *s = text; *s; s++) {
        if (*s == ' ') {
            put(4, 0.0f);
            continue;
        }
        for (const char *m = morse(*s); *m; m++) {
            put(*m == '.' ? 1 : 3, 1.0f);
            put(1, 0.0f);
        }
        put(2, 0.0f);
    }
    put(trail, 0.0f);
    return e;
}

std::vector<float> tone_iq(const std::vector<float> &env, double rate, double off_hz, float amp, float sigma,
                           unsigned seed) {
    std::mt19937 r(seed);
    std::vector<float> iq(2 * env.size());
    double e = 0, a = std::exp(-1.0 / (0.005 * rate));
    for (size_t n = 0; n < env.size(); n++) {
        e = a * e + (1 - a) * env[n];
        const double ph = 2.0 * SKIM_PI * off_hz * n / rate;
        double ni = 0, nq = 0;
        if (sigma > 0) {
            const double u1 = std::max(rand_double(r), 1e-12), u2 = rand_double(r);
            const double m = std::sqrt(-2.0 * std::log(u1));
            ni = sigma * m * std::cos(2 * SKIM_PI * u2);
            nq = sigma * m * std::sin(2 * SKIM_PI * u2);
        }
        iq[2 * n] = static_cast<float>(amp * e * std::cos(ph) + ni);
        iq[2 * n + 1] = static_cast<float>(amp * e * std::sin(ph) + nq);
    }
    return iq;
}

// Feed in 64-frame blocks, then drain the workers (async) or not (sync).
std::string run(SkimDecodeChannel &st, const std::vector<float> &iq, bool sync, bool hold_at_end) {
    std::string o;
    SkimDecode d;
    const unsigned n = static_cast<unsigned>(iq.size() / 2);
    for (unsigned i = 0; i < n; i += 64) {
        if (st.process(iq.data() + 2 * i, std::min(64u, n - i), &d))
            o += d.text;
        if (!sync)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (hold_at_end)
        st.hold_begin();
    for (int k = 0, idle = 0; k < 500 && idle < 5; k++) {
        if (st.process(iq.data(), 0, &d)) {
            o += d.text;
            idle = 0;
        }
        const auto dbg = SkimDeepcw2Backend::debug(st);
        if (dbg && dbg->inflight) {
            idle = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            idle++;
        }
    }
    return o;
}

void model_section(const char *label, DeepcwV2Config cfg, const char *path) {
    cfg.model_path = path;
    std::printf("[C] %s: %s\n", label, path);
    SkimDeepcw2Backend be(cfg);
    std::string err;
    if (!be.available(&err)) {
        std::printf("      unavailable: %s\n", err.c_str());
        check("the model loads (pin, runtime)", false);
        return;
    }
    std::printf("      runtime %s\n", be.runtime_info().c_str());
    const double RATE = 250.0;
    {
        const std::vector<float> env = keyer("CQ CQ DE OK1BR OK1BR K CQ CQ DE OK1BR OK1BR K", 25.0, RATE, 40, 120);
        const std::vector<float> iq = tone_iq(env, RATE, +20.0, 0.05f, 0.004f, 5);
        auto st = be.channel_new(RATE);
        const std::string o = run(*st, iq, cfg.sync, true);
        std::printf("      text |%s|\n", o.c_str());
        check("reads the call (OK1BR)", o.find("OK1BR") != std::string::npos);
        check("reads CQ", o.find("CQ") != std::string::npos);
        check("single spaces at the commit seams", o.find("  ") == std::string::npos);
        check("no call doubled across windows", o.find("OK1BROK1BR") == std::string::npos &&
                                                    o.find("OK1BR OK1BR OK1BR") == std::string::npos);
    }
    {
        const std::vector<float> env0(20 * static_cast<unsigned>(RATE), 0.0f);
        const std::vector<float> iq = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 6);
        DeepcwV2Config c2 = cfg;
        c2.gate = false;                 // the model alone on noise
        SkimDeepcw2Backend be2(c2);
        auto sn = be2.channel_new(RATE);
        const std::string o = run(*sn, iq, cfg.sync, false);
        std::printf("      noise (gate off) |%s|\n", o.c_str());
        check("noise only, gate off: no text", o.empty());
    }
}

} // namespace

int main() {
    std::printf("=== deepcw2_test ===\n");

    // ---- (N) normalisation --------------------------------------------------
    std::printf("[N] noise_q90_log1p\n");
    {
        std::mt19937 r(1);
        std::vector<float> x(4000);
        for (float &v : x) {   // Rayleigh, σ = 2
            v = static_cast<float>(2.0 * std::sqrt(-2.0 * std::log(std::max(rand_double(r), 1e-12))));
        }
        std::vector<float> y = x;
        deepcw2_normalize(y.data(), y.size(), y.data(), y.size());
        std::vector<float> s = y;
        std::sort(s.begin(), s.end());
        const float q = s[static_cast<size_t>(std::floor(0.9 * (s.size() - 1)))];
        check("the 90th percentile lands on log1p(sqrt(-2 ln 0.1))",
              std::fabs(q - std::log1p(std::sqrt(-2.0 * std::log(0.1)))) < 1e-4);
        const float med = s[s.size() / 2];
        check("Rayleigh noise: the median reads log1p(σ·1.18 / σ) ± 5 %",
              std::fabs(med - std::log1p(1.1774)) < 0.05 * std::log1p(1.1774));
        std::vector<float> z(100, 0.0f);
        deepcw2_normalize(z.data(), z.size(), z.data(), z.size());
        check("all zero stays zero (floor 1e-5)", *std::max_element(z.begin(), z.end()) == 0.0f);
        z[7] = 5.0f;
        deepcw2_normalize(z.data(), z.size(), z.data(), z.size());
        check("a lone peak reads against peak·1e-3", std::fabs(z[7] - std::log1p(1000.0f)) < 1e-3);
        std::vector<float> tile(10, 0.0f), src = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
        tile[3] = 2.0f;
        deepcw2_normalize(tile.data(), tile.size(), src.data(), src.size());
        check("the noise level comes from the source cells, not the zero fill",
              std::fabs(tile[3] - std::log1p(2.0 * std::sqrt(-2.0 * std::log(0.1)))) < 1e-3 && tile[0] == 0.0f);
    }

    // ---- (A) the stitcher ---------------------------------------------------
    std::printf("[A] posterior stitcher\n");
    const DeepcwStitchConfig sc;
    const double fps = 250.0 / 4 / 2;   // the channel's output frames: 31.25/s
    {
        DeepcwStitcher st(sc, fps);
        std::vector<float> g = blank_stream(200);
        write_text(g, 20, 8, "CQ DE");
        st.add_window(0, g.data(), 200, true);
        const auto t = st.take(true);
        check("one final window commits everything", t.committed && t.committed->text == "CQ DE");
    }
    {
        const size_t T = 1200;   // 38 s
        std::vector<float> g = blank_stream(T);
        size_t t = write_text(g, 40, 7, "CQ CQ DE OK1BR OK1BR K ");
        t = write_text(g, t + 30, 7, "TEST DE OK1BR");
        write_text(g, t + 200, 7, "5NN TU");   // 200 frames = 6.4 s of silence before it
        DeepcwStitcher a(sc, fps), b(sc, fps);
        std::string pend;
        const std::string ra = stream(a, g, T, 31, 0, &pend);
        const std::string rb = stream(b, g, T, 31, 1);
        std::printf("      steady |%s|\n      jitter |%s|\n", ra.c_str(), rb.c_str());
        check("overlapping windows: nothing doubled, nothing lost",
              ra == "CQ CQ DE OK1BR OK1BR K TEST DE OK1BR 5NN TU");
        check("windows jittered by a frame read the same (dilation)", rb == ra);
        check("pending text was offered before the commit", !pend.empty());
    }
    {
        DeepcwStitcher st(sc, fps);
        std::vector<float> g = blank_stream(400);
        spike(g, 20, cls_of('A'));
        spike(g, 21, cls_of('A'));
        spike(g, 20 + 80, cls_of('B'));            // 80 frames = 2.56 s ≥ 2.01 s
        spike(g, 20 + 80 + 20, cls_of('C'));       // 0.64 s: no gap
        st.add_window(0, g.data(), 400, true);
        const auto t = st.take(true);
        check("a blank run ≥ 2.01 s is a word gap, a shorter one is not",
              t.committed && t.committed->text == "A BC");
    }
    {
        DeepcwStitcher st(sc, fps);
        std::vector<float> g = blank_stream(100);
        write_text(g, 10, 8, "OK1BR");
        st.add_window(0, g.data(), 100, true);
        st.take(true);
        st.restart(500);
        std::vector<float> h = blank_stream(100);
        write_text(h, 10, 8, "5NN");
        st.add_window(500, h.data(), 100, true);
        const auto t = st.take(true);
        check("after restart the text starts a new word",
              t.committed && t.committed->text == "5NN" && t.committed->prepend_space);
    }

    // ---- (B) DSP half -------------------------------------------------------
    std::printf("[B] front end + gate (no model needed)\n");
    DeepcwV2Config cfg = DeepcwV2Config::from_env();
    cfg.sync = true;
    {
        DeepcwV2Config nc = cfg;
        nc.model_path.clear();
        nc.narrow = true;
        SkimDeepcw2Backend be(nc);
        {
            auto bad = be.channel_new(333.0);
            const auto d = SkimDeepcw2Backend::debug(*bad);
            check("rate 333 Hz → dead state", d && d->dead);
        }
        const double RATE = 250.0;
        const std::vector<float> env = keyer("CQ CQ DE OK1BR K", 25.0, RATE, 8, 8);
        const std::vector<float> iq = tone_iq(env, RATE, +30.0, 0.1f, 0.003f, 1);
        auto st = be.channel_new(RATE);
        const std::string o = run(*st, iq, true, false);
        const auto d = SkimDeepcw2Backend::debug(*st);
        std::printf("      keyed: gate %d ratio %.1f dB duty %.2f line bin %d offset %.1f Hz\n", d->gate,
                    d->ratio_db, d->duty, d->line_bin, st->tone_offset_hz());
        check("keyed tone: gate open", d->gate && d->ratio_db >= 6.0);
        check("+30 Hz tone: the narrow tile centres on bin 7 (+25 Hz)", d->line_bin == 7);
        check("+30 Hz tone → offset +30 ± 4 Hz", std::fabs(st->tone_offset_hz() - 30.0) < 4.0);
        check("no model → no text", o.empty() && d->ticks == 0);
        const std::vector<float> env0(8 * static_cast<unsigned>(RATE), 0.0f);
        const std::vector<float> iqn = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 4);
        auto sn = be.channel_new(RATE);
        run(*sn, iqn, true, false);
        check("noise only: gate closed", !SkimDeepcw2Backend::debug(*sn)->gate);
    }

    // ---- (C) the models -----------------------------------------------------
    const char *wide = std::getenv("SKIM_DEEPCW2_MODEL");
    const char *narrow = std::getenv("SKIM_DEEPCW2_NARROW_MODEL");
    if (!(wide && wide[0]) && !(narrow && narrow[0])) {
        std::printf("[C] SKIP — set SKIM_ORT_LIB and SKIM_DEEPCW2_MODEL (model.onnx v2) and/or "
                    "SKIM_DEEPCW2_NARROW_MODEL (model_narrow.onnx)\n");
        std::printf("=== %d checks, %d failed (model section skipped) ===\n", checks, fails);
        return fails ? 1 : 77;
    }
    const bool sync_env = std::getenv("SKIM_DEEPCW2_SYNC") != nullptr;
    cfg.sync = sync_env;
    if (wide && wide[0]) {
        DeepcwV2Config c = cfg;
        c.narrow = false;
        model_section("wide (model.onnx)", c, wide);
    }
    if (narrow && narrow[0]) {
        DeepcwV2Config c = cfg;
        c.narrow = true;
        model_section("narrow (model_narrow.onnx)", c, narrow);
    }
    std::printf("=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
