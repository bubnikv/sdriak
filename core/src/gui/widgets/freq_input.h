#pragma once
#include <config.h>
#include <imgui.h>
#include <stdint.h>
#include <algorithm>
#include <memory>
#include <string>

namespace bandplan {
    struct Band_t;
    struct BandPlan_t;
}

// The frequency-input dialog: a modal with Band, Spectrum and Frequency pages, opened
// by the top bar's keypad button or by a long press on a frequency digit.
//
// The digit widget in frequency_select.h owns one Dialog and hands it a Context
// each frame. Everything here is a view: the only tuning that happens is through
// gui::bandStack, which the band page is a view over, and the frequency the
// keypad page returns in its Outcome.
namespace freq_input {

    // The tuning situation a page works against: where the radio is now, and
    // what the source will accept, passed to every keypad helper as one
    // argument.
    struct Context {
        uint64_t frequency = 0; // current display frequency, Hz
        bool limited = false;   // the source imposes a tuning range
        //FIXME fill in where the minFreq/maxFreq come from, what they mean.
        uint64_t minFreq = 0;
        uint64_t maxFreq = 0;

        // Range edges, normalised so lo <= hi. Both 0 when unlimited; callers
        // that care check `limited` first.
        uint64_t rangeLo() const { return limited ? ((minFreq < maxFreq) ? minFreq : maxFreq) : 0; }
        uint64_t rangeHi() const { return limited ? ((minFreq > maxFreq) ? minFreq : maxFreq) : 0; }
    };

    // The rectangle the dialog and its popups must stay inside: the viewport
    // less a margin that keeps them off the screen edges. One definition, so
    // the modal's size constraint and the register list's placement cannot
    // drift apart.
    struct SafeArea {
        ImVec2 lo = ImVec2(0.0f, 0.0f);
        ImVec2 hi = ImVec2(0.0f, 0.0f);

        ImVec2 size() const { return ImVec2(hi.x - lo.x, hi.y - lo.y); }

        // The top-left nearest `wanted` that keeps a `size` rectangle inside.
        // When the rectangle is larger than the area the near edge wins: the
        // start of a list is worth more than its end.
        ImVec2 fit(ImVec2 wanted, ImVec2 size) const {
            return ImVec2(
                (std::max)(lo.x, (std::min)(wanted.x, hi.x - size.x)),
                (std::max)(lo.y, (std::min)(wanted.y, hi.y - size.y)));
        }

        static SafeArea get();
    };

    // The dialog's layout budget, recomputed each frame by Dialog. Every page
    // is sized from it, so the popup keeps one width across pages and the dp()
    // constants exist in exactly one place; the page toggle and the band grid
    // read them from here rather than re-deriving them.
    //
    // Everything here is a function of the viewport and the style, never of the
    // content. A width that depended on what the grid holds would resize the
    // dialog as the user switches category; a height that depended on how much
    // the keypad has to say would move the keys out from under a finger
    // mid-entry.
    struct Metrics {
        // Keypad block.
        ImVec2 keySize;             // one digit key
        float funcWidth = 0.0f;     // CE / Cancel / ENT column
        float keypadWidth = 0.0f;   // 3 digit columns + the function column
        bool wideKeypad = false;    // readout beside the keys, not above them
        float readoutWidth = 0.0f;  // readout block, where wideKeypad puts it
        float readoutHeight = 0.0f;

        // Page.
        float totalWidth = 0.0f;    // content width, identical on every page
        float pageHeight = 0.0f;    // height left for a page below the toggle row
        float toggleHeight = 0.0f;  // page toggle / close row
        float rowHeight = 0.0f;     // minimum interactive row (48 dp under touch)

        // Grids. The column count follows the width, so a viewport that is
        // short and wide -- a phone in landscape -- trades rows for columns and
        // fits instead of overflowing.
        int bandCols = 4;
        int spectrumCols = 3;
        float bandKeyHeight = 0.0f;
        float spectrumKeyHeight = 0.0f;

        // Width available to grid keys: `totalWidth` less a permanently
        // reserved scrollbar, so nothing shifts on the frame a grid starts
        // scrolling and the popup does not change width between pages.
        float gridWidth = 0.0f;

        // Whole rows of `keyH` that fit in `avail`, never fewer than two.
        static int rowsThatFit(float avail, float keyH);

