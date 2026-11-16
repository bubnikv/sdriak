#include <gui/widgets/freq_input.h>
#include <gui/widgets/freq_memory.h>
#include <gui/widgets/popup_dialog.h>
#include <gui/widgets/segmented_control.h>
#include <gui/style.h>
#include <config.h>
#include <core.h>
#include <algorithm>
#include <cmath>

namespace freq_input {

    static constexpr const char* DIALOG_ID =
        "Bands / Frequency##sdrpp_freq_input_dialog";

    // Columns of `minKey` that fit in `avail`, clamped to a sane band.
    static int colsThatFit(float avail, float minKey, float spacing, int lo, int hi) {
        const int fit = (int)std::floor((avail + spacing) / (minKey + spacing));
        return std::clamp(fit, lo, hi);
    }

    int Metrics::rowsThatFit(float avail, float keyH) {
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        return std::max(2, (int)std::floor((avail + sp) / (keyH + sp)));
    }

    float Metrics::gridHeight(int rowsNeeded, int rowsFit, float keyH) {
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float rows = (rowsNeeded <= rowsFit) ? (float)rowsNeeded
                                                   : ((float)rowsFit - 0.5f);
        return rows * (keyH + sp) - sp;
    }

    SafeArea SafeArea::get() {
        // Keep the dialog and its popups off the screen edges.
        const float margin = style::dp(8.0f);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        SafeArea a;
        a.lo = ImVec2(vp->Pos.x + margin, vp->Pos.y + margin);
        a.hi = ImVec2(std::max(a.lo.x, vp->Pos.x + vp->Size.x - margin),
                      std::max(a.lo.y, vp->Pos.y + vp->Size.y - margin));
        return a;
    }

