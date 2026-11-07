// Signal level meter adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include <gui/widgets/snr_meter.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <gui/style.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace {
    // The scale spans -90..0 dBFS: 10 tick labels over 9 intervals.
    constexpr float METER_RANGE = 90.0f;

    // Key the cache on the font pointer AND the scale epoch: a font atlas
    // rebuild can reuse the same pointer address, and style::dp() feeds the
    // cached widths even when the font pointer is unchanged.
    static ImFont*  cachedFont = nullptr;
    static uint64_t cachedEpoch = UINT64_MAX;
    static float    cachedLabelWidth = 0.0f;
    static float    cachedMinWidth = 0.0f;
    static float    cachedTextColWidth = 0.0f;

    void updateWidthCache() {
        ImFont* currentFont = ImGui::GetFont();
        uint64_t currentEpoch = style::scaleEpoch();
        if (currentFont == cachedFont && currentEpoch == cachedEpoch) { return; }
        cachedFont = currentFont;
        cachedEpoch = currentEpoch;
        float maxLabelWidth = 0.0f;
        char buf[32];
        for (int i = 0; i < 10; i++) {
            sprintf(buf, "%d", (i - 9) * 10);
            maxLabelWidth = std::max(maxLabelWidth, ImGui::CalcTextSize(buf).x);
        }
        cachedLabelWidth = maxLabelWidth;
        // Numeric readout column on the right; widest realistic value.
        cachedTextColWidth = ImGui::CalcTextSize("-150.0 dB").x + style::dp(4.0f);
        // Hard minimum width: the bar with sparse (every-other) tick labels, so
        // each labeled interval spans two ticks and stays at least as wide as
        // the widest label. The numeric readout is optional and not required.
        cachedMinWidth = maxLabelWidth * 4.5f + style::dp(8.0f);
    }
};

namespace ImGui {
    float GetLevelMeterMinWidth() {
        updateWidthCache();
        return cachedMinWidth;
    }

    float GetLevelMeterHeight() {
        // Labels sit at scale(16) and are one line tall; the two-line numeric
        // readout on the right ends at the same place, since the base font is
        // 16 dp. Was a literal 26 px, which stopped scaling past 1x -- the
        // widget then painted ~4x its own item box on a phone and only the top
        // 26 px of it answered clicks.
        return (float)style::scale(16.0f) + GetTextLineHeight();
    }

