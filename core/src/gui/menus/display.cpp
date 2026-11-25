#include <gui/menus/display.h>
#include <imgui.h>
#include <gui/gui.h>
#include <gui/menus/theme.h>
#include <core.h>
#include <backend.h>
#include <gui/colormaps.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <algorithm>

namespace displaymenu {
    bool showWaterfall;
    bool fullWaterfallUpdate = true;
    int colorMapId = 0;
    std::vector<std::string> colorMapNames;
    std::string colorMapNamesTxt = "";
    std::string colorMapAuthor = "";
    int selectedWindow = 0;
    int fftRate = 20;
    int fftSizeId = 0;
    int uiScaleFactorId = 0;
    bool fftHold = false;
    int fftHoldSpeed = 60;
    bool fftSmoothing = false;
    int fftSmoothingSpeed = 100;

    OptionList<int, int> fftSizes;
    OptionList<float, float> uiScaleFactors;
    OptionList<std::string, IQFrontEnd::FFTWindow> fftWindows;

    void updateFFTSpeeds() {
        gui::waterfall.setFFTHoldSpeed((float)fftHoldSpeed / ((float)fftRate * 10.0f));
        gui::waterfall.setFFTSmoothingSpeed(std::min<float>((float)fftSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
    }

    void init() {
        // Define FFT sizes
        // 1M disabled until the waterfall allocation path is hardened: it stores
        // rawFFTSize * waterfallHeight floats (>1 GiB at a few hundred rows) and
        // reallocs without checking for failure.
        // fftSizes.define(1048576, "1048576", 1048576);
        fftSizes.define(524288, "524288", 524288);
        fftSizes.define(262144, "262144", 262144);
        fftSizes.define(131072, "131072", 131072);
        fftSizes.define(65536, "65536", 65536);
        fftSizes.define(32768, "32768", 32768);
        fftSizes.define(16384, "16384", 16384);
        fftSizes.define(8192, "8192", 8192);
        fftSizes.define(4096, "4096", 4096);
        fftSizes.define(2048, "2048", 2048);
        fftSizes.define(1024, "1024", 1024);

        // Define FFT windows, in order of increasing dynamic range
        fftWindows.define("Rectangular", "Rectangular", IQFrontEnd::FFTWindow::RECTANGULAR);
        fftWindows.define("Hamming", "Hamming", IQFrontEnd::FFTWindow::HAMMING);
        fftWindows.define("Hann", "Hann", IQFrontEnd::FFTWindow::HANN);
        fftWindows.define("Blackman", "Blackman", IQFrontEnd::FFTWindow::BLACKMAN);
        fftWindows.define("Nuttall", "Nuttall", IQFrontEnd::FFTWindow::NUTTALL);
        fftWindows.define("Blackman-Harris 4", "Blackman-Harris 4", IQFrontEnd::FFTWindow::BLACKMAN_HARRIS4);
        fftWindows.define("Blackman-Harris 7", "Blackman-Harris 7", IQFrontEnd::FFTWindow::BLACKMAN_HARRIS7);

        // Everything the menu remembers is read in one pass and applied below,
        // so the config lock is never held across a call into the waterfall or
        // the front end.
        std::string colormapName;
        int fftSize = 65536;
        std::string winName = "Nuttall";
        float factor = 1.0f;
        {
            auto configAccess = core::configManager.edit();
            configAccess.tryGet("showWaterfall", showWaterfall);
            configAccess.tryGet("colorMap", colormapName);
            configAccess.tryGet("fullWaterfallUpdate", fullWaterfallUpdate);
            configAccess.tryGet("fftSize", fftSize);
            configAccess.tryGet("fftRate", fftRate);

            // The window is stored by name; legacy configs stored an index into
            // the {Rectangular, Blackman, Nuttall} list.
            json fftWindowConf = configAccess.value("fftWindow", json());
            if (fftWindowConf.is_string()) {
                winName = fftWindowConf;
            }
            else if (fftWindowConf.is_number_integer()) {
                const char* legacyWindows[] = { "Rectangular", "Blackman", "Nuttall" };
                winName = legacyWindows[std::clamp<int>(fftWindowConf, 0, 2)];
                configAccess.set("fftWindow", winName);
            }

            configAccess.tryGet("lockMenuOrder", gui::menu.locked);
            configAccess.tryGet("fftHold", fftHold);
            configAccess.tryGet("fftHoldSpeed", fftHoldSpeed);
            configAccess.tryGet("fftSmoothing", fftSmoothing);
            configAccess.tryGet("fftSmoothingSpeed", fftSmoothingSpeed);
            configAccess.tryGet("uiScaleFactor", factor);
        }

        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        if (const auto selected = colormaps::maps.find(colormapName);
            selected != colormaps::maps.end())
        {
            const colormaps::Map& map = selected->second;
            gui::waterfall.updatePalletteFromArray(
                map.colors.data(), map.entryCount());
        }

        for (auto const& [name, map] : colormaps::maps) {
            colorMapNames.push_back(name);
            colorMapNamesTxt += name;
            colorMapNamesTxt += '\0';
            if (name == colormapName) {
                colorMapId = (colorMapNames.size() - 1);
                colorMapAuthor = map.author;
            }
        }

        gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);

        fftSizeId = fftSizes.keyExists(fftSize) ? fftSizes.keyId(fftSize) : fftSizes.valueId(65536);
        sigpath::iqFrontEnd.setFFTSize(fftSizes.value(fftSizeId));

        sigpath::iqFrontEnd.setFFTRate(fftRate);

        selectedWindow = fftWindows.keyExists(winName) ? fftWindows.keyId(winName) : fftWindows.keyId("Nuttall");
        sigpath::iqFrontEnd.setFFTWindow(fftWindows.value(selectedWindow));

        gui::waterfall.setFFTHold(fftHold);
        gui::waterfall.setFFTSmoothing(fftSmoothing);
        updateFFTSpeeds();

        // Define and load UI scale factor options
        uiScaleFactors.define(0.50f, "50%",  0.50f);
        uiScaleFactors.define(0.75f, "75%",  0.75f);
        uiScaleFactors.define(1.00f, "100%", 1.00f);
        uiScaleFactors.define(1.25f, "125%", 1.25f);
        uiScaleFactors.define(1.50f, "150%", 1.50f);
        uiScaleFactors.define(1.75f, "175%", 1.75f);
        uiScaleFactors.define(2.00f, "200%", 2.00f);
        // valueId() throws on unknown values (it never returns negative), so a
        // hand-edited config must be checked first or it aborts startup.
        uiScaleFactorId = uiScaleFactors.valueExists(factor) ? uiScaleFactors.valueId(factor) : uiScaleFactors.valueId(1.00f);
    }

    void setWaterfallShown(bool shown) {
        showWaterfall = shown;
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        core::configManager.edit().set("showWaterfall", showWaterfall);
    }

    void checkKeybinds() {
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            setWaterfallShown(!showWaterfall);
        }
    }

