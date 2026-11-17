/*
 * callsign.h — callsign extraction + validation (RBN-grade).
 *
 * Pulls candidate callsigns out of decoded text and scores their plausibility:
 * a structural parser with the ITU allocation tables (letter+digit country
 * prefixes are where decode garbage like "T1BR" dies), CW context markers
 * (DE / CQ), repetition counting and an optional known-call dictionary
 * (MASTER.SCP format: one call per line). The RBN feed must never emit
 * unvalidated calls, so this gates spotting (M4 gates M6).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "skim_call.h"

#include <unordered_map>
#include <unordered_set>


// What a callsign is, and the decoder's corrections in decode text.
// SDRIAK: upstream's skim_callsign_is_valid(), SKIM_CALLSIGN_FIX_OPEN/CLOSE
// and skim_callsign_strip_fix().
class SkimCallsign {
public:
    SkimCallsign() = delete;

    // true if s is a structurally valid amateur callsign with an allocated
    // prefix. Portable designators are understood (OK1BR/P, F/OK1BR, OK1BR/4).
    static bool is_valid(const char *s);

    // A decoder may correct the word it just finished for the extractor alone:
    // "<raw chars> kFixOpen <call> kFixClose <space>" in the decode text. The
    // extractor drops the raw token and takes the call; everything that SHOWS
    // text (pane, decode log) drops the bracketed part and shows what was
    // heard — strip_fix().
    static constexpr char kFixOpen = '\x01';
    static constexpr char kFixClose = '\x02';
    // In place: the text without its corrections. true when there were some.
    static bool strip_fix(char *text);
};


// What a load of these bytes WOULD take, without building a
// dictionary — the updater's sanity check on a download. Safe on arbitrary
// bytes (no terminator needed, NULs and endless lines are junk).
struct SkimCallsignDictInfo {
    unsigned calls = 0;             // lines carrying a call
    unsigned valid = 0;             // of those, SkimCallsign::is_valid()
    unsigned junk = 0;              // neither call, blank, '#' nor "!!" line
    char  release[32] = "";      // "2026.09.18", "" when the file names none
};

// The extractor's switches (upstream: SKIM_CALL_* environment variables, read
// at first use into function-local statics). The defaults are upstream's;
// from_env() applies upstream's variables, parsed as
// upstream parses them, on top of a base.
struct SkimCallsignConfig {
    bool tu_strict = false;     // SKIM_CALL_TU_STRICT: a TU window never counts
                                // as a clean CQ
    bool cqwords = false;       // SKIM_CALL_CQWORDS: POTA/SOTA/… after CQ, a
                                // lone D waits for its E or X
    bool fused = true;          // SKIM_CALL_FUSED: a run of one call repeated
                                // without gaps is taken apart (since v0.6.0)
    bool glue = false;          // SKIM_CALL_GLUE: a token glued to a known call
    int  tu = 2;                // SKIM_CALL_TU: 0 = TU opens no calling window,
                                // 1 = a window, 2 = the next token only (2
                                // since v0.6.0)
    bool exch = false;          // SKIM_CALL_EXCH: "<call> 5NN" marks the call
    bool despace = true;        // SKIM_CALL_DESPACE: an over read with its
                                // spaces dropped (since v0.6.0)
    bool despace_debug = false; // SKIM_DESPACE_DEBUG: print each despace hit

    // (Two overloads, not a `base = {}` default argument: a default argument
    // of the class's own type inside the class needs its default member
    // initializers before the class is complete — Clang and GCC reject it.)
    static SkimCallsignConfig from_env();
    static SkimCallsignConfig from_env(SkimCallsignConfig base);
};

// SDRIAK: the known-call dictionary is an object, handed to whoever reads it
// (upstream: five globals under a GRWLock, loaded and read through
// skim_callsign_dict_load/size/release/has/near()). A SkimCallsignDict is one
// immutable table; its owner publishes tables into a SkimCallsignDictSource,
// and readers take a snapshot of the source per unit of work. A reload builds
// the next table with no lock held and publishes it in one atomic store; a
// snapshot keeps its table alive while anyone holds it — the old table dies
// with its last reader, never under one. The call strings live in m_calls (an
// unordered_set: its elements never move) and the near-call index points into
// them, so a dictionary is never copied.
class SkimCallsignDict {
public:
    // A dictionary from a file (MASTER.SCP style: one call per line, '#'
    // comments, "!!" directives and lines that cannot be a call are skipped).
    // path is UTF-8; null when it cannot be read, with *error (when given)
    // saying why (SDRIAK: a std::string, GError upstream).
    static std::shared_ptr<const SkimCallsignDict> load(const char *path,
                                                        std::string *error = nullptr);
    // … from bytes in memory (safe on arbitrary bytes, as inspect).
    static std::shared_ptr<const SkimCallsignDict> parse(std::string_view data);
    // What a load of these bytes WOULD take (SkimCallsignDictInfo), without
    // building a dictionary.
    static SkimCallsignDictInfo inspect(std::string_view data);

    SkimCallsignDict(const SkimCallsignDict &) = delete;
    SkimCallsignDict &operator=(const SkimCallsignDict &) = delete;

    unsigned size() const { return static_cast<unsigned>(m_calls.size()); }

    // The file's "# Release 2026.09.18" comment as "2026.09.18", "" when it
    // carried none.
    const char *release() const { return m_release; }

    // true when the dictionary knows this call (case-insensitive). The scorer
    // uses it for its +0.15 boost; the app highlights dictionary hits in the
    // monitor pane.
    bool has(const char *call) const;
    // … spelled exactly (upper case), as the extractor looks calls up.
    bool contains(const char *call) const { return m_calls.count(call) != 0; }

    // The dictionary's calls within two edits (Levenshtein) of `word` — the
    // candidates a decoder re-reads a misread call against (decode_cw_v2.c,
    // SKIM_V2_RESCORE). Found through a one-deletion index plus the shapes a
    // misread takes most (a call one or two characters shorter or longer, the
    // lost characters at either end), each one checked: not every call within
    // two edits is found, every call returned is within two. `word` itself is
    // among them when the dictionary knows it. Upper case in, at most `max`
    // out; returns how many.
    unsigned near(const char *word, SkimCall *out, unsigned max) const;

private:
    friend struct Near;                    // near()'s query (callsign.cpp)
    SkimCallsignDict() = default;
    void build_index();

    std::unordered_set<std::string> m_calls;     // the calls
    char m_release[32] = "";                     // "2026.09.18"; "" = none
    // near()'s index, built with the table: every call with one character
    // deleted → the calls it came from; the calls sorted, and sorted reversed
    // (a misread that lost its first characters).
    std::unordered_map<std::string, std::vector<const char *>> m_del;
    std::vector<std::string> m_fwd;
    std::vector<std::string> m_rev;
};

// The slot a dictionary's owner publishes tables into: the application owns
// one (empty = no dictionary) and hands it to the decoders and extractors,
// which read it through snapshot(). Both calls are safe from any thread at
// any time: a snapshot is the old table or the new one, never a half-built
// one.
class SkimCallsignDictSource {
public:
    void publish(std::shared_ptr<const SkimCallsignDict> dict) {
        std::atomic_store(&m_dict, std::move(dict));
    }
    std::shared_ptr<const SkimCallsignDict> snapshot() const { return std::atomic_load(&m_dict); }

private:
    std::shared_ptr<const SkimCallsignDict> m_dict;     // atomic access only
};


// The candidate best() would pick were there no spot threshold, with the
// parts of its score — what the feed-gate learning log records (gatelog,
// pipeline.h), so a learned gate can see the calls the hand gate drops
// below 0.70 too.
struct SkimCallsignCand {
    SkimCall call;
    double   score;          // the plausibility best() would report, unclamped
                             // by the threshold (0 when stale)
    unsigned    count;          // copies read (= hearings)
    unsigned    parts;          // fewest tokens it was glued from
    unsigned    idle_tokens;    // tokens decoded since it was last read
    bool de_marked;
    bool cq_context;
    bool dict;           // MASTER.SCP knows it
    int64_t   last_us;        // caller's clock at its last copy (set_now)
};

// The extractor's pick (SkimCallsignExtractor::best()).
struct SkimCallsignBest {
    SkimCall call;               // empty when no candidate reaches the threshold
    double score = 0.0;          // its score; 0 when none reaches it
    bool   cq_context = false;   // the leading candidate (even one under the
                                 // threshold) was heard CALLING
};

struct SkimCallsignExtractorState;

// Stateful per-channel extractor: feed decoded text incrementally (token
// fragments survive across calls), poll best() for the leading candidate.
// SDRIAK: a class (upstream: an opaque struct and skim_callsign_extractor_*
// functions — new/free are the constructor/destructor, the rest are the
// methods of the same names).
class SkimCallsignExtractor {
public:
    // Score a candidate must reach before it may be spotted: structural
    // validity alone (0.55) is NOT enough — it takes a DE marker, repetition,
    // CQ context or a dictionary hit on top.
    static constexpr double kSpotThreshold = 0.70;

    // One-shot convenience over a complete text buffer (same scoring).
    static SkimCallsignBest extract(const char *text,
                                    std::shared_ptr<const SkimCallsignDictSource> dict = nullptr,
                                    const SkimCallsignConfig &cfg = {});

    // dict: the dictionary the scorer consults (null = none); cfg: the
    // switches.
    explicit SkimCallsignExtractor(std::shared_ptr<const SkimCallsignDictSource> dict = nullptr,
                                   const SkimCallsignConfig &cfg = {});
    ~SkimCallsignExtractor();
    SkimCallsignExtractor(const SkimCallsignExtractor &) = delete;
    SkimCallsignExtractor &operator=(const SkimCallsignExtractor &) = delete;

    void   reset();
    void   feed(const char *text);

    // Best current candidate and its score (an empty call and 0.0 when none
    // reaches SkimCallsignExtractor::kSpotThreshold), and whether the candidate was
    // heard CALLING — in the context of a CQ/TEST/QRZ marker (leading or
    // trailing). The CQ-only spot policy keys off this: S&P answers do not
    // own the frequency. (Upstream: best() and best_ex().)
    SkimCallsignBest best();

    // How many copies of `call` this extractor has counted — the number of
    // times it was keyed and read, not how often it was reported. 0 when the
    // call is no candidate (never seen, or evicted). The RBN feed policy wants
    // two: one decode with DE and CQ around it scores 0.90 on its own, and a
    // garble keyed once reads exactly like that (skimmer-compare vs RBN,
    // 2026-10-01: 36 % of the feed's calls were never confirmed).
    unsigned hearings(const char *call) const;

    // true when the call's calling context was withdrawn: it was flagged in a
    // CQ window, but a report followed it ("UR3VZ 5NN" — the runner addressing
    // a caller) and that window crossed an over break. best()/top() then
    // report it not calling; this tells the station table to drop a flag an
    // earlier report of the same call set.
    bool addressed(const char *call) const;

    // The candidate best() would pick were there no spot threshold (see
    // SkimCallsignCand); nullopt when the extractor holds no live candidate.
    std::optional<SkimCallsignCand> top() const;

    // The caller's clock, for the time of each hearing: set it before feed().
    // Candidates age by TOKENS (traffic on the channel), not by time — a call
    // stays the best candidate through an hour of silence, and the first noise
    // after it reports the call again. last_heard() tells such a stale report
    // from a fresh one: the time of the last copy actually read, -1 when the
    // call is no candidate (0 while no clock was ever set).
    void    set_now(int64_t now_us);
    int64_t last_heard(const char *call) const;

private:
    std::unique_ptr<SkimCallsignExtractorState> m_x;   // the state (callsign.cpp)
};
