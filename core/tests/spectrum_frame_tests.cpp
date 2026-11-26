#include "test_support.h"

#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/server_zoom.h>
#include <signal_path/spectrum_frame.h>

#include <condition_variable>
#include <thread>
#include <vector>

namespace {

using test_support::expect;
using Clock = std::chrono::steady_clock;

SpectrumFrame ramp(int n, double start, double span) {
    SpectrumFrame f;
    for (int i = 0; i < n; i++) { f.bins.push_back((float)i); }
    f.startHz = start;
    f.spanHz = span;
    return f;
}

void testQueueOrderAndDue() {
    SpectrumQueue q;
    const auto now = Clock::now();
    SpectrumFrame a = ramp(4, 0.0, 4.0);
    SpectrumFrame b = ramp(4, 1.0, 4.0);
    b.presentAt = now + std::chrono::milliseconds(100);
    SpectrumFrame c = ramp(4, 2.0, 4.0);
    expect(q.push(std::move(a)) && q.push(std::move(b)) && q.push(std::move(c)), "push failed");

    SpectrumFrame out;
    expect(q.pop(out, now) && out.startHz == 0.0, "epoch frame not shown at once");
    expect(!q.pop(out, now), "frame popped before it was due");
    // The frame behind a not-yet-due frame waits too.
    expect(q.pop(out, now + std::chrono::milliseconds(100)) && out.startHz == 1.0, "due frame not popped");
    expect(q.pop(out, now) && out.startHz == 2.0, "last frame lost");
    expect(!q.pop(out, now), "empty queue popped");
    expect(q.dropped() == 0, "nothing should have been dropped");
}

void testQueueDropsOldest() {
    SpectrumQueue q;
    for (int i = 0; i < (int)SpectrumQueue::kSlots + 3; i++) {
        expect(q.push(ramp(2, (double)i, 2.0)), "push into a full ring must succeed");
    }
    expect(q.dropped() == 3, "full ring did not count its drops");
    SpectrumFrame out;
    expect(q.pop(out) && out.startHz == 3.0, "full ring did not drop the oldest");
    q.clear();
    expect(!q.pop(out), "clear left frames");
}

void testDelayedRowsAndPeakReduction() {
    SpectrumQueue q;
    const auto now = Clock::now();
    for (int i = 0; i <= 20; ++i) {
        auto frame = ramp(2, (double)i, 2.0);
        frame.presentAt = now + std::chrono::seconds(1) + std::chrono::milliseconds(50 * i);
        expect(q.push(std::move(frame)), "delayed row rejected");
    }
    SpectrumFrame frame;
    expect(q.pop(frame, now + std::chrono::seconds(1)) && frame.startHz == 0.0, "first delayed row was evicted");

    frame.startHz = 1000.0;
    frame.spanHz = 16384;
    frame.bins.assign(16384, -120.0f);
    frame.bins[200] = -33.0f;
    frame.bins[203] = -42.0f;
    std::vector<float> out(8192);
    expect(mapSpectrumFrame(frame, frame.startHz, frame.spanHz, out.data(), (int)out.size()), "wide row dropped");
    expect(out[100] == -33.0f && out[101] == -42.0f, "reduction lost a narrow peak");
}

void testLocalFftHandoff() {
    constexpr int localSize = 65536;
    constexpr int serverSize = 1024;
    struct TestProvider : SpectrumProvider {
        std::string name() const override { return "test"; }
        int binCount() const override { return serverSize; }
        std::vector<double> zoomSpans() const override { return {}; }
        void requestSpan(double) override {}
        bool available() const override { return true; }
    } provider;
    struct Destination {
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<float> bins = std::vector<float>(localSize);
        bool acquired = false;
        bool canWrite = false;
        std::atomic<int> rows{0};
    } destination;
    auto acquire = [](void* ctx) -> float* {
        auto& dest = *static_cast<Destination*>(ctx);
        std::unique_lock lock(dest.mutex);
        dest.acquired = true;
        dest.ready.notify_one();
        dest.ready.wait(lock, [&] { return dest.canWrite; });
        return dest.bins.data();
    };
    auto release = [](void* ctx) { ++static_cast<Destination*>(ctx)->rows; };
    dsp::stream<dsp::complex_t> input(localSize);
    auto& frontend = sigpath::iqFrontEnd;
    struct Stop {
        IQFrontEnd& frontend;
        Destination& destination;
        ~Stop() {
            {
                std::lock_guard lock(destination.mutex);
                destination.canWrite = true;
                destination.ready.notify_one();
            }
            frontend.stop();
        }
    };
    frontend.init(&input, localSize, false, 1, false, localSize, 1, IQFrontEnd::RECTANGULAR,
                  acquire, release, &destination);
    Stop stop{frontend, destination};
    frontend.start();
    for (int i = 0; i < 3; ++i) {
        {
            std::lock_guard lock(destination.mutex);
            destination.acquired = false;
            destination.canWrite = false;
        }
        std::fill_n(input.writeBuf, localSize, dsp::complex_t{1.0f, 0.0f});
        expect(input.swap(localSize), "could not feed the local FFT");
        bool acquired;
        {
            std::unique_lock lock(destination.mutex);
            acquired = destination.ready.wait_for(lock, std::chrono::seconds(2), [&] { return destination.acquired; });
        }
        std::thread allowWrite([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            std::lock_guard lock(destination.mutex);
            destination.canWrite = true;
            destination.ready.notify_one();
        });
        frontend.setSpectrumProvider(&provider);
        const bool wroteBeforeHandoff = destination.rows.load() == i + 1;
        allowWrite.join();
        expect(acquired && wroteBeforeHandoff, "server provider took ownership while the local FFT was writing");
        gui::waterfall.setRawFFTSize(serverSize);
        destination.bins.resize(serverSize);
        frontend.setSpectrumProvider(nullptr);
        expect(gui::waterfall.getRawFFTSize() == localSize, "fallback did not restore local FFT size");
        destination.bins.resize(localSize);
    }
}

void testMapShift() {
    // Same span and size, display shifted by +2 bins: a whole-bin shift with
    // the top two bins uncovered.
    SpectrumFrame f = ramp(8, 1000.0, 8.0);
    std::vector<float> out(8);
    expect(mapSpectrumFrame(f, 1002.0, 8.0, out.data(), 8), "shifted frame dropped");
    expect(out == std::vector<float>({ 2, 3, 4, 5, 6, 7, kUncoveredDb, kUncoveredDb }), "whole-bin shift wrong");

    expect(mapSpectrumFrame(f, 998.0, 8.0, out.data(), 8), "shifted frame dropped");
    expect(out == std::vector<float>({ kUncoveredDb, kUncoveredDb, 0, 1, 2, 3, 4, 5 }), "negative shift wrong");
}

void testCarrierWithTuningOffset() {
    constexpr double offset = 250000.0;
    constexpr double centre = 1004.0;
    for (bool invert : {false, true}) {
        auto f = ramp(8, 0.0, 8.0);
        f.bins.assign(8, -120.0f);
        f.bins[6] = -20.0f;
        placeSpectrumFrame(f, 1000.0 + offset, offset, centre, invert);
        std::vector<float> out(8);
        expect(mapSpectrumFrame(f, 1000.0, 8.0, out.data(), 8), "shifted carrier outside the display");
        expect(out[invert ? 1 : 6] == -20.0f, "carrier mapped to the wrong display frequency");
    }
}

void testMapResample() {
    // A frame twice as wide as the display: every other bin.
    SpectrumFrame wide = ramp(8, 0.0, 16.0);
    std::vector<float> out(4);
    expect(mapSpectrumFrame(wide, 4.0, 8.0, out.data(), 4), "wide frame dropped");
    expect(out == std::vector<float>({ 2, 3, 4, 5 }), "narrowing wrong");

    // A frame with half the bins over the same span: each bin twice.
    SpectrumFrame coarse = ramp(4, 0.0, 8.0);
    std::vector<float> out8(8);
    expect(mapSpectrumFrame(coarse, 0.0, 8.0, out8.data(), 8), "coarse frame dropped");
    expect(out8 == std::vector<float>({ 0, 0, 1, 1, 2, 2, 3, 3 }), "widening wrong");
}

void testMapDropsPoorOverlap() {
    SpectrumFrame f = ramp(8, 0.0, 8.0);
    std::vector<float> out(8, 42.0f);
    expect(!mapSpectrumFrame(f, 5.0, 8.0, out.data(), 8), "frame covering 3/8 of the display kept");
    expect(out[0] == 42.0f, "dropped frame wrote into the line");
    expect(mapSpectrumFrame(f, 4.0, 8.0, out.data(), 8), "frame covering half the display dropped");
    SpectrumFrame empty;
    empty.spanHz = 8.0;
    expect(!mapSpectrumFrame(empty, 0.0, 8.0, out.data(), 8), "empty frame kept");
}

void testServerZoomWheel() {
    ServerZoom z;
    z.setLadder({ 30e6 }, 30e6);
    expect(!z.active(), "a one-rung ladder enabled server zoom");

    const std::vector<double> ladder = { 30e6, 15e6, 7.5e6 };
    z.setLadder(ladder, 15e6);
    expect(z.active() && z.rung() == 1, "ladder not synced to the current span");

    const auto t0 = ServerZoom::Clock::now();
    z.wheel(2.0f);
    expect(z.tick(t0) == 7.5e6, "first wheel step not taken");
    // The ladder ends here: the rest of the wheel is discarded.
    expect(z.tick(t0 + std::chrono::milliseconds(200)) == 0.0, "stepped past the narrowest rung");
    z.wheel(-2.0f);
    expect(z.tick(t0 + std::chrono::milliseconds(300)) == 15e6, "zoom out not taken");
    expect(z.tick(t0 + std::chrono::milliseconds(350)) == 0.0, "second step within the interval");
    expect(z.tick(t0 + std::chrono::milliseconds(460)) == 30e6, "second step not taken after the interval");

    // A fractional pinch accumulates to a step.
    z.wheel(0.6f);
    expect(z.tick(t0 + std::chrono::milliseconds(700)) == 0.0, "partial pinch stepped");
    z.wheel(0.6f);
    expect(z.tick(t0 + std::chrono::milliseconds(700)) == 15e6, "accumulated pinch did not step");

    // Setting the same ladder again keeps the rung.
    z.setLadder(ladder, 30e6);
    expect(z.rung() == 1, "unchanged ladder reset the rung");
    z.confirm(7.4e6);
    expect(z.rung() == 2, "confirmed span not mapped to the nearest rung");
}

void testServerZoomSlider() {
    ServerZoom z;
    z.setLadder({ 30e6, 15e6, 7.5e6, 3.75e6, 1.875e6 }, 30e6);
    expect(z.sliderPos() == 1.0f, "widest rung not at the slider top");
    expect(z.slider(0.0f) == 1.875e6, "slider bottom not the narrowest rung");
    expect(z.slider(0.05f) == 0.0, "same rung requested twice");
    expect(z.slider(0.5f) == 7.5e6, "slider middle not the middle rung");
    expect(z.sliderPos() == 0.5f, "slider position not derived from the rung");
}

} // namespace

int main() {
    return test_support::run("spectrum_frame_tests", [] {
        testQueueOrderAndDue();
        testQueueDropsOldest();
        testDelayedRowsAndPeakReduction();
        testMapShift();
        testCarrierWithTuningOffset();
        testMapResample();
        testMapDropsPoorOverlap();
        testServerZoomWheel();
        testServerZoomSlider();
        testLocalFftHandoff();
    });
}
