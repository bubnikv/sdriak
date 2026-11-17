/*
 * cw_debug.h — the CW decoder's debug trace (SKIM_CWn_DEBUG) for the decoder
 * and its stages.
 *
 * SKIM_CWn_DEBUG=1 traces every channel; =<Hz> (an absolute frequency, as
 * the pipeline labels channels with set_freq) only the channel within 70 Hz
 * of it — one station out of a whole band replay.
 *
 * SDRIAK: a CwTrace each channel owns and hands to the stages that print
 * (upstream: cw_dbg() on file-scope state).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "decode_cw_config.h"

// One channel's debug trace: the diagnostics switches and the label of the
// channel it speaks for (the channel owns it; set_freq() moves the label).
struct CwTrace {
    const CwDiagConfig *diag = nullptr;
    const double       *freq_hz = nullptr;

    bool on() const {
        if (!diag || !diag->debug)
            return false;
        const double hz = diag->debug_hz;
        return hz < 1000.0 || (freq_hz && fabs(*freq_hz - hz) <= 70.0);
    }
};

#define CW_DBG(trace, ...) \
  do { if ((trace).on()) { fprintf(stderr, __VA_ARGS__); } } while (0)
