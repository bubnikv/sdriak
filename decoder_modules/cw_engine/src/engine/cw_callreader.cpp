/*
 * cw_callreader.cpp — see cw_callreader.h. Part of skimmer-for-linux.
 * GPL-3.0-or-later. */
#include "cw_callreader.h"
#include "callsign.h"
#include "cw_debug.h"
#include "cw_discriminator.h"
#include "oracle.h"

static bool looks_like_call(const char *w) {
    bool dig = false, let = false;
    unsigned n = 0;
    for (const char *p = w; *p; p++, n++) {
        dig |= std::isdigit(static_cast<unsigned char>(*p)) != 0;
        let |= std::isalpha(static_cast<unsigned char>(*p)) != 0;
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '/')
            return false;
    }
    return dig && let && n >= 3 && n <= SkimCall::kMaxLength;
}

static void add_candidate(SkimCall *cand, unsigned *nc, const char *w) {
    if (*nc >= CwCallReader::kMaxCandidates || !looks_like_call(w))
        return;
    if (std::any_of(cand, cand + *nc, [&](const SkimCall &c) { return c == w; }))
      return;
    cand[(*nc)++].assign(w);
}

static void add_near_calls(const SkimCallsignDict *d, SkimCall *cand, unsigned *nc, const char *w) {
    if (strlen(w) < 3 || !SkimCall::fits(w))
        return;
    SkimCall near[MAX_NEAR_CALLS];
    const unsigned nn = d ? d->near(w, near, MAX_NEAR_CALLS) : 0;
    for (unsigned i = 0; i < nn; i++) { add_candidate(cand, nc, near[i].c_str()); }
}

// The forced-alignment score of `call` over the window, minus the free
// reading's (≤ 0 by construction, NEG when the call does not fit).
static double score_call(const double *llr_sum, const CwCallAlignment &k, const float *prefix,
                       const float *suffix, float *scratch1, float *scratch2, const char *call) {
    CwSeg seg[MAX_CALL_SEGMENTS];
    const unsigned K = call_segments(call, seg);
    if (!K)
        return -1e9;
    const double v = k.align_call(llr_sum, seg, K, prefix, suffix, scratch1, scratch2);
    return v > CwCallAlignment::kUnreachable ? v : -1e9;
}

