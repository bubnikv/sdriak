#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

// One waterfall line from a SpectrumProvider, in the display domain.
struct SpectrumFrame {
    std::vector<float> bins;  // dB, ascending frequency
    double startHz = 0.0;     // lower edge of bins[0], absolute Hz
    double spanHz = 0.0;      // width covered by bins
    // Show no earlier than this; the epoch means show at once.
    std::chrono::steady_clock::time_point presentAt{};
};

inline void placeSpectrumFrame(SpectrumFrame& frame, double nativeStart, double tuningOffset,
                               double displayCentre, bool invertIQ) {
    frame.startHz = nativeStart - tuningOffset;
    if (invertIQ) {
        std::reverse(frame.bins.begin(), frame.bins.end());
        frame.startHz = 2.0 * displayCentre - (frame.startHz + frame.spanHz);
    }
}

// Frames from a network thread to the UI thread. A fixed ring: push never
// blocks and never allocates a slot, pop recycles the caller's vector into
// the ring, so a steady stream does not allocate.
class SpectrumQueue {
public:
    // One second of receiver delay at 20 rows/s, with room for bursts.
    static constexpr size_t kSlots = 64;

    // Any thread. Drops the frame when the UI holds the lock, and the
    // oldest frame when the ring is full; both count as dropped.
    bool push(SpectrumFrame&& frame) {
        std::unique_lock lck(m_mtx, std::try_to_lock);
        if (!lck.owns_lock()) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (m_count == kSlots) {
            m_head = (m_head + 1) % kSlots;
            m_count--;
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        SpectrumFrame& slot = m_ring[(m_head + m_count) % kSlots];
        slot.bins.swap(frame.bins);
        slot.startHz = frame.startHz;
        slot.spanHz = frame.spanHz;
        slot.presentAt = frame.presentAt;
        m_count++;
        return true;
    }

    // UI thread. The oldest frame if it is due at `now`. Frames are kept in
    // push order, so a frame that is not yet due holds back the ones after
    // it.
    bool pop(SpectrumFrame& out, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard lck(m_mtx);
        if (m_count == 0) { return false; }
        SpectrumFrame& front = m_ring[m_head];
        if (front.presentAt != std::chrono::steady_clock::time_point{} && front.presentAt > now) { return false; }
        out.bins.swap(front.bins);
        out.startHz = front.startHz;
        out.spanHz = front.spanHz;
        out.presentAt = front.presentAt;
        m_head = (m_head + 1) % kSlots;
        m_count--;
        return true;
    }

    void clear() {
        std::lock_guard lck(m_mtx);
        m_head = 0;
        m_count = 0;
    }

    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    std::mutex m_mtx;
    std::array<SpectrumFrame, kSlots> m_ring;
    size_t m_head = 0;
    size_t m_count = 0;
    std::atomic<uint64_t> m_dropped{0};
};

// Fill out[0..outSize) covering [displayStart, displayStart + displaySpan)
// from the frame, nearest neighbour when expanding, peak when reducing. When the frame has the
// display's span and bin count this is a shift by whole bins. Display bins
// the frame does not cover get kUncoveredDb. Returns false, leaving out
// untouched, for an empty frame or one overlapping less than half the
// display.
constexpr float kUncoveredDb = -200.0f;

inline bool mapSpectrumFrame(const SpectrumFrame& frame, double displayStart, double displaySpan, float* out, int outSize) {
    const int n = (int)frame.bins.size();
    if (n == 0 || outSize <= 0 || !(frame.spanHz > 0.0) || !(displaySpan > 0.0)) { return false; }

    const double frameEnd = frame.startHz + frame.spanHz;
    const double displayEnd = displayStart + displaySpan;
    const double overlap = (frameEnd < displayEnd ? frameEnd : displayEnd) - (frame.startHz > displayStart ? frame.startHz : displayStart);
    if (overlap < displaySpan / 2.0) { return false; }

    const double outBin = displaySpan / (double)outSize;
    const double inBin = frame.spanHz / (double)n;
    for (int i = 0; i < outSize; i++) {
        const double left = (displayStart - frame.startHz + i * outBin) / inBin;
        const double ratio = outBin / inBin;
        if (ratio <= 1.0) {
            const double pos = std::floor(left + ratio * 0.5);
            out[i] = (pos >= 0.0 && pos < (double)n) ? frame.bins[(size_t)pos] : kUncoveredDb;
        }
        else {
            const int first = (int)std::clamp(std::ceil(left - 0.5), 0.0, (double)n);
            const int end = (int)std::clamp(std::ceil(left + ratio - 0.5), 0.0, (double)n);
            out[i] = first < end ? *std::max_element(frame.bins.begin() + first, frame.bins.begin() + end) : kUncoveredDb;
        }
    }
    return true;
}
