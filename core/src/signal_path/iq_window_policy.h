#pragma once
#include <algorithm>
#include <utility>
#include <vector>

// Placement of a networked source's IQ window over the VFOs. Pure, so a
// source can call it every UI tick and tests can check it directly. All
// values are offsets from the display centre C, in Hz.
struct IQWindowInput {
    // VFO passbands [lo, hi], the selected VFO first.
    std::vector<std::pair<double, double>> passbands;
    // Window offset now in use, usable window width, and the guard kept
    // between a passband and the window edge when the window moves
    // (about 10 % of the width).
    double currentOffset = 0.0;
    double width = 0.0;
    double margin = 0.0;
    // Server slide limits around C.
    double minOffset = 0.0;
    double maxOffset = 0.0;
};

// Hysteresis: keep currentOffset while every passband fits in the window.
// Otherwise centre on the union of the passbands if it fits inside the
// window less the margins, else on the selected VFO. The result is clamped
// to the slide limits.
inline double chooseIQWindow(const IQWindowInput& in) {
    auto clampOffset = [&in](double offset) {
        return std::clamp(offset, (std::min)(in.minOffset, in.maxOffset), (std::max)(in.minOffset, in.maxOffset));
    };
    if (in.passbands.empty()) { return clampOffset(in.currentOffset); }

    const double half = in.width / 2.0;
    bool allFit = true;
    double unionLo = in.passbands.front().first;
    double unionHi = in.passbands.front().second;
    for (const auto& [lo, hi] : in.passbands) {
        if (lo < in.currentOffset - half || hi > in.currentOffset + half) { allFit = false; }
        unionLo = (std::min)(unionLo, lo);
        unionHi = (std::max)(unionHi, hi);
    }
    if (allFit) { return clampOffset(in.currentOffset); }

    if (unionHi - unionLo <= in.width - (2.0 * in.margin)) {
        return clampOffset((unionLo + unionHi) / 2.0);
    }
    const auto& selected = in.passbands.front();
    return clampOffset((selected.first + selected.second) / 2.0);
}
