#pragma once
#include <imgui.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>

namespace ImGui {
    // For consumers that want the whole band (baseband recorder, IQ
    // exporter, scanner): while a networked source streams only a narrow
    // IQ window, say so. They keep working on what the window carries.
    // UI thread.
    inline void NarrowIQNotice() {
        if (!sigpath::iqFrontEnd.isIQNarrow()) { return; }
        const double centre = gui::waterfall.getCenterFrequency() + sigpath::iqFrontEnd.narrowIQOffset();
        const double halfWidth = sigpath::iqFrontEnd.narrowIQWidth() / 2.0;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
        ImGui::TextWrapped("IQ is a narrow server window (%.3f MHz +/- %.1f kHz); baseband covers only that.",
            centre / 1e6, halfWidth / 1e3);
        ImGui::PopStyleColor();
    }
}