    void draw(void* ctx) {
        thememenu::draw(ctx);

        if (ImGui::Checkbox("Show Waterfall##_sdrpp", &showWaterfall)) {
            setWaterfallShown(showWaterfall);
        }

        if (ImGui::Checkbox("Full Waterfall Update##_sdrpp", &fullWaterfallUpdate)) {
            gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
            core::configManager.edit().set("fullWaterfallUpdate", fullWaterfallUpdate);
        }

        if (ImGui::Checkbox("Lock Menu Order##_sdrpp", &gui::menu.locked)) {
            core::configManager.edit().set("lockMenuOrder", gui::menu.locked);
        }

        if (ImGui::Checkbox("FFT Hold##_sdrpp", &fftHold)) {
            gui::waterfall.setFFTHold(fftHold);
            core::configManager.edit().set("fftHold", fftHold);
        }
        ImGui::SameLine();
        ImGui::FillWidth();
        if (ImGui::InputInt("##sdrpp_fft_hold_speed", &fftHoldSpeed)) {
            updateFFTSpeeds();
            core::configManager.edit().set("fftHoldSpeed", fftHoldSpeed);
        }

        if (ImGui::Checkbox("FFT Smoothing##_sdrpp", &fftSmoothing)) {
            gui::waterfall.setFFTSmoothing(fftSmoothing);
            core::configManager.edit().set("fftSmoothing", fftSmoothing);
        }
        ImGui::SameLine();
        ImGui::FillWidth();
        if (ImGui::InputInt("##sdrpp_fft_smoothing_speed", &fftSmoothingSpeed)) {
            fftSmoothingSpeed = std::max<int>(fftSmoothingSpeed, 1);
            updateFFTSpeeds();
            core::configManager.edit().set("fftSmoothingSpeed", fftSmoothingSpeed);
        }

        if (ImGui::Checkbox("Touch-Friendly UI##_sdrpp", &style::touchStyle)) {
            style::applyScaledStyle(thememenu::applyTheme);
            core::configManager.edit().set("touchStyle", style::touchStyle);
        }

        ImGui::LeftLabel("UI Scale Adjustment");
        ImGui::FillWidth();
        // Factors that would clamp at the 1.0 effective-scale floor on this
        // display (detected DPI scale x factor < 1) silently do nothing, so
        // show them disabled instead of pretending they apply. Evaluated per
        // frame because the detected scale follows the current monitor.
        float detectedScale = backend::getContentScale();
        if (ImGui::BeginCombo("##sdrpp_ui_scale", uiScaleFactors.name(uiScaleFactorId).c_str())) {
            for (int i = 0; i < uiScaleFactors.size(); i++) {
                bool dead = (detectedScale * uiScaleFactors.value(i)) < 0.999f;
                bool selected = (i == uiScaleFactorId);
                if (ImGui::Selectable(uiScaleFactors.name(i).c_str(), selected, dead ? ImGuiSelectableFlags_Disabled : 0) && !dead) {
                    uiScaleFactorId = i;
                    backend::setUserScaleFactor(uiScaleFactors.value(i));
                }
                if (selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }

        // The provider's frames replace the local FFT, so its settings would
        // only be stored; show them but keep them read-only.
        const bool spectrumFromSource = (sigpath::iqFrontEnd.spectrumProvider() != nullptr);
        if (spectrumFromSource) {
            ImGui::TextDisabled("Spectrum supplied by the source");
            style::beginDisabled();
        }

        ImGui::LeftLabelFill("FFT Framerate");
        if (ImGui::InputInt("##sdrpp_fft_rate", &fftRate, 1, 10)) {
            fftRate = std::max<int>(1, fftRate);
            sigpath::iqFrontEnd.setFFTRate(fftRate);
            updateFFTSpeeds();
            core::configManager.edit().set("fftRate", fftRate);
        }

        ImGui::LeftLabelFill("FFT Size");
        if (ImGui::Combo("##sdrpp_fft_size", &fftSizeId, fftSizes.txt)) {
            sigpath::iqFrontEnd.setFFTSize(fftSizes.value(fftSizeId));
            core::configManager.edit().set("fftSize", fftSizes.key(fftSizeId));
        }

        ImGui::LeftLabelFill("FFT Window");
        if (ImGui::Combo("##sdrpp_fft_window", &selectedWindow, fftWindows.txt)) {
            sigpath::iqFrontEnd.setFFTWindow(fftWindows.value(selectedWindow));
            core::configManager.edit().set("fftWindow", fftWindows.key(selectedWindow));
        }
        if (spectrumFromSource) { style::endDisabled(); }

        if (colorMapNames.size() > 0) {
            ImGui::LeftLabelFill("Color Map");
            if (ImGui::Combo("##_sdrpp_color_map_sel", &colorMapId, colorMapNamesTxt.c_str())) {
                const colormaps::Map& map = colormaps::maps.at(colorMapNames[colorMapId]);
                gui::waterfall.updatePalletteFromArray(
                    map.colors.data(), map.entryCount());
                core::configManager.edit().set("colorMap", colorMapNames[colorMapId]);
                colorMapAuthor = map.author;
            }
            ImGui::Text("Color map Author: %s", colorMapAuthor.c_str());
        }

    }
}
