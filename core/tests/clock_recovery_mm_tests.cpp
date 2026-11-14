#include "test_support.h"

#include <dsp/clock_recovery/mm.h>

#include <stdexcept>
#include <vector>

namespace {

using test_support::expect;

class InspectableMM : public dsp::clock_recovery::MM<float> {
public:
    using dsp::clock_recovery::MM<float>::MM;

    void forceLoopState(float frequency, float phase = 0.0f) {
        pcl.freq = frequency;
        pcl.phase = phase;
        offset = 0;
    }
};

void testPocsagWorstCaseBound() {
    constexpr int maxInput = 1000000;
    dsp::clock_recovery::MM<float> recovery(
        24000.0 / 2400.0, 1e-4, 1.0, 0.05, 128, 8, maxInput);

    // min phase advance = 10 * (1 - 0.05) - 1 = 8.5 samples.
    // ceil(1,000,000 / 8.5) = 117,648. The old nominal-rate bound was
    // 100,064 and allowed process() to overrun both POCSAG output streams.
    expect(recovery.maxOutputBlock() == 117648,
           "POCSAG MM output bound does not cover its minimum phase advance");
}

void testBoundTracksLoopParameters() {
    constexpr int maxInput = 1000000;
    dsp::clock_recovery::MM<float> recovery(10.0, 1e-4, 1.0, 0.05, 128, 8, maxInput);

    recovery.setMuGain(0.0);
    expect(recovery.maxOutputBlock() == 105264,
           "MM output bound did not follow a mu-gain change");

    recovery.setMuGain(1.0);
    expect(recovery.maxOutputBlock() == 117648,
           "MM output bound did not restore after a mu-gain change");
}

void testLowerFrequencyAlreadyExceedsOldCapacity() {
    constexpr int maxInput = 1000000;
    constexpr int oldNominalCapacity = 100064;
    constexpr float canary = 123456.0f;
    InspectableMM recovery(10.0, 1e-4, 1.0, 0.05, 128, 8, maxInput);

    // Zero input produces zero timing error. Put the loop at its legal 9.5
    // lower frequency limit so process() advances by 9.5 samples per output.
    // This alone exceeds the old nominal 10:1 + 64 allocation; the additional
    // muGain*error term is why maxOutputBlock() reserves still more room.
    recovery.forceLoopState(9.5f);
    std::vector<float> input(maxInput, 0.0f);
    std::vector<float> output((size_t)recovery.maxOutputBlock() + 2, canary);
    const int count = recovery.process(maxInput, input.data(), output.data());

    expect(count == 105264,
           "MM did not emit the expected count at its lower frequency limit");
    expect(count > oldNominalCapacity,
           "MM lower frequency no longer demonstrates the old POCSAG overflow");
    expect(output[recovery.maxOutputBlock()] == canary &&
               output[recovery.maxOutputBlock() + 1] == canary,
           "MM wrote beyond its reported output bound");
}

void testUnboundedConfigurationIsRejected() {
    bool threw = false;
    try {
        // 1 * (1 - 0.05) - 1 <= 0: process() could emit forever without
        // advancing its input offset, so no finite destination can be safe.
        dsp::clock_recovery::MM<float> recovery(1.0, 1e-4, 1.0, 0.05, 128, 8, 1024);
    }
    catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "MM accepted a configuration with no finite output bound");
}

} // namespace

int main() {
    return test_support::run("clock_recovery_mm_tests", [] {
        testPocsagWorstCaseBound();
        testBoundTracksLoopParameters();
        testLowerFrequencyAlreadyExceedsOldCapacity();
        testUnboundedConfigurationIsRejected();
    });
}
