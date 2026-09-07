#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <imgui_internal.h>
#include <stdio.h>
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/android.h>
#include <gui/dialogs/credits.h>
#ifdef __ANDROID__
#include <android_backend.h>
#endif
#include <filesystem>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>
#include <algorithm>
#include <cmath>

namespace {
    void invalidateChildLayoutCache(ImGuiWindow* parent, const char* childName) {
        if (parent == NULL) { return; }

        ImGuiID childId = parent->GetID(childName);
        ImGuiContext& g = *GImGui;
        for (int i = 0; i < g.Windows.Size; i++) {
            ImGuiWindow* child = g.Windows[i];
            if (child == NULL || child->ParentWindow != parent || child->ChildId != childId) { continue; }

            child->ContentSize = ImVec2(0, 0);
            child->ContentSizeIdeal = ImVec2(0, 0);
            child->ScrollbarSizes = ImVec2(0, 0);
            child->ScrollbarX = false;
            child->ScrollbarY = false;
            return;
        }
    }
}

void MainWindow::init() {
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();
    gui::waterfall.setRawFFTSize(fftSize);

    credits::init();

    json menuElements = core::configManager.read().value("menuElements", json::array());
    std::string modulesDir = core::getModulesDirectory();
    std::string resourcesDir = core::getResourcesDirectory();

    // Load menu elements
    gui::menu.order.clear();
    for (auto& elem : menuElements) {
        if (!elem.contains("name")) {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open")) {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Source", sourcemenu::draw, NULL);
    gui::menu.registerEntry("Sinks", sinkmenu::draw, NULL);
    gui::menu.registerEntry("Band Plan", bandplanmenu::draw, NULL, bandplanmenu::getInstance());
    gui::menu.registerEntry("Display", displaymenu::draw, NULL);
    gui::menu.registerEntry("VFO Color", vfo_color_menu::draw, NULL);
    gui::menu.registerEntry("Module Manager", module_manager_menu::draw, NULL);
#ifdef __ANDROID__
    gui::menu.registerEntry("System", androidmenu::draw, NULL);
#endif

    gui::freqSelect.init();

    // Set default values for waterfall in case no source init's it
    gui::waterfall.setBandwidth(8000000);
    gui::waterfall.setViewBandwidth(8000000);

    fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

    sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    sigpath::iqFrontEnd.start();

    vfoCreatedHandler.handler = vfoAddedHandler;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);

    flog::info("Loading modules");

    // Load modules from /module directory
    if (std::filesystem::is_directory(modulesDir)) {
        for (const auto& file : std::filesystem::directory_iterator(modulesDir)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION) {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Read module config
    std::vector<std::string> modules;
    json moduleInstances;
    {
        auto configAccess = core::configManager.read();
        configAccess.tryGet("modules", modules);
        moduleInstances = configAccess.value("moduleInstances", json::object());
    }
    auto modList = moduleInstances.items();

    // Load additional modules specified through config. Relative paths are
    // interpreted relative to the executable's directory, like the module and
    // resource directories.
    for (auto const& path : modules) {
#ifndef __ANDROID__
        std::string apath = core::resolveConfigPath(path);
        flog::info("Loading {0}", apath);
        LoadingScreen::show("Loading " + std::filesystem::path(path).filename().string());
        core::moduleManager.loadModule(apath);
#else
        core::moduleManager.loadModule(path);
#endif
    }

    // Create module instances
    for (auto const& [name, _module] : modList) {
        std::string mod = _module["module"];
        bool enabled = _module["enabled"];
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled) { core::moduleManager.disableInstance(name); }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps")) {
        for (const auto& file : std::filesystem::directory_iterator(resourcesDir + "/colormaps")) {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            colormaps::loadMap(path);
        }
    }
    else {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Look the default up instead of indexing: maps[] would insert an empty
    // "Turbo" that then shows up as a selectable, unusable entry in the menu.
    if (const auto turbo = colormaps::maps.find("Turbo"); turbo != colormaps::maps.end()) {
        gui::waterfall.updatePalletteFromArray(
            turbo->second.colors.data(), turbo->second.entryCount());
    }
    else {
        flog::error("Default color map 'Turbo' is missing");
    }

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();
#ifdef __ANDROID__
    androidmenu::init();
#endif

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    // Read every field first and let go of the lock: applying them tunes the
    // source and steps the module manager, which reach for the config themselves.
    double frequency = 0.0;
    bool stickyAutoRange = false;
    float rawMenuWidth = 250.0f;
    float rawFftHeight = 150.0f;
    bool centerTuning = false;
    {
        auto configAccess = core::configManager.read();
        configAccess.tryGet("min", fftMin);
        configAccess.tryGet("max", fftMax);
        stickyAutoRange = configAccess.value("waterfallAutoRange", false);
        configAccess.tryGet("frequency", frequency);
        configAccess.tryGet("showMenu", showMenu);
        rawMenuWidth = configAccess.value("menuWidth", rawMenuWidth);
        rawFftHeight = configAccess.value("fftHeight", rawFftHeight);
        centerTuning = configAccess.value("centerTuning", false);
    }

    autoRange.init(&fftMin, &fftMax, stickyAutoRange);
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMax(fftMax);

    startedWithMenuClosed = !showMenu;

    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);
    bw = 1.0;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();

    // Current configs store logical units and are scaled here. Some test
    // builds wrote already-scaled physical widths without a version marker;
    // keep those as physical so they do not get multiplied again.
    menuWidth = style::scaleOrPhysical(rawMenuWidth, 250.0f);
    newWidth = menuWidth;

    fftHeight = style::scale(rawFftHeight);
    gui::waterfall.setFFTHeight(fftHeight);

    tuningMode = centerTuning ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto& [_name, _vfo] : gui::waterfall.vfos) {
        if (_vfo->lowerOffset < -finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    autostart = core::args["autostart"].b();
    initComplete = true;

    core::moduleManager.doPostInitAll();
}

float* MainWindow::acquireFFTBuffer(void* ctx) {
    return gui::waterfall.getFFTBuffer();
}

void MainWindow::releaseFFTBuffer(void* ctx) {
    gui::waterfall.pushFFT();
}

void MainWindow::onContentScaleChanged(float oldScale) {
    // style::uiScale already holds the new scale when this is called.
    // Rescale physical splitter positions proportionally, then persist logical values.
    menuWidth = style::rescale(menuWidth, oldScale);
    newWidth = menuWidth;
    fftHeight = style::rescale(fftHeight, oldScale);
    gui::waterfall.setFFTHeight(fftHeight);
    auto configAccess = core::configManager.edit();
    configAccess.set("menuWidth", style::unscale(menuWidth));
    configAccess.set("fftHeight", style::unscale(fftHeight));
}

void MainWindow::vfoAddedHandler(VFOManager::VFO* vfo, void* ctx) {
    MainWindow* _this = (MainWindow*)ctx;
    std::string name = vfo->getName();
    double offset;
    {
        auto configAccess = core::configManager.read();
        if (!configAccess.section("vfoOffsets").tryGet(name, offset)) { return; }
    }

    double viewBW = gui::waterfall.getViewBandwidth();
    double viewOffset = gui::waterfall.getViewOffset();

    double viewLower = viewOffset - (viewBW / 2.0);
    double viewUpper = viewOffset + (viewBW / 2.0);

    double newOffset = std::clamp<double>(offset, viewLower, viewUpper);

    sigpath::vfoManager.setCenterOffset(name, _this->initComplete ? newOffset : offset);
}

void MainWindow::draw() {
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    // On a scale change, child windows still carry the previous
    // frame's ContentSize/ScrollbarSizes. ImGui uses those to decide
    // scrollbar visibility for the current frame, so after a scale change
    // FillWidth()/GetContentRegionAvail() inside the left panel and the
    // waterfall-controls column are off by ScrollbarSize for a frame.
    // Clear those values so ImGui re-evaluates from the current frame.
    {
        static uint64_t lastLayoutScaleEpoch = 0;
        uint64_t currentLayoutEpoch = style::scaleEpoch();
        if (currentLayoutEpoch != lastLayoutScaleEpoch) {
            lastLayoutScaleEpoch = currentLayoutEpoch;
            const char* names[] = { "Left Column", "Waterfall", "WaterfallControls" };
            ImGuiWindow* mainWindow = ImGui::GetCurrentWindow();
            for (const char* name : names) {
                invalidateChildLayoutCache(mainWindow, name);
            }
        }
    }

    ImGui::WaterfallVFO* vfo = NULL;
    if (gui::waterfall.selectedVFO != "") {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }

    // Handle VFO movement
    if (vfo != NULL) {
        if (vfo->centerOffsetChanged) {
            if (tuningMode == tuner::TUNER_MODE_CENTER) {
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            } else if (tuningMode == tuner::TUNER_MODE_NORMAL && tuner::vfoLockedToCenter()) {
                // If the VFO is locked to IQ center frequency, then dragging a VFO should be suppressed.
                assert(gui::waterfall.VFOMoveSingleClick);
                // The tuner will synchronize VFO with the IQ center frequency.
                tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            }
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            gui::freqSelect.frequencyChanged = false;
            core::configManager.edit()
                .section("vfoOffsets")
                .set(gui::waterfall.selectedVFO, vfo->generalOffset);
        }
    }

    // Clears wtfVFO->centerOffsetChanged of all VFOs and updates DSP.
    sigpath::vfoManager.updateFromWaterfall(&gui::waterfall);

    // Handle selection of another VFO
    if (gui::waterfall.selectedVFOChanged) {
        gui::freqSelect.setFrequency((vfo != NULL) ? (vfo->generalOffset + gui::waterfall.getCenterFrequency()) : gui::waterfall.getCenterFrequency());
        gui::waterfall.selectedVFOChanged = false;
        gui::freqSelect.frequencyChanged = false;
    }

    // Handle change in selected frequency
    if (gui::freqSelect.frequencyChanged) {
        gui::freqSelect.frequencyChanged = false;
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
        if (vfo != NULL) {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        auto configAccess = core::configManager.edit();
        configAccess.set("frequency", gui::waterfall.getCenterFrequency());
        if (vfo != NULL) {
            configAccess.section("vfoOffsets").set(gui::waterfall.selectedVFO, vfo->generalOffset);
        }
    }

    // Handle dragging the frequency scale
    if (gui::waterfall.centerFreqMoved) {
        gui::waterfall.centerFreqMoved = false;
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + (vfo == NULL ? 0 : vfo->generalOffset));
        core::configManager.edit().set("frequency", gui::waterfall.getCenterFrequency());
    }

    int _fftHeight = gui::waterfall.getFFTHeight();
    if (fftHeight != _fftHeight) {
        fftHeight = _fftHeight;
        core::configManager.edit().set("fftHeight", style::unscale(fftHeight));
    }

#if 1
    {
        FrameDrawArgs frameArgs;
        frameArgs.deltaTime = ImGui::GetIO().DeltaTime;
        frameArgs.frameRate = ImGui::GetIO().Framerate;
        onFrameDraw.emit(frameArgs);
    }
#endif

    // To Bar
    // ImGui::BeginChild("TopBarChild", ImVec2(0, 49.0f * style::uiScale), false, ImGuiWindowFlags_HorizontalScrollbar);
    float topBarWidth = ImGui::GetWindowSize().x;
    // Top of the row. Everything in it is centred on style::topBarRowHeight()
    // from here, rather than each widget carrying its own offset -- those were
    // all tuned at uiScale 1 and drifted apart by up to 22 px on a phone.
    float rowTop = ImGui::GetCursorPosY();
    auto centerInRow = [rowTop](float itemHeight) {
        ImGui::SetCursorPosY(rowTop + style::topBarRowOffset(itemHeight));
    };

    // Whole-pixel icon quads — a fractional size (37.5px at 1.25x) blurs the
    // toolbar icon textures no matter where they land.
    ImVec2 btnSize((float)style::scale(30.0f), (float)style::scale(30.0f));
    int toolbarButtonPadding = style::scale(5.0f);
    // The buttons define the row height, so this is 0; kept explicit so the
    // whole row reads the same way.
    centerInRow(btnSize.y + 2.0f * toolbarButtonPadding);
    ImGui::PushID(ImGui::GetID("sdrpp_menu_btn"));
    bool menuClicked = ImGui::ImageButton(icons::MENU, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false);
#ifdef __ANDROID__
    // Hold the hamburger button to ask about exiting the app; a tap keeps
    // toggling the menu. The release after a long press must not toggle.
    if (ImGui::IsItemActive()) {
        menuBtnHoldTime += ImGui::GetIO().DeltaTime;
        if (!menuBtnLongPressFired && menuBtnHoldTime >= 0.6f) {
            menuBtnLongPressFired = true;
            backend::hapticTick();
            exitDialogRequest = true;
        }
    }
    else {
        if (menuBtnLongPressFired) { menuClicked = false; }
        menuBtnHoldTime = 0.0f;
        menuBtnLongPressFired = false;
    }
#endif
    if (menuClicked) {
        showMenu = !showMenu;
        core::configManager.edit().set("showMenu", showMenu);
    }
    ImGui::PopID();

    ImGui::SameLine();

    bool tmpPlaySate = playing;
    if (playButtonLocked && !tmpPlaySate) { style::beginDisabled(); }
    if (playing) {
        ImGui::PushID(ImGui::GetID("sdrpp_stop_btn"));
        if (ImGui::ImageButton(icons::STOP, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(false);
        }
        ImGui::PopID();
    }
    else { // TODO: Might need to check if there even is a device
        ImGui::PushID(ImGui::GetID("sdrpp_play_btn"));
        if (ImGui::ImageButton(icons::PLAY, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(true);
        }
        ImGui::PopID();
    }
    if (playButtonLocked && !tmpPlaySate) { style::endDisabled(); }

    // Handle auto-start
    if (autostart) {
        autostart = false;
        setPlayState(true);
    }

    ImGui::SameLine();

    // Logo geometry, needed here because the level meter is right-aligned
    // against the space the logo reserves: the logo plus the gap in front of it.
    const float logoSize       = (float)style::scale(32.0f);
    const float logoRightInset = (float)style::scale(16.0f);
    const float meterOffset    = logoSize + logoRightInset + ImGui::GetStyle().ItemSpacing.x;

    // The volume slider and the level meter share the space left over after the
    // fixed elements between them (frequency display, tuning/keypad buttons, and
    // the right-edge reserve). They each take half of that pool; the volume
    // slider is capped, and the meter absorbs whatever it leaves over. The
    // meter's *allocation* is deliberately uncapped: capping it left everything
    // past both maxima with nowhere to go, collecting in the single gap in front
    // of the meter (~627 px on a 1920 px window, against 8 px everywhere else).
    // What it actually draws is capped separately, see meterDrawWidth. The
    // frequency-keypad button is optional: it shows only when it fits without
    // pushing either widget below its minimum.
    float volumeWidth;
    float meterWidth;
    float meterDrawWidth;
    bool  showKeypadButton;
    {
        float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
        float tuningCost  = btnSize.x + 2 * toolbarButtonPadding + itemSpacing; // btn + padding + gap
        float keypadCost  = btnSize.x + 2 * toolbarButtonPadding + itemSpacing;
        float freqCost    = gui::freqSelect.getWidth() + itemSpacing;

        float volMin   = 100.0f * style::uiScale;
        float volMax   = 248.0f * style::uiScale;
        float meterMin = ImGui::GetLevelMeterMinWidth();

        // Space shared by the two flexible widgets. The trailing itemSpacing is
        // the gap in front of the meter; without it the pool was one gap too
        // large and the meter landed a gap right of where it was placed.
        float pool = topBarWidth - ImGui::GetCursorPosX() - freqCost - tuningCost - itemSpacing - meterOffset;
        showKeypadButton = (pool - keypadCost >= volMin + meterMin);
        if (showKeypadButton) { pool -= keypadCost; }

        // Even 50/50 split, honoring the volume cap and handing its surplus to
        // the meter.
        volumeWidth = std::clamp(pool * 0.5f, volMin, volMax);
        meterWidth  = std::max(pool - volumeWidth, meterMin);
        volumeWidth = std::clamp(pool - meterWidth, volMin, volMax);

        // A meter wider than this shows nothing more, it just stretches the
        // scale, so on a large monitor draw at most 350 dp of the slot the split
        // above handed out and let the rest stay empty on its left. The
        // allocation itself is untouched: the volume slider keeps its even half
        // and the surplus still ends up here rather than in a gap elsewhere in
        // the row. meterMin wins if the labels need more than 350 dp.
        meterDrawWidth = std::min(meterWidth, std::max(350.0f * style::uiScale, meterMin));
    }
    sigpath::sinkManager.showVolumeSlider(gui::waterfall.selectedVFO, "##_sdrpp_main_volume_", volumeWidth, btnSize.x, toolbarButtonPadding, true);

    ImGui::SameLine();

    ImGui::SetCursorPosY(rowTop);
    gui::freqSelect.draw();

    ImGui::SameLine();

    if (showKeypadButton) {
        ImGui::SetCursorPosY(rowTop);
        ImGui::PushID(ImGui::GetID("sdrpp_freq_keypad_btn"));
        if (ImGui::ImageButton(icons::KEYPAD, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol)) {
            gui::freqSelect.openKeypad();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    ImGui::SetCursorPosY(rowTop);
    if (tuningMode == tuner::TUNER_MODE_CENTER) {
        ImGui::PushID(ImGui::GetID("sdrpp_ena_st_btn"));
        if (ImGui::ImageButton(icons::CENTER_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_NORMAL;
            gui::waterfall.VFOMoveSingleClick = false;
            core::configManager.edit().set("centerTuning", false);
        }
        ImGui::PopID();
    }
    else { // TODO: Might need to check if there even is a device
        ImGui::PushID(ImGui::GetID("sdrpp_dis_st_btn"));
        if (ImGui::ImageButton(icons::NORMAL_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), toolbarButtonPadding, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_CENTER;
            gui::waterfall.VFOMoveSingleClick = true;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
            core::configManager.edit().set("centerTuning", true);
        }
        ImGui::PopID();
    }

    ImGui::SameLine();

    // meterWidth was computed together with volumeWidth above so the two share
    // the flexible space evenly. Right-align the meter against the logo's
    // reserve, clamping so it can't overlap the tuning button on a very narrow
    // window. Below the 350 dp draw cap this lands exactly one ItemSpacing after
    // the tuning button, so the row has no oversized gap left in it; above it,
    // the meter stays pinned to the right and the slack shows on its left.
    float meterPos = std::max(topBarWidth - (meterDrawWidth + meterOffset), ImGui::GetCursorPosX());

    ImGui::SetCursorPosX(meterPos);
    centerInRow(ImGui::GetLevelMeterHeight());
    ImGui::SetNextItemWidth(meterDrawWidth);
    if (vfo != NULL) {
        ImGui::LevelMeter(gui::waterfall.selectedVFOLevel, gui::waterfall.selectedVFOLevelMax, gui::waterfall.selectedVFOSNR);
    }
    else {
        ImGui::LevelMeter(-INFINITY, -INFINITY, NAN);
    }

    // Keeps the logo on this line: it is placed by absolute cursor position, so
    // without this it would start a new one and double the top bar's height.
    ImGui::SameLine();

    // ImGui::EndChild();

    // Logo button. Was pinned to an absolute 10 dp from the window top, which
    // only happened to look centred at uiScale 1.
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - logoRightInset - logoSize);
    centerInRow(logoSize);
    if (ImGui::ImageButton(icons::LOGO, ImVec2(logoSize, logoSize), ImVec2(0, 0), ImVec2(1, 1), 0)) {
        showCredits = true;
    }

    // Reset waterfall lock
    lockWaterfallControls = showCredits;

    // Handle menu resize
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 mousePos = ImGui::GetMousePos();
#ifdef __ANDROID__
    bool isWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
#endif
    if (showMenu && !grabbingMenu) {
        int clampedMenuWidth = style::clampSplit(menuWidth, winSize.x, 250.0f, 250.0f);
        if (clampedMenuWidth != menuWidth) {
            menuWidth = clampedMenuWidth;
            newWidth = clampedMenuWidth;
        }
    }
    // Menu splitter pill: computed by the splitter handling below, drawn later
    // inside the waterfall child so that floating windows (e.g. the KiwiSDR
    // map popup) paint over it. Shown whenever the touch style is on, on any
    // platform -- a 2 dp hit band cannot be grabbed with a finger, and a desktop
    // with a touch screen has the same problem a phone does.
    bool menuPillVisible = false;
    ImVec2 menuPillCenter;
    if (!lockWaterfallControls && showMenu) {
        float curY = ImGui::GetCursorPosY();
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        float splitBottom = winSize.y - style::dp(10.0f);
        if (grabbingMenu) {
            newWidth = mousePos.x + menuGrabOffset;
            newWidth = style::clampSplit(newWidth, winSize.x, 250.0f, 250.0f);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(newWidth, curY), ImVec2(newWidth, splitBottom), ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
        // Touch handling. A full-length fat hit band fights with the menu
        // scrollbar sitting right against the splitter, so the fat target is a
        // visible pill handle instead: it reaches over the waterfall's dB-scale
        // strip (which takes no input) and grabs on touch-down.
        ImVec2 pillCenter(newWidth + style::dp(4.0f), curY + (splitBottom - curY) * 0.75f);
        float pillHalfH = style::dp(16.0f);
        bool inPillBox = style::touchStyle &&
                         mousePos.x >= newWidth - style::dp(2.0f) && mousePos.x <= newWidth + style::dp(30.0f) &&
                         fabsf(mousePos.y - pillCenter.y) <= pillHalfH + style::dp(10.0f);
        if (style::touchStyle) {
            // The pill hit box overlaps the waterfall child window, so hover of
            // any child of the main window counts for it.
            if (!grabbingMenu && click && inPillBox && !ImGui::IsAnyItemActive() &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                grabbingMenu = true;
                menuGrabOffset = newWidth - mousePos.x;
#ifdef __ANDROID__
                backend::hapticTick();
#endif
            }
            menuPillVisible = true;
            menuPillCenter = pillCenter;
        }

#ifdef __ANDROID__
        // Along the rest of the line a touch is only accepted once the finger's
        // first movement runs across the splitter — a vertical start means
        // scrolling. Android only: it withholds the grab for a few frames, which
        // a mouse user would feel as lag, so the desktop keeps its thin band and
        // gains only the pill above.
        if (menuSplitterPending) {
            float dx = mousePos.x - menuSplitterDownPos.x;
            float dy = mousePos.y - menuSplitterDownPos.y;
            if (!down) {
                menuSplitterPending = false;
            }
            else if (std::max(fabsf(dx), fabsf(dy)) >= style::dp(6.0f)) {
                menuSplitterPending = false;
                if (fabsf(dx) > fabsf(dy)) {
                    grabbingMenu = true;
                    menuGrabOffset = newWidth - menuSplitterDownPos.x;
                    backend::hapticTick();
                }
            }
        }
        else if (!grabbingMenu && click && !inPillBox) {
            if (isWindowHovered && fabsf(mousePos.x - (float)newWidth) <= style::dp(20.0f) && mousePos.y > curY) {
                menuSplitterPending = true;
                menuSplitterDownPos = mousePos;
            }
        }
#else
        // The hit band straddles the Left Column and Waterfall children (and
        // under the touch style FindHoveredWindow() grows their hover boxes by
        // TouchExtraPadding, right over the band), so hover of any child of the
        // main window counts for it. Floating windows and popups are separate
        // top-level windows and still correctly block the splitter.
        float separatorHitRadius = style::dp(2.0f);
        // inPillBox so the resize cursor also appears over the pill when the
        // touch style is on; the grab itself was already taken above.
        bool overSplit = inPillBox || (fabsf(mousePos.x - (float)newWidth) <= separatorHitRadius && mousePos.y > curY);
        if (grabbingMenu || (overSplit && !ImGui::IsAnyItemActive() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (click && !grabbingMenu) {
                grabbingMenu = true;
                menuGrabOffset = newWidth - mousePos.x;
            }
        }
        else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
#endif
        if (!down && grabbingMenu) {
            grabbingMenu = false;
            menuWidth = newWidth;
            core::configManager.edit().set("menuWidth", style::unscale(menuWidth));
#ifdef __ANDROID__
            backend::hapticTick();
#endif
        }
    }

    // Process menu keybinds
    displaymenu::checkKeybinds();

    // Left Column
    if (showMenu) {
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, menuWidth);
        ImGui::SetColumnWidth(1, std::max<int>(winSize.x - menuWidth - (60.0f * style::uiScale), 100.0f * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
        ImGui::BeginChild("Left Column");

        if (gui::menu.draw(firstMenuRender)) {
            json arr = json::array();
            for (int i = 0; i < gui::menu.order.size(); i++) {
                arr[i]["name"] = gui::menu.order[i].name;
                arr[i]["open"] = gui::menu.order[i].open;
            }

            // isEnabled() is plugin-owned code. Query it before taking the core
            // config lock so a plugin cannot block the lock or attempt a nested
            // access to its own ConfigManager.
            std::vector<std::pair<std::string, bool>> instanceStates;
            instanceStates.reserve(core::moduleManager.instances.size());
            for (const auto& [_name, inst] : core::moduleManager.instances) {
                instanceStates.emplace_back(_name, inst.instance->isEnabled());
            }

            auto configAccess = core::configManager.edit();
            configAccess.set("menuElements", arr);

            // Update enabled and disabled modules
            ConfigManager::EditSection instances = configAccess.section("moduleInstances");
            for (const auto& [_name, enabled] : instanceStates) {
                if (!instances.contains(_name)) { continue; }
                instances.section(_name).set("enabled", enabled);
            }
        }
        if (startedWithMenuClosed) {
            startedWithMenuClosed = false;
        }
        else {
            firstMenuRender = false;
        }

#ifdef SDRPP_ENABLE_DEBUG_MENU
        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Text("Frame time: %.3f ms/frame", ImGui::GetIO().DeltaTime * 1000.0f);
            ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
            ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
            ImGui::Text("Source name: %s", sourceName.c_str());
            ImGui::Checkbox("Show demo window", &demoWindow);
            ImGui::Text("ImGui version: %s", ImGui::GetVersion());

            // ImGui::Checkbox("Bypass buffering", &sigpath::iqFrontEnd.inputBuffer.bypass);

            // ImGui::Text("Buffering: %d", (sigpath::iqFrontEnd.inputBuffer.writeCur - sigpath::iqFrontEnd.inputBuffer.readCur + 32) % 32);

            if (ImGui::Button("Test Bug")) {
                flog::error("Will this make the software crash?");
            }

            if (ImGui::Button("Testing something")) {
                gui::menu.order[0].open = true;
                firstMenuRender = true;
            }

            ImGui::Checkbox("WF Single Click", &gui::waterfall.VFOMoveSingleClick);
            ImGui::Checkbox("Lock Menu Order", &gui::menu.locked);

            ImGui::Spacing();
        }
#endif

        ImGui::EndChild();
    }
    else {
        // When hiding the menu bar
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, 8 * style::uiScale);
        ImGui::SetColumnWidth(1, winSize.x - ((8 + 60) * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
    }

    // Right Column
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::NextColumn();
    ImGui::PopStyleVar();

    ImGui::BeginChild("Waterfall");

    gui::waterfall.draw();

    // Menu splitter drag handle (see the splitter handling above). Drawn into
    // this child's draw list so floating windows and popups paint over it, with
    // an expanded clip rect since it straddles the child's left edge. The dark
    // backing keeps it readable on any theme.
    if (menuPillVisible) {
        float halfW = style::dp(grabbingMenu ? 6.0f : 4.5f);
        float halfH = style::dp(16.0f) + (grabbingMenu ? style::dp(4.0f) : 0.0f);
        float pad = style::dp(2.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(ImVec2(menuPillCenter.x - halfW - pad, menuPillCenter.y - halfH - pad), ImVec2(menuPillCenter.x + halfW + pad, menuPillCenter.y + halfH + pad), IM_COL32(0, 0, 0, 120), halfW + pad);
        dl->AddRectFilled(ImVec2(menuPillCenter.x - halfW, menuPillCenter.y - halfH), ImVec2(menuPillCenter.x + halfW, menuPillCenter.y + halfH),
                          grabbingMenu ? ImGui::GetColorU32(ImGuiCol_SeparatorActive) : IM_COL32(210, 210, 210, 200), halfW);
        dl->PopClipRect();
    }

    ImGui::EndChild();

    if (!lockWaterfallControls) {
        // Handle arrow keys
        if (vfo != NULL && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            bool freqChanged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset - vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (freqChanged) {
                auto configAccess = core::configManager.edit();
                configAccess.set("frequency", gui::waterfall.getCenterFrequency());
                if (vfo != NULL) {
                    configAccess.section("vfoOffsets").set(gui::waterfall.selectedVFO, vfo->generalOffset);
                }
            }
        }

        // Handle scrollwheel. Precision touchpads and free-spinning wheels
        // report fractional deltas that a plain int cast would drop, so
        // accumulate and only tune on whole steps. The accumulator is
        // cleared outside the FFT/waterfall area (and while Ctrl-zooming)
        // so partial scrolls elsewhere can't discharge into a tune later.
        if (!ImGui::GetIO().KeyCtrl && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            wheelAccum += ImGui::GetIO().MouseWheel;
        }
        else {
            wheelAccum = 0.0f;
        }
        int wheel = (int)wheelAccum;
        if (wheel != 0) {
            wheelAccum -= (float)wheel;
            double nfreq;
            if (vfo != NULL) {
                // Select factor depending on modifier keys
                double interval;
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                    interval = vfo->snapInterval * 10.0;
                }
                else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                    interval = vfo->snapInterval * 0.1;
                }
                else {
                    interval = vfo->snapInterval;
                }

                nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + (interval * wheel);
                nfreq = roundl(nfreq / interval) * interval;
            }
            else {
                nfreq = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() * wheel / 20.0);
            }
            tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
            gui::freqSelect.setFrequency(nfreq);
            auto configAccess = core::configManager.edit();
            configAccess.set("frequency", gui::waterfall.getCenterFrequency());
            if (vfo != NULL) {
                configAccess.section("vfoOffsets").set(gui::waterfall.selectedVFO, vfo->generalOffset);
            }
        }
    }

    ImGui::NextColumn();
    drawWaterfallControls(vfo, textCol);

    // Continuous ("sticky") auto-range steps here (outside the controls child)
    // so it keeps tracking even when the menu is collapsed.
    autoRange.update();

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

#ifdef __ANDROID__
    // Exit confirmation, requested by holding the hamburger menu button.
    // Back with the dialog open lands in handleBackPress()'s popup branch
    // and cancels it.
    if (exitDialogRequest) {
        exitDialogRequest = false;
        ImGui::OpenPopup("Exit##sdrpp_exit_confirm");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Exit##sdrpp_exit_confirm", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(playing ? "Stop the radio and exit the application?" : "Exit the application?");
        ImGui::Spacing();
        ImVec2 exitBtnSize(style::dp(120.0f), 0.0f);
        if (ImGui::Button("Cancel##sdrpp_exit_cancel", exitBtnSize)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit##sdrpp_exit_ok", exitBtnSize)) {
            setPlayState(false);
            backend::finishAppAndRemoveTask();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif

    ImGui::End();

    if (showCredits) {
        showCredits = credits::show();
    }

#ifdef SDRPP_ENABLE_DEBUG_MENU
    if (demoWindow) {
        ImGui::ShowDemoWindow();
    }
#endif
}

// Right-hand strip: Zoom / Range / Ref sliders and the auto-range button.
void MainWindow::drawWaterfallControls(ImGui::WaterfallVFO* vfo, const ImVec4& textCol) {
    // The strip must fit without scrolling: a scrollbar here is as wide as the
    // sliders themselves and would eat a third of this already narrow column
    // (and offset the centering, which is computed from the full window width).
    // The layout below sizes the sliders to the exact available height, so the
    // flags only matter as a guard for a pathologically short window.
    ImGui::BeginChild("WaterfallControls", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Rubber-band vertical layout for the sliders + their labels + the
    // autoscale button. The budget is measured once, before drawing anything,
    // so every leftover pixel goes to the sliders while the labels and the
    // button are always accounted for and can never be squeezed out.
    const ImVec2 avail      = ImGui::GetContentRegionAvail();
    const float textH       = ImGui::GetTextLineHeight();
    const float spacingY    = ImGui::GetStyle().ItemSpacing.y;
    const float btnSide     = WaterfallAutoRange::buttonSize(avail.x);
    const float idealSlider = 150.0f * style::uiScale;
    const float minSlider   = 16.0f * style::uiScale; // 40 dp will not fit a 720p landscape strip

    // The strip is flush with the screen edges, and on a phone the window covers
    // the physical display corners: nothing here applies display insets (the
    // main window is placed at 0,0 over the full DisplaySize). A corner inset by
    // d on both axes clears a corner radius of up to 3.41*d, so keep the square
    // button off the bottom edge -- centring it already gives ~30 px of
    // horizontal inset on a phone, and a matching bottom margin covers radii to
    // ~102 px, past anything current phones use. Desktop windows are not
    // clipped, so no margin there.
    const float bottomMargin = style::dp(style::touchStyle ? 10.0f : 0.0f);
    const float budgetH      = avail.y - bottomMargin;

    // Slider track width. 20 dp is fine for a mouse, but on a phone it is ~3.6 mm
    // against a ~7.6 mm minimum touch target, in a column with 6 mm of unused
    // width either side -- so the touch style gets a much wider track, and the
    // grab is bumped with it where it is pushed below.
    const float sliderW = std::floor(std::min(style::dp(style::touchStyle ? 40.0f : 20.0f), avail.x - 2.0f * style::dp(6.0f)));

    // Fixed (non-slider) cost: one label per slider, the autoscale button, and
    // the gap ImGui inserts between consecutive items. That inter-item gap has
    // to be counted explicitly: each slider carries one of its own, so
    // budgeting the labels as whole text lines (glyphs + one gap) leaves one
    // gap per slider unpaid and overflows the child by exactly that much. The
    // blank-line separators (one after each slider) are added only if the
    // ideal-height layout still fits with them. The same cost drives that
    // choice, the Ref decision and the slider height, so the computed layout
    // matches what actually gets drawn.
    auto fixedCost = [&](int sliders, bool separators) {
        const int textLines = sliders * (separators ? 2 : 1); // labels [+ a blank line after each slider]
        const int items     = textLines + sliders + 1;        // + the sliders themselves + the button
        return textLines * textH + btnSide + (items - 1) * spacingY;
    };

    // Continuous auto-range owns Ref, and the FFT's dB axis (drawFFT's vertical
    // scale) already reports the level it tracks -- so on a strip too short for
    // full-height sliders, drop the slider while latched and hand its pixels to
    // Zoom and Range. That is worth doing only while the remaining two sliders
    // would still be short of their ideal height: once they reach it, the freed
    // space would just pad the strip and strand the button above the bottom, so
    // Ref stays visible (disabled) instead. On a landscape phone this takes the
    // two live sliders from ~10 mm of travel to ~17 mm; in portrait and on the
    // desktop nothing changes.
    const bool twoSlidersReachIdeal = (2.0f * idealSlider + fixedCost(2, true)) <= budgetH;
    const bool showRef     = !autoRange.sticky() || twoSlidersReachIdeal;
    const int  sliderCount = showRef ? 3 : 2;

    const bool sliderSeparators = ((float)sliderCount * idealSlider + fixedCost(sliderCount, true)) <= budgetH;

    float sliderH = (budgetH - fixedCost(sliderCount, sliderSeparators)) / (float)sliderCount;
    sliderH = std::max<float>(minSlider, std::min<float>(sliderH, idealSlider));
    const ImVec2 wfSliderSize(sliderW, std::floor(sliderH));

    // Labels, sliders and the button are all narrower than the strip and share
    // one centre line.
    auto centerFor = [](float width) {
        ImGui::SetCursorPosX(std::round((ImGui::GetWindowSize().x - width) * 0.5f));
    };

    // Sliders only: a wider track deserves a grab you can see (see sliderW).
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, style::dp(style::touchStyle ? 18.0f : 10.0f));

    centerFor(ImGui::CalcTextSize("Zoom").x);
    ImGui::TextUnformatted("Zoom");
    centerFor(sliderW);
    bool zoomSliderChanged = ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0, 0.0, "");
    // Applies to the last submitted item: keeps wheel events over the slider
    // from scrolling the child window under it.
    ImGui::SetItemUsingMouseWheel();
    if (zoomSliderChanged) {
        double factor = (double)bw * (double)bw;

        // Map 0.0 -> 1.0 to 1000.0 -> bandwidth
        double wfBw = gui::waterfall.getBandwidth();
        double delta = wfBw - 1000.0;
        double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);

        gui::waterfall.setViewBandwidth(finalBw);
        if (vfo != NULL) {
            gui::waterfall.setViewOffset(vfo->centerOffset); // center vfo on screen
        }
    }

    if (sliderSeparators) ImGui::NewLine();

    // Range = displayed dynamic range / contrast (fftMax - fftMin). Always
    // live: the user owns contrast even while sticky auto-range tracks Ref.
    // Dragging it pivots on Ref (the floor), extending the top.
    float wfRange = fftMax - fftMin;
    centerFor(ImGui::CalcTextSize("Range").x);
    ImGui::TextUnformatted("Range");
    centerFor(sliderW);
    bool rangeSliderChanged = ImGui::VSliderFloat("##_8_", wfSliderSize, &wfRange, WaterfallAutoRange::RANGE_MIN, WaterfallAutoRange::RANGE_MAX, "");
    ImGui::SetItemUsingMouseWheel();
    if (rangeSliderChanged) {
        fftMax = fftMin + wfRange;
        auto configAccess = core::configManager.edit();
        configAccess.set("min", fftMin);
        configAccess.set("max", fftMax);
    }

    if (sliderSeparators) ImGui::NewLine();

    // Ref = noise-floor level (fftMin, bottom of the window). Sticky auto-range
    // drives it, so grey it out while sticky (and see showRef above: on a short
    // strip it is dropped rather than greyed). Dragging it slides the whole
    // window, preserving Range.
    if (showRef) {
        float wfRef = fftMin;
        ImGui::BeginDisabled(autoRange.sticky());
        centerFor(ImGui::CalcTextSize("Ref").x);
        ImGui::TextUnformatted("Ref");
        centerFor(sliderW);
        bool refSliderChanged = ImGui::VSliderFloat("##_9_", wfSliderSize, &wfRef, WaterfallAutoRange::REF_MIN, WaterfallAutoRange::REF_MAX, "");
        ImGui::SetItemUsingMouseWheel();
        if (refSliderChanged) {
            float range = fftMax - fftMin; // keep the user's Range, slide the window
            fftMin = wfRef;
            fftMax = wfRef + range;
            auto configAccess = core::configManager.edit();
            configAccess.set("min", fftMin);
            configAccess.set("max", fftMax);
        }
        ImGui::EndDisabled();

        if (sliderSeparators) ImGui::NewLine();
    }

    ImGui::PopStyleVar(); // GrabMinSize

    centerFor(btnSide);
    autoRange.drawButton(btnSide, textCol);

    ImGui::EndChild();
}

void MainWindow::setPlayState(bool _playing) {
    if (_playing == playing) { return; }
    if (_playing) {
        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.start();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        playing = true;
        onPlayStateChange.emit(true);
    }
    else {
        playing = false;
        onPlayStateChange.emit(false);
        sigpath::sourceManager.stop();
        sigpath::iqFrontEnd.flushInputBuffer();
    }
}

void MainWindow::setViewBandwidthSlider(float bandwidth) {
    bw = bandwidth;
}

bool MainWindow::sdrIsRunning() {
    return playing;
}

bool MainWindow::isPlaying() {
    return playing;
}

bool MainWindow::handleBackPress() {
    // Runs on the app thread between frames (same thread as the render loop),
    // so mutating popup state here is safe.
    ImGuiContext* g = ImGui::GetCurrentContext();

    // Topmost open popup: combo dropdowns, context menus, modal dialogs. The
    // credits overlay is one of these (BeginPopupModal), so it is dismissed
    // here; credits::show() notices its popup is gone and clears showCredits
    // on the next frame.
    if (g && g->OpenPopupStack.Size > 0) {
        ImGui::ClosePopupToLevel(g->OpenPopupStack.Size - 1, true);
        return true;
    }

    // Fallback for the credits overlay, in case a second Back arrives before
    // the frame that would have cleared the flag above. show() never runs
    // again in that case, so its lifecycle state has to be dropped here or the
    // next presentation opens nothing.
    if (showCredits) {
        showCredits = false;
        credits::reset();
        return true;
    }

    // Deliberately NOT dismissed by Back: the menu panel (it is the primary
    // control surface, not a navigation level) and the app itself (exit goes
    // through holding the menu button instead).
    return false;
}

void MainWindow::setFirstMenuRender() {
    firstMenuRender = true;
}
