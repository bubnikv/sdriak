#include "test_support.h"

#include <dsp/multirate/rational_resampler.h>

#include <algorithm>
#include <vector>

namespace {

using test_support::expect;

void exercise(double inputRate, double outputRate, int maxInput, const char* message) {
    dsp::multirate::RationalResampler<float> resampler(inputRate, outputRate, maxInput);
    const int bound = resampler.maxOutputBlock();
    expect(bound > 0, "resampler returned a non-positive output bound");

    std::vector<float> input(maxInput, 0.0f);
    // Keep diagnostic headroom so a bad bound fails the assertion instead of
    // making the test itself write out of bounds before it can report it.
    std::vector<float> output((std::max)(bound, maxInput * 4), 0.0f);

    // Vary block sizes so the retained polyphase offset and phase cross block
    // boundaries instead of testing only the reset state.
    const int sizes[] = { maxInput, (maxInput / 3) + 1, maxInput - 1, (maxInput / 2) + 7 };
    for (int size : sizes) {
        const int count = resampler.process(size, input.data(), output.data());
        expect(count <= bound, message);
    }
}

void testNearUnityApproximationUsesActualRatio() {
    constexpr int maxInput = 100000;
    dsp::multirate::RationalResampler<float> resampler(1026.0, 1025.0, maxInput);

    // The numerator cap selects 1:1 for this requested ratio.
    expect(resampler.maxOutputBlock() >= maxInput,
           "near-unity passthrough was bounded by the smaller requested ratio");

    std::vector<float> input(maxInput, 0.0f);
    std::vector<float> output(resampler.maxOutputBlock(), 0.0f);
    expect(resampler.process(maxInput, input.data(), output.data()) == maxInput,
           "near-unity approximation did not select the expected passthrough");
}

void testConfiguredBounds() {
    exercise(2500000.0, 250000.0, 100003,
             "decimating resampler exceeded its configured output bound");
    exercise(48000.0, 96000.0, 1001,
             "interpolating resampler exceeded its configured output bound");
    exercise(44100.0, 48000.0, 4097,
             "fractional resampler exceeded its configured output bound");
}

} // namespace

int main() {
    return test_support::run("rational_resampler_tests", [] {
        testNearUnityApproximationUsesActualRatio();
        testConfiguredBounds();
    });
}
