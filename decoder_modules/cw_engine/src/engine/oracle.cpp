/*
 * oracle.c — see oracle.h. Measurement only (docs/WEAK-SIGNAL-PLAN.md, 0.1).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "oracle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// SDRIAK: upstream's load() (run at the first skim_oracle_modes()), building
// an oracle object instead of file-scope state.
std::shared_ptr<const SkimOracle> SkimOracle::from_env() {
    const char *path = std::getenv("SKIM_ORACLE");
    const char *mode = std::getenv("SKIM_ORACLE_MODE");
    if (!path || !path[0] || !mode || !mode[0])
        return nullptr;
    unsigned m = 0;
    std::string modes(mode);
    for (size_t at = 0; at <= modes.size();) {
        size_t comma = modes.find(',', at);
        if (comma == std::string::npos) { comma = modes.size(); }
        const std::string q = modes.substr(at, comma - at);
        at = comma + 1;
        if (q == "gate") { m |= SkimOracle::kGate; }
        else if (q == "clock") { m |= SkimOracle::kClock; }
        else if (q == "freq") { m |= SkimOracle::kFreq; }
        else if (q == "ghost") { m |= SkimOracle::kGhost; }
        else if (q == "gatesoft") { m |= SkimOracle::kSoftGate; }
        else if (q == "combine") { m |= SkimOracle::kCombine; }
        else if (q == "kws") { m |= SkimOracle::kKws; }
        else if (q == "all") {
            m |= SkimOracle::kGate | SkimOracle::kClock | SkimOracle::kFreq;
        } else if (q != "none") {
            std::fprintf(stderr, "SKIM_ORACLE_MODE: unknown \"%s\"\n", q.c_str());
            std::abort();
        }
    }
    std::shared_ptr<SkimOracle> o(new SkimOracle());
    const char *mp = std::getenv("SKIM_ORACLE_MINPROM");
    const double minprom = mp && mp[0] ? std::strtod(mp, nullptr) : 6.0;
    const char *fr = std::getenv("SKIM_REPLAY_FROM");
    o->m_from = fr && fr[0] ? std::strtod(fr, nullptr) : 0.0;

    FILE *f = fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "SKIM_ORACLE: cannot open %s\n", path);
        std::abort();
    }
    char line[512], call[64];
    unsigned skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        Interval iv;
        double snr, prom;
        if (sscanf(line, "%lf %lf %lf %lf %63s %lf %lf", &iv.t0, &iv.t1, &iv.hz,
                   &iv.wpm, call, &snr, &prom) != 7)
            continue;
        if (prom < minprom || iv.wpm <= 0) {
            skipped++;
            continue;
        }
        iv.call.assign(call);
        o->m_intervals.push_back(iv);
        o->m_maxlen = std::max(o->m_maxlen, iv.t1 - iv.t0);
    }
    fclose(f);
    std::sort(o->m_intervals.begin(), o->m_intervals.end(),
              [](const Interval &a, const Interval &b) { return a.t0 < b.t0; });
    o->m_modes = m;
    std::fprintf(stderr, "oracle: %s mode 0x%x — %u intervals, %u under %.1f dB prominence "
               "skipped\n", path, m, static_cast<unsigned>(o->m_intervals.size()), skipped, minprom);
    return o;
}

std::optional<SkimOracleHit> SkimOracle::at(double chan_hz, double half_hz, double t_s) const {
    const unsigned n = static_cast<unsigned>(m_intervals.size());
    if (!n)
        return std::nullopt;
    // First interval that may still cover t_s: t0 >= t_s - maxlen.
    unsigned lo = 0, hi = n;
    const double from = t_s - m_maxlen;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (m_intervals[mid].t0 < from) { lo = mid + 1; } else { hi = mid; }
    }
    double best = half_hz;
    std::optional<SkimOracleHit> hit;
    for (unsigned i = lo; i < n && m_intervals[i].t0 <= t_s; i++) {
        if (m_intervals[i].t1 < t_s)
            continue;
        const double d = fabs(m_intervals[i].hz - chan_hz);
        if (d <= best) {
            best = d;
            hit.emplace();
            hit->carrier_hz = m_intervals[i].hz;
            hit->wpm = m_intervals[i].wpm;
            hit->call = m_intervals[i].call;
        }
    }
    return hit;
}

unsigned SkimOracle::decoys(const char *call, unsigned seed, SkimCall *out, unsigned max) const {
    const unsigned nv = static_cast<unsigned>(m_intervals.size());
    unsigned n = 0;
    uint32_t h = seed * 2654435761u + 1u;
    for (unsigned tries = 0; nv && tries < 16 * max && n < max; tries++) {
        h = h * 1664525u + 1013904223u;
        const SkimCall &c = m_intervals[(h >> 8) % nv].call;
        bool dup = c == call;
        for (unsigned j = 0; j < n && !dup; j++) { dup = out[j] == c; }
        if (!dup) { out[n++] = c; }
    }
    return n;
}
