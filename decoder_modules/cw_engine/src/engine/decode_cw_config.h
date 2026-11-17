/*
 * decode_cw_config.h — the CW v2/v3 decoders' tunables: one part per
 * consumer (each stage takes its own part), the v3-only parts under v3.
 *
 * SDRIAK: upstream reads each of these from its own environment variable,
 * lazily, into a function-local static (SKIM_V2_* for v2, SKIM_V3_* for v3,
 * SKIM_CW2_* / SKIM_CW3_* for the debug switches). Here a backend owns one
 * CwDecoderConfig, every channel it creates takes a copy, and the decoders
 * read their tunables from it. The defaults are upstream's defaults;
 * cw_config_from_env() reads upstream's variables, with upstream's parsing,
 * on top of a base — so every SKIM_* switch still works as upstream
 * documents it.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

class SkimOracle;
class CwRunDumpSink;

// --- diagnostics (both versions): the debug trace, the llr trace, the run
// dump, the oracle's kws probe ------------------------------------------------
struct CwDiagConfig {
    bool        debug = false;         // SKIM_CWn_DEBUG set: trace to stderr
    double      debug_hz = 0.0;        // its value: < 1000 = every channel, else
                                       // only the channel within 70 Hz of it
    bool        trace = false;         // SKIM_CWn_TRACE: per-sample llr trace
    std::shared_ptr<CwRunDumpSink> dump;  // SKIM_CW_DUMP_RUNS: the run dump (null
                                          // = off; CwRunDumpSink::from_env())
    bool        kws_dump = false;      // SKIM_ORACLE_KWS_DUMP (oracle mode kws)
    std::shared_ptr<const SkimOracle> oracle;  // SKIM_ORACLE: measurement only
                                               // (null = off; SkimOracle::from_env())
};

// --- the envelope (CwEnvelope) -------------------------------------------------
struct CwEnvelopeConfig {
    double narrow_fc = 45.0;           // SKIM_Vn_NARROW: carrier-centred envelope
                                       // low-pass, Hz (≤ 0 or ≥ 0.45·rate = off)
};

// --- the character assembly (CwAssembler) --------------------------------------
struct CwAssemblyConfig {
    bool   nearest = false;            // SKIM_Vn_NEAREST: invalid code → unique
                                       // neighbour character
};

// --- the lattice's priors and windows (CwSegmentPriors) -----------------------------
struct CwLatticeConfig {
    // SKIM_V3_EDGE=1 (off by default since v0.6.0): soft edges (keying shape,
    // filter smear, multipath) make every mark LONGER and every space
    // SHORTER by the same δ — v2 already tracks both (dit from marks, the
    // element space EMA from element spaces) and reports the unbiased
    // (dit + element space)/2 as the speed, but its priors put marks AND
    // spaces on the biased dit: element spaces of a strong, smeared signal
    // sit near 0.6 dit, under the ElementSpace floor, and two dits fuse
    // into a dah. It won on CQ WW 2012 and lost live: strong signals ~10
    // points (2026-10-13), so live air ran without it from then on and
    // v0.6.0 made that the default.
    bool   edge = false;               // SKIM_Vn_EDGE: the edge model everywhere
    // SKIM_V3_EDGE_WEAK=<dB>: the edge model on a weak channel only. Weak
    // signals come out of the envelope smeared — marks long, spaces short —
    // and a short character gap then reads as an element gap: HA3HS at 12 dB,
    // dits of 20 samples, element gaps of 14, character gaps of 29–31 = 1.5
    // biased dits, "CQ CQ DE HA3HS" read as QCSRQT (recording 2026-10-16). The
    // edge model fixes that, but switched on everywhere it cost busts and
    // strong signals (docs/LIVE-AIR.md), so it acts while the channel's
    // mark/space level ratio is under this many dB (3 dB hysteresis). Off
    // unless set.
    double edge_weak_db = 0.0;         // SKIM_Vn_EDGE_WEAK: … on channels under
                                       // this mark/space ratio, dB (0 = off)
    double dah_max = 5.0;              // SKIM_Vn_DAHMAX: dah window ceiling, dits
};

// --- the clock's re-lock (CwClock::mark) ----------------------------------------
struct CwClockConfig {
    double relock_db = 0.0;            // SKIM_Vn_RELOCK_DB: no faster re-lock
                                       // under this mark/space ratio (0 = off)
};

// --- v2's llr views (CwDiscriminator::llr) ---------------------------------------
struct CwLlrConfig {
    bool   rayfade = false;            // SKIM_Vn_RAYFADE "r0,r1[,wmin]"
    double rayfade_r0 = 4.0;
    double rayfade_r1 = 10.0;
    double rayfade_wmin = 0.0;
    bool   raysmear = true;            // SKIM_Vn_RAYSMEAR "s0,s1" ("0" = off)
    double raysmear_s0 = 0.08;
    double raysmear_s1 = 0.16;
    bool   raycap = false;             // SKIM_Vn_RAYCAP
};

// --- the MASTER.SCP word re-read (CwWordReread) ------------------------------------
struct CwRescoreConfig {
    bool   on = true;                  // SKIM_Vn_RESCORE: the word re-read ("0" =
                                       // off, "fit,sep,ev": the three below)
    // CQ WW 2012 (dev 40 m 02Z, test 40/80/160 m): fit 5 raised recall on
    // every band-hour and never raised the bust rate; 8 and 12 bought a
    // little more recall with more busts. Live air: the right corrections
    // sat at 0 … −4 (EC6AAE, MM0GOR, E70T), the wrong ones — real calls
    // the dictionary lacks — at −10 … −15; noise words under ev 2.0.
    double max_deficit = 5.0;          // "fit": how far a call may score under
                                       // the free reading
    double min_margin = 3.0;           // "sep": how far it must score over the
                                       // runner-up
    double min_evidence = 2.0;         // "ev": the free reading's llr per sample
                                       // a word needs to be re-read at all
    bool   debug = false;              // SKIM_Vn_RESCORE_DEBUG
    double min_evidence_weak = 0.25;   // SKIM_V3_RS_EV_WEAK: min_evidence in v3's
                                       // weak view (its llrs are nats)
};

// --- v3: the front end (CwFrontEnd) ------------------------------------------------
struct CwFrontEndConfig {
    // SKIM_V3_FRONT=v2 keeps v2's envelope path — the A/B of the front end alone.
    bool   on = true;                  // SKIM_V3_FRONT ("v2" = v2's envelope)
    // SKIM_V3_DET_DB: a carrier's prominence over the floor (6 dB) held for
    // SKIM_V3_DET_S (1 s) makes a candidate.
    double det_db = 6.0;               // SKIM_V3_DET_DB: carrier prominence
    double det_s = 1.0;                // SKIM_V3_DET_S: … held this long
    // SKIM_V3_WEAK_DB (SNR in 500 Hz, 8 — where v2 and v3 cross on the weak
    // ladder): below it the lattice reads the calibrated matched LLR; 3 dB
    // above it v2's own view again (hysteresis). v2 was tuned on strong live
    // air against CW Skimmer Server — its notch
    // and QSB handling stays where it was earned; the new statistic serves
    // the stations v2 loses (L of R 26-34 % under 10 dB).
    double weak_db = 8.0;              // SKIM_V3_WEAK_DB: weak view below, SNR500
};

// --- v3: the decoder in the weak view -------------------------------------------
struct CwWeakViewConfig {
    double dead_s = 0.3;               // SKIM_V3_DEAD_S: unlocked weak dead air
    // SKIM_V3_MIN_DB (SNR in 500 Hz, −4; −7 let dictionary words out of noise on
    // live air, EE3E/EE5M): a weak-view channel under it is dead
    // air — the ladder copies nothing below ~−2 dB, and noise measured against
    // its own peaks reads ~−9 dB.
    double min_db = -4.0;              // SKIM_V3_MIN_DB: dead air below, SNR500
    // SKIM_V3_PREROLL_X: the pre-roll's head-over-quiet bar on the matched
    // amplitude (2.5; v2's 5.6 on its envelope kept weak heads out on
    // purpose).
    double preroll_x = 2.5;            // SKIM_V3_PREROLL_X: weak head bar
    // SKIM_V3_FIST_HOLD_S: quiet before the fist is forgotten (30 s; v2: 8).
    double fist_hold_s = 30.0;         // SKIM_V3_FIST_HOLD_S: weak-view fist memory
};

// --- v3: the call reader (CwCallReader) --------------------------------------------
struct CwCallReaderConfig {
    // SKIM_V3_KWS=0 turns the reader off; SKIM_V3_KWS_DC (−5), SKIM_V3_KWS_SEP (3)
    // the bars; SKIM_V3_KWS_DEBUG=1 prints every read.
    bool   on = true;                  // SKIM_V3_KWS: the call reader ("0" = off)
    double min_score = -5.0;           // SKIM_V3_KWS_DC: a call's score against
                                       // the free reading (≤ 0) must reach this
    double min_margin = 3.0;           // SKIM_V3_KWS_SEP: … and beat the runner-up
                                       // by this much
    bool   debug = false;              // SKIM_V3_KWS_DEBUG
};

// --- v3 only (docs/CW-V3.md) -----------------------------------------------------
struct CwV3Config {
    bool   robust_clock = true;        // v3's three clock fixes, unconditional in
                                       // upstream's v3 (no switch): no lattice
                                       // without a clock, the stall watchdog,
                                       // forgetting the fist clears the carrier
    CwFrontEndConfig   front;
    CwWeakViewConfig   weak;
    CwCallReaderConfig reader;
};

// The decoder's whole configuration: one part per consumer.
struct CwDecoderConfig {
    CwDiagConfig     diag;
    // v2 and v3
    CwEnvelopeConfig envelope;
    CwAssemblyConfig assembly;
    CwLatticeConfig  lattice;
    CwClockConfig    clock;
    CwLlrConfig      llr;
    CwRescoreConfig  rescore;
    // v3 only
    CwV3Config       v3;
};

// The two decoders as presets of the one implementation (decode_cw_viterbi):
// v3 is the defaults above; v2 is v3 with the front end, the call reader and
// the clock fixes off — exactly upstream v2's code paths.
CwDecoderConfig cw_preset_v2();
CwDecoderConfig cw_preset_v3();

// base with upstream's environment variables applied, parsed as upstream
// parses them: version 2 reads SKIM_V2_* / SKIM_CW2_*, version 3 reads
// SKIM_V3_* / SKIM_CW3_* and the v3-only switches. Unset variables leave
// base as it is.
CwDecoderConfig cw_config_from_env(CwDecoderConfig base, int version);
