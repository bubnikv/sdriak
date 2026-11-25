#include <gui/waterfall_autorange.h>
#include <gui/gui.h>   // gui::waterfall
#include <gui/icons.h> // icons::CONTRAST
#include <gui/style.h>
#include <gui/widgets/toggle_style.h>
#ifdef __ANDROID__
#include <android_backend.h>
#endif
#include <algorithm>
#include <cmath>

void WaterfallAutoRange::init(float* min, float* max, bool sticky, std::function<void()> persistLevels) {
    fftMin = min;
    fftMax = max;
    persist = std::move(persistLevels);
    stickyEnabled = sticky;
    initialized = false;
    normalizeRange();
}

void WaterfallAutoRange::normalizeRange() {
    // Bring legacy / out-of-bounds config into the Ref+Range model on load:
    // Ref (= fftMin) in [-160,-30], Range (= fftMax-fftMin) in [30,160].
    // In-memory only; config is rewritten on the next slider change or when
    // sticky is toggled off.
    float ref = std::clamp(*fftMin, REF_MIN, REF_MAX);
    float range = std::clamp(*fftMax - *fftMin, RANGE_MIN, RANGE_MAX);
    *fftMin = ref;
    *fftMax = ref + range;
}

void WaterfallAutoRange::applyRef(float ref) {
    // Ref is the floor / bottom of the window. Slide the window to it while
    // preserving the user's Range (contrast). Ref stays within the slider's
    // bounds so the greyed Ref slider shows a valid position while sticky.
    float range = *fftMax - *fftMin;
    ref = std::clamp(ref, REF_MIN, REF_MAX);
    *fftMin = ref;
    *fftMax = ref + range;
}

void WaterfallAutoRange::oneShotFit() {
    // Ref only: re-level to the measured noise floor, keep the user's Range.
    // Pool the last 5 FFT lines so a single click can't land on a transient
    // whole-band burst (sticky uses 1 line -- its EMA smooths over time).
    float ref;
    bool ok = gui::waterfall.getAutorangeRef(ref, 5);
    // Release the 5x pool buffer (grown even on a failed fit) so the 1-line
    // sticky path doesn't hold 5x the memory.
    gui::waterfall.freeAutorangeScratch();
    if (!ok) { return; }
    applyRef(ref);
    if (persist) { persist(); }
}

void WaterfallAutoRange::setSticky(bool on) {
    stickyEnabled = on;
    initialized = false; // snap to the first fit on (re)acquire
    // Also stores wherever the auto-scaler left the range, so the manual
    // sliders resume from there.
    if (persist) { persist(); }
}

// Glyph and the padding around it, matching the toolbar's ImageButtons.
static constexpr float BTN_GLYPH_DP = 30.0f;
static constexpr float BTN_PAD_DP   = 5.0f;

float WaterfallAutoRange::buttonSize(float maxSide) {
    return std::floor(std::min(maxSide, style::dp(BTN_GLYPH_DP + 2.0f * BTN_PAD_DP)));
}