void CwCallReader::read(const CwReadView &v, const CwCallReaderConfig &cfg) {
    const uint64_t t = v.t;
    unsigned ns = static_cast<unsigned>(kWindowSeconds * v.rate);
    ns = std::min(ns, static_cast<unsigned>(t - v.lattice->t0()));
    ns = std::min(ns, static_cast<unsigned>(CwLattice::kSize) - 16);
    const double h = v.priors.dit / kSlotsPerDit;
    if (v.priors.dit <= 0 || ns < static_cast<unsigned>(2.0 * v.rate) || h < 1.0)
        return;
    const uint64_t t0 = t - ns;

    // candidates from the text whose audio lies in the window
    SkimCall cand[kMaxCandidates];
    unsigned nc = 0;
    SkimCall tok[24];
    unsigned nt = 0;
    bool calling = false;
    SkimCall cur;                           // the token being read
    for (unsigned i = 0; i <= m_text_n && nt < std::size(tok); i++) {
        const bool end = i == m_text_n || m_text[i].c == ' ';
        if (!end && m_text[i].at >= t0) {
            cur.push_back(m_text[i].c);         // a longer token is cut short
            continue;
        }
        if (end && !cur.empty()) {
            tok[nt++] = cur;
            // "CQ" glued to what follows ("CQCE", "CQDE", "CQT") calls too: the
            // free text tears and glues at word gaps, and a call the audio
            // confirmed without its CQ never reaches the feed (rec 2: SA6AUT
            // read 210 times as "CQCE SA6A T", not one spot).
            if (cur == "TEST" || cur == "QRZ" ||
                (cur.view().compare(0, 2, "CQ") == 0 && cur.size() <= 8)) {
                calling = true;
            }
        }
        cur.clear();
    }
    for (unsigned i = 0; i + 1 < nt; i++) {             // torn "C Q"
        if (tok[i] == "C" && tok[i + 1] == "Q") { calling = true; }
    }
    for (unsigned i = 0; i < nt; i++) {
        if (v.dict && v.dict->has(tok[i].c_str())) { add_candidate(cand, &nc, tok[i].c_str()); }
    }
    for (unsigned i = 0; i < nt; i++) {
        add_near_calls(v.dict, cand, &nc, tok[i].c_str());
        char j[48];
        skim_strlcpy(j, tok[i].c_str(), sizeof(j));
        for (unsigned k = i + 1; k < std::min(nt, i + 3); k++) {   // torn: 2-3 joined
            skim_strlcat(j, tok[k].c_str(), sizeof(j));
            if (strlen(j) >= 3 && strlen(j) <= 10) { add_near_calls(v.dict, cand, &nc, j); }
        }
        const unsigned L = static_cast<unsigned>(tok[i].size());
        if (L >= 7) {                                     // glued: inner calls
            for (unsigned a = 0; a < L; a++) {
                for (unsigned n = 4; n <= 8 && a + n <= L; n++) {
                    const SkimCall sub(tok[i].view().substr(a, n));
                    if (v.dict && v.dict->has(sub.c_str())) { add_candidate(cand, &nc, sub.c_str()); }
                }
            }
        }
    }
    for (unsigned i = 0; i < kHitSlots; i++) {
        if (!m_confirmed[i].call.empty() && t - m_confirmed[i].last_read < static_cast<uint64_t>(kHoldSeconds * v.rate)) {
            add_candidate(cand, &nc, m_confirmed[i].call.c_str());
        }
    }
    if (calling) { m_calling_seen = t + 1; }          // text CQ, even with no call
    if (!nc)
        return;

    // the window's LLR, a slot every third of a dit (integrate and dump)
    const CwLatticeRings *Lt = v.lattice->rings();
    const int N = static_cast<int>(ns / h);
    if (N < 32)
        return;
    std::vector<double> c_v(static_cast<size_t>(2 * (N + 1)));
    double *llr_sum = c_v.data();
    double *llr_sum_rev = llr_sum + N + 1;
    const double base = Lt->llr_sum[t0 & CwLattice::kMask];
    for (int j = 0; j <= N; j++) {
        const uint64_t tj = t0 + static_cast<uint64_t>(lround(j * h));
        llr_sum[j] = Lt->llr_sum[std::min(tj, t) & CwLattice::kMask] - base;
    }
    for (int u = 0; u <= N; u++) { llr_sum_rev[u] = llr_sum[N] - llr_sum[N - u]; }
    std::vector<float> buf_v(static_cast<size_t>(7 * (N + 1)));
    float *buf = buf_v.data();
    float *prefix = buf, *prefix_rev = buf + (N + 1), *suffix = buf + 2 * (N + 1);
    float *scratch1 = buf + 3 * (N + 1), *scratch2 = buf + 4 * (N + 1),
          *scratch3 = buf + 5 * (N + 1), *scratch4 = buf + 6 * (N + 1);
    const CwCallAlignment k(kSlotsPerDit, N);
    const double F = k.free_reading(llr_sum, prefix, scratch1, scratch2, scratch3, scratch4);
    k.free_reading(llr_sum_rev, prefix_rev, scratch1, scratch2, scratch3, scratch4);
    for (int u = 0; u <= N; u++) { suffix[u] = prefix_rev[N - u]; }
    // A call must sit 4 dits inside the window at both ends: one cut
    // by the window's edge fits its own head or tail as a shorter call
    // (OK1HR out of OK1HRA, the A not in the window yet).
    const int edge = static_cast<int>(4.0 * kSlotsPerDit);
    for (int u = 0; u <= N; u++) {
        if (u < edge) { prefix[u] = CwLattice::kNoScore; }
        if (u > N - edge) { suffix[u] = CwLattice::kNoScore; }
    }

    double best = -1e9;
    int bi = -1;
    double sc[kMaxCandidates];
    for (unsigned i = 0; i < nc; i++) {
        sc[i] = score_call(llr_sum, k, prefix, suffix, scratch1, scratch2, cand[i].c_str()) - F;
        if (sc[i] > best) {
            best = sc[i];
            bi = static_cast<int>(i);
        }
    }
    double nb = -1e9;
    const char *nbc = "-";
    SkimCall near[MAX_NEAR_CALLS];
    if (bi >= 0 && best >= cfg.min_score) {
        // the control: every dictionary call two edits from the winner, and
        // every other candidate — a busted twin must lose by SKIM_V3_KWS_SEP
        for (unsigned i = 0; i < nc; i++) {
            if (static_cast<int>(i) != bi && sc[i] > nb) {
                nb = sc[i];
                nbc = cand[i].c_str();
            }
        }
        const unsigned nn = v.dict ? v.dict->near(cand[bi].c_str(), near, MAX_NEAR_CALLS) : 0;
        unsigned done = 0;
        for (unsigned i = 0; i < nn && done < kMaxNearControls; i++) {
            if (near[i] == cand[bi])
                continue;
            if (std::any_of(cand, cand + nc, [&](const SkimCall &c) { return c == near[i]; }))
              continue;
            const double v = score_call(llr_sum, k, prefix, suffix, scratch1, scratch2, near[i].c_str()) - F;
            done++;
            if (v > nb) {
                nb = v;
                nbc = near[i].c_str();
            }
        }
    }
    double cq = -1e9;
    const bool found = bi >= 0 && best >= cfg.min_score && best - nb >= cfg.min_margin;
    // CQ evidence is kept per channel for kCqSeconds: the window that confirms
    // the call often starts after the over's "CQ CQ DE" (rec 2: 21 stations
    // R spotted, the reader confirmed and the feed never saw — every one for
    // want of a calling context).
    if (bi >= 0 && !calling) {
        cq = std::max(score_call(llr_sum, k, prefix, suffix, scratch1, scratch2, "CQ"),
                      score_call(llr_sum, k, prefix, suffix, scratch1, scratch2, "TEST")) - F;
        calling = cq >= -3.0;
    }
    if (calling) { m_calling_seen = t + 1; }
    calling = m_calling_seen && t + 1 - m_calling_seen <= static_cast<uint64_t>(kCqSeconds * v.rate);
    if (cfg.debug && bi >= 0) {
        std::fprintf(stderr, "kws: %.2f %.1f %s dC %.1f next %s %.1f cq %d(%.1f) cands %u%s\n",
                   static_cast<double>(t) / v.rate, v.freq_hz, cand[bi].c_str(), best, nbc, nb, calling, cq,
                   nc, found ? " HIT" : "");
    }
    if (!found)
        return;

    // hit memory: separate windows (no shared audio) count as separate reads
    int hi = -1, old = 0;
    for (unsigned i = 0; i < kHitSlots; i++) {
        if (m_confirmed[i].call == cand[bi]) { hi = static_cast<int>(i); }
        if (m_confirmed[i].last_read < m_confirmed[old].last_read) { old = static_cast<int>(i); }
    }
    if (hi < 0) {
        hi = old;
        m_confirmed[hi] = {};
        m_confirmed[hi].call = cand[bi];
    }
    if (m_confirmed[hi].reads == 0 || t - m_confirmed[hi].last_read >= static_cast<uint64_t>(ns)) {
        m_confirmed[hi].reads++;
        m_confirmed[hi].last_read = t;
    }
    m_confirmed[hi].best_score = std::max(m_confirmed[hi].best_score, best);
    const bool sure = m_confirmed[hi].reads >= 2 || (best >= -2.0 && best - nb >= 5.0);
    const bool due = m_confirmed[hi].last_sent == 0 ||
                         t - m_confirmed[hi].last_sent >= static_cast<uint64_t>(kRepeatSeconds * v.rate);
    if (sure && due && !m_fix_phrase[0]) {
        std::snprintf(m_fix_phrase, sizeof(m_fix_phrase), "%s%c%s%c ",
                   calling ? "\x01" "CQ\x02 " : "", SkimCallsign::kFixOpen, cand[bi].c_str(),
                   SkimCallsign::kFixClose);
        m_confirmed[hi].last_sent = t;
        CW_DBG(*v.trace, "[kws: %s dC %.1f sep %.1f reads %u%s]\n", cand[bi].c_str(), best, best - nb,
                m_confirmed[hi].reads, calling ? " calling" : "");
    }
}

