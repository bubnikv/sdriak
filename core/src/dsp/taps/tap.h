#pragma once
#include <assert.h>
#include <type_traits>
#include <volk/volk.h>
#include "../buffer/buffer.h"

namespace dsp {
    // Owning, move-only tap set. The generators in dsp::taps return one, and
    // assigning a fresh set frees the previous one.
    //
    // The FIR family adopts a set handed over as an rvalue and borrows one
    // handed over as an lvalue; internally it reads the set through a
    // TapView, which survives the owning tap<T> being moved.
    template<class T>
    class tap {
    public:
        tap() {}
        // Allocates count uninitialised taps; see taps::alloc().
        explicit tap(unsigned int count) : m_taps(buffer::alloc<T>(count)), m_size(count) {}
        ~tap() { release(); }

        tap(const tap&) = delete;
        tap& operator=(const tap&) = delete;

        tap(tap&& other) noexcept : m_taps(other.m_taps), m_size(other.m_size) {
            other.m_taps = nullptr;
            other.m_size = 0;
        }

        tap& operator=(tap&& other) noexcept {
            if (this != &other) {
                release();
                m_taps = other.m_taps;
                m_size = other.m_size;
                other.m_taps = nullptr;
                other.m_size = 0;
            }
            return *this;
        }

        T* taps() { return m_taps; }
        const T* taps() const { return m_taps; }
        unsigned int size() const { return m_size; }

        T& operator[](unsigned int i) { assert(i < m_size); return m_taps[i]; }
        const T& operator[](unsigned int i) const { assert(i < m_size); return m_taps[i]; }

        // Sum of the taps, i.e. the filter's gain at DC.
        T sum() const {
            T s{};
            for (unsigned int i = 0; i < m_size; i++) { s += m_taps[i]; }
            return s;
        }

        // Multiply every tap by k.
        void scale(float k) {
            for (unsigned int i = 0; i < m_size; i++) { m_taps[i] *= k; }
        }

        // Scale to unity DC gain: real taps then sum to 1, complex taps to a
        // sum of magnitude 1. Leaves the set unchanged when the sum is zero,
        // as it is for a band-pass or high-pass.
        void normalize() {
            float gain;
            if constexpr (std::is_same_v<T, float>) { gain = sum(); }
            else { gain = sum().amplitude(); }
            if (gain == 0.0f) { return; }
            scale(1.0f / gain);
        }

        void release() {
            if (!m_taps) { return; }
            buffer::free(m_taps);
            m_taps = nullptr;
            m_size = 0;
        }

    private:
        T* m_taps = nullptr;
        unsigned int m_size = 0;
    };

    // Non-owning view of a tap set someone else keeps alive.
    template<class T>
    class TapView {
    public:
        TapView() {}
        TapView(const tap<T>& t) : m_taps(t.taps()), m_size(t.size()) {}

        const T* taps() const { return m_taps; }
        unsigned int size() const { return m_size; }

        const T& operator[](unsigned int i) const { assert(i < m_size); return m_taps[i]; }

    private:
        const T* m_taps = nullptr;
        unsigned int m_size = 0;
    };

    namespace taps {
        template<class T>
        inline tap<T> alloc(int count) {
            return tap<T>(count);
        }
    }
}
