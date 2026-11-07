#pragma once
#include <volk/volk.h>
#include "../buffer/buffer.h"

namespace dsp {
    template<class T>
    class tap {
    public:
        T* taps = NULL;
        unsigned int size = 0;
    };

    namespace taps {
        template<class T>
        inline tap<T> alloc(int count) {
            tap<T> taps;
            taps.size = count;
            taps.taps = buffer::alloc<T>(count);
            return taps;
        }

        template<class T>
        inline void free(tap<T>& taps) {
            if (!taps.taps) { return; }
            buffer::free(taps.taps);
            taps.taps = NULL;
            taps.size = 0;
        }
    }

    // RAII owner for a tap set. Derives from tap<T> so `.taps` and `.size` and
    // passing it to anything taking a tap<T>& all keep working unchanged, and
    // assigning a freshly generated tap set frees the previous one.
    //
    // Note the ownership split this makes explicit: blocks that *generate* taps
    // own them (use this); blocks that merely *hold* a copy handed to init(),
    // such as FIR::_taps, must not free and keep a plain tap<T>.
    template<class T>
    class OwnedTap : public tap<T> {
    public:
        OwnedTap() {}

        OwnedTap(const tap<T>& t) {
            this->taps = t.taps;
            this->size = t.size;
        }

        ~OwnedTap() { dsp::taps::free(*this); }

        OwnedTap(const OwnedTap&) = delete;
        OwnedTap& operator=(const OwnedTap&) = delete;

        OwnedTap(OwnedTap&& other) noexcept {
            this->taps = other.taps;
            this->size = other.size;
            other.taps = NULL;
            other.size = 0;
        }

        OwnedTap& operator=(OwnedTap&& other) noexcept {
            if (this != &other) {
                dsp::taps::free(*this);
                this->taps = other.taps;
                this->size = other.size;
                other.taps = NULL;
                other.size = 0;
            }
            return *this;
        }

        // Adopt a newly generated tap set, releasing whatever we held.
        OwnedTap& operator=(const tap<T>& t) {
            if (this->taps != t.taps) {
                dsp::taps::free(*this);
                this->taps = t.taps;
                this->size = t.size;
            }
            return *this;
        }
    };
}