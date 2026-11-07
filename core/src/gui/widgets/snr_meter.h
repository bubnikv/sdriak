#pragma once
// Signal level meter adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include <imgui/imgui.h>

namespace ImGui {
    float GetLevelMeterMinWidth();

    // Drawn height of the meter: the tick/label band plus one text line. The
    // widget reserves exactly this, so its item box matches what it paints --
    // callers need it to centre the meter in the top-bar row, and the
    // click-to-toggle-scale hit box depends on it too.
    float GetLevelMeterHeight();

    // Signal meter: level bar (dBFS) with peak hold marker, plus a numeric
    // peak-level / SNR readout on the right. Pass non-finite level/levelMax
    // (e.g. -INFINITY) to draw an empty meter, and NAN snr to blank the SNR
    // readout (no VFO selected).
    void LevelMeter(float level, float levelMax, float snr, const ImVec2& size_arg = ImVec2(0, 0));
}
