/*
 * This file is based on code from project SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown)
 * originally licensed under the GNU General Public License, version 3 (GPLv3).
 *
 * Modified and extended by qrp73, 2025 (https://github.com/qrp73/SDRPP).
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <imgui.h>
#include <gui/style.h>
#include <string>

inline void doRightText(const std::string &title) {
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize((title+"W").c_str()).x, 0));
    ImGui::SameLine();
    ImGui::TextUnformatted(title.c_str());
}

// Like ImGui::Text(), but draws a translucent black rectangle behind the
// text so labels stay readable when overlaid on a busy background (e.g.
// drawn over a map). Cursor advances as for normal Text.
//
// The backdrop extends down by ItemSpacing.y so successive overlay lines
// produce abutting rectangles instead of leaving a transparent stripe of
// background between them. The right edge is extended by the same amount
// for visual symmetry — text doesn't hug the rectangle edge.
inline void doOverlayText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz  = ImGui::CalcTextSize(buf);
    const float pad = ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddRectFilled(
        pos,
        ImVec2(pos.x + sz.x + pad, pos.y + sz.y + pad),
        IM_COL32(0, 0, 0, 128));
    ImGui::TextUnformatted(buf);
}

// Centered AddText that shrinks the font size to fit maxWidth, for labels drawn
// onto a key or a segment whose width is fixed by the grid rather than by the
// text. `size` is a starting point, not a floor: a label too wide for its key
// comes out smaller rather than clipped or truncated.
//
// The font is explicit because each one covers only the glyphs it was built
// for (style::loadFonts) -- style::labelFont is the one for large labels
// containing letters, style::bigFont holds digits alone. A font that cannot
// spell the label is corrected rather than obeyed: ImGui would otherwise
// substitute its fallback glyph per character and quietly draw a different
// string, and no data reaching a key label is worth that.
inline void drawCenteredLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 center, float maxWidth, ImU32 col, const char* text) {
    if (!style::fontCovers(font, text)) { font = style::fontFor(text, size); }
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (ts.x > maxWidth && ts.x > 0.0f) {
        size *= maxWidth / ts.x;
        ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }
    dl->AddText(font, size, ImVec2(center.x - ts.x / 2.0f, center.y - ts.y / 2.0f), col, text);
}

// Bump the button color alpha so labels stay legible when buttons sit on
// top of a busy background (e.g. the world map). Pair with
// popOverlayButtonStyle. Doubles alpha clamped to 1.0 — buttons whose
// theme alpha is already 1 are unchanged.
inline void pushOverlayButtonStyle() {
    const auto bump = [](ImGuiCol id) {
        ImVec4 c = ImGui::GetStyle().Colors[id];
        c.w = std::min(1.0f, c.w * 2.0f);
        return c;
    };
    ImGui::PushStyleColor(ImGuiCol_Button, bump(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bump(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bump(ImGuiCol_ButtonActive));
}
inline void popOverlayButtonStyle() {
    ImGui::PopStyleColor(3);
}
