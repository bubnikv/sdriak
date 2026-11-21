// cw_bench — the CW decoders measured behind the real channelizer.
//
// The gates (src/cw_test.cpp) feed the decoder at 250 Hz directly; they never
// see the channelizer's passband, its zero-history start, or a wideband
// impairment. cw_bench keys synthetic CW into wideband complex IQ, optionally
// through a Watterson fading channel, with impulse noise, or into a recorded
// IQ bed, runs it through SkimChannelizer and one decoder channel, and scores
// the decoded text against what was sent.
//
// One invocation is one configuration with lists of speeds, offsets and SNRs;
// experiments/cw_bench/bench.py runs named cases and prints the tables.
// --gate runs fixed regression checks (registered with ctest).
//
// Output, one line per over:
//   cfg wpm offset_hz snr500 seed ref_len dist pay_len pay_dist
// dist: Levenshtein of the whole decoded over against "VVV <payload>";
// pay_dist: the payload alone against the decoded text, free alignment start
// (the warm-up and any junk before it are not counted).
//
// Part of SDRIAK. GPL-3.0-or-later.
#include "engine/decode_cw.h"
#include "engine/decode_bell.h"
#ifdef CW_ENGINE_DEEPCW
#include "engine/decode_deepcw.h"
#include "engine/decode_deepcw2.h"
#endif
#include "engine/channelizer.h"
#include "engine/impulse_repair.h"
#include "engine/callsign.h"
#include "sdriak/fir_bandpass.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using cplx = std::complex<double>;

// --- options -----------------------------------------------------------------

struct Path {                          // one Watterson path
    double power = 1.0;                // share of the mean power
    double spread_hz = 0.0;            // Doppler spread B = 2σ_f (0 = no fading)
    double shift_hz = 0.0;             // Doppler shift
    double delay_ms = 0.0;             // differential delay
};

// Impulse shapes (--impulse ...:<shape>): one wideband sample; the pulse as
// a receiver delivers it (band-limited, ringing, at a random sub-sample
// time); a sky-wave sferic (2–4 such pulses 0.5–3 ms apart).
enum ImpShape { kImpDelta = 0, kImpRx = 1, kImpSferic = 2 };
// The rx pulse: half-length in samples, low-pass edge as a fraction of the rate.
constexpr int kImpHalf = 24;
constexpr double kImpFc = 0.45;

struct Options {
    std::string cfg = "v3";
    std::vector<double> wpm = {25};
    std::vector<double> offset = {10};
    std::vector<double> snr = {10};
    int seeds = 10, seed0 = 0;
    double rate = 8000.0;
    unsigned channel = 0;
    double passband = 0.0;             // 0 = the channelizer's default
    unsigned taps = 0;
    double lead_s = 0.5;
    double tail_s = 0.0;               // extra noise after the over (decoders that commit late)
    double jitter = 0.03;
    double edge_ms = 5.0;
    double preamble_db = 0.0;          // > 0: the "VVV " preamble this much stronger
    std::vector<Path> paths;           // empty = no fading
    double imp_rate = 0.0;             // impulses per second
    double imp_db = 0.0;               // impulse amplitude over the wideband noise rms
    bool imp_periodic = false;         // a regular train (ignition) instead of Poisson
    bool imp_crash = false;            // static crashes: imp_rate flashes per second
    int imp_shape = 0;                 // ImpShape: delta, receiver-shaped, sferic
    double clip_db = 0.0;              // > 0: ADC clipping, this far over the wideband noise rms
    bool repair = false;               // ImpulseRepair on the wideband stream before the channelizer
    double drift_hz_s = 0.0;           // the target's frequency drift, Hz/s
    double chirp_hz = 0.0, chirp_ms = 0.0;   // each key-down's offset and its decay
    bool cochannel = false;            // a second keyed station in the target's channel
    double cochannel_db = 0.0, cochannel_hz = 0.0;  // its level over the target, its offset
    int qrm_n = 0;                     // other keyed CW stations in other channels
    double qrm_db = 30.0;              // their SNR500, the strongest
    double qrm_range = 0.0;            // and how far below it they spread
    double repair_thr_db = 12.0;       // its detection threshold
    double repair_split_db = 25.0;     // its strong-signal split (0: none)
    double repair_holdoff_ms = 8.0;    // its pause after a rejected gap
    int repair_order = 32;             // its AR model order
    double classa_a = 0.0;             // > 0: Middleton Class A noise, impulsive index A
    double classa_gamma = 0.0;         //      and Gaussian-to-impulsive power ratio Γ
    std::string bed;                   // cf32 IQ recording at `rate`
    double noise_only_s = 0.0;         // > 0: decode this much noise, count characters
    bool show = false;
    bool fade_check = false;           // print the fading gain's statistics
    std::string gate;                  // "" = no gate; "quick", "startup"
};

std::vector<double> parse_list(const char *s) {
    std::vector<double> v;
    std::string t(s);
    size_t at = 0;
    while (at <= t.size()) {
        size_t e = t.find(',', at);
        if (e == std::string::npos) e = t.size();
        const std::string tok = t.substr(at, e - at);
        const size_t colon = tok.find(':');
        if (colon != std::string::npos) {           // a:b:step
            const size_t c2 = tok.find(':', colon + 1);
            const double a = std::atof(tok.substr(0, colon).c_str());
            const double b = std::atof(tok.substr(colon + 1, c2 - colon - 1).c_str());
            const double st = c2 == std::string::npos ? 1.0 : std::atof(tok.substr(c2 + 1).c_str());
            for (double x = a; x <= b + 1e-9; x += st) v.push_back(x);
        } else if (!tok.empty()) {
            v.push_back(std::atof(tok.c_str()));
        }
        at = e + 1;
    }
    return v;
}

// ITU-R F.1487 conditions as two equal paths (delay, spread each), and a
// single flat path at a given spread (VE3NEA/DeepCW's grid: 0.1/0.3/1/3 Hz).
bool parse_fade(const std::string &f, std::vector<Path> &paths) {
    struct Itu { const char *name; double delay_ms, spread_hz; };
    static const Itu kItu[] = {
        {"good", 0.5, 0.1}, {"moderate", 1.0, 0.5}, {"poor", 2.0, 1.0},
        {"lowlat", 6.0, 10.0}, {"highlat", 3.0, 10.0}, {"highlat-poor", 7.0, 30.0},
    };
    paths.clear();
    if (f == "none") return true;
    if (f.rfind("itu:", 0) == 0) {
        for (const Itu &c : kItu) {
            if (f.substr(4) == c.name) {
                paths.push_back({0.5, c.spread_hz, 0.0, 0.0});
                paths.push_back({0.5, c.spread_hz, 0.0, c.delay_ms});
                return true;
            }
        }
        return false;
    }
    if (f.rfind("flat:", 0) == 0) {
        paths.push_back({1.0, std::atof(f.c_str() + 5), 0.0, 0.0});
        return true;
    }
    if (f.rfind("paths:", 0) == 0) {                // power,spread,shift,delay_ms;...
        std::string t = f.substr(6);
        double sum = 0;
        size_t at = 0;
        while (at < t.size()) {
            size_t e = t.find(';', at);
            if (e == std::string::npos) e = t.size();
            Path p;
            if (std::sscanf(t.substr(at, e - at).c_str(), "%lf,%lf,%lf,%lf", &p.power, &p.spread_hz,
                            &p.shift_hz, &p.delay_ms) < 2)
                return false;
            sum += p.power;
            paths.push_back(p);
            at = e + 1;
        }
        for (Path &p : paths) p.power /= sum;
        return !paths.empty();
    }
    return false;
}

