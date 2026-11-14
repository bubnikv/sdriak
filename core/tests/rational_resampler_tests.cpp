#include "test_support.h"

#include <dsp/multirate/rational_resampler.h>

#include <vector>

namespace {

using test_support::expect;

void exercise(double inputRate, double outputRate, int maxInput, const char* message) {
    dsp::multirate::RationalResampler<float> resampler(inputRate, outputRate, maxInput);
    const int bound = resampler.maxOutputBlock();
    const int workspace = resampler.maxOutputBuffer();
    expect(bound > 0, "resampler returned a non-positive output bound");
    expect(workspace >= bound, "resampler workspace is smaller than its final output bound");

    std::vector<float> input(maxInput, 0.0f);
    constexpr float canary = 123456.0f;
    std::vector<float> output((size_t)workspace + 1, canary);

    // Vary block sizes so the retained polyphase offset and phase cross block
    // boundaries instead of testing only the reset state.
    const int sizes[] = { maxInput, (maxInput / 3) + 1, maxInput - 1, (maxInput / 2) + 7 };
    for (int size : sizes) {
        const int count = resampler.process(size, input.data(), output.data());
        expect(count <= bound, message);
        expect(output[workspace] == canary,
               "resampler wrote past its configured workspace bound");
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

void testRateSizedCallersUseSelectedRatio() {
    constexpr int maxInput = 1000000;
    constexpr float canary = 123456.0f;
    std::vector<float> input(maxInput, 0.0f);

    // Near-unity approximation selects passthrough. The old RDS requested-rate
    // estimate allocated 999,865 samples for this configuration, 135 short.
    const int rdsPreview = dsp::multirate::RationalResampler<float>::maxOutputBlockForRates(
        5001.0, 5000.0, maxInput);
    const int rdsWorkspace = dsp::multirate::RationalResampler<float>::maxOutputBufferForRates(
        5001.0, 5000.0, maxInput);
    expect(rdsPreview == maxInput,
           "RDS preview did not include near-unity passthrough");
    expect(rdsWorkspace == maxInput,
           "RDS workspace preview did not include near-unity passthrough");
    {
        dsp::multirate::RationalResampler<float> rds(5001.0, 5000.0, maxInput);
        expect(rds.maxOutputBlock() == rdsPreview,
               "RDS preview disagrees with the configured resampler");
        expect(rds.maxOutputBuffer() == rdsWorkspace,
               "RDS workspace preview disagrees with the configured resampler");
        std::vector<float> output((size_t)rdsWorkspace + 1, canary);
        expect(rds.process(maxInput, input.data(), output.data()) == rdsPreview,
               "RDS selected ratio emitted an unexpected block size");
        expect(output[rdsWorkspace] == canary,
               "RDS selected ratio wrote past its previewed workspace");
    }

    // The 4:1 power decimator performs all the work after approximation picks
    // 1:1. The old CTCSS formula allocated 249,940 rather than 250,000 samples.
    const int ctcssPreview = dsp::multirate::RationalResampler<float>::maxOutputBlockForRates(
        2001.0, 500.0, maxInput);
    const int ctcssWorkspace = dsp::multirate::RationalResampler<float>::maxOutputBufferForRates(
        2001.0, 500.0, maxInput);
    expect(ctcssPreview == 250000,
           "CTCSS preview did not include the selected decimator ratio");
    expect(ctcssWorkspace == 500000,
           "CTCSS workspace did not include the first decimator stage");
    {
        dsp::multirate::RationalResampler<float> ctcss(2001.0, 500.0, maxInput);
        expect(ctcss.maxOutputBlock() == ctcssPreview,
               "CTCSS preview disagrees with the configured resampler");
        expect(ctcss.maxOutputBuffer() == ctcssWorkspace,
               "CTCSS workspace preview disagrees with the configured resampler");
        std::vector<float> output((size_t)ctcssWorkspace + 1, canary);
        expect(ctcss.process(maxInput, input.data(), output.data()) == ctcssPreview,
               "CTCSS selected ratio emitted an unexpected block size");
        expect(output[ctcssWorkspace] == canary,
               "CTCSS selected ratio wrote past its previewed workspace");
    }

    // Normal production configurations exercise a multi-stage power decimator
    // followed by a fractional polyphase stage. Their final count is much
    // smaller than the first intermediate write.
    expect(dsp::multirate::RationalResampler<float>::maxOutputBlockForRates(
               250000.0, 5000.0, maxInput) == 20001,
           "normal RDS final bound changed unexpectedly");
    expect(dsp::multirate::RationalResampler<float>::maxOutputBufferForRates(
               250000.0, 5000.0, maxInput) == 125000,
           "normal RDS workspace omitted its first decimator stage");
    expect(dsp::multirate::RationalResampler<float>::maxOutputBlockForRates(
               50000.0, 500.0, maxInput) == 10001,
           "normal CTCSS final bound changed unexpectedly");
    expect(dsp::multirate::RationalResampler<float>::maxOutputBufferForRates(
               50000.0, 500.0, maxInput) == 125000,
           "normal CTCSS resampler workspace omitted its first decimator stage");
}

} // namespace

int main() {
    return test_support::run("rational_resampler_tests", [] {
        testNearUnityApproximationUsesActualRatio();
        testConfiguredBounds();
        testRateSizedCallersUseSelectedRatio();
    });
}
