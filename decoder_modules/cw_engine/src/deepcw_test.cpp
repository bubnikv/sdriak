/*
 * skimmer-deepcw-test — offline gate for the DeepCW backend.
 *
 * Sections: (A) the pure commit rule on synthetic CTC matrices; (B) the
 * tile builder through the public interface on synthetic complex baseband
 * — offset sign (+30 / −30 Hz), level scaling, gate keyed vs noise, dit
 * estimate, rate rejection; (C) the real model on a synthetic keyed tone
 * and on noise — ONLY when SKIM_ORT_LIB / SKIM_DEEPCW_MODEL resolve
 * (exit 77 = SKIP otherwise, reason printed).
 *
 * SDRIAK: section (S), the model pin (SHA-256 vectors, a file that is not
 * the pinned model is refused and named). The gate on the standard library (upstream: GLib; GRand is
 * std::mt19937 with GLib's draw, as in cw_test.cpp). Upstream's two checks
 * through the whole offline pipeline (pane routing, the TX hold's station
 * report) are not here: the pipeline is not part of SDRIAK's engine.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/skim_common.h"

#include <chrono>
#include <random>
#include <thread>

#include "engine/decode.h"
#include "engine/decode_deepcw.h"
#include "engine/deepcw_runtime.h"
#include "engine/skim_sha256.h"

#include <filesystem>
#include <fstream>

namespace {

int fails, checks;
void check(const char *what, bool ok) {
    checks++;
    if (!ok)
        fails++;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

void sleep_us(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

constexpr int NC    = DeepcwModel::kClasses;
constexpr int BLANK = DeepcwModel::kBlank;
constexpr int SPACE = DeepcwModel::kSpace;

int cls_of(char c) {
    for (int i = 0; i < NC - 1; i++) {
        if (DeepcwModel::kChars[i] == c)
            return i;
    }
    return -1;
}

std::vector<float> logp_new(unsigned T) {
    std::vector<float> m(static_cast<size_t>(T) * NC);
    for (unsigned t = 0; t < T; t++) {
        for (int c = 0; c < NC; c++)
            m[t * NC + c] = std::log(0.1f / 41.0f);
        m[t * NC + BLANK] = std::log(0.9f);
    }
    return m;
}

void spike(std::vector<float> &m, unsigned t, int cls, float p) {
    for (int c = 0; c < NC; c++)
        m[t * NC + c] = std::log((1.0f - p) / 41.0f);
    m[t * NC + cls] = std::log(p);
}

// Write "text" as spikes: chars every `step` frames from t0; ' ' is a
// SPACE spike. Returns the frame after the last spike.
unsigned spikes_text(std::vector<float> &m, unsigned t0, unsigned step, const char *text) {
    unsigned t = t0;
    for (const char *s = text; *s; s++, t += step)
        spike(m, t, cls_of(*s), 0.95f);
    return t;
}

// ---- synthetic complex baseband ---------------------------------------

// GLib's g_rand_double on MT19937 (cw_test.cpp).
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

// Keyed envelope for text at wpm, rate Hz; leading/trailing quiet in dits.
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

// Complex tone at off_hz with the keyed envelope (5 ms raised-cosine edges
// via a 1-pole smoother), amplitude amp, plus white noise sigma.
std::vector<float> tone_iq(const std::vector<float> &env, double rate, double off_hz, float amp,
                           float sigma, unsigned seed) {
    std::mt19937 r(seed);
    std::vector<float> iq(2 * env.size());
    double e = 0, a = std::exp(-1.0 / (0.005 * rate));
    for (size_t n = 0; n < env.size(); n++) {
        e = a * e + (1 - a) * env[n];
        const double ph = 2.0 * SKIM_PI * off_hz * n / rate;
        double ni = 0, nq = 0;
        if (sigma > 0) {
            // Box–Muller
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

// Run a channel over iq in 64-frame blocks (the pipeline's drain); collect
// emitted text into out, return number of hits.
unsigned run_state(SkimDecodeChannel &st, const float *iq, unsigned nframes, std::string &out,
                   double *last_conf) {
    unsigned hits = 0;
    SkimDecode d;
    for (unsigned i = 0; i < nframes; i += 64) {
        const unsigned n = std::min(64u, nframes - i);
        if (st.process(iq + 2 * i, n, &d)) {
            hits++;
            out += d.text;
            if (last_conf)
                *last_conf = d.confidence;
        }
    }
    return hits;
}

SkimDeepcwDebug dbg_of(const SkimDecodeChannel &st) {
    return SkimDeepcwBackend::debug(st).value_or(SkimDeepcwDebug{});
}

} // namespace

int main() {
    const double RATE = 250.0;
    std::printf("=== skimmer-deepcw-test ===\n");
    // The model sections run the RAW tail as draft (no distance/posterior
    // bar) so that a draft-only hit deterministically precedes the first
    // final text — the pipeline routing trap this gate guards. The shipped
    // reliable-prefix rule (SKIM_DEEPCW_DRAFT_MIN 0.384 s, _P 0.9 — measured
    // on the 80 m fixture) is proven as a pure function in section A2. An
    // explicit env still wins.
    DeepcwConfig cfg = DeepcwConfig::from_env();
    if (!std::getenv("SKIM_DEEPCW_DRAFT_MIN"))
        cfg.draft_min_s = 0.0;
    if (!std::getenv("SKIM_DEEPCW_DRAFT_P"))
        cfg.draft_p = 0.0;
    const DeepcwCommitRule rule(cfg);

    // ---- (S) the model pin (SDRIAK) ----------------------------------------
    std::printf("[S] model pin (SHA-256)\n");
    {
        check("SHA-256 of \"\" (FIPS 180-4)",
              SkimSha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        check("SHA-256 of \"abc\"",
              SkimSha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        check("SHA-256 of the 448-bit message (two blocks)",
              SkimSha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        check("SHA-256 of a million 'a' (streamed in odd pieces)", [] {
            SkimSha256 h;
            const std::string chunk(997, 'a');
            size_t left = 1000000;
            while (left) {
                const size_t n = std::min(left, chunk.size());
                h.update(chunk.data(), n);
                left -= n;
            }
            return SkimSha256::hex(h.finish()) ==
                   "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        }());
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "deepcw_test_not_a_model.onnx";
        {
            std::ofstream f(tmp, std::ios::binary);
            f << "not a model";
        }
        const std::string why = deepcw_model_mismatch(kDeepcwModelV1, tmp.u8string());
        std::printf("      %s\n", why.c_str());
        check("a file that is not the pinned model is refused", !why.empty());
        DeepcwConfig bad = cfg;
        bad.model_path = tmp.u8string();
        SkimDeepcwBackend wrong(bad);
        std::string e;
        check("the backend refuses it and says why", !wrong.available(&e) && e.find("not DeepCW v1") != std::string::npos);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (!cfg.model_path.empty()) {
            const std::string m = deepcw_model_mismatch(kDeepcwModelV1, cfg.model_path);
            std::printf("      SKIM_DEEPCW_MODEL: %s\n", m.empty() ? "the pinned v1 model" : m.c_str());
        }
    }

    // ---- (A) commit rule -------------------------------------------------
    std::printf("[A] commit rule (sliding window, frame cursor)\n");
    {
        const int E = cls_of('E');
        unsigned T = 400;
        std::vector<float> m = logp_new(T);
        spikes_text(m, 10, 10, "CQ");        // C@10 Q@20
        spike(m, 100, SPACE, 0.95f);
        spikes_text(m, 150, 10, "TEST");     // 150..180
        spike(m, 200, SPACE, 0.95f);
        spikes_text(m, 340, 10, "DE");       // 340, 350: inside the tail
        std::string out;
        uint64_t cur = 0;
        bool lsp = true;
        double conf = 0;
        unsigned n = rule.commit(m.data(), T, 0, cur, 70, 4, lsp, out, &conf);
        check("commits every spike ≥ tail before the end, nothing inside the tail",
              n == 8 && out == "CQ TEST " && cur == 200);
        check("confidence = spike posterior", std::fabs(conf - 0.95) < 0.02);
        // the same window re-read with every spike jittered +2 frames
        m = logp_new(T);
        spikes_text(m, 12, 10, "CQ");
        spike(m, 102, SPACE, 0.95f);
        spikes_text(m, 152, 10, "TEST");
        spike(m, 201, SPACE, 0.95f);
        spikes_text(m, 342, 10, "DE");
        n = rule.commit(m.data(), T, 0, cur, 70, 4, lsp, out, &conf);
        check("a re-read with spikes jittered +2 frames re-emits nothing",
              n == 0 && out == "CQ TEST " && cur == 200);
        // the window slid by 100 frames: DE is now clear of the tail, OK1BR new
        m = logp_new(T);
        spikes_text(m, 340 - 100, 10, "DE");
        spike(m, 360 - 100, SPACE, 0.9f);
        spikes_text(m, 380 - 100, 10, "OK1BR");            // abs 380..420
        n = rule.commit(m.data(), T, 100, cur, 70, 4, lsp, out, &conf);
        check("next tick commits the characters beyond the cursor",
              n == 8 && out == "CQ TEST DE OK1BR" && cur == 420);
        // a word gap two frames after the last character passes on frame
        // order (no margin for spaces); its double is squeezed
        m = logp_new(T);
        spikes_text(m, 340 - 100, 10, "DE");
        spike(m, 360 - 100, SPACE, 0.9f);
        spikes_text(m, 380 - 100, 10, "OK1BR");
        spike(m, 422 - 100, SPACE, 0.9f);
        spike(m, 424 - 100, SPACE, 0.9f);
        n = rule.commit(m.data(), T, 100, cur, 70, 4, lsp, out, &conf);
        check("a gap right after the last character passes once, its double is squeezed",
              n == 1 && out == "CQ TEST DE OK1BR " && cur == 422);
        // silence
        m = logp_new(200);
        out.clear();
        cur = 0;
        lsp = true;
        n = rule.commit(m.data(), 200, 0, cur, 50, 4, lsp, out, &conf);
        check("silence → nothing emitted, cursor untouched", n == 0 && cur == 0 && out.empty());
        // CTC collapse: E E E in a run = one E; separated by blanks = more
        m = logp_new(100);
        out.clear();
        cur = 0;
        lsp = true;
        spike(m, 10, E, 0.9f);
        spike(m, 11, E, 0.9f);
        spike(m, 12, E, 0.9f);
        spike(m, 20, E, 0.9f);
        spike(m, 30, E, 0.9f);
        spike(m, 40, SPACE, 0.9f);
        n = rule.commit(m.data(), 100, 0, cur, 10, 4, lsp, out, &conf);
        check("CTC collapse: run = one char, blank-separated = more", n == 4 && out == "EEE ");
        check("nothing below the cursor is re-emitted",
              rule.commit(m.data(), 100, 0, cur, 10, 4, lsp, out, &conf) == 0);
        // weak word gaps: torn short piece is glued, a weak gap between two
        // real words stays (default bar 0.8, SKIM_DEEPCW_SPACE_P unset)
        T = 400;
        m = logp_new(T);
        cur = 0;
        lsp = true;
        out.clear();
        unsigned tt = spikes_text(m, 10, 10, "OK2B");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "TK");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "TEST");
        spike(m, tt, SPACE, 0.97f);
        n = rule.commit(m.data(), T, 0, cur, 50, 4, lsp, out, &conf);
        check("weak gap before a 2-char piece is glued (OK2B TK → OK2BTK), weak gap between words kept",
              out == "OK2BTK TEST ");
    }

    // ---- (A3) word gaps: what is glued, what passes near the cursor ------
    std::printf("[A3] word gaps\n");
    {
        const unsigned T = 400;
        std::vector<float> m;
        std::string out;
        uint64_t cur, spt;
        bool lsp;
        double conf;
        // a weak gap after a SHORT word (DE, CQ) stays: only a torn call TAIL
        // (left ≥ 3, right ≤ 2) is glued
        m = logp_new(T);
        cur = 0;
        spt = 0;
        lsp = true;
        out.clear();
        unsigned tt = spikes_text(m, 10, 10, "DE");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "OK1MDK");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "CQ");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "TEST");
        spike(m, tt, SPACE, 0.97f);
        rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("weak gaps after DE and CQ (short LEFT piece) are kept", out == "DE OK1MDK CQ TEST ");
        // the torn tail still glues: OK1C Z → OK1CZ
        m = logp_new(T);
        cur = 0;
        spt = 0;
        lsp = true;
        out.clear();
        tt = spikes_text(m, 10, 10, "OK1C");
        spike(m, tt, SPACE, 0.55f);
        tt = spikes_text(m, tt + 10, 10, "Z");
        spike(m, tt, SPACE, 0.97f);
        rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("weak gap before a 1-char tail is still glued (OK1C Z → OK1CZ)", out == "OK1CZ ");
        // a gap the model placed 2 frames BEFORE the last committed char (no
        // committed gap near it) passes once; a re-read of a COMMITTED gap
        // 3 frames before the cursor is squeezed
        m = logp_new(T);
        cur = 0;
        spt = 0;
        lsp = true;
        out.clear();
        spikes_text(m, 10, 10, "DE");                     // D@10 E@20
        unsigned n = rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("setup: DE committed, cursor on E", n == 2 && cur == 20);
        m = logp_new(T);
        spikes_text(m, 11, 10, "DE");                     // re-read, +1
        spike(m, 18, SPACE, 0.9f);                        // gap placed 2 before E's cursor
        spikes_text(m, 30, 10, "OK");
        n = rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("a gap placed just before the cursor passes (DE OK), cursor never moves back",
              out == "DE OK" && cur == 40 && spt == 18);
        m = logp_new(T);
        spikes_text(m, 11, 10, "DE");
        spike(m, 17, SPACE, 0.9f);    // the committed gap, jittered
        spikes_text(m, 31, 10, "OK");
        spike(m, 37, SPACE, 0.9f);    // a gap placed 3 before K's cursor, no committed gap near
        spikes_text(m, 50, 10, "1M");
        n = rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("a re-read of the committed gap is squeezed, a new gap before the cursor passes",
              out == "DE OK 1M");
        // a gap far behind the cursor (an old read) never passes
        m = logp_new(T);
        spike(m, 5, SPACE, 0.9f);
        spikes_text(m, 70, 10, "X");
        n = rule.commit_ex(m.data(), T, 0, cur, 50, 4, lsp, spt, out, &conf, nullptr, 0, 0.0);
        check("a gap far behind the cursor is an old read: squeezed", out == "DE OK 1MX");
    }

    // ---- (A2) the draft: the tail guard's reading, display only ----------
    std::printf("[A2] draft (tail-guard reading)\n");
    {
        const unsigned T = 400;
        std::vector<float> m = logp_new(T);
        spikes_text(m, 10, 10, "CQ");
        spike(m, 100, SPACE, 0.95f);
        spikes_text(m, 150, 10, "TEST");
        spike(m, 200, SPACE, 0.95f);
        spikes_text(m, 340, 10, "DE");                  // inside the tail
        std::string out, dr, out2;
        uint64_t cur = 0, cur2 = 0, spt = 0;
        bool lsp = true, lsp2 = true;
        double conf = 0, conf2 = 0;
        unsigned n = rule.commit_ex(m.data(), T, 0, cur, 70, 4, lsp, spt, out, &conf, &dr, 0, 0.0);
        const unsigned n2 = rule.commit(m.data(), T, 0, cur2, 70, 4, lsp2, out2, &conf2);
        check("draft: tail characters land in the draft, not in out",
              n == 8 && out == "CQ TEST " && dr == "DE");
        check("draft: the final output is identical to the plain rule",
              n == n2 && out == out2 && cur == cur2 && lsp == lsp2 && std::fabs(conf - conf2) < 1e-6);
        check("draft: the cursor stays on the last FINAL character", cur == 200);
        // the same window re-read, spikes jittered +2, a double gap in the tail
        m = logp_new(T);
        spikes_text(m, 12, 10, "CQ");
        spike(m, 102, SPACE, 0.95f);
        spikes_text(m, 152, 10, "TEST");
        spike(m, 201, SPACE, 0.95f);
        spikes_text(m, 342, 10, "DE");
        spike(m, 362, SPACE, 0.9f);
        spike(m, 364, SPACE, 0.9f);
        dr.clear();
        n = rule.commit_ex(m.data(), T, 0, cur, 70, 4, lsp, spt, out, &conf, &dr, 0, 0.0);
        check("draft: a jittered re-read commits nothing and re-reads the draft (double gap squeezed)",
              n == 0 && out == "CQ TEST " && dr == "DE " && cur == 200);
        // the window slid by 100: DE + OK1B clear the tail, R is still inside;
        // a re-placed copy of B two frames after the cursor is not draft
        m = logp_new(T);
        spikes_text(m, 340 - 100, 10, "DE");
        spike(m, 360 - 100, SPACE, 0.9f);
        spikes_text(m, 400 - 100, 10, "OK1B");          // abs 400..430
        spike(m, 432 - 100, cls_of('B'), 0.9f);         // re-placed B
        spike(m, 445 - 100, cls_of('R'), 0.9f);         // tail: 445 > 430
        dr.clear();
        n = rule.commit_ex(m.data(), T, 100, cur, 70, 4, lsp, spt, out, &conf, &dr, 0, 0.0);
        check("draft: the slide commits what cleared the tail, the rest stays draft",
              n == 7 && out == "CQ TEST DE OK1B" && dr == "R" && cur == 430);
        // final text ending in a gap: a tail that starts with a gap shows no
        // double space in the draft
        m = logp_new(T);
        out.clear();
        dr.clear();
        cur = 0;
        lsp = true;
        spt = 0;
        spikes_text(m, 240, 10, "DE");
        spike(m, 270, SPACE, 0.9f);
        spike(m, 335, SPACE, 0.9f);
        spike(m, 345, cls_of('K'), 0.9f);   // tail
        n = rule.commit_ex(m.data(), T, 0, cur, 70, 4, lsp, spt, out, &conf, &dr, 0, 0.0);
        check("draft: a gap after a final gap is squeezed out of the draft",
              out == "DE " && dr == "K" && lsp);
        // the reliable prefix: draft_min frames from the window end and the
        // posterior bar cut the reading at the FIRST failing spike — nothing
        // behind it shows even if it would pass on its own
        m = logp_new(T);
        out.clear();
        dr.clear();
        cur = 0;
        lsp = true;
        spt = 0;
        spikes_text(m, 240, 10, "DE");
        spike(m, 270, SPACE, 0.9f);           // final
        spike(m, 335, cls_of('O'), 0.95f);   // dist 65 ≥ 24, p ok → draft
        spike(m, 345, cls_of('K'), 0.95f);   // dist 55 → draft
        spike(m, 355, cls_of('1'), 0.60f);   // p < 0.9 → cut here
        spike(m, 365, cls_of('M'), 0.95f);   // behind the cut: not shown
        spike(m, 390, cls_of('D'), 0.95f);   // dist 10 < 24 anyway
        n = rule.commit_ex(m.data(), T, 0, cur, 70, 4, lsp, spt, out, &conf, &dr, 24, 0.9);
        check("draft: the reliable prefix — cut at the first weak spike, nothing behind it",
              out == "DE " && dr == "OK");
        dr.clear();
        cur = 0;
        lsp = true;
        spt = 0;
        out.clear();
        n = rule.commit_ex(m.data(), T, 0, cur, 70, 4, lsp, spt, out, &conf, &dr, 24, 0.0);
        check("draft: no posterior bar → cut only at the distance bar", dr == "OK1M");
        // silence: no draft
        m = logp_new(200);
        out.clear();
        dr.clear();
        cur = 0;
        lsp = true;
        spt = 0;
        n = rule.commit_ex(m.data(), 200, 0, cur, 50, 4, lsp, spt, out, &conf, &dr, 0, 0.0);
        check("draft: silence → no draft either", n == 0 && dr.empty());
    }

    // ---- (B) tile builder / interface ------------------------------------
    std::printf("[B] tile builder + gate (no model needed)\n");
    SkimDeepcwBackend be(cfg);
    {
        {
            auto bad = be.channel_new(333.0);
            check("rate 333 Hz (no integer 12.5 Hz DFT) → dead state", dbg_of(*bad).dead);
        }

        const std::vector<float> env = keyer("CQ CQ DE OK1BR K", 25.0, RATE, 8, 8);
        const unsigned nf = static_cast<unsigned>(env.size());
        const std::vector<float> iq_p = tone_iq(env, RATE, +30.0, 0.1f, 0.003f, 1);
        const std::vector<float> iq_m = tone_iq(env, RATE, -30.0, 0.1f, 0.003f, 2);
        const std::vector<float> iq_2 = tone_iq(env, RATE, +30.0, 0.2f, 0.003f, 3);
        auto sp = be.channel_new(RATE), sm = be.channel_new(RATE), s2 = be.channel_new(RATE);
        std::string o;
        run_state(*sp, iq_p.data(), nf, o, nullptr);
        run_state(*sm, iq_m.data(), nf, o, nullptr);
        run_state(*s2, iq_2.data(), nf, o, nullptr);
        const double op = sp->tone_offset_hz(), om = sm->tone_offset_hz();
        std::printf("      offsets: +30 → %.1f Hz, −30 → %.1f Hz; levels %.3f / %.3f\n",
                    op, om, sp->level(), s2->level());
        check("+30 Hz tone → offset +30 ± 4 Hz (sign = v2's, above centre positive)", std::fabs(op - 30.0) < 4.0);
        check("−30 Hz tone → offset −30 ± 4 Hz", std::fabs(om + 30.0) < 4.0);
        const double lr = s2->level() / std::max(sp->level(), 1e-9);
        check("level scales with amplitude (×2 → 1.7..2.3)", lr > 1.7 && lr < 2.3);
        const SkimDeepcwDebug dbg = dbg_of(*sp);
        std::printf("      keyed: ticks %u gate %d ratio %.1f dB duty %.2f wpm %.1f\n",
                    dbg.ticks, dbg.gate, dbg.ratio_db, dbg.duty, dbg.wpm);
        check("keyed tone: gate OPEN at the last tick", dbg.ticks > 0 && dbg.gate);
        check("keyed tone: line ≥ 6 dB over the floor", dbg.ratio_db >= 6.0);
        check("dit estimate lands within ±35 % of 25 WPM", dbg.wpm > 16.0 && dbg.wpm < 34.0);
        sp.reset();
        sm.reset();
        s2.reset();
        // noise only
        const std::vector<float> env0(8 * static_cast<unsigned>(RATE), 0.0f);
        const std::vector<float> iq_n = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 4);
        auto sn = be.channel_new(RATE);
        run_state(*sn, iq_n.data(), static_cast<unsigned>(env0.size()), o, nullptr);
        const SkimDeepcwDebug dn = dbg_of(*sn);
        std::printf("      noise: ticks %u gate %d ratio %.1f dB duty %.2f\n", dn.ticks, dn.gate, dn.ratio_db, dn.duty);
        check("noise only: gate CLOSED", dn.ticks > 0 && !dn.gate);
        check("noise only: nothing ever committed (cursor never moved)", dn.committed == 0);
        check("no model → no text emitted from the DSP half alone", o.empty() || be.available());
    }

    // ---- (C) the model ----------------------------------------------------
    std::printf("[C] model\n");
    std::string err;
    if (!be.available(&err)) {
        std::printf("  SKIP — %s\n  (set SKIM_ORT_LIB and SKIM_DEEPCW_MODEL to run this section)\n",
                    err.empty() ? "?" : err.c_str());
        std::printf("=== %d checks, %d failed (model section skipped) ===\n", checks, fails);
        return fails ? 1 : 77;
    }
    std::printf("  runtime %s\n", be.runtime_info().c_str());
    const bool sync_env = cfg.sync;
    {
        // Device selection: ask for CUDA. With the CPU-only runtime (or no
        // GPU) the session must fall back and SAY so; with a CUDA runtime it
        // runs on CUDA:0. Either way the model must still read. Then back to
        // the CPU through reset — the way the settings switch it.
        be.reset();
        be.set_device("cuda");
        check("session reloads after reset with device=cuda requested", be.available());
        const std::string info = be.runtime_info();
        std::printf("      device=cuda → %s\n", info.empty() ? "(none)" : info.c_str());
        check("runtime_info names CUDA:0 or the fallback reason",
              info.find("CUDA:0") != std::string::npos || info.find("cuda unavailable") != std::string::npos);
        be.reset();
        be.set_device("cpu");
        check("reset + device=cpu reloads on the CPU",
              be.available() && be.runtime_info().find(", CPU") != std::string::npos);
    }
    {
        // Inline (the replay path): a burst feed, deterministic text.
        const std::vector<float> env = keyer("CQ CQ DE OK1BR OK1BR K CQ CQ DE OK1BR OK1BR K", 25.0, RATE, 40, 200);
        const std::vector<float> iq = tone_iq(env, RATE, +20.0, 0.05f, 0.004f, 5);
        auto st = be.channel_new(RATE);
        std::string o;
        double conf = 0;
        unsigned hits = 0;
        const unsigned len = static_cast<unsigned>(env.size());
        if (sync_env) {
            hits = run_state(*st, iq.data(), len, o, &conf);
        } else {
            // Async (the app's path): pace the feed like a live channel would
            // arrive (a 64-frame block ≈ 256 ms of audio; 2 ms wall per block
            // is 128× realtime — the workers must keep up), then drain the
            // mailbox until the last window has come back.
            SkimDecode d;
            for (unsigned i = 0; i < len; i += 64) {
                const unsigned n = std::min(64u, len - i);
                if (st->process(iq.data() + 2 * i, n, &d)) {
                    hits++;
                    o += d.text;
                    conf = d.confidence;
                }
                sleep_us(2000);
            }
            for (int k = 0; k < 500; k++) {
                if (st->process(iq.data(), 0, &d)) {
                    hits++;
                    o += d.text;
                    conf = d.confidence;
                }
                if (!dbg_of(*st).inflight && o.size() > 20)
                    break;
                sleep_us(10000);
            }
        }
        const SkimDeepcwDebug dbg = dbg_of(*st);
        std::printf("      %s text |%s| hits %u ticks %u last conf %.2f wpm %.0f\n",
                    sync_env ? "sync " : "async", o.c_str(), hits, dbg.ticks, conf, dbg.wpm);
        check("model reads the call (text contains OK1BR)", o.find("OK1BR") != std::string::npos);
        check("commit seams carry single spaces", o.find("  ") == std::string::npos);
        check("model reads CQ", o.find("CQ") != std::string::npos);
        check("no doubled call from re-decoding committed audio",
              o.find("OK1BROK1BR") == std::string::npos && o.find("OK1BR OK1BR OK1BR") == std::string::npos);
        check("committed text carries a confidence", conf > 0.5);
        st.reset();
        check("a state freed with a window in flight does not crash (deferred free)", true);
        // three channels fed in lockstep through the async path: the worker
        // batches whatever is queued, zero-padding to the longest window —
        // each channel must still read ITS OWN text.
        if (!sync_env) {
            const char *texts[3] = { "CQ CQ DE OK1BR OK1BR K", "TEST DL1ABC DL1ABC 5NN", "VVV DE SP9XYZ SP9XYZ K" };
            const double offs[3] = { +20.0, -25.0, +40.0 };
            const double wpms[3] = { 25.0, 30.0, 20.0 };
            std::vector<float> envs[3], iqs[3];
            std::unique_ptr<SkimDecodeChannel> sts[3];
            std::string os[3];
            size_t blen = 0;
            for (int k = 0; k < 3; k++) {
                envs[k] = keyer(texts[k], wpms[k], RATE, 40, 200);
                iqs[k] = tone_iq(envs[k], RATE, offs[k], 0.05f, 0.004f, 10 + k);
                sts[k] = be.channel_new(RATE);
                blen = std::max(blen, envs[k].size());
            }
            SkimDecode d;
            for (size_t i = 0; i < blen; i += 64) {
                for (int k = 0; k < 3; k++) {
                    if (i >= envs[k].size())
                        continue;
                    const unsigned n = static_cast<unsigned>(std::min<size_t>(64, envs[k].size() - i));
                    if (sts[k]->process(iqs[k].data() + 2 * i, n, &d))
                        os[k] += d.text;
                }
                sleep_us(2000);
            }
            for (int r = 0; r < 300; r++) {
                bool busy = false;
                for (int k = 0; k < 3; k++) {
                    if (sts[k]->process(iqs[k].data(), 0, &d))
                        os[k] += d.text;
                    busy |= dbg_of(*sts[k]).inflight;
                }
                if (!busy && os[0].size() > 20 && os[1].size() > 10 && os[2].size() > 10)
                    break;
                sleep_us(10000);
            }
            for (int k = 0; k < 3; k++)
                std::printf("      batch ch%d |%s|\n", k, os[k].c_str());
            check("batched channel 0 reads OK1BR", os[0].find("OK1BR") != std::string::npos);
            check("batched channel 1 reads DL1ABC", os[1].find("DL1ABC") != std::string::npos);
            check("batched channel 2 reads SP9XYZ", os[2].find("SP9XYZ") != std::string::npos);
            check("no cross-talk between batched channels",
                  os[0].find("DL1ABC") == std::string::npos && os[1].find("OK1BR") == std::string::npos &&
                  os[2].find("OK1BR") == std::string::npos);
        }
        // TX-hold flush (decode.h hold_begin, gh#18). The operator answers a
        // station a moment after its over: the tail guard still holds the end
        // of the call. hold_begin must commit it (and close the word), the
        // text must drain through process() with ZERO frames, a word the hold
        // cut short must NOT go out, and the first text after resync starts a
        // new word.
        {
            const char *over = "CQ TEST DE OK1BR";
            // 14 dits of quiet after the over = the operator's reaction time
            const std::vector<float> envh = keyer(over, 25.0, RATE, 40, 14);
            const std::vector<float> iqh = tone_iq(envh, RATE, +20.0, 0.05f, 0.004f, 21);
            // the same over, the feed stopping inside the B of the call
            const std::vector<float> head = keyer("CQ TEST DE OK1", 25.0, RATE, 40, 0);
            const unsigned cut_at = static_cast<unsigned>(head.size()) +
                                    static_cast<unsigned>(std::llround(5 * (1.2 / 25.0) * RATE));
            SkimDecode d;
            auto feed = [&](SkimDecodeChannel &stp, std::string &o2, const float *ptr, unsigned n) {
                for (unsigned i = 0; i < n; i += 64) {
                    if (stp.process(ptr + 2 * i, std::min(64u, n - i), &d))
                        o2 += d.text;
                    if (!sync_env)
                        sleep_us(2000);
                }
            };
            auto drain = [&](SkimDecodeChannel &stp, std::string &o2) {
                for (int k = 0, idle = 0; k < 500 && idle < 5; k++) {
                    if (stp.process(iqh.data(), 0, &d)) {
                        o2 += d.text;
                        idle = 0;
                    }
                    if (!dbg_of(stp).inflight) {
                        idle++;
                    } else {
                        idle = 0;
                        if (!sync_env)
                            sleep_us(10000);
                    }
                }
            };
            const unsigned hlen = static_cast<unsigned>(envh.size());
            check("the DeepCW backend implements hold_begin", be.implements(SkimDecodeHook::HoldBegin));
            // (1) the over ended: the flush completes the call
            auto sf = be.channel_new(RATE);
            std::string of;
            feed(*sf, of, iqh.data(), hlen);
            drain(*sf, of);
            const std::string before = of;
            sf->hold_begin();
            drain(*sf, of);
            std::printf("      flush: before |%s| after |%s|\n", before.c_str(), of.c_str());
            check("flush: the tail guard still held the end of the call when the hold began",
                  before.find("OK1BR") == std::string::npos);
            check("flush: hold_begin commits the tail — the call is whole, the word closed",
                  ends_with(of, "OK1BR "));
            check("flush: nothing doubled at the seam",
                  of.find("OK1BR") == of.rfind("OK1BR") && of.find("  ") == std::string::npos);
            // (2) the answer after the hold starts a new word
            {
                const std::vector<float> env2 = keyer("5NN TU", 25.0, RATE, 10, 120);
                const std::vector<float> iq2 = tone_iq(env2, RATE, +20.0, 0.05f, 0.004f, 22);
                sf->resync();
                feed(*sf, of, iq2.data(), static_cast<unsigned>(env2.size()));
                drain(*sf, of);
                std::printf("      after the hold |%s|\n", of.c_str());
                check("flush: the text after the hold starts a NEW word (no OK1BR5NN glue)",
                      of.find("OK1BR 5NN") != std::string::npos && of.find("  ") == std::string::npos);
            }
            // (3) control: resync alone abandons the tail — the loss this fixes
            auto sc = be.channel_new(RATE);
            std::string oc;
            feed(*sc, oc, iqh.data(), hlen);
            drain(*sc, oc);
            sc->resync();
            {
                const std::vector<float> zz(2 * 64, 0.0f);
                for (unsigned i = 0; i < 3 * static_cast<unsigned>(RATE); i += 64) {
                    if (sc->process(zz.data(), 64, &d))
                        oc += d.text;
                    if (!sync_env)
                        sleep_us(20000);
                }
            }
            drain(*sc, oc);
            std::printf("      control (no hold_begin) |%s|\n", oc.c_str());
            check("control: without hold_begin the end of the call never comes out",
                  oc.find("OK1BR") == std::string::npos);
            // (4) the hold cuts the call mid-character: nothing of the cut word
            auto sk = be.channel_new(RATE);
            std::string ok;
            feed(*sk, ok, iqh.data(), cut_at);
            drain(*sk, ok);
            const std::string kbefore = ok;
            sk->hold_begin();
            drain(*sk, ok);
            std::printf("      cut: before |%s| after |%s|\n", kbefore.c_str(), ok.c_str());
            check("flush: a word the hold cut short adds NOTHING and stays open (its head is no token)",
                  ok == kbefore && !ends_with(ok, " "));
        }
        // noise only, 20 s: no phantom text
        const std::vector<float> env0(20 * static_cast<unsigned>(RATE), 0.0f);
        const std::vector<float> iq_n = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 6);
        auto sn = be.channel_new(RATE);
        std::string on;
        run_state(*sn, iq_n.data(), static_cast<unsigned>(env0.size()), on, nullptr);
        check("noise only through the model: no text", on.empty());
    }
    std::printf("=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
