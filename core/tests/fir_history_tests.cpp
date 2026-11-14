#include "test_support.h"

#include <dsp/channel/rx_vfo.h>
#include <dsp/filter/fir.h>
#include <dsp/taps/from_array.h>

#include <cmath>

namespace {

using test_support::expect;

void expectNear(float actual, float expected, const char* message) {
    if (std::fabs(actual - expected) > 1e-5f) {
        throw std::runtime_error(message);
    }
}

class InspectableRxVFO : public dsp::channel::RxVFOKernel {
public:
    void processFilter(int count, const dsp::complex_t* in, dsp::complex_t* out) {
        filter.process(count, in, out);
    }

    int filterCapacity() const { return filter.maxBlock(); }
};

void testCapacityChangePreservesFIRHistory() {
    const float coefficients[] = { 1.0f, 1.0f };
    dsp::filter::FIR<float, float> fir(
        dsp::taps::fromArray<float>(2, coefficients), 2);

    const float first[] = { 1.0f, 2.0f };
    float firstOut[2]{};
    fir.process(2, first, firstOut);
    expectNear(firstOut[0], 1.0f, "FIR initial zero history changed");
    expectNear(firstOut[1], 3.0f, "FIR did not retain samples within a block");

    fir.setMaxBlock(8);
    expect(fir.maxBlock() == 8, "FIR did not grow its block capacity");
    const float afterGrow[] = { 3.0f };
    float afterGrowOut[1]{};
    fir.process(1, afterGrow, afterGrowOut);
    expectNear(afterGrowOut[0], 5.0f,
               "growing FIR block capacity discarded its history");

    fir.setMaxBlock(1);
    expect(fir.maxBlock() == 1, "FIR did not shrink its block capacity");
    const float afterShrink[] = { 4.0f };
    float afterShrinkOut[1]{};
    fir.process(1, afterShrink, afterShrinkOut);
    expectNear(afterShrinkOut[0], 7.0f,
               "shrinking FIR block capacity discarded its history");

    fir.reset();
    const float afterReset[] = { 5.0f };
    float afterResetOut[1]{};
    fir.process(1, afterReset, afterResetOut);
    expectNear(afterResetOut[0], 5.0f,
               "explicit FIR reset did not clear its history");
}

void seedFilter(InspectableRxVFO& vfo) {
    dsp::complex_t input[8];
    dsp::complex_t output[8];
    for (auto& sample : input) { sample = { 1.0f, 0.25f }; }
    vfo.processFilter(8, input, output);
}

void testInputRateChangePreservesPostResamplerHistory() {
    InspectableRxVFO changed;
    InspectableRxVFO reference;
    changed.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    reference.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    seedFilter(changed);
    seedFilter(reference);

    expect(changed.filterCapacity() == 16,
           "unexpected initial post-resampler FIR capacity");
    changed.setInSamplerate(24000.0);
    expect(changed.filterCapacity() == 32,
           "input-rate change did not update FIR capacity");

    const dsp::complex_t input[] = { { 0.0f, 0.0f } };
    dsp::complex_t changedOut[1]{};
    dsp::complex_t referenceOut[1]{};
    changed.processFilter(1, input, changedOut);
    reference.processFilter(1, input, referenceOut);
    expectNear(changedOut[0].re, referenceOut[0].re,
               "input-rate change discarded post-resampler FIR history");
    expectNear(changedOut[0].im, referenceOut[0].im,
               "input-rate change changed post-resampler FIR history");
}

void testOutputRateChangeClearsPostResamplerHistory() {
    InspectableRxVFO changed;
    InspectableRxVFO fresh;
    changed.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    fresh.init(48000.0, 8000.0, 4000.0, 0.0, 64);
    seedFilter(changed);

    changed.setOutSamplerate(8000.0, 4000.0);

    const dsp::complex_t input[] = { { 0.0f, 0.0f } };
    dsp::complex_t changedOut[1]{};
    dsp::complex_t freshOut[1]{};
    changed.processFilter(1, input, changedOut);
    fresh.processFilter(1, input, freshOut);
    expectNear(changedOut[0].re, freshOut[0].re,
               "output-rate change retained old-rate FIR history");
    expectNear(changedOut[0].im, freshOut[0].im,
               "output-rate change retained complex FIR history");
}

void testBandwidthOnlyChangePreservesHistory() {
    InspectableRxVFO changed;
    InspectableRxVFO fresh;
    changed.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    fresh.init(48000.0, 12000.0, 4000.0, 0.0, 64);
    seedFilter(changed);

    changed.setBandwidth(4000.0);

    dsp::complex_t input[16]{};
    dsp::complex_t changedOut[16]{};
    dsp::complex_t freshOut[16]{};
    changed.processFilter(16, input, changedOut);
    fresh.processFilter(16, input, freshOut);
    float difference = 0.0f;
    for (int i = 0; i < 16; i++) {
        difference += std::fabs(changedOut[i].re - freshOut[i].re) +
                      std::fabs(changedOut[i].im - freshOut[i].im);
    }
    expect(difference > 1e-5f,
           "bandwidth-only change unexpectedly cleared FIR history");
}

void testUnchangedOutputRateIsANoOp() {
    InspectableRxVFO unchanged;
    InspectableRxVFO reference;
    unchanged.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    reference.init(48000.0, 12000.0, 6000.0, 0.0, 64);
    seedFilter(unchanged);
    seedFilter(reference);

    unchanged.setOutSamplerate(12000.0, 6000.0);

    const dsp::complex_t input[] = { { 0.0f, 0.0f } };
    dsp::complex_t unchangedOut[1]{};
    dsp::complex_t referenceOut[1]{};
    unchanged.processFilter(1, input, unchangedOut);
    reference.processFilter(1, input, referenceOut);
    expectNear(unchangedOut[0].re, referenceOut[0].re,
               "unchanged output rate reset FIR history");
    expectNear(unchangedOut[0].im, referenceOut[0].im,
               "unchanged output rate changed FIR history");
}

} // namespace

int main() {
    return test_support::run("fir_history_tests", [] {
        testCapacityChangePreservesFIRHistory();
        testInputRateChangePreservesPostResamplerHistory();
        testOutputRateChangeClearsPostResamplerHistory();
        testBandwidthOnlyChangePreservesHistory();
        testUnchangedOutputRateIsANoOp();
    });
}
