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
        void clearMarkers() {
            std::lock_guard lck(m_mtx);
            m_markers.clear();
            m_pending.store(0, std::memory_order_release);
        }

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
        explicit Prebuffer(int ownCapacity) : out(ownCapacity) {}
        Prebuffer(stream<T>* in) { init(in); }

        void init(stream<T>* in, stream<T>* destination = nullptr) {
            input = in;
            output = destination ? destination : &out;
            registerInput(input);
            registerOutput(output);
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
                        count = (int)popLocked(output->writeBuf, std::min(bufferedSamples, (size_t)output->capacity()));
                        endIndex += count;
                    }
                    if (count > 0) {
                        if (position) { position->fireBefore(endIndex); }
                        if (!output->swap(count)) { return -1; }
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
                    if (waitForProducer(std::chrono::ceil<std::chrono::milliseconds>(nextTick - now)) < 0) { return -1; }
                    continue;
                }

                int count = 0;
                uint64_t endIndex = 0;
                double sampleRateSnapshot = 1.0;
                {
                    std::lock_guard lck(bufferMtx);
                    endIndex = outputIndexLocked();
                    count = (int)popLocked(output->writeBuf, std::min({ bufferedSamples, slice, (size_t)output->capacity() }));
                    endIndex += count;
                    sampleRateSnapshot = sampleRate;
                    if (bufferedSamples < slice) { primed = false; }
                }
                if (count <= 0) {
                    continue;
                }

                nextTick += sliceDuration(count, sampleRateSnapshot);
                if (position) { position->fireBefore(endIndex); }
                if (!output->swap(count)) { return -1; }
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
        stream<T>* output = nullptr;
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
    // The bypass path counts and marks the samples it forwards.
    template <class T>
    class PositionLink : public routing::StreamLink<T> {
        using base_type = routing::StreamLink<T>;
    public:
        void setPosition(StreamPosition* pos) { m_position = pos; }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            for (int offset = 0; offset < count;) {
                const int n = std::min(count - offset, base_type::_out->capacity());
                memcpy(base_type::_out->writeBuf, base_type::_in->readBuf + offset, n * sizeof(T));
                m_position->addInput((size_t)n);
                m_position->fireBefore(m_position->samplesIn());
                if (!base_type::_out->swap(n)) { base_type::_in->flush(); return -1; }
                offset += n;
            }
            base_type::_in->flush();
            return count;
        }

    private:
        StreamPosition* m_position = nullptr;
    };

    template <class T>
    class PrebufferedLink {
    public:
        void init(stream<T>* in, stream<T>* out) {
            input = in;
            prebuffer.init(in, out);
            prebuffer.setPosition(&position);
            link.init(in, out);
            link.setPosition(&position);
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
                    link.stop();
                    prebufferActive = true;
                }
                if (resetBuffer) { prebuffer.clear(); }
                if (chainLive) { prebuffer.start(); }
                return;
            }
            if (prebufferActive) {
                prebuffer.stop();
                prebufferActive = false;
                if (chainLive) { link.start(); }
            }
        }

        // Start/stop the whole segment (the prebuffer too when routed
        // through it). Both are idempotent, like the blocks they wrap.
        void start() {
            if (prebufferActive) { prebuffer.start(); }
            else { link.start(); }
        }

        void stop() {
            link.stop();
            prebuffer.stop();
        }

        // Stopped, with the writer idle: discard a block the writer handed
        // over that the link never read. Such a block survives stop() and
        // would be read and counted after the next start(), shifting the
        // input index of everything a new session pushes by its size. Call
        // before taking samplesIn() as a session's base.
        void discardUnreadInput() {
            input->flush();
            position.clearMarkers();
        }

    private:
        stream<T>* input = nullptr;
        StreamPosition position;
        Prebuffer<T> prebuffer{1};
        PositionLink<T> link;
        // Read by playoutTime() and bufferedSamples() on the producer's
        // thread while the owner switches modes.
        std::atomic<bool> prebufferActive{false};
    };
}
