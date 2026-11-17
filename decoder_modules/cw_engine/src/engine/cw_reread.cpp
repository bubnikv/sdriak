// cw_reread.cpp — see cw_reread.h. Part of skimmer-for-linux. GPL-3.0-or-later.
#include "cw_reread.h"
#include "callsign.h"
#include "oracle.h"

// Best score of seg[0..K) laid on (A, A+N]: a free leading space, the
// sequence, a free trailing space. f/g: scratch, N + 1 floats each.
double CwWordReread::forced(const CwReadView &v, uint64_t A, unsigned N, const CwSeg *seg,
                             unsigned K, float *f, float *g) {
    const CwLatticeRings *L = v.lattice->rings();
    const auto llr_sum = [L, A](unsigned i) { return L->llr_sum[(A + i) & CwLattice::kMask]; };  // the llr sum at A + i
    for (unsigned t = 0; t <= N; t++) { f[t] = static_cast<float>(-(llr_sum(t) - llr_sum(0))); }
    for (unsigned k = 0; k < K; k++) {
        const CwSeg ty = seg[k];
        const double sign = is_mark(ty) ? 1.0 : -1.0;
        int dlo, dhi;
        v.priors.search_window(ty, &dlo, &dhi);
        const bool lz = m_oracle_loose && ty == CwSeg::CharSpace;
        if (lz) {
            int wlo, whi;
            v.priors.search_window(CwSeg::WordSpace, &wlo, &whi);
            dhi = std::max(dhi, whi);
        }
        for (unsigned t = 0; t <= N; t++) {
            float best = CwLattice::kNoScore;
            for (int d = dlo; d <= dhi && static_cast<unsigned>(d) <= t; d++) {
                const float p = f[t - d];
                if (p <= CwLattice::kNoScore)
                    continue;
                const double pen = lz ? std::max(v.lattice->duration_prior(v.priors, CwSeg::CharSpace, d),
                                            v.lattice->duration_prior(v.priors, CwSeg::WordSpace, d))
                                      : v.lattice->duration_prior(v.priors, ty, d);
                const float sc = p + static_cast<float>(sign * (llr_sum(t) - llr_sum(t - d)) + pen);
                if (sc > best) { best = sc; }
            }
            g[t] = best;
        }
        std::swap(f, g);
    }
    double best = CwLattice::kNoScore;
    for (unsigned t = 0; t <= N; t++) {
        if (f[t] > CwLattice::kNoScore) { best = std::max(best, f[t] - (llr_sum(N) - llr_sum(t))); }
    }
    return best;
}

