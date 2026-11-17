/*
 * decode_cw.h — CW (Morse) decode backend. Phase 1.
 *
 * Envelope detection → adaptive threshold → dot/dash element timing → adaptive
 * WPM → Morse table, with tolerance for a ragged fist (HMM/Bayes planned).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "decode.h"
#include "decode_cw_config.h"
#include "callsign.h"


// SDRIAK: the classical v1 backend (skim_decode_cw, decode_cw.c) is not part
// of SDRIAK.
//
// v2 and v3 are one implementation (decode_cw_viterbi.cpp) in two presets
// (cw_preset_v2 / cw_preset_v3, decode_cw_config.h). A backend is made on a
// configuration — the preset, upstream's SKIM_* environment switches on top
// of it (cw_config_from_env), or a module's own tunables — and on the
// dictionary source its channels read (null = none); the name is "cw2" /
// "cw3". SDRIAK: upstream's process-wide singletons skim_decode_cw_v2()/v3()
// are not taken: they would read a dictionary from hidden memory.

// v2: soft-decision semi-Markov Viterbi over the same plumbing — built for
// QSB (a faded element is weak evidence, not no evidence). The pipeline
// DEFAULT since 2026-08-04; SKIM_CW_V1=1 falls back to the classical v1.
std::unique_ptr<SkimDecodeBackend> skim_decode_cw_v2_new(const CwDecoderConfig &cfg,
                                                         std::shared_ptr<const SkimCallsignDictSource> dict);

// v3: v2's lattice behind a carrier-matched front end, calibrated LLRs,
// a CFAR gate and an audio-confirmed callsign reader (docs/CW-V3.md).
// The default on the v3 branch; cw:v2 / SKIM_CW_ENGINE=v2 keep v2.
std::unique_ptr<SkimDecodeBackend> skim_decode_cw_v3_new(const CwDecoderConfig &cfg,
                                                         std::shared_ptr<const SkimCallsignDictSource> dict);
