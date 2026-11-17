/*
 * skimmer-cw-test — offline gate for the CW decode backend (M3).
 *
 * Synthesizes keyed CW at complex baseband (250 Hz, like a channelizer
 * output): text → ITU element timing → envelope with 5 ms raised-cosine
 * edges → tone at an in-channel offset + AWGN, then checks the decoder:
 *   - clean copy at 15 / 20 / 25 / 35 WPM (exact text),
 *   - WPM, SNR, offset and confidence estimates are sane,
 *   - 12 dB SNR, ±15 % timing jitter (ragged fist) and 10 dB QSB still copy
 *     (small Levenshtein tolerance),
 *   - a mid-stream 18 → 28 WPM speed change re-locks,
 *   - SQUELCH: 20 s of pure noise emits NOT ONE character (RBN-grade rule),
 *   - integration: keyed carrier at 12.018 kHz through the real channelizer,
 *     the right channel decodes the text, a neighbour channel stays silent.
 *
 * Off-air A/B against fldigi / CW Skimmer needs recorded IQ — that is the
 * second half of the M3 gate, run when a capture from the radio exists.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/skim_common.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/callsign.h"
#include "engine/channelizer.h"
#include "engine/decode_cw.h"
#include "engine/cw_rundump.h"
#include "engine/oracle.h"

#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <random>

// SDRIAK: the gate on the standard library (upstream: GLib). GLib's GRand
// is MT19937 with init_genrand seeding — std::mt19937 is that generator,
// seeded the same way — and rand_double() is rand_double()'s own draw on
// it, so every synthetic signal is upstream's, sample for sample.
typedef std::vector<float> Env;
typedef std::mt19937 Rng;

static double rand_double(Rng *r) {
    for (;;) {                               // two 32-bit draws, 2^-32 each
        double v = (*r)() * 2.3283064365386962890625e-10;
        v = (v + (*r)()) * 2.3283064365386962890625e-10;
        if (v < 1.0)
            return v;
    }
}

static double rand_double_range(Rng *r, double begin, double end) {
    return rand_double(r) * (end - begin) + begin;
}

static char *str_dup(const std::string &s) {
    char *d = static_cast<char *>(std::malloc(s.size() + 1));
    memcpy(d, s.c_str(), s.size() + 1);
    return d;
}

static char *str_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return str_dup(buf);
}

static std::string tmp_file(const char *name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

static void write_file(const std::string &path, const char *text) {
    std::ofstream(path, std::ios::binary) << text;
}

static void set_env(const char *name, const char *value, bool overwrite) {
    if (!overwrite && std::getenv(name))
        return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

constexpr auto RATE = 250.0;

static int fails, checks;
static void check(const char *what, int ok) {
    checks++;
    if (!ok) { fails++; }
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// --- CW synthesis ------------------------------------------------------------

static const struct { char c; const char *m; } MORSE[] = {
    {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
    {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
    {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
    {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
    {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
    {'Z',"--.."},
    {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
    {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
};

static const char *morse_of(char c) {
    for (unsigned i = 0; i < std::size(MORSE); i++) {
        if (MORSE[i].c == c) { return MORSE[i].m; }
    }
    return nullptr;
}

// Append `samps` samples of keyed envelope value `on` (with ramps handled by
// the caller via lerp between elements — we keep it simple: rectangular runs,
// the 3-tap MA in the decoder plays the role of the shaping).
static void key_run(Env *env, double samps, float on, double jitter,
                    Rng *rng) {
    if (jitter > 0) { samps *= 1.0 + rand_double_range(rng, -jitter, jitter); }
    unsigned n = static_cast<unsigned>(samps + 0.5);
    for (unsigned i = 0; i < n; i++) { env->push_back(on); }
}

// Append `text` keyed verbatim — no warmup, no trailing flush (the second
// over of a pause pair, or a cold start whose head is under test).
static void gen_env_append(Env *env, const char *text, double wpm,
                           double rate, double jitter, Rng *rng,
                           double csp_dits, double wsp_dits) {
    double dit = 1.2 / wpm * rate;
    float  ON = 1.0f, OFF = 0.0f;
    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            key_run(env, wsp_dits * dit, OFF, jitter, rng);
            continue;
        }
        const char *m = morse_of(*p);
        if (!m) { continue; }
        for (const char *e = m; *e; e++) {
            key_run(env, *e == '-' ? 3 * dit : dit, ON, jitter, rng);
            if (e[1]) { key_run(env, dit, OFF, jitter, rng); }
        }
        key_run(env, csp_dits * dit, OFF, jitter, rng);
    }
}

// text → keyed envelope at `rate`, dit = 1.2/wpm s. Leading "VVV " warms up
// the decoder's dit bootstrap; trailing 1.5 s of silence flushes the tail.
// csp_dits/wsp_dits set the operator's char/word spacing (ITU: 3/7 — real
// fists stretch and squeeze them, which is what the fist model is for).
static Env *gen_env_fist(const char *payload, double wpm, double rate,
                            double jitter, Rng *rng, double csp_dits,
                            double wsp_dits) {
    Env *env = new Env();
    char   *text = str_printf("VVV %s", payload);
    double  dit = 1.2 / wpm * rate;

    key_run(env, 3 * dit, 0.0f, 0, rng);
    gen_env_append(env, text, wpm, rate, jitter, rng, csp_dits, wsp_dits);
    key_run(env, 1.5 * rate, 0.0f, 0, rng);
    std::free(text);
    return env;
}

static Env *gen_env(const char *payload, double wpm, double rate,
                       double jitter, Rng *rng) {
    return gen_env_fist(payload, wpm, rate, jitter, rng, 3.0, 7.0);
}

// 5 ms raised-cosine keying edges: convolve the rectangular envelope with a
// normalised Hann kernel. Real transmitters shape their keying — without it
// the hard edges are key-clicks that leak into neighbouring channels.
static void shape_env(Env *env, double rate) {
    unsigned L = static_cast<unsigned>(rate * 0.005);
    if (L < 3) { return; }
    std::vector<float> wv(L);
    float *w = wv.data();
    double sum = 0.0;
    for (unsigned i = 0; i < L; i++) {
        w[i] = 0.5f - 0.5f * static_cast<float>(cos(2.0 * SKIM_PI * i / (L - 1)));
        sum += w[i];
    }
    for (unsigned i = 0; i < L; i++) { w[i] = static_cast<float>(w[i] / sum); }
    float *src = env->data();
    std::vector<float> dstv(env->size());
    float *dst = dstv.data();
    for (unsigned n = 0; n < env->size(); n++) {
        float acc = 0.0f;
        unsigned kmax = std::min(L, n + 1);
        for (unsigned k = 0; k < kmax; k++) { acc += w[k] * src[n - k]; }
        dst[n] = acc;
    }
    memcpy(src, dst, env->size() * sizeof(float));
}

// Gaussian noise via Box-Muller on a seeded GRand.
static double gauss(Rng *rng) {
    double u1 = 1.0 - rand_double(rng), u2 = rand_double(rng);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * SKIM_PI * u2);
}

// The backend under test — the whole suite runs once per backend.
static const SkimDecodeBackend *g_cw;

// Run an envelope through the decoder as IQ: tone at `foff` Hz, amplitude
// 0.5·env, complex AWGN for `snr_db` (per-component σ), optional QSB.
// Returns the concatenated decoded text; fills last decode info.
static char *run_decoder(Env *env, double foff, double snr_db,
                         double qsb_hz, double qsb_depth_db, Rng *rng,
                         SkimDecode *last) {
    const SkimDecodeBackend *cw = g_cw;
    std::unique_ptr<SkimDecodeChannel> st = cw->channel_new(RATE);
    std::string txt;
    const double A = 0.5;
    const double sigma = A / (sqrt(2.0) * pow(10.0, snr_db / 20.0));
    double ph = 0.0, dph = 2.0 * SKIM_PI * foff / RATE;

    float iq[2 * 64];
    SkimDecode d;
    memset(&d, 0, sizeof(d));
    for (unsigned at = 0; at < env->size(); at += 64) {
        unsigned n = std::min(64u, static_cast<unsigned>(env->size()) - at);
        for (unsigned i = 0; i < n; i++) {
            double a = A * (*env)[at + i];
            if (qsb_hz > 0) {
                double t = static_cast<double>(at + i) / RATE;
                double depth = pow(10.0, -qsb_depth_db / 20.0);
                a *= depth + (1.0 - depth) * (0.5 + 0.5 * sin(2.0 * SKIM_PI * qsb_hz * t));
            }
            iq[2 * i]     = static_cast<float>(a * cos(ph) + sigma * gauss(rng));
            iq[2 * i + 1] = static_cast<float>(a * sin(ph) + sigma * gauss(rng));
            ph += dph;
        }
        if (st->process(iq, n, &d)) {
            txt += d.text;
            if (last) { *last = d; }
        }
    }
    return str_dup(txt);
}

// SDRIAK: v1 is not part of SDRIAK. Where upstream ran it ahead of v2 on the
// same generator, this takes the draws run_decoder() would have taken (two
// gauss() per I/Q sample, two draws each), so v2 and v3 still see exactly
// upstream's noise.
static void rng_burn(const Env *env, Rng *rng) {
    for (unsigned i = 0; i < 4 * env->size(); i++) { static_cast<void>(rand_double(rng)); }
}

// --- fuzzy matching -----------------------------------------------------------

// Full Levenshtein of `pat` against the LAST over in `txt` (the text after
// the final "·" separator, trimmed). Unlike fuzzy_dist the prefix is NOT
// free — a stale clock that explodes the over's first word into a dozen
// phantom chars must show up in the distance.
static int last_over_dist(const char *txt, const char *pat) {
    const char *seg = txt;
    for (const char *p = txt; *p; p++) {
        if (static_cast<unsigned char>(p[0]) == 0xC2 && static_cast<unsigned char>(p[1]) == 0xB7) {
            const char *q = p + 2;
            while (*q == ' ') { q++; }
            if (*q) { seg = q; }                 // last separator with content
        }
    }
    while (*seg == ' ') { seg++; }
    char *s = str_dup(seg);
    char *cut = strstr(s, "\xC2\xB7");     // the over ends at the next mark
    if (cut) { *cut = '\0'; }
    for (size_t k_ = strlen(s); k_ && isspace(static_cast<unsigned char>(s[k_ - 1]));) { s[--k_] = '\0'; }
    size_t n = strlen(s), m = strlen(pat);
    std::vector<int> dpv(n + 1), nxv(n + 1);
    int *dp = dpv.data(), *nx = nxv.data();
    for (size_t j = 0; j <= n; j++) { dp[j] = static_cast<int>(j); }
    for (size_t i = 1; i <= m; i++) {
        nx[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; j++) {
            int sub = dp[j - 1] + (pat[i - 1] != s[j - 1]);
            nx[j] = std::min(sub, std::min(dp[j] + 1, nx[j - 1] + 1));
        }
        memcpy(dp, nx, (n + 1) * sizeof(int));
    }
    int d = dp[n];
    std::free(s);
    return d;
}

// Levenshtein distance of `pat` against the best-matching substring of `txt`
// (free prefix/suffix — the classic fuzzy substring DP).
static int fuzzy_dist(const char *txt, const char *pat) {
    size_t n = strlen(txt), m = strlen(pat);
    std::vector<int> dpv(n + 1), nxv(n + 1);
    int *dp = dpv.data(), *nx = nxv.data();
    for (size_t j = 0; j <= n; j++) { dp[j] = 0; }        // free prefix
    for (size_t i = 1; i <= m; i++) {
        nx[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; j++) {
            int sub = dp[j - 1] + (pat[i - 1] != txt[j - 1]);
            nx[j] = std::min(sub, std::min(dp[j] + 1, nx[j - 1] + 1));
        }
        memcpy(dp, nx, (n + 1) * sizeof(int));
    }
    int best = dp[0];
    for (size_t j = 1; j <= n; j++) { best = std::min(best, dp[j]); }
    return best;
}

static void show(const char *label, const char *got) {
    printf("       %-14s \"%s\"\n", label, got);
}

// The whole classic suite, parametrized by backend.
static void run_suite(const SkimDecodeBackend *cw) {
    g_cw = cw;
    printf("--- backend: %s ---\n", cw->name());
    Rng *rng = new Rng(20260715);         // deterministic
    SkimDecode last;

    // -- clean copy across speeds --------------------------------------------
    static const double SPEEDS[] = { 15, 20, 25, 35 };
    const char *PAYLOAD = "CQ TEST DE OK1BR OK1BR K";
    for (unsigned s = 0; s < std::size(SPEEDS); s++) {
        Env *env = gen_env(PAYLOAD, SPEEDS[s], RATE, 0.0, rng);
        char *got = run_decoder(env, 18.0, 30.0, 0, 0, rng, &last);
        char what[64];
        std::snprintf(what, sizeof(what), "clean copy at %.0f WPM (exact)", SPEEDS[s]);
        int d = fuzzy_dist(got, PAYLOAD);
        if (d) { show("decoded:", got); }
        check(what, d == 0);
        if (SPEEDS[s] == 20) {
            check("WPM estimate ≈ 20 (±2)", fabs(last.speed - 20.0) < 2.0);
            if (last.snr_db <= 20.0) {
                printf("       (snr_db = %.1f)\n", last.snr_db);
            }
            check("SNR estimate > 20 dB on a clean trace", last.snr_db > 20.0);
            check("tone offset ≈ +18 Hz (±5)", fabs(last.freq_offset_hz - 18.0) < 5.0);
            check("confidence high on clean keying", last.confidence > 0.7);
        }
        delete env;
        std::free(got);
    }

    // -- 12 dB SNR -------------------------------------------------------------
    Env *env = gen_env(PAYLOAD, 20, RATE, 0.0, rng);
    char *got = run_decoder(env, 18.0, 12.0, 0, 0, rng, &last);
    show("12 dB SNR:", got);
    check("copies at 12 dB SNR (≤2 errors)", fuzzy_dist(got, PAYLOAD) <= 2);
    std::free(got);

    // -- ragged fist: ±15 % element jitter --------------------------------------
    Env *envj = gen_env(PAYLOAD, 20, RATE, 0.15, rng);
    got = run_decoder(envj, 18.0, 30.0, 0, 0, rng, &last);
    show("15 % jitter:", got);
    check("copies a ±15 % ragged fist (≤2 errors)", fuzzy_dist(got, PAYLOAD) <= 2);
    check("confidence drops on the ragged fist", last.confidence < 0.9);
    delete envj;
    std::free(got);

    // -- QSB: 10 dB at 0.4 Hz ----------------------------------------------------
    got = run_decoder(env, 18.0, 30.0, 0.4, 10.0, rng, &last);
    show("10 dB QSB:", got);
    check("copies through 10 dB / 0.4 Hz QSB (≤2 errors)",
          fuzzy_dist(got, PAYLOAD) <= 2);
    delete env;
    std::free(got);

    // -- speed change mid-stream: 18 → 28 WPM ------------------------------------
    {
        Env *a = gen_env("VVV VVV DE OK1BR", 18, RATE, 0, rng);
        Env *b = gen_env("VVV VVV DE OK1BR", 28, RATE, 0, rng);
        a->insert(a->end(), b->begin(), b->end());
        got = run_decoder(a, 18.0, 30.0, 0, 0, rng, &last);
        show("18→28 WPM:", got);
        printf("       (final speed %.1f WPM)\n", last.speed);
        // Both payload copies must be present; the second proves the re-lock.
        check("re-locks after a mid-stream 18 → 28 WPM change",
              fuzzy_dist(got, "DE OK1BR") <= 1 &&
              fabs(last.speed - 28.0) < 3.0);
        delete a;
        delete b;
        std::free(got);
    }

    // -- QSO turnaround: the OTHER op comes back on the same channel -------------
    // A short pause (< the 8 s fist-forget) keeps the first op's dit, so the
    // second over starts on a stale clock. A speed-UP is the nastier
    // direction: the new op's dahs sit under the 2-dit class boundary and
    // read as dits, so the plain EMA is pulled the WRONG way (live 40 m,
    // 2026-07-18 — WPM adaptation must re-lock fast, no model involved).
    // The bar: the second over must copy from its second word on.
    {
        static const struct { double wpm_a, wpm_b; } TURN[] = {
            { 18, 30 }, { 26, 14 },
        };
        Rng *trng = new Rng(20260718);  // own stream — the shared
                                        // rng feeds later tests
        for (unsigned c = 0; c < std::size(TURN); c++) {
            Env *a = gen_env("CQ TEST DE OK1BR OK1BR K", TURN[c].wpm_a,
                                RATE, 0, trng);
            Env *b = gen_env("R R UR 5NN 5NN TU OK1BR K", TURN[c].wpm_b,
                                RATE, 0, trng);
            float z = 0.0f;
            for (unsigned i = 0; i < static_cast<unsigned>(3.0 * RATE); i++) {
                a->push_back(z);
            }
            a->insert(a->end(), b->begin(), b->end());
            got = run_decoder(a, 18.0, 30.0, 0, 0, trng, &last);
            char what[80];
            std::snprintf(what, sizeof(what),
                       "QSO turnaround %.0f → %.0f WPM re-locks (whole over ≤3)",
                       TURN[c].wpm_a, TURN[c].wpm_b);
            show("turnaround:", got);
            // The WHOLE second over against the WHOLE keyed audio (gen_env
            // prepends a "VVV " warmup), garbage insertions included — a stale
            // clock that explodes the opening into a dozen phantom chars must
            // fail here, a clean copy of the warmup must not.
            check(what, last_over_dist(got, "VVV R R UR 5NN 5NN TU OK1BR K") <= 3);
            delete a;
            delete b;
            std::free(got);
        }
        delete trng;
    }

    // -- SQUELCH: pure noise must emit nothing -----------------------------------
    {
        Env *quiet = new Env();
        float z = 0.0f;
        for (unsigned i = 0; i < static_cast<unsigned>(20 * RATE); i++) {
            quiet->push_back(z);
        }
        got = run_decoder(quiet, 0.0, 30.0, 0, 0, rng, nullptr);
        check("20 s of pure noise decodes to NOTHING (RBN rule)", got[0] == '\0');
        if (got[0]) { show("noise gave:", got); }
        delete quiet;
        std::free(got);
    }

    // -- integration: through the real channelizer -------------------------------
    {
        std::unique_ptr<SkimChannelizer> bank = SkimChannelizer::create(48000.0, 125.0);
        std::unique_ptr<SkimDecodeChannel> st_hit  = cw->channel_new(bank->out_rate());
        std::unique_ptr<SkimDecodeChannel> st_miss = cw->channel_new(bank->out_rate());
        std::string hit, miss;

        // The quiet channel sits 6.7 kHz off the carrier: shaped keying still
        // splatters measurably a few hundred Hz out (a REAL near-channel signal
        // the decoder rightly copies — adjacent-channel ghosts of strong
        // stations are the M5 station tracker's dedup job, not the decoder's),
        // but far channels must hold only noise and stay mute.
        Env *e48 = gen_env("DE OK1BR", 20, 48000.0, 0, rng);
        shape_env(e48, 48000.0);
        double ph = 0.0, dph = 2.0 * SKIM_PI * 12018.0 / 48000.0;
        float blk[2 * 1024], chan[2 * 512];
        SkimDecode d;
        for (unsigned at = 0; at < e48->size(); at += 1024) {
            unsigned n = std::min(1024u, static_cast<unsigned>(e48->size()) - at);
            for (unsigned i = 0; i < n; i++) {
                double a = 0.5 * (*e48)[at + i];
                blk[2 * i]     = static_cast<float>(a * cos(ph) + 0.01 * gauss(rng));
                blk[2 * i + 1] = static_cast<float>(a * sin(ph) + 0.01 * gauss(rng));
                ph += dph;
            }
            bank->push(blk, n);
            unsigned g;
            while ((g = bank->read(96, chan, 512)) > 0) {
                if (st_hit->process(chan, g, &d)) { hit += d.text; }
            }
            while ((g = bank->read(150, chan, 512)) > 0) {
                if (st_miss->process(chan, g, &d)) { miss += d.text; }
            }
        }
        show("via bank ch96:", hit.c_str());
        check("channelizer → decoder end-to-end copies the text",
              fuzzy_dist(hit.c_str(), "DE OK1BR") <= 1);
        if (miss.size()) { show("ch150 gave:", miss.c_str()); }
        check("a noise-only channel through the bank stays silent",
              miss.size() == 0);
        delete e48;
    }

    delete rng;
}

// The soft-decision cases (QSB, flutter, the fist model, a speed drop, the
// squelch attack, the cold start, the word re-read): what v2 exists for.
// v3 inherits v2's lattice and must pass every one of them too — the
// cases run for both, v1 beside them for information.
static void soft_cases(const SkimDecodeBackend *soft, const char *tag,
                       const std::shared_ptr<SkimCallsignDictSource> &scp) {
    printf("\n=== soft-decision cases: %s ===\n", tag);

    // -- QSB mutation cases: what v2 exists for ------------------------------------
    // Deep slow QSB eats the weak elements of a character — the live-caught
    // 9A170NT case ("7" --... losing its trailing dits to a fade → "G" --.).
    // v1's binary threshold drops the faded dits outright; v2's soft Viterbi
    // must carry them. v1 runs uncounted here, printed for comparison.
    {
        Rng *rng = new Rng(20260715);
        const char *PAYLOAD = "CQ TEST 9A170NT 9A170NT K";
        SkimDecode last;

        Env *env = gen_env(PAYLOAD, 25, RATE, 0.0, rng);
        rng_burn(env, rng);                        // SDRIAK: v1 dropped
        // SDRIAK: QSB_TRACE runs this case on a traced twin of the backend
        // (upstream sets SKIM_CW2_TRACE around it; the tunables are a config).
        std::unique_ptr<SkimDecodeBackend> traced;
        if (std::getenv("QSB_TRACE")) {
            const bool v3 = strcmp(soft->name(), "cw3") == 0;
            CwDecoderConfig c = cw_config_from_env(v3 ? cw_preset_v3() : cw_preset_v2(), v3 ? 3 : 2);
            c.diag.trace = true;
            traced = v3 ? skim_decode_cw_v3_new(c, scp) : skim_decode_cw_v2_new(c, scp);
        }
        g_cw = traced ? traced.get() : soft;
        char *v2 = run_decoder(env, 18.0, 30.0, 0.31, 16.0, rng, &last);
        printf("--- QSB mutation: 16 dB @ 0.31 Hz, 25 WPM ---\n");
        show("v2:", v2);
        printf("       v2 dist %d\n", fuzzy_dist(v2, PAYLOAD));
        check("v2 copies through element-eating QSB (≤2 errors)",
              fuzzy_dist(v2, PAYLOAD) <= 2);
        std::free(v2);
        delete env;

        // Sub-dit flutter dropouts — the in-dash notch that splits a dash in
        // two (v1's admitted limit). 35 ms at −18 dB every 700 ms: shorter
        // than any real inter-element space, so a decoder CAN tell it apart.
        // (A dropout a full dit long is amplitude-indistinguishable from real
        // keying — no envelope decoder recovers that.)
        env = gen_env(PAYLOAD, 22, RATE, 0.0, rng);
        for (unsigned at = 0; at < env->size(); at++) {
            double tsec = static_cast<double>(at) / RATE;
            double ph = fmod(tsec, 0.7);
            if (ph < 0.035) {
                float *v = &(*env)[at];
                *v *= 0.126f;                          // −18 dB notch
            }
        }
        rng_burn(env, rng);                        // SDRIAK: v1 dropped
        g_cw = soft;
        v2 = run_decoder(env, 18.0, 30.0, 0, 0, rng, &last);
        printf("--- flutter notches: 35 ms / -18 dB every 0.7 s, 22 WPM ---\n");
        show("v2:", v2);
        printf("       v2 dist %d\n", fuzzy_dist(v2, PAYLOAD));
        check("v2 copies through in-dash dropout notches (≤2 errors)",
              fuzzy_dist(v2, PAYLOAD) <= 2);
        std::free(v2);
        delete env;
        delete rng;
    }

    // -- fist model: spacing is personal ---------------------------------------------
    // EA1EYL (live 2026-07-16) stretches his char gaps to ~5 dits when
    // calling CQ — the fixed 3/7-dit priors filed the C–Q gap as a WORD
    // space, "C Q" matched no marker and the spot never fired. v2 must learn
    // the operator's own two space centres and put "CQ" back together; the
    // counter-case guards the other direction (a tight runner's 5-dit word
    // gaps must NOT collapse into char gaps once the model adapts). v1 keeps
    // its fixed 5.5-dit boundary and is printed for info only.
    {
        Rng *rng = new Rng(20260716);
        SkimDecode last;

        const char *CALL = "CQ CQ DE EA1EYL EA1EYL K";
        char *loop = str_printf("%s %s %s", CALL, CALL, CALL);
        Env *env = gen_env_fist(loop, 24, RATE, 0.05, rng, 5.1, 9.8);
        rng_burn(env, rng);                        // SDRIAK: v1 dropped
        g_cw = soft;
        char *v2 = run_decoder(env, 12.0, 25.0, 0, 0, rng, &last);
        printf("--- stretched fist: char gaps 5.1 dits, word gaps 9.8 (24 WPM) ---\n");
        show("v2:", v2);
        printf("       v2 dist %d\n", fuzzy_dist(v2, CALL));
        check("v2 learns the stretched fist — a call copies with CQ intact",
              fuzzy_dist(v2, CALL) <= 1);
        std::free(v2);
        delete env;
        std::free(loop);

        const char *RUN = "TEST OK1BR OK1BR TEST OK1BR OK1BR NR 5";
        env = gen_env_fist(RUN, 28, RATE, 0.05, rng, 3.0, 5.0);
        g_cw = soft;
        v2 = run_decoder(env, 12.0, 25.0, 0, 0, rng, &last);
        printf("--- tight runner: word gaps squeezed to 5.0 dits (28 WPM) ---\n");
        show("v2:", v2);
        check("tight runner's 5-dit word gaps stay WORDS (no merges)",
              fuzzy_dist(v2, RUN) <= 1);
        std::free(v2);
        delete env;
        delete rng;
    }

    // -- mid-stream speed DROP: the stale-clock carrier trap ------------------------
    // An operator falling from ~34 to ~11 WPM makes every slow dah ~9× the
    // stale dit — the old "8 dits = carrier" rule swallowed the whole slow
    // transmission and the clock never saw a commit to relearn from
    // (live-caught 2026-07-16, 7028.9). The 0.8 s carrier floor must keep
    // the slow dahs committing so the boosted clock can snap down.
    {
        Rng *rng = new Rng(20260717);
        SkimDecode last;
        const char *SLOW = "CQ CQ DE OK1WW OK1WW PSE K";

        Env *env = gen_env("CQ CQ DE OK1WW OK1WW K", 34, RATE, 0.03, rng);
        Env *tail = gen_env(SLOW, 11, RATE, 0.03, rng);
        env->insert(env->end(), tail->begin(), tail->end());
        delete tail;

        rng_burn(env, rng);                        // SDRIAK: v1 dropped
        g_cw = soft;
        char *v2 = run_decoder(env, 14.0, 28.0, 0, 0, rng, &last);
        printf("--- speed drop: 34 -> 11 WPM mid-stream ---\n");
        show("v2:", v2);
        printf("       v2 dist %d\n", fuzzy_dist(v2, SLOW));
        check("v2 survives a 3x speed drop (slow copy ≤2 errors)",
              fuzzy_dist(v2, SLOW) <= 2);
        std::free(v2);
        delete env;
        delete rng;
    }

    // -- squelch attack: the head of an over after dead air --------------------------
    // The ONE systematic weakness the live A/B against SDC left standing
    // (2026-07-19): the first 1-2 characters after a longer pause. Reproduced
    // mechanisms: after the 8 s fist-forget the next over's first 4-8 marks
    // feed the dit BOOTSTRAP, not the decoder ("YOTA" → "OTA" — Y is exactly
    // the 4 marks the bootstrap eats; 32 % of the R3OM reference channel's
    // overs in the live log), and a cold channel pays the keying-duty EMA
    // ~0.3 s before the gate concedes it is keying. v2 keeps a ~2 s pre-roll
    // ring of the envelope and replays it through the lattice the moment the
    // channel wakes with a usable clock — the head must now read WHOLE, at
    // both speeds, after any pause, and from channel birth. The 4 s pause
    // guards the solid reopen that already worked. v1 has no ring: info only.
    {
        static const struct { double pause_s, wpm; } CASES[] = {
            { 10, 33 }, { 10, 20 }, { 4, 33 },
        };
        const char *OVER = "YOTA TEST R3OM R3OM";
        printf("--- squelch attack: over -> pause -> over, head survival ---\n");
        for (unsigned c = 0; c < std::size(CASES); c++) {
            char *v2 = nullptr;
            for (unsigned b = 1; b < 2; b++) {          // SDRIAK: b = 0 was v1
                Rng *rng = new Rng(20260719);
                // gen_env's 1.5 s flush tail is part of the pause.
                Env *env = gen_env(OVER, CASES[c].wpm, RATE, 0.0, rng);
                float z = 0.0f;
                for (unsigned i = 0; i < static_cast<unsigned>(CASES[c].pause_s * RATE); i++) {
                    env->push_back(z);
                }
                gen_env_append(env, OVER, CASES[c].wpm, RATE, 0.0, rng, 3.0, 7.0);
                key_run(env, 1.5 * RATE, 0.0f, 0, rng);
                g_cw = soft;
                v2 = run_decoder(env, 18.0, 25.0, 0, 0, rng, nullptr);
                delete env;
                delete rng;
            }
            char what[96];
            std::snprintf(what, sizeof(what),
                       "head survives a %.0f s pause at %.0f WPM (2nd over exact)",
                       CASES[c].pause_s, CASES[c].wpm);
            show("v2:", v2);
            check(what, last_over_dist(v2, OVER) == 0);
            std::free(v2);
        }

        // Cold start: the channel's very first over — no warmup, no history.
        char *v2 = nullptr;
        for (unsigned b = 1; b < 2; b++) {            // SDRIAK: b = 0 was v1
            Rng *rng = new Rng(20260719);
            Env *env = new Env();
            float z = 0.0f;
            for (unsigned i = 0; i < static_cast<unsigned>(3.0 * RATE); i++) {
                env->push_back(z);
            }
            gen_env_append(env, OVER, 33, RATE, 0.0, rng, 3.0, 7.0);
            key_run(env, 1.5 * RATE, 0.0f, 0, rng);
            g_cw = soft;
            v2 = run_decoder(env, 18.0, 25.0, 0, 0, rng, nullptr);
            delete env;
            delete rng;
        }
        show("v2:", v2);
        check("cold start decodes from the FIRST character (33 WPM)",
              last_over_dist(v2, OVER) == 0);
        std::free(v2);
    }

    // -- word re-read (SKIM_V2_RESCORE) ---------------------------------------------
    // A finished word the dictionary does not know is laid against the calls
    // it does know within two edits; one that the audio barely disputes
    // replaces it — for the extractor only, bracketed in the text. Three
    // ways it must behave: noise read as dit chains is never talked into a
    // call the dictionary holds (EE5E, SE5E … are in MASTER.SCP); a clean,
    // strong call the dictionary lacks stays as read (EA5BF beside EA5BH —
    // live 2026-10-15, a real station the re-read would have busted); and an
    // element keyed on the dit/dah border, read the wrong way, comes back
    // when the dictionary knows the call (the 6 of EC6AAE read as a 5).
    {
        const std::string dict = tmp_file("skimmer-cw-test.scp");
        write_file(dict, "EE5E\nSE5E\n5E5E\nN5EE\nEE4E\nEA5BH\nEC6AAE\n");
        scp->publish(SkimCallsignDict::load(dict.c_str()));
        g_cw = soft;
        Rng *rng = new Rng(20261016);
        printf("--- word re-read against the dictionary ---\n");

        Env *env = new Env();
        key_run(env, 60.0 * RATE, 0.0f, 0, rng);
        char *got = run_decoder(env, 10.0, 0.0, 0, 0, rng, nullptr);
        check("noise: no correction written", strchr(got, SkimCallsign::kFixOpen) == nullptr);
        std::free(got);
        delete env;

        env = gen_env("CQ DE EA5BF EA5BF K", 25, RATE, 0.02, rng);
        got = run_decoder(env, 12.0, 25.0, 0, 0, rng, nullptr);
        show("v2:", got);
        check("a clean call the dictionary lacks stays as read",
              strstr(got, "EA5BF") && strchr(got, SkimCallsign::kFixOpen) == nullptr);
        std::free(got);
        delete env;

        // EC6AAE with the 6's dah keyed `x` dits long — on the border, the
        // lattice reads a dit and the word comes out EC5AAE.
        const char *x_env = std::getenv("RS_TEST_DAH");
        const double x = x_env ? std::strtod(x_env, nullptr) : 1.7;
        const double wpm = 24, dit = 1.2 / wpm * RATE;
        env = gen_env("CQ DE", wpm, RATE, 0.0, rng);
        env->resize(env->size() - static_cast<unsigned>(1.5 * RATE));   // drop the flush
        key_run(env, 7 * dit, 0.0f, 0, rng);
        for (unsigned rep = 0; rep < 2; rep++) {
            for (const char *p = "EC6AAE"; *p; p++) {
                const char *m = morse_of(*p);
                for (const char *e = m; *e; e++) {
                    const double len = *e == '.' ? 1.0 : (*p == '6' ? x : 3.0);
                    key_run(env, len * dit, 1.0f, 0, rng);
                    if (e[1]) { key_run(env, dit, 0.0f, 0, rng); }
                }
                key_run(env, 3 * dit, 0.0f, 0, rng);
            }
            key_run(env, 4 * dit, 0.0f, 0, rng);                   // 7-dit word gap
        }
        gen_env_append(env, "K", wpm, RATE, 0.0, rng, 3.0, 7.0);
        key_run(env, 1.5 * RATE, 0.0f, 0, rng);
        got = run_decoder(env, 12.0, 20.0, 0, 0, rng, nullptr);
        char *shown = str_dup(got);
        SkimCallsign::strip_fix(shown);
        show("v2 shown:", shown);
        check("border dah: the pane shows what was heard (EC5AAE)",
              strstr(shown, "EC5AAE") != nullptr);
        check("border dah: the extractor gets EC6AAE",
              strstr(got, "\x01" "EC6AAE\x02") != nullptr);
        std::free(shown);
        std::free(got);
        delete env;

        delete rng;
        std::remove(dict.c_str());
    }

}


// -- weak-signal ladder: v3's reason to exist ------------------------------------
// The same over, keyed by a clean fist at 25 WPM, under falling SNR (this
// gate's scale: carrier over the noise in the 250 Hz channel, SNR500 + 3 dB),
// plain and under slow 8 dB QSB, three seeds a rung. Every rung prints the
// edit distance of v2 and v3 summed over the seeds; the checks: v3 never
// loses to v2 on the ladder's sum, and wins where v2 starts to lose.
static void weak_ladder(const SkimDecodeBackend *v2, const SkimDecodeBackend *v3) {
    const char *PAY = "CQ CQ DE OK1HRA OK1HRA K";
    static const double RUNG[] = { 12, 9, 6, 4, 2, 0, -2 };
    printf("\n=== weak-signal ladder (25 WPM, SNR in 250 Hz; SNR500 = −3 dB) ===\n");
    int sum2 = 0, sum3 = 0, low2 = 0, low3 = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned r = 0; r < std::size(RUNG); r++) {
            int d2 = 0, d3 = 0;
            for (unsigned seed = 0; seed < 3; seed++) {
                char *got[2];
                for (unsigned b = 0; b < 2; b++) {
                    Rng *rng = new Rng(20261114 + 97 * seed + 1000 * q);
                    Env *env = gen_env(PAY, 25, RATE, 0.03, rng);
                    gen_env_append(env, PAY, 25, RATE, 0.03, rng, 3.0, 7.0);
                    key_run(env, 1.5 * RATE, 0.0f, 0, rng);
                    g_cw = b ? v3 : v2;
                    got[b] = run_decoder(env, 11.0, RUNG[r], q ? 0.15 : 0.0, q ? 8.0 : 0.0, rng, nullptr);
                    delete env;
                    delete rng;
                }
                char *both = str_printf("%s %s", PAY, PAY);
                d2 += std::min(fuzzy_dist(got[0], both), static_cast<int>(strlen(both)));
                d3 += std::min(fuzzy_dist(got[1], both), static_cast<int>(strlen(both)));
                if (std::getenv("LADDER_SHOW")) { show("v2:", got[0]); show("v3:", got[1]); }
                std::free(both);
                std::free(got[0]);
                std::free(got[1]);
            }
            printf("  %s %5.1f dB (SNR500 %5.1f): v2 dist %3d   v3 dist %3d\n",
                   q ? "QSB  " : "plain", RUNG[r], RUNG[r] - 3.0, d2, d3);
            sum2 += d2;
            sum3 += d3;
            if (RUNG[r] <= 6) { low2 += d2; low3 += d3; }
        }
    }
    printf("  sum: v2 %d, v3 %d; at SNR500 <= 3 dB: v2 %d, v3 %d\n", sum2, sum3, low2, low3);
    check("weak ladder: v3 never worse than v2 over the ladder", sum3 <= sum2);
    check("weak ladder: v3 reads more than v2 at SNR500 <= 3 dB", low3 < low2);
}


// -- the call reader (v3 M8): a weak call the text tears is confirmed by
// the audio — the dictionary call, never its twin; noise gets nothing.
static void kws_cases(const SkimDecodeBackend *v3,
                      const std::shared_ptr<SkimCallsignDictSource> &scp) {
    const std::string dict = tmp_file("skimmer-cw-test-kws.scp");
    write_file(dict, "OK1HRA\nOK1HRB\nOK2HRA\nOK1HR\nOK1RA\nDL1HRA\nOK1HQA\nEA5BH\n");
    scp->publish(SkimCallsignDict::load(dict.c_str()));
    g_cw = v3;
    printf("\n=== call reader (v3) ===\n");
    const char *PAY = "CQ CQ DE OK1HRA OK1HRA K";
    int hit = 0, twin = 0, runs = 0;
    for (unsigned seed = 0; seed < 6; seed++) {
        Rng *rng = new Rng(20261115 + seed);
        Env *env = gen_env(PAY, 26, RATE, 0.04, rng);
        for (unsigned rep = 0; rep < 2; rep++) {
            gen_env_append(env, PAY, 26, RATE, 0.04, rng, 3.0, 7.0);
        }
        key_run(env, 2.0 * RATE, 0.0f, 0, rng);
        char *got = run_decoder(env, 9.0, 4.0 + 1.0 * (seed % 3), 0, 0, rng, nullptr);
        runs++;
        hit  += strstr(got, "\x01" "OK1HRA\x02") != nullptr;
        twin += strstr(got, "\x01" "OK1HRB\x02") || strstr(got, "\x01" "OK2HRA\x02") ||
                strstr(got, "\x01" "OK1HR\x02") || strstr(got, "\x01" "DL1HRA\x02") ||
                strstr(got, "\x01" "OK1HQA\x02");
        char *shown = str_dup(got);
        for (char *p = shown; *p; p++) {
            if (*p == SkimCallsign::kFixOpen) { *p = '['; }
            if (*p == SkimCallsign::kFixClose) { *p = ']'; }
        }
        show("v3:", shown);
        std::free(shown);
        std::free(got);
        delete env;
        delete rng;
    }
    printf("       reader: OK1HRA confirmed in %d of %d runs, a twin in %d\n", hit, runs, twin);
    check("call reader: the weak call confirmed in most runs", hit * 2 > runs);
    check("call reader: never a dictionary twin", twin == 0);

    Rng *rng = new Rng(20261116);
    Env *env = new Env();
    key_run(env, 90.0 * RATE, 0.0f, 0, rng);
    char *got = run_decoder(env, 0.0, 6.0, 0, 0, rng, nullptr);
    check("call reader: 90 s of noise, no correction", strchr(got, SkimCallsign::kFixOpen) == nullptr);
    std::free(got);
    delete env;
    delete rng;
    std::remove(dict.c_str());
}

int main() {
    printf("=== CW decode gate (offline, synthetic) ===\n");
    // The word re-read is on for the whole gate: with no dictionary loaded
    // (until its own section) it finds no candidate and changes nothing.
    set_env("SKIM_V2_RESCORE", "1", false);
    set_env("SKIM_V3_RESCORE", "1", false);
    // SDRIAK: the gate's own dictionary (empty until a section loads one)
    // and backends on upstream's presets and SKIM_* switches (upstream: the
    // global dictionary and process-wide singletons).
    const auto scp = std::make_shared<SkimCallsignDictSource>();
    CwDecoderConfig c2 = cw_config_from_env(cw_preset_v2(), 2);
    CwDecoderConfig c3 = cw_config_from_env(cw_preset_v3(), 3);
    c2.diag.oracle = c3.diag.oracle = SkimOracle::from_env();   // SKIM_ORACLE
    c2.diag.dump = c3.diag.dump = CwRunDumpSink::from_env();    // SKIM_CW_DUMP_RUNS
    const auto v2 = skim_decode_cw_v2_new(c2, scp);
    const auto v3 = skim_decode_cw_v3_new(c3, scp);
    run_suite(v2.get());                         // SDRIAK: no v1 suite
    run_suite(v3.get());
    soft_cases(v2.get(), "v2", scp);
    soft_cases(v3.get(), "v3", scp);
    weak_ladder(v2.get(), v3.get());
    kws_cases(v3.get(), scp);

    printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
           fails ? "FAIL" : "PASS — all three CW backends copy, adapt and stay "
                            "quiet on noise; v2 and v3 ride out QSB and learn the "
                            "operator's fist.");
    return fails ? 1 : 0;
}
