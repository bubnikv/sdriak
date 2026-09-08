#include <gui/widgets/waterfall.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imutils.h>
#include <algorithm>
#include <cmath>
#include <volk/volk.h>
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#ifdef __ANDROID__
#include <android_backend.h>
#endif

float DEFAULT_COLOR_MAP[][3] = {
    { 0x00, 0x00, 0x20 },
    { 0x00, 0x00, 0x30 },
    { 0x00, 0x00, 0x50 },
    { 0x00, 0x00, 0x91 },
    { 0x1E, 0x90, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0x00 },
    { 0xFE, 0x6D, 0x16 },
    { 0xFF, 0x00, 0x00 },
    { 0xC6, 0x00, 0x00 },
    { 0x9F, 0x00, 0x00 },
    { 0x75, 0x00, 0x00 },
    { 0x4A, 0x00, 0x00 }
};

// TODO: Fix this hacky BS

double freq_ranges[] = {
    1.0, 2.0, 2.5, 5.0,
    10.0, 20.0, 25.0, 50.0,
    100.0, 200.0, 250.0, 500.0,
    1000.0, 2000.0, 2500.0, 5000.0,
    10000.0, 20000.0, 25000.0, 50000.0,
    100000.0, 200000.0, 250000.0, 500000.0,
    1000000.0, 2000000.0, 2500000.0, 5000000.0,
    10000000.0, 20000000.0, 25000000.0, 50000000.0
};

inline double findBestRange(double bandwidth, int maxSteps) {
    for (int i = 0; i < 32; i++) {
        if (bandwidth / freq_ranges[i] < (double)maxSteps) {
            return freq_ranges[i];
        }
    }
    return 50000000.0;
}

inline void printAndScale(double freq, char* buf) {
    double freqAbs = fabs(freq);
    if (freqAbs < 1000) {
        sprintf(buf, "%.6g", freq);
    }
    else if (freqAbs < 1000000) {
        sprintf(buf, "%.6lgK", freq / 1000.0);
    }
    else if (freqAbs < 1000000000) {
        sprintf(buf, "%.6lgM", freq / 1000000.0);
    }
    else if (freqAbs < 1000000000000) {
        sprintf(buf, "%.6lgG", freq / 1000000000.0);
    }
}

inline void doZoom(int offset, int width, int inSize, int outSize, float* in, float* out) {
    // NOTE: REMOVE THAT SHIT, IT'S JUST A HACKY FIX
    if (offset < 0) {
        offset = 0;
    }
    if (width > 524288) {
        width = 524288;
    }

    float factor = (float)width / (float)outSize;
    float sFactor = ceilf(factor);
    float uFactor;
    float id = offset;
    float maxVal;
    int sId;
    for (int i = 0; i < outSize; i++) {
        maxVal = -INFINITY;
        sId = (int)id;
        uFactor = (sId + sFactor > inSize) ? sFactor - ((sId + sFactor) - inSize) : sFactor;
        for (int j = 0; j < uFactor; j++) {
            if (in[sId + j] > maxVal) { maxVal = in[sId + j]; }
        }
        out[i] = maxVal;
        id += factor;
    }
}

namespace ImGui {
    WaterFall::WaterFall() {
        fftMin = -70.0;
        fftMax = 0.0;
        waterfallMin = -70.0;
        waterfallMax = 0.0;
        FFTAreaHeight = 300;
        newFFTAreaHeight = FFTAreaHeight;
        fftHeight = FFTAreaHeight - 50;
        dataWidth = 600;
        lastWidgetPos.x = 0;
        lastWidgetPos.y = 0;
        lastWidgetSize.x = 0;
        lastWidgetSize.y = 0;
        latestFFT = new float[dataWidth];
        latestFFTHold = new float[dataWidth];
        // Fill with the "hide everything" sentinel so the buffers hold a
        // well-defined value before the first FFT frame / onResize() fill.
        for (int i = 0; i < dataWidth; i++) {
            latestFFT[i] = -1000.0f;
            latestFFTHold[i] = -1000.0f;
        }
        waterfallFb = new uint32_t[1];

        viewBandwidth = 1.0;
        wholeBandwidth = 1.0;

        updatePallette(DEFAULT_COLOR_MAP, 13);
    }

    void WaterFall::init() {
        glGenTextures(1, &textureId);
    }

    void WaterFall::drawFFT() {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        // Calculate scaling factor
        float startLine = floorf(fftMax / vRange) * vRange;
        float vertRange = fftMax - fftMin;
        float scaleFactor = fftHeight / vertRange;
        char buf[100];

        ImU32 trace = ImGui::GetColorU32(ImGuiCol_PlotLines);
        ImU32 traceHold = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftHoldColor);
        ImU32 shadow = ImGui::GetColorU32(ImGuiCol_PlotLines, 0.2);
        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        float textVOffset = (float)style::scale(10.0f);

        // Vertical scale
        for (float line = startLine; line > fftMin; line -= vRange) {
            float yPos = fftAreaMax.y - ((line - fftMin) * scaleFactor);
            window->DrawList->AddLine(ImVec2(fftAreaMin.x, roundf(yPos)),
                                      ImVec2(fftAreaMax.x, roundf(yPos)),
                                      IM_COL32(50, 50, 50, 255), style::lineWidth());
            sprintf(buf, "%d", (int)line);
            ImVec2 txtSz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(ImVec2(fftAreaMin.x - txtSz.x - textVOffset, roundf(yPos - (txtSz.y / 2.0))), text, buf);
        }

