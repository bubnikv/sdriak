#pragma once
#include <assert.h>
#include <string.h>
#include <algorithm>
#include <utility>
#include "buffer.h"

namespace dsp::buffer {
    // Sliding window for blocks that need the last few input samples of the
    // previous block: FIR delay lines, polyphase resamplers, delays, sync
    // correlators. Layout:
    //
    //     [ history: history() samples | block: up to maxBlock() samples ]
    //
    // Per block: push() copies the input in behind the history and returns the
    // window start, the caller reads window[0 .. history + count), and
    // advance() keeps the last history() samples for the next block. The input
    // is copied rather than read in place so callers may process in place
    // (in == out). Both are inlined memcpy/memmove; the memmove is O(history)
    // against the O(count * history) work a filter does, so a ring buffer
    // would not pay.
    //
    // Every operation that allocates gives the strong guarantee: a throw
    // leaves the window exactly as it was.
    template<class T>
    class HistoryBuffer {
    public:
        HistoryBuffer() = default;
        HistoryBuffer(int maxBlock, int history) { init(maxBlock, history); }

        HistoryBuffer(const HistoryBuffer&) = delete;
        HistoryBuffer& operator=(const HistoryBuffer&) = delete;
        HistoryBuffer(HistoryBuffer&&) noexcept = default;
        HistoryBuffer& operator=(HistoryBuffer&&) noexcept = default;

        // [RATE-SIZED] maxBlock is the largest count push() will ever be
        // given; callers derive it from their rate arithmetic. An overrun is
        // heap corruption, caught by the assert in push() in debug builds.
        // see doc/design/buffer-size-requirements.md 2.6
        // Allocates exactly maxBlock + history and zeroes the history.
        void init(int maxBlock, int history) {
            assert(maxBlock >= 0 && history >= 0);
            m_buf.allocate(capacityFor(maxBlock, history));
            m_maxBlock = maxBlock;
            m_history = history;
            m_room = history;
            clear();
        }

        // New bound and history length; the history is discarded and zeroed.
        // Reallocates only when the bound changes or the history no longer
        // fits, so re-applying the current configuration is cheap.
        void resize(int maxBlock, int history) {
            assert(maxBlock >= 0 && history >= 0);
            if (maxBlock != m_maxBlock || history > m_room || !m_buf.get()) {
                init(maxBlock, history);
                return;
            }
            m_history = history;
            clear();
        }

        // Change only the largest block push() may receive, preserving every
        // retained history sample. The history lives before the current block,
        // so changing the amount of room after it does not change its sample
        // domain or invalidate it. Acquire first and commit second: allocation
        // failure leaves both the old capacity and history intact.
        void setMaxBlock(int maxBlock) {
            assert(maxBlock >= 0);
            if (maxBlock == m_maxBlock) { return; }
            if (!m_buf.get()) {
                init(maxBlock, m_history);
                return;
            }

            Buffer<T> fresh(capacityFor(maxBlock, m_room));
            if (m_history > 0) {
                memcpy(fresh.get(), m_buf.get(), (size_t)m_history * sizeof(T));
            }
            m_buf = std::move(fresh);
            m_maxBlock = maxBlock;
        }

        // New history length, keeping the newest samples: they stay right
        // before the block, and the slots entering on the left are zero (or
        // the oldest ones drop off when it shrinks), so a filter changing its
        // tap count carries on without a discontinuity. Grows the allocation
        // geometrically when the new length does not fit, so a UI control
        // dragging it up reallocates only a handful of times.
        void setHistory(int history) {
            assert(history >= 0);
            const int old = m_history;
            if (history > m_room) {
                const int room = (std::max)(history, 2 * m_room);
                Buffer<T> fresh(capacityFor(m_maxBlock, room));
                memset(fresh.get(), 0, (size_t)(history - old) * sizeof(T));
                memcpy(fresh.get() + (history - old), m_buf.get(), (size_t)old * sizeof(T));
                m_buf = std::move(fresh);
                m_room = room;
            }
            else if (history < old) {
                memmove(m_buf.get(), m_buf.get() + (old - history), (size_t)history * sizeof(T));
            }
            else if (history > old) {
                memmove(m_buf.get() + (history - old), m_buf.get(), (size_t)old * sizeof(T));
                memset(m_buf.get(), 0, (size_t)(history - old) * sizeof(T));
            }
            m_history = history;
        }

        // Zero the history.
        void clear() {
            if (m_history > 0) { memset(m_buf.get(), 0, (size_t)m_history * sizeof(T)); }
        }

        void release() {
            m_buf.release();
            m_maxBlock = 0;
            m_history = 0;
            m_room = 0;
        }

        // Copies count samples in behind the history and returns the window:
        // history() past samples followed by the count new ones.
        inline T* push(const T* in, int count) {
            assert(count >= 0 && count <= m_maxBlock);
            memcpy(m_buf.get() + m_history, in, (size_t)count * sizeof(T));
            return m_buf.get();
        }

        // After processing a block of count samples: the last history()
        // samples of the window become the next block's history.
        inline void advance(int count) {
            assert(count >= 0 && count <= m_maxBlock);
            memmove(m_buf.get(), m_buf.get() + count, (size_t)m_history * sizeof(T));
        }

        T* data() { return m_buf.get(); }
        const T* data() const { return m_buf.get(); }
        int history() const { return m_history; }
        int maxBlock() const { return m_maxBlock; }

    private:
        // volk_malloc(0) may legitimately return NULL, which alloc() reports
        // as out of memory, so never ask for nothing.
        static int capacityFor(int maxBlock, int history) {
            return (std::max)(1, maxBlock + history);
        }

        Buffer<T> m_buf;
        int m_maxBlock = 0;
        int m_history = 0;
        // History length the allocation has room for: it holds
        // m_maxBlock + m_room samples, and m_history <= m_room.
        int m_room = 0;
    };
}