// The best reading of (A, A+N] as ONE word — any marks, element and
// character spaces between them, a free space around — the bound every
// forced reading of the span stays under.
double CwWordReread::free_reading(const CwReadView &v, uint64_t A, unsigned N, float *lead,
                                   float *gm, float *gs) {
    const CwLatticeRings *L = v.lattice->rings();
    const auto llr_sum = [L, A](unsigned i) { return L->llr_sum[(A + i) & CwLattice::kMask]; };  // the llr sum at A + i
    static const CwSeg MT[2] = { CwSeg::Dit, CwSeg::Dah }, ST[2] = { CwSeg::ElementSpace, CwSeg::CharSpace };
    for (unsigned t = 0; t <= N; t++) {
        lead[t] = static_cast<float>(-(llr_sum(t) - llr_sum(0)));
        gm[t] = gs[t] = CwLattice::kNoScore;
    }
    double best = CwLattice::kNoScore;
    for (unsigned t = 1; t <= N; t++) {
        float bm = CwLattice::kNoScore, bs = CwLattice::kNoScore;
        for (int k = 0; k < 2; k++) {
            int dlo, dhi;
            v.priors.search_window(MT[k], &dlo, &dhi);
            for (int d = dlo; d <= dhi && static_cast<unsigned>(d) <= t; d++) {
                const float p = std::max(lead[t - d], gs[t - d]);
                if (p <= CwLattice::kNoScore)
                    continue;
                const float sc = p + static_cast<float>((llr_sum(t) - llr_sum(t - d)) +
                                             v.lattice->duration_prior(v.priors, MT[k], d));
                if (sc > bm) { bm = sc; }
            }
            v.priors.search_window(ST[k], &dlo, &dhi);
            const bool lz = m_oracle_loose && ST[k] == CwSeg::CharSpace;
            if (lz) {
                int wlo, whi;
                v.priors.search_window(CwSeg::WordSpace, &wlo, &whi);
                dhi = std::max(dhi, whi);
            }
            for (int d = dlo; d <= dhi && static_cast<unsigned>(d) <= t; d++) {
                const float p = gm[t - d];
                if (p <= CwLattice::kNoScore)
                    continue;
                const double pen = lz ? std::max(v.lattice->duration_prior(v.priors, CwSeg::CharSpace, d),
                                            v.lattice->duration_prior(v.priors, CwSeg::WordSpace, d))
                                      : v.lattice->duration_prior(v.priors, ST[k], d);
                const float sc = p + static_cast<float>(-(llr_sum(t) - llr_sum(t - d)) + pen);
                if (sc > bs) { bs = sc; }
            }
        }
        gm[t] = bm;
        gs[t] = bs;
        if (bm > CwLattice::kNoScore) { best = std::max(best, bm - (llr_sum(N) - llr_sum(t))); }
    }
    return best;
}

