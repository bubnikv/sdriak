/*
 * cw_callreader.h — the call reader (docs/CW-V3.md).
 *
 * Every CwCallReader::kEverySeconds on a locked channel the last CwCallReader::kWindowSeconds of the lattice's own
 * LLR are searched for the calls the decoded text points at — a token two
 * edits from a MASTER.SCP call, torn tokens joined, a glued token's inner
 * call, a call this channel confirmed before — by forced alignment against
 * the free reading (the kws oracle, WEAK-SIGNAL-PLAN §10: 291 of 711 missed
 * CQ stations found in the audio, ~30 ppm false alarms). A call that fits
 * within SKIM_V3_KWS_DC of the free reading and beats its dictionary neighbours by
 * SKIM_V3_KWS_SEP, twice in separate windows (or once by far), goes to the extractor
 * as a correction token — with a CQ before it when the over calls.
 *
 * Also the oracle's kws probe (SKIM_ORACLE_MODE=kws, measurement only).
 *
 * SDRIAK: upstream's kh/kh_t (the text ring, here one TextChar array),
 * kr_* and kws fields of the decoder state, kws_read()
 * with its kr_* helpers, and kws_eval()/kws_push(), as stages
 * (cw_callreader.cpp); the bodies read the decoder through a CwReadView.
 * The fields and helpers are renamed for what they hold (README,
 * "Renamed").
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_align.h"
#include "skim_call.h"

class CwCallReader {
public:
    static constexpr auto kTextChars = 160;       // decoded text kept for reading
    static constexpr auto kHitSlots = 8;          // calls remembered as confirmed
    static constexpr auto kWindowSeconds = 6.0;   // the llr window read
    static constexpr auto kEverySeconds = 3.0;    // one read this often
    static constexpr auto kSlotsPerDit = 3.0;     // llr slots a dit
    static constexpr auto kMaxCandidates = 16;    // calls scored per read
    static constexpr auto kMaxNearControls = 8;   // dictionary neighbours scored
                                                  // against the winner
    static constexpr auto kHoldSeconds = 300.0;   // a confirmed call is looked for
                                                  // this long
    static constexpr auto kRepeatSeconds = 20.0;  // a call goes out at most this
                                                  // often
    static constexpr auto kCqSeconds = 12.0;      // CQ evidence covers the call
                                                  // this long

    // One emitted character (or space) whose audio ended at `at`.
    void push_char(char c, uint64_t at) {
        if (m_text_n == kTextChars) {                   // the oldest half goes
            std::copy(m_text + kTextChars / 2, m_text + kTextChars, m_text);
            m_text_n = kTextChars / 2;
        }
        m_text[m_text_n++] = { at, c };
    }

    // At sample t (rate): a read is due (every kEverySeconds); the next one
    // is scheduled.
    bool due(uint64_t t, double rate) {
        if (t < m_next_read)
            return false;
        m_next_read = t + static_cast<uint64_t>(kEverySeconds * rate);
        return true;
    }

    // One read of the last kWindowSeconds (upstream: kws_read); a confirmed
    // call becomes the FIX phrase.
    void read(const CwReadView &v, const CwCallReaderConfig &cfg);

    // The FIX phrase due at the next word gap ("" = none), and its delivery.
    const char *fix() const { return m_fix_phrase; }
    void fix_sent() { m_fix_phrase[0] = '\0'; }

private:
    struct TextChar {
        uint64_t at;                        // when its audio ended (samples)
        char     c;                         // a char or a gap
    };
    TextChar m_text[kTextChars] = {};     // the decoded text
    unsigned m_text_n = 0;
    uint64_t m_next_read = 0;               // next read (samples)
    // The calls this channel confirmed.
    struct Confirmed {
        SkimCall call;
        uint64_t last_read;                 // the last window that found it
        uint64_t last_sent;                 // when it last went out (0 = never)
        double   best_score;                // its best score against the free reading
        unsigned reads;                     // windows that found it, no audio shared
    };
    Confirmed m_confirmed[kHitSlots] = {};
    char     m_fix_phrase[48] = {};         // FIX phrase due at the next word space
    uint64_t m_calling_seen = 0;            // last CQ evidence (text or audio) + 1

};

// SKIM_ORACLE_MODE=kws (oracle.h): the probe's envelope ring and one "okws:"
// line per hop (cw_callreader.cpp). pre_head: the pre-roll's write count,
// the oracle's clock.
class CwKwsProbe {
public:
    static constexpr auto kWindowSeconds = 6.0;  // the envelope searched
    static constexpr auto kHopSeconds = 3.0;     // one search this often
    static constexpr auto kNearCalls = 8;        // dictionary calls near R's
    static constexpr auto kDecoys = 6;           // R's other stations' calls

    // dump: SKIM_ORACLE_KWS_DUMP (the window's llr as text).
    void push(const CwReadView &v, unsigned pre_head, float ms, bool dump);

private:
    void eval(const CwReadView &v, unsigned pre_head, int N, bool dump);

    std::vector<float> m_ring;            // envelope ring
    uint64_t m_written = 0;               // samples written to it
};
