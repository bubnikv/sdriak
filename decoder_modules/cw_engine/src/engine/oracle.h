/*
 * oracle.h — measurement only: tell the decoder where R heard a station.
 *
 * Step 0.1 of docs/WEAK-SIGNAL-PLAN.md. SKIM_ORACLE=<file> (the output of
 * skimmer-compare/oracle-truth.py: "t0_s t1_s carrier_hz wpm call …" in
 * stream seconds) and SKIM_ORACLE_MODE=gate,clock,freq,ghost,gatesoft hand the CW v2
 * decoder and the ghost arbitration what CW Skimmer Server (R) heard:
 *
 *   gate   the squelch and the keying duty are held open on the channel
 *          that owns R's station while R heard it,
 *   clock  the dit comes from R's WPM instead of the bootstrap, and a clock
 *          more than 25 % off it is reset,
 *   freq   the tone offset is the carrier measured in the IQ, not tracked,
 *   ghost  a stronger neighbour does not suppress that channel's decodes,
 *   gatesoft  the squelch and keying-duty bars are halved there (2.0×,
 *          0.07) instead of held open — the gate still shuts on the gaps.
 *   combine  nothing changes; every word the decoder closes there is laid
 *          against R's call and its dictionary neighbours (forced against
 *          free reading, the re-read's own scores) and logged to stderr as
 *          "ocomb:" lines — how far soft-combining the repeats (step 6)
 *          could carry a station one reading does not.
 *   kws    nothing changes; every 3 s the channel's last 6 s of envelope
 *          are searched for R's call (and, as the control, its dictionary
 *          neighbours and the calls of R's other stations) by forced
 *          alignment against a free reading, no word boundaries — "okws:"
 *          lines: is the call in the audio at all (step 5).
 *
 * How much more of R's stations L reads with each is how much each stage
 * costs today. Unset, nothing changes: there is no oracle.
 * SKIM_ORACLE_MINPROM (dB, default 6) skips carriers the truth file could
 * not find clearly. Stream time is the replay's plus SKIM_REPLAY_FROM.
 *
 * SDRIAK: the oracle is an object, handed to the decoder in its
 * configuration (CwDiagConfig::oracle; null = off) — upstream: file-scope
 * state loaded at the first skim_oracle_modes() and read through the
 * skim_oracle_*() functions, which are its methods here.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "skim_call.h"


struct SkimOracleHit {
    double carrier_hz;   // absolute
    double wpm;          // R's
    SkimCall call;       // R's
};

class SkimOracle {
public:
    // The modes (SKIM_ORACLE_MODE; upstream: SKIM_ORACLE_GATE … SKIM_ORACLE_KWS).
    static constexpr unsigned kGate     = 1u << 0;   // gate
    static constexpr unsigned kClock    = 1u << 1;   // clock
    static constexpr unsigned kFreq     = 1u << 2;   // freq
    static constexpr unsigned kGhost    = 1u << 3;   // ghost
    static constexpr unsigned kSoftGate = 1u << 4;   // gatesoft
    static constexpr unsigned kCombine  = 1u << 5;   // combine
    static constexpr unsigned kKws      = 1u << 6;   // kws

    // The oracle upstream's variables describe (SKIM_ORACLE, SKIM_ORACLE_MODE,
    // SKIM_ORACLE_MINPROM, SKIM_REPLAY_FROM), parsed as upstream parses them:
    // null when SKIM_ORACLE or SKIM_ORACLE_MODE is unset; an unknown mode or
    // an unreadable file aborts, as upstream's does. Call it once and share
    // the result.
    static std::shared_ptr<const SkimOracle> from_env();

    // The modes switched on (kGate, …).
    unsigned modes() const { return m_modes; }

    // Stream seconds of the replay at stream time t_s of the pipeline.
    double time(double t_s) const { return m_from + t_s; }

    // The station R heard at time t_s whose carrier lies within half_hz of
    // chan_hz (the nearest one if several); nullopt when none.
    std::optional<SkimOracleHit> at(double chan_hz, double half_hz, double t_s) const;

    // Up to max calls of R's other stations (never `call`, no repeats), picked
    // by seed — the false-alarm control of the kws mode.
    unsigned decoys(const char *call, unsigned seed, SkimCall *out, unsigned max) const;

private:
    struct Interval {                    // one station R heard
        double t0, t1, hz, wpm;
        SkimCall call;
    };
    SkimOracle() = default;

    unsigned        m_modes = 0;
    std::vector<Interval> m_intervals;   // by t0 (SDRIAK: was a GArray-built block)
    double          m_maxlen = 0;        // the longest interval
    double          m_from = 0;          // SKIM_REPLAY_FROM
};
