#pragma once

#include <dsp/block.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

// A single DSP worker feeds the PortAudio callback through a bounded SPSC
// queue. The producer may block on the upstream DSP stream, but the realtime
// callback only performs lock-free atomic loads/stores and copies already
// available samples. Storage is allocated by configure(), never in a hot path.
class PortAudioBridge : public dsp::block {
public:
    static_assert(std::atomic<size_t>::is_always_lock_free,
                  "PortAudioBridge requires lock-free native-word atomics");

    PortAudioBridge() = default;

    ~PortAudioBridge() override {
        // Derived members are destroyed before dsp::block's destructor, so
        // join the worker while the FIFO still exists.
        if (_block_init) { stop(); }
    }

    void init(dsp::stream<dsp::stereo_t>* input) {
        _input = input;
        registerInput(_input);
        _block_init = true;
    }

    void configure(int channels, size_t periodFrames) {
        assert(_block_init);
        assert(!running);
        assert(channels == 1 || channels == 2);

        // Two periods absorb normal scheduling jitter without allowing audio
        // latency to grow without bound. A power of two makes wraparound cheap.
        const size_t requestedFrames = std::max<size_t>(256, periodFrames * 2);
        const size_t newCapacity = nextPowerOfTwo(requestedFrames);
        if (_capacityFrames == newCapacity && _channels == channels) {
            reset();
            return;
        }

        std::vector<float> newFifo(newCapacity * (size_t)channels);

        _fifo = std::move(newFifo);
        _capacityFrames = newCapacity;
        _frameMask = newCapacity - 1;
        _channels = channels;
        reset();
    }

    size_t pop(float* output, size_t requestedFrames) noexcept {
        const size_t read = _readSequence.load(std::memory_order_relaxed);
        const size_t write = _writeSequence.load(std::memory_order_acquire);
        const size_t available = write - read;
        const size_t frames = std::min(requestedFrames, available);
        if (!frames) { return 0; }

        const size_t readIndex = read & _frameMask;
        const size_t firstFrames = std::min(frames, _capacityFrames - readIndex);
        copyOut(output, readIndex, firstFrames);
        if (firstFrames != frames) {
            copyOut(output + firstFrames * (size_t)_channels, 0, frames - firstFrames);
        }

        _readSequence.store(read + frames, std::memory_order_release);
        return frames;
    }

    size_t availableFrames() const noexcept {
        const size_t read = _readSequence.load(std::memory_order_relaxed);
        const size_t write = _writeSequence.load(std::memory_order_acquire);
        return write - read;
    }

    void discard() noexcept {
        _readSequence.store(_writeSequence.load(std::memory_order_acquire), std::memory_order_release);
    }

    void reset() noexcept {
        assert(!running);
        _readSequence.store(0, std::memory_order_relaxed);
        _writeSequence.store(0, std::memory_order_relaxed);
    }

    int channels() const noexcept { return _channels; }
    size_t capacityFrames() const noexcept { return _capacityFrames; }

    int run() override {
        const int count = _input->read();
        if (count < 0) { return -1; }

        push(_input->readBuf, (size_t)count);
        _input->flush();
        return count;
    }

private:
    static size_t nextPowerOfTwo(size_t value) noexcept {
        size_t result = 1;
        while (result < value) { result <<= 1; }
        return result;
    }

    void push(const dsp::stereo_t* input, size_t inputFrames) noexcept {
        const size_t write = _writeSequence.load(std::memory_order_relaxed);
        const size_t read = _readSequence.load(std::memory_order_acquire);
        const size_t used = write - read;
        const size_t writable = (used < _capacityFrames) ? (_capacityFrames - used) : 0;
        const size_t frames = std::min(inputFrames, writable);
        if (!frames) { return; }

        // If an upstream block is larger than the available space, retain its
        // newest samples. The producer never moves the consumer-owned cursor.
        input += inputFrames - frames;

        const size_t writeIndex = write & _frameMask;
        const size_t firstFrames = std::min(frames, _capacityFrames - writeIndex);
        copyIn(writeIndex, input, firstFrames);
        if (firstFrames != frames) {
            copyIn(0, input + firstFrames, frames - firstFrames);
        }

        _writeSequence.store(write + frames, std::memory_order_release);
    }

    void copyIn(size_t writeIndex, const dsp::stereo_t* input, size_t frames) noexcept {
        float* output = &_fifo[writeIndex * (size_t)_channels];
        if (_channels == 2) {
            static_assert(std::is_trivially_copyable<dsp::stereo_t>::value,
                          "stereo_t must be safe to copy into an interleaved float buffer");
            static_assert(sizeof(dsp::stereo_t) == 2 * sizeof(float),
                          "stereo_t must contain exactly two floats");
            std::memcpy(output, input, frames * sizeof(dsp::stereo_t));
            return;
        }

        for (size_t i = 0; i < frames; ++i) {
            output[i] = (input[i].l + input[i].r) / 2.0f;
        }
    }

    void copyOut(float* output, size_t readIndex, size_t frames) const noexcept {
        std::memcpy(output, &_fifo[readIndex * (size_t)_channels],
                    frames * (size_t)_channels * sizeof(float));
    }

    dsp::stream<dsp::stereo_t>* _input = nullptr;
    std::vector<float> _fifo;
    size_t _capacityFrames = 0;
    size_t _frameMask = 0;
    int _channels = 0;

    // Each cursor has one writer. Keeping them on separate cache lines avoids
    // producer/consumer false sharing in the steady-state hot path.
    alignas(64) std::atomic<size_t> _readSequence{ 0 };
    alignas(64) std::atomic<size_t> _writeSequence{ 0 };
};
