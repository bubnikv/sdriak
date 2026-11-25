#include "test_support.h"

#include <dsp/buffer/prebuffer.h>
#include <dsp/channel/rx_vfo.h>
#include <signal_path/iq_window_policy.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace {

using test_support::expect;

bool approxEqual(double a, double b) { return std::fabs(a - b) < 1e-6; }

IQWindowInput window(double current, std::vector<std::pair<double, double>> passbands) {
    IQWindowInput in;
    in.passbands = std::move(passbands);
    in.currentOffset = current;
    in.width = 10000.0;
    in.margin = 1000.0;
    in.minOffset = -50000.0;
    in.maxOffset = 50000.0;
    return in;
}

void testPolicy() {
    // Nothing to place: the window stays.
    expect(approxEqual(chooseIQWindow(window(1234.0, {})), 1234.0), "empty input moved the window");
    // Every passband fits: hysteresis keeps the offset, even near an edge.
    expect(approxEqual(chooseIQWindow(window(0.0, { { 3000.0, 4900.0 }, { -4900.0, -4000.0 } })), 0.0),
        "fitting passbands moved the window");
    // One passband leaves: centre on the union, which fits with margins.
    expect(approxEqual(chooseIQWindow(window(0.0, { { 4000.0, 6000.0 }, { 2000.0, 3000.0 } })), 4000.0),
        "window not centred on the passband union");
    // The union is too wide: centre on the selected VFO.
    expect(approxEqual(chooseIQWindow(window(0.0, { { 20000.0, 22000.0 }, { -1000.0, 1000.0 } })), 21000.0),
        "window not centred on the selected VFO");
    // A union exactly width - 2 margins wide still counts as fitting.
    expect(approxEqual(chooseIQWindow(window(0.0, { { 10000.0, 12000.0 }, { 16000.0, 18000.0 } })), 14000.0),
        "union at the margin limit not centred");
    // Clamped to the slide limits.
    expect(approxEqual(chooseIQWindow(window(0.0, { { 90000.0, 92000.0 } })), 50000.0),
        "window not clamped to the slide limit");
    IQWindowInput reversed = window(0.0, { { -92000.0, -90000.0 } });
    std::swap(reversed.minOffset, reversed.maxOffset);
    expect(approxEqual(chooseIQWindow(reversed), -50000.0), "reversed limits not handled");
}

void testStreamPositionOrder() {
    dsp::buffer::StreamPosition pos;
    std::vector<int> fired;
    pos.schedule(30, [&] { fired.push_back(30); });
    pos.schedule(10, [&] { fired.push_back(10); });
    pos.schedule(20, [&] { fired.push_back(20); });
    pos.fireBefore(10);
    expect(fired.empty(), "marker fired before its sample");
    pos.fireBefore(25);
    expect(fired == std::vector<int>({ 10, 20 }), "markers fired out of order");
    pos.fireBefore(1000);
    expect(fired == std::vector<int>({ 10, 20, 30 }), "last marker not fired");
}

// Streams the ramp 0, 1, 2, ... (one sample per index) through a
// PrebufferedLink and checks that every marker fires before the reader sees
// the block holding its sample, and that the input counter is exact.
void runLink(int prebufferMsec) {
    constexpr int kTotal = 4096;
    constexpr int kChunk = 64;
    constexpr double kRate = 20000.0;

    dsp::stream<float> in, out;
    dsp::buffer::PrebufferedLink<float> link;
    link.init(&in, &out);
    link.setSampleRate(kRate);
    link.setPrebufferMsec(prebufferMsec, true, false);

    const std::vector<uint64_t> marks = { 0, 1, 63, 64, 1000, 2047, 3000 };
    std::vector<std::atomic<bool>> fired(marks.size());
    for (size_t i = 0; i < marks.size(); i++) {
        fired[i] = false;
        link.scheduleMarker(marks[i], [&fired, i] { fired[i] = true; });
    }

    std::atomic<bool> ok{true};
    std::atomic<int> seen{0};
    std::thread reader([&] {
        while (true) {
            int count = out.read();
            if (count < 0) { break; }
            uint64_t first = (uint64_t)out.readBuf[0];
            uint64_t end = first + count;
            for (size_t i = 0; i < marks.size(); i++) {
                if (marks[i] < end && !fired[i]) { ok = false; }
            }
            seen = (int)end;
            out.flush();
        }
    });

    link.start();
    for (int pos = 0; pos < kTotal; pos += kChunk) {
        for (int i = 0; i < kChunk; i++) { in.writeBuf[i] = (float)(pos + i); }
        expect(in.swap(kChunk), "input swap failed");
    }
    // The prebuffer releases whole 10 ms slices only; wait for what it can.
    const int expected = prebufferMsec > 0 ? 3000 : kTotal;
    for (int i = 0; i < 400 && (seen < expected || link.samplesIn() < (uint64_t)kTotal); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    link.stop();
    out.stopReader();
    reader.join();

    expect(ok, "a marker fired after its block was published");
    expect(seen >= expected, "link did not deliver the stream");
    expect(link.samplesIn() == (uint64_t)kTotal, "input counter wrong");
    for (size_t i = 0; i < marks.size(); i++) {
        if (marks[i] < (uint64_t)seen) { expect(fired[i], "marker for a delivered sample did not fire"); }
    }
}

void testBypassMarkers() { runLink(0); }
void testPrebufferMarkers() { runLink(30); }

void testMute() {
    constexpr int kBlock = 1024;
    dsp::channel::RxVFOKernel vfo(48000.0, 48000.0, 48000.0, 0.0, kBlock);
    std::vector<dsp::complex_t> in(kBlock, dsp::complex_t{ 1.0f, 0.0f });
    std::vector<dsp::complex_t> out(vfo.maxOutputBuffer());

    auto energy = [&](int count) {
        double e = 0.0;
        for (int i = 0; i < count; i++) { e += out[i].re * out[i].re + out[i].im * out[i].im; }
        return e;
    };

    int n = vfo.process(kBlock, in.data(), out.data());
    expect(n > 0 && energy(n) > 0.0, "unmuted VFO produced silence");

    vfo.muteFor(1500);
    n = vfo.process(kBlock, in.data(), out.data());
    expect(n > 0 && energy(n) == 0.0, "muteFor did not silence the first block");
    n = vfo.process(kBlock, in.data(), out.data());
    expect(n > 0 && energy(n) == 0.0, "muteFor did not silence the remainder");
    n = vfo.process(kBlock, in.data(), out.data());
    expect(n > 0 && energy(n) > 0.0, "muteFor countdown never ended");

    vfo.setMuted(true);
    n = vfo.process(kBlock, in.data(), out.data());
    expect(n == kBlock, "muted VFO changed its sample count");
    expect(energy(n) == 0.0, "muted VFO produced output");
    vfo.setMuted(false);
    n = vfo.process(kBlock, in.data(), out.data());
    expect(energy(n) > 0.0, "unmuted VFO stayed silent");
}

} // namespace

int main() {
    return test_support::run("iq_window_tests", [] {
        testPolicy();
        testStreamPositionOrder();
        testBypassMarkers();
        testPrebufferMarkers();
        testMute();
    });
}
