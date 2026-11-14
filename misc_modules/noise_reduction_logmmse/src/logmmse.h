#pragma once

#include "arrays.h"
#include "math.h"
#include "bgnoise.h"
#include <utils/flog.h>
#include <cstring>
#include <atomic>
#include <deque>
#include <utility>

namespace dsp {

    // ported from https://github.com/rajivpoddar/logmmse/  by sannysanoff

    namespace logmmse {

        using namespace ::dsp::arrays;
        using namespace ::dsp::math;

        struct LogMMSE {

            struct SavedParamsC {
                int noise_history_len() {
                    if (nFFT < 1000) {
                        return 2000;
                    } else {
                        return 200;
                    }
                }

                std::deque<FloatArray> noise_history;      // chunks of nFFT*float
                std::deque<FloatArray> dev_history;
                FloatArray sums;      // sliding sum of last N noise_history
                FloatArray devs;      // sliding sum of dev_history

                FloatArray noise_mu2;
                FloatArray Xk_prev;   // empty until logmmse_sample() has run
                ComplexArray x_old;

                int Slen;
                int PERC;
                int len1;
                int len2;
                FloatArray win;
                int nFFT;
                std::unique_ptr<ZeroPaddedFFTW> forwardPlan;
                std::unique_ptr<ZeroPaddedFFTW> reversePlan;
                float aa = 0.98;
                float ksi_min;
                // Written by the TX-state handler on another thread, read on
                // the DSP thread.
                std::atomic<bool> hold{ false };

                void reset() {
                    noise_history.clear();
                    dev_history.clear();
                    Xk_prev.clear();
                    noise_mu2.clear();
                    x_old.clear();
                    backgroundNoiseCalculator.reset();
                }

                // Takes the frame by value: callers pass std::move once they
                // are done with the buffer, so the deque adopts it without a
                // deep copy.
                void add_noise_history(FloatArray noise) {
                    if (hold) {
                        return;
                    }
                    if (noise.size() != nFFT) {
                        flog::info("ERROR noise.size() != nFFT: {} {}", (int)noise.size(), nFFT);
                        return;
                    }
                    noise_history.emplace_back(std::move(noise));
                    // deque guarantees back() stays valid through the
                    // pop_front() trimming below (history length >= 200).
                    const FloatArray& latest = noise_history.back();
                    volk_32f_x2_add_32f(sums.data(), sums.data(), latest.data(), nFFT);
                    while (noise_history.size() > noise_history_len()) {
                        volk_32f_x2_subtract_32f(sums.data(), sums.data(), noise_history.front().data(), nFFT);
                        noise_history.pop_front();
                    }

                    auto noiseAvg = div(sums, (float)noise_history.size());

                    auto diff = subeach(latest, noiseAvg);
                    diff = muleach(diff, diff);

                    volk_32f_x2_add_32f(devs.data(), devs.data(), diff.data(), nFFT);
                    dev_history.emplace_back(std::move(diff));

                    while (dev_history.size() > noise_history_len()) {
                        volk_32f_x2_subtract_32f(devs.data(), devs.data(), dev_history.front().data(), nFFT);
                        dev_history.pop_front();
                    }

                }

                BackgroundNoiseCalculator backgroundNoiseCalculator;

                void update_noise_mu2() {
                    auto nframes = noise_history.size();
                    if (nframes > 100 && !hold) {
                        auto noise_mu2_copy = noise_mu2;

                        auto noiseAvg = mul(sums, 1 / (float)nframes);

                        auto hi = mul(devs, 1 / (float)nframes);
                        auto devSquare = muleach(hi, hi);
                        auto devSquareD = devSquare.data();
                        for (int z = 0; z < nFFT; z++) {
                            if (abs(z - nFFT/2) < nFFT * 15 / 100) {
                                // after fft, rightmost and leftmost sides of real frequencies range are at the center of the resulting table.
                                // We exclude middle of the table from lookup
                                devSquareD[z] = BackgroundNoiseCalculator::ERASED_SAMPLE;
                            }
                        }
                        memset(noise_mu2.data(), 0, nFFT*sizeof(float));
                        float detectedNoise = backgroundNoiseCalculator.addFrame(devSquare);
                        auto acceptible_stdev = detectedNoise;
                        auto nmu2 = noise_mu2.data();
                        auto navg =  noiseAvg.data();
                        for(int q=0; q < nFFT; q++) {
                            if (devSquareD[q] < acceptible_stdev) {
                                nmu2[q] = navg[q] * navg[q];
                            }
                        }

                        if (!linearInterpolateHoles(nmu2, nFFT)) {
                            noise_mu2 = std::move(noise_mu2_copy);
                        }
                    }
                }
            };

