#pragma once
#include "../block.h"
#include "../routing/stream_link.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace dsp::buffer {
    // Input-sample counter and stream-position markers shared by the two
    // paths of a PrebufferedLink. Exactly one path counts and fires at a
    // time: the Prebuffer when routed through it, the bypass link otherwise.
    class StreamPosition {
    public:
        // Total input samples accepted so far.
        uint64_t samplesIn() const { return m_samplesIn.load(std::memory_order_acquire); }
        void addInput(size_t count) { m_samplesIn.fetch_add(count, std::memory_order_acq_rel); }

        // Any thread. cb runs on the output thread before the block that
        // holds input sample `inputIndex` is published, or with the next
        // published block when that sample was dropped.
        void schedule(uint64_t inputIndex, std::function<void()> cb) {
            std::lock_guard lck(m_mtx);
            m_markers.emplace_back(inputIndex, std::move(cb));
            m_pending.store(m_markers.size(), std::memory_order_release);
        }

        // Output thread: run every marker whose index is below endIndex,
        // in index order, outside the lock.
        void fireBefore(uint64_t endIndex) {
            if (m_pending.load(std::memory_order_acquire) == 0) { return; }
            std::vector<std::pair<uint64_t, std::function<void()>>> due;
            {
                std::lock_guard lck(m_mtx);
                auto split = std::stable_partition(m_markers.begin(), m_markers.end(),
                    [endIndex](const auto& m) { return m.first >= endIndex; });
                due.assign(std::make_move_iterator(split), std::make_move_iterator(m_markers.end()));
                m_markers.erase(split, m_markers.end());
                m_pending.store(m_markers.size(), std::memory_order_release);
            }
            std::stable_sort(due.begin(), due.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& m : due) { m.second(); }
        }

    private:
        std::atomic<uint64_t> m_samplesIn{0};
        std::atomic<size_t> m_pending{0};
        std::mutex m_mtx;
        std::vector<std::pair<uint64_t, std::function<void()>>> m_markers;
    };

    template <class T>
    class Prebuffer : public block {
        using base_type = block;
        using Clock = std::chrono::steady_clock;

    public:
        Prebuffer() = default;
        Prebuffer(stream<T>* in) { init(in); }

        void init(stream<T>* in) {
            input = in;
            registerInput(input);
            registerOutput(&out);
            _block_init = true;
        }

        void setPrebufferMsec(int msec) {
            std::lock_guard lck(bufferMtx);
            prebufferMsec = std::max(0, msec);
            trimLocked();
            resizeRingLocked(maxBufferedSamplesLocked());
        }

        void setSampleRate(double sr) {
            std::lock_guard lck(bufferMtx);
            sampleRate = std::max(1.0, sr);
            clearLocked();
            resizeRingLocked(maxBufferedSamplesLocked());
        }

        void clear() {
            std::lock_guard lck(bufferMtx);
            clearLocked();
        }

        double getSampleRate() {
            std::lock_guard lck(bufferMtx);
            return sampleRate;
        }

        int getPercentFull() {
            std::lock_guard lck(bufferMtx);
            size_t target = targetSamplesLocked();
            if (target == 0) { return 100; }
            return (int)std::min<size_t>(100, (bufferedSamples * 100) / target);
        }

        // Count input and fire markers through `pos` (nullptr: neither).
        // Set before start().
        void setPosition(StreamPosition* pos) {
            std::lock_guard lck(bufferMtx);
            position = pos;
        }

        size_t getBufferedSamples() {
            std::lock_guard lck(bufferMtx);
            return bufferedSamples;
        }

        // Seconds from now until input sample `inputIndex` is published,
        // negative when it already left. Includes the wait for priming.
        double secondsUntilOutput(uint64_t inputIndex) {
            std::lock_guard lck(bufferMtx);
            if (!position) { return 0.0; }
            double outIndex = (double)(position->samplesIn() - bufferedSamples);
            double delay = (double)inputIndex - outIndex;
            size_t target = targetSamplesLocked();
            if (!primed && target > bufferedSamples) { delay += (double)(target - bufferedSamples); }
            return delay / sampleRate;
        }

        int run() override {
            auto nextTick = Clock::now();

            while (!stopRequested.load()) {
                size_t target = 0;
                size_t slice = 0;
                size_t available = 0;
                bool primedNow = false;
                {
                    std::lock_guard lck(bufferMtx);
                    target = targetSamplesLocked();
                    slice = sliceSamplesLocked();
                    available = bufferedSamples;

                    if (target == 0) {
                        primed = available > 0;
                    }
                    else if (!primed && available >= target) {
                        primed = true;
                        nextTick = Clock::now();
                    }
                    primedNow = primed;
                }

                if (target == 0) {
                    int count = 0;
                    uint64_t endIndex = 0;
                    {
                        std::lock_guard lck(bufferMtx);
                        endIndex = outputIndexLocked();
                        count = (int)popLocked(out.writeBuf, std::min(bufferedSamples, (size_t)out.capacity()));
                        endIndex += count;
                    }
                    if (count > 0) {
                        if (position) { position->fireBefore(endIndex); }
                        if (!out.swap(count)) { return -1; }
                        continue;
                    }
                    if (waitForProducer(std::chrono::milliseconds(TICK_MS)) < 0) { return -1; }
                    continue;
                }

                if (!primedNow) {
                    if (waitForProducer(std::chrono::milliseconds(TICK_MS)) < 0) { return -1; }
                    continue;
                }

                auto now = Clock::now();
                if (now < nextTick) {
                    if (waitForProducer(std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now)) < 0) { return -1; }
                    continue;
                }

                int count = 0;
                uint64_t endIndex = 0;
                double sampleRateSnapshot = 1.0;
                {
                    std::lock_guard lck(bufferMtx);
                    endIndex = outputIndexLocked();
                    count = (int)popLocked(out.writeBuf, std::min({ bufferedSamples, slice, (size_t)out.capacity() }));
                    endIndex += count;
                    sampleRateSnapshot = sampleRate;
                    if (bufferedSamples < slice) { primed = false; }
                }
                if (count <= 0) {
                    continue;
                }

                nextTick += sliceDuration(count, sampleRateSnapshot);
                if (position) { position->fireBefore(endIndex); }
                if (!out.swap(count)) { return -1; }
            }

            return -1;
        }

        stream<T> out;

    private:
        void doStart() override {
            stopRequested = false;
            base_type::doStart();
        }

        void doStop() override {
            stopRequested = true;
            base_type::doStop();
            std::lock_guard lck(bufferMtx);
            stopRequested = false;
            clearLocked();
        }

        int waitForProducer(std::chrono::milliseconds timeout) {
            int count = input->read_for(timeout);
            if (count < 0) { return -1; }
            if (count > 0) {
                {
                    std::lock_guard lck(bufferMtx);
                    pushLocked(input->readBuf, (size_t)count);
                }
                input->flush();
            }
            return count;
        }

        static std::chrono::steady_clock::duration sliceDuration(size_t samples, double sampleRate) {
            return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(samples / sampleRate));
        }

        // Input index of the oldest buffered sample: the buffer always holds
        // the newest contiguous input, so it is the total minus the backlog.
        uint64_t outputIndexLocked() const {
            return position ? position->samplesIn() - bufferedSamples : 0;
        }

        size_t targetSamplesLocked() const {
            return (size_t)((sampleRate * (double)prebufferMsec) / 1000.0);
        }

        size_t durationSamplesLocked(int msec) const {
            return std::max<size_t>(1, (size_t)((sampleRate * (double)msec) / 1000.0));
        }

        size_t sliceSamplesLocked() const {
            return durationSamplesLocked(TICK_MS);
        }

        size_t maxBufferedSamplesLocked() const {
            size_t target = targetSamplesLocked();
            size_t slice = sliceSamplesLocked();
            if (target == 0) {
                return std::max<size_t>(4096, slice * 4);
            }

            // Keep a bounded amount of backlog over the requested prebuffer.
            // The slack is expressed in time, not raw samples, so high-rate
            // streams do not silently reserve an extra quarter second.
            size_t burstSlack = std::max(durationSamplesLocked(MIN_SLACK_MS),
                std::min(target / 2, durationSamplesLocked(MAX_SLACK_MS)));
            return std::max<size_t>(slice * 8, target + burstSlack);
        }

        void clearLocked() {
            head = 0;
            bufferedSamples = 0;
            primed = false;
        }

        void resizeRingLocked(size_t capacity) {
            capacity = std::max<size_t>(1, capacity);
            if (ring.size() == capacity) { return; }

            std::vector<T> newRing(capacity);
            size_t copied = popLocked(newRing.data(), std::min(bufferedSamples, capacity), false);
            ring.swap(newRing);
            head = 0;
            bufferedSamples = copied;
        }

        void ensureCapacityLocked(size_t minCapacity) {
            if (ring.size() >= minCapacity) { return; }
            resizeRingLocked(minCapacity);
        }

        void dropOldestLocked(size_t count) {
            if (count >= bufferedSamples) {
                clearLocked();
                return;
            }
            head = (head + count) % ring.size();
            bufferedSamples -= count;
        }

        void trimLocked() {
            size_t maxBuffered = maxBufferedSamplesLocked();
            if (bufferedSamples > maxBuffered) {
                dropOldestLocked(bufferedSamples - maxBuffered);
            }
        }

        void pushLocked(const T* data, size_t count) {
            if (count == 0) { return; }
            if (position) { position->addInput(count); }

            size_t maxBuffered = maxBufferedSamplesLocked();
            if (count > maxBuffered) {
                data += count - maxBuffered;
                count = maxBuffered;
                clearLocked();
            }

            ensureCapacityLocked(std::max(maxBuffered, bufferedSamples + count));
            if (bufferedSamples + count > maxBuffered) {
                dropOldestLocked(bufferedSamples + count - maxBuffered);
            }

            size_t tail = (head + bufferedSamples) % ring.size();
            size_t first = std::min(count, ring.size() - tail);
            std::copy_n(data, first, ring.data() + tail);
            if (count > first) {
                std::copy_n(data + first, count - first, ring.data());
            }
            bufferedSamples += count;
        }

        size_t popLocked(T* outBuf, size_t count, bool consume = true) {
            if (count == 0 || bufferedSamples == 0 || ring.empty()) { return 0; }

            size_t actual = std::min(count, bufferedSamples);
            size_t first = std::min(actual, ring.size() - head);
            std::copy_n(ring.data() + head, first, outBuf);
            if (actual > first) {
                std::copy_n(ring.data(), actual - first, outBuf + first);
            }

            if (consume) {
                head = (head + actual) % ring.size();
                bufferedSamples -= actual;
                if (bufferedSamples == 0) { head = 0; }
            }
            return actual;
        }

        static constexpr int TICK_MS = 10;
        static constexpr int MIN_SLACK_MS = 40;
        static constexpr int MAX_SLACK_MS = 100;

        stream<T>* input = nullptr;
        StreamPosition* position = nullptr;
        int prebufferMsec = 100;
        double sampleRate = 1000000.0;
        std::atomic<bool> stopRequested{false};
        bool primed = false;

        std::vector<T> ring;
        size_t head = 0;
        size_t bufferedSamples = 0;
        std::mutex bufferMtx;
    };

    // A Prebuffer with a live bypass, feeding a StreamLink: `in` is routed
    // through the prebuffer when the target is nonzero, or straight into the
    // link when it is zero, so a zero-length prebuffer is a true live path
    // instead of a still-active buffering hop.
    //
    // Not thread-safe on its own: the caller serializes setPrebufferMsec()
    // against its own start()/stop() (see the KiwiSDR source's bufferModeMtx
    // and the SDRIAK server client's pushedStateMtx). If `in` has a manual
    // writer that can sit blocked in swap(), the caller is responsible for
    // releasing it around a live mode switch (see the KiwiSDR source).
    // StreamLink that, while counting, adds what it forwards to a
    // StreamPosition and fires its markers: the bypass path of a
    // PrebufferedLink. Not counting, it forwards Prebuffer output, which
    // has already been counted and marked.
    template <class T>
    class PositionLink : public routing::StreamLink<T> {
        using base_type = routing::StreamLink<T>;
    public:
        void setPosition(StreamPosition* pos) { m_position = pos; }
        void setCounting(bool counting) { m_counting.store(counting, std::memory_order_release); }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            memcpy(base_type::_out->writeBuf, base_type::_in->readBuf, count * sizeof(T));
            base_type::_in->flush();

            if (m_position && m_counting.load(std::memory_order_acquire)) {
                m_position->addInput((size_t)count);
                m_position->fireBefore(m_position->samplesIn());
            }
            if (!base_type::_out->swap(count)) { return -1; }
            return count;
        }

    private:
        StreamPosition* m_position = nullptr;
        std::atomic<bool> m_counting{true};
    };

    template <class T>
    class PrebufferedLink {
    public:
        void init(stream<T>* in, stream<T>* out) {
            input = in;
            prebuffer.init(in);
            prebuffer.setPosition(&position);
            link.init(in, out);
            link.setPosition(&position);
            link.setCounting(!prebufferActive);
        }

        // Total input samples accepted so far, in either mode.
        uint64_t samplesIn() const { return position.samplesIn(); }

        // Any thread. Fire cb on the link's output thread right before the
        // output block that contains input sample `inputIndex` is
        // published. Works in prebuffer and bypass mode; a marker whose
        // sample the prebuffer dropped fires with the next published block.
        void scheduleMarker(uint64_t inputIndex, std::function<void()> cb) {
            position.schedule(inputIndex, std::move(cb));
        }

        // Expected steady_clock time at which input sample `inputIndex`
        // leaves the link (in the past when it already left).
        std::chrono::steady_clock::time_point playoutTime(uint64_t inputIndex) {
            double seconds;
            if (prebufferActive) {
                seconds = prebuffer.secondsUntilOutput(inputIndex);
            }
            else {
                seconds = ((double)inputIndex - (double)position.samplesIn()) / prebuffer.getSampleRate();
            }
            return std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        }

        // Samples held in the prebuffer; 0 in bypass mode.
        size_t bufferedSamples() { return prebufferActive ? prebuffer.getBufferedSamples() : 0; }

        void setSampleRate(double sr) { prebuffer.setSampleRate(sr); }

        bool prebuffering() const { return prebufferActive; }

        // Percent of the prebuffer target currently held, or -1 in bypass
        // mode (there is no buffer to fill).
        int getPercentFull() { return prebufferActive ? prebuffer.getPercentFull() : -1; }

        // Route through the prebuffer (msec > 0) or bypass it (msec == 0).
        // `chainLive` says whether the surrounding DSP chain is running, i.e.
        // whether the prebuffer worker must be started/stopped now or merely
        // re-plumbed for the next start().
        void setPrebufferMsec(int msec, bool resetBuffer, bool chainLive) {
            prebuffer.setPrebufferMsec(msec);
            if (msec > 0) {
                if (!prebufferActive) {
                    // Stop counting first: the prebuffer counts from here.
                    link.setCounting(false);
                    link.setInput(&prebuffer.out);
                    prebufferActive = true;
                }
                if (resetBuffer) { prebuffer.clear(); }
                if (chainLive) { prebuffer.start(); }
                return;
            }
            if (prebufferActive) {
                prebuffer.stop();
                // The prebuffer is stopped, so nothing reaches the link
                // until it reads `input` again.
                link.setCounting(true);
                link.setInput(input);
                prebufferActive = false;
            }
        }

        // Start/stop the whole segment (the prebuffer too when routed
        // through it). Both are idempotent, like the blocks they wrap.
        void start() {
            if (prebufferActive) { prebuffer.start(); }
            link.start();
        }

        void stop() {
            link.stop();
            prebuffer.stop();
        }

    private:
        stream<T>* input = nullptr;
        StreamPosition position;
        Prebuffer<T> prebuffer;
        PositionLink<T> link;
        // Read by playoutTime() and bufferedSamples() on the producer's
        // thread while the owner switches modes.
        std::atomic<bool> prebufferActive{false};
    };
}