bool parse_args(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--cfg") o.cfg = val();
        else if (a == "--wpm") o.wpm = parse_list(val());
        else if (a == "--offset") o.offset = parse_list(val());
        else if (a == "--snr") o.snr = parse_list(val());
        else if (a == "--seeds") o.seeds = std::atoi(val());
        else if (a == "--seed0") o.seed0 = std::atoi(val());
        else if (a == "--rate") o.rate = std::atof(val());
        else if (a == "--channel") o.channel = static_cast<unsigned>(std::atoi(val()));
        else if (a == "--passband") o.passband = std::atof(val());
        else if (a == "--taps") o.taps = static_cast<unsigned>(std::atoi(val()));
        else if (a == "--lead") o.lead_s = std::atof(val());
        else if (a == "--tail") o.tail_s = std::atof(val());
        else if (a == "--jitter") o.jitter = std::atof(val());
        else if (a == "--edge") o.edge_ms = std::atof(val());
        else if (a == "--preamble-db") o.preamble_db = std::atof(val());
        else if (a == "--fade") { if (!parse_fade(val(), o.paths)) { std::fprintf(stderr, "bad --fade\n"); return false; } }
        else if (a == "--impulse") {    // poisson|periodic|crash:<rate>:<dB>[:delta|rx|sferic]
            const std::string s = val();
            o.imp_periodic = s.rfind("periodic:", 0) == 0;
            o.imp_crash = s.rfind("crash:", 0) == 0;
            const char *p = std::strchr(s.c_str(), ':');
            char shape[16] = "";
            if (!p || std::sscanf(p + 1, "%lf:%lf:%15s", &o.imp_rate, &o.imp_db, shape) < 2) {
                std::fprintf(stderr, "bad --impulse\n");
                return false;
            }
            const std::string sh = shape;
            if (sh.empty() || sh == "delta") o.imp_shape = kImpDelta;
            else if (sh == "rx") o.imp_shape = kImpRx;
            else if (sh == "sferic") o.imp_shape = kImpSferic;
            else { std::fprintf(stderr, "bad --impulse shape %s\n", shape); return false; }
        }
        else if (a == "--clip") o.clip_db = std::atof(val());
        else if (a == "--qrm") {                                  // <N>:<SNR500 dB>[:<range dB>]
            if (std::sscanf(val(), "%d:%lf:%lf", &o.qrm_n, &o.qrm_db, &o.qrm_range) < 2 || o.qrm_n < 0) {
                std::fprintf(stderr, "bad --qrm\n");
                return false;
            }
        }
        else if (a.rfind("--repair-order=", 0) == 0) {    // its AR model order
            o.repair = true;
            o.repair_order = std::atoi(a.c_str() + 15);
        }
        else if (a.rfind("--repair-holdoff=", 0) == 0) {  // its pause after a rejected gap, ms
            o.repair = true;
            o.repair_holdoff_ms = std::atof(a.c_str() + 17);
        }
        else if (a.rfind("--repair-split=", 0) == 0) {    // the strong-signal split, dB (0: off)
            o.repair = true;
            o.repair_split_db = std::atof(a.c_str() + 15);
        }
        else if (a.rfind("--repair", 0) == 0) {           // --repair[=<threshold dB>]
            o.repair = true;
            if (a.size() > 9 && a[8] == '=') o.repair_thr_db = std::atof(a.c_str() + 9);
        }
        else if (a == "--drift") o.drift_hz_s = std::atof(val());     // Hz/s
        else if (a == "--chirp") {                                  // <Hz>:<ms>
            if (std::sscanf(val(), "%lf:%lf", &o.chirp_hz, &o.chirp_ms) != 2 || o.chirp_ms <= 0) {
                std::fprintf(stderr, "bad --chirp\n");
                return false;
            }
        }
        else if (a == "--cochannel") {                              // <dB>:<Hz>
            if (std::sscanf(val(), "%lf:%lf", &o.cochannel_db, &o.cochannel_hz) != 2) {
                std::fprintf(stderr, "bad --cochannel\n");
                return false;
            }
            o.cochannel = true;
        }
        else if (a == "--classa") {                                 // <A>:<Gamma>
            if (std::sscanf(val(), "%lf:%lf", &o.classa_a, &o.classa_gamma) != 2 || o.classa_a <= 0) {
                std::fprintf(stderr, "bad --classa\n");
                return false;
            }
        }
        else if (a == "--bed") o.bed = val();
        else if (a == "--noise-only") o.noise_only_s = std::atof(val());
        else if (a == "--show") o.show = true;
        else if (a == "--fade-check") o.fade_check = true;
        else if (a == "--gate") o.gate = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "quick";
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

// --- text and keying ------------------------------------------------------------

const char *morse(char c) {
    static const struct { char c; const char *m; } T[] = {
        {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},{'G',"--."},
        {'H',"...."},{'I',".."},{'J',".---"},{'K',"-.-"},{'L',".-.."},{'M',"--"},{'N',"-."},
        {'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},{'S',"..."},{'T',"-"},{'U',"..-"},
        {'V',"...-"},{'W',".--"},{'X',"-..-"},{'Y',"-.--"},{'Z',"--.."},{'0',"-----"},
        {'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},{'5',"....."},{'6',"-...."},
        {'7',"--..."},{'8',"---.."},{'9',"----."}};
    for (auto &e : T) if (e.c == c) return e.m;
    return nullptr;
}

// 12 tokens: callsign-like (1–2 letters, a digit, 1–3 letters) or traffic words.
std::string payload(std::mt19937 &rng) {
    static const char *W[] = {"CQ", "DE", "TEST", "5NN", "TU", "K", "UR", "RST"};
    const char *L = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string s;
    for (int t = 0; t < 12; t++) {
        if (!s.empty()) s += ' ';
        if (rng() % 2) {
            s += W[rng() % 8];
        } else {
            int pre = 1 + rng() % 2, suf = 1 + rng() % 3;
            for (int i = 0; i < pre; i++) s += L[rng() % 26];
            s += char('0' + rng() % 10);
            for (int i = 0; i < suf; i++) s += L[rng() % 26];
        }
    }
    return s;
}

// Keyed envelope at `rate`: ±jitter on every run (lead and tail included),
// raised-cosine edges of edge_ms.
std::vector<float> envelope(const std::string &text, double wpm, double rate, double jitter,
                            double lead_s, double edge_ms, std::mt19937 &rng) {
    std::uniform_real_distribution<double> uj(-jitter, jitter);
    const double dit = 1.2 / wpm * rate;
    std::vector<float> e;
    auto run = [&](double n, float v) {
        n *= 1.0 + uj(rng);
        for (long i = 0; i < std::lround(n); i++) e.push_back(v);
    };
    run(lead_s * rate, 0.f);
    for (char c : text) {
        if (c == ' ') { run(4 * dit, 0.f); continue; }   // + the 3-dit char space = 7
        const char *m = morse(c);
        if (!m) continue;
        for (const char *p = m; *p; p++) {
            run(*p == '-' ? 3 * dit : dit, 1.f);
            if (p[1]) run(dit, 0.f);
        }
        run(3 * dit, 0.f);
    }
    run(2.0 * rate, 0.f);
    const int L = std::max(1, static_cast<int>(edge_ms * 1e-3 * rate));
    if (L < 3) return e;
    std::vector<float> w(L);
    double sum = 0;
    for (int i = 0; i < L; i++) {
        w[i] = static_cast<float>(0.5f - 0.5f * std::cos(2 * SKIM_PI * i / (L - 1)));
        sum += w[i];
    }
    std::vector<float> o(e.size());
    for (size_t n = 0; n < e.size(); n++) {
        double a = 0;
        for (int k = 0; k < L && k <= static_cast<int>(n); k++) a += w[k] * e[n - k];
        o[n] = static_cast<float>(a / sum);
    }
    return o;
}


// The keying of envelope() as on/off runs, with the same random draws, for
// keying rendered sample by sample (--qrm: hundreds of stations would not
// fit in memory as full envelopes).
std::vector<std::pair<long, unsigned char>> envelope_runs(const std::string &text, double wpm, double rate,
                                                          double jitter, double lead_s, double /*edge_ms*/,
                                                          std::mt19937 &rng) {
    std::uniform_real_distribution<double> uj(-jitter, jitter);
    const double dit = 1.2 / wpm * rate;
    std::vector<std::pair<long, unsigned char>> r;
    auto run = [&](double n, unsigned char v) {
        n *= 1.0 + uj(rng);
        r.emplace_back(std::max(0L, std::lround(n)), v);
    };
    run(lead_s * rate, 0);
    for (char c : text) {
        if (c == ' ') { run(4 * dit, 0); continue; }
        const char *m = morse(c);
        if (!m) continue;
        for (const char *p = m; *p; p++) {
            run(*p == '-' ? 3 * dit : dit, 1);
            if (p[1]) run(dit, 0);
        }
        run(3 * dit, 0);
    }
    run(2.0 * rate, 0);
    return r;
}

// envelope()'s output from its runs, one sample at a time: the raised-cosine
// edge filter computed only where the last L keying samples differ, so the
// values are envelope()'s exactly.
struct KeyingRender {
    std::vector<std::pair<long, unsigned char>> runs;
    size_t ri = 0;
    long left = 0;
    unsigned char cur = 0;
    std::vector<unsigned char> ring;
    int pos = 0, ones = 0;

    float next(const std::vector<float> &w, double sum) {
        unsigned char b = 0;
        while (left == 0 && ri < runs.size()) {
            left = runs[ri].first;
            cur = runs[ri].second;
            ri++;
        }
        if (left > 0) {
            b = cur;
            left--;
        }
        const int L = static_cast<int>(w.size());
        if (L < 3) return b;
        if (ring.empty()) ring.assign(L, 0);
        ones += b - ring[pos];
        ring[pos] = b;
        const int newest = pos;
        pos = (pos + 1) % L;
        if (ones == 0) return 0.0f;
        if (ones == L) return 1.0f;     // envelope() sums the same weights as its normaliser
        double a = 0;
        for (int k = 0; k < L; k++) a += w[k] * static_cast<float>(ring[(newest - k + L) % L]);
        return static_cast<float>(a / sum);
    }
};
// --- the channel ------------------------------------------------------------------

// One Watterson path's complex gain for n samples at rate: complex white
// Gaussian noise at a low rate through a Gaussian filter of impulse-response
// width σ_t = √2/(2πB) (its amplitude response is √2 wider than the
// Gaussian power spectrum of spread B = 2σ_f), normalised to unit power,
// linearly interpolated to rate, rotated by the Doppler shift.
std::vector<cplx> fading_gain(const Path &p, size_t n, double rate, std::mt19937 &rng) {
    std::vector<cplx> g(n, cplx(1.0, 0.0));
    if (p.spread_hz <= 0) {
        for (size_t i = 0; i < n; i++) g[i] = std::polar(1.0, 2 * SKIM_PI * p.shift_hz * i / rate);
        return g;
    }
    const double fs = std::max(32.0, 64.0 * p.spread_hz);
    const double sig_t = std::sqrt(2.0) / (2 * SKIM_PI * p.spread_hz);
    const int half = static_cast<int>(std::ceil(4 * sig_t * fs));
    std::vector<double> h(2 * half + 1);
    double e2 = 0;
    for (int k = -half; k <= half; k++) {
        const double t = k / fs;
        h[k + half] = std::exp(-0.5 * t * t / (sig_t * sig_t));
        e2 += h[k + half] * h[k + half];
    }
    for (double &x : h) x /= std::sqrt(e2);
    const size_t m = static_cast<size_t>(n / rate * fs) + 3;
    std::normal_distribution<double> nd(0.0, std::sqrt(0.5));
    std::vector<cplx> w(m + h.size());
    for (cplx &x : w) x = cplx(nd(rng), nd(rng));
    std::vector<cplx> lo(m);
    for (size_t i = 0; i < m; i++) {
        cplx acc = 0;
        for (size_t k = 0; k < h.size(); k++) acc += h[k] * w[i + k];
        lo[i] = acc;
    }
    for (size_t i = 0; i < n; i++) {
        const double x = i / rate * fs;
        const size_t j = static_cast<size_t>(x);
        const double f = x - j;
        g[i] = (lo[j] * (1 - f) + lo[std::min(j + 1, m - 1)] * f) *
               std::polar(1.0, 2 * SKIM_PI * p.shift_hz * i / rate);
    }
    return g;
}

// The fading generator against Watterson's Gaussian Doppler spectrum: mean
// power 1, and |ρ(Δ)| = exp(−2π²σ_f²Δ²) for one path (σ_f = B/2), measured on
// 600 s of gain at 250 Hz. Returns the largest |ρ| error over the lags shown.
double fade_check(const Path &p) {
    const double rate = 250.0;
    std::mt19937 rng(1);
    const std::vector<cplx> g = fading_gain(p, static_cast<size_t>(600 * rate), rate, rng);
    double pw = 0;
    for (const cplx &x : g) pw += std::norm(x);
    pw /= g.size();
    std::printf("spread %.2f Hz: mean power %.3f;", p.spread_hz, pw);
    const double sf = p.spread_hz / 2;
    double worst = 0;
    for (double lag : {0.375 / p.spread_hz * 0.5, 0.375 / p.spread_hz, 0.375 / p.spread_hz * 2}) {
        const size_t k = static_cast<size_t>(lag * rate);
        if (k == 0 || k >= g.size() / 4) continue;
        cplx acc = 0;
        for (size_t i = k; i < g.size(); i++) acc += g[i] * std::conj(g[i - k]);
        const double rho = std::abs(acc) / ((g.size() - k) * pw);
        const double want = std::exp(-2 * SKIM_PI * SKIM_PI * sf * sf * (k / rate) * (k / rate));
        std::printf("  |rho(%.3f s)| %.3f (theory %.3f)", k / rate, rho, want);
        worst = std::max(worst, std::fabs(rho - want));
    }
    std::printf("\n");
    return worst;
}

// The channelizer prototype's noise bandwidth (complex, two-sided, Hz): the
// band floor set_floor() is told about, as the pipeline would measure it.
double channel_enbw(const Options &o) {
    const double spacing = 125.0;
    const unsigned M = static_cast<unsigned>(o.rate / spacing + 0.5);
    const unsigned K = o.taps ? o.taps : 8;
    const double pb = o.passband > 0 ? o.passband : spacing / 2;
    const std::vector<double> h = fir_bandpass(static_cast<int>(K * M), -pb, pb, o.rate, 0, 0, 1.0);
    double s = 0, s2 = 0;
    for (double x : h) { s += x; s2 += x * x; }
    return o.rate * s2 / (s * s);
}

std::vector<float> read_bed(const std::string &path) {
    std::vector<float> v;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    v.resize(static_cast<size_t>(n) / sizeof(float) & ~static_cast<size_t>(1));
    const size_t got = std::fread(v.data(), sizeof(float), v.size(), f);
    v.resize(got & ~static_cast<size_t>(1));
    std::fclose(f);
    return v;
}

// --- scoring --------------------------------------------------------------------------

int lev(const std::string &a, const std::string &b, bool free_start_in_b) {
    std::vector<int> d(b.size() + 1), n(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) d[j] = free_start_in_b ? 0 : static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        n[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++)
            n[j] = std::min({d[j] + 1, n[j - 1] + 1, d[j - 1] + (a[i - 1] != b[j - 1])});
        std::swap(d, n);
    }
    if (!free_start_in_b) return d[b.size()];
    return *std::min_element(d.begin(), d.end());     // and free end
}

std::string normalise(const std::string &s) {
    std::vector<char> buf(s.begin(), s.end());
    buf.push_back('\0');
    SkimCallsign::strip_fix(buf.data());
    const std::string t(buf.data());
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (c == 0xC2 && i + 1 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0xB7) {
            i++;
            o += ' ';
            continue;
        }
        o += static_cast<char>(c);
    }
    std::string r;
    for (char c : o) {
        if (c == ' ' && (r.empty() || r.back() == ' ')) continue;
        r += c;
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

// --- one over --------------------------------------------------------------------------

struct Result {
    size_t ref_len = 0, pay_len = 0;
    int dist = 0, pay_dist = 0;
    std::string ref, hyp;
};

std::unique_ptr<SkimDecodeBackend> make_backend(const std::string &name) {
#ifdef CW_ENGINE_DEEPCW
    // The DeepCW backends (DeepCW.md), inline so a run is deterministic:
    //   dcw1  — v1 model, SKIM_DEEPCW_MODEL (SKIM_DEEPCW_* switches)
    //   dcw2  — v2 wide model, SKIM_DEEPCW2_MODEL (SKIM_DEEPCW2_* switches)
    //   dcw2n — v2 narrow model, SKIM_DEEPCW2_NARROW_MODEL
    // Suffixes: ":nogate" infers on every tick (no keyed-line gate);
    // ":median" (default) / ":q90" / ":fill" set dcw2's tile noise level (DeepcwV2Config::noise_mode).
    std::string base = name;
    bool nogate = false;
    std::string noise_mode;
    for (size_t c; (c = base.rfind(':')) != std::string::npos; base.resize(c)) {
        const std::string suf = base.substr(c + 1);
        if (suf == "nogate") nogate = true;
        else if (suf == "median" || suf == "fill" || suf == "q90") noise_mode = suf;
        else break;
    }
    if (base == "dcw1") {
        DeepcwConfig c = DeepcwConfig::from_env();
        c.sync = true;
        if (nogate)
            c.gate_db = -1e9;
        auto be = std::make_unique<SkimDeepcwBackend>(c);
        std::string err;
        if (!be->available(&err)) { std::fprintf(stderr, "dcw1: %s\n", err.c_str()); std::exit(2); }
        return be;
    }
    if (base == "dcw2" || base == "dcw2n" || base == "dcw2v1") {
        // dcw2v1: the v1 model (SKIM_DEEPCW_MODEL) through dcw2's stitcher —
        // against dcw1 it shows what the stitcher does (DeepcwV2Config::model_v1).
        DeepcwV2Config c = DeepcwV2Config::from_env();
        c.sync = true;
        c.narrow = base == "dcw2n";
        c.model_v1 = base == "dcw2v1";
        if (c.model_v1) {
            const char *m = std::getenv("SKIM_DEEPCW_MODEL");
            c.model_path = m ? m : "";
        }
        if (c.narrow) {
            const char *m = std::getenv("SKIM_DEEPCW2_NARROW_MODEL");
            c.model_path = m ? m : "";
        }
        if (nogate)
            c.gate = false;
        if (!noise_mode.empty())
            c.noise_mode = noise_mode;
        auto be = std::make_unique<SkimDeepcw2Backend>(c);
        std::string err;
        if (!be->available(&err)) { std::fprintf(stderr, "%s: %s\n", base.c_str(), err.c_str()); std::exit(2); }
        return be;
    }
#endif
    // E. L. Bell's 1977 decoder (BELL.md). "bell" as corrected against the
    // thesis listing, with the front end's clamps; "bell:listing" without
    // them; "bell:port" as morse-wip has it. Further suffixes switch one
    // part each: ":sign" morse-wip's xtrans sign, ":noclamp" neither clamp,
    // ":paths<N>", ":noif" no emulation of Bell's IF filter, ":wpm<lo>-<hi>"
    // the speeds a path may move to, ":q<x>" MODEL's amplitude process
    // noise per 5 ms during a mark, ":qbank" four q classes (1e-4, 3e-4,
    // 1e-3, 3e-3) chosen by the posterior, ":qbank<x>/<y>/..." others,
    // ":qsw<x>" their switch probability at an element change; ":coh" the
    // coherent variant, ":noafc" without its AFC, ":imp<eps>" and
    // ":kappa<k>" its impulse mixture (":impscore" for scoring only),
    // ":repw" weighing samples --repair rebuilt by 1 − their share,
    // ":rate<Hz>" its sample
    // rate (125), ":spread<B>" its Doppler
    // spread (Hz),
    // ":spreads<x>/<y>/..." spread classes, ":match<ms>" its AR(1) match lag.
    if (name.rfind("bell", 0) == 0) {
        BellBackendConfig c;
        for (size_t at = name.find(':'); at != std::string::npos;) {
            const size_t next = name.find(':', at + 1);
            const std::string suf = name.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
            if (suf == "port") c = BellBackendConfig::port();
            else if (suf == "listing") c = BellBackendConfig::listing();
            else if (suf == "sign") c.decoder.port_xtrans_sign = true;
            else if (suf == "noclamp") { c.decoder.clamp_z = false; c.agc_clamp = false; }
            else if (suf == "noif") c.if_filter = false;
            else if (suf.rfind("paths", 0) == 0) c.decoder.paths = std::atoi(suf.c_str() + 5);
            else if (suf == "qbank") c.decoder.q_bank = {0.0001f, 0.0003f, 0.001f, 0.003f};
            else if (suf.rfind("qbank", 0) == 0) {                  // qbank<x>/<y>/...
                c.decoder.q_bank.clear();
                for (size_t p = 5; p < suf.size();) {
                    const size_t e = std::min(suf.find('/', p), suf.size());
                    c.decoder.q_bank.push_back(static_cast<float>(std::atof(suf.substr(p, e - p).c_str())));
                    p = e + 1;
                }
            }
            else if (suf.rfind("qsw", 0) == 0) c.decoder.q_switch = static_cast<float>(std::atof(suf.c_str() + 3));
            else if (suf == "coh") c.decoder.coherent = true;
            else if (suf == "noafc") c.coh_afc = false;
            else if (suf == "impscore") c.decoder.coh_imp_update = false;
            else if (suf == "repw") c.decoder.coh_repair_weight = true;
            else if (suf.rfind("imp", 0) == 0) c.decoder.coh_imp_eps = static_cast<float>(std::atof(suf.c_str() + 3));
            else if (suf.rfind("kappa", 0) == 0) c.decoder.coh_imp_kappa = static_cast<float>(std::atof(suf.c_str() + 5));
            else if (suf.rfind("rate", 0) == 0) c.coh_rate_hz = std::atof(suf.c_str() + 4);
            else if (suf.rfind("spreads", 0) == 0) {                // spreads<x>/<y>/...
                c.decoder.coh_spreads.clear();
                for (size_t p = 7; p < suf.size();) {
                    const size_t e = std::min(suf.find('/', p), suf.size());
                    c.decoder.coh_spreads.push_back(static_cast<float>(std::atof(suf.substr(p, e - p).c_str())));
                    p = e + 1;
                }
            }
            else if (suf.rfind("spread", 0) == 0) c.decoder.coh_spread = static_cast<float>(std::atof(suf.c_str() + 6));
            else if (suf.rfind("match", 0) == 0) c.decoder.coh_match_ms = static_cast<float>(std::atof(suf.c_str() + 5));
            else if (suf.size() > 1 && suf[0] == 'q') c.decoder.mark_q = static_cast<float>(std::atof(suf.c_str() + 1));
            else if (suf.rfind("wpm", 0) == 0 && suf.find('-') != std::string::npos) {
                c.decoder.wpm_min = std::atoi(suf.c_str() + 3);
                c.decoder.wpm_max = std::atoi(suf.c_str() + suf.find('-') + 1);
            }
            else { std::fprintf(stderr, "unknown bell switch: %s\n", suf.c_str()); std::exit(2); }
            at = next;
        }
        return std::make_unique<SkimBellBackend>(c);
    }
    CwDecoderConfig cfg;
    const bool v3 = name.rfind("v3", 0) == 0;
    cfg = v3 ? cw_preset_v3() : cw_preset_v2();
    if (name.size() > 3 && name[2] == ':') cfg.envelope.narrow_fc = std::atof(name.c_str() + 3);
    cfg = cw_config_from_env(cfg, v3 ? 3 : 2);          // SKIM_V2_* / SKIM_V3_* switches
    return v3 ? skim_decode_cw_v3_new(cfg, nullptr) : skim_decode_cw_v2_new(cfg, nullptr);
}

// The over for seed `seed`: with `noise_only_s` > 0 no signal at all, that
// much noise (or bed) only.
Result run_over(const Options &o, const SkimDecodeBackend &backend, double wpm, double foff,
                double snr500, int seed, const std::vector<float> &bed) {
    Result r;
    std::mt19937 rng(1000 + seed);                    // the text, the keying and the noise
    std::mt19937 rng_ch(5000 + seed);                 // fading and impulses: their own stream
    std::vector<float> env;
    if (o.noise_only_s > 0) {
        env.assign(static_cast<size_t>(o.noise_only_s * o.rate), 0.0f);
    } else {
        r.ref = "VVV " + payload(rng);
        env = envelope(r.ref, wpm, o.rate, o.jitter, o.lead_s, o.edge_ms, rng);
        env.resize(env.size() + static_cast<size_t>(o.tail_s * o.rate), 0.0f);
    }

    // the wideband impulse repair before the filter bank (--repair)
    ImpulseRepair::Config rc;
    rc.thr_db = o.repair_thr_db;
    rc.split_db = o.repair_split_db;
    rc.hold_off_ms = o.repair_holdoff_ms;
    rc.order = o.repair_order;
    auto ch = SkimChannelizer::create_ex(o.rate, 125.0, o.passband, o.taps, o.repair ? &rc : nullptr);
    if (!ch) { std::fprintf(stderr, "bad channelizer geometry\n"); std::exit(2); }
    auto dec = backend.channel_new(ch->out_rate());
    const ImpulseRepair *repair = ch->repair();
    std::vector<float> wch(4096);           // the channel frames' repair weights
    const double enbw = channel_enbw(o);
    const double chan_hz = ch->offset_hz(o.channel);

    // noise: synthetic N0 = 1/C/N0 with a unit tone, or the bed's own floor
    const double cn0 = std::pow(10.0, (snr500 + 27.0) / 10.0);
    double amp = 1.0, sigma = std::sqrt(o.rate / cn0 / 2.0);
    size_t bed_at = 0;
    if (!bed.empty()) {
        const size_t frames = bed.size() / 2;
        if (frames < env.size()) { std::fprintf(stderr, "bed shorter than the over\n"); std::exit(2); }
        const size_t room = frames - env.size();
        bed_at = room ? (static_cast<size_t>(seed) * 7919u * static_cast<size_t>(o.rate / 10)) % room : 0;
        // the bed's noise in this channel: the median of 0.25 s block powers
        auto probe = SkimChannelizer::create_ex(o.rate, 125.0, o.passband, o.taps);
        std::vector<float> cb(2 * 8192);
        std::vector<double> blocks;
        double acc = 0;
        size_t cnt = 0;
        const size_t blk = static_cast<size_t>(0.25 * ch->out_rate());
        for (size_t at = 0; at < std::min(frames, static_cast<size_t>(20 * o.rate)); at += 1024) {
            const size_t n = std::min<size_t>(1024, frames - at);
            probe->push(bed.data() + 2 * at, static_cast<unsigned>(n));
            const unsigned got = probe->read(o.channel, cb.data(), 8192);
            for (unsigned k = 0; k < got; k++) {
                acc += cb[2 * k] * cb[2 * k] + cb[2 * k + 1] * cb[2 * k + 1];
                if (++cnt == blk) { blocks.push_back(acc / cnt); acc = 0; cnt = 0; }
            }
        }
        std::nth_element(blocks.begin(), blocks.begin() + blocks.size() / 2, blocks.end());
        const double pc = blocks.empty() ? 1e-12 : blocks[blocks.size() / 2];
        const double n0 = pc / enbw;
        amp = std::sqrt(cn0 * n0);
        sigma = 0.0;
        dec->set_floor(pc);
    } else {
        dec->set_floor(2.0 * sigma * sigma * enbw / o.rate);
    }
    std::normal_distribution<double> g(0.0, sigma > 0 ? sigma : 1.0);

    // fading paths (delayed copies of the keyed tone)
    std::vector<std::vector<cplx>> gain;
    std::vector<size_t> delay;
    for (const Path &p : o.paths) {
        gain.push_back(fading_gain(p, env.size(), o.rate, rng_ch));
        delay.push_back(static_cast<size_t>(std::lround(p.delay_ms * 1e-3 * o.rate)));
    }

    // impulses: single samples (delta), or a waveform for the shaped kinds
    std::vector<std::pair<size_t, cplx>> imps;
    std::vector<cplx> impw;
    const bool shaped = o.imp_rate > 0 && (o.imp_shape != kImpDelta || o.imp_crash);
    if (shaped) {
        impw.assign(env.size(), cplx());
        const double rms = sigma > 0 ? sigma * std::sqrt(2.0) : 1.0;
        const double a = rms * std::pow(10.0, o.imp_db / 20.0);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        // The receiver's pulse response: a low-pass to ±0.45 of the rate,
        // a Hamming-windowed sinc over ±3 ms at 8 kHz, peak 1.
        auto h = [](double x) {
            if (std::fabs(x) >= kImpHalf) return 0.0;
            const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(2 * SKIM_PI * kImpFc * x) / (2 * SKIM_PI * kImpFc * x);
            return s * (0.54 + 0.46 * std::cos(SKIM_PI * x / kImpHalf));
        };
        auto add_rx = [&](double t, cplx c) {          // one receiver-shaped pulse at t seconds
            const double centre = t * o.rate;
            const long n0 = static_cast<long>(std::floor(centre));
            const double d = centre - static_cast<double>(n0);
            for (int k = -kImpHalf; k <= kImpHalf; k++) {
                const long idx = n0 + k;
                if (idx < 0 || idx >= static_cast<long>(impw.size())) continue;
                impw[static_cast<size_t>(idx)] += c * h(static_cast<double>(k) - d);
            }
        };
        auto add_sferic = [&](double t, double peak) {  // 2–4 copies, 0.5–3 ms apart
            const int copies = 2 + static_cast<int>(u(rng_ch) * 3.0);
            for (int i = 0; i < copies; i++) {
                if (i) t += (0.5 + 2.5 * u(rng_ch)) * 1e-3;
                const double ai = i ? peak * (0.3 + 0.7 * u(rng_ch)) : peak;
                add_rx(t, std::polar(ai, 2 * SKIM_PI * u(rng_ch)));
            }
        };
        const double dur = static_cast<double>(env.size()) / o.rate;
        double t = 0;
        while (true) {
            t += o.imp_periodic ? (1.0 / o.imp_rate) * (1.0 + 0.02 * (u(rng_ch) - 0.5))
                                : -std::log(1.0 - u(rng_ch)) / o.imp_rate;
            if (t >= dur) break;
            const double peak = a * (0.5 + u(rng_ch));
            if (o.imp_crash) {
                // A flash: 3–6 return strokes 40–60 ms apart, each a sferic,
                // and 10–30 weaker in-cloud pulses (−5 to −20 dB) over it.
                const int strokes = 3 + static_cast<int>(u(rng_ch) * 4.0);
                double ts = t;
                for (int s = 0; s < strokes; s++) {
                    if (s) ts += (40.0 + 20.0 * u(rng_ch)) * 1e-3;
                    add_sferic(ts, a * (0.5 + u(rng_ch)));
                }
                const int weak = 10 + static_cast<int>(u(rng_ch) * 21.0);
                for (int w = 0; w < weak; w++)
                    add_rx(t + (ts - t) * u(rng_ch), std::polar(a * std::pow(10.0, -(5.0 + 15.0 * u(rng_ch)) / 20.0),
                                                                2 * SKIM_PI * u(rng_ch)));
            } else if (o.imp_shape == kImpSferic) {
                add_sferic(t, peak);
            } else {
                add_rx(t, std::polar(peak, 2 * SKIM_PI * u(rng_ch)));
            }
        }
    } else if (o.imp_rate > 0) {
        const double rms = sigma > 0 ? sigma * std::sqrt(2.0) : 1.0;
        const double a = rms * std::pow(10.0, o.imp_db / 20.0);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double t = 0;
        while (true) {
            t += o.imp_periodic ? (1.0 / o.imp_rate) * (1.0 + 0.02 * (u(rng_ch) - 0.5))
                                : -std::log(1.0 - u(rng_ch)) / o.imp_rate;
            const size_t at = static_cast<size_t>(t * o.rate);
            if (at >= env.size()) break;
            imps.push_back({at, std::polar(a * (0.5 + u(rng_ch)), 2 * SKIM_PI * u(rng_ch))});
        }
    }
    size_t next_imp = 0;
    std::poisson_distribution<int> classa(o.classa_a > 0 ? o.classa_a : 1.0);
    const double clip = (sigma > 0 ? sigma * std::sqrt(2.0) : 1.0) * std::pow(10.0, o.clip_db / 20.0);

    const size_t pre_n = o.preamble_db > 0 ? static_cast<size_t>((o.lead_s + 40 * 1.2 / wpm) * o.rate) : 0;
    const double pre_gain = o.preamble_db > 0 ? std::pow(10.0, -o.preamble_db / 20.0) : 1.0;
    const double w0 = 2 * SKIM_PI * (chan_hz + foff) / o.rate;
    // The target's phase beyond w0·k (--drift, --chirp): a linear ramp, and
    // at each key-down an offset decaying from its onset.
    std::vector<double> xph;
    if (o.drift_hz_s != 0.0 || o.chirp_hz != 0.0) {
        xph.assign(env.size(), 0.0);
        double acc = 0.0, since = 1e300;
        const double tau = o.chirp_ms * 1e-3 * o.rate;
        for (size_t k = 0; k < env.size(); k++) {
            if (env[k] > 0.0f && (k == 0 || env[k - 1] == 0.0f)) since = 0.0;
            double f = o.drift_hz_s * static_cast<double>(k) / o.rate;
            if (o.chirp_hz != 0.0 && since < 50.0 * tau) f += o.chirp_hz * std::exp(-since / tau);
            xph[k] = acc;
            acc += 2 * SKIM_PI * f / o.rate;
            since += 1.0;
        }
    }

    // other stations (--qrm): keyed CW in other channels, at least two
    // channels away, each with its own text, speed and level
    struct Qrm { KeyingRender key; double amp, w; };
    std::vector<Qrm> qrm;
    // envelope()'s edge filter, for the stations' keying
    std::vector<float> qw;
    double qsum = 0;
    {
        const int L = std::max(1, static_cast<int>(o.edge_ms * 1e-3 * o.rate));
        if (L >= 3) {
            qw.resize(L);
            for (int i = 0; i < L; i++) {
                qw[i] = static_cast<float>(0.5f - 0.5f * std::cos(2 * SKIM_PI * i / (L - 1)));
                qsum += qw[i];
            }
        }
    }
    if (o.qrm_n > 0) {
        std::mt19937 rq(9000 + seed);
        std::uniform_real_distribution<double> uq(0.0, 1.0);
        const unsigned m = ch->count();
        for (int q = 0; q < o.qrm_n; q++) {
            unsigned c = 0;
            for (int tries = 0; tries < 1000; tries++) {
                c = static_cast<unsigned>(rq() % m);
                if (std::fabs(ch->offset_hz(c) - chan_hz) >= 2.5 * 125.0) break;
            }
            const double f = ch->offset_hz(c) + 60.0 * (uq(rq) - 0.5);
            const double lvl = o.qrm_db - o.qrm_range * uq(rq);
            const double wq = 15.0 + 20.0 * uq(rq);
            Qrm s;
            size_t len = 0;
            while (len < env.size()) {                // over after over, to the end
                const auto r = envelope_runs(payload(rq), wq, o.rate, o.jitter, 0.3 + uq(rq), o.edge_ms, rq);
                for (const auto &p : r) len += static_cast<size_t>(p.first);
                s.key.runs.insert(s.key.runs.end(), r.begin(), r.end());
            }
            s.amp = amp * std::pow(10.0, (lvl - snr500) / 20.0);
            s.w = 2 * SKIM_PI * f / o.rate;
            qrm.push_back(std::move(s));
        }
    }
    // a second station in the target's own channel (--cochannel): its own
    // text and speed, cochannel_hz from the target, cochannel_db over it
    if (o.cochannel) {
        std::mt19937 rc(9500 + seed);
        std::uniform_real_distribution<double> uc(0.0, 1.0);
        const double wc = 15.0 + 20.0 * uc(rc);
        Qrm s;
        size_t len = 0;
        while (len < env.size()) {
            const auto r = envelope_runs(payload(rc), wc, o.rate, o.jitter, 0.3 + uc(rc), o.edge_ms, rc);
            for (const auto &p : r) len += static_cast<size_t>(p.first);
            s.key.runs.insert(s.key.runs.end(), r.begin(), r.end());
        }
        s.amp = amp * std::pow(10.0, o.cochannel_db / 20.0);
        s.w = 2 * SKIM_PI * (chan_hz + foff + o.cochannel_hz) / o.rate;
        qrm.push_back(std::move(s));
    }

    std::string out;
    std::vector<float> iq(2 * 512), cbuf(2 * 4096);
    double ph = 0.0;
    for (size_t at = 0; at < env.size(); at += 512) {
        const size_t n = std::min<size_t>(512, env.size() - at);
        for (size_t i = 0; i < n; i++) {
            const size_t k = at + i;
            cplx s;
            if (o.paths.empty()) {
                s = std::polar(amp * env[k], xph.empty() ? ph : ph + xph[k]);
            } else {
                for (size_t pi = 0; pi < o.paths.size(); pi++) {
                    if (k < delay[pi]) continue;
                    const size_t kd = k - delay[pi];
                    s += std::sqrt(o.paths[pi].power) * gain[pi][k] *
                         std::polar(amp * env[kd], w0 * static_cast<double>(kd) + (xph.empty() ? 0.0 : xph[kd]));
                }
            }
            for (Qrm &q : qrm) {
                const float e = q.key.next(qw, qsum);
                if (e > 0.0f) s += std::polar(q.amp * e, q.w * static_cast<double>(k));
            }
            const double ns = k < pre_n ? pre_gain : 1.0;
            double re = s.real(), im = s.imag();
            if (bed.empty()) {
                // Middleton Class A: m ~ Poisson(A) impulsive sources at once,
                // variance (m/A + Γ)/(1 + Γ) of the Gaussian's, the same mean.
                const double ca = o.classa_a > 0
                    ? std::sqrt((classa(rng) / o.classa_a + o.classa_gamma) / (1.0 + o.classa_gamma)) : 1.0;
                re += ns * ca * g(rng);
                im += ns * ca * g(rng);
            } else {
                re += bed[2 * (bed_at + k)];
                im += bed[2 * (bed_at + k) + 1];
            }
            while (next_imp < imps.size() && imps[next_imp].first == k) {
                re += imps[next_imp].second.real();
                im += imps[next_imp].second.imag();
                next_imp++;
            }
            if (!impw.empty()) {
                re += impw[k].real();
                im += impw[k].imag();
            }
            if (o.clip_db > 0) {
                // ADC saturation: the complex magnitude limited, everything in it.
                const double mag = std::hypot(re, im);
                if (mag > clip) {
                    re *= clip / mag;
                    im *= clip / mag;
                }
            }
            iq[2 * i] = static_cast<float>(re);
            iq[2 * i + 1] = static_cast<float>(im);
            ph += w0;
        }
        ch->push(iq.data(), static_cast<unsigned>(n));
        const unsigned got = ch->read(o.channel, cbuf.data(), 4096, wch.data());
        if (repair && got) dec->set_repair_weights(wch.data(), got);
        SkimDecode d{};
        if (got && dec->process(cbuf.data(), got, &d)) out += d.text;
    }
    if (repair)
        std::fprintf(stderr, "repair: seed %d gaps %ld rejected %ld narrow %ld repaired %.2f %% impulsive %.1f %% strong %.1f\n",
                     seed, repair->gaps(), repair->rejected(), repair->narrow(),
                     100.0 * repair->repaired() / std::max(1.0, repair->output()), 100.0 * repair->impulsive_frames(), repair->strong_bins());
    r.hyp = normalise(out);
    if (o.noise_only_s > 0) {
        r.dist = static_cast<int>(std::count_if(r.hyp.begin(), r.hyp.end(), [](char c) { return c != ' '; }));
        return r;
    }
    const std::string pay = r.ref.substr(4);
    r.ref_len = r.ref.size();
    r.pay_len = pay.size();
    r.dist = lev(r.hyp, r.ref, false);
    r.pay_dist = std::min(lev(pay, r.hyp, true), static_cast<int>(pay.size()));
    return r;
}

// --- the gates -----------------------------------------------------------------------------

struct Totals { size_t len = 0; int dist = 0, failed = 0, overs = 0; };

Totals measure(Options o, double wpm, double snr, int seeds, bool payload_only) {
    o.seeds = seeds;
    auto be = make_backend(o.cfg);
    std::vector<float> bed;
    Totals t;
    for (int s = o.seed0; s < o.seed0 + seeds; s++) {
        const Result r = run_over(o, *be, wpm, o.offset[0], snr, s, bed);
        const size_t n = payload_only ? r.pay_len : r.ref_len;
        const int d = payload_only ? r.pay_dist : std::min<int>(r.dist, static_cast<int>(n));
        t.len += n;
        t.dist += d;
        t.failed += d > 0.3 * n;
        t.overs++;
    }
    return t;
}

int run_gate(const Options &base, const std::string &which) {
    int fails = 0;
    auto check = [&](const char *name, bool ok, const std::string &what) {
        std::printf("%s %s: %s\n", ok ? "  ok  " : "  FAIL", name, what.c_str());
        fails += !ok;
    };
    char buf[160];
    Options o = base;
    o.offset = {10};
    if (which == "quick" || which == "all") {
        // clean copy behind the channelizer, start transient out of the way
        o.lead_s = 12.0;
        Totals t = measure(o, 25, 14, 5, true);
        std::snprintf(buf, sizeof buf, "payload CER %.1f %% (limit 2 %%)", 100.0 * t.dist / t.len);
        check("clean copy, 25 WPM, SNR500 14 dB", 100.0 * t.dist / t.len <= 2.0, buf);
        // copy at the channel edge: a tone midway between two channels
        Options e = o;
        e.offset = {62.5};
        t = measure(e, 25, 10, 5, true);
        std::snprintf(buf, sizeof buf, "payload CER %.1f %% (limit 5 %%)", 100.0 * t.dist / t.len);
        check("channel edge (62.5 Hz), 25 WPM, SNR500 10 dB", 100.0 * t.dist / t.len <= 5.0, buf);
        // slow fading, ITU-R F.1487 mid-latitude quiet
        Options f = o;
        parse_fade("itu:good", f.paths);
        t = measure(f, 25, 14, 5, true);
        std::snprintf(buf, sizeof buf, "payload CER %.1f %% (limit 10 %%)", 100.0 * t.dist / t.len);
        check("ITU good fading, 25 WPM, SNR500 14 dB", 100.0 * t.dist / t.len <= 10.0, buf);
        // noise alone must not decode into text
        Options n = o;
        n.noise_only_s = 120.0;
        auto be = make_backend(n.cfg);
        std::vector<float> bed;
        int chars = 0;
        for (int s = 0; s < 3; s++) chars += run_over(n, *be, 25, 10, 0, s, bed).dist;
        const double per_hour = chars / (3 * n.noise_only_s) * 3600.0;
        std::snprintf(buf, sizeof buf, "%.0f characters per hour (limit 600)", per_hour);
        check("noise only, 6 minutes", per_hour <= 600.0, buf);
    }
    if (which == "startup" || which == "all") {
        // the channel starts on noise and keying follows 0.5 s later: the
        // squelch must not open on the channelizer's zero-history start
        o.lead_s = 0.5;
        Totals t = measure(o, 35, 14, 20, false);
        std::snprintf(buf, sizeof buf, "%d of %d overs failed, CER %.1f %% (limit: none failed, 2 %%)",
                      t.failed, t.overs, 100.0 * t.dist / t.len);
        check("channel start, 35 WPM, SNR500 14 dB", t.failed == 0 && 100.0 * t.dist / t.len <= 2.0, buf);
    }
    std::printf("=== %d failures ===\n", fails);
    return fails ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    if (!parse_args(argc, argv, o)) return 2;
    if (static_cast<unsigned>(o.rate / 125.0 + 0.5) % 2) {
        std::fprintf(stderr, "--rate must be an even multiple of 125 Hz\n");
        return 2;
    }
    if (!o.gate.empty()) return run_gate(o, o.gate);
    if (o.fade_check) {
        for (double b : {0.1, 0.5, 1.0, 3.0, 10.0, 30.0}) fade_check({1.0, b, 0.0, 0.0});
        return 0;
    }

    std::vector<float> bed;
    if (!o.bed.empty()) {
        bed = read_bed(o.bed);
        if (bed.empty()) { std::fprintf(stderr, "cannot read %s\n", o.bed.c_str()); return 2; }
    }
    auto be = make_backend(o.cfg);
    if (o.noise_only_s > 0) {
        long chars = 0;
        for (int s = o.seed0; s < o.seed0 + o.seeds; s++) {
            const Result r = run_over(o, *be, o.wpm[0], o.offset[0], 0.0, s, bed);
            chars += r.dist;
            std::printf("%s noise %d %.0f s %d chars\n", o.cfg.c_str(), s, o.noise_only_s, r.dist);
            if (o.show) std::printf("   hyp: %s\n", r.hyp.c_str());
        }
        std::printf("%s noise total: %.0f characters per hour\n", o.cfg.c_str(),
                    chars / (o.seeds * o.noise_only_s) * 3600.0);
        return 0;
    }
    for (double wpm : o.wpm)
        for (double off : o.offset)
            for (double snr : o.snr)
                for (int s = o.seed0; s < o.seed0 + o.seeds; s++) {
                    const Result r = run_over(o, *be, wpm, off, snr, s, bed);
                    std::printf("%s %.0f %.1f %.1f %d %zu %d %zu %d\n", o.cfg.c_str(), wpm, off, snr, s,
                                r.ref_len, r.dist, r.pay_len, r.pay_dist);
                    if (o.show) std::printf("   ref: %s\n   hyp: %s\n", r.ref.c_str(), r.hyp.c_str());
                    std::fflush(stdout);
                }
    return 0;
}