        // Horizontal scale
        double startFreq = ceilf(lowerFreq / range) * range;
        double horizScale = (double)dataWidth / viewBandwidth;
        float scaleVOfsset = (float)style::scale(7.0f);
        for (double freq = startFreq; freq < upperFreq; freq += range) {
            double xPos = fftAreaMin.x + ((freq - lowerFreq) * horizScale);
            window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMin.y + 1),
                                      ImVec2(roundf(xPos), fftAreaMax.y),
                                      IM_COL32(50, 50, 50, 255), style::lineWidth());
            window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMax.y),
                                      ImVec2(roundf(xPos), fftAreaMax.y + scaleVOfsset),
                                      text, style::lineWidth());
            printAndScale(freq, buf);
            ImVec2 txtSz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(ImVec2(roundf(xPos - (txtSz.x / 2.0)), fftAreaMax.y + txtSz.y), text, buf);
        }

        // Data
        if (latestFFT != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                double aPos = fftAreaMax.y - ((latestFFT[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFT[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), trace, 1.0);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i, roundf(bPos)),
                                          ImVec2(fftAreaMin.x + i, fftAreaMax.y), shadow, 1.0);
            }
        }

        // Hold
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                double aPos = fftAreaMax.y - ((latestFFTHold[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFTHold[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), traceHold, 1.0);
            }
        }

        FFTRedrawArgs args;
        args.min = fftAreaMin;
        args.max = fftAreaMax;
        args.lowFreq = lowerFreq;
        args.highFreq = upperFreq;
        args.freqToPixelRatio = horizScale;
        args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
        args.window = window;
        onFFTRedraw.emit(args);

        // X Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMax.y),
                                  ImVec2(fftAreaMax.x, fftAreaMax.y),
                                  text, style::lineWidth());
        // Y Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMin.y),
                                  ImVec2(fftAreaMin.x, fftAreaMax.y - 1),
                                  text, style::lineWidth());
    }

    void WaterFall::drawWaterfall() {
        if (waterfallUpdate) {
            waterfallUpdate = false;
            updateWaterfallTexture();
        }
        {
            std::lock_guard<std::mutex> lck(texMtx);
            window->DrawList->AddImage((void*)(intptr_t)textureId, wfMin, wfMax);
        }
        
        ImVec2 mPos = ImGui::GetMousePos();

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            IS_IN_AREA(mPos, wfMin, wfMax) && !gui::mainWindow.lockWaterfallControls && !inputHandled) {
            for (auto const& [name, vfo] : vfos) {
                window->DrawList->AddRectFilled(vfo->wfRectMin, vfo->wfRectMax, vfo->color);
                if (!vfo->lineVisible) { continue; }
                window->DrawList->AddLine(vfo->wfLineMin, vfo->wfLineMax, (name == selectedVFO) ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::lineWidth());
            }
        }
    }

    void WaterFall::drawVFOs() {
        for (auto const& [name, vfo] : vfos) {
            vfo->draw(window, name == selectedVFO);
        }
    }

    // Pan view / spectrum when dragging the frequency scale or when mouse wheeling or left / right keys
    // and the mouse hovers over the frequency scale.
    // viewDelta moves the view, not the trace: positive shifts the view up in frequency, which slides
    // the spectrum left on screen. Each caller negates as its input device requires, so that the
    // device convention ("dragging right pulls the trace right") stays visible at the call site.
    //FIXME respectCenterFrequencyLock parameter is questionable.
    // Maybe it shall be dropped and the lock shall be respected always?
    void WaterFall::panView(double viewDelta, bool respectCenterFrequencyLock) {
        // Drag handling calls this every frame while the button is held. Do not
        // report a center-frequency change until the pointer actually moves.
        if (viewDelta == 0.0) { return; }

        if (tuner::vfoLockedToCenter()) {
            // The VFO is synchronized with the IQ center frequency and the VFO is locked to the center of the view
            // or as close as possible to the center of the view considering the zoom factor.
            // See normalTuningLocked()
            // Tune the radio while maintaining the view offset and view bandwidth.
            //FIXME respectCenterFrequencyLock is ignored on purpose.
            centerFreq += viewDelta;
            centerFreqMoved = true;
        } else {
            viewOffset += viewDelta;
            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0) {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                if (!respectCenterFrequencyLock || !centerFrequencyLocked) {
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                if (!respectCenterFrequencyLock || !centerFrequencyLocked) {
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }
        }

        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

        // updateAllVFOs() and updateWaterfallFb() do nothing with tuner::vfoLockedToCenter(), but that may change in the future.
        if (! tuner::vfoLockedToCenter() && viewBandwidth != wholeBandwidth) {
            updateAllVFOs();
            if (_fullUpdate) { updateWaterfallFb(); };
        }
    }

    void WaterFall::selectFirstVFO() {
        // Only signal a change when the selection actually moved: this is called
        // on every teardown and on startup with no VFOs yet, and a spurious
        // selectedVFOChanged makes listeners re-run their VFO switch handling.
        std::string first = vfos.empty() ? "" : vfos.begin()->first;
        if (first == selectedVFO) { return; }
        selectedVFO = first;
        selectedVFOChanged = true;
    }

    void WaterFall::processInputs(bool fftResizePillVisible, const ImVec2& fftResizePillCenter) {
        // Pre calculate useful values
        WaterfallVFO* selVfo = NULL;
        if (selectedVFO != "") {
            selVfo = vfos[selectedVFO];
        }
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        ImVec2 dragOrigin(mousePos.x - drag.x, mousePos.y - drag.y);

        bool mouseHovered, mouseHeld;
        bool mouseClicked = ImGui::ButtonBehavior(ImRect(fftAreaMin, wfMax), GetID("WaterfallID"), &mouseHovered, &mouseHeld,
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_PressedOnClick);

        float splitterY = widgetPos.y + newFFTAreaHeight;
        bool mouseInFFTResizePill = false;
        mouseInFFTResize = false;
        if (waterfallVisible) {
            if (fftResizePillVisible) {
                // A 2 dp line cannot be hit reliably with a finger, so touch
                // style adds a visible pill that grabs immediately. This is a
                // visual/style choice independent of the platform-wide band
                // below: a touch-screen desktop needs the pill too.
                mouseInFFTResizePill = fabsf(dragOrigin.x - fftResizePillCenter.x) <= style::dp(24.0f) &&
                                       fabsf(dragOrigin.y - fftResizePillCenter.y) <= style::dp(18.0f);
            }

#ifdef __ANDROID__
            // Android always needs a finger-sized band, even if the optional
            // touch styling is disabled. Start it at the FFT area so it stays
            // clear of the menu splitter handle over the dB-scale strip.
            float separatorHitRadius = style::dp(12.0f);
            bool mouseInFFTResizeBand = dragOrigin.x > fftAreaMin.x && dragOrigin.x < fftAreaMax.x &&
                                        dragOrigin.y >= splitterY - separatorHitRadius && dragOrigin.y <= splitterY + separatorHitRadius;
#else
            // Desktop keeps a precise divider target. Touch style adds the pill
            // above instead of making this entire band steal nearby input.
            float separatorHitRadius = style::dp(2.0f);
            bool mouseInFFTResizeBand = dragOrigin.x > widgetPos.x && dragOrigin.x < widgetPos.x + widgetSize.x &&
                                        dragOrigin.y >= splitterY - separatorHitRadius && dragOrigin.y <= splitterY + separatorHitRadius;
#endif
            mouseInFFTResize = mouseInFFTResizePill || mouseInFFTResizeBand;
        }
        mouseInFreq = IS_IN_AREA(dragOrigin, freqAreaMin, freqAreaMax);
        mouseInFFT = IS_IN_AREA(dragOrigin, fftAreaMin, fftAreaMax);
        mouseInWaterfall = IS_IN_AREA(dragOrigin, wfMin, wfMax);

        float mouseWheel = ImGui::GetIO().MouseWheel;

        bool mouseMoved = false;
        if (mousePos.x != lastMousePos.x || mousePos.y != lastMousePos.y) { mouseMoved = true; }
        lastMousePos = mousePos;

        std::string hoveredVFOName;
        for (auto const& [name, _vfo] : vfos) {
            if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax)) {
                hoveredVFOName = name;
                break;
            }
        }
        // An initial press on empty spectrum or on the already-selected VFO
        // may tune immediately. A different VFO is handled as selection below.
        bool allowVfoClickMove = hoveredVFOName.empty() || selectedVFO == hoveredVFOName;

        // Deselect everything if the mouse is released
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (fftResizeSelect) {
                FFTAreaHeight = newFFTAreaHeight;
                onResize();
#ifdef __ANDROID__
                backend::hapticTick();
#endif
            }
#ifdef __ANDROID__
            if (fftResizePending) {
                // The touch was held back waiting for its drag direction and
                // turned out to be a plain tap — give it its original meaning:
                // select the VFO under the finger, or tune to the tap.
                fftResizePending = false;
                if (hoveredVFOName != "") {
                    selectedVFO = hoveredVFOName;
                    selectedVFOChanged = true;
                }
                else if (selVfo != NULL && (mouseInFFT || mouseInWaterfall)) {
                    int refCenter = mousePos.x - fftAreaMin.x;
                    if (refCenter >= 0 && refCenter < dataWidth) {
                        double off = ((((double)refCenter / ((double)dataWidth / 2.0)) - 1.0) * (viewBandwidth / 2.0)) + viewOffset;
                        off += centerFreq;
                        off = (round(off / selVfo->snapInterval) * selVfo->snapInterval) - centerFreq;
                        selVfo->setOffset(off);
                    }
                }
            }
#endif

            fftResizeSelect = false;
            freqScaleSelect = false;
            vfoBorderSelect = false;
            lastDrag = 0;
        }

        bool targetFound = false;

        // If the mouse was clicked anywhere in the waterfall, check if the resize was clicked
        if (mouseInFFTResize) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
#ifdef __ANDROID__
                if (!mouseInFFTResizePill) {
                    // Not on the pill — hold the Android touch back until its
                    // drag direction is known (resolved below). This remains a
                    // platform behavior when touch styling, and therefore the
                    // optional immediate-grab pill, is disabled.
                    fftResizePending = true;
                    fftResizePendingPos = mousePos;
                }
                else {
                    fftResizeSelect = true;
                    fftResizeGrabOffset = splitterY - mousePos.y;
                    backend::hapticTick();
                }
#else
                fftResizeSelect = true;
                fftResizeGrabOffset = splitterY - mousePos.y;
#endif
                targetFound = true;
            }
        }

#ifdef __ANDROID__
        // A touch near the divider is on hold until its direction is known:
        // across the divider = resize, along it = the gesture the band would
        // otherwise steal (freq-scale pan or VFO tuning).
        if (fftResizePending) {
            float dx = mousePos.x - fftResizePendingPos.x;
            float dy = mousePos.y - fftResizePendingPos.y;
            if (std::max(fabsf(dx), fabsf(dy)) < style::dp(6.0f)) {
                return; // still undecided — swallow the touch for now
            }
            fftResizePending = false;
            if (fabsf(dy) > fabsf(dx)) {
                fftResizeSelect = true;
                fftResizeGrabOffset = splitterY - fftResizePendingPos.y;
                backend::hapticTick();
            }
            else if (mouseInFreq) {
                freqScaleSelect = true;
            }
            // else: the touch started in the waterfall part of the band; the
            // VFO-move branch below keys off the same drag origin and takes over.
        }