// Re-read the word that just closed; true with the call in `alt` when a
// dictionary call replaces it.
bool CwWordReread::reread(const CwReadView &v, const CwRescoreConfig &cfg, uint64_t B, SkimCall &alt) {
    if (!cfg.on || !v.lattice->rings() || v.priors.dit <= 0 || m_word_over || m_word.size() < 3)
        return false;
    bool digit = false, alpha = false;
    for (size_t i = 0; i < m_word.size(); i++) {
        digit |= std::isdigit(static_cast<unsigned char>(m_word[i])) != 0;
        alpha |= std::isalpha(static_cast<unsigned char>(m_word[i])) != 0;
    }
    if (!digit || !alpha)
        return false;                       // calls carry both
    // The span: from a little before the word's first mark (never into the
    // previous word, never before the ring's oldest valid sample) to B, the
    // end of the word gap or now while it runs — no next word is in it.
    uint64_t A = m_word_t0 > static_cast<uint64_t>(kLeadDits * v.priors.dit)
                    ? m_word_t0 - static_cast<uint64_t>(kLeadDits * v.priors.dit) : 0;
    A = std::max(A, std::max(m_word_prev, v.lattice->t0()));
    if (B > CwLattice::kSize - 8 && A < B - (CwLattice::kSize - 8)) { A = B - (CwLattice::kSize - 8); }
    if (B <= A || B - A > CwLattice::kSize / 2 || m_word_t0 < A || B > v.t)
        return false;
    const unsigned N = static_cast<unsigned>(B - A);

    SkimCall cand[MAX_NEAR_CALLS];
    unsigned nc = v.dict ? v.dict->near(m_word.c_str(), cand, MAX_NEAR_CALLS) : 0;
    if (!nc)
        return false;
    bool raw_known = false;
    for (unsigned i = 0; i < nc; i++) { raw_known |= cand[i] == m_word; }
    if (raw_known && !cfg.debug)
        return false;                       // a known call stays as read

    v.lattice->sync_prior_memo(v.priors);
    std::vector<float> buf_v(static_cast<size_t>(3 * (N + 1)));
    float *buf = buf_v.data();
    const double sf = free_reading(v, A, N, buf, buf + N + 1, buf + 2 * (N + 1));
    // Evidence per sample of the free reading: a keyed word scores high, a
    // word read out of noise (dit chains, "EI5E") barely above zero — and
    // noise must never be talked into a call the dictionary happens to hold.
    const double ev = sf / N;
    // The weak view's LLRs are calibrated nats, a few tenths a sample on a
    // keyed word (v2's heuristic saturates at 2.5): its own evidence bar,
    // SKIM_V3_RS_EV_WEAK. max_deficit and min_margin are likelihood
    // differences in both.
    const double ev_bar = v.weak ? cfg.min_evidence_weak : cfg.min_evidence;
    if (ev < ev_bar && !cfg.debug) {
        return false;
    }
    CwSeg seg[MAX_CALL_SEGMENTS];
    double s_raw = CwLattice::kNoScore;
    unsigned K = call_segments(m_word.c_str(), seg);
    if (K) { s_raw = forced(v, A, N, seg, K, buf, buf + N + 1); }
    // The decision is a prior against a likelihood. The word as read is
    // (nearly always) the free reading itself — the lattice found it — so
    // no call ever fits BETTER than it; but a string the dictionary does not
    // know is unlikely a priori, a call it knows is not. A dictionary call
    // wins when the audio disputes it by at most `max_deficit` and it is the
    // one such call — `min_margin` clear of the runner-up among the
    // dictionary's.
    // A word the dictionary knows stays as read.
    double s1 = CwLattice::kNoScore, s2 = CwLattice::kNoScore;
    int b1 = -1;
    double sc[MAX_NEAR_CALLS];
    for (unsigned i = 0; i < nc; i++) {
        sc[i] = CwLattice::kNoScore;
        if (cand[i] == m_word) {
            sc[i] = s_raw;
        } else if ((K = call_segments(cand[i].c_str(), seg))) {
            sc[i] = forced(v, A, N, seg, K, buf, buf + N + 1);
        }
        if (sc[i] > s1) {
            s2 = s1;
            s1 = sc[i];
            b1 = static_cast<int>(i);
        } else if (sc[i] > s2) {
            s2 = sc[i];
        }
    }
    const bool take = b1 >= 0 && !raw_known && s1 > CwLattice::kNoScore && ev >= ev_bar &&
                          std::max(sf, s_raw) - s1 <= cfg.max_deficit && s1 - s2 >= cfg.min_margin;
    if (cfg.debug) {
        std::string g;
        for (unsigned i = 0; i < nc; i++) {
            { char t_[48]; std::snprintf(t_, sizeof(t_), " %s:%.1f", cand[i].c_str(), sc[i] - sf); g += t_; }
        }
        std::fprintf(stderr, "rescore: %.2f %s%s free %.1f ev %.2f raw %+.1f best %s %+.1f next %+.1f%s |%s\n",
                   static_cast<double>(m_word_t0) / v.rate, m_word.c_str(), raw_known ? "(scp)" : "",
                   sf, ev, s_raw - sf, b1 >= 0 ? cand[b1].c_str() : "-", s1 - sf, s2 - sf,
                   take ? " TAKE" : "", g.c_str());
    }
    if (take) { alt = cand[b1]; }
    return take;
}

