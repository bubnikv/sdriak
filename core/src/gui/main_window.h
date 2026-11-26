#pragma once
#include <imgui/imgui.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <mutex>
#include <atomic>
#include <deque>
#include <functional>
#include <thread>
#include <gui/tuner.h>
#include <gui/waterfall_autorange.h>
#include <signal_path/server_zoom.h>

class SpectrumProvider;

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    // Move every VFO that sticks out of the waterfall span back inside it.
    void clampVFOsToSpan();
    // core::setDisplaySpan(): the span the source's server now shows.
    void displaySpanConfirmed(double spanHz);
    // Why the source's server waterfall fell back to the local FFT, or "".
    const std::string& spectrumFallbackNotice() const { return spectrumNotice; }
    bool sdrIsRunning();
    void setFirstMenuRender();
    void onContentScaleChanged(float oldScale);

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    void runOnUiThread(std::function<void()> action);
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
    // UI tick: follow the provider's zoom ladder and send wheel steps.
    void serverZoomTick();
    // UI tick: fall back to the local FFT when the provider is unavailable,
    // and switch waterfall levels when the provider changes.
    void spectrumProviderTick();
    // Waterfall levels per provider: config waterfallLevels[<name>|"local"].
    void loadLevels(const std::string& key, const SpectrumProvider* provider);
    void persistLevels();

    std::string levelsKey;
    std::string spectrumNotice;

    ServerZoom serverZoom;

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
    std::atomic<bool> playing{false};
    std::thread::id uiThread;
    std::mutex uiActionsMtx;
    std::deque<std::function<void()>> uiActions;
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