    void LevelMeter(float level, float levelMax, float snr, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        // The meter is placed via a float-computed SetCursorPosX, so snap the
        // origin — all bar/tick geometry below assumes whole-pixel offsets.
        ImVec2 min = ImVec2(floorf(window->DC.CursorPos.x), floorf(window->DC.CursorPos.y));
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), GetLevelMeterHeight());
        ImRect bb(min, min + size);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

        ImGuiID id = window->GetID("##level_meter");
        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, id)) {
            return;
        }

        // Click the scale to toggle between the dBFS scale (0 dB at the right
        // edge, negative to the left) and the positive scale (0 at the left,
        // increasing to the right — the original SNR-style reading). Starts in
        // dBFS mode on every launch.
        static bool dbfsMode = true;
        if (ButtonBehavior(bb, id, nullptr, nullptr)) {
            dbfsMode = !dbfsMode;
        }

        updateWidthCache();
        // The numeric readout is optional: only reserve its column when the bar
        // clears its sparse-label minimum plus some slack (so the readout hides
        // a bit sooner, before the bar gets cramped) with the column removed.
        float minBarWidth = cachedLabelWidth * 5.5f;
        bool showReadout = (size.x - cachedTextColWidth) >= minBarWidth;
        float barWidth = std::max(showReadout ? size.x - cachedTextColWidth : size.x, 1.0f);
        // Mandatory gap between adjacent labels.
        float labelGap = style::dp(3.0f);
        // Label every tick when there is room for all ten with that gap;
        // otherwise label every other tick (all tick marks are still drawn).
        bool fullLabels = barWidth >= (cachedLabelWidth + labelGap) * 9.0f;
        float ratio = barWidth / METER_RANGE;
        float it = barWidth / 9;
        char buf[32];

        // Whole pixels: the bar fill bottom, the axis baseline at barHeight-1
        // and the tick ends must land on the same pixel row at fractional
        // scales (dp(10) would be 12.5 at 1.25x).
        float barHeight = (float)style::scale(10.0f);

        // Level bar and peak hold marker
        if (std::isfinite(level)) {
            float gval = std::clamp<float>(level, -METER_RANGE, 0) + METER_RANGE;
            window->DrawList->AddRectFilled(min + ImVec2(0, 1), min + ImVec2(roundf(gval * ratio), barHeight), IM_COL32(0, 192, 0, 255));
        }
        if (std::isfinite(levelMax)) {
            float fval = std::clamp<float>(levelMax, -METER_RANGE, 0) + METER_RANGE;
            float px = roundf(fval * ratio);
            window->DrawList->AddRectFilled(min + ImVec2(px, 1), min + ImVec2(px + std::max(2.0f, style::dp(2.0f)), barHeight), IM_COL32(255, 255, 0, 255));
        }

        window->DrawList->AddLine(min, min + ImVec2(0, barHeight - 1), text, style::lineWidth());
        window->DrawList->AddLine(min + ImVec2(0, barHeight - 1), min + ImVec2(barWidth + 1, barHeight - 1), text, style::lineWidth());

        // Which ticks get a numeric label: all of them at full density, else
        // every other one — odd ticks in dBFS mode (…-20, 0), even ticks in
        // positive mode (0, 20, 40, 60, 80). The leftmost (0) is dropped below
        // if it does not fit; see skipFirstLabel.
        auto tickLabeled = [&](int i) {
            if (fullLabels) { return true; }
            return dbfsMode ? (i % 2) == 1 : (i % 2) == 0;
        };
        auto tickLabelValue = [&](int i) { return dbfsMode ? (i - 9) * 10 : i * 10; };

        // The leftmost label is left-aligned to its tick (it cannot center
        // without spilling off the left edge). Drop it if that pushes it into
        // the next label.
        int firstLabelIdx = -1, secondLabelIdx = -1;
        for (int i = 0; i < 10; i++) {
            if (!tickLabeled(i)) { continue; }
            if (firstLabelIdx < 0) { firstLabelIdx = i; }
            else { secondLabelIdx = i; break; }
        }
        bool skipFirstLabel = false;
        {
            char b0[32], b1[32];
            sprintf(b0, "%d", tickLabelValue(firstLabelIdx));
            sprintf(b1, "%d", tickLabelValue(secondLabelIdx));
            float w0 = ImGui::CalcTextSize(b0).x;
            float w1 = ImGui::CalcTextSize(b1).x;
            float x0 = std::max(roundf((float)firstLabelIdx * it - w0 / 2.0f) + 1, 0.0f);
            float x1 = roundf((float)secondLabelIdx * it - w1 / 2.0f) + 1;
            skipFirstLabel = (x0 + w0 + labelGap) > x1;
        }

        for (int i = 0; i < 10; i++) {
            // Sparse mode draws the unlabeled 10 dB ticks in between shorter.
            bool labeled = tickLabeled(i);
            float tickBottom = (float)style::scale(labeled ? 15.0f : 12.5f);
            window->DrawList->AddLine(min + ImVec2(roundf((float)i * it), barHeight - 1), min + ImVec2(roundf((float)i * it), tickBottom - 1), text, style::lineWidth());
            if (!labeled) { continue; }
            if (i == firstLabelIdx && skipFirstLabel) { continue; }
            sprintf(buf, "%d", tickLabelValue(i));
            ImVec2 sz = ImGui::CalcTextSize(buf);
            // Center the label on the tick, but keep the first one inside the widget
            float labelX = std::max(roundf(((float)i * it) - (sz.x / 2.0f)) + 1, 0.0f);
            window->DrawList->AddText(min + ImVec2(labelX, (float)style::scale(16.0f)), text, buf);
        }

        // Numeric readout: peak level on top, SNR below. Hidden when the widget
        // is too narrow to reserve the column without starving the bar.
        if (showReadout) {
            if (std::isfinite(levelMax)) {
                sprintf(buf, dbfsMode ? "%+.1f dB" : "%.1f dB", dbfsMode ? levelMax : levelMax + METER_RANGE);
            }
            else {
                strcpy(buf, "--.- dB");
            }
            ImVec2 sz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(min + ImVec2(size.x - sz.x, 0), text, buf);

            if (std::isfinite(snr)) {
                sprintf(buf, "%.1f dB", snr);
            }
            else {
                strcpy(buf, "--.- dB");
            }
            sz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(min + ImVec2(size.x - sz.x, sz.y), text, buf);
        }
    }
}
