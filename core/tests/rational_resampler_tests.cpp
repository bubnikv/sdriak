#include "test_support.h"

#include <dsp/multirate/rational_resampler.h>

#include <cmath>
#include <vector>

namespace {

using test_support::expect;

// Exposes which internal stages hold storage. The stages are protected
// members; their own public queries report whether they are populated.
template<class T>
class InspectableResampler : public dsp::multirate::RationalResampler<T> {
public:
    using dsp::multirate::RationalResampler<T>::RationalResampler;
    int decimStages() const { return this->decim.getStageCount(); }
    bool resampEmpty() const { return this->resamp.isEmpty(); }
};

std::vector<float> testSignal(int count) {
    std::vector<float> signal((size_t)count);
    for (int i = 0; i < count; i++) {
        signal[(size_t)i] = (float)(std::sin(0.013 * i) + 0.25 * std::sin(0.31 * i));
    }
    return signal;
}

// Runs the same blocks through both resamplers and requires identical output.
void expectSameOutput(dsp::multirate::RationalResampler<float>& a,
                      dsp::multirate::RationalResampler<float>& b,
                      int maxInput, const char* message) {
    const std::vector<float> input = testSignal(maxInput);
    std::vector<float> outA((size_t)a.maxOutputBuffer());
    std::vector<float> outB((size_t)b.maxOutputBuffer());
    const int sizes[] = { maxInput, (maxInput / 3) + 1, maxInput - 1 };
    for (int size : sizes) {
        const int countA = a.process(size, input.data(), outA.data());
        const int countB = b.process(size, input.data(), outB.data());
        expect(countA == countB, message);
        for (int i = 0; i < countA && i < countB; i++) {
            if (outA[(size_t)i] != outB[(size_t)i]) {
                expect(false, message);
                return;
            }
        }
    }
}

void testInterpolatingInitBuildsNoDecimator() {
    InspectableResampler<float> resampler(24000.0, 48000.0, 4096);
    expect(resampler.decimStages() == 0,
           "interpolating resampler built decimator stages it never runs");
    expect(!resampler.resampEmpty(),
           "interpolating resampler has no polyphase stage");
}

void testDecimateOnlyReleasesPolyphase() {
    InspectableResampler<float> resampler(64000.0, 16000.0, 4096);
    expect(resampler.decimStages() == 2,
           "exact 4:1 resampler did not build the plan_4 decimator");
    expect(resampler.resampEmpty(),
           "decimate-only resampler kept a polyphase delay line");

    // reset() on the released stage must be safe, and processing must
    // still respect the bound.
    resampler.reset();
    const std::vector<float> input = testSignal(4096);
    std::vector<float> output((size_t)resampler.maxOutputBuffer());
    expect(resampler.process(4096, input.data(), output.data()) <= resampler.maxOutputBlock(),
           "decimate-only resampler exceeded its output bound");
}

void testPassthroughHoldsNothing() {
    InspectableResampler<float> resampler(48000.0, 48000.0, 4096);
    expect(resampler.decimStages() == 0 && resampler.resampEmpty(),
           "passthrough resampler holds stage storage");

    resampler.setRates(0.0, 48000.0);
    expect(resampler.decimStages() == 0 && resampler.resampEmpty(),
           "invalid-rate passthrough holds stage storage");
}

void testSwitchingReleasesAbandonedStages() {
    constexpr int maxInput = 20000;

    // The radio's AF resampler: built for WFM, then switched to NFM.
    InspectableResampler<float> resampler(250000.0, 48000.0, maxInput);
    expect(resampler.decimStages() == 2 && !resampler.resampEmpty(),
           "250 kHz -> 48 kHz did not use both stages");

    resampler.setInSamplerate(50000.0);
    expect(resampler.decimStages() == 0,
           "switch to 50 kHz kept the 250 kHz decimator");
    expect(!resampler.resampEmpty(),
           "switch to 50 kHz lost its polyphase stage");
    {
        dsp::multirate::RationalResampler<float> fresh(50000.0, 48000.0, maxInput);
        expectSameOutput(resampler, fresh, maxInput,
                         "switched resampler differs from a fresh 50 kHz one");
    }

    // And back: both stages are rebuilt from nothing.
    resampler.setInSamplerate(250000.0);
    expect(resampler.decimStages() == 2 && !resampler.resampEmpty(),
           "switch back to 250 kHz did not rebuild both stages");
    {
        dsp::multirate::RationalResampler<float> fresh(250000.0, 48000.0, maxInput);
        expectSameOutput(resampler, fresh, maxInput,
                         "rebuilt resampler differs from a fresh 250 kHz one");
    }

    // Decimate-only from a configuration that used the polyphase stage.
    resampler.setRates(64000.0, 16000.0);
    expect(resampler.resampEmpty(),
           "switch to an exact 4:1 ratio kept the polyphase delay line");
}

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
        testInterpolatingInitBuildsNoDecimator();
        testDecimateOnlyReleasesPolyphase();
        testPassthroughHoldsNothing();
        testSwitchingReleasesAbandonedStages();
    });
}
