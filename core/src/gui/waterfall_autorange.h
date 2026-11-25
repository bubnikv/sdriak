#pragma once
#include <imgui/imgui.h>
#include <functional>

// Continuous ("sticky") waterfall auto-range, driven by the autoscale button.
//
// A short click does a one-shot fit of the display range to the current FFT.
// Holding the button latches a continuous fit that keeps re-tracking the live
// noise floor / signal population with asymmetric temporal smoothing, so the
// range follows a rising floor quickly but relaxes downward slowly instead of
// pumping. The latched state persists in the config.
//
// The range itself (fftMin/fftMax) is owned by MainWindow -- this class only
// borrows references to it so the manual sliders, config and auto-scaler all
// act on the same values.
class WaterfallAutoRange {
public:
    // Slider bounds for the Ref+Range model. The manual sliders in MainWindow
    // and the auto-scaler's clamps must agree, so they share these: Ref (the
    // window floor, = fftMin) and Range (= fftMax - fftMin), both in dB.
    static constexpr float REF_MIN = -160.0f;
    static constexpr float REF_MAX = -30.0f;
    static constexpr float RANGE_MIN = 30.0f;
    static constexpr float RANGE_MAX = 160.0f;

    // Bind to MainWindow's fftMin/fftMax and seed the latch. persist
    // stores fftMin/fftMax and sticky() wherever the owner keeps them.
    void init(float* fftMin, float* fftMax, bool sticky, std::function<void()> persist);

    // Side of the (square) autoscale button: the app's icon-button footprint,
    // 30 dp glyph + 5 dp padding, as the toolbar buttons use -- squared so the
    // round contrast glyph reads as an icon rather than a mark floating in a
    // pill, and so it is a comfortable target in both axes. Capped by the width
    // the caller has for it. Depends only on that and the UI scale, hence
    // static: the caller's vertical layout budget and the draw below must agree
    // exactly or the controls strip overflows, so both size it from here.
    static float buttonSize(float maxSide);

    // Draw the autoscale button (Material contrast glyph) at the current cursor
    // and handle the click-vs-hold gesture. The caller positions the cursor and
    // gives the square button's side; `tint` colours the glyph while the latch
    // is off (while it is on, the shared latched-toggle colours take over).
    void drawButton(float side, const ImVec4& tint);

    // Step the continuous fit (throttled to ~10 Hz internally). Call once per
    // frame after the controls child so it keeps running with the menu closed.
    void update();

    // True while continuous auto-range is latched; used to disable the manual
    // Min/Max sliders so the user isn't fighting the auto-scaler.
    bool sticky() const { return stickyEnabled; }

private:
    void oneShotFit();
    void setSticky(bool on);
    void applyRef(float ref);  // set Ref (floor), preserving Range
    void normalizeRange();     // clamp legacy config into Ref/Range bounds

    float* fftMin = nullptr;
    float* fftMax = nullptr;
    std::function<void()> persist;

    bool stickyEnabled = false;
    bool initialized = false; // animated range seeded from the first fit
    float btnHold = 0.0f;     // long-press timer
    bool longPressed = false; // long-press already latched this press
    float updateAccum = 0.0f; // throttles the continuous fit to ~10 Hz
};
