#pragma once
#include <imgui/imgui.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <mutex>
#include <gui/tuner.h>
#include <gui/waterfall_autorange.h>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    bool sdrIsRunning();
    void setFirstMenuRender();
    void onContentScaleChanged(float oldScale);

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    // Android Back: dismiss the topmost UI layer (popup/modal, then the
    // credits overlay). Returns false when nothing was left to dismiss, in
    // which case the caller decides what leaving the app means.
    bool handleBackPress();

    // Active tuning mode: tuner::TUNER_MODE_CENTER or tuner::TUNER_MODE_NORMAL
    int getTuningMode() const { return tuningMode; }

    struct FrameDrawArgs {
        double deltaTime = 0.0;
        double frameRate = 0.0;
    };

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    Event<bool> onPlayStateChange;
    Event<FrameDrawArgs> onFrameDraw;

private:
    static void vfoAddedHandler(VFOManager::VFO* vfo, void* ctx);

    // Right-hand strip of the main window: Zoom / Range / Ref sliders and the
    // auto-range button. Drawn from draw(), into the third window column.
    void drawWaterfallControls(ImGui::WaterfallVFO* vfo, const ImVec4& textCol);

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    // Continuous ("sticky") waterfall auto-range behind the autoscale button.
    WaterfallAutoRange autoRange;
    float bw = 8000000;
    float wheelAccum = 0.0f;
    bool playing = false;
    bool showCredits = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    float menuGrabOffset = 0.0f;      // grab-point offset so the splitter doesn't jump to the finger/cursor
    bool menuSplitterPending = false; // Android: touch near the splitter, drag direction not decided yet
    ImVec2 menuSplitterDownPos;
    int fftHeight = 300;
    bool showMenu = true;
    bool exitDialogRequest = false;      // Android: set by holding the menu button
    float menuBtnHoldTime = 0.0f;        // Android: hamburger hold-to-exit
    bool menuBtnLongPressFired = false;
    int tuningMode = tuner::TUNER_MODE_NORMAL;
    dsp::stream<dsp::complex_t> dummyStream;
    // Used only under SDRPP_ENABLE_DEBUG_MENU, but kept unconditional: the
    // define is not visible to out-of-tree plugins, so a conditional member
    // would change the class layout across the module ABI.
    bool demoWindow = false;
    int selectedWindow = 0;

    bool initComplete = false;
    bool autostart = false;

    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
};