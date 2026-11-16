#include <gui/widgets/frequency_select.h>
#include <gui/widgets/popup_dialog.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <backend.h>
#include <utils/hrfreq.h>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#ifdef __ANDROID__
#include <android_backend.h>
#endif

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

static bool isInArea(ImVec2 val, ImVec2 min, ImVec2 max) {
    return val.x >= min.x && val.x < max.x && val.y >= min.y && val.y < max.y;
}

FrequencySelect::FrequencySelect() {
}

void FrequencySelect::init() {
    for (int i = 0; i < 12; i++) {
        digits[i] = 0;
    }
}

void FrequencySelect::onPosChange() {
    // Round the glyph metrics (fractional at fractional UI scales) so this
    // computation stays identical to getWidth() — both must produce the same
    // cachedWidth_ or the top-bar layout shifts whenever the other one runs.
    ImVec2 digitSz = ImGui::CalcTextSize("0");
    ImVec2 commaSz = ImGui::CalcTextSize(".");
    int digitHeight = (int)roundf(digitSz.y);
    int digitWidth = (int)roundf(digitSz.x);
    int commaWidth = (int)roundf(commaSz.x);
    int commaOffset = 0;
    for (int i = firstDigit; i < 12; i++) {
        int pos = i - firstDigit;
        digitTopMins[i] = ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset, widgetPos.y);
        digitBottomMins[i] = ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset, widgetPos.y + (digitHeight / 2));

        digitTopMaxs[i] = ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset + digitWidth, widgetPos.y + (digitHeight / 2));
        digitBottomMaxs[i] = ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset + digitWidth, widgetPos.y + digitHeight);

        if ((i + 1) % 3 == 0 && i < 11) {
            commaOffset += commaWidth;
        }
    }
    // commaOffset now holds the total accumulated comma width — reuse it for the
    // width cache rather than recomputing with a separate PushFont/CalcTextSize pair.
    cachedWidth_ = (12 - firstDigit) * digitWidth + commaOffset + style::dp(17.0f);
}

void FrequencySelect::incrementDigit(int i) {
    if (i < 0) {
        return;
    }
    if (digits[i] < 9) {
        digits[i]++;
    }
    else {
        digits[i] = 0;
        incrementDigit(i - 1);
    }
    frequencyChanged = true;
}

void FrequencySelect::decrementDigit(int i) {
    if (i < 0) {
        return;
    }
    if (digits[i] > 0) {
        digits[i]--;
    }
    else {
        if (i == 0) { return; }

        // Check if there are non zero digits afterwards
        bool otherNoneZero = false;
        for (int j = i - 1; j >= 0; j--) {
            if (digits[j] > 0) {
                otherNoneZero = true;
                break;
            }
        }
        if (!otherNoneZero) { return; }

        digits[i] = 9;
        decrementDigit(i - 1);
    }
    frequencyChanged = true;
}

void FrequencySelect::moveCursorToDigit(int i) {
    double xpos, ypos;
    backend::getMouseScreenPos(xpos, ypos);
    double nxpos = (digitTopMaxs[i].x + digitTopMins[i].x) / 2.0;
    backend::setMouseScreenPos(nxpos, ypos);
}

// The tuning situation handed to the frequency-input dialog each frame.
freq_input::Context FrequencySelect::inputContext() const {
    freq_input::Context ctx;
    ctx.frequency = frequency;
    ctx.limited = limitFreq;
    ctx.minFreq = minFreq;
    ctx.maxFreq = maxFreq;
    return ctx;
}

float FrequencySelect::getWidth() {
    // getWidth() is called during top-bar layout before draw() runs, so it
    // must refresh the cache itself when the scale changes — otherwise the
    // first scaled frame reserves space using old-scale digit widths.
    uint64_t currentScaleEpoch = style::scaleEpoch();
    if (currentScaleEpoch != lastScaleEpoch || cachedWidth_ == 0.0f) {
        ImGui::PushFont(style::bigFont);
        // Same rounded metrics as onPosChange() — the two must agree exactly.
        int digitWidth = (int)roundf(ImGui::CalcTextSize("0").x);
        int commaWidth = (int)roundf(ImGui::CalcTextSize(".").x);
        ImGui::PopFont();
        int commaCount = 0;
        for (int i = firstDigit; i < 11; i++) {
            if ((i + 1) % 3 == 0) { commaCount++; }
        }
        cachedWidth_ = (12 - firstDigit) * digitWidth + commaCount * commaWidth + style::dp(17.0f);
        // Don't update lastScaleEpoch here — draw() still needs to see the
        // change to recompute the digit position arrays.
    }
    return cachedWidth_;
}