// SKIM_ORACLE_MODE=combine (oracle.h) — measurement only. The word that just
// closed, and it with the word before (a torn call), laid against R's call
// and the dictionary's calls near it or near the word: one "ocomb:" line
// per span, scores relative to the free reading of the span. Character
// gaps may run word-gap long here (oc_loose) in the forced and the free
// reading alike. Decoding is untouched: the lattice memo is all it shares.
void CwWordReread::oracle_span(const CwReadView &v, const SkimOracleHit &oh, uint64_t A,
                            uint64_t B, const char *word, int k) {
    if (B <= A || B > v.t || v.t - A > CwLattice::kSize - 16 || A < v.lattice->t0())
        return;
    const unsigned N = static_cast<unsigned>(B - A);
    SkimCall cand[2 * MAX_NEAR_CALLS];
    unsigned nc = v.dict ? v.dict->near(oh.call.c_str(), cand, MAX_NEAR_CALLS) : 0;
    SkimCall flat;                          // the span's text, '_' joins dropped
    for (const char *c = word; *c && flat.size() < SkimCall::kMaxLength; c++) {
        if (*c != '_') { flat.push_back(*c); }
    }
    if (flat.size() >= 2) {
        nc += v.dict ? v.dict->near(flat.c_str(), cand + nc, MAX_NEAR_CALLS) : 0;
    }
    std::vector<float> buf_v(static_cast<size_t>(3 * (N + 1)));
    float *buf = buf_v.data();
    m_oracle_loose = true;
    const double sf = free_reading(v, A, N, buf, buf + N + 1, buf + 2 * (N + 1));
    CwSeg seg[MAX_CALL_SEGMENTS];
    unsigned K = call_segments(oh.call.c_str(), seg);
    const double sc = K ? forced(v, A, N, seg, K, buf, buf + N + 1) : CwLattice::kNoScore;
    K = call_segments(flat.c_str(), seg);
    const double sr = K ? forced(v, A, N, seg, K, buf, buf + N + 1) : CwLattice::kNoScore;
    double s2 = CwLattice::kNoScore;
    int b2 = -1;
    for (unsigned i = 0; i < nc; i++) {
        if (cand[i] == oh.call)
            continue;
        const bool dup = std::any_of(cand, cand + i,
                                     [&](const SkimCall &c) { return c == cand[i]; });
        if (dup || !(K = call_segments(cand[i].c_str(), seg)))
            continue;
        const double x = forced(v, A, N, seg, K, buf, buf + N + 1);
        if (x > s2) {
            s2 = x;
            b2 = static_cast<int>(i);
        }
    }
    m_oracle_loose = false;
    std::fprintf(stderr, "ocomb: %.2f %.1f %s %s %d %u %.1f %.1f %.1f %s %.1f %.3f\n",
               v.oracle->time(static_cast<double>(B) / v.rate), v.freq_hz, oh.call.c_str(),
               word, k, N, sf, sc > CwLattice::kNoScore ? sc - sf : -999.0,
               sr > CwLattice::kNoScore ? sr - sf : -999.0, b2 >= 0 ? cand[b2].c_str() : "-",
               s2 > CwLattice::kNoScore ? s2 - sf : -999.0, sf / N);
}

void CwWordReread::oracle_word(const CwReadView &v, uint64_t B) {
    if (!v.oracle || !(v.oracle->modes() & SkimOracle::kCombine) || !v.lattice->rings() ||
        v.priors.dit <= 0 || m_word_over || m_word.empty())
        return;
    const std::optional<SkimOracleHit> hit =
        v.oracle->at(v.freq_hz, 0.25 * v.rate, v.oracle->time(static_cast<double>(B) / v.rate));
    if (!hit || hit->call.empty())
        return;
    const SkimOracleHit &oh = *hit;
    v.lattice->sync_prior_memo(v.priors);
    const uint64_t lead = static_cast<uint64_t>(kLeadDits * v.priors.dit);
    uint64_t A = m_word_t0 > lead ? m_word_t0 - lead : 0;
    A = std::max(A, std::max(m_word_prev, v.lattice->t0()));
    if (m_word.size() >= 2) { oracle_span(v, oh, A, B, m_word.c_str(), 1); }
    if (!m_oracle_prev.empty() && m_oracle_prev_t0 >= v.lattice->t0()) {
        char two[40];
        std::snprintf(two, sizeof(two), "%s_%s", m_oracle_prev.c_str(), m_word.c_str());
        const uint64_t A2 = m_oracle_prev_t0 > lead ? m_oracle_prev_t0 - lead : 0;
        if (m_oracle_prev.size() + m_word.size() < SkimCall::kCapacity) {
            oracle_span(v, oh, std::max(A2, v.lattice->t0()), B, two, 2);
        }
    }
}