    Metrics Metrics::compute(const Context& ctx) {
        Metrics m;
        const ImGuiStyle& s = ImGui::GetStyle();
        const ImVec2 sp = s.ItemSpacing;

        // Content area inside the largest window we are willing to occupy,
        // discounting the window padding the content will sit inside.
        const ImVec2 outer = SafeArea::get().size();
        const float availW = outer.x - 2.0f * s.WindowPadding.x;
        const float availH = outer.y - 2.0f * s.WindowPadding.y;

        // Material's 48 dp minimum touch target. A mouse is precise enough that
        // the frame height serves on desktop.
        m.rowHeight = style::touchStyle
            ? std::max(style::dp(48.0f), ImGui::GetFrameHeight())
            : ImGui::GetFrameHeight();
        m.toggleHeight = std::max(style::dp(34.0f), m.rowHeight);

        // Keypad width. The digit keys give width back only when the viewport
        // cannot hold them at all, which takes something narrower than a phone
        // in portrait.
        m.funcWidth = style::dp(84.0f);
        const float keyW = std::clamp((availW - m.funcWidth - 3.0f * sp.x) / 3.0f,
                                      style::dp(40.0f), style::dp(56.0f));
        m.keypadWidth = 3.0f * keyW + 3.0f * sp.x + m.funcWidth;

        // Content width: the keypad block is the floor, the viewport drives it,
        // and the cap stops a 4K monitor from producing a wall of keys.
        m.totalWidth = std::clamp(availW, m.keypadWidth, style::dp(640.0f));
        m.pageHeight = availH - m.toggleHeight - 2.0f * sp.y;

        // Keypad arrangement, decided on both axes. Width alone handed the tall
        // arrangement to viewports with no room for it -- and unlike the grids,
        // which give back rows, the keypad has nothing to yield and simply drew
        // its bottom row past the edge of the screen. Note the test is in dp
        // against a physical viewport, so raising the UI scale is one of the
        // ways a phone crosses it.
        //
        // Beside the keys when there is a comfortable column for it (a phone in
        // landscape and a desktop window both qualify), and also when stacking
        // would not fit but a usable column exists: a cramped readout beats a
        // row of keys below the fold.
        const auto keyRowsHeight = [&](float keyH) {
            return 4.0f * keyH + 3.0f * sp.y;
        };
        const float sideWidth = m.totalWidth - m.keypadWidth - sp.x;
        float keyH = std::max(style::dp(42.0f), m.rowHeight);
        // Stacked, the readout costs two spacings, not one: EndChild() advances
        // by the child's height plus ItemSpacing.y, and the explicit Spacing()
        // between the two blocks adds another.
        const float stackedGap = 2.0f * sp.y;

        // A comfortable column decides on its own. Only when there is not one
        // do we have to ask how tall stacking would be -- and that question is
        // the expensive part, since answering it measures every line the
        // readout can show.
        float readoutAbove = 0.0f;
        if (sideWidth >= style::dp(200.0f)) {
            m.wideKeypad = true;
        }
        else {
            readoutAbove = Keypad::readoutHeight(ctx, m.totalWidth);
            m.wideKeypad =
                (readoutAbove + stackedGap + keyRowsHeight(keyH)) > m.pageHeight &&
                sideWidth >= style::dp(120.0f);
        }

        // Where the readout ends up and how tall it is there. Settled here so
        // the page does not restate the width and measure the height again --
        // and so the two cannot disagree about which arrangement is in force.
        m.readoutWidth = m.wideKeypad ? sideWidth : m.totalWidth;
        m.readoutHeight = m.wideKeypad ? Keypad::readoutHeight(ctx, sideWidth)
                                       : readoutAbove;

        // Last resort, once the arrangement is chosen: shrink the keys, below
        // the 48 dp touch target if it comes to that. The readout cannot give
        // -- it reserves its worst case precisely so nothing moves mid-entry --
        // and a key that is awkward to hit still beats one that is off-screen.
        const float keysBudget = m.wideKeypad
            ? m.pageHeight
            : m.pageHeight - m.readoutHeight - stackedGap;
        if (keyRowsHeight(keyH) > keysBudget) {
            keyH = std::clamp((keysBudget - 3.0f * sp.y) / 4.0f,
                              style::dp(32.0f), keyH);
        }
        m.keySize = ImVec2(keyW, keyH);

        m.gridWidth = m.totalWidth - s.ScrollbarSize;
        m.bandCols = colsThatFit(m.gridWidth, style::dp(63.0f), sp.x, 4, 10);
        m.spectrumCols = colsThatFit(m.gridWidth, style::dp(87.0f), sp.x, 3, 8);
        m.bandKeyHeight = std::max(style::dp(52.0f), m.rowHeight);
        m.spectrumKeyHeight = std::max(style::dp(54.0f), m.rowHeight);
        return m;
    }

