// cw_align.cpp — see cw_align.h. Part of skimmer-for-linux. GPL-3.0-or-later.
#include "cw_align.h"
#include "cw_morse.h"

unsigned call_segments(const char *call, CwSeg *seg) {
    unsigned k = 0;
    for (const char *c = call; *c; c++) {
        const char *m = CwMorse::pattern(static_cast<unsigned char>(*c));
        if (!m)
            return 0;
        if (c != call) {
            if (k >= MAX_CALL_SEGMENTS) { return 0; }
            seg[k++] = CwSeg::CharSpace;
        }
        for (const char *e = m; *e; e++) {
            if (k + 2 > MAX_CALL_SEGMENTS)
                return 0;
            if (e != m) { seg[k++] = CwSeg::ElementSpace; }
            seg[k++] = *e == '-' ? CwSeg::Dah : CwSeg::Dit;
        }
    }
    return k;
}

CwCallAlignment::CwCallAlignment(double T, int N) : m_N(N) {
    // Per class, in dits: the range searched, the ideal, the prior's α.
    struct Shape { double lo, hi, ideal, alpha; };
    static const Shape kShape[kKinds] = {
        { 0.6,  2.6, 1.0, 10.0 },   // kDit
        { 1.6,  5.0, 3.0, 10.0 },   // kDah
        { 0.7,  2.6, 1.0, 10.0 },   // kElementSpace
        { 1.6,  5.5, 3.0,  6.0 },   // kCharSpace
        { 4.0, 17.0, 7.0,  2.5 },   // kWordSpace
        { 1.6,  8.0, 3.0,  6.0 },   // kLongCharSpace
    };
    for (int j = 0; j < kKinds; j++) {
        m_durations[j].lo = std::max(2, static_cast<int>(kShape[j].lo * T + 0.5));
        m_durations[j].hi = std::max(m_durations[j].lo, std::min(static_cast<int>(kShape[j].hi * T + 0.5), N));
        m_durations[j].prior.assign(static_cast<size_t>(m_durations[j].hi) + 1, 0.0f);
        for (int d = m_durations[j].lo; d <= m_durations[j].hi; d++) {
            const double r = log(d / (kShape[j].ideal * T));
            double p = -kShape[j].alpha * r * r - CwSegmentPriors::kSegmentCost;
            if (j == kLongCharSpace) {
                const double rw = log(d / (7.0 * T));
                p = std::max(p, -2.5 * rw * rw - CwSegmentPriors::kSegmentCost);
            }
            m_durations[j].prior[d] = static_cast<float>(p);
        }
    }
    m_long_silence = m_durations[kWordSpace].hi + 1;
    m_long_silence_prior = m_durations[kWordSpace].prior[m_durations[kWordSpace].hi];
}

double CwCallAlignment::free_reading(const double *llr_sum, float *prefix, float *end_mark,
                                     float *end_element_space, float *end_long_silence,
                                     float *end_char_space) const {
    const int N = m_N;
    prefix[0] = 0.0f;
    end_mark[0] = end_element_space[0] = end_long_silence[0] = end_char_space[0] = CwLattice::kNoScore;
    double best_word = -(llr_sum[N] - llr_sum[0]);
    for (int t = 1; t <= N; t++) {
        float best_mark = CwLattice::kNoScore;
        for (int j = kDit; j <= kDah; j++) {
            for (int d = m_durations[j].lo; d <= m_durations[j].hi && d <= t; d++) {
                const float p = std::max(prefix[t - d],
                                         std::max(end_element_space[t - d], end_char_space[t - d]));
                if (p < kUnreachable)
                    continue;
                const float sc = p + static_cast<float>(llr_sum[t] - llr_sum[t - d]) +
                                 m_durations[j].prior[d];
                if (sc > best_mark) { best_mark = sc; }
            }
        }
        end_mark[t] = best_mark;
        float best_element_space = CwLattice::kNoScore, best_char_space = CwLattice::kNoScore,
              best_word_space = CwLattice::kNoScore;
        for (int j = kElementSpace; j <= kWordSpace; j++) {
            for (int d = m_durations[j].lo; d <= m_durations[j].hi && d <= t; d++) {
                const float p = end_mark[t - d];
                if (p < kUnreachable)
                    continue;
                const float sc = p - static_cast<float>(llr_sum[t] - llr_sum[t - d]) +
                                 m_durations[j].prior[d];
                float *best = j == kElementSpace ? &best_element_space
                              : j == kCharSpace  ? &best_char_space
                                                 : &best_word_space;
                if (sc > *best) { *best = sc; }
            }
        }
        end_element_space[t] = best_element_space;
        end_char_space[t] = best_char_space;
        float long_silence = end_long_silence[t - 1] < kUnreachable
                                 ? CwLattice::kNoScore
                                 : end_long_silence[t - 1] - static_cast<float>(llr_sum[t] - llr_sum[t - 1]);
        if (t >= m_long_silence && end_mark[t - m_long_silence] > kUnreachable) {
            long_silence = std::max(long_silence,
                                    end_mark[t - m_long_silence] -
                                        static_cast<float>(llr_sum[t] - llr_sum[t - m_long_silence]) +
                                        m_long_silence_prior);
        }
        end_long_silence[t] = long_silence;
        prefix[t] = std::max(std::max(static_cast<float>(-(llr_sum[t] - llr_sum[0])), best_word_space),
                             long_silence);
        if (best_mark > kUnreachable) { best_word = std::max(best_word, best_mark - (llr_sum[N] - llr_sum[t])); }
    }
    return best_word;
}

double CwCallAlignment::align_call(const double *llr_sum, const CwSeg *seg, unsigned K,
                                   const float *prefix, const float *suffix, float *row,
                                   float *next_row) const {
    const int N = m_N;
    memcpy(row, prefix, (N + 1) * sizeof(float));
    for (unsigned q = 0; q < K; q++) {
        const int j = kind_of(seg[q]);
        const double sign = (j == kDit || j == kDah) ? 1.0 : -1.0;
        for (int t = 0; t <= N; t++) {
            float best = CwLattice::kNoScore;
            for (int d = m_durations[j].lo; d <= m_durations[j].hi && d <= t; d++) {
                const float p = row[t - d];
                if (p < kUnreachable)
                    continue;
                const float sc = p + static_cast<float>(sign * (llr_sum[t] - llr_sum[t - d])) +
                                 m_durations[j].prior[d];
                if (sc > best) { best = sc; }
            }
            next_row[t] = best;
        }
        std::swap(row, next_row);
    }
    double best = CwLattice::kNoScore;
    for (int t = 0; t <= N; t++) {
        if (row[t] > kUnreachable && suffix[t] > kUnreachable) { best = std::max<double>(best, row[t] + suffix[t]); }
    }
    return best;
}