void WaterfallAutoRange::drawButton(float side, const ImVec4& tint) {
    // A plain Button with the glyph drawn centred on top, rather than an
    // ImageButton: the frame, rounding and hover/active states then come from
    // the same path as the mode-grid buttons, and the latched state can use the
    // shared toggle look. ImageButton's bg_col only covers the area inside the
    // frame padding, so a latched fill there would be a small square, and in
    // ButtonActive -- the colour a button shows while being pressed, which
    // reads as "idle" rather than "on".
    //
    // The toggle colours are only derived while latched -- the idle button uses
    // the plain theme colours and the caller's tint.
    style::SelectedToggleColors cols;
    if (stickyEnabled) {
        cols = style::selectedToggleColors();
        style::pushSelectedToggle(cols);
    }
    bool clicked = ImGui::Button("##sdrpp_wf_autorange", ImVec2(side, side));
    // Capture item state before any other ImGui call (the tooltip) moves the
    // "last item" these queries refer to.
    bool active = ImGui::IsItemActive();
#ifndef __ANDROID__
    bool hovered = ImGui::IsItemHovered(); // feeds the desktop-only tooltip below
#endif
    ImVec2 rectMin = ImGui::GetItemRectMin();
    ImVec2 rectMax = ImGui::GetItemRectMax();
    if (stickyEnabled) {
        style::drawSelectedToggleStroke(cols);
        style::popSelectedToggle();
    }

    // Glyph centred in the square, inset by the toolbar's icon padding so it
    // carries the same optical weight as the hamburger and play icons (and
    // shrinks with the button if the strip was too narrow for the full side).
    // Whole-pixel corners keep it sharp.
    float glyph = std::max(1.0f, std::round(side - 2.0f * style::dp(BTN_PAD_DP)));
    ImVec2 glyphMin(std::round((rectMin.x + rectMax.x - glyph) * 0.5f), std::round((rectMin.y + rectMax.y - glyph) * 0.5f));
    ImGui::GetWindowDrawList()->AddImage(icons::CONTRAST, glyphMin, ImVec2(glyphMin.x + glyph, glyphMin.y + glyph),
                                         ImVec2(0, 0), ImVec2(1, 1),
                                         ImGui::GetColorU32(stickyEnabled ? cols.content : tint));

#ifndef __ANDROID__
    // Hover-tooltip is desktop-only, as in the KiwiSDR map: the Android
    // backend never parks the cursor off-screen (it feeds the touch point on
    // both DOWN and UP, unlike GLFW), so IsItemHovered() stays true after the
    // finger lifts and the tooltip would sit over the waterfall until the
    // next touch elsewhere. Touch users get the haptic tick below instead.
    if (hovered) {
        ImGui::SetTooltip("Click: auto-fit range once\nHold: %s continuous auto-range", stickyEnabled ? "disable" : "enable");
    }
#endif

    // Hold-to-latch: the long-press fires while the button is still held (so
    // the release doesn't also trigger the one-shot). longPressed is consumed
    // before it's reset so a release that lands outside the button can't leave
    // the button dead.
    if (active) {
        btnHold += ImGui::GetIO().DeltaTime;
        if (!longPressed && btnHold >= 0.5f) {
            longPressed = true;
            setSticky(!stickyEnabled);
#ifdef __ANDROID__
            // Confirms a long-press that fires while the finger is still down,
            // as the hamburger hold-to-exit and the menu splitter do. On touch
            // this is the only cue that the hold (rather than a tap) landed.
            backend::hapticTick();
#endif
        }
    }
    if (clicked && !longPressed) {
        oneShotFit(); // short click
    }
    if (!active) {
        btnHold = 0.0f;
        longPressed = false;
    }
}

void WaterfallAutoRange::update() {
    if (!stickyEnabled) { return; }

    // Throttle to ~10 Hz (the deep-research 4-10 Hz recommendation): for users
    // who enabled fullWaterfallUpdate, each range change forces a full
    // framebuffer recolor, so we must not do it every frame.
    const float step = 0.1f;
    updateAccum += ImGui::GetIO().DeltaTime;
    if (updateAccum < step) { return; }
    float dt = updateAccum;
    updateAccum = 0.0f;

    float refTarget;
    if (!gui::waterfall.getAutorangeRef(refTarget)) { return; }

    if (!initialized) {
        // First fit after latching / reacquisition: snap, don't glide.
        applyRef(refTarget);
        initialized = true;
    }
    else {
        // Asymmetric smoothing on Ref (the noise floor): follow a rising floor
        // quickly so the display can't wash out, but relax downward slowly to
        // avoid flicker. A large jump (band / device / gain change) bypasses
        // the glide and reacquires immediately. applyRef preserves Range.
        float cur = *fftMin;
        // Deadband: once the floor has settled, the asymptotic EMA would keep
        // nudging Ref by fractions of a dB every tick forever, and each nudge
        // forces a full framebuffer recolor for users with fullWaterfallUpdate.
        // Below this the estimate is noise, so let it rest and stop recoloring.
        const float DEADBAND_DB = 0.1f;
        if (std::abs(refTarget - cur) < DEADBAND_DB) { return; }
        // A large jump (band / device / gain change) bypasses the glide.
        const float REACQUIRE_JUMP_DB = 25.0f;
        // Asymmetric time constants: follow a rising floor quickly, relax down
        // slowly to avoid flicker.
        const float TAU_RISE_S = 0.6f;
        const float TAU_FALL_S = 3.0f;
        float newRef;
        if (std::abs(refTarget - cur) > REACQUIRE_JUMP_DB) {
            newRef = refTarget;
        }
        else {
            float tau = (refTarget > cur) ? TAU_RISE_S : TAU_FALL_S;
            float alpha = 1.0f - expf(-dt / tau);
            newRef = cur + alpha * (refTarget - cur);
        }
        applyRef(newRef);
    }
}