    // Close glyph. Roboto-Medium ships no multiplication sign in our glyph
    // ranges, so the cross is drawn by hand, as the backspace key's icon is.
    static void drawCloseIcon() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 bmin = ImGui::GetItemRectMin();
        const ImVec2 bmax = ImGui::GetItemRectMax();
        const ImVec2 ctr((bmin.x + bmax.x) / 2.0f, (bmin.y + bmax.y) / 2.0f);
        const float r = style::dp(6.0f);
        const float th = style::lineWidth(1.5f);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddLine(ImVec2(ctr.x - r, ctr.y - r), ImVec2(ctr.x + r, ctr.y + r), col, th);
        dl->AddLine(ImVec2(ctr.x - r, ctr.y + r), ImVec2(ctr.x + r, ctr.y - r), col, th);
    }

    Outcome Dialog::draw(const Context& ctx) {
        Outcome out;
        const float initialHeightFloor = style::dp(320.0f);

        if (requestOpen) {
            requestOpen = false;
            lastSize = ImVec2(0.0f, 0.0f);
            // A modest baseline absorbs the usual page-height differences
            // without requesting the full safe-area height. It is a floor, not
            // a fixed size: taller content may grow it.
            heightFloor = initialHeightFloor;
            keypad.onOpen();
            // Both pages' restored state and the last-used page come from one
            // access, which then records the page being entered. Opening the
            // dialog is a single config transaction rather than four.
            {
                auto configAccess = core::configManager.edit();
                bands.onOpen(configAccess);
                spectrum.onOpen(configAccess);
                const std::string storedPage = freq_memory::root(configAccess).value(
                    freq_memory::PAGE,
                    "keypad");
                if (storedPage == "band") { page = 0; }
                else if (storedPage == "spectrum") { page = 1; }
                else { page = 2; }
                // Opening straight onto a page counts as entering it.
                if (page == 0) { bands.onActivate(configAccess); }
                else if (page == 1) { spectrum.onActivate(configAccess); }
            }
            ImGui::OpenPopup(DIALOG_ID);
        }

        const ImVec2 viewport = ImGui::GetMainViewport()->Size;
        if (viewport.x != lastViewport.x || viewport.y != lastViewport.y) {
            lastViewport = viewport;
            // A high-water mark belongs to the geometry in which it was
            // measured. Re-establish it after rotation or resizing rather than
            // carrying (for example) a tall portrait page into landscape.
            lastSize = ImVec2(0.0f, 0.0f);
            heightFloor = initialHeightFloor;
            recenter = true;
        }
        // Bound the window before it sizes itself: with AlwaysAutoResize it
        // takes whatever its content asks for, and a page asking for more than
        // the screen has would hang off the edge with no way to reach the rest.
        // Constrained, it scrolls instead. The pages size themselves to fit and
        // should never reach this; it is the backstop for the ones that cannot,
        // such as a viewport too short for four rows of keys.
        const ImVec2 maxSize = SafeArea::get().size();
        const float constrainedHeightFloor = (std::min)(heightFloor, maxSize.y);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0.0f, constrainedHeightFloor), maxSize);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                recenter ? ImGuiCond_Always : ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        recenter = false;
        // No title bar: its height is the scarcest thing this dialog has on a
        // phone in landscape, and the page toggle names the available views.
        if (!ImGui::BeginPopupModal(DIALOG_ID, NULL,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoMove))
        {
            return out;
        }

        Metrics m = Metrics::compute(ctx);
        const float sp = ImGui::GetStyle().ItemSpacing.x;

        // Page toggle plus the dialog's one dismiss control, in the same place
        // on every page -- the pages below carry no Cancel of their own.
        // `page` is the segment index by construction.
        static const char* pageLabels[] = { "BAND", "SPECTRUM", "Frequency" };
        const float closeW = m.toggleHeight;
        if (doSegmentedControl("##sdrpp_freq_input_page", page, pageLabels, 3,
                               ImVec2(m.totalWidth - closeW - sp, m.toggleHeight)))
        {
            auto configAccess = core::configManager.edit();
            if (page == 0) { bands.onActivate(configAccess); }
            else if (page == 1) { spectrum.onActivate(configAccess); }
            freq_memory::root(configAccess).set(
                freq_memory::PAGE,
                (page == 0) ? "band" :
                (page == 1) ? "spectrum" : "keypad");
        }
        ImGui::SameLine();
        const bool closePressed =
            ImGui::Button("##sdrpp_freq_input_close", ImVec2(closeW, m.toggleHeight));
        drawCloseIcon();
        ImGui::Spacing();

        if (page == 0) { out = bands.draw(ctx, m); }
        else if (page == 1) { out = spectrum.draw(ctx, m); }
        else { out = keypad.draw(ctx, m); }

        // Escape closes from any page, so it is handled here rather than three
        // times -- unless the page had a popup of its own open and used the key
        // to back out of that, one level at a time.
        if (closePressed ||
            (!out.consumedCancel && PopupDialog::cancelKeyPressed()))
        {
            out.close = true;
        }
        if (out.close) { ImGui::CloseCurrentPopup(); }

        // Preserve the greatest height reached in the current viewport. A
        // shorter page then leaves slack below its content instead of shrinking
        // and moving the centred dialog. Growth remains content-driven and is
        // capped by maxSize above; an oversized page scrolls inside the modal.
        // Width changes and growth are re-centred on the next frame so the
        // window cannot run off an edge.
        const ImVec2 size = ImGui::GetWindowSize();
        const bool widthChanged = size.x != lastSize.x;
        const bool grew = size.y > lastSize.y;
        heightFloor = (std::max)(heightFloor, size.y);
        if (widthChanged || grew) {
            recenter = true;
        }
        lastSize = size;
        ImGui::EndPopup();
        return out;
    }

}
