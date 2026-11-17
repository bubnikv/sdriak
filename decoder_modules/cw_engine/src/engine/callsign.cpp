/*
 * callsign.c — callsign extraction + validation (M4).
 *
 * Validation = a structural parse against the shapes real callsigns take,
 * with the ITU allocation encoded where it discriminates:
 *   V1  single-letter series (B F G I K M N R W) + area digit(s) + suffix
 *   V2  two-letter prefix (any but Q*) + area digit(s) + suffix
 *   V3  letter+digit country prefix (per-letter table: T2..T8 yes, T1 NO —
 *       exactly the class of CW decode garbage this must kill) + optional
 *       area digit(s) + suffix; the no-area form needs a ≥2-letter suffix
 *       (C6AGU yes, "5NN" no)
 *   V4  digit-first prefix 2..9 + letter (+letter: 3DA) + area + suffix
 * Suffix: 1–4 chars ending in a letter. Portable designators are split on
 * '/' and either side may be the call (OK1BR/P, F/OK1BR).
 *
 * Extraction: tokenised stream with CW context — a token after DE gets the
 * strongest marker, tokens shortly after CQ get a weaker one, repetitions
 * accumulate, and an optional known-call dictionary (MASTER.SCP) boosts.
 * A degenerate fist can close the gaps AROUND the markers ("CQCQCQ
 * DEEA1EYL"); two strictly-shaped fallbacks recover those tokens, and both
 * fire only where the normal path fails — cleanly keyed streams never
 * enter them.
 * Scores: 0.55 structural+allocation, +0.25 DE, +0.10 CQ, +0.20 repeated
 * (+0.05 at ≥3), +0.15 dictionary — capped at 1.0. The spot threshold 0.70
 * keeps a BARE structurally-valid token off the panadapter — but one copy
 * with DE and CQ around it scores 0.90, past the RBN feed's 0.85 too. That
 * is why the feed policy (pipeline.c) also counts hearings, see
 * SkimCallsignExtractor::hearings().
 *
 * Engine-thread only (no locking), like the rest of the pipeline.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "callsign.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <cstdlib>
#include <cstring>

// --- ITU allocation ---------------------------------------------------------
// Single-letter series and the letter+digit country prefixes. The two-letter
// space is allocated for every first letter except Q (reserved for Q-codes).
// Maintainable data — extend here when a rare prefix shows up missing.
static const char ITU_SINGLE_LETTER_SERIES[] = "BFGIKMNRW";

static const char *ld_digits(char letter) {
    switch (letter) {
    case 'A': return "2456789";   // A2 Botswana … A9 Bahrain
    case 'C': return "2345689";   // C2 Nauru … C9 Mozambique (C7 unassigned)
    case 'D': return "23456789";  // D2 Angola … D7-9 Korea
    case 'E': return "234567";    // E2 Thailand … E7 Bosnia
    case 'H': return "2346789";   // H2 Cyprus … H8-9 Panama
    case 'J': return "2345678";   // J2 Djibouti … J8 St Vincent
    case 'L': return "23456789";  // L2-L9 Argentina
    case 'P': return "2345";      // P2 PNG, P3 Cyprus, P4 Aruba, P5 DPRK
    case 'S': return "2579";      // S2 Bangladesh, S5 Slovenia, S7, S9
    case 'T': return "2345678";   // T2 Tuvalu, T4 Cuba … T8 Palau — T1 does
                                  // NOT exist
    case 'V': return "2345678";   // V2 Antigua … V8 Brunei
    case 'Y': return "23456789";  // Y2-Y9 Germany
    case 'Z': return "2368";      // Z2 Zimbabwe, Z3 Macedonia, Z6, Z8
    default:  return "";
    }
}

static bool is_letter(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_digit(char c)  { return c >= '0' && c <= '9'; }

// area digit(s) [1-2] + suffix [1-4, ends in a letter], the common tail.
static bool tail_ok(const char *s) {
    if (!is_digit(*s))
        return false;
    s++;
    if (is_digit(*s)) { s++; }
    size_t n = strlen(s);
    if (n < 1 || n > 4 || !is_letter(s[n - 1]))
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!is_letter(s[i]) && !is_digit(s[i]))
            return false;
    }
    return true;
}

// Structural + allocation check of a bare call (no '/' designators).
static bool core_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 8)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!is_letter(s[i]) && !is_digit(s[i]))
            return false;
    }
    if (s[0] == 'Q')
        return false;                              // Q-codes, never callsigns

    if (is_letter(s[0]) && is_letter(s[1])) {    // V2: two-letter prefix
        return tail_ok(s + 2);
    }
    if (is_letter(s[0]) && is_digit(s[1])) {
        // V1: single-letter series — F5IN, K1A, B1HQ.
        if (strchr(ITU_SINGLE_LETTER_SERIES, s[0]) && tail_ok(s + 1))
            return true;
        // V3: letter+digit country prefix — E73ABC, T77XX, C6AGU.
        if (strchr(ld_digits(s[0]), s[1])) {
            if (tail_ok(s + 2))
                return true;                           // with an area digit
            size_t m = strlen(s + 2);                 // no area: ≥2 letters only
            if (m >= 2 && m <= 4) {
                for (size_t i = 2; i < n; i++) {
                    if (!is_letter(s[i]))
                        return false;
                }
                return true;
            }
        }
        return false;
    }
    if (is_digit(s[0]) && s[0] >= '2' && is_letter(s[1])) {
        // V4: digit-first — 9A1AA, 2E0ABC, 3DA0RS (optional second letter).
        if (tail_ok(s + 2))
            return true;
        if (is_letter(s[2]) && tail_ok(s + 3))
            return true;
        return false;
    }
    return false;
}

// The short side of a '/' call — a designator (P, M, QRP, 4, MM) or a bare
// prefix (F, DL, EA8). Letters and digits only, and three letters are only
// ever QRP: in two weeks of RBN (240 k slashed spots) no other three-letter
// designator came by, while L read /P+, /P+K, /PIN, /PHB off glued text
// (live, 2026-10-12).
static bool designator_ok(const char *s, size_t n) {
    bool letters = true;
    for (size_t i = 0; i < n; i++) {
        if (!is_letter(s[i]) && !is_digit(s[i]))
            return false;
        if (!is_letter(s[i])) { letters = false; }
    }
    return n < 3 || !letters || strncmp(s, "QRP", 3) == 0;
}

bool SkimCallsign::is_valid(const char *s) {
    if (!s || !s[0])
        return false;
    const char *slash = strchr(s, '/');
    if (!slash)
        return core_valid(s);
    if (strchr(slash + 1, '/'))
        return false;                              // at most one designator
    // Either side may be the call: OK1BR/P, OK1BR/4, F/OK1BR. The other side
    // must be short (a designator or a bare DXCC prefix).
    size_t left = static_cast<size_t>(slash - s), right = strlen(slash + 1);
    char core[16];
    // Both sides full calls: the licence of one country used in another
    // under its full local call — VP2V/AA7V (CQ WW 2012, 56 CW Skimmer
    // spots the 3-character designator rule threw away).
    if (left < sizeof(core) && left >= 4 && right >= 4) {
        memcpy(core, s, left);
        core[left] = '\0';
        if (core_valid(core) && core_valid(slash + 1))
            return true;
    }
    if (left < sizeof(core) && right >= 1 && right <= 3 &&
        designator_ok(slash + 1, right)) {
        memcpy(core, s, left);
        core[left] = '\0';
        if (core_valid(core))
            return true;
    }
    if (right < sizeof(core) && left >= 1 && left <= 3 &&
        designator_ok(s, left)) {
        if (core_valid(slash + 1))
            return true;
    }
    return false;
}

// --- known-call dictionary ----------------------------------------------------

constexpr auto NEAR_QUERY_BUF_LEN = 24; // the near-call index's query
                                        // buffers (a word + 1 char)

// SkimCallsignDict and SkimCallsignDictSource: callsign.h.

// ASCII in place: trim leading/trailing white space (g_strstrip upstream).
static char *strstrip(char *s) {
    char *p = s;
    while (*p && std::isspace(static_cast<unsigned char>(*p))) { p++; }
    if (p != s) { memmove(s, p, strlen(p) + 1); }
    size_t n = strlen(s);
    while (n && std::isspace(static_cast<unsigned char>(s[n - 1]))) { s[--n] = '\0'; }
    return s;
}

// One stripped dictionary line → true when it carries a call. Blank lines,
// '#' comments and "!!" directives (Super Check Partial opens its files with
// "!!Order,1,1") carry none, and neither does anything holding a character
// no callsign has — whatever header the format grows next stays out too.
// Deliberately NOT SkimCallsign::is_valid(): the list is the authority on
// who is on the air, the validator only knows the prefixes it was taught.
static bool dict_line_is_call(const char *s) {
    if (!s[0] || s[0] == '#' || (s[0] == '!' && s[1] == '!'))
        return false;
    size_t n = 0;
    bool digit = false, alpha = false;
    for (const char *p = s; *p; p++, n++) {
        if (std::isdigit(static_cast<unsigned char>(*p))) { digit = true; }
        else if (std::isalpha(static_cast<unsigned char>(*p))) { alpha = true; }
        else if (*p != '/') { return false; }
    }
    return digit && alpha && n >= 3 && n <= 15;
}

// The one parser: the loader and SkimCallsignDict::inspect() walk a file
// the same way, so what a check counted is what a load would take. Runs on
// a length, not on a terminator — the updater hands it bytes off the network,
// and those may be anything (an HTML error page, a NUL-ridden binary). A line
// longer than the stack copy, or one hiding a NUL, is junk by definition.
static SkimCallsignDictInfo dict_parse(std::string_view data, std::unordered_set<std::string> *into) {
    SkimCallsignDictInfo z;
    const char *p = data.data(), *end = data.data() + data.size();
    while (p < end) {
        const char *nl = static_cast<const char *>(memchr(p, '\n', static_cast<size_t>(end - p)));
        size_t n = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);
        char line[64];
        bool fits = n < sizeof(line) && !memchr(p, '\0', n);
        if (fits) {
            memcpy(line, p, n);
            line[n] = '\0';
            strstrip(line);
        }
        p += n + (nl ? 1 : 0);
        if (!fits) { z.junk++; continue; }
        if (!dict_line_is_call(line)) {
            if (line[0] == '#') {                    // "# Release 2026.09.18"
                const char *r = line + 1;
                while (*r == ' ') { r++; }
                bool rel = true;                       // ASCII "Release " any case
                for (int k = 0; k < 8 && rel; k++) {
                    rel = std::tolower(static_cast<unsigned char>(r[k])) == "release "[k];
                }
                if (!z.release[0] && rel) {
                    skim_strlcpy(z.release, r + 8, sizeof(z.release));
                    strstrip(z.release);
                }
            } else if (line[0] && !(line[0] == '!' && line[1] == '!')) {
                z.junk++;
            }
            continue;
        }
        for (char *c = line; *c; c++) { *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c))); }
        z.calls++;
        if (SkimCallsign::is_valid(line)) { z.valid++; }
        if (into) { into->insert(line); }
    }
    return z;
}

SkimCallsignDictInfo SkimCallsignDict::inspect(std::string_view data) {
    return dict_parse(data, nullptr);
}

static std::string str_rev(const char *s) {
    std::string r(s);
    std::reverse(r.begin(), r.end());
    return r;
}

void SkimCallsignDict::build_index() {
    m_fwd.reserve(m_calls.size());
    m_rev.reserve(m_calls.size());
    for (const std::string &entry : m_calls) {
        const char *call = entry.c_str();
        const size_t n = entry.size();
        m_fwd.push_back(entry);
        m_rev.push_back(str_rev(call));
        char key[NEAR_QUERY_BUF_LEN];
        if (n >= sizeof(key))
            continue;
        for (size_t i = 0; i < n; i++) {
            if (i && call[i] == call[i - 1])
                continue;                              // "OO" deletes to one key
            memcpy(key, call, i);
            memcpy(key + i, call + i + 1, n - i);
            m_del[key].push_back(call);
        }
    }
    std::sort(m_fwd.begin(), m_fwd.end());
    std::sort(m_rev.begin(), m_rev.end());
}

// A reload may come at any time (the app swaps a freshly downloaded file in
// while the engine thread scores tokens and the GTK thread underlines them):
// the new table is built with no lock held and published by its owner
// (SkimCallsignDictSource) in one atomic store; readers keep the table they
// took until they let it go.
std::shared_ptr<const SkimCallsignDict> SkimCallsignDict::load(const char *path,
                                                               std::string *error) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        if (error) { *error = std::string("cannot open ") + path; }
        return nullptr;
    }
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (f.bad()) {
        if (error) { *error = std::string("cannot read ") + path; }
        return nullptr;
    }
    return parse(data);
}

std::shared_ptr<const SkimCallsignDict> SkimCallsignDict::parse(std::string_view data) {
    std::shared_ptr<SkimCallsignDict> fresh(new SkimCallsignDict());
    const SkimCallsignDictInfo info = dict_parse(data, &fresh->m_calls);
    fresh->build_index();
    skim_strlcpy(fresh->m_release, info.release, sizeof(fresh->m_release));
    return fresh;
}

// Edit distance, given up past `cap` (returns cap + 1 then).
static unsigned lev_capped(const char *a, const char *b, unsigned cap) {
    const size_t na = strlen(a), nb = strlen(b);
    if ((na > nb ? na - nb : nb - na) > cap)
        return cap + 1;
    unsigned row[NEAR_QUERY_BUF_LEN + 1];
    if (nb >= std::size(row))
        return cap + 1;
    for (size_t j = 0; j <= nb; j++) { row[j] = static_cast<unsigned>(j); }
    for (size_t i = 1; i <= na; i++) {
        unsigned diag = row[0], lo;
        row[0] = lo = static_cast<unsigned>(i);
        for (size_t j = 1; j <= nb; j++) {
            const unsigned up = row[j];
            row[j] = std::min(std::min(row[j] + 1, row[j - 1] + 1),
                              diag + static_cast<unsigned>(a[i - 1] != b[j - 1]));
            diag = up;
            lo = std::min(lo, row[j]);
        }
        if (lo > cap)
            return cap + 1;
    }
    return row[nb];
}

// One near-call query: the calls within two edits of word, into out.
struct Near {
    const char *word;
    SkimCall *out;
    unsigned n, max;

    void add(const char *call);
    void del(const SkimCallsignDict &d, const char *key);
    void ext(const std::vector<std::string> &arr, const char *s, bool reversed);
};

void Near::add(const char *call) {
    if (n >= max || !SkimCall::fits(call))
        return;
    if (std::any_of(out, out + n, [&](const SkimCall &c) { return c == call; }))
        return;
    if (lev_capped(word, call, 2) <= 2) { out[n++].assign(call); }
}

void Near::del(const SkimCallsignDict &d, const char *key) {
    const auto it = d.m_del.find(key);
    if (it == d.m_del.end())
        return;
    for (const char *call : it->second) { add(call); }
}

// Calls of length n + 2 that start with s, in a sorted array (reversed:
// that end with it).
void Near::ext(const std::vector<std::string> &arr, const char *s, bool reversed) {
    const size_t n = strlen(s);
    size_t lo = 0, hi = arr.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (strcmp(arr[mid].c_str(), s) < 0) { lo = mid + 1; } else { hi = mid; }
    }
    for (size_t i = lo, seen = 0; i < arr.size() && seen < 400; i++, seen++) {
        const char *c = arr[i].c_str();
        if (strncmp(c, s, n) != 0)
            break;
        if (arr[i].size() != n + 2)
            continue;
        if (reversed) {
            add(str_rev(c).c_str());
        } else {
            add(c);
        }
    }
}

unsigned SkimCallsignDict::near(const char *word, SkimCall *out, unsigned max) const {
    const size_t n = word ? strlen(word) : 0;
    if (n < 2 || n + 1 >= NEAR_QUERY_BUF_LEN)
        return 0;
    Near q = { word, out, 0, max };
    if (m_calls.count(word)) { q.add(word); }
    q.del(*this, word);                   // one character longer
    char d1[NEAR_QUERY_BUF_LEN], d2[NEAR_QUERY_BUF_LEN];
    for (size_t i = 0; i < n; i++) {
        memcpy(d1, word, i);
        memcpy(d1 + i, word + i + 1, n - i);
        if (m_calls.count(d1)) { q.add(d1); }      // shorter
        q.del(*this, d1);                   // one character replaced
        for (size_t j = i; j + 1 < n; j++) {        // two characters shorter
            memcpy(d2, d1, j);
            memcpy(d2 + j, d1 + j + 1, n - 1 - j);
            if (m_calls.count(d2)) { q.add(d2); }
        }
    }
    q.ext(m_fwd, word, false);            // the end lost
    q.ext(m_rev, str_rev(word).c_str(), true);    // the head lost
    return q.n;
}

bool SkimCallsign::strip_fix(char *text) {
    char *w = text;
    bool any = false, in = false;
    for (const char *r = text; *r; r++) {
        if (*r == SkimCallsign::kFixOpen) {
            in = any = true;
        } else if (*r == SkimCallsign::kFixClose) {
            in = false;
        } else if (!in) {
            *w++ = *r;
        }
    }
    *w = '\0';
    return any;
}

// SDRIAK: upstream reads each switch at its first use (xk_tu_strict(),
// cqwords_on(), fused_on(), glue_on(), take_token()'s tu_mode and exch_on,
// despace_on(), despace_scan()'s SKIM_DESPACE_DEBUG); the same parsing.
SkimCallsignConfig SkimCallsignConfig::from_env() { return from_env(SkimCallsignConfig()); }

SkimCallsignConfig SkimCallsignConfig::from_env(SkimCallsignConfig c) {
    const char *e;
    // "on when set and not 0"
    auto on = [](const char *v) { return v && v[0] && strcmp(v, "0") != 0; };
    // "on unless set to 0" (the switches on since v0.6.0)
    auto on_unless_0 = [](const char *v) { return !(v && v[0]) || strcmp(v, "0") != 0; };
    if ((e = std::getenv("SKIM_CALL_TU_STRICT"))) { c.tu_strict = on(e); }
    if ((e = std::getenv("SKIM_CALL_CQWORDS"))) { c.cqwords = on(e); }
    if ((e = std::getenv("SKIM_CALL_FUSED"))) { c.fused = on_unless_0(e); }
    if ((e = std::getenv("SKIM_CALL_GLUE"))) { c.glue = on(e); }
    if ((e = std::getenv("SKIM_CALL_TU")) && e[0]) { c.tu = atoi(e); }
    if ((e = std::getenv("SKIM_CALL_EXCH"))) { c.exch = on(e); }
    if ((e = std::getenv("SKIM_CALL_DESPACE"))) { c.despace = on_unless_0(e); }
    if (std::getenv("SKIM_DESPACE_DEBUG")) { c.despace_debug = true; }
    return c;
}

bool SkimCallsignDict::has(const char *call) const {
    if (!call || !call[0] || strlen(call) >= 24)
        return false;
    char up[24];
    skim_strlcpy(up, call, sizeof(up));
    for (char *p = up; *p; p++) { *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p))); }
    return contains(up);
}

// A lookup through a table that may be absent (no dictionary).
static bool knows(const SkimCallsignDict *d, const char *call) {
    return d && d->contains(call);
}

// --- stateful extractor --------------------------------------------------------

constexpr auto CALL_BUF_LEN = SkimCall::kCapacity;   // a call and its terminator
constexpr auto TOKEN_BUF_LEN = (2 * CALL_BUF_LEN); // a glued CQ chain + a call

struct Cand {
    SkimCall call;
    unsigned    count;
    unsigned    last_tok;                           // x->tok_n at the last hit
    int64_t   last_us;                            // x->now_us at the last hit
    unsigned    parts;                              // fewest tokens it came in
    bool de_marked;
    bool cq_context;
    bool cq_clean;                           // … with no over break between
    bool addressed;                          // a report followed the call
    bool exch;                               // "<call> 5NN" (runner's QSO)

    // Calling, all told: a CQ window reached the call, and either nobody sent it
    // a report, or that window had no over break in it. A report right after a
    // call is the runner ADDRESSING a caller ("UR3VZ 5NN") — and a caller who
    // comes in after the runner's "TEST DL4KWA" or "TU", past the over break,
    // inherited that window (skimmer-compare 2026-10-10: the feed's "taken"
    // busts — a caller spotted as calling where VE3NEA and RBN heard another
    // station call — were mostly such callers). A runner's own clean "TU M0NGN
    // 5NN" stays calling.
    bool calling() const { return cq_context && (!addressed || cq_clean); }
};

struct SkimCallsignExtractorState {
    // Enough slots that a rare clean copy of a QSB-mangled call SURVIVES the
    // flood of its own mutations until the next clean copy arrives — with 12,
    // 9A170NT's single good decode kept getting evicted before its second
    // hearing could lift it over the spot threshold (live, 2026-07-15).
    static constexpr auto kMaxCandidates = 24;
    static constexpr auto kCqWindowTokens = 3;  // tokens after CQ that count
    static constexpr auto kMaxRunFragments = 12;  // fragments between two edges
    static constexpr auto kMaxTornParts = 6;    // a torn call: ≤ 6 of them
    // A candidate goes STALE this many processed tokens after its last hit —
    // the frequency changed hands and the new occupant must win immediately;
    // an ever-growing count kept the OLD call on top for the rest of the run
    // (live-caught 2026-07-15). The clock ticks on channel traffic, not wall
    // time, so a station pausing between overs never ages out. Short on
    // purpose: while a stale best keeps being re-reported, its station record
    // keeps a fresh last_heard and the takeover eviction never fires — the
    // tuned label flickered between the old and the new occupant (EA2BTN vs
    // IT9IQN, live-caught 2026-07-15). A runner repeats their call every CQ
    // cycle (~5-10 tokens), so 12 keeps real stations alive.
    static constexpr auto kStaleAfterTokens = 12;
    static constexpr auto kMaxFusedLength = 96; // longest run taken apart
    static constexpr auto kMaxOverBytes = 240;  // bytes of an over kept

    std::string tok;                                // partial token across feeds
    int     de_pending;                         // DE marker lives ≤ 2 tokens
    int     cq_recent;                          // tokens since CQ (≤ window)
    bool cq_broken;                          // an over break since the CQ
    unsigned    cq_pairs;                           // adjacent "C","Q" pairs seen
    bool cq_half;                            // last token was a lone "C"
    bool tu_win;                             // the open window is a TU's
    SkimCall tu_call;                               // the call that TU blessed
    unsigned    tok_n;                              // tokens processed (age clock)
    int64_t   now_us;                             // caller's clock (set_now)
    SkimCall prev_tok;                              // previous token (join hyp.)
    bool prev_valid;                         // it was a valid call itself
    bool prev_de;                            // DE applied to it
    bool prev_cq;                            // CQ window applied to it
    bool prev_gap;                           // an over break came after it
    std::string over;                               // SKIM_CALL_DESPACE: the over
    std::string over_toks;                          // so far, spaces dropped, and
                                                    // its tokens " A B " as read
    std::string run;                                // fragments since the last
    unsigned    run_n;                              // edge, glued (torn call)
    bool run_de, run_cq;
    SkimCall run_hit;                               // its join counted on arrival
    char     run_lead;                           // lone E/T/K/R right before
    bool run_spelled;                        // every piece one character
    bool held_d, d_flush;                    // SKIM_CALL_CQWORDS: a lone D
                                             // waits for its E or X
    Cand     cand[kMaxCandidates];
    SkimCallsignConfig cfg;                                  // the switches
    std::shared_ptr<const SkimCallsignDictSource> source;   // the dictionary
    std::shared_ptr<const SkimCallsignDict> dict;   // … as of this call (null =
                                                    // none)
    unsigned    ncand;

    // SDRIAK: upstream's static helpers on the extractor, as methods (defined
    // below, where upstream has the functions).
    void run_clear();
    double cand_score(const Cand *c) const;
    void cand_add(const char *call, unsigned parts, bool de_marked, bool cq_context);
    void run_take(const char *tok, bool de, bool cq);
    bool run_ambiguous(const char *call, char trail) const;
    bool run_close(char trail);
    bool fused_split(const char *tok);
    bool cand_has(const char *call) const;
    bool glue_split(const char *tok);
    void take_token(const char *tok);
    void despace_scan();
    void despace_note(const char *tok);
    const Cand *best_pick(double *score) const;
};

SkimCallsignExtractor::SkimCallsignExtractor(std::shared_ptr<const SkimCallsignDictSource> dict,
                                             const SkimCallsignConfig &cfg)
    : m_x(std::make_unique<SkimCallsignExtractorState>()) {
    SkimCallsignExtractorState *x = m_x.get();
    x->cfg = cfg;
    x->source = std::move(dict);
    x->over_toks = " ";
    x->cq_recent = SkimCallsignExtractorState::kCqWindowTokens + 1;
}

SkimCallsignExtractor::~SkimCallsignExtractor() = default;

void SkimCallsignExtractorState::run_clear() {
    run.clear();
    run_n  = 0;
    run_de = run_cq = false;
    run_hit.clear();
    run_lead = '\0';
    run_spelled = true;
}

void SkimCallsignExtractor::reset() {
    SkimCallsignExtractorState *x = m_x.get();
    x->tok.clear();
    x->de_pending = 0;
    x->cq_recent  = SkimCallsignExtractorState::kCqWindowTokens + 1;
    x->cq_broken  = false;
    x->cq_pairs   = 0;
    x->cq_half    = false;
    x->held_d     = x->d_flush = false;
    x->tu_win     = false;
    x->tu_call.clear();
    x->over.clear();
    x->over_toks = " ";
    x->tok_n      = 0;
    x->ncand      = 0;
    x->prev_tok.clear();
    x->prev_valid = x->prev_de = x->prev_cq = x->prev_gap = false;
    x->run_clear();
}

// A call for the extractor: the structural parse, or the dictionary's word.
// The parse must stay strict about the prefix-only shapes noise produces
// ("5NN", "T5T"), and so it refuses the short calls contest stations get —
// D4C, C5A, P3N (CQ WW 2012: 1 100+ CW Skimmer spots of real stations
// the extractor could never report). MASTER.SCP knows them.
static bool call_ok(const SkimCallsignDict *d, const char *s) {
    return SkimCallsign::is_valid(s) || (s && strlen(s) >= 3 && knows(d, s));
}

double SkimCallsignExtractorState::cand_score(const Cand *c) const {
    if (tok_n - c->last_tok > kStaleAfterTokens)
        return 0.0;                                // not heard lately — dead
    double s = 0.55;                             // structural + allocation
    if (c->de_marked)  { s += 0.25; }
    if (c->cq_context) { s += 0.10; }
    if (c->count >= 2) { s += 0.20; }
    if (c->count >= 3) { s += 0.05; }
    if (knows(dict.get(), c->call.c_str())) { s += 0.15; }
    if (c->exch)       { s += 0.25; }
    return std::min(s, 1.0);
}

// SKIM_CALL_TU_STRICT=1: a call a TU window blessed is never clean-calling,
// so a report after it withdraws the blessing — tried for v0.6.0 and left
// off: "5NN TU M0NGN 5NN" (a contest runner, the 5NN to the next caller)
// then read as addressed.

void SkimCallsignExtractorState::cand_add(const char *call, unsigned parts, bool de_marked, bool cq_context) {
    Cand *c = nullptr;
    for (unsigned i = 0; i < ncand; i++) {
        if (cand[i].call == call) { c = &cand[i]; break; }
    }
    if (!c) {
        if (ncand < kMaxCandidates) {
            c = &cand[ncand++];
        } else {
            // Evict the weakest; among equally weak (stale ones all score 0),
            // the LEAST-REPEATED goes first — a stale twice-heard call is worth
            // keeping over a stale one-off mutation.
            c = &cand[0];
            for (unsigned i = 1; i < kMaxCandidates; i++) {
                double si = cand_score(&cand[i]), sc = cand_score(c);
                if (si < sc || (si == sc && cand[i].count < c->count)) {
                    c = &cand[i];
                }
            }
        }
        *c = Cand{};
        c->call.assign(call);
        c->parts = parts;
    }
    c->parts = std::min(c->parts, parts);
    c->count++;
    c->last_tok = tok_n;
    c->last_us  = now_us;
    if (de_marked)  { c->de_marked  = true; }
    if (cq_context) { c->cq_context = true; }
    if (cq_context && !cq_broken && !(tu_win && cfg.tu_strict)) { c->cq_clean = true; }
}

// "5NN", "599", "ENN" and their kin, a serial number glued on or not.
static bool report_token(const char *s) {
    static const char *RST[] = { "5NN", "599", "5NNN", "ENN", "55N" };
    for (unsigned i = 0; i < std::size(RST); i++) {
        const size_t n = strlen(RST[i]);
        if (strncmp(s, RST[i], n) == 0) {
            const char *t = s + n;
            while (std::isdigit(static_cast<unsigned char>(*t))) { t++; }
            if (*t == '\0' && (t == s + n || i < 2)) { return true; }
        }
    }
    return false;
}

// Tokens that never take part in a join: prosigns and stock CW abbreviations
// glue onto a neighbouring call into a VALID-looking phantom ("OK1BR K" →
// "OK1BRK", "R EA3I" → "REA3I" — R is a legal prefix).
static bool join_stop_word(const char *s) {
    static const char *STOP[] = {
        "K", "KN", "BK", "SK", "AR", "AS", "TU", "R", "E", "T", "EE",
        "PSE", "QRZ", "QRL", "TEST", "NR", "UR", "5NN", "599", "73", "88",
        // stock QSO vocabulary — "UB7M TNX" glued into the valid-looking
        // phantom UB7MTNX (live-caught 2026-07-15)
        "TNX", "FER", "RPRT", "QSO", "QTH", "AGN", "HW", "GA", "GE", "GM",
        "DR", "OM", "ES", "VY", "ABT", "HPE", "SRI", "RIG", "ANT", "WX", "OP",
    };
    for (unsigned i = 0; i < std::size(STOP); i++) {
        if (strcmp(s, STOP[i]) == 0) { return true; }
    }
    return false;
}

// "CQCQCQ" — a fist whose inter-word gaps collapse sends the whole CQ chain
// as ONE token (EA1EYL, live-caught 2026-07-16: word ≈ letter gaps at ~5-6
// dits, the fist model rightly refuses such a fit — so the fix belongs
// here, in the lexicon). Strict shape: nothing but "CQ" repeated, ≥2 times;
// a cleanly keyed stream cannot produce it, so the normal CQ path is
// untouched.
// SKIM_CALL_CQWORDS=1 — the words that qualify a CQ ("CQ DX", "CQ POTA",
// "CQ SOTA DE …") do not age the calling window, glued on ("CQPOTA",
// "CQCQSOTA") they peel off like DX and TEST, and inside an open window a
// lone "D" followed by "E" or "X" is the torn DE / DX ("CQ D X D E
// EA6ACA" — four tokens closed the 3-token window before the call came).
// WEAK-SIGNAL-PLAN §8: of R's CQ stations L read and never saw calling,
// 60 % had a calling marker in the text the window did not reach.

static bool cq_word(const char *s) {
    static const char *W[] = { "DX", "POTA", "SOTA", "WWFF", "IOTA", "NA",
                                 "EU", "AS", "AF", "OC", "SA", "JA" };
    for (unsigned i = 0; i < std::size(W); i++) {
        if (strcmp(s, W[i]) == 0)
            return true;
    }
    return false;
}

// The glued qualifier at s: its length, 0 when none (DX and TEST are the
// old path's own).
static size_t cq_glued_word(const SkimCallsignConfig &cfg, const char *s) {
    if (!cfg.cqwords)
        return 0;
    static const char *W[] = { "POTA", "SOTA", "WWFF", "IOTA" };
    for (unsigned i = 0; i < std::size(W); i++) {
        if (strncmp(s, W[i], 4) == 0)
            return 4;
    }
    return 0;
}

static bool cq_run_token(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || (n % 2) != 0)
        return false;
    for (size_t i = 0; i < n; i += 2) {
        if (s[i] != 'C' || s[i + 1] != 'Q')
            return false;
    }
    return true;
}

// CQ-strip — the CQ half of the degenerate fist: the gap AFTER the calling
// marker collapses as well, and CQ arrives glued to what follows ("CQDE",
// "CQCQDEIZ8BRE", "CQDXDEF8GFA", "CQTEST"). Two days of decode tap
// (2026-10-06): of the stations VE3NEA spotted calling that L read but
// never saw calling, 98 had no CQ but a glued one. Strictly a fallback like
// the DE-strip: a token that is a valid call itself stays whole (CQ7A), and
// the remainder must be nothing, DE, DE + a call, or a call — "CQC",
// "CQDEM" and the rest of the garble keep their old fate. Returns the
// length of the marker head to peel off, 0 when the token is no such run.
static size_t cq_glued_head(const SkimCallsignDict *d, const SkimCallsignConfig &cfg,
                            const char *tok) {
    size_t i = 0, ncq = 0;
    bool word = false;
    for (;;) {
        if (tok[i] == 'C' && tok[i + 1] == 'Q') {
            i += 2;
            ncq++;
        } else if (ncq && strncmp(tok + i, "DX", 2) == 0) {
            i += 2;
            word = true;
        } else if (ncq && strncmp(tok + i, "TEST", 4) == 0) {
            i += 4;
            word = true;
        } else if (ncq && cq_glued_word(cfg, tok + i)) {
            i += 4;
            word = true;
        } else {
            break;
        }
    }
    if (!ncq)
        return 0;
    char bare[CALL_BUF_LEN];
    if (strlen(tok + i) >= sizeof(bare))
        return 0;
    skim_strlcpy(bare, tok + i, sizeof(bare));
    const size_t bl = strlen(bare);
    if (bl > 2 && strcmp(bare + bl - 2, "\xC2\xB7") == 0) { bare[bl - 2] = '\0'; }
    if (bare[0] == '\0')
        return word ? i : 0;                       // pure CQ runs: cq_run_token
    if (strcmp(bare, "DE") == 0 || call_ok(d, bare) ||
        (bare[0] == 'D' && bare[1] == 'E' && strlen(bare) >= 5 &&
         call_ok(d, bare + 2)))
        return i;
    return 0;
}

// The torn call — gh#3. An operator who leaves a word gap after EVERY group
// sends "UA 6 H NU" (live 2026-09-11, 14039: four tokens in 16 of 28 overs,
// three in the rest, six when it got worse). Gluing neighbours token by
// token cannot work there: "UA6 H" validates one token before "UA6 H NU"
// does, the pipeline reports the best candidate after every token, and the
// head went to the panadapter as the call. So the fragments between two
// EDGES — DE, a calling marker, a stop word, a token that is a call by
// itself, the decoder's over break — are glued as a WHOLE: all of the run
// or nothing, or the same call keyed two or three times over. No head, no
// tail, no call plus the first letter of its own repeat ("SP6OS SP").

// A serial or a report ("79", "001") and a lone "/" are never a piece of a
// call — a single digit may be ("UA 6 H NU"). Without this the exchange in
// front of a torn call joined its run and killed it ("5NN 7 79 OL7 ABS",
// 80 m contest fixture).
static bool run_edge_token(const char *tok) {
    if (strcmp(tok, "/") == 0)
        return true;
    size_t n = 0;
    for (; tok[n]; n++) {
        if (tok[n] < '0' || tok[n] > '9')
            return false;
    }
    return n >= 2;
}

void SkimCallsignExtractorState::run_take(const char *tok, bool de, bool cq) {
    if (run_n > kMaxRunFragments)
        return;                                    // babble — wait for an edge
    run_n++;
    run += tok;
    if (tok[1] != '\0') { run_spelled = false; }
    if (de) { run_de = true; }
    if (cq) { run_cq = true; }
}

// E, T, K and R are stop words because noise and procedure are full of
// them — and they are letters of calls. Next to a run, with no break in
// between, a lone one may be either: "E A2 D DC" is EA2DDC keyed letter by
// letter, not A2DDC after a stray dit; "F 4 I K C" is not F4I and a K. When
// the call reads with that letter glued on as well, the run is ambiguous
// and counts as nothing — what the extractor made of such text before.
// After the run this applies to spelled-out runs only: "UA 6 H NU K" ends
// in a real K, and UA6HNUK would validate too.
bool SkimCallsignExtractorState::run_ambiguous(const char *call, char trail) const {
    char with[CALL_BUF_LEN + 2];
    if (run_lead) {
        std::snprintf(with, sizeof(with), "%c%s", run_lead, call);
        if (call_ok(dict.get(), with)) { return true; }
    }
    if (trail && run_spelled) {
        std::snprintf(with, sizeof(with), "%s%c", call, trail);
        if (call_ok(dict.get(), with)) { return true; }
    }
    return false;
}

static char lone_stop_letter(const char *tok) {
    return (tok[1] == '\0' && strchr("ETKR", tok[0])) ? tok[0] : '\0';
}

// true when the run read as a call (and counted). `trail` = the lone stop
// letter that ended it, or 0.
bool SkimCallsignExtractorState::run_close(char trail) {
    const size_t len = run.size();
    bool hit = false;
    if (run_n >= 2 && run_n <= kMaxRunFragments) {
        for (unsigned r = 3; r >= 1 && !hit; r--) {
            if (len % r != 0 || run_n < 2 * r || run_n > kMaxTornParts * r)
                continue;
            const size_t l = len / r;
            if (l >= CALL_BUF_LEN)
                continue;
            char call[CALL_BUF_LEN];
            memcpy(call, run.c_str(), l);
            call[l] = '\0';
            bool same = true;
            for (unsigned i = 1; i < r && same; i++) {
                same = memcmp(run.c_str() + i * l, call, l) == 0;
            }
            if (!same || !call_ok(dict.get(), call))
                continue;
            if (run_ambiguous(call, trail))
                break;
            hit = true;
            // the two-token join the dictionary knows was counted on arrival
            if (r == 1 && run_hit == call)
                break;
            for (unsigned i = 0; i < r; i++) {
                cand_add(call, (run_n + r - 1) / r, run_de, run_cq);
            }
        }
    }
    return hit;
}

// SKIM_CALL_FUSED=1 — the fused run. A strong signal whose word gaps the
// decoder does not hear comes out as ONE token: "CQCQCQDEIT9VJOIT9VJOIT9VJO
// CQCQCQDEIT9VJOIT9VJOPSEK", "DXDERN3QBGRN3QBGRN3QK", "DEDL5ANTDL5ANT"
// (live 2026-10-15/16: of the stations CW Skimmer spotted at ≥ 25 dB and L
// did not, more than half sat in L's text read exactly — fused like this).
// The run is taken apart where the evidence is strong: the calling and
// sender markers it opens with peeled off, then a call that REPEATS back to
// back ("IT9VJO" three times; twice for a call MASTER.SCP knows), never one
// that a copy of it runs on into a longer known call ("OK1OOK1OA" is OK1OA
// twice, cut, not OK1O). What is left goes round again; a run that does
// not open on a repeat keeps its old fate. A dictionary call read once
// between a marker and a "K" was tried as well: it minted the heads of
// longer calls (OK1O, ON5C, PA3D — "DEOK1OK…"), busts on 50 h of taps.

static size_t fused_markers(const SkimCallsignConfig &cfg, const char *s, bool *cq, bool *de) {
    size_t i = 0;
    for (;;) {
        if (strncmp(s + i, "CQ", 2) == 0) {
            i += 2;
            *cq = true;
        } else if (strncmp(s + i, "TEST", 4) == 0) {
            i += 4;
            *cq = true;
        } else if (strncmp(s + i, "QRZ", 3) == 0) {
            i += 3;
            *cq = true;
        } else if (i && strncmp(s + i, "DX", 2) == 0) {
            i += 2;                                  // "CQDX", "DXDE"
        } else if (*cq && cq_glued_word(cfg, s + i)) {
            i += 4;                                  // "CQPOTA", "CQSOTADE"
        } else if (strncmp(s + i, "DE", 2) == 0) {
            i += 2;
            *de = true;
        } else {
            return i;
        }
    }
}


// A copy of `call` at s that runs on into a LONGER call the dictionary
// knows ("OK1O" in "OK1OA", "ON5C" in "ON5CENT") is a cut-down copy of
// that one, not a repeat of its own.
static bool fused_extends(const SkimCallsignDict *d, const char *s, size_t l) {
    char ext[CALL_BUF_LEN];
    for (size_t e = l + 1; e <= l + 4 && e < CALL_BUF_LEN && s[e - 1]; e++) {
        memcpy(ext, s, e);
        ext[e] = '\0';
        if (knows(d, ext))
            return true;
    }
    return false;
}

// The call that repeats back to back at the head of r (rn bytes): its
// length, the call in `call` and the copies in *reps; 0 when none does.
static size_t fused_repeat(const SkimCallsignDict *d, const char *r, size_t rn, char *call,
                           unsigned *reps_out) {
    unsigned reps = 0;
    size_t len = 0;
    for (size_t l = std::min(rn, static_cast<size_t>(CALL_BUF_LEN) - 1); l >= 3 && !reps; l--) {
        memcpy(call, r, l);
        call[l] = '\0';
        if (!call_ok(d, call))
            continue;
        unsigned k = 1;
        while ((k + 1) * l <= rn && memcmp(r + k * l, call, l) == 0) { k++; }
        bool cut = false;
        for (unsigned j = 0; j <= k && !cut; j++) {
            if (j * l < rn && memcmp(r + j * l, call, std::min(l, rn - j * l)) == 0) {
                cut = fused_extends(d, r + j * l, l);
            }
        }
        // a call the dictionary knows needs two copies, any other three
        if (!cut && (k >= 3 || (k >= 2 && knows(d, call)))) {
            reps = k;
            len = l;
        }
    }
    if (reps) {
        memcpy(call, r, len);
        call[len] = '\0';
    }
    *reps_out = reps;
    return len;
}

bool SkimCallsignExtractorState::fused_split(const char *tok) {
    const size_t n = strlen(tok);
    if (n < 8 || n > kMaxFusedLength)
        return false;
    bool cq = false, de = false;
    const size_t h = fused_markers(cfg, tok, &cq, &de);
    const char *r = tok + h;
    char call[CALL_BUF_LEN];
    unsigned reps = 0;
    const size_t len = fused_repeat(dict.get(), r, n - h, call, &reps);
    if (!len)
        return false;
    if (cq) { take_token("CQ"); }
    if (de) { take_token("DE"); }
    call[len] = '\0';
    for (unsigned i = 0; i < reps; i++) { take_token(call); }
    // The rest goes round again — unless it is the head of one more copy,
    // cut where the run ended ("OK1DOLOK1DOLOK1O": OK1O is no station).
    const char *rest = r + reps * len;
    if (rest[0] && strncmp(call, rest, strlen(rest)) != 0) { take_token(rest); }
    return true;
}

// SKIM_CALL_GLUE=1 — a call with a word of the QSO glued on: the gap
// before PSE, K, UP or 73 not heard ("RO3ZPSE", "UT2QLPSE", "G4ENBK",
// "7T5EUP"; live 2026-10-16/17, sent by the feed, confirmed by nobody).
// The token splits into the call and the word when the call stands on
// its own — the dictionary knows it or this extractor already holds it
// as a candidate — and the whole token is no call the dictionary knows.

bool SkimCallsignExtractorState::cand_has(const char *call) const {
    for (unsigned i = 0; i < ncand; i++) {
        if (cand[i].call == call)
            return true;
    }
    return false;
}

bool SkimCallsignExtractorState::glue_split(const char *tok) {
    static const char *W[] = { "PSEK", "PSE", "KN", "BK", "UP", "73", "TU", "K" };
    const size_t n = strlen(tok);
    if (n < 5 || n >= CALL_BUF_LEN || knows(dict.get(), tok))
        return false;
    for (unsigned i = 0; i < std::size(W); i++) {
        const size_t lw = strlen(W[i]);
        if (n < lw + 3 || strcmp(tok + n - lw, W[i]) != 0)
            continue;
        char call[CALL_BUF_LEN];
        memcpy(call, tok, n - lw);
        call[n - lw] = '\0';
        if (call_ok(dict.get(), call) && (knows(dict.get(), call) || cand_has(call))) {
            take_token(call);
            take_token(W[i]);
            return true;
        }
    }
    return false;
}

void SkimCallsignExtractorState::take_token(const char *tok) {
    if (cfg.cqwords && !d_flush) {
        if (held_d) {
            held_d = false;
            if (strcmp(tok, "E") == 0) {
                take_token("DE");
                return;
            }
            if (strcmp(tok, "X") == 0) {
                take_token("DX");
                return;
            }
            d_flush = true;
            take_token("D");
            d_flush = false;
        }
        if (strcmp(tok, "D") == 0 && cq_recent <= kCqWindowTokens) {
            held_d = true;
            return;
        }
    }
    if (strcmp(tok, "\xC2\xB7") == 0) {
        // The decoder's over-break mark ("·") is metadata, not received text —
        // transparent for the two-token join: it must not eat a DE marker and
        // must not break prev_tok, or a QSB dip inside a call kills the join
        // hypothesis ("LZ67 · PP" never reassembled into LZ67PP; live-caught
        // 2026-07-15). A RUN of fragments ends here: pieces from two overs are
        // not one call ("M7· O ·" read M7O, which the dictionary knows — a new
        // station from one hearing of two scraps, SAC fixture).
        run_close(0);
        run_clear();
        prev_gap = !prev_tok.empty();
        // a CQ window that runs past an over break may be someone else's
        if (cq_recent <= kCqWindowTokens) { cq_broken = true; }
        return;
    }
    // A report right after a call, no over break between: that call is being
    // ADDRESSED — the runner answering a caller (Cand::calling).
    if (report_token(tok) && !prev_tok.empty() && !prev_gap) {
        for (unsigned i = 0; i < ncand; i++) {
            if (cand[i].call == prev_tok) {
                cand[i].addressed = true;
                break;
            }
        }
    }
    if (!call_ok(dict.get(), tok)) {
        const size_t head = cq_glued_head(dict.get(), cfg, tok);
        if (head) {
            take_token("CQ");                     // as if the gap were keyed
            if (tok[head]) { take_token(tok + head); }
            return;
        }
        if (cfg.fused && fused_split(tok))
            return;
    }
    if (cfg.glue && glue_split(tok))
        return;
    if (strlen(tok) >= CALL_BUF_LEN)
        return;                                    // a run nothing could read
    tok_n++;
    // Letter-spaced CQ chain — the TORN twin of the fused "CQCQ" run: the
    // same degenerate fist that glues gaps shut also stretches them, and the
    // CQ chain arrives as single-letter tokens "C Q C Q" (EA1EYL again,
    // live-caught 2026-07-16 — one operator, both shapes in one evening).
    // Two adjacent pairs in strict alternation make a CQ marker; a machine
    // always keys CQ into ONE token, so clean traffic never comes here.
    if (tok[0] == 'C' && tok[1] == '\0') {
        if (cq_half) { cq_pairs = 0; }       // "C C" broke the chain
        cq_half = true;
    } else if (cq_half && tok[0] == 'Q' && tok[1] == '\0') {
        cq_half = false;
        if (++cq_pairs >= 2) { cq_recent = 0; }
    } else {
        cq_half  = false;
        cq_pairs = 0;
    }
    if (strcmp(tok, "DE") == 0) {
        // SKIM_CALL_TU=2: "TU 73 UA3WBC DE DK2DR" — the call TU blessed is
        // the one being worked when DE follows it: not calling.
        if (!tu_call.empty() && prev_tok == tu_call) {
            for (unsigned i = 0; i < ncand; i++) {
                if (cand[i].call == tu_call) { cand[i].addressed = true; }
            }
        }
        tu_call.clear();
        run_close(0);
        run_clear();
        de_pending = 2;
        prev_tok.clear();                     // a call never straddles DE
        return;
    }
    if (strcmp(tok, "CQ") == 0 || strcmp(tok, "TEST") == 0 ||
        strcmp(tok, "QRZ") == 0 || strcmp(tok, "CWT") == 0 ||
        strcmp(tok, "TU") == 0 || cq_run_token(tok)) {
        // A calling marker: CQ/QRZ, TEST and CWT (CWops) — both contest-style
        // leading ("TEST SD1A") and trailing ("SD1A TEST" / "F5IN CWT") —
        // bless the call that was JUST sent too, then open the window for the
        // one that follows. TU is LEADING-only: "TU M0NGN" is the runner
        // closing a QSO and re-announcing (Richard, 2026-07-15), but in
        // "<call> TU" the thanks may go to the OTHER station.
        run_close(0);
        run_clear();
        // SKIM_CALL_TU=0: TU still ends a run but opens no calling window —
        // live 2026-10-16/17, a third of the calls the feed sent and nobody
        // confirmed were QSO partners after the thanks ("TU 73 UA3WBC DE
        // DK2DR", "5NN TU OE7PHI"), not a runner re-announcing.
        // SKIM_CALL_TU=2: TU blesses only the very next token, and a DE right
        // after that call takes the blessing back (see the DE case).
        const int tu_mode = cfg.tu;
        const bool is_tu = strcmp(tok, "TU") == 0;
        tu_call.clear();
        if (is_tu && tu_mode == 0) {
            prev_tok.clear();
            return;
        }
        // the window ages before the next token reads it: kCqWindowTokens - 1
        // leaves exactly that one token inside
        cq_recent = is_tu && tu_mode == 2 ? kCqWindowTokens - 1 : 0;
        tu_win = is_tu && tu_mode == 2;
        cq_broken = false;
        if (strcmp(tok, "TU") != 0 && !prev_tok.empty()) {
            for (unsigned i = 0; i < ncand; i++) {
                if (cand[i].call == prev_tok) {
                    cand[i].cq_context = true;
                    if (!prev_gap) { cand[i].cq_clean = true; }
                    break;
                }
            }
        }
        prev_tok.clear();
        return;
    }
    if (cfg.cqwords && cq_recent <= kCqWindowTokens && cq_word(tok)) {
        run_close(0);                           // "CQ POTA": the window waits
        run_clear();
        prev_tok.clear();
        return;
    }
    if (cq_recent <= kCqWindowTokens) { cq_recent++; }

    // SKIM_CALL_EXCH=1: "<call> 5NN …" is a runner sending the report to
    // the station it works — as strong a marker for that call as DE is for
    // the sender's own (CQ WW 2012: a caller heard once in a runner's
    // exchange never reached 0.85, CW Skimmer spots such callers).
    if (cfg.exch && prev_valid && !prev_tok.empty() &&
        ((strncmp(tok, "5NN", 3) == 0) || (strncmp(tok, "599", 3) == 0))) {
        for (unsigned i = 0; i < ncand; i++) {
            if (cand[i].call == prev_tok) {
                cand[i].exch = true;
                break;
            }
        }
    }

    const bool valid = call_ok(dict.get(), tok) && strlen(tok) < CALL_BUF_LEN;
    const bool de_now = de_pending > 0;
    const bool cq_now = cq_recent <= kCqWindowTokens;
    if (tu_win) {
        if (cq_now && valid) { tu_call.assign(tok); }
        if (!cq_now) { tu_win = false; }
    }

    // Join hypothesis — the sloppy-fist fix: an operator who stretches an
    // inter-letter gap splits their call across two tokens ("EA3I XQ" for
    // EA3IXQ, live-caught 2026-07-15). Try gluing the previous token on:
    // accept when the JOIN is a structurally valid call and it explains
    // something the parts do not (a dictionary hit, or a fragment that is no
    // call by itself). Repetition then outscores the torn variants, and the
    // station table's clip fold retires them. Two FRAGMENTS are a different
    // matter — they may be the head of a longer torn call, so that join
    // belongs to the run (above), unless the dictionary knows it or an over
    // break sits between the two (no run crosses one).
    const bool stop = join_stop_word(tok);
    if (!prev_tok.empty() && !join_stop_word(prev_tok.c_str()) && !stop) {
        char join[CALL_BUF_LEN];
        if (prev_tok.size() + strlen(tok) < CALL_BUF_LEN) {
            std::snprintf(join, sizeof(join), "%s%s", prev_tok.c_str(), tok);
            const bool known = knows(dict.get(), join);
            if (call_ok(dict.get(), join) &&
                (known || valid != prev_valid || prev_gap)) {
                cand_add(join, 2, prev_de || de_now, prev_cq || cq_now);
                if (!valid && !prev_valid) {
                    run_hit.assign(join);
                }
            }
        }
    }
    // The over-break mark arrives GLUED to a character the decoder committed
    // together with it ("NU·", "R·"): for the run that is the bare token,
    // then the mark.
    const bool de_glued = !valid && strlen(tok) >= 5 && tok[0] == 'D' &&
                              tok[1] == 'E' && call_ok(dict.get(), tok + 2);
    char  bare[CALL_BUF_LEN];
    size_t bl = strlen(tok);
    const bool marked = bl > 2 && strcmp(tok + bl - 2, "\xC2\xB7") == 0;
    skim_strlcpy(bare, tok, sizeof(bare));
    if (marked) { bare[bl - 2] = '\0'; }
    if (stop || valid || de_glued || run_edge_token(bare) ||
        (marked && (join_stop_word(bare) || call_ok(dict.get(), bare)))) {
        run_close(marked ? '\0' : lone_stop_letter(tok));   // an edge
        run_clear();
        if (!marked) { run_lead = lone_stop_letter(tok); }
    } else {
        run_take(bare, de_now, cq_now);
        if (marked) {
            run_close(0);
            run_clear();
        }
    }

    // DE-strip — the other half of the degenerate-fist fix: the gap between
    // DE and the call collapses too and the marker arrives GLUED on
    // ("DEEA1EYL", live-caught 2026-07-16). Strictly a fallback: fires only
    // when the whole token is NOT a valid call itself (a real DE1ABC stays
    // whole, and so does every cleanly keyed token) and the remainder
    // validates; the call inherits the full DE marker, exactly as if the gap
    // had been keyed.
    if (de_glued) {
        cand_add(tok + 2, 1, true, cq_now);
        prev_tok.assign(tok + 2);
        prev_valid = true;
        prev_de    = true;
        prev_cq    = cq_now;
        prev_gap   = false;
        de_pending = 0;
        return;
    }

    prev_tok.assign(tok);
    prev_valid = valid;
    prev_de    = de_now;
    prev_cq    = cq_now;
    prev_gap   = false;

    if (!valid) {
        // A garbled token between DE and the call must not eat the marker
        // ("DE R OK1BR") — but the marker does not live forever either.
        if (de_pending > 0) { de_pending--; }
        return;
    }

    cand_add(tok, 1, de_now, cq_now);
    de_pending = 0;
}

// SKIM_CALL_DESPACE=1 — the torn run, the fused run's mirror. A fist whose
// character gaps stretch to a word gap's length comes out with spaces
// INSIDE the call: "CQ CQ CQ D E G 0WD T G 0WD T G 0WD T P SE K", "Y O4 BE
// X Y O4 BE X" (live recordings 2026-10-15/16: 15 % of the stations CW
// Skimmer spotted and L missed). At the over break the over is read once
// more with its spaces dropped, the fused-run rule applied after every
// calling or sender marker in it: a call repeating back to back becomes a
// candidate — unless the over already held it as a token of its own,
// which the plain path has counted.

void SkimCallsignExtractorState::despace_scan() {
    const char *s = over.c_str();
    const size_t n = over.size();
    for (size_t i = 0; i + 5 < n;) {
        if (strncmp(s + i, "CQ", 2) != 0 && strncmp(s + i, "DE", 2) != 0 &&
            strncmp(s + i, "TEST", 4) != 0) {
            i++;
            continue;
        }
        bool cq = false, de = false;
        const size_t h = fused_markers(cfg, s + i, &cq, &de);
        char call[CALL_BUF_LEN];
        unsigned reps = 0;
        const size_t l = fused_repeat(dict.get(), s + i + h, n - i - h, call, &reps);
        if (!l) {
            i += std::max(h, static_cast<size_t>(1));
            continue;
        }
        char key[CALL_BUF_LEN + 2];
        std::snprintf(key, sizeof(key), " %s ", call);
        if (!strstr(over_toks.c_str(), key)) {
            if (cfg.despace_debug) {
                std::fprintf(stderr, "despace: %s this%u%s%s\n", call, reps, cq ? " cq" : "", de ? " de" : "");
            }
            for (unsigned k = 0; k < std::min(reps, 3u); k++) { cand_add(call, 2, de, cq); }
            over_toks += key + 1;   // once per over
        }
        i += h + l * reps;
    }
    over.clear();
    over_toks = " ";
}

// Keep the over's text (spaces dropped) and its tokens; scan at the break.
void SkimCallsignExtractorState::despace_note(const char *tok) {
    const char *mark = strstr(tok, "\xC2\xB7");
    const size_t n = mark ? static_cast<size_t>(mark - tok) : strlen(tok);
    if (n) {
        over.append(tok, n);
        over_toks.append(tok, n);
        over_toks += ' ';
    }
    if (mark || over.size() > kMaxOverBytes) { despace_scan(); }
}

void SkimCallsignExtractor::feed(const char *text) {
    SkimCallsignExtractorState *x = m_x.get();
    x->dict = x->source ? x->source->snapshot() : nullptr;
    for (const char *p = text; *p; p++) {
        const char c = *p;
        if (c == SkimCallsign::kFixOpen) {         // the decoder's correction:
            x->tok.clear();            // the raw word is replaced
            continue;
        }
        if (c == SkimCallsign::kFixClose)
            continue;
        if (c == ' ' || c == '\n' || c == '\t') {
            if (x->tok.size()) {
                // a glued CQ chain may run past a call's length ("CQCQCQDEON4LDL")
                if (x->tok.size() < CALL_BUF_LEN || cq_glued_head(x->dict.get(), x->cfg, x->tok.c_str()) ||
                    (x->cfg.fused && x->tok.size() >= 8)) {
                    x->take_token(x->tok.c_str());
                }
                if (x->cfg.despace) { x->despace_note(x->tok.c_str()); }
                x->tok.clear();
            }
            continue;
        }
        x->tok += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (x->tok.size() > (x->cfg.fused ? SkimCallsignExtractorState::kMaxFusedLength : TOKEN_BUF_LEN)) { x->tok.clear(); }
    }
}

// The leading candidate, threshold aside (best_ex and top share it).
const Cand *SkimCallsignExtractorState::best_pick(double *score) const {
    double best = 0.0;
    const Cand *bc = nullptr;
    for (unsigned i = 0; i < ncand; i++) {
        // A call glued from three or more fragments has to REPEAT before it is
        // believed: a torn fist tears every time, a garble does not — and the
        // run carries its DE/CQ markers, so a one-off would go straight out.
        // Two-token joins keep their old standing.
        if (cand[i].parts > 2 && cand[i].count < 2)
            continue;
        double s = cand_score(&cand[i]);
        // Tie goes to the LONGER call: a torn fragment ("EA3I") and its join
        // ("EA3IXQ") both max the score once repeated — the join is the call.
        if (s > best ||
            (s == best && bc && cand[i].call.size() > bc->call.size())) {
            best = s;
            bc = &cand[i];
        }
    }
    *score = best;
    return bc;
}

std::optional<SkimCallsignCand> SkimCallsignExtractor::top() const {
    m_x->dict = m_x->source ? m_x->source->snapshot() : nullptr;
    const SkimCallsignExtractorState *x = m_x.get();
    double s = 0.0;
    const Cand *c = x ? x->best_pick(&s) : nullptr;
    if (!c || s <= 0.0)
        return std::nullopt;
    SkimCallsignCand out{};
    out.call = c->call;
    out.score       = s;
    out.count       = c->count;
    out.parts       = c->parts;
    out.idle_tokens = x->tok_n - c->last_tok;
    out.de_marked   = c->de_marked;
    out.cq_context  = c->calling();
    out.dict        = knows(x->dict.get(), c->call.c_str());
    out.last_us     = c->last_us;
    return out;
}

void SkimCallsignExtractor::set_now(int64_t now_us) {
    SkimCallsignExtractorState *x = m_x.get();
    if (x) { x->now_us = now_us; }
}

int64_t SkimCallsignExtractor::last_heard(const char *call) const {
    const SkimCallsignExtractorState *x = m_x.get();
    if (!x || !call)
        return -1;
    for (unsigned i = 0; i < x->ncand; i++) {
        if (x->cand[i].call == call) { return x->cand[i].last_us; }
    }
    return -1;
}

SkimCallsignBest SkimCallsignExtractor::best() {
    m_x->dict = m_x->source ? m_x->source->snapshot() : nullptr;
    const SkimCallsignExtractorState *x = m_x.get();
    double best = 0.0;
    const Cand *bc = x->best_pick(&best);
    SkimCallsignBest r;
    r.cq_context = bc ? bc->calling() : false;
    if (!bc || best < SkimCallsignExtractor::kSpotThreshold)
        return r;
    r.call = bc->call;
    r.score = best;
    return r;
}

bool SkimCallsignExtractor::addressed(const char *call) const {
    const SkimCallsignExtractorState *x = m_x.get();
    if (!x || !call)
        return false;
    for (unsigned i = 0; i < x->ncand; i++) {
        const Cand *c = &x->cand[i];
        if (c->call == call) {
            return c->cq_context && !c->calling();
        }
    }
    return false;
}

unsigned SkimCallsignExtractor::hearings(const char *call) const {
    const SkimCallsignExtractorState *x = m_x.get();
    if (!x || !call)
        return 0;
    for (unsigned i = 0; i < x->ncand; i++) {
        if (x->cand[i].call == call) { return x->cand[i].count; }
    }
    return 0;
}

SkimCallsignBest SkimCallsignExtractor::extract(const char *text,
                                                std::shared_ptr<const SkimCallsignDictSource> dict,
                                                const SkimCallsignConfig &cfg) {
    SkimCallsignExtractor x(std::move(dict), cfg);
    x.feed(text);
    x.feed(" ");                                 // flush the last token
    x.m_x->run_close(0);                          // the end of the text: an edge
    return x.best();
}
