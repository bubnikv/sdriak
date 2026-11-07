#pragma once
#include <vector>
#include <string.h>
#include "../taps/tap.h"
#include "../buffer/buffer.h"

namespace dsp::multirate {
    template<class T>
    struct PolyphaseBank {
        int phaseCount = 0;
        int tapsPerPhase = 0;
        T** phases = NULL;
    };

    template<class T>
    inline void freePolyphaseBank(PolyphaseBank<T>& bank);

    template<class T>
    inline PolyphaseBank<T> buildPolyphaseBank(int phaseCount, tap<T>& taps) {
        // Allocate bank
        PolyphaseBank<T> pb;
        pb.phaseCount = phaseCount;
        pb.tapsPerPhase = (taps.size + phaseCount - 1) / phaseCount;
        pb.phases = buffer::alloc<T*>(phaseCount);

        // alloc() throws on OOM, so the table has to be null-filled before the
        // first phase allocation: otherwise a throw part-way through the loop
        // leaks the table plus every phase already built, and the cleanup below
        // would walk uninitialised pointers.
        memset(pb.phases, 0, (size_t)phaseCount * sizeof(T*));

        // Allocate phases
        try {
            for (int i = 0; i < phaseCount; i++) {
                pb.phases[i] = buffer::alloc<T>(pb.tapsPerPhase);
                buffer::clear<T>(pb.phases[i], pb.tapsPerPhase);
            }
        }
        catch (...) {
            freePolyphaseBank(pb);
            throw;
        }

        // Fill phases
        int totTapCount = phaseCount * pb.tapsPerPhase;
        for (int i = 0; i < totTapCount; i++) {
            pb.phases[(phaseCount - 1) - (i % phaseCount)][i / phaseCount] = (i < taps.size) ? taps.taps[i] : 0;
        }

        // int currentTap = 0;
        // for (int tap = 0; tap < pb.tapsPerPhase; tap++) {
        //     for (int phase = 0; phase < phaseCount; phase++) {
        //         if (currentTap < taps.size) {
        //             pb.phases[(phaseCount - 1) - phase][tap] = taps.taps[currentTap++];
        //         }
        //         else {
        //             pb.phases[(phaseCount - 1) - phase][tap] = 0;
        //         }
        //     }
        // }

        return pb;
    }

    template<class T>
    inline void freePolyphaseBank(PolyphaseBank<T>& bank) {
        if (!bank.phases) { return; }
        for (int i = 0; i < bank.phaseCount; i++) {
            if (!bank.phases[i]) { continue; }
            buffer::free(bank.phases[i]);
        }
        buffer::free(bank.phases);
        bank.phases = NULL;
        bank.phaseCount = 0;
        bank.tapsPerPhase = 0;
    }

    // RAII owner for a polyphase bank. Derives from PolyphaseBank<T> so every
    // field access keeps working, and assigning a newly built bank frees the
    // previous one -- which is what MM and PolyphaseResampler were doing by
    // hand, in an order that leaked the bank if the following allocation threw.
    template<class T>
    class OwnedPolyphaseBank : public PolyphaseBank<T> {
    public:
        OwnedPolyphaseBank() {}

        OwnedPolyphaseBank(const PolyphaseBank<T>& b) {
            this->phaseCount = b.phaseCount;
            this->tapsPerPhase = b.tapsPerPhase;
            this->phases = b.phases;
        }

        ~OwnedPolyphaseBank() { freePolyphaseBank(*this); }

        OwnedPolyphaseBank(const OwnedPolyphaseBank&) = delete;
        OwnedPolyphaseBank& operator=(const OwnedPolyphaseBank&) = delete;

        // Must be explicit: declaring the copy operations suppresses the
        // implicit move ones, which would leave std::move() binding to the
        // base-adopting operator= below -- a shallow copy, then a double free.
        OwnedPolyphaseBank(OwnedPolyphaseBank&& other) noexcept {
            this->phaseCount = other.phaseCount;
            this->tapsPerPhase = other.tapsPerPhase;
            this->phases = other.phases;
            other.phaseCount = 0;
            other.tapsPerPhase = 0;
            other.phases = NULL;
        }

        OwnedPolyphaseBank& operator=(OwnedPolyphaseBank&& other) noexcept {
            if (this != &other) {
                freePolyphaseBank(*this);
                this->phaseCount = other.phaseCount;
                this->tapsPerPhase = other.tapsPerPhase;
                this->phases = other.phases;
                other.phaseCount = 0;
                other.tapsPerPhase = 0;
                other.phases = NULL;
            }
            return *this;
        }

        OwnedPolyphaseBank& operator=(const PolyphaseBank<T>& b) {
            if (this->phases != b.phases) {
                freePolyphaseBank(*this);
                this->phaseCount = b.phaseCount;
                this->tapsPerPhase = b.tapsPerPhase;
                this->phases = b.phases;
            }
            return *this;
        }
    };
}