void FrequencySelect::draw() {
    auto window = ImGui::GetCurrentWindow();
    auto io = ImGui::GetIO();
    widgetPos = ImGui::GetWindowContentRegionMin();
    ImVec2 cursorPos = ImGui::GetCursorPos();
    widgetPos.x += window->Pos.x + cursorPos.x;
    ImGui::PushFont(style::bigFont);
    ImVec2 digitSz = ImGui::CalcTextSize("0");
    ImVec2 commaSz = ImGui::CalcTextSize(".");
    // Snap the origin to whole pixels — a fractional baseline blurs the big
    // digits at fractional UI scales. Centred on the top-bar row: the previous
    // hand-tuned offset carried an unscaled +5, so it was exact at 1x and drew
    // the digits 5*(uiScale-1) px high everywhere else (10 px on Android).
    widgetPos.x = roundf(widgetPos.x);
    widgetPos.y = roundf(window->Pos.y + cursorPos.y + style::topBarRowOffset(digitSz.y));

    // Recompute the first visible digit only when maxFreq or limitFreq changes
    bool firstDigitChanged = false;
    if (maxFreq != lastMaxFreq || limitFreq != lastLimitFreq) {
        lastMaxFreq = maxFreq;
        lastLimitFreq = limitFreq;
        int newFirstDigit = 0;
        if (limitFreq && maxFreq > 0) {
            uint64_t mf = maxFreq;
            int numDigits = 0;
            while (mf > 0) { mf /= 10; numDigits++; }
            newFirstDigit = 12 - numDigits;
            if (newFirstDigit < 0) { newFirstDigit = 0; }
        }
        firstDigitChanged = (newFirstDigit != firstDigit);
        firstDigit = newFirstDigit;
        // Zero out hidden leading digits
        for (int i = 0; i < firstDigit; i++)
            digits[i] = 0;
    }

    uint64_t currentScaleEpoch = style::scaleEpoch();
    if (widgetPos.x != lastWidgetPos.x || widgetPos.y != lastWidgetPos.y || firstDigitChanged || currentScaleEpoch != lastScaleEpoch) {
        lastWidgetPos = widgetPos;
        lastScaleEpoch = currentScaleEpoch;
        onPosChange();
    }

    ImU32 disabledColor = ImGui::GetColorU32(ImGuiCol_Text, 0.3f);
    ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);


    // Same rounded metrics as onPosChange() so drawn digits line up with the
    // cached hitboxes.
    int digitWidth = (int)roundf(digitSz.x);
    int commaWidth = (int)roundf(commaSz.x);
    int commaOffset = 0;
    float textOffset = (float)style::scale(11.0f);
    bool zeros = true;

    // Reserve the row height, not the glyph box: one bigFont line is 45 dp,
    // taller than the row's 40 dp buttons, so reserving it made the frequency
    // display set the top bar's height and pushed the FFT down by 5 dp for no
    // reason. The digits still overhang the row by half the difference, which
    // is what the centring above intends; hit-testing uses the explicit digit
    // rects below, not this box.
    ImGui::ItemSize(ImVec2(digitBottomMaxs[11].x + style::dp(17.0f) - digitTopMins[firstDigit].x, style::topBarRowHeight()));

    for (int i = firstDigit; i < 12; i++) {
        if (digits[i] != 0) {
            zeros = false;
        }
        int pos = i - firstDigit;
        const char digit[2] = { (char)('0' + digits[i]), 0 };
        window->DrawList->AddText(ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset, widgetPos.y),
                                  zeros ? disabledColor : textColor, digit);
        if ((i + 1) % 3 == 0 && i < 11) {
            commaOffset += commaWidth;
            window->DrawList->AddText(ImVec2(widgetPos.x + (pos * digitWidth) + commaOffset + textOffset, widgetPos.y),
                                      zeros ? disabledColor : textColor, ".");
        }
    }

    bool hovered = false;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
        !gui::mainWindow.lockWaterfallControls)
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        bool leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool rightClick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        // Precision touchpads report fractional wheel deltas; accumulate and
        // step digits on whole notches instead of truncating them away.
        wheelAccum += io.MouseWheel;
        int mw = (int)wheelAccum;
        wheelAccum -= (float)mw;
        bool onDigit = false;

        for (int i = firstDigit; i < 12; i++) {
            onDigit = false;
            if (isInArea(mousePos, digitTopMins[i], digitTopMaxs[i])) {
                window->DrawList->AddRectFilled(digitTopMins[i], digitTopMaxs[i], IM_COL32(255, 0, 0, 75));
                if (leftClick) {
                    pressDigit = i;
                    pressDir = 1;
                    longPressDone = false;
                }
                onDigit = true;
            }
            if (isInArea(mousePos, digitBottomMins[i], digitBottomMaxs[i])) {
                window->DrawList->AddRectFilled(digitBottomMins[i], digitBottomMaxs[i], IM_COL32(0, 0, 255, 75));
                if (leftClick) {
                    pressDigit = i;
                    pressDir = -1;
                    longPressDone = false;
                }
                onDigit = true;
            }
            if (onDigit) {
                hovered = true;
                if (rightClick || ImGui::IsKeyPressed(ImGuiKey_Delete) || PopupDialog::confirmKeyPressed()) {
                    for (int j = i; j < 12; j++) {
                        digits[j] = 0;
                    }

                    frequencyChanged = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                    incrementDigit(i);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                    decrementDigit(i);
                }
                if ((ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && i > firstDigit) {
                    moveCursorToDigit(i - 1);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && i < 11) {
                    moveCursorToDigit(i + 1);
                }

                auto chars = io.InputQueueCharacters;

                // For each keyboard characters, type it
                for (int j = 0; j < chars.Size; j++) {
                    if (chars[j] >= '0' && chars[j] <= '9') {
                        if ((i + j) > 11) { break; }
                        digits[i + j] = chars[j] - '0';
                        if ((i + j) < 11) { moveCursorToDigit(i + j + 1); }
                        frequencyChanged = true;
                    }
                }

                if (mw != 0) {
                    int count = abs(mw);
                    for (int j = 0; j < count; j++) {
                        mw > 0 ? incrementDigit(i) : decrementDigit(i);
                    }
                }
            }
        }

        // A press armed on a digit half steps it on a quick release; held
        // motionless past the threshold it opens the frequency-input dialog instead.
        // Stepping waits for the release so the two can't both fire.
        if (pressDigit >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float slop = 10.0f * style::uiScale;
                float dx = mousePos.x - io.MouseClickedPos[ImGuiMouseButton_Left].x;
                float dy = mousePos.y - io.MouseClickedPos[ImGuiMouseButton_Left].y;
                if ((dx * dx) + (dy * dy) > (slop * slop)) {
                    pressDigit = -1; // moved away: neither a tap nor a long press
                }
                else if (!longPressDone && io.MouseDownDuration[ImGuiMouseButton_Left] >= 0.5f) {
                    longPressDone = true;
                    dialog.open();
#ifdef __ANDROID__
                    backend::hapticTick();
#endif
                }
            }
            else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (!longPressDone) {
                    if (pressDir > 0 && isInArea(mousePos, digitTopMins[pressDigit], digitTopMaxs[pressDigit])) {
                        incrementDigit(pressDigit);
                    }
                    else if (pressDir < 0 && isInArea(mousePos, digitBottomMins[pressDigit], digitBottomMaxs[pressDigit])) {
                        decrementDigit(pressDigit);
                    }
                }
                pressDigit = -1;
            }
            else {
                pressDigit = -1; // press was lost without a release event
            }
        }

        if (isInArea(mousePos, digitTopMins[firstDigit], digitBottomMaxs[11])) {
            bool shortcutKey = io.ConfigMacOSXBehaviors ? (io.KeyMods == ImGuiKeyModFlags_Super) : (io.KeyMods == ImGuiKeyModFlags_Ctrl);
            bool ctrlOnly = (io.KeyMods == ImGuiKeyModFlags_Ctrl);
            bool shiftOnly = (io.KeyMods == ImGuiKeyModFlags_Shift);
            bool copy  = ((shortcutKey && ImGui::IsKeyPressed(ImGuiKey_C)) || (ctrlOnly  && ImGui::IsKeyPressed(ImGuiKey_Insert)));
            bool paste = ((shortcutKey && ImGui::IsKeyPressed(ImGuiKey_V)) || (shiftOnly && ImGui::IsKeyPressed(ImGuiKey_Insert)));
            if (copy) {
                // Convert the freqency to a string
                std::string freqStr = hrfreq::toString(frequency);

                // Write it to the clipboard
                ImGui::SetClipboardText(freqStr.c_str());
            }
            if (paste) {
                // Attempt to parse the clipboard as a number
                const char* clip = ImGui::GetClipboardText();

                // If the clipboard is not empty, attempt to parse it
                if (clip) {
                    double newFreq;
                    if (hrfreq::fromString(clip, newFreq)) {
                        setFrequency(abs(newFreq));
                        frequencyChanged = true;
                    }
                }
            }
        }
    }
    // Assigned outside the hover-gated block, so leaving the window while over
    // a digit cannot leave digitHovered stuck true and block the arrow-key
    // tuning in main_window. Partial wheel notches are also dropped once the
    // cursor is off the digits so they can't discharge into a step later.
    digitHovered = hovered;
    if (!hovered) { wheelAccum = 0.0f; }

    uint64_t freq = 0;
    for (int i = 0; i < 12; i++) {
        freq += digits[i] * pow(10, 11 - i);
    }

    uint64_t orig = freq;
    freq = std::clamp<uint64_t>(freq, minFreq, maxFreq);
    if (freq != orig && limitFreq) {
        setFrequency(freq);
    }
    else {
        frequency = orig;
    }

    ImGui::PopFont();

    ImGui::SetCursorPosX(digitBottomMaxs[11].x + (float)style::scale(17.0f));

    // The dialog draws last, after the bigFont pop and after the digit
    // hit-testing above: while it is open, ImGui blocks hover on the top bar, so
    // the two consumers of InputQueueCharacters can't both fire in one frame.
    freq_input::Outcome result = dialog.draw(inputContext());
    if (result.commit) {
        //FIXME set mode if the band stack did not store the mode yet
        setFrequency((int64_t)result.frequency);
        frequencyChanged = true;
    }
}

void FrequencySelect::setFrequency(int64_t freq) {
    freq = std::max<int64_t>(0, freq);
    int i = 11;
    for (uint64_t f = freq; i >= 0; i--) {
        digits[i] = f % 10;
        f -= digits[i];
        f /= 10;
    }
    frequency = freq;
}