// SKIM_ORACLE_MODE=kws (oracle.h) — measurement only, plan step 5: is R's
// call in the audio at all, found without the decoder's words? Every
// kHopSeconds on the channel that owns one of R's stations, the last
// kWindowSeconds of the soft path's envelope become llr on the window's own
// levels (the soft path's two views, levels from the window's quantiles,
// so the decoder's gate and trackers play no part), and the call is
// searched for anywhere in it: a free reading, a word gap, the call's exact
// elements, a word gap, a free reading — against the best free reading of the whole window. The
// clock is R's WPM at three scales, the best kept; character gaps inside
// the call may stretch to 8 dits (torn calls). The same search for the
// dictionary's calls near R's (what a wrong read would be) and for calls of
// R's other stations (what a false alarm would be) is the control. One
// "okws:" line per window; decoding is untouched.

void CwKwsProbe::eval(const CwReadView &v, unsigned pre_head, int N, bool dump) {
    if (!v.oracle)
        return;
    const double t_s = v.oracle->time(static_cast<double>(pre_head) / v.rate);
    const std::optional<SkimOracleHit> hit = v.oracle->at(v.freq_hz, 0.25 * v.rate, t_s);
    if (!hit || hit->call.empty() || hit->wpm <= 0)
        return;
    const SkimOracleHit &oh = *hit;
    SkimCall calls[1 + kNearCalls + kDecoys];
    unsigned nc = 0;
    calls[nc++] = oh.call;
    SkimCall near[MAX_NEAR_CALLS];
    const unsigned nn = v.dict ? v.dict->near(oh.call.c_str(), near, MAX_NEAR_CALLS) : 0;
    for (unsigned i = 0; i < nn && nc < 1 + kNearCalls; i++) {
        if (near[i] != oh.call) { calls[nc++] = near[i]; }
    }
    const unsigned near_end = nc;
    SkimCall dec[kDecoys];
    const unsigned nd = v.oracle->decoys(oh.call.c_str(), static_cast<unsigned>(t_s) ^ static_cast<unsigned>(v.freq_hz),
                                        dec, kDecoys);
    for (unsigned i = 0; i < nd; i++) { calls[nc++] = dec[i]; }

    // llr on the window's own levels: noise σ from the lower quartile
    // (Rayleigh), the mark level at the 90th percentile.
    const unsigned mask = 2047;
    std::vector<float> e_v(static_cast<size_t>(N)), srt_v(static_cast<size_t>(N));
    float *e = e_v.data(), *srt = srt_v.data();
    for (int i = 0; i < N; i++) {
        e[i] = m_ring[(m_written - static_cast<uint64_t>(N) + static_cast<uint64_t>(i)) & mask];
    }
    memcpy(srt, e, N * sizeof(float));
    std::sort(srt, srt + N);
    const double sig = std::max(srt[N / 4] / 0.7585, 1e-12);
    const double mu_s = 1.2533 * sig;
    const double mu_m = std::max<double>(srt[(9 * N) / 10], 1.5 * mu_s);
    const double mid = 0.5 * (mu_m + mu_s);
    std::vector<double> c_v(static_cast<size_t>(N) + 1), cr_v(static_cast<size_t>(N) + 1);
    double *llr_sum = c_v.data(), *llr_sum_rev = cr_v.data();
    llr_sum[0] = 0.0;
    for (int i = 0; i < N; i++) {
        const double x = (e[i] - mid) / (mu_m - mu_s);
        const double span = std::clamp(CwDiscriminator::kLlrGain * x, -CwDiscriminator::kLlrMax, CwDiscriminator::kLlrMax);
        const double z = e[i] / (mu_s / 1.2533);
        const double ray = z <= 1.0 ? -CwDiscriminator::kLlrMax
                                    : std::clamp(3.3 * (0.5 * z * z - log(z) - 2.89),
                                            -CwDiscriminator::kLlrMax, CwDiscriminator::kLlrMax);
        llr_sum[i + 1] = llr_sum[i] + 0.5 * span + 0.5 * ray;
    }
    for (int u = 0; u <= N; u++) { llr_sum_rev[u] = llr_sum[N] - llr_sum[N - u]; }
    if (dump) {
        std::string x;
        for (int i = 0; i < N; i++) {
            const double l = llr_sum[i + 1] - llr_sum[i];
            x += (l > 1.0 ? '#' : l > 0 ? '+' : l > -1.0 ? '-' : '.');
        }
        std::fprintf(stderr, "okwsx: %.2f %s\n", t_s, x.c_str());
    }

    std::vector<float> P_v(static_cast<size_t>(N) + 1), Pr_v(static_cast<size_t>(N) + 1), S_v(static_cast<size_t>(N) + 1),
        a_v(static_cast<size_t>(N) + 1), b_v(static_cast<size_t>(N) + 1), d3_v(static_cast<size_t>(N) + 1), d4_v(static_cast<size_t>(N) + 1);
    float *prefix = P_v.data(), *prefix_rev = Pr_v.data(),
          *suffix = S_v.data(), *scratch1 = a_v.data(),
          *scratch2 = b_v.data(), *scratch3 = d3_v.data(),
          *scratch4 = d4_v.data();
    static const double SC[3] = { 0.85, 1.0, 1.18 };
    double dbest[1 + kNearCalls + kDecoys];
    for (unsigned i = 0; i < nc; i++) { dbest[i] = -999.0; }
    double F1 = 0.0;
    int s1 = -1;
    const double dit0 = 1.2 * v.rate / oh.wpm;
    for (int si = 0; si < 3; si++) {
        const CwCallAlignment k(dit0 * SC[si], N);
        const double F = k.free_reading(llr_sum, prefix, scratch1, scratch2, scratch3, scratch4);
        k.free_reading(llr_sum_rev, prefix_rev, scratch1, scratch2, scratch3, scratch4);
        for (int t = 0; t <= N; t++) { suffix[t] = prefix_rev[N - t]; }
        for (unsigned i = 0; i < nc; i++) {
            CwSeg seg[MAX_CALL_SEGMENTS];
            const unsigned K = call_segments(calls[i].c_str(), seg);
            if (!K)
                continue;
            const double kw = k.align_call(llr_sum, seg, K, prefix, suffix, scratch1, scratch2);
            if (kw > CwCallAlignment::kUnreachable && kw - F > dbest[i]) {
                dbest[i] = kw - F;
                if (i == 0) { s1 = si; F1 = F; }
            }
        }
    }
    int bn = -1;
    for (unsigned i = 1; i < near_end; i++) {
        if (bn < 0 || dbest[i] > dbest[bn]) { bn = static_cast<int>(i); }
    }
    std::string gs;
    for (unsigned i = near_end; i < nc; i++) {
        { char t_[48]; std::snprintf(t_, sizeof(t_), " %s:%.1f", calls[i].c_str(), dbest[i]); gs += t_; }
    }
    std::fprintf(stderr, "okws: %.2f %.1f %s %.0f %d %d %.1f %.3f %.2f %.1f %s %.1f |%s\n",
               t_s, v.freq_hz, oh.call.c_str(), oh.wpm, N, s1, F1, F1 / N,
               20.0 * log10(mu_m / mu_s), dbest[0], bn >= 0 ? calls[bn].c_str() : "-",
               bn >= 0 ? dbest[bn] : -999.0, gs.c_str());
}

void CwKwsProbe::push(const CwReadView &v, unsigned pre_head, float ms, bool dump) {
    const int N = static_cast<int>(kWindowSeconds * v.rate);
    const unsigned hop = static_cast<unsigned>(kHopSeconds * v.rate);
    if (N + 1 > 2048 || !hop)
        return;
    if (m_ring.empty()) { m_ring.assign(2048, 0.0f); }
    m_ring[m_written++ & 2047] = ms;
    if (m_written >= static_cast<uint64_t>(N) && m_written % hop == 0) { eval(v, pre_head, N, dump); }
}
