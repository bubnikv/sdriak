// fir_bandpass.cpp — WDSP's fir_bandpass() for the upstream channelizer,
// built on SDRIAK's own DSP library instead of a vendored WDSP.
//
// Upstream's channelizer calls WDSP's fir_bandpass(); this is it: WDSP's
// windowed-sinc design, the same concept as dsp::taps::windowedSinc, with
// two details kept WDSP's so the prototype taps match upstream's:
//   - the window spans N - 1, not N: dsp::window::blackmanHarris(i, N - 1)
//     evaluates cos(2πk·i/(N-1)), which is exactly WDSP's 4-term
//     Blackman-Harris (its Horner polynomial in cos(π·i/m) expands to the
//     same 0.35875 / 0.48829 / 0.14128 / 0.01168 coefficients);
//   - the odd-N centre tap is 2·ft·scale, unwindowed.
// The taps come back as a std::vector (WDSP: a malloc'd block the caller
// free()s, from an LRU impulse cache that is not reproduced here).
//
// Supported: wintype 0 (BH4) — the only window the engine asks for — and
// rtype 0 (real) / 1 (complex, interleaved). Anything else returns an empty
// vector, which the channelizer treats as a failed construction.
//
// Part of SDRIAK. GPL-3.0-or-later.
#include "fir_bandpass.h"

#include <cmath>
#include <dsp/math/constants.h>
#include <dsp/window/blackman_harris.h>

std::vector<double> fir_bandpass(int N, double f_low, double f_high, double samplerate,
                                 int wintype, int rtype, double scale) {
    if (N < 1 || samplerate <= 0.0 || wintype != 0 || (rtype != 0 && rtype != 1)) {
        return {};
    }

    // N real or N complex (interleaved) taps.
    std::vector<double> taps(static_cast<size_t>(N) * (rtype ? 2 : 1), 0.0);
    double* c = taps.data();

    const double ft = (f_high - f_low) / (2.0 * samplerate);    // one-sided cutoff, cycles/sample
    const double ftRad = 2.0 * DB_M_PI * ft;
    const double wOsc = DB_M_PI * (f_high + f_low) / samplerate; // band centre, rad/sample
    const double m = 0.5 * static_cast<double>(N - 1);

    for (int i = 0; i < N; i++) {
        const double t = static_cast<double>(i) - m;
        double coef;
        if (t == 0.0) {
            coef = scale * 2.0 * ft;                              // odd N: the centre tap
        }
        else {
            const double sinc = std::sin(ftRad * t) / (DB_M_PI * t);
            coef = scale * sinc * dsp::window::blackmanHarris(static_cast<double>(i), static_cast<double>(N - 1));
        }
        if (rtype == 0) {
            c[i] = coef * std::cos(t * wOsc);
        }
        else {
            c[2 * i + 0] = coef * std::cos(t * wOsc);
            c[2 * i + 1] = -coef * std::sin(t * wOsc);
        }
    }
    return taps;
}
