#include <dsp/buffer/fftw.h>

namespace {

// Constructed before main(), while fftwMutex() is first touched by init() from
// main. Its plan is consequently destroyed after an ordinary function-local
// mutex would have been destroyed. The test's process exit is the assertion:
// libc++/bionic abort when FFTWPlan::~FFTWPlan() locks that dead mutex.
struct StaticPlanOwner {
    void init() {
        input.allocate(16);
        output.allocate(16);
        plan.planDFT1D(16, input, output, FFTW_FORWARD);
    }

    // Reverse member destruction releases the plan before its bound arrays.
    dsp::buffer::FFTWBuffer<fftwf_complex> input;
    dsp::buffer::FFTWBuffer<fftwf_complex> output;
    dsp::buffer::FFTWPlan plan;
};

StaticPlanOwner owner;

} // namespace

int main() {
    owner.init();
    return 0;
}
