#pragma once
#include <assert.h>
#include <string.h>
#include <volk/volk.h>
#include "../taps/tap.h"
#include "../buffer/buffer.h"

namespace dsp::multirate {
    // Owning, move-only polyphase bank. All phases live in one aligned
    // allocation, row after row: phase i starts at m_data + i * m_stride.
    //
    // m_stride is tapsPerPhase rounded up to the volk alignment, so every row
    // starts on an aligned boundary.
    template<class T>
    class PolyphaseBank {
    public:
        PolyphaseBank() = default;

        // Splits taps into phaseCount phases, the layout the polyphase
        // resamplers and interpolators index: tap i goes to phase
        // (phaseCount - 1) - (i % phaseCount), position i / phaseCount.
        PolyphaseBank(int phaseCount, const tap<T>& taps) {
            assert(phaseCount > 0);
            const int tapsPerPhase = (taps.size() + phaseCount - 1) / phaseCount;
            const int align = (int)(volk_get_alignment() / sizeof(T));
            const int stride = (align > 1) ? ((tapsPerPhase + align - 1) / align) * align : tapsPerPhase;

            // One allocation: it either succeeds whole or throws with nothing
            // to clean up.
            T* data = buffer::alloc<T>(phaseCount * stride);
            buffer::clear<T>(data, phaseCount * stride);
            for (unsigned int i = 0; i < taps.size(); i++)
                data[((phaseCount - 1) - (i % phaseCount)) * stride + (i / phaseCount)] = taps[i];

            m_data = data;
            m_phaseCount = phaseCount;
            m_tapsPerPhase = tapsPerPhase;
            m_stride = stride;
        }

        ~PolyphaseBank() { release(); }

        PolyphaseBank(const PolyphaseBank&) = delete;
        PolyphaseBank& operator=(const PolyphaseBank&) = delete;

        PolyphaseBank(PolyphaseBank&& other) noexcept :
            m_data(other.m_data), m_phaseCount(other.m_phaseCount),
            m_tapsPerPhase(other.m_tapsPerPhase), m_stride(other.m_stride) {
            other.reset();
        }

        PolyphaseBank& operator=(PolyphaseBank&& other) noexcept {
            if (this != &other) {
                release();
                m_data = other.m_data;
                m_phaseCount = other.m_phaseCount;
                m_tapsPerPhase = other.m_tapsPerPhase;
                m_stride = other.m_stride;
                other.reset();
            }
            return *this;
        }

        const T* phase(int i) const {
            assert(i >= 0 && i < m_phaseCount);
            return m_data + (size_t)i * m_stride;
        }

        int phaseCount() const { return m_phaseCount; }
        int tapsPerPhase() const { return m_tapsPerPhase; }

        void release() {
            if (m_data) { buffer::free(m_data); }
            reset();
        }

    private:
        void reset() {
            m_data = nullptr;
            m_phaseCount = 0;
            m_tapsPerPhase = 0;
            m_stride = 0;
        }

        T* m_data = nullptr;
        int m_phaseCount = 0;
        int m_tapsPerPhase = 0;
        int m_stride = 0;
    };

    template<class T>
    inline PolyphaseBank<T> buildPolyphaseBank(int phaseCount, const tap<T>& taps) {
        return PolyphaseBank<T>(phaseCount, taps);
    }
}