#endif

        // If mouse was clicked inside the central part, check what was clicked
        if (mouseClicked && !targetFound) {
            mouseDownPos = mousePos;

            // First, check if a VFO border was selected
            for (auto const& [name, _vfo] : vfos) {
                if (_vfo->bandwidthLocked) { continue; }
                if (_vfo->rectMax.x - _vfo->rectMin.x < 10) { continue; }
                bool resizing = false;
                if (_vfo->reference != REF_LOWER) {
                    if (IS_IN_AREA(mousePos, _vfo->lbwSelMin, _vfo->lbwSelMax)) { resizing = true; }
                    else if (IS_IN_AREA(mousePos, _vfo->wfLbwSelMin, _vfo->wfLbwSelMax)) {
                        resizing = true;
                    }
                }
                if (_vfo->reference != REF_UPPER) {
                    if (IS_IN_AREA(mousePos, _vfo->rbwSelMin, _vfo->rbwSelMax)) { resizing = true; }
                    else if (IS_IN_AREA(mousePos, _vfo->wfRbwSelMin, _vfo->wfRbwSelMax)) {
                        resizing = true;
                    }
                }
                if (!resizing) { continue; }
                relatedVfo = _vfo;
                vfoBorderSelect = true;
                targetFound = true;
                break;
            }

            // Next, check if a VFO was selected
            if (!targetFound && !hoveredVFOName.empty()) {
                if (selectedVFO != hoveredVFOName) {
                    selectedVFO = hoveredVFOName;
                    selectedVFOChanged = true;
                    // Selecting and moving are separate gestures. Latch this
                    // press until its release so motion from a finger (or a
                    // slightly moving mouse) cannot immediately drag the VFO.
                    vfoSelect = true;
                    // Clear hit state for the rest of this frame so the
                    // main-window wheel and arrow handlers stay inactive too.
                    mouseInFFTResize = false;
                    mouseInFreq = false;
                    mouseInFFT = false;
                    mouseInWaterfall = false;
                    targetFound = true;
                    return;
                }
            }

            // Now, check frequency scale
            if (!targetFound && mouseInFreq) {
                freqScaleSelect = true;
            }
        }

        // If the FFT resize bar was selected, resize FFT accordingly
        if (fftResizeSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            newFFTAreaHeight = mousePos.y + fftResizeGrabOffset - widgetPos.y;
            newFFTAreaHeight = style::clampSplit(newFFTAreaHeight, widgetSize.y, 150.0f, 50.0f);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(widgetPos.x, newFFTAreaHeight + widgetPos.y), ImVec2(widgetEndPos.x, newFFTAreaHeight + widgetPos.y),
                                                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), style::lineWidth());
            return;
        }

        // If a vfo border is selected, resize VFO accordingly
        if (vfoBorderSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double dist = (relatedVfo->reference == REF_CENTER) ? fabsf(mousePos.x - relatedVfo->lineMin.x) : (mousePos.x - relatedVfo->lineMin.x);
            if (relatedVfo->reference == REF_UPPER) { dist = -dist; }
            double hzDist = dist * (viewBandwidth / (double)dataWidth);
            if (relatedVfo->reference == REF_CENTER) {
                hzDist *= 2.0;
            }
            hzDist = std::clamp<double>(hzDist, relatedVfo->minBandwidth, relatedVfo->maxBandwidth);
            relatedVfo->setBandwidth(hzDist);
            relatedVfo->onUserChangedBandwidth.emit(hzDist);
            return;
        }

        // If the frequency scale is selected, move it
        if (freqScaleSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double deltax = drag.x - lastDrag;
            lastDrag = drag.x;
            // Negated: dragging right must pull the trace right with the pointer, i.e. move the view down.
            double viewDelta = -deltax * (viewBandwidth / (double)dataWidth);
            panView(viewDelta, true);
            return;
        }

        // Pinch-to-zoom from the Android gesture recognizer arrives on the
        // horizontal wheel axis (MouseWheelH) to avoid Ctrl key-state timing
        // issues.  Desktop Ctrl+scroll also zooms via the vertical axis.
        // Positive value = fingers apart / scroll up = zoom in (narrower view).
        float zoomWheel = 0.0f;
#ifdef __ANDROID__
        float mouseWheelH = ImGui::GetIO().MouseWheelH;
        if (mouseWheelH != 0.0f && (mouseInFFT || mouseInWaterfall || mouseInFreq))
            zoomWheel = mouseWheelH;
        else
