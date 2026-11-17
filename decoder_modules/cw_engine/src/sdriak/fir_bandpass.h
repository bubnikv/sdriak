// fir_bandpass.h — WDSP's fir_bandpass() on SDRIAK's dsp library (see
// fir_bandpass.cpp).
//
// N taps of a windowed-sinc band-pass from f_low to f_high (Hz) at samplerate:
// rtype 0 = N real taps, rtype 1 = N complex taps interleaved (2N values).
// wintype 0 = 4-term Blackman-Harris, the only window supported. scale
// multiplies every tap. Empty on an unsupported request (upstream: NULL).
//
// Part of SDRIAK. GPL-3.0-or-later.
#ifndef SDRIAK_FIR_BANDPASS_H
#define SDRIAK_FIR_BANDPASS_H

#include <vector>

std::vector<double> fir_bandpass(int N, double f_low, double f_high, double samplerate,
                                 int wintype, int rtype, double scale);

#endif // SDRIAK_FIR_BANDPASS_H
