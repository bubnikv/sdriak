/*
 * skimmer-call-test — offline gate for callsign extraction/validation (M4).
 *
 * Three layers:
 *   - validity: structural + ITU allocation on hand-picked calls (rare
 *     prefixes must pass, decode-garbage shapes must die — T1BR, 5NN, Q…),
 *   - a labelled corpus of realistic decoder output lines (with the error
 *     patterns decode_cw actually makes) → PRECISION MUST BE 1.0, recall
 *     ≥ 0.9 — the RBN rule is "never spot garbage",
 *   - fuzz: E/T-biased noise-decode babble and random alnum tokens must
 *     never reach the spot threshold (seeded, deterministic).
 * Plus mechanics: token continuity across fragmented feeds, DE-marker
 * behaviour, dictionary boost, candidate eviction.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/skim_common.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/callsign.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

// SDRIAK: the gate on the standard library (upstream: GLib). GLib's GRand
// is MT19937 with init_genrand seeding — std::mt19937 is that generator,
// seeded the same way — and rand_int_range() is rand_int_range()'s own
// rejection sampling on it, so the fuzz tokens are upstream's.
typedef std::mt19937 Rng;

static int32_t rand_int_range(Rng *r, int32_t begin, int32_t end) {
    const uint32_t dist = static_cast<uint32_t>(end) - static_cast<uint32_t>(begin);
    uint32_t random = 0;
    if (dist) {
        uint32_t maxvalue;                     // below the greatest multiple
        if (dist <= 0x80000000u) {             // of dist that fits in 2^32
            uint32_t leftover = (0x80000000u % dist) * 2;
            if (leftover >= dist) { leftover -= dist; }
            maxvalue = 0xffffffffu - leftover;
        } else {
            maxvalue = dist - 1;
        }
        do { random = static_cast<uint32_t>((*r)()); } while (random > maxvalue);
        random %= dist;
    }
    return static_cast<int32_t>(static_cast<uint32_t>(begin) + random);
}

static std::string tmp_file(const char *name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

static void write_file(const std::string &path, const std::string &text) {
    std::ofstream(path, std::ios::binary) << text;
}

static int fails, checks;
static void check(const char *what, int ok) {
    checks++;
    if (!ok) { fails++; }
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// Load a dictionary into a source; false (and the source as it was) when the
// file cannot be read.
static bool load_into(SkimCallsignDictSource &src, const char *path, std::string *err = nullptr) {
    std::shared_ptr<const SkimCallsignDict> d = SkimCallsignDict::load(path, err);
    if (!d)
        return false;
    src.publish(std::move(d));
    return true;
}

// Upstream's skim_callsign_extract() shape over SkimCallsignExtractor::extract().
static double extract_into(const char *text, char *got, size_t n,
                           std::shared_ptr<const SkimCallsignDictSource> dict,
                           const SkimCallsignConfig &cfg) {
    const SkimCallsignBest b = SkimCallsignExtractor::extract(text, std::move(dict), cfg);
    skim_strlcpy(got, b.call.c_str(), n);
    return b.score;
}

// Upstream's best()/best_ex() shape over SkimCallsignExtractor::best(): the
// call into got (n bytes), the calling flag into *cq, the score returned.
static double best_into(SkimCallsignExtractor *x, char *got, size_t n, bool *cq = nullptr) {
    const SkimCallsignBest b = x->best();
    skim_strlcpy(got, b.call.c_str(), n);
    if (cq) { *cq = b.cq_context; }
    return b.score;
}

// Live-swap probe: readers look one call up in a tight loop while main()
// reloads the dictionary under them.
struct SwapProbe {
    std::shared_ptr<const SkimCallsignDictSource> scp;
    std::atomic<int> stop{0};
    std::atomic<int> lookups{0}, misses{0};
};

static void swap_reader(SwapProbe *sp) {
    while (!sp->stop.load()) {
        bool has = sp->scp->snapshot()->has("HB9CV");
        sp->lookups++;
        if (!has) { sp->misses++; }
    }
}

int main() {
    // The gate's dictionary: empty until its section loads one.
    const auto scp = std::make_shared<SkimCallsignDictSource>();
    // The extractor's switches: upstream's SKIM_CALL_* variables, as before.
    const SkimCallsignConfig ccfg = SkimCallsignConfig::from_env();
    printf("=== callsign gate (offline) ===\n");

    // -- validity: must accept ------------------------------------------------
    static const char *GOOD[] = {
        "OK1BR", "W1AW", "K1A", "DL1ABC", "LZ2PP", "UA9CDC", "F5IN", "G3XYZ",
        "B1HQ", "9A1AA", "2E0ABC", "3DA0RS", "E73ABC", "T77XX", "C6AGU",
        "VK6ABC", "ZL4AA", "5B4AH", "9V1AB", "OK1BR/P", "OK1BR/4", "F/OK1BR",
        "HB9CV", "OL2025X", "SP9XYZ", "PY2ABC", "OK1BR/QRP", "W1AW/CA",
        "DL/OK1BR", "OK1BR/MM",
    };
    int good_ok = 0;
    for (unsigned i = 0; i < std::size(GOOD); i++) {
        if (SkimCallsign::is_valid(GOOD[i])) {
            good_ok++;
        } else {
            printf("       rejected valid: %s\n", GOOD[i]);
        }
    }
    check("accepts real callsigns incl. rare prefixes (30/30)",
          good_ok == static_cast<int>(std::size(GOOD)));

    // -- validity: must reject -------------------------------------------------
    static const char *BAD[] = {
        "T1BR",                      // the decoder's classic O→T truncation
        "QRL", "QRZ1X", "Q1AB",      // Q is Q-codes, never a call
        "5NN", "599", "73",          // exchange babble
        "TEST", "TU", "K", "AGN",    // letters-only / too short
        "E", "EEE", "TTT",           // noise decodes
        "OK1", "OKABC", "1ABC",      // no suffix / no digit / 0-1 first
        "0K1BR", "OK1BR2",           // zero-first, suffix ends in a digit
        "OK1BRXYZQ",                 // too long
        "OK1BR/P/M",                 // two designators
        "F5TLF/P+", "HB9TVK/P+K",    // designators read off glued text
        "SM7FBJ/PIN", "PHB/SM7FBJ",  // three letters are only ever QRP
    };
    int bad_ok = 0;
    for (unsigned i = 0; i < std::size(BAD); i++) {
        if (!SkimCallsign::is_valid(BAD[i])) {
            bad_ok++;
        } else {
            printf("       accepted garbage: %s\n", BAD[i]);
        }
    }
    check("rejects garbage and decode-error shapes (25/25)",
          bad_ok == static_cast<int>(std::size(BAD)));

    // -- labelled corpus: realistic decoder lines ------------------------------
    static const struct { const char *text, *expect; } CORPUS[] = {
        { "CQ TEST DE OK1BR OK1BR K",            "OK1BR"  },
        { "UV CQ TEST DE OK1BR OK1BR K",         "OK1BR"  },  // warmup garble
        { "CQ CQ DE DL1ABC DL1ABC PSE K",        "DL1ABC" },
        { "CQ DE W1AW K",                        "W1AW"   },  // single, DE
        { "TEST OK5Z OK5Z TEST",                 "OK5Z"   },  // repeat, no DE
        { "DE R OK1BR OK1BR",                    "OK1BR"  },  // garble after DE
        { "T1BR OK1BR OK1BR",                    "OK1BR"  },  // err then good
        { "CQ DE 9A1AA 9A1AA",                   "9A1AA"  },  // digit-first
        { "QRL QSY DE F5IN K",                   "F5IN"   },  // q-codes around
        { "CQ DE E73ABC E73ABC K",               "E73ABC" },  // letter+digit px
        { "TU 5NN 73 GL",                        nullptr     },  // exchange only
        { "E EEE T TTT EE",                      nullptr     },  // noise babble
        { "VVV VVV VVV",                         nullptr     },  // tune-up
        { "OK1XX",                               nullptr     },  // lone, no ctx
        { "CQ TEST DE OK1BR/P OK1BR/P",          "OK1BR/P"},  // portable
    };
    int tp = 0, fp = 0, fn = 0;
    for (unsigned i = 0; i < std::size(CORPUS); i++) {
        char got[32];
        double s = extract_into(CORPUS[i].text, got, sizeof(got), scp, ccfg);
        if (CORPUS[i].expect) {
            if (s > 0 && strcmp(got, CORPUS[i].expect) == 0) {
                tp++;
            } else if (s > 0) {
                fp++;
                printf("       [%u] wrong call: \"%s\" -> %s (want %s)\n",
                       i, CORPUS[i].text, got, CORPUS[i].expect);
            } else {
                fn++;
                printf("       [%u] missed: \"%s\" (want %s)\n",
                       i, CORPUS[i].text, CORPUS[i].expect);
            }
        } else if (s > 0) {
            fp++;
            printf("       [%u] false spot: \"%s\" -> %s (%.2f)\n",
                   i, CORPUS[i].text, got, s);
        }
    }
    int npos = 0;
    for (unsigned i = 0; i < std::size(CORPUS); i++) {
        if (CORPUS[i].expect) { npos++; }
    }
    printf("       corpus: %d TP, %d FP, %d FN over %d positives\n",
           tp, fp, fn, npos);
    check("corpus precision = 1.0 (not one false spot)", fp == 0);
    check("corpus recall ≥ 0.9", tp >= static_cast<int>(ceil(0.9 * npos)));

    // -- token continuity across fragmented feeds ------------------------------
    {
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQ TE");
        x->feed("ST DE OK1B");
        x->feed("R OK1BR K ");
        char got[32];
        double s = best_into(x, got, sizeof(got));
        check("token fragments survive across feed() calls",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "OK1BR") == 0);
        x->reset();
        check("reset clears candidates",
              best_into(x, got, sizeof(got)) == 0.0);
        delete x;
    }

    // -- sloppy fist: a stretched gap tears the call into two tokens -------------
    {
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        // EA3IXQ sent with a too-wide gap before the last two letters — the join
        // hypothesis must reassemble it (live-caught 2026-07-15: spotted EA3I).
        x->feed("CQ TEST DE EA3I XQ CQ TEST DE EA3I XQ K ");
        char got[32];
        double s = best_into(x, got, sizeof(got));
        check("split call re-joins across a sloppy gap (EA3I XQ → EA3IXQ)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "EA3IXQ") == 0);
        delete x;

        // A QSB dip fires the decoder's over-break mark MID-CALL — it is
        // metadata and must stay transparent to the join ("LZ67 · PP").
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed(
            "CQ DE LZ67 \xC2\xB7 PP CQ DE LZ67 \xC2\xB7 PP K ");
        s = best_into(x, got, sizeof(got));
        check("over-break mark is transparent (LZ67 · PP → LZ67PP)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "LZ67PP") == 0);
        delete x;

        // The prosign K must NOT glue onto the call (OK1BRK is a valid shape).
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQ DE OK1BR K CQ DE OK1BR K ");
        s = best_into(x, got, sizeof(got));
        check("prosign K does not join onto the call (no OK1BRK phantom)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "OK1BR") == 0);
        delete x;
    }

    // -- hearings: copies read, the RBN feed's second gate --------------------------
    {
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        char got[32];
        // ONE copy after CQ DE already clears the feed's 0.85 score …
        x->feed("CQ DE SM7XYZ K ");
        double s = best_into(x, got, sizeof(got));
        check("one copy after CQ DE scores past 0.85 (why hearings exist)",
              s >= 0.85 && strcmp(got, "SM7XYZ") == 0);
        check("…but counts as ONE hearing",
              x->hearings("SM7XYZ") == 1);
        x->feed("CQ DE SM7XYZ SM7XYZ K ");
        check("two more copies: three hearings",
              x->hearings("SM7XYZ") == 3);
        check("a call that is no candidate: zero hearings",
              x->hearings("OK1BR") == 0);
        delete x;
    }

    // -- calling context: leading and trailing markers ----------------------------
    {
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        char got[32];
        bool cq = false;
        // contest-style TRAILING marker only: "SD1A TEST SD1A TEST"
        x->feed("SD1A TEST SD1A TEST ");
        double s = best_into(x, got, sizeof(got), &cq);
        check("trailing TEST marks the call as CALLING",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "SD1A") == 0 && cq);
        delete x;

        // a runner closing QSOs with "TU <call>" owns the frequency too
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("5NN TU M0NGN 5NN TU M0NGN ");
        s = best_into(x, got, sizeof(got), &cq);
        check("leading TU marks the runner as CALLING (TU M0NGN)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "M0NGN") == 0 && cq);
        delete x;

        // an S&P answer has no calling marker at all
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("EA2BTN EA2BTN 5NN 73 EA2BTN EA2BTN ");
        s = best_into(x, got, sizeof(got), &cq);
        check("an S&P answer is NOT flagged as calling",
              strcmp(got, "EA2BTN") == 0 && !cq);
        delete x;

        // A caller after the runner's TEST/TU, past the over break, inherits
        // that window — the report the runner sends it takes it back (tap,
        // 2026-10-10: "TEST DL4KWA E TEST · P PE2K · PE2K GT 5NN").
        static const struct { const char *text, *call; bool calling; const char *what; }
        addr[] = {
            { "TEST DL4KWA TEST · PE2K PE2K 5NN ", "PE2K", false,
                "caller past the over break, then 5NN → not calling (PE2K)" },
            { "5NN TU · RM5Z RM5Z 599 ", "RM5Z", false,
                "caller after TU · then 599 → not calling (RM5Z)" },
            { "TEST DL4KWA TEST · PE2K PE2K ", "PE2K", true,
                "no report: the window still counts (PE2K, today's rule)" },
            { "TEST DL4KWA TEST · PE2K PE2K · 5NN ", "PE2K", true,
                "an over break between the call and the report → still calling" },
            { "CQ TEST UR3VZ UR3VZ 5NN15 ", "UR3VZ", true,
                "a clean CQ window wins over a report (UR3VZ calling)" },
        };
        for (unsigned i = 0; i < std::size(addr); i++) {
            x = new SkimCallsignExtractor(scp, ccfg);
            x->feed(addr[i].text);
            cq = !addr[i].calling;
            s = best_into(x, got, sizeof(got), &cq);
            check(addr[i].what, strcmp(got, addr[i].call) == 0 && cq == addr[i].calling &&
                                x->addressed(addr[i].call) ==
                                    !addr[i].calling);
            delete x;
        }
    }

    // -- degenerate fist: gaps around the markers collapse (2026-07-16) ----------
    {
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        char got[32];
        bool cq = false;
        // EA1EYL live: word ≈ letter gaps, the CQ chain fuses into one token and
        // DE glues onto the call — both fallbacks must fire, and the call must
        // carry the CALLING flag (the cq_only feed policy keys off it).
        x->feed("CQCQCQ DEEA1EYL CQCQCQ DEEA1EYL K ");
        double s = best_into(x, got, sizeof(got), &cq);
        check("fused chain: CQCQCQ DEEA1EYL → EA1EYL, flagged calling",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "EA1EYL") == 0 && cq);
        delete x;

        // the stripped call inherits the full DE marker — a single hearing
        // scores exactly like a cleanly keyed "CQ DE OK1BR"
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQCQ DEOK1BR ");
        s = best_into(x, got, sizeof(got), &cq);
        check("DE-strip inherits the marker (CQCQ DEOK1BR spots in one hearing)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "OK1BR") == 0 && cq);
        delete x;

        // a token that IS a valid call must never be stripped — the fallback
        // only runs where the normal path fails (machine keying untouched)
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQ DE DE1ABC DE1ABC K ");
        s = best_into(x, got, sizeof(got));
        check("valid DE-prefixed call stays whole (DE1ABC never stripped)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "DE1ABC") == 0);
        delete x;

        // CQ-strip: the marker glued onto DE / DE+call / the call (tap of
        // 2026-10-04..06 — CQDE, CQCQCQDEIZ8BRI, CQDXDEF8GFA; one past a call's
        // length) — the call must come out flagged calling
        {
            static const struct { const char *text, *call; } glued[] = {
                { "CQDE IQ6MW IQ6MW K ", "IQ6MW" },
                { "CQCQCQDEIZ8BRI IZ8BRI IZ8BRI K ", "IZ8BRI" },
                { "CQDXDEF8GFA F8GFA K ", "F8GFA" },
                { "CQCQCQCQDEON4LDL ON4LDL K ", "ON4LDL" },
                { "CQTEST SF6W SF6W ", "SF6W" },
            };
            for (unsigned i = 0; i < std::size(glued); i++) {
                x = new SkimCallsignExtractor(scp, ccfg);
                x->feed(glued[i].text);
                cq = false;
                s = best_into(x, got, sizeof(got), &cq);
                char what[96];
                std::snprintf(what, sizeof(what), "CQ-strip: %s→ %s, flagged calling",
                           glued[i].text, glued[i].call);
                check(what, s >= SkimCallsignExtractor::kSpotThreshold &&
                            strcmp(got, glued[i].call) == 0 && cq);
                delete x;
            }
        }

        // a valid call that begins with CQ (Portugal) is never stripped
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQ DE CQ7MEL CQ7MEL K ");
        s = best_into(x, got, sizeof(got));
        check("valid CQ-prefixed call stays whole (CQ7MEL never stripped)",
              !SkimCallsign::is_valid("CQ7MEL") ||
              (s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "CQ7MEL") == 0));
        delete x;

        // a glued CQ in front of garble flags nothing: no remainder that is DE,
        // DE + a call or a call — and an answer after it is not blessed
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("CQDEM CQXYZ CQC EA2BTN EA2BTN 5NN ");
        cq = false;
        s = best_into(x, got, sizeof(got), &cq);
        check("glued CQ before garble flags nothing (EA2BTN not calling)",
              strcmp(got, "EA2BTN") == 0 && !cq);
        delete x;

        // strict shapes only: DE-glued garbage and almost-CQ runs all die
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("DEEE DE5NN DETEST CQC CQCQC ");
        s = best_into(x, got, sizeof(got));
        check("DE-glued garbage and non-CQ runs never spot", s == 0.0);
        delete x;

        // the TORN twin: stretched gaps tear the chain into single letters
        // ("C Q C Q C Q DE EA1EYL", live-caught 2026-07-16 same evening)
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("C Q C Q C Q DE EA1EYL EA1EYL K ");
        s = best_into(x, got, sizeof(got), &cq);
        check("letter-spaced chain: C Q C Q → CQ marker (EA1EYL calling)",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "EA1EYL") == 0 && cq);
        delete x;

        // one pair is no marker, and interruptions reset the chain
        x = new SkimCallsignExtractor(scp, ccfg);
        x->feed("C Q DE OK1XX OK1XX C C Q Q C E Q ");
        s = best_into(x, got, sizeof(got), &cq);
        check("one C Q pair / broken chains never open the CQ window",
              s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "OK1XX") == 0 && !cq);
        delete x;
    }

    // -- the torn call (gh#3) -----------------------------------------------------
    {
        // 14039 kHz, 2026-09-11 18:49-18:57, eight minutes of pane text as the
        // decoder wrote it: a fist that leaves a word gap after every group.
        // Fed token by token, the way the pipeline asks after every token —
        // the head UA6H must never be the best candidate, not for one token.
        static const char LIVE[] =
          "\xC2\xB7" "  \xC2\xB7" "  Z \xC2\xB7" " T U T U S K EE EE \xC2\xB7" "  NC N T \xC2\xB7" " C Q C Q + Q CQ C Q \xC2\xB7" " C Q "
          "\xC2\xB7" " CQ CQ DE \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H NU E UA 6 H NU CQ CQ CQ CQ RQDE \xC2\xB7" " "
          "UA6 H NU \xC2\xB7" " UA6 H NU \xC2\xB7" " EA6 H NU PSEK \xC2\xB7" " C Q \xC2\xB7" " C Q C Q C M C Q C Q \xC2\xB7" " "
          "C Q CQ C Q DE \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H \xC2\xB7" " T \xC2\xB7" " UA E\xC2\xB7" " H  N U \xC2\xB7" " U\xC2\xB7" " 6\xC2\xB7" " N U "
          "F\xC2\xB7" " Q CQ D\xC2\xB7" " T 6 H N U \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H NU PSEK \xC2\xB7" " CQ \xC2\xB7" " CQ \xC2\xB7" " CQ "
          "\xC2\xB7" " CN E\xC2\xB7" " CQ CQ CQ CQ DE \xC2\xB7" " UA6 H NU \xC2\xB7" " TUA6 H NU \xC2\xB7" " UA6 H NU CQ CQ CQ "
          "CQTCQDE \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA6 H NU \xC2\xB7" " UA 6 H NU PSEK \xC2\xB7" " ? EE \xC2\xB7" " CQ CQ CQ "
          "CQ DE \xC2\xB7" " UA6 H NU \xC2\xB7" " UA6 5 NU \xC2\xB7" " UA6 H NU PSEK \xC2\xB7" " C Q \xC2\xB7" " C Q \xC2\xB7" " C Q C Q "
          "C Q C Q D E \xC2\xB7" " T 6 H N U \xC2\xB7" " U A 6 H NU \xC2\xB7" " UA 6 H NU CQ CQ CQ \xC2\xB7" " CQ DE "
          "\xC2\xB7" " UA6 H NU \xC2\xB7" " UA\xC2\xB7" " H NU \xC2\xB7" " UA6 H NU \xC2\xB7" " UA \xC2\xB7" " I N U PSEK \xC2\xB7" " CQ \xC2\xB7" " CQ \xC2\xB7" " CQ "
          "\xC2\xB7" " CQ \xC2\xB7" " CQ DE \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H NU PSEK "
          "\xC2\xB7" " CQ CQ CQ CQ CQ DE \xC2\xB7" " UA6 H NU \xC2\xB7" " I UKH\xC2\xB7" " A6X B IKN U5 N\xC2\xB7" " XB A \xC2\xB7" " A "
          "6 K5HN XN B U IKC5  \xC2\xB7" " Q CQ CQ O C\xC2\xB7" " Q CQ DE \xC2\xB7" " UA 6 H NU \xC2\xB7" " UA 6 H "
          "NU \xC2\xB7" " UA6 H NU \xC2\xB7" " UA B \xC2\xB7" " N U \xC2\xB7" " UA 6 H NU PSEK \xC2\xB7" " ";
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        char   got[32];
        unsigned  head = 0, other = 0, whole = 0;
        std::vector<std::string> tok;          // LIVE split on ' ', as g_strsplit
        for (size_t at = 0;;) {
            const size_t sp = strchr(LIVE + at, ' ') ? static_cast<size_t>(strchr(LIVE + at, ' ') - LIVE)
                                                     : strlen(LIVE);
            tok.emplace_back(LIVE + at, sp - at);
            if (!LIVE[sp]) { break; }
            at = sp + 1;
        }
        for (const std::string &t : tok) {
            if (t.empty()) { continue; }
            x->feed(t.c_str());
            x->feed(" ");
            if (best_into(x, got, sizeof(got)) <= 0) { continue; }
            if (strcmp(got, "UA6HNU") == 0) { whole++; }
            else if (strcmp(got, "UA6H") == 0) { head++; }
            else { other++; }
        }
        delete x;
        printf("       live stream: UA6HNU best at %u tokens, UA6H at %u, other at %u\n",
               whole, head, other);
        check("live torn fist: UA6HNU is the call", whole >= 100);
        check("live torn fist: the head UA6H is never the best candidate", head == 0);
        check("live torn fist: nothing else is either", other == 0);

        static const struct { const char *text, *want, *what; } TORN[] = {
            { "CQ CQ DE UA6 H NU UA6 H NU PSE K ", "UA6HNU",
                "three pieces, twice, one over" },
            { "CQ CQ DE UA 6 H NU UA 6 H NU PSE K ", "UA6HNU",
                "four pieces, twice — no edge between the repeats" },
            { "CQ DE U A 6 H N U \xC2\xB7 U A 6 H N U PSE K ", "UA6HNU",
                "letter by letter, an over break between the repeats" },
            { "DE UA 6 H NU\xC2\xB7 UA 6 H NU\xC2\xB7 K ", "UA6HNU",
                "the over mark glued onto the last piece" },
            { "5NN 7 79 OL7 ABS K 5NN 79 OL7 ABS K ", "OL7ABS",
                "a serial in front of the call is an edge, not a piece" },
            { "DE LZ67 \xC2\xB7 PP K ", "LZ67PP",
                "an over break INSIDE a call still joins (2026-07-15)" },
        };
        for (unsigned i = 0; i < std::size(TORN); i++) {
            double s = extract_into(TORN[i].text, got, sizeof(got), scp, ccfg);
            if (strcmp(got, TORN[i].want) != 0) {
                printf("       \"%s\" → \"%s\" %.2f\n", TORN[i].text, got, s);
            }
            check(TORN[i].what,
                  s >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, TORN[i].want) == 0);
        }

        // what must NOT be glued — "" = nothing reaches the spot threshold
        static const struct { const char *text, *want, *what; } PHANTOM[] = {
            { "CQ DE UA6 H NU I UKH A6X CQ DE UA6 H NU I XB A ", "",
                "stray pieces after the call: the run is not a call, no UA6HNUI" },
            { "DE UA 6 H NU K ", "",
                "three and more pieces heard ONCE are not believed yet" },
            { "CQ SP 6OS SP 6OS SP K CQ SP 6OS SP 6OS SP K ", "",
                "the first piece of a repeat is no tail (no SP6OSSP)" },
            { "OK1BR PSE K UA6 H OK1BR PSE K UA6 H ", "OK1BR",
                "a run never crosses a stop word or a whole call" },
            { "I A N S M 5 S I A N M 5 S I A N S M 5 S I A ", "",
                "thirteen and more pieces are babble, whatever they spell" },
            { "TEST E A2 D DC E A2 D DC E A2 D DC ", "",
                "a lone E before the run may be its first letter (EA2DDC, not A2DDC)" },
            { "DE F 4 I K C F 4 I K C ", "",
                "a lone K after a SPELLED run may be a letter of the call (F4IKC)" },
            { "CQ DE UA 6 H K CQ DE UA 6 H K ", "UA6H",
                "control: a short call keyed the same way IS glued" },
        };
        for (unsigned i = 0; i < std::size(PHANTOM); i++) {
            double s = extract_into(PHANTOM[i].text, got, sizeof(got), scp, ccfg);
            if (strcmp(got, PHANTOM[i].want) != 0) {
                printf("       \"%s\" → \"%s\" %.2f\n", PHANTOM[i].text, got, s);
            }
            check(PHANTOM[i].what, strcmp(got, PHANTOM[i].want) == 0);
        }
    }

    // -- dictionary boost --------------------------------------------------------
    {
        char got[32];
        double lone = extract_into("HB9CV", got, sizeof(got), scp, ccfg);
        check("a lone call without context is NOT spottable", lone == 0.0);

        const std::string dict = tmp_file("skimmer-call-dict.txt");
        write_file(dict, "# test dict\nHB9CV\nOK1BR\n");
        std::string err;
        check("dictionary loads (MASTER.SCP style)",
              load_into(*scp, dict.c_str(), &err) && scp->snapshot()->size() == 2);
        double boosted = extract_into("HB9CV", got, sizeof(got), scp, ccfg);
        check("dictionary hit lifts a lone call over the threshold",
              boosted >= SkimCallsignExtractor::kSpotThreshold && strcmp(got, "HB9CV") == 0);

        // The real file's head, byte for byte (CRLF, the "!!Order" directive,
        // the '#' block) — gh#7: the directive used to count as a call.
        write_file(dict,
                   "!!Order,1,1\r\n#\r\n# Super Check Partial\r\n"
                   "# Release 2026.09.18\r\n# Generated by bb-scp\r\n#\r\n"
                   "2D0OMN\r\nok1br/p\r\n<html>\r\nE\r\nC4W\r\n");
        check("SCP header: directive, comments and junk lines are not calls",
              load_into(*scp, dict.c_str(), &err) && scp->snapshot()->size() == 3);
        check("SCP header: \"!!Order,1,1\" stays out, the calls are in",
              !scp->snapshot()->has("!!ORDER,1,1") &&
              scp->snapshot()->has("2D0OMN") && scp->snapshot()->has("OK1BR/P") &&
              scp->snapshot()->has("C4W"));
        std::string rel = std::string(scp->snapshot()->release());
        check("the file's \"# Release\" comment is kept", rel == "2026.09.18");

        // inspect(): what a load WOULD take — on bytes as hostile as a download
        // can be: a NUL inside a line, a line with no end, an HTML tag.
        {
            std::string b("!!Order,1,1\r\n#  release 2026.01.02 \r\n"
                          "OK1BR\r\nhb9cv\r\n<html>\r\nQQ0QQQ\r\n");
            b.append("OK1\0BR\n", 7);
            for (int i = 0; i < 300; i++) { b += 'A'; }
            SkimCallsignDictInfo di;
            di = SkimCallsignDict::inspect(b);   // no final newline
            check("inspect: 3 call lines, 2 of them valid, 3 junk, release read",
                  di.calls == 3 && di.valid == 2 && di.junk == 3 &&
                  strcmp(di.release, "2026.01.02") == 0);
            check("inspect leaves the loaded dictionary alone",
                  scp->snapshot()->size() == 3 && scp->snapshot()->has("C4W"));
            di = SkimCallsignDict::inspect("");
            check("inspect: empty input is empty, not a crash",
                  di.calls == 0 && di.junk == 0 && !di.release[0]);
        }

        check("an unreadable file leaves the loaded dictionary as it was",
              !load_into(*scp, "/nonexistent/skimmer/master.scp", nullptr) &&
              scp->snapshot()->size() == 3 && scp->snapshot()->has("2D0OMN"));

        // Live swap: two threads look HB9CV up without a pause while the file is
        // reloaded 200 times. HB9CV is the LAST line of 20 000, so a loader that
        // empties the table and refills it in place leaves a long window where
        // the lookup misses (or reads freed memory) — every lookup must hit.
        {
            std::string big("# Release 2026.09.18\n");
            for (int i = 0; i < 20000; i++) {
                char line[16];
                std::snprintf(line, sizeof(line), "OK%d%c%c%c\n", i % 10, 'A' + i / 676 % 26,
                              'A' + i / 26 % 26, 'A' + i % 26);
                big += line;
            }
            big += "HB9CV\n";
            write_file(dict, big);
            load_into(*scp, dict.c_str());
            SwapProbe sp;
            sp.scp = scp;
            std::thread t1(swap_reader, &sp);
            std::thread t2(swap_reader, &sp);
            bool loads_ok = true;
            for (int i = 0; i < 200; i++) {
                loads_ok = load_into(*scp, dict.c_str()) && loads_ok;
            }
            sp.stop = 1;
            t1.join();
            t2.join();
            check("live swap: 200 reloads under two reader threads, no lookup misses",
                  loads_ok && sp.lookups > 1000 && sp.misses == 0);
        }

        // the sections below run with the two-call dictionary, as before
        write_file(dict, "# test dict\nHB9CV\nOK1BR\n");
        load_into(*scp, dict.c_str());
        rel = std::string(scp->snapshot()->release());
        check("a file without a release comment reports none", rel.empty());
        std::remove(dict.c_str());
    }

    // -- fuzz: E/T-biased noise babble -------------------------------------------
    {
        Rng *rng = new Rng(4711);
        static const char *NOISE = "EEEETTTISNAHM";   // CW noise letter bias
        SkimCallsignExtractor *x = new SkimCallsignExtractor(scp, ccfg);
        char got[32];
        int spots = 0;
        for (int t = 0; t < 4000; t++) {
            char tok[8];
            int len = rand_int_range(rng, 1, 6);
            for (int i = 0; i < len; i++) {
                tok[i] = NOISE[rand_int_range(rng, 0, static_cast<int>(strlen(NOISE)))];
            }
            tok[len] = ' ';
            tok[len + 1] = '\0';
            x->feed(tok);
            if (best_into(x, got, sizeof(got)) > 0) { spots++; }
        }
        check("4000 tokens of E/T noise babble: zero spots", spots == 0);
        delete x;

        // Random alnum tokens: a SINGLE mention of even structurally valid junk
        // must stay under the threshold (no DE, no repeats, no dict) — reset per
        // token, repetition of one call is legitimately spottable by design.
        x = new SkimCallsignExtractor(scp, ccfg);
        static const char *AL = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        spots = 0;
        for (int t = 0; t < 4000; t++) {
            char tok[10];
            int len = rand_int_range(rng, 2, 8);
            for (int i = 0; i < len; i++) {
                tok[i] = AL[rand_int_range(rng, 0, 36)];
            }
            tok[len] = ' ';
            tok[len + 1] = '\0';
            x->feed(tok);
            if (best_into(x, got, sizeof(got)) > 0) { spots++; }
            x->reset();
        }
        check("4000 random alnum tokens, single mentions: zero spots", spots == 0);
        delete x;
        delete rng;
    }

    printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
           fails ? "FAIL" : "PASS — precision holds, garbage dies, calls "
                            "get through.");
    return fails ? 1 : 0;
}
