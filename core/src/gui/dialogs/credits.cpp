#include <gui/dialogs/credits.h>
#include <imgui.h>
#include <gui/widgets/popup_dialog.h>
#include <gui/icons.h>
#include <gui/style.h>
#include <config.h>
#include <credits.h>
#include <version.h>

namespace credits {
    ImFont* bigFont;
    ImVec2 imageSize(128.0f, 128.0f);

    // True once a touch/click began while the dialog was already open. Prevents the
    // click that opened the dialog (or one that started on the scrollbar) from closing it.
    static bool dismissArmed = false;

    // The popup is opened once per presentation rather than every frame.
    // Android Back closes the topmost ImGui popup from the outside (see
    // MainWindow::handleBackPress), and re-issuing OpenPopup() here would put it
    // straight back on the stack in the same frame -- which is why Back used to
    // do nothing at all. Every path that closes the dialog must clear this, or
    // the next presentation opens nothing and the one after it is the first to
    // work. Paths that bypass show() entirely go through reset().
    static bool popupOpened = false;

    void init() {
        imageSize = style::dp(128.0f, 128.0f);
    }

    void reset() {
        dismissArmed = false;
        popupOpened = false;
    }

    bool show() {
        bool open = true;
        imageSize = style::dp(128.0f, 128.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style::dp(20.0f, 20.0f));
        // The scrollbar keeps the shared style width, like every other scrollable
        // panel. It used to be widened to a finger-sized 25 dp, but the body
        // drag-scrolls anywhere (see below), so nobody needs to hit the bar to
        // scroll on a touch screen and the extra width only steals content space.
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImVec2 dispSize = ImGui::GetIO().DisplaySize;
        ImVec2 center = ImVec2(dispSize.x / 2.0f, dispSize.y / 2.0f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!popupOpened) {
            ImGui::OpenPopup("Credits");
            popupOpened = true;
        }
        if (!ImGui::BeginPopupModal("Credits", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
            // Closed from the outside, i.e. Android Back dismissing the topmost
            // popup. Report it so the caller drops showCredits. BeginPopupModal
            // consumed the SetNextWindowPos above on its way out, so only the
            // style stack is left to unwind here.
            reset();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            return false;
        }

        ImGui::PushFont(style::hugeFont);
        ImGui::TextUnformatted("SDRIAK");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - imageSize.x, 0.0f);
        ImGui::Image(icons::LOGO, imageSize);
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::TextUnformatted("Maintained by Vojtech Bubnik (OK1IAK), original software by Alexandre Rouma (ON5RYZ) with the help of\n\n");

        ImGui::Columns(4, "CreditColumns", true);

        ImGui::TextUnformatted("Contributors");
        for (int i = 0; i < sdrpp_credits::contributorCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::contributors[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Libraries");
        for (int i = 0; i < sdrpp_credits::libraryCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::libraries[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Hardware Donators");
        for (int i = 0; i < sdrpp_credits::hardwareDonatorCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::hardwareDonators[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Patrons");
        for (int i = 0; i < sdrpp_credits::patronCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::patrons[i]);
        }

        ImGui::Columns(1, "CreditColumnsEnd", true);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextUnformatted("SDRIAK v" VERSION_STR " (Built at " __TIME__ ", " __DATE__ ")");

        ImGuiIO& io = ImGui::GetIO();
        float dragThreshold = std::max(io.MouseDragThreshold, style::dp(6.0f));

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Presses landing on the scrollbar must neither dismiss the dialog nor drag-scroll it
            ImVec2 winPos = ImGui::GetWindowPos();
            ImVec2 winSize = ImGui::GetWindowSize();
            bool onScrollbar = (ImGui::GetScrollMaxY() > 0.0f) && (io.MousePos.x >= winPos.x + winSize.x - ImGui::GetStyle().ScrollbarSize);
            dismissArmed = !onScrollbar;
        }

        // Drag anywhere to scroll (touch screens); latched on the press so it keeps working
        // when the finger slides past the window edge
        if (dismissArmed && ImGui::IsMouseDragging(ImGuiMouseButton_Left, dragThreshold)) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        }

        // Dismiss on Escape, or on a tap/click that didn't turn into a scroll drag
        bool tapped = dismissArmed && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < dragThreshold * dragThreshold);
        if (tapped || PopupDialog::cancelKeyPressed()) {
            reset();
            ImGui::CloseCurrentPopup();
            open = false;
        }

        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return open;
    }
}
