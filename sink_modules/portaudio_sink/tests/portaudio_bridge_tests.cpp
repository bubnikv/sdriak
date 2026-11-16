#include "portaudio_bridge.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void publish(dsp::stream<dsp::stereo_t>& source, const std::vector<dsp::stereo_t>& input) {
    expect(input.size() <= (size_t)source.capacity(), "test input exceeds the DSP stream");
    std::copy(input.begin(), input.end(), source.writeBuf);
    expect(source.swap((int)input.size()), "DSP stream stopped during publish");
}

void waitForAvailable(const PortAudioBridge& bridge, size_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (bridge.availableFrames() < frames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expect(bridge.availableFrames() >= frames, "bridge worker did not publish samples");
}

std::vector<float> popFrames(PortAudioBridge& bridge, size_t frames) {
    waitForAvailable(bridge, frames);
    std::vector<float> output(frames * (size_t)bridge.channels(), -999.0f);
    expect(bridge.pop(output.data(), frames) == frames, "bridge returned a short read");
    return output;
}

void testStorageIsBoundedAtMaximumRate() {
    dsp::stream<dsp::stereo_t> source;
    PortAudioBridge bridge;
    bridge.init(&source);
    bridge.configure(2, 800); // 48 kHz / 60 Hz.

    expect(bridge.capacityFrames() == 2048, "48 kHz bridge has an unexpected capacity");
    expect(bridge.capacityFrames() * (size_t)bridge.channels() * sizeof(float) == 16 * 1024,
           "48 kHz stereo bridge exceeds its 16 KiB sample-storage bound");
}

void testStereoOrderAndWraparound() {
    dsp::stream<dsp::stereo_t> source;
    PortAudioBridge bridge;
    bridge.init(&source);
    bridge.configure(2, 1); // The minimum 256-frame queue.
    bridge.start();

    size_t sequence = 0;
    const size_t chunks[] = { 73, 191, 17, 128, 251, 9 };
    for (size_t chunk : chunks) {
        std::vector<dsp::stereo_t> input(chunk);
        for (size_t i = 0; i < chunk; ++i, ++sequence) {
            input[i] = { (float)sequence, -(float)sequence };
        }

        publish(source, input);
        const std::vector<float> output = popFrames(bridge, chunk);
        for (size_t i = 0; i < chunk; ++i) {
            expect(output[2 * i] == input[i].l, "left stereo channel changed");
            expect(output[2 * i + 1] == input[i].r, "right stereo channel changed");
        }
    }

    bridge.stop();
}

void testMonoConversionMatchesStereoToMono() {
    dsp::stream<dsp::stereo_t> source;
    PortAudioBridge bridge;
    bridge.init(&source);
    bridge.configure(1, 32);
    bridge.start();

    std::vector<dsp::stereo_t> input = {
        { 1.0f, 3.0f }, { -4.0f, 2.0f }, { 0.25f, -0.75f }, { 100.0f, 20.0f }
    };
    publish(source, input);
    const std::vector<float> output = popFrames(bridge, input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        expect(output[i] == (input[i].l + input[i].r) / 2.0f,
               "mono conversion differs from StereoToMono");
    }

    bridge.stop();
}

void testOverflowKeepsNewestBoundedAudio() {
    dsp::stream<dsp::stereo_t> source;
    PortAudioBridge bridge;
    bridge.init(&source);
    bridge.configure(1, 1); // Capacity is 256 frames.
    bridge.start();

    std::vector<dsp::stereo_t> input(300);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = { (float)i, (float)i };
    }
    publish(source, input);

    const std::vector<float> output = popFrames(bridge, 256);
    for (size_t i = 0; i < output.size(); ++i) {
        expect(output[i] == (float)(i + 44), "overflow did not retain the newest audio");
    }
    expect(bridge.availableFrames() == 0, "overflow exceeded the bounded queue");

    bridge.stop();
}

void testDiscardAndUnderflowAreNonBlocking() {
    dsp::stream<dsp::stereo_t> source;
    PortAudioBridge bridge;
    bridge.init(&source);
    bridge.configure(2, 8);
    bridge.start();

    std::vector<dsp::stereo_t> input(12, { 2.0f, -2.0f });
    publish(source, input);
    waitForAvailable(bridge, input.size());
    bridge.discard();
    expect(bridge.availableFrames() == 0, "discard left queued audio");

    float untouched[] = { 17.0f, 18.0f };
    expect(bridge.pop(untouched, 1) == 0, "empty bridge did not report underflow");
    expect(untouched[0] == 17.0f && untouched[1] == 18.0f,
           "empty bridge wrote beyond the returned frame count");

    bridge.stop();
}

} // namespace

int main() {
    try {
        testStorageIsBoundedAtMaximumRate();
        testStereoOrderAndWraparound();
        testMonoConversionMatchesStereoToMono();
        testOverflowKeepsNewestBoundedAudio();
        testDiscardAndUnderflowAreNonBlocking();
        std::cout << "portaudio_bridge_tests passed\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "portaudio_bridge_tests failed: " << e.what() << '\n';
        return 1;
    }
}
