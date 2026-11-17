/*
 * cw_align.h — forced alignment of calls against an llr span: a call's
 * segment sequence, and the call alignment's duration tables, free reading
 * and call reading (shared by the call reader and the oracle's kws probe).
 * Also CwReadView: what the re-read and the call reader read of the decoder.
 *
 * Upstream's RS_MAX_SEG/RS_MAX_CAND are MAX_CALL_SEGMENTS/MAX_NEAR_CALLS,
 * KWS_SKIP CwCallAlignment::kUnreachable.
 *
 * SDRIAK: upstream's rs_segments() from the decoder, with external linkage,
 * as call_segments(), and its KwsDur, kws_dur(), kws_dur_free(), kws_free()
 * and kws_call() as the class CwCallAlignment (the constructor, the vectors'
 * destructor, free_reading() and align_call()); bodies unchanged, k->x read
 * as the member (cw_align.cpp).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_lattice.h"
#include "decode_cw_config.h"

class SkimCallsignDict;
class SkimOracle;
struct CwTrace;

constexpr auto MAX_CALL_SEGMENTS = 128;  // elements + spaces of one call
constexpr auto MAX_NEAR_CALLS = 24;      // dictionary calls fetched near a word

// What the word re-read and the call reader read of the decoder, at the
// moment they run: the lattice (its llr ring and duration memo), its priors,
// the sample clock, the channel and its configuration.
struct CwReadView {
    CwLattice              *lattice;
    CwSegmentPriors         priors;       // the clock (priors.dit), fist, edge
    uint64_t                t;            // the decoder's sample index
    double                  rate;
    double                  freq_hz;      // the channel's absolute frequency
    bool                    weak;         // v3's weak view (WEAKV)
    const SkimCallsignDict *dict;         // the dictionary as of this block (null
                                          // = none)
    const SkimOracle       *oracle;       // measurement only (null = off)
    const CwTrace          *trace;        // the channel's debug trace
};

// A call's segment sequence: Dit/Dah with ElementSpace inside a character,
// CharSpace between characters. 0 when a character has no Morse or it is too long.
unsigned call_segments(const char *call, CwSeg *seg);


// The keyword search over an llr window of N slots, T slots a dit: the
// segment duration tables (built once), the free reading and the call
// reading. llr_sum[0..N] are the window's cumulative llr sums; the float
// buffers are the caller's, N + 1 each.
class CwCallAlignment {
public:
    static constexpr auto kUnreachable = (-1e29f);  // scores under it: no reading

    CwCallAlignment(double T, int N);

    // The free reading of the window: silence, then marks and spaces, then
    // silence; a silence longer than a word space may sit inside too.
    // prefix[t]: the best reading of (0, t] that a call may follow — ending
    // in silence or a word space (after a character space, a call's tail
    // would fit inside a longer call: K1HC in OK1HCG). end_mark,
    // end_element_space, end_long_silence, end_char_space: scratch, the best
    // reading of (0, t] whose last segment is of that kind. Returns the best
    // reading of the whole window.
    double free_reading(const double *llr_sum, float *prefix, float *end_mark,
                        float *end_element_space, float *end_long_silence,
                        float *end_char_space) const;

    // The call's elements seg[0..K) between a free reading before (prefix)
    // and after (suffix, read from the window's end). row, next_row: scratch.
    double align_call(const double *llr_sum, const CwSeg *seg, unsigned K, const float *prefix,
                      const float *suffix, float *row, float *next_row) const;

private:
    // The search's duration classes: the lattice's five, and a character
    // space stretched up to 8 dits (a torn call).
    enum Kind : int { kDit, kDah, kElementSpace, kCharSpace, kWordSpace, kLongCharSpace, kKinds };
    // A call's segment as the search's class: its character spaces may
    // stretch.
    static Kind kind_of(CwSeg s) {
        return s == CwSeg::CharSpace ? kLongCharSpace : static_cast<Kind>(seg_index(s));
    }

    int    m_N;                           // the window, slots
    struct Durations {                    // one class's
        int lo = 0, hi = 0;                 // the range, slots
        std::vector<float> prior;           // prior[d], lo ≤ d ≤ hi
    };
    Durations m_durations[kKinds];
    int    m_long_silence;                // a silence longer than a word space, slots
    float  m_long_silence_prior;          // … and its duration prior
};
