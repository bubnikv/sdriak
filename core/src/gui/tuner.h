#pragma once
#include <string>
#include <module.h>

namespace tuner {
    enum {
        TUNER_MODE_CENTER,
        TUNER_MODE_NORMAL,
        TUNER_MODE_LOWER_HALF,
        TUNER_MODE_UPPER_HALF,
        TUNER_MODE_IQ_ONLY,
        _TUNER_MODE_COUNT
    };

    void tune(int mode, std::string vfoName, double freq);

    // For synchronization with hardware radio, where the VFO frequency is locked at a fixed offset from IF center frequency.
    // For example, the QMX radio demodulates at +12kHz from the IQ center frequency.
    // Returns true if changed.
    bool lockVFOtoCenter(double offsetHz);
    // Stop synchroning VFO to IQ center frequency.
    void unlockVFO();
    // Returns true if the VFO is currently locked to the IQ center frequency.
    bool vfoLockedToCenter();
}