            static void logmmse_sample(const ComplexArray &x, int Srate, SavedParamsC *params, int noise_frames) {
                params->Slen = floor(0.02 * Srate);
                if (params->Slen % 2 == 1) params->Slen++;
                params->PERC = 50;
                params->len1 = floor(params->Slen * params->PERC / 100);
                params->noise_history.clear();
                params->dev_history.clear();
                params->len2 = params->Slen - params->len1;         // len1+len2
                params->win = nphanning(params->Slen);
                params->win = div(mul(params->win, params->len2), npsum(params->win));
                params->nFFT = 2 * params->Slen;
                params->forwardPlan = std::make_unique<ZeroPaddedFFTW>(false, params->nFFT);
                params->reversePlan = std::make_unique<ZeroPaddedFFTW>(true, params->nFFT);
                params->sums = npzeros(params->nFFT);
                params->devs = npzeros(params->nFFT);

                flog::debug("logmmse: sampling noise, srate={}, Slen={}, nFFT={}", Srate, params->Slen, params->nFFT);
                auto noise_mean = npzeros(params->nFFT);
                for (int j = 0; j < params->Slen * noise_frames; j += params->Slen) {
                    params->forwardPlan->npfftfft(muleach(params->win, nparange(x, j, j + params->Slen)));
                    auto noise = npabsolute(params->forwardPlan->getOutput());
                    noise_mean = addeach(noise_mean, noise);
                    params->add_noise_history(std::move(noise));
                }
                params->noise_mu2 = div(noise_mean, noise_frames);
                params->noise_mu2 = npmavg(params->noise_mu2, 120);
                params->noise_mu2 = muleach(params->noise_mu2, params->noise_mu2);
                params->Xk_prev = npzeros(params->len1);
                params->x_old = npzeros_c(params->len1);
                params->ksi_min = ::pow(10, -25.0 / 10.0);
            }

            // maxInput caps how much of x is consumed in one pass (-1 = all of it),
            // so a large backlog can be drained in bounded chunks without copying.
            static ComplexArray logmmse_all(const ComplexArray &x, SavedParamsC *params, int maxInput = -1) {
                int inputLen = (int)x.size();
                if (maxInput >= 0 && maxInput < inputLen) {
                    inputLen = maxInput;
                }
                auto Nframes = floor(inputLen / params->len2) - floor(params->Slen / params->len2);
                params->update_noise_mu2();
                auto xfinal = npzeros_c(Nframes * params->len2);
                for (int k = 0; k < Nframes * params->len2; k += params->len2) {
                    auto insign = muleach(params->win, nparange(x, k, k + params->Slen));
                    params->forwardPlan->npfftfft(insign);
                    const auto& spec = params->forwardPlan->getOutput();
                    auto sig = npabsolute(spec);
                    auto sigD = sig.data();
                    for (auto z = 1; z < sig.size(); z++) {
                        if (sigD[z] == 0) {
                            sigD[z] = sigD[z - 1];      // for some reason fft returns 0 instead if small value
                        }
                    }
                    auto sig2 = muleach(sig, sig);

                    auto gammak = npminimum(diveach(sig2, params->noise_mu2), 40);
                    FloatArray ksi;
                    if (!npall(params->Xk_prev)) {
                        ksi = add(mul(npmaximum(add(gammak, -1), 0), 1 - params->aa), params->aa);
                    } else {
                        const FloatArray d1 = diveach(mul(params->Xk_prev, params->aa), params->noise_mu2);
                        const FloatArray m1 = mul(npmaximum(add(gammak, -1), 0), (1 - params->aa));
                        ksi = addeach(d1, m1);
                        ksi = npmaximum(std::move(ksi), params->ksi_min);
                    }
                    auto A = diveach(ksi, add(ksi, 1));
                    auto vk = muleach(A, gammak);
                    auto ei_vk = mul(scipyspecialexpn(vk), 0.5);
                    auto hw = muleach(A, npexp(ei_vk));
                    auto sigHw = muleach(sig, hw);
                    params->Xk_prev = muleach(sigHw, sigHw);
                    // Deferred from right after the hole-filling loop: this is
                    // past sig's last read, so the history can adopt the
                    // buffer instead of copying it. The intervening math does
                    // not touch sums/devs/noise_history, so the ordering is
                    // equivalent.
                    params->add_noise_history(std::move(sig));
                    auto hwmulspec = muleach(hw, spec);
                    params->reversePlan->npfftfft(hwmulspec);
                    const auto& xi_w0 = params->reversePlan->getOutput();
                    auto final = addeach(params->x_old, nparange(xi_w0, 0, params->len1));
                    nparangeset(xfinal, k, final);
                    params->x_old = nparange(xi_w0, params->len1, params->Slen);
                }
                return xfinal;
            }

        };

    }
}