#endif
        if (mouseWheel != 0.0f && ImGui::GetIO().KeyCtrl && (mouseInFFT || mouseInWaterfall || mouseInFreq))
            zoomWheel = mouseWheel;
        if (zoomWheel != 0.0f) {
            double newBw = viewBandwidth * std::pow(0.9, (double)zoomWheel);
            newBw = std::clamp(newBw, wholeBandwidth / 200.0, wholeBandwidth);
            // Anchor the zoom on the frequency under the cursor — the first
            // (tuning) finger when pinching on touch — so that frequency keeps
            // its screen position while the view stretches around it.
            double anchorX = (double)mousePos.x - fftAreaMin.x;
            if (newBw != viewBandwidth && anchorX >= 0.0 && anchorX < (double)dataWidth) {
                double norm = (anchorX / ((double)dataWidth / 2.0)) - 1.0;    // -1..1 across the view
                double anchorOff = viewOffset + (norm * viewBandwidth / 2.0); // Hz relative to center freq
                viewOffset = anchorOff - (norm * newBw / 2.0);                // setViewBandwidth clamps to band edges
            }
            setViewBandwidth(newBw);
            return;
        }

        // If the mouse wheel is moved on the frequency scale
        if (mouseWheel != 0.0f && mouseInFreq) {
            // Negated to match dragging: scrolling up slides the trace right, i.e. moves the view down.
            double viewDelta = -(double)mouseWheel * viewBandwidth / 20.0;
            panView(viewDelta);
            return;
        }

        // If the left and right keys are pressed while hovering the freq scale, move it too
        bool leftKeyPressed = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        if ((leftKeyPressed || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && mouseInFreq) {
            double viewDelta = leftKeyPressed ? (viewBandwidth / 20.0) : (-viewBandwidth / 20.0);
            panView(viewDelta);
            return;
        }

        // Finally, if nothing else was selected, just move the VFO
        if ((VFOMoveSingleClick ? ImGui::IsMouseClicked(ImGuiMouseButton_Left) : ImGui::IsMouseDown(ImGuiMouseButton_Left)) && (mouseInFFT | mouseInWaterfall) && (mouseMoved || allowVfoClickMove)) {
            if (selVfo != NULL) {
                int refCenter = mousePos.x - fftAreaMin.x;
                if (refCenter >= 0 && refCenter < dataWidth) {
                    double off = ((((double)refCenter / ((double)dataWidth / 2.0)) - 1.0) * (viewBandwidth / 2.0)) + viewOffset;
                    off += centerFreq;
                    off = (round(off / selVfo->snapInterval) * selVfo->snapInterval) - centerFreq;
                    selVfo->setOffset(off);
                }
            }
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Check if a VFO is hovered. If yes, show tooltip
            for (auto const& [name, _vfo] : vfos) {
                if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax)) {
                    char buf[128];
                    ImGui::BeginTooltip();

                    ImGui::TextUnformatted(name.c_str());

                    if (ImGui::GetIO().KeyCtrl) {
                        ImGui::Separator();
                        printAndScale(_vfo->generalOffset + centerFreq, buf);
                        ImGui::Text("Frequency: %sHz", buf);
                        printAndScale(_vfo->bandwidth, buf);
                        ImGui::Text("Bandwidth: %sHz", buf);
                        ImGui::Text("Bandwidth Locked: %s", _vfo->bandwidthLocked ? "Yes" : "No");

                        // Hold buf_mtx while reading rawFFTs: this runs on the UI
                        // thread, and without the lock the FFT worker (getFFTBuffer/
                        // pushFFT) or onResize() can move currentFFTLine / reallocate
                        // rawFFTs underneath us and produce an out-of-bounds read.
                        float strength, snr;
                        bool infoValid;
                        {
                            std::lock_guard<std::recursive_mutex> lck(buf_mtx);
                            infoValid = calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, _vfo, strength, snr);
                        }
                        if (infoValid) {
                            ImGui::Text("Strength: %0.1fdBFS", strength);
                            ImGui::Text("SNR: %0.1fdB", snr);
                        }
                        else {
                            ImGui::TextUnformatted("Strength: ---.-dBFS");
                            ImGui::TextUnformatted("SNR: ---.-dB");
                        }
                    }

                    ImGui::EndTooltip();
                    break;
                }
            }
        }

        // Handle Page Up to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && selVfo != NULL) {
            std::string next = (--vfos.end())->first;
            std::string lowest = "";
            double lowestOffset = INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto& [_name, _vfo] : vfos) {
                if (_vfo->generalOffset > firstVfoOffset && (_vfo->generalOffset - firstVfoOffset) < smallestDistance) {
                    next = _name;
                    smallestDistance = (_vfo->generalOffset - firstVfoOffset);
                    found = true;
                }
                if (_vfo->generalOffset < lowestOffset) {
                    lowestOffset = _vfo->generalOffset;
                    lowest = _name;
                }
            }
            selectedVFO = found ? next : lowest;
            selectedVFOChanged = true;
        }

        // Handle Page Down to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && selVfo != NULL) {
            std::string next = (--vfos.end())->first;
            std::string highest = "";
            double highestOffset = -INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto& [_name, _vfo] : vfos) {
                if (_vfo->generalOffset < firstVfoOffset && (firstVfoOffset - _vfo->generalOffset) < smallestDistance) {
                    next = _name;
                    smallestDistance = (firstVfoOffset - _vfo->generalOffset);
                    found = true;
                }
                if (_vfo->generalOffset > highestOffset) {
                    highestOffset = _vfo->generalOffset;
                    highest = _name;
                }
            }
            selectedVFO = found ? next : highest;
            selectedVFOChanged = true;
        }
    }

    bool WaterFall::calculateVFOSignalInfo(float* fftLine, WaterfallVFO* _vfo, float& strength, float& snr) {
        if (fftLine == NULL || fftLines <= 0) { return false; }

        // Calculate FFT index data
        double vfoMinSizeFreq = _vfo->centerOffset - _vfo->bandwidth;
        double vfoMinFreq = _vfo->centerOffset - (_vfo->bandwidth / 2.0);
        double vfoMaxFreq = _vfo->centerOffset + (_vfo->bandwidth / 2.0);
        double vfoMaxSizeFreq = _vfo->centerOffset + _vfo->bandwidth;
        int vfoMinSideOffset = std::clamp<int>(((vfoMinSizeFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);
        // Clamped to rawFFTSize - 1: both are used as inclusive indices below,
        // unlike the side offsets which are only exclusive loop bounds.
        int vfoMinOffset = std::clamp<int>(((vfoMinFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize - 1);
        int vfoMaxOffset = std::clamp<int>(((vfoMaxFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize - 1);
        int vfoMaxSideOffset = std::clamp<int>(((vfoMaxSizeFreq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);

        double avg = 0;
        float max = -INFINITY;
        int avgCount = 0;

        // Calculate Left average
        for (int i = vfoMinSideOffset; i < vfoMinOffset; i++) {
            avg += fftLine[i];
            avgCount++;
        }

        // Calculate Right average
        for (int i = vfoMaxOffset + 1; i < vfoMaxSideOffset; i++) {
            avg += fftLine[i];
            avgCount++;
        }

        // A VFO spanning the whole sampled band (or clamped against both FFT
        // edges) leaves no out-of-band bins, so there is no noise reference and
        // SNR is undefined. Report no info rather than dividing by zero: a NaN
        // here poisons the caller's rolling noise window for LEVEL_HOLD_FRAMES.
        if (avgCount == 0) { return false; }
        avg /= (double)(avgCount);

        // Calculate max
        for (int i = vfoMinOffset; i <= vfoMaxOffset; i++) {
            if (fftLine[i] > max) { max = fftLine[i]; }
        }

        strength = max;
        snr = max - avg;

        return true;
    }

    void WaterFall::updateWaterfallFb() {
        if (!waterfallVisible || rawFFTs == NULL) {
            return;
        }
        double offsetRatio = viewOffset / (wholeBandwidth / 2.0);
        int drawDataSize;
        int drawDataStart;
        // TODO: Maybe put on the stack for faster alloc?
        float* tempData = new float[dataWidth];
        float pixel;
        float dataRange = waterfallMax - waterfallMin;
        int count = std::min<float>(waterfallHeight, fftLines);
        if (rawFFTs != NULL && fftLines >= 0) {
            for (int i = 0; i < count; i++) {
                drawDataSize = (viewBandwidth / wholeBandwidth) * rawFFTSize;
                drawDataStart = (((double)rawFFTSize / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);
                doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, &rawFFTs[((i + currentFFTLine) % waterfallHeight) * rawFFTSize], tempData);
                for (int j = 0; j < dataWidth; j++) {
                    pixel = (std::clamp<float>(tempData[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;
                    waterfallFb[(i * dataWidth) + j] = waterfallPallet[(int)(pixel * (WATERFALL_RESOLUTION - 1))];
                }
            }

            for (int i = count; i < waterfallHeight; i++) {
                for (int j = 0; j < dataWidth; j++) {
                    waterfallFb[(i * dataWidth) + j] = (uint32_t)255 << 24;
                }
            }
        }
        delete[] tempData;
        waterfallUpdate = true;
    }

    void WaterFall::drawBandPlan() {
        int count = bandplan->bands.size();
        double horizScale = (double)dataWidth / viewBandwidth;
        double start, end, center, aPos, bPos, cPos, width;
        ImVec2 txtSz;
        bool startVis, endVis;
        uint32_t color, colorTrans;

        float height = ImGui::CalcTextSize("0").y * 2.5f;
        float bpBottom;

        if (bandPlanPos == BANDPLAN_POS_BOTTOM) {
            bpBottom = fftAreaMax.y;
        }
        else {
            bpBottom = fftAreaMin.y + height + 1;
        }


        for (int i = 0; i < count; i++) {
            start = bandplan->bands[i].start;
            end = bandplan->bands[i].end;
            if (start < lowerFreq && end < lowerFreq) {
                continue;
            }
            if (start > upperFreq && end > upperFreq) {
                continue;
            }
            startVis = (start > lowerFreq);
            endVis = (end < upperFreq);
            start = std::clamp<double>(start, lowerFreq, upperFreq);
            end = std::clamp<double>(end, lowerFreq, upperFreq);
            center = (start + end) / 2.0;
            aPos = fftAreaMin.x + ((start - lowerFreq) * horizScale);
            bPos = fftAreaMin.x + ((end - lowerFreq) * horizScale);
            cPos = fftAreaMin.x + ((center - lowerFreq) * horizScale);
            width = bPos - aPos;
            txtSz = ImGui::CalcTextSize(bandplan->bands[i].name.c_str());
            if (bandplan::colorTable.find(bandplan->bands[i].type.c_str()) != bandplan::colorTable.end()) {
                color = bandplan::colorTable[bandplan->bands[i].type].colorValue;
                colorTrans = bandplan::colorTable[bandplan->bands[i].type].transColorValue;
            }
            else {
                color = IM_COL32(255, 255, 255, 255);
                colorTrans = IM_COL32(255, 255, 255, 100);
            }
            if (aPos <= fftAreaMin.x) {
                aPos = fftAreaMin.x + 1;
            }
            if (bPos <= fftAreaMin.x) {
                bPos = fftAreaMin.x + 1;
            }
            if (width >= 1.0) {
                window->DrawList->AddRectFilled(ImVec2(roundf(aPos), bpBottom - height),
                                                ImVec2(roundf(bPos), bpBottom), colorTrans);
                if (startVis) {
                    window->DrawList->AddLine(ImVec2(roundf(aPos), bpBottom - height - 1),
                                              ImVec2(roundf(aPos), bpBottom - 1), color, style::lineWidth());
                }
                if (endVis) {
                    window->DrawList->AddLine(ImVec2(roundf(bPos), bpBottom - height - 1),
                                              ImVec2(roundf(bPos), bpBottom - 1), color, style::lineWidth());
                }
            }
            if (txtSz.x <= width) {
                window->DrawList->AddText(ImVec2(cPos - (txtSz.x / 2.0), bpBottom - (height / 2.0f) - (txtSz.y / 2.0f)),
                                          IM_COL32(255, 255, 255, 255), bandplan->bands[i].name.c_str());
            }
        }
    }

    void WaterFall::updateWaterfallTexture() {
        std::lock_guard<std::mutex> lck(texMtx);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dataWidth, waterfallHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t*)waterfallFb);
    }

    void WaterFall::onPositionChange() {
        // Nothing to see here...
    }

    void WaterFall::onResize() {
        // buf_mtx must be held first: it guards rawFFTs, currentFFTLine and
        // waterfallHeight, which the FFT worker reads/writes in getFFTBuffer()/
        // pushFFT(). onResize() reallocates rawFFTs and rewrites those fields, so
        // without this lock the worker can index the old (smaller) allocation with
        // a currentFFTLine sized for the new waterfallHeight and read out of bounds
        // (e.g. on SpyServer connect, when FFT frames arrive mid-relayout). It is a
        // recursive_mutex and the other onResize() call sites already hold it, so
        // re-locking there is a no-op. Lock order: buf_mtx -> latestFFTMtx -> smoothingBufMtx.
        std::lock_guard<std::recursive_mutex> lck0(buf_mtx);
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        std::lock_guard<std::mutex> lck2(smoothingBufMtx);
        // return if widget is too small
        if (widgetSize.x < style::dp(100.0f) || widgetSize.y < style::dp(100.0f)) {
            return;
        }

        int lastWaterfallHeight = waterfallHeight;

        if (waterfallVisible) {
            int effectiveFFTAreaHeight = style::clampSplit(FFTAreaHeight, widgetSize.y, 150.0f, 50.0f);
            newFFTAreaHeight = effectiveFFTAreaHeight;
            fftHeight = effectiveFFTAreaHeight - style::scale(50.0f);
            waterfallHeight = widgetSize.y - fftHeight - style::scale(50.0f) - 2;
        }
        else {
            fftHeight = widgetSize.y - style::scale(50.0f);
        }
        dataWidth = widgetSize.x - style::scale(60.0f);

        if (waterfallVisible) {
            // Raw FFT resize
            fftLines = std::min<int>(fftLines, waterfallHeight) - 1;
            if (rawFFTs != NULL) {
                if (currentFFTLine != 0) {
                    float* tempWF = new float[currentFFTLine * rawFFTSize];
                    int moveCount = lastWaterfallHeight - currentFFTLine;
                    memcpy(tempWF, rawFFTs, currentFFTLine * rawFFTSize * sizeof(float));
                    memmove(rawFFTs, &rawFFTs[currentFFTLine * rawFFTSize], moveCount * rawFFTSize * sizeof(float));
                    memcpy(&rawFFTs[moveCount * rawFFTSize], tempWF, currentFFTLine * rawFFTSize * sizeof(float));
                    delete[] tempWF;
                }
                currentFFTLine = 0;
                rawFFTs = (float*)realloc(rawFFTs, waterfallHeight * rawFFTSize * sizeof(float));
            }
            else {
                rawFFTs = (float*)malloc(waterfallHeight * rawFFTSize * sizeof(float));
            }
            // ==============
        }

        // Reallocate display FFT
        if (latestFFT != NULL) {
            delete[] latestFFT;
        }
        latestFFT = new float[dataWidth];

        // Reallocate hold FFT
        if (latestFFTHold != NULL) {
            delete[] latestFFTHold;
        }
        latestFFTHold = new float[dataWidth];

        // Reallocate smoothing buffer
        if (fftSmoothing) {
            if (smoothingBuf) { delete[] smoothingBuf; }
            smoothingBuf = new float[dataWidth];
            for (int i = 0; i < dataWidth; i++) {
                smoothingBuf[i] = -1000.0f; 
            }
        }

        if (waterfallVisible) {
            delete[] waterfallFb;
            waterfallFb = new uint32_t[dataWidth * waterfallHeight];
            memset(waterfallFb, 0, dataWidth * waterfallHeight * sizeof(uint32_t));
        }
        for (int i = 0; i < dataWidth; i++) {
            latestFFT[i] = -1000.0f; // Hide everything
            latestFFTHold[i] = -1000.0f;
        }

        // Whole-pixel area anchors: everything below (grid, trace, axes, the
        // waterfall texture) is placed relative to these.
        fftAreaMin = ImVec2(widgetPos.x + (float)style::scale(50.0f), widgetPos.y + (float)style::scale(9.0f));
        fftAreaMax = ImVec2(fftAreaMin.x + dataWidth, fftAreaMin.y + fftHeight + 1);

        freqAreaMin = ImVec2(fftAreaMin.x, fftAreaMax.y + 1);
        freqAreaMax = ImVec2(fftAreaMax.x, fftAreaMax.y + (float)style::scale(40.0f));

        wfMin = ImVec2(fftAreaMin.x, freqAreaMax.y + 1);
        wfMax = ImVec2(fftAreaMin.x + dataWidth, wfMin.y + waterfallHeight);

        // Use style::baseFont directly — GImGui->Font is stale when onResize() is called
        // from setFFTHeight() during a scale change (before NewFrame() restores it).
        {
            float tw = style::baseFont->CalcTextSizeA(style::baseFont->FontSize, FLT_MAX, 0.0f, "000.000").x;
            float th = style::baseFont->CalcTextSizeA(style::baseFont->FontSize, FLT_MAX, 0.0f, "000.000").y;
            maxHSteps = dataWidth / (tw + style::dp(10.0f));
            maxVSteps = fftHeight / th;
        }

        range = findBestRange(viewBandwidth, maxHSteps);
        vRange = findBestRange(fftMax - fftMin, maxVSteps);

        updateWaterfallFb();
        updateAllVFOs();
    }

    void WaterFall::draw() {
        buf_mtx.lock();
        window = GetCurrentWindow();

        widgetPos = ImGui::GetWindowContentRegionMin();
        widgetEndPos = ImGui::GetWindowContentRegionMax();
        widgetPos.x += window->Pos.x;
        widgetPos.y += window->Pos.y;
        widgetEndPos.x += window->Pos.x - 4; // Padding
        widgetEndPos.y += window->Pos.y;
        // Snap to whole pixels: a fractional origin would put every derived
        // line, the FFT trace and the waterfall texture on half-pixel
        // boundaries, blurring them at fractional UI scales.
        widgetPos = ImVec2(floorf(widgetPos.x), floorf(widgetPos.y));
        widgetEndPos = ImVec2(floorf(widgetEndPos.x), floorf(widgetEndPos.y));
        widgetSize = ImVec2(widgetEndPos.x - widgetPos.x, widgetEndPos.y - widgetPos.y);

        if (selectedVFO == "" && vfos.size() > 0) {
            selectFirstVFO();
        }

        if (widgetPos.x != lastWidgetPos.x || widgetPos.y != lastWidgetPos.y) {
            lastWidgetPos = widgetPos;
            onPositionChange();
        }
        // Force a layout recompute on scale changes: onContentScaleChanged()
        // triggers setFFTHeight() -> onResize() pre-frame with stale widgetPos,
        // and draw()'s size check can miss transitions where the widget pixel
        // size happens to stay the same across a scale change.
        uint64_t currentScaleEpoch = style::scaleEpoch();
        if (widgetSize.x != lastWidgetSize.x || widgetSize.y != lastWidgetSize.y || currentScaleEpoch != lastScaleEpoch) {
            lastWidgetSize = widgetSize;
            lastScaleEpoch = currentScaleEpoch;
            onResize();
        }

        // Compute the pill's visibility and position once so drawing and input
        // handling cannot drift apart.
        bool fftResizePillVisible = style::touchStyle && waterfallVisible;
        ImVec2 fftResizePillCenter;
        if (fftResizePillVisible) {
            fftResizePillCenter = ImVec2(fftAreaMin.x + (float)dataWidth * 0.75f,
                                        widgetPos.y + newFFTAreaHeight);
        }

        //window->DrawList->AddRectFilled(widgetPos, widgetEndPos, IM_COL32( 0, 0, 0, 255 ));
        ImU32 bg = ImGui::ColorConvertFloat4ToU32(gui::themeManager.waterfallBg);
        window->DrawList->AddRectFilled(widgetPos, widgetEndPos, bg);
        window->DrawList->AddRect(widgetPos, widgetEndPos, IM_COL32(50, 50, 50, 255), 0.0, 0, style::lineWidth());
        window->DrawList->AddLine(ImVec2(widgetPos.x, freqAreaMax.y), ImVec2(widgetPos.x + widgetSize.x, freqAreaMax.y), IM_COL32(50, 50, 50, 255), style::lineWidth());

        // A first press on an unselected VFO only changes the selection. Keep
        // the whole waterfall inert until that press is released, including
        // overlay input handlers and the wheel/key handling in main_window.
        // Do this outside the hover test so a release that happens after the
        // pointer leaves the waterfall still terminates the gesture.
        if (vfoSelect && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            vfoSelect = false;
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !gui::mainWindow.lockWaterfallControls && !vfoSelect)
        {
            inputHandled = false;
            InputHandlerArgs args;
            args.fftRectMin = fftAreaMin;
            args.fftRectMax = fftAreaMax;
            args.freqScaleRectMin = freqAreaMin;
            args.freqScaleRectMax = freqAreaMax;
            args.waterfallRectMin = wfMin;
            args.waterfallRectMax = wfMax;
            args.lowFreq = lowerFreq;
            args.highFreq = upperFreq;
            args.freqToPixelRatio = (double)dataWidth / viewBandwidth;
            args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
            onInputProcess.emit(args);
            if (!inputHandled) { processInputs(fftResizePillVisible, fftResizePillCenter); }
        }
        else {
            // These are only recomputed inside processInputs(), which just got
            // skipped. Without this reset they keep whatever value the last
            // hovered frame left behind (a fast mouse exit can leave them
            // true), and main_window's wheel/arrow-key tuning keeps firing
            // while the cursor is over the menu or another window.
            mouseInFFTResize = false;
            mouseInFreq = false;
            mouseInFFT = false;
            mouseInWaterfall = false;
            fftResizePending = false;
        }

        updateAllVFOs(true);

        drawFFT();
        if (waterfallVisible) {
            drawWaterfall();
        }
        drawVFOs();
        if (bandplan != NULL && bandplanVisible) {
            drawBandPlan();
        }

        // FFT/waterfall divider drag handle: the fat touch target that grabs on
        // touch-down (see processInputs). Drawn last so it sits on top of the
        // freq scale and the waterfall; the dark backing keeps it readable.
        if (fftResizePillVisible) {
            float halfW = style::dp(16.0f) + (fftResizeSelect ? style::dp(4.0f) : 0.0f);
            float halfH = style::dp(fftResizeSelect ? 6.0f : 4.5f);
            float pad = style::dp(2.0f);
            window->DrawList->AddRectFilled(ImVec2(fftResizePillCenter.x - halfW - pad, fftResizePillCenter.y - halfH - pad),
                                            ImVec2(fftResizePillCenter.x + halfW + pad, fftResizePillCenter.y + halfH + pad),
                                            IM_COL32(0, 0, 0, 120), halfH + pad);
            window->DrawList->AddRectFilled(ImVec2(fftResizePillCenter.x - halfW, fftResizePillCenter.y - halfH),
                                            ImVec2(fftResizePillCenter.x + halfW, fftResizePillCenter.y + halfH),
                                            fftResizeSelect ? ImGui::GetColorU32(ImGuiCol_SeparatorActive) : IM_COL32(210, 210, 210, 200), halfH);
        }

        if (!waterfallVisible) {
            buf_mtx.unlock();
            return;
        }

        buf_mtx.unlock();
    }

    float* WaterFall::getFFTBuffer() {
        if (rawFFTs == NULL) { return NULL; }
        buf_mtx.lock();
        if (waterfallVisible) {
            currentFFTLine--;
            fftLines++;
            currentFFTLine = ((currentFFTLine + waterfallHeight) % waterfallHeight);
            fftLines = std::min<float>(fftLines, waterfallHeight);
            return &rawFFTs[currentFFTLine * rawFFTSize];
        }
        return rawFFTs;
    }

    void WaterFall::pushFFT() {
        if (rawFFTs == NULL) { return; }
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        double offsetRatio = viewOffset / (wholeBandwidth / 2.0);
        int drawDataSize = (viewBandwidth / wholeBandwidth) * rawFFTSize;
        int drawDataStart = (((double)rawFFTSize / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);

        if (waterfallVisible) {
            doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, &rawFFTs[currentFFTLine * rawFFTSize], latestFFT);
            memmove(&waterfallFb[dataWidth], waterfallFb, dataWidth * (waterfallHeight - 1) * sizeof(uint32_t));
            float pixel;
            float dataRange = waterfallMax - waterfallMin;
            for (int j = 0; j < dataWidth; j++) {
                pixel = (std::clamp<float>(latestFFT[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;
                int id = (int)(pixel * (WATERFALL_RESOLUTION - 1));
                waterfallFb[j] = waterfallPallet[id];
            }
            waterfallUpdate = true;
        }
        else {
            doZoom(drawDataStart, drawDataSize, rawFFTSize, dataWidth, rawFFTs, latestFFT);
            fftLines = 1;
        }

        // Apply smoothing if enabled
        if (fftSmoothing && latestFFT != NULL && smoothingBuf != NULL && fftLines != 0) {
            std::lock_guard<std::mutex> lck2(smoothingBufMtx);
            volk_32f_s32f_multiply_32f(latestFFT, latestFFT, fftSmoothingAlpha, dataWidth);
            volk_32f_s32f_multiply_32f(smoothingBuf, smoothingBuf, fftSmoothingBeta, dataWidth);
            volk_32f_x2_add_32f(smoothingBuf, latestFFT, smoothingBuf, dataWidth);
            memcpy(latestFFT, smoothingBuf, dataWidth * sizeof(float));
        }

        if (selectedVFO != "" && vfos.size() > 0) {
            // Reset the peak hold when the VFO selection changes, so the new
            // VFO doesn't inherit the previous VFO's peak
            if (selectedVFO != levelHistoryVFO) {
                levelHistoryVFO = selectedVFO;
                levelHistoryPos = 0;
                levelHistoryCount = 0;
            }

            float newLevel = -INFINITY;
            float newSNR = NAN;
            bool infoValid = calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, vfos[selectedVFO], newLevel, newSNR);

            if (infoValid && std::isfinite(newLevel)) {
                selectedVFOLevel = newLevel;

                // Rolling window over the last LEVEL_HOLD_FRAMES FFT frames: hold
                // the peak in-band level, average the out-of-VFO-subband noise
                // floor over the same window, and report SNR as the held peak
                // over that averaged noise (dB difference = linear ratio).
                float newNoise = newLevel - newSNR; // out-of-band average (dB)
                levelHistory[levelHistoryPos] = newLevel;
                noiseHistory[levelHistoryPos] = newNoise;
                levelHistoryPos = (levelHistoryPos + 1) % LEVEL_HOLD_FRAMES;
                levelHistoryCount = std::min<int>(levelHistoryCount + 1, LEVEL_HOLD_FRAMES);
                float maxLevel = -INFINITY;
                float noiseSum = 0.0f;
                for (int i = 0; i < levelHistoryCount; i++) {
                    maxLevel = std::max<float>(maxLevel, levelHistory[i]);
                    noiseSum += noiseHistory[i];
                }
                selectedVFOLevelMax = maxLevel;
                selectedVFOSNR = maxLevel - (noiseSum / (float)levelHistoryCount);
            }
            else {
                // No usable FFT data: blank the meter instead of showing stale
                // values, and drop the history so values don't merge across a gap
                levelHistoryPos = 0;
                levelHistoryCount = 0;
                selectedVFOLevel = -INFINITY;
                selectedVFOLevelMax = -INFINITY;
                selectedVFOSNR = NAN;
            }
        }

        // If FFT hold is enabled, update it
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                latestFFTHold[i] = std::max<float>(latestFFT[i], latestFFTHold[i] - fftHoldSpeed);
            }
        }

        buf_mtx.unlock();
    }

    void WaterFall::updatePallette(float colors[][3], int colorCount) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++) {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[lowerId][0] * (1.0 - ratio)) + (colors[upperId][0] * (ratio));
            float g = (colors[lowerId][1] * (1.0 - ratio)) + (colors[upperId][1] * (ratio));
            float b = (colors[lowerId][2] * (1.0 - ratio)) + (colors[upperId][2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    void WaterFall::updatePalletteFromArray(const float* colors, int colorCount) {
        // A color map that failed to load is empty; keep the current palette
        // instead of indexing a null array (and clamping to an empty range).
        if (!colors || colorCount <= 0) {
            flog::error("Ignoring an empty waterfall color map");
            return;
        }
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++) {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[(lowerId * 3) + 0] * (1.0 - ratio)) + (colors[(upperId * 3) + 0] * (ratio));
            float g = (colors[(lowerId * 3) + 1] * (1.0 - ratio)) + (colors[(upperId * 3) + 1] * (ratio));
            float b = (colors[(lowerId * 3) + 2] * (1.0 - ratio)) + (colors[(upperId * 3) + 2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    // Collect the raw FFT bins of the currently-visible span (current zoom/pan)
    // into `out`, excluding the device band edges and the DC/LO notch, so the
    // range estimators see only the meaningful noise+signal population.
    //
    // Estimating from the RAW line (not the max-downsampled display FFT) avoids
    // the upward bias that max-selection downsampling introduces when zoomed
    // out. Coordinates: rawFFTs is DC-centered (a (-1)^n factor is folded into
    // the FFT window, see iq_frontend.cpp), so DC is at N/2 and the +-Fs/2 band
    // edges are at 0 and N. Both exclusion zones are device-absolute and then
    // intersected with the visible window, so a zoom into a clean interior
    // slice keeps all of it, while a full-zoom-out drops the roll-off edges and
    // the center spike (matching the SDR Console / gqrx heuristics).
    //
    // Pooling several lines gives the one-shot a steadier estimate and rejects
    // a transient whole-band burst at the click instant: the burst's elevated
    // bins sort into the upper part of the pool, so a low quantile stays on the
    // good lines. Takes buf_mtx, which guards rawFFTs / currentFFTLine /
    // geometry (documented lock order buf_mtx -> latestFFTMtx, so this outermost
    // lock is safe and lets us read the newest line without racing the DSP
    // write). Returns false if too few bins survive.
    bool WaterFall::collectAutorangeBins(std::vector<float>& out, int lines) {
        // The band-edge and DC/LO exclusions are contiguous index ranges, not
        // per-bin predicates, so resolve them to at most two surviving segments.
        // All the boundary math is derived here, before taking buf_mtx: it
        // depends only on the view geometry (GUI-thread-owned, so stable in this
        // GUI-thread call) and rawFFTSize. The lock then holds for essentially
        // just the memcpy.
        //
        // rawFFTSize is the one caveat: the DSP thread (iq_frontend) reallocates
        // rawFFTs to a new rawFFTSize under buf_mtx. We snapshot it as N here and
        // re-check it under the lock before copying -- if it changed, the offsets
        // no longer match the buffer, so we bail (the caller keeps the previous
        // range; the next tick reacquires). Every offset below is clamped into
        // [0, N], so a stale N can only cause a bail, never an out-of-bounds copy.
        int N = rawFFTSize;
        if (N <= 0 || wholeBandwidth <= 0.0) { return false; }

        // Visible span in raw-bin coordinates (identical to pushFFT).
        double offsetRatio = viewOffset / (wholeBandwidth / 2.0);
        int drawDataSize = (int)((viewBandwidth / wholeBandwidth) * N);
        int drawDataStart = (int)(((double)N / 2.0) * (offsetRatio + 1)) - (drawDataSize / 2);
        int visStart = std::clamp<int>(drawDataStart, 0, N);
        int visEnd = std::clamp<int>(drawDataStart + drawDataSize, 0, N);
        if (visStart >= visEnd) { return false; }

        // Device band-edge zones (anti-alias roll-off): 15% per side. Keep
        // [edgeLo, edgeHi); intersect with the visible window to get [a, b).
        const double EDGE_FRACTION = 0.15;
        int edgeLo = (int)(EDGE_FRACTION * N);  // [0, edgeLo)  = -Fs/2 edge
        int edgeHi = N - edgeLo;                // [edgeHi, N)  = +Fs/2 edge
        int a = std::max(visStart, edgeLo);
        int b = std::min(visEnd, edgeHi);

        // DC/LO notch at center (N/2): +-250 Hz, capped at 5% of the visible
        // span so it can't dominate a narrowband source or an extreme zoom.
        const double CENTER_NOTCH_BANDWIDTH = 250.0; // Hz, half-width
        double hzPerBin = wholeBandwidth / (double)N;
        double notchHz = std::min(CENTER_NOTCH_BANDWIDTH, 0.05 * viewBandwidth);
        int notchHalf = (int)(notchHz / hzPerBin);
        int dcLo = (N / 2) - notchHalf;
        int dcHi = (N / 2) + notchHalf;

        // [dcLo, dcHi] inclusive is cut out of [a, b), leaving the bins below the
        // notch (segment 1) and above it (segment 2). The clamps collapse the
        // degenerate cases: a notch outside [a, b) leaves the whole interior in
        // one segment, a notch covering it leaves none. Lengths floored at 0 so
        // an empty segment contributes nothing; all offsets stay within [0, N].
        int seg1Lo = a;
        int len1 = std::max(0, std::min(b, dcLo) - a);
        int seg2Lo = std::max(a, dcHi + 1);
        int len2 = std::max(0, b - seg2Lo);
        int perLine = len1 + len2;
        if (perLine <= 0) { return false; }

        {
            std::lock_guard<std::recursive_mutex> lck(buf_mtx);
            // rawFFTSize may have been reallocated under us (DSP thread); if so
            // the precomputed offsets no longer match rawFFTs -- bail rather than
            // index past it.
            if (rawFFTs == NULL || fftLines <= 0 || waterfallHeight <= 0 || rawFFTSize != N) { return false; }
            int nLines = std::clamp<int>(lines, 1, fftLines);
            size_t total = (size_t)perLine * nLines;
            // resize (not reserve) so out.size() is exactly the copied count -- a
            // prior larger call must not leave a stale tail for the caller's
            // nth_element. Grows only past capacity, so the pooled buffer keeps
            // the steady-state path allocation-free.
            out.resize(total);
            for (int l = 0; l < nLines; l++) {
                // currentFFTLine is the newest; older lines follow at +1, +2...
                // When the waterfall is hidden the live line is rawFFTs[0], not
                // the frozen ring slot at currentFFTLine.
                const float* line = waterfallVisible
                    ? &rawFFTs[((currentFFTLine + l) % waterfallHeight) * N]
                    : rawFFTs;
                float* dst = out.data() + (size_t)l * perLine;
                if (len1 > 0) { memcpy(dst, &line[seg1Lo], len1 * sizeof(float)); dst += len1; }
                if (len2 > 0) { memcpy(dst, &line[seg2Lo], len2 * sizeof(float)); }
            }
        }

        // Reject the sentinel / non-finite values (nothing real sits below
        // -200 dBFS). Compact in place, unlocked; a no-op on a live band.
        out.erase(std::remove_if(out.begin(), out.end(),
                      [](float v) { return !(std::isfinite(v) && v > -200.0f); }),
                  out.end());
        // Too few bins to trust (e.g. zoomed onto pure roll-off): bail so the
        // caller keeps the previous range.
        return out.size() >= 64;
    }

    // Estimate just the noise floor (the bottom of the display window) from the
    // visible raw bins. The span/top is left to the user's Range. Reuses the
    // member output buffer (collectAutorangeBins resize()s it, keeping capacity)
    // so the periodic sticky path doesn't alloc/free a ~65k-float vector every
    // call.
    bool WaterFall::getAutorangeRef(float& refDb, int lines) {
        std::vector<float>& vals = autorangeVals;
        if (!collectAutorangeBins(vals, lines)) { return false; }

        // Robust low quantile instead of the raw minimum: raw min chases single
        // downward noise excursions. nth_element is O(n) vs O(n log n) for a
        // full sort, which matters on a ~65k-bin raw line.
        size_t k = (size_t)(0.30 * (vals.size() - 1));
        std::nth_element(vals.begin(), vals.begin() + k, vals.end());
        float floorDb = vals[k];
        // Place the floor 8 dB above the window bottom so noise sits just off
        // the darkest color instead of pure black.
        refDb = floorDb - 8.0f;
        return true;
    }

    // Release the pooled bin buffer. Call after a multi-line one-shot so the
    // single-line sticky path doesn't keep the 5x-larger allocation around.
    void WaterFall::freeAutorangeScratch() {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        std::vector<float>().swap(autorangeVals);
    }

    void WaterFall::setCenterFrequency(double freq) {
        centerFreq = freq;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        updateAllVFOs();
    }

    double WaterFall::getCenterFrequency() {
        return centerFreq;
    }

    void WaterFall::setBandwidth(double bandWidth) {
        double currentRatio = viewBandwidth / wholeBandwidth;
        wholeBandwidth = bandWidth;
        setViewBandwidth(bandWidth * currentRatio);
        for (auto const& [name, vfo] : vfos) {
            if (vfo->lowerOffset < -(bandWidth / 2)) {
                vfo->setCenterOffset(-(bandWidth / 2));
            }
            if (vfo->upperOffset > (bandWidth / 2)) {
                vfo->setCenterOffset(bandWidth / 2);
            }
        }
        updateAllVFOs();
    }

    double WaterFall::getBandwidth() {
        return wholeBandwidth;
    }

    void WaterFall::setViewBandwidth(double bandWidth) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (bandWidth == viewBandwidth) {
            return;
        }
        if (abs(viewOffset) + (bandWidth / 2.0) > wholeBandwidth / 2.0) {
            if (viewOffset < 0) {
                viewOffset = (bandWidth / 2.0) - (wholeBandwidth / 2.0);
            }
            else {
                viewOffset = (wholeBandwidth / 2.0) - (bandWidth / 2.0);
            }
        }
        viewBandwidth = bandWidth;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        range = findBestRange(bandWidth, maxHSteps);
        if (_fullUpdate) { updateWaterfallFb(); };
        updateAllVFOs();
    }

    double WaterFall::getViewBandwidth() {
        return viewBandwidth;
    }

    void WaterFall::setViewOffset(double offset) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (offset == viewOffset) {
            return;
        }
        if (offset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
            offset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
        }
        if (offset + (viewBandwidth / 2.0) > (wholeBandwidth / 2.0)) {
            offset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
        }
        viewOffset = offset;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        if (_fullUpdate) { updateWaterfallFb(); };
        updateAllVFOs();
    }

    double WaterFall::getViewOffset() {
        return viewOffset;
    }

    void WaterFall::setFFTMin(float min) {
        fftMin = min;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMin() {
        return fftMin;
    }

    void WaterFall::setFFTMax(float max) {
        fftMax = max;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMax() {
        return fftMax;
    }

    void WaterFall::setFullWaterfallUpdate(bool fullUpdate) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        _fullUpdate = fullUpdate;
    }

    void WaterFall::setWaterfallMin(float min) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (min == waterfallMin) {
            return;
        }
        waterfallMin = min;
        if (_fullUpdate) { updateWaterfallFb(); };
    }

    float WaterFall::getWaterfallMin() {
        return waterfallMin;
    }

    void WaterFall::setWaterfallMax(float max) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (max == waterfallMax) {
            return;
        }
        waterfallMax = max;
        if (_fullUpdate) { updateWaterfallFb(); };
    }

    float WaterFall::getWaterfallMax() {
        return waterfallMax;
    }

    void WaterFall::updateAllVFOs(bool checkRedrawRequired) {
        for (auto const& [name, vfo] : vfos) {
            if (checkRedrawRequired && !vfo->redrawRequired) { continue; }
            vfo->updateDrawingVars(viewBandwidth, dataWidth, viewOffset, widgetPos, fftHeight);
            vfo->wfRectMin = ImVec2(vfo->rectMin.x, wfMin.y);
            vfo->wfRectMax = ImVec2(vfo->rectMax.x, wfMax.y);
            vfo->wfLineMin = ImVec2(vfo->lineMin.x, wfMin.y - 1);
            vfo->wfLineMax = ImVec2(vfo->lineMax.x, wfMax.y - 1);
            float wfGrip = style::dp(2.0f);
            vfo->wfLbwSelMin = ImVec2(vfo->wfRectMin.x - wfGrip, vfo->wfRectMin.y);
            vfo->wfLbwSelMax = ImVec2(vfo->wfRectMin.x + wfGrip, vfo->wfRectMax.y);
            vfo->wfRbwSelMin = ImVec2(vfo->wfRectMax.x - wfGrip, vfo->wfRectMin.y);
            vfo->wfRbwSelMax = ImVec2(vfo->wfRectMax.x + wfGrip, vfo->wfRectMax.y);
            vfo->redrawRequired = false;
        }
    }

    void WaterFall::setRawFFTSize(int size) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        rawFFTSize = size;
        int wfSize = std::max<int>(1, waterfallHeight);
        if (rawFFTs != NULL) {
            rawFFTs = (float*)realloc(rawFFTs, rawFFTSize * wfSize * sizeof(float));
        }
        else {
            rawFFTs = (float*)malloc(rawFFTSize * wfSize * sizeof(float));
        }
        fftLines = 0;
        memset(rawFFTs, 0, rawFFTSize * waterfallHeight * sizeof(float));
        updateWaterfallFb();
    }

    void WaterFall::setBandPlanPos(int pos) {
        bandPlanPos = pos;
    }

    void WaterFall::setFFTHold(bool hold) {
        fftHold = hold;
        if (fftHold && latestFFTHold) {
            for (int i = 0; i < dataWidth; i++) {
                latestFFTHold[i] = -1000.0;
            }
        }
    }

    void WaterFall::setFFTHoldSpeed(float speed) {
        fftHoldSpeed = speed;
    }

    void WaterFall::setFFTSmoothing(bool enabled) {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        fftSmoothing = enabled;

        // Free buffer if not null
        if (smoothingBuf) {delete[] smoothingBuf; }

        // If disabled, stop here
        if (!enabled) {
            smoothingBuf = NULL;
            return;
        }

        // Allocate and copy existing FFT into it
        smoothingBuf = new float[dataWidth];
        if (latestFFT) {
            std::lock_guard<std::recursive_mutex> lck2(latestFFTMtx);
            memcpy(smoothingBuf, latestFFT, dataWidth * sizeof(float));
        }
        else {
            memset(smoothingBuf, 0, dataWidth * sizeof(float));
        }
    }

    void WaterFall::setFFTSmoothingSpeed(float speed) {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        fftSmoothingAlpha = speed;
        fftSmoothingBeta = 1.0f - speed;
    }

    float* WaterFall::acquireLatestFFT(int& width) {
        latestFFTMtx.lock();
        if (!latestFFT) {
            latestFFTMtx.unlock();
            return NULL;
        }
        width = dataWidth;
        return latestFFT;
    }

    void WaterFall::releaseLatestFFT() {
        latestFFTMtx.unlock();
    }

    void WaterfallVFO::setOffset(double offset) {
        generalOffset = offset;
        if (reference == REF_CENTER) {
            centerOffset = offset;
            lowerOffset = offset - (bandwidth / 2.0);
            upperOffset = offset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER) {
            lowerOffset = offset;
            centerOffset = offset + (bandwidth / 2.0);
            upperOffset = offset + bandwidth;
        }
        else if (reference == REF_UPPER) {
            upperOffset = offset;
            centerOffset = offset - (bandwidth / 2.0);
            lowerOffset = offset - bandwidth;
        }
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setCenterOffset(double offset) {
        if (reference == REF_CENTER) {
            generalOffset = offset;
        }
        else if (reference == REF_LOWER) {
            generalOffset = offset - (bandwidth / 2.0);
        }
        else if (reference == REF_UPPER) {
            generalOffset = offset + (bandwidth / 2.0);
        }
        centerOffset = offset;
        lowerOffset = offset - (bandwidth / 2.0);
        upperOffset = offset + (bandwidth / 2.0);
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setBandwidth(double bw) {
        if (bandwidth == bw || bw < 0) {
            return;
        }
        bandwidth = bw;
        if (reference == REF_CENTER) {
            lowerOffset = centerOffset - (bandwidth / 2.0);
            upperOffset = centerOffset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER) {
            centerOffset = lowerOffset + (bandwidth / 2.0);
            upperOffset = lowerOffset + bandwidth;
            centerOffsetChanged = true;
        }
        else if (reference == REF_UPPER) {
            centerOffset = upperOffset - (bandwidth / 2.0);
            lowerOffset = upperOffset - bandwidth;
            centerOffsetChanged = true;
        }
        bandwidthChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setReference(int ref) {
        if (reference == ref || ref < 0 || ref >= _REF_COUNT) {
            return;
        }
        reference = ref;
        setOffset(generalOffset);
    }

    void WaterfallVFO::setNotchOffset(double offset) {
        notchOffset = offset;
        redrawRequired = true;
    }

    void WaterfallVFO::setNotchVisible(bool visible) {
        notchVisible = visible;
        redrawRequired = true;
    }

    void WaterfallVFO::updateDrawingVars(double viewBandwidth, float dataWidth, double viewOffset, ImVec2 widgetPos, int fftHeight) {
        int center = roundf((((centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int left = roundf((((lowerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int right = roundf((((upperOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int notch = roundf((((notchOffset + centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));

        // Check weather the line is visible
        if (left >= 0 && left < dataWidth && reference == REF_LOWER) {
            lineVisible = true;
        }
        else if (center >= 0 && center < dataWidth && reference == REF_CENTER) {
            lineVisible = true;
        }
        else if (right >= 0 && right < dataWidth && reference == REF_UPPER) {
            lineVisible = true;
        }
        else {
            lineVisible = false;
        }

        // Calculate the position of the line
        if (reference == REF_LOWER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_CENTER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_UPPER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMax.y - 1);
        }

        int _left = left;
        int _right = right;
        left = std::clamp<int>(left, 0, dataWidth - 1);
        right = std::clamp<int>(right, 0, dataWidth - 1);
        leftClamped = (left != _left);
        rightClamped = (right != _right);

        rectMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y + 1);
        rectMax = ImVec2(gui::waterfall.fftAreaMin.x + right + 1, gui::waterfall.fftAreaMax.y);

        float gripSize = style::dp(2.0f);
        lbwSelMin = ImVec2(rectMin.x - gripSize, rectMin.y);
        lbwSelMax = ImVec2(rectMin.x + gripSize, rectMax.y);
        rbwSelMin = ImVec2(rectMax.x - gripSize, rectMin.y);
        rbwSelMax = ImVec2(rectMax.x + gripSize, rectMax.y);

        notchMin = ImVec2(gui::waterfall.fftAreaMin.x + notch - gripSize, gui::waterfall.fftAreaMin.y);
        notchMax = ImVec2(gui::waterfall.fftAreaMin.x + notch + gripSize, gui::waterfall.fftAreaMax.y - 1);
    }

    void WaterfallVFO::draw(ImGuiWindow* window, bool selected) {
        window->DrawList->AddRectFilled(rectMin, rectMax, color);
        if (lineVisible) {
            window->DrawList->AddLine(lineMin, lineMax, selected ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::lineWidth());
        }

        if (notchVisible) {
            window->DrawList->AddRectFilled(notchMin, notchMax, IM_COL32(255, 0, 0, 127));
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !gui::mainWindow.lockWaterfallControls && !gui::waterfall.inputHandled)
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            if (rectMax.x - rectMin.x < 10) { return; }
            if (reference != REF_LOWER && !bandwidthLocked && !leftClamped) {
                if (IS_IN_AREA(mousePos, lbwSelMin, lbwSelMax)) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
                else if (IS_IN_AREA(mousePos, wfLbwSelMin, wfLbwSelMax)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
            if (reference != REF_UPPER && !bandwidthLocked && !rightClamped) {
                if (IS_IN_AREA(mousePos, rbwSelMin, rbwSelMax)) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
                else if (IS_IN_AREA(mousePos, wfRbwSelMin, wfRbwSelMax)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
        }
    };

    void WaterFall::showWaterfall() {
        buf_mtx.lock();
        if (rawFFTs == NULL) {
            flog::error("Null rawFFT");
        }
        waterfallVisible = true;
        onResize();
        memset(rawFFTs, 0, waterfallHeight * rawFFTSize * sizeof(float));
        updateWaterfallFb();
        buf_mtx.unlock();
    }

    void WaterFall::hideWaterfall() {
        buf_mtx.lock();
        waterfallVisible = false;
        onResize();
        buf_mtx.unlock();
    }

    void WaterFall::setFFTHeight(int height) {
        FFTAreaHeight = height;
        newFFTAreaHeight = height;
        buf_mtx.lock();
        onResize();
        buf_mtx.unlock();
    }

    int WaterFall::getFFTHeight() {
        return FFTAreaHeight;
    }

    void WaterFall::showBandplan() {
        bandplanVisible = true;
    }

    void WaterFall::hideBandplan() {
        bandplanVisible = false;
    }

    void WaterfallVFO::setSnapInterval(double interval) {
        snapInterval = interval;
    }
};