        // Child height for a grid of `rowsNeeded` rows: exactly what it needs
        // when it fits, otherwise half a row short of the budget, the clipped
        // row being the hint that it scrolls.
        static float gridHeight(int rowsNeeded, int rowsFit, float keyH);

        // Takes the context because the keypad's readout reserves two extra
        // lines for a source with tuning limits, and whether those lines fit
        // is what decides the keypad's arrangement.
        static Metrics compute(const Context& ctx);
    };

    // What a page asks of the dialog after a frame. Bands only ever closes: it
    // tunes through gui::bandStack itself. Keypad has no application reach at
    // all and returns its result here, which is what keeps keypad.cpp free of
    // every gui:: and config dependency.
    struct Outcome {
        bool close = false;
        bool commit = false;    // tune to `frequency`
        uint64_t frequency = 0;
        // The page took this frame's cancel key for itself: it had a popup of
        // its own to back out of first. Without it the dialog reads the same
        // Escape and closes everything, which is not what leaving a sub-popup
        // means. Android's Back never had the problem -- it goes through
        // MainWindow::handleBackPress(), which closes the topmost popup only.
        bool consumedCancel = false;
    };

    // Frequency page: direct numeric entry in MHz, IC-705 style. See keypad.cpp.
    class Keypad {
    public:
        void onOpen();
        Outcome draw(const Context& ctx, const Metrics& m);

        // Height the readout block reserves when laid out at `width`: every
        // line it can show in any state, so that neither what is typed nor how
        // long the source's range prints ever moves the keys. Metrics asks for
        // it because whether that block fits above the keys is what decides
        // which arrangement the page gets.
        static float readoutHeight(const Context& ctx, float width);

    private:
        void key(char k, const Context& ctx);
        void commit(const Context& ctx, Outcome& out) const;

        // The typed value in MHz: digits plus at most one decimal point.
        std::string entry;
    };

    // BAND page: a grid of band keys over the loaded band plan, backed by
    // gui::bandStack. See bands.cpp.
    class Bands {
    public:
        Bands();
        ~Bands();

        // Both take the dialog's access: opening the dialog settles every page's
        // state in one config transaction. See freq_memory::activate().
        void onOpen(const ConfigManager::ReadAccess& configAccess);
        void onActivate(ConfigManager::EditAccess& configAccess);
        Outcome draw(const Context& ctx, const Metrics& m);

    private:
        void resetTransientState();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // SPECTRUM page: a service-independent, non-overlapping ITU/IEEE range
    // selector. It tunes through Outcome and keeps its memory separate from the
    // service-band stacking registers. See spectrum.cpp.
    class Spectrum {
    public:
        void onOpen(const ConfigManager::ReadAccess& configAccess);
        // This page has become the visible one. It claims the frequency memory
        // here, mirroring how the band grid claims it when it resolves a band,
        // rather than restating the claim on every frame it is drawn.
        void onActivate(ConfigManager::EditAccess& configAccess);
        Outcome draw(const Context& ctx, const Metrics& m);

    private:
        std::string lastRangeId;
    };

    // The modal shell: the page toggle, the last-used page, and the keys that
    // apply to both pages. See dialog.cpp.
    class Dialog {
    public:
        // Request the dialog on the next draw(). The popup is opened inside
        // draw() so that OpenPopup lands at the same ImGui scope as the
        // matching BeginPopupModal.
        void open() { requestOpen = true; }

        Outcome draw(const Context& ctx);

    private:
        bool requestOpen = false;
        int page = 2; // 0 = Band, 1 = Spectrum, 2 = Frequency
        // Re-centre once when the viewport changes size. Android handles
        // rotation live (configChanges in the manifest), so a dialog open
        // across one would otherwise be left off-centre or half off-screen.
        ImVec2 lastViewport = ImVec2(0.0f, 0.0f);
        // Width changes and vertical growth still re-centre the dialog, but
        // shorter content does not: heightFloor is a high-water mark for the
        // current viewport while the dialog is open. This keeps page/category
        // switches from moving the controls up and down while still allowing
        // genuinely taller content to fit. The window is measured rather than
        // resize events being enumerated, so a
        // category, register count, or source-limit change is covered without
        // every page having to report it.
        ImVec2 lastSize = ImVec2(0.0f, 0.0f);
        float heightFloor = 0.0f;
        bool recenter = false;
        Keypad keypad;
        Bands bands;
        Spectrum spectrum;
    };
}
