#include "test_support.h"

#include <dsp/buffer/frame_tap.h>

#include <vector>

namespace {

using test_support::expect;

struct Recorder {
    std::vector<std::vector<float>> frames;

    static void handler(const float* frame, int count, void* ctx) {
        ((Recorder*)ctx)->frames.emplace_back(frame, frame + count);
    }
};

// Pushes the ramp 0, 1, 2, ... (total samples) in blocks of `chunk` and checks
// the frames against the contract. Sliding/contiguous frames end at each
// interval boundary; decimating frames preserve Reshaper's old phase and hold
// the first `keep` samples of each interval.
void expectFrames(int keep, int interval, int chunk, int total) {
    Recorder rec;
    dsp::buffer::FrameTap<float> tap(keep, interval, Recorder::handler, &rec);

    std::vector<float> ramp(total);
    for (int i = 0; i < total; i++) { ramp[i] = (float)i; }
    for (int pos = 0; pos < total; pos += chunk) {
        tap.push(&ramp[pos], std::min(chunk, total - pos));
    }

    size_t f = 0;
    for (int end = interval; end <= total; end += interval) {
        if (end < keep) { continue; }
        expect(f < rec.frames.size(), "tap published too few frames");
        const std::vector<float>& frame = rec.frames[f++];
        expect((int)frame.size() == keep, "frame has the wrong length");
        const int start = (interval > keep) ? (end - interval) : (end - keep);
        for (int i = 0; i < keep; i++) {
            expect(frame[i] == (float)(start + i), "frame holds the wrong samples");
        }
    }
    expect(f == rec.frames.size(), "tap published too many frames");
}

void testDecimatingPhaseMatchesReshaper() {
    Recorder rec;
    dsp::buffer::FrameTap<float> tap(4, 10, Recorder::handler, &rec);
    float ramp[25];
    for (int i = 0; i < 25; ++i) { ramp[i] = (float)i; }

    // Split both the kept prefix and skipped tail across arbitrary pushes.
    tap.push(ramp, 2);
    tap.push(ramp + 2, 5);
    tap.push(ramp + 7, 11);
    tap.push(ramp + 18, 7);

    expect(rec.frames.size() == 2, "decimating tap published at the wrong cadence");
    expect(rec.frames[0] == std::vector<float>({ 0, 1, 2, 3 }),
           "first decimating frame moved to the end of its interval");
    expect(rec.frames[1] == std::vector<float>({ 10, 11, 12, 13 }),
           "second decimating frame lost the old Reshaper phase");
}

void testFrameBoundariesIgnoreBlockSize() {
    const int chunks[] = { 1, 7, 99, 100, 101, 250, 5000 };
    for (int chunk : chunks) {
        expectFrames(100, 100, chunk, 2000);   // contiguous (VOR, M17)
        expectFrames(100, 13, chunk, 2000);    // sliding window (RDS, POCSAG)
        expectFrames(100, 333, chunk, 2000);   // decimating (Meteor, RyFi)
        expectFrames(1, 1, chunk, 50);         // degenerate
    }
}

void testUninitialisedTapIgnoresPush() {
    dsp::buffer::FrameTap<float> tap;
    float data[16] = {};
    tap.push(data, 16);   // must neither crash nor call anything
}

void testResetForgetsHistory() {
    Recorder rec;
    dsp::buffer::FrameTap<float> tap(4, 4, Recorder::handler, &rec);
    float a[3] = { 1, 2, 3 };
    tap.push(a, 3);
    tap.reset();
    float b[4] = { 10, 11, 12, 13 };
    tap.push(b, 4);
    expect(rec.frames.size() == 1, "reset tap published the wrong number of frames");
    expect(rec.frames[0] == std::vector<float>({ 10, 11, 12, 13 }), "reset tap kept samples from before reset()");
}

} // namespace

int main() {
    return test_support::run("frame_tap_tests", [] {
        testFrameBoundariesIgnoreBlockSize();
        testDecimatingPhaseMatchesReshaper();
        testUninitialisedTapIgnoresPush();
        testResetForgetsHistory();
    });
}
