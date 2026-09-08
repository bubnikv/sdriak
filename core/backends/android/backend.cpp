#include <backend.h>
#include <backend_common.h>
#include "android_backend.h"
#include <core.h>
#include <gui/gui.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <gui/icons.h>
#include <gui/style.h>
#include <gui/menus/theme.h>
#include <filesystem>

// Credit to the ImGui android OpenGL3 example for a lot of this code!

namespace backend {
    struct android_app* app = NULL;
    EGLDisplay _EglDisplay = EGL_NO_DISPLAY;
    EGLSurface _EglSurface = EGL_NO_SURFACE;
    EGLContext _EglContext = EGL_NO_CONTEXT;
    char _LogTag[] = "SDRIAK";
    bool initialized = false;

    void saveLifecycleState(const char* event) {
        if (!core::saveState()) {
            flog::error("One or more configs could not be saved during {}", event);
        }
    }
    bool pauseRendering = false;
    std::atomic<bool> sleepScreenDimmed{false};   // True during DIM and DARK phases (set via JNI)
    std::atomic<bool> sleepRenderPaused{false};   // True during DARK phase only (set via JNI)
    std::atomic<bool> sleepBlackFrameSent{false}; // Ensures one black frame before pausing
    std::atomic<bool> audioOutputOpenSLES{false};
    std::atomic<bool> backInvokedPending{false};  // Back via OnBackInvokedCallback (set on UI thread via JNI)
    std::atomic<int> usbHotplugGeneration{0};
    std::atomic<int> audioRoutingGeneration{0};
    bool exited = false;
    static bool wasPlayingBeforeSuspend = false;
    static bool restartOnResume = true;
    static common::ScaleState scaleState;
    // Sleep-reset heartbeat state — accessed only from the app thread (same thread as render loop).
    static bool sleepResetMotionPending = false;
    static std::chrono::steady_clock::time_point sleepResetLastCall{};

    // Forward declarations
    int ShowSoftKeyboardInput();
    int PollUnicodeChars();
    float getContentScale();
    static void resetInputRecognizers();
    static UsbDeviceHandle getUsbDeviceHandle(const std::vector<DevVIDPID>& allowedVidPids);
    static bool releaseUsbDeviceHandle(const UsbDeviceHandle& handle);
    static bool callActivityVoidMethod(const char* name);

    // Encapsulates the JNI per-call lifecycle: GetEnv → AttachCurrentThread →
    // GetObjectClass(activity). Detaches automatically on destruction.
    // Check valid() before use; env and clazz are accessible individually for
    // callers that need to distinguish attach failure from class-lookup failure.
    struct JniSession {
        JNIEnv* env   = nullptr;
        jclass  clazz = nullptr;

        JniSession() {
            if (!app || !app->activity || !app->activity->vm) return;
            vm_ = app->activity->vm;
            jint ret = vm_->GetEnv((void**)&env, JNI_VERSION_1_6);
            if (ret == JNI_EDETACHED) {
                if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) { env = nullptr; return; }
                attached_ = true;  // only detach in destructor if we did the attach
            } else if (ret != JNI_OK) {
                return;  // JNI_EVERSION or other error
            }
            clazz = env->GetObjectClass(app->activity->clazz);
        }
        ~JniSession() { if (attached_) vm_->DetachCurrentThread(); }

        bool valid() const { return env != nullptr && clazz != nullptr; }

        JniSession(const JniSession&) = delete;
        JniSession& operator=(const JniSession&) = delete;

    private:
        JavaVM* vm_       = nullptr;
        bool    attached_ = false;
    };

    static bool callActivityVoidMethod(const char* name) {
        JniSession jni;
        if (!jni.valid()) return false;
        jmethodID method = jni.env->GetMethodID(jni.clazz, name, "()V");
        if (!method) return false;
        jni.env->CallVoidMethod(app->activity->clazz, method);
        return true;
    }

    void setRestartOnResume(bool value) { restartOnResume = value; }

    void hapticTick() { callActivityVoidMethod("hapticTick"); }

    void finishAppAndRemoveTask() { callActivityVoidMethod("finishAppAndRemoveTask"); }

    int startSleepTimer() {
        sleepResetLastCall = std::chrono::steady_clock::now();
        return callActivityVoidMethod("startSleepTimer") ? 0 : -1;
    }
    int stopSleepTimer()        { return callActivityVoidMethod("stopSleepTimer")     ? 0 : -1; }
    int suspendSleepTimer()     { return callActivityVoidMethod("suspendSleepTimer")  ? 0 : -1; }
    int resumeSleepTimer() {
        sleepResetLastCall = std::chrono::steady_clock::now();
        return callActivityVoidMethod("resumeSleepTimer") ? 0 : -1;
    }
    int resetSleepToActive() {
        sleepResetLastCall = std::chrono::steady_clock::now();
        return callActivityVoidMethod("resetSleepToActive") ? 0 : -1;
    }

    void setSleepTimerConfig(int mode, int dimAfterSec, int darkAfterSec) {
        JniSession jni;
        if (!jni.valid()) return;
        jmethodID method = jni.env->GetMethodID(jni.clazz, "setSleepTimerConfig", "(III)V");
        if (!method) return;
        jni.env->CallVoidMethod(app->activity->clazz, method, (jint)mode, (jint)dimAfterSec, (jint)darkAfterSec);
    }

    void setUserScaleFactor(float newFactor) {
        common::setUserScaleFactor(scaleState, newFactor);
    }

    void doPartialInit() {
        std::string root = (std::string)core::args["root"];
        std::string resDir = root + "/res";
        float oldScale = style::uiScale;
        backend::init(resDir);
        common::setScaleFromConfig(scaleState, getContentScale());
        style::loadFonts(resDir); // TODO: Don't hardcode, use config
        icons::load(resDir);
        style::applyScaledStyle(thememenu::applyTheme);
        if (std::fabs(style::uiScale - oldScale) >= 0.0001f) {
            gui::mainWindow.onContentScaleChanged(oldScale);
        }
        gui::mainWindow.setFirstMenuRender();
    }

    static void handleImGuiAppFocusChange(bool focused) {
        if (!focused) {
            // These recognizers outlive the ImGui context across TERM_WINDOW.
            resetInputRecognizers();
        }
        if (!ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();
        io.AddFocusEvent(focused);
        if (!focused) {
            // Android can interrupt a gesture without delivering its UP event.
            // Release every emulated mouse button so UI gesture latches cannot
            // survive switching applications or losing the native window.
            for (int button = 0; button < ImGuiMouseButton_COUNT; button++) {
                io.AddMouseButtonEvent(button, false);
            }
        }
    }

    void handleAppCmd(struct android_app* app, int32_t appCmd) {
        switch (appCmd) {
        case APP_CMD_SAVE_STATE:
            flog::warn("APP_CMD_SAVE_STATE");
            saveLifecycleState("APP_CMD_SAVE_STATE");
            break;
        case APP_CMD_PAUSE:
            flog::warn("APP_CMD_PAUSE");
            handleImGuiAppFocusChange(false);
            wasPlayingBeforeSuspend = gui::mainWindow.sdrIsRunning();
            // Android may kill the process while it is backgrounded. Commit
            // while the radio interface is still alive, then force the config
            // to disk instead of waiting for the auto-save interval.
            saveLifecycleState("APP_CMD_PAUSE");
            gui::mainWindow.setPlayState(false);
            break;
        case APP_CMD_STOP:
            flog::warn("APP_CMD_STOP");
            saveLifecycleState("APP_CMD_STOP");
            break;
        case APP_CMD_RESUME:
            flog::warn("APP_CMD_RESUME");
            if (sleepScreenDimmed) {
                resetSleepToActive();
            }
            {
                bool shouldRestart = wasPlayingBeforeSuspend && restartOnResume;
                wasPlayingBeforeSuspend = false;
                if (shouldRestart) {
                    gui::mainWindow.setPlayState(true);
                }
            }
            break;
        case APP_CMD_INIT_WINDOW:
            flog::warn("APP_CMD_INIT_WINDOW");
            if (pauseRendering && !exited) {
                doPartialInit();
                pauseRendering = false;
            }
            exited = false;
            resumeSleepTimer();
            break;
        case APP_CMD_TERM_WINDOW:
            flog::warn("APP_CMD_TERM_WINDOW");
            handleImGuiAppFocusChange(false);
            suspendSleepTimer();
            pauseRendering = true;
            backend::end();
            break;
        case APP_CMD_GAINED_FOCUS:
            flog::warn("APP_CMD_GAINED_FOCUS");
            handleImGuiAppFocusChange(true);
            break;
        case APP_CMD_LOST_FOCUS:
            flog::warn("APP_CMD_LOST_FOCUS");
            handleImGuiAppFocusChange(false);
            break;
        case APP_CMD_CONFIG_CHANGED:
            flog::warn("APP_CMD_CONFIG_CHANGED");
            common::queueContentScale(scaleState, getContentScale());
            break;
        }
    }

    // Set when a wake-tap ACTION_DOWN is consumed; keeps the rest of that gesture consumed
    // even after sleepScreenDimmed is cleared by the async resetSleepToActive() call.
    // Only accessed from the input-handler thread — no need for atomic.
    static bool consumingWakeGesture = false;

    // ---------------------------------------------------------------------------
    // Multi-touch gesture recognizer — pinch-to-zoom
    // Runs on the input-handler thread (same thread as the render loop in
    // NativeActivity), so no locking is required.
    // ---------------------------------------------------------------------------
    struct GestureRecognizer {
        enum class State { IDLE, PINCHING };
        State state = State::IDLE;
        float p[2][2] = {};   // [pointer_index][x, y]
        float lastDist = 0.0f;
        int32_t firstFingerId = -1;   // pointer id of the finger that started the gesture

        // Returns 1 if the event was consumed (do NOT forward to ImGui).
        int32_t handle(AInputEvent* ev) {
            if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;

            int32_t raw    = AMotionEvent_getAction(ev);
            int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
            int32_t count  = static_cast<int32_t>(AMotionEvent_getPointerCount(ev));

            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN && count == 2) {
                // Second finger arrived — begin pinch tracking.
                for (int i = 0; i < 2; i++) {
                    p[i][0] = AMotionEvent_getX(ev, i);
                    p[i][1] = AMotionEvent_getY(ev, i);
                }
                // The finger that was already down (i.e. not the one this event
                // reports as going down) is the anchor of the zoom; remember it
                // by pointer id since indices reshuffle as fingers come and go.
                int32_t downIdx = (raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                firstFingerId = AMotionEvent_getPointerId(ev, downIdx == 0 ? 1 : 0);
                lastDist = fingerDist();
                state = State::PINCHING;
                // Release any ImGui mouse button so pending drags don't leak.
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                return 1;
            }

            if (state == State::IDLE) return 0;

            // ---- PINCHING state: consume all motion events ----
            if (action == AMOTION_EVENT_ACTION_MOVE && count >= 2) {
                for (int i = 0; i < 2; i++) {
                    p[i][0] = AMotionEvent_getX(ev, i);
                    p[i][1] = AMotionEvent_getY(ev, i);
                }
                float d     = fingerDist();
                float delta = d - lastDist;
                lastDist    = d;

                // Keep the ImGui cursor on the first finger: the user tunes with
                // it, so the waterfall anchors the zoom on the frequency under it
                // (and the mouseIn* hit-tests resolve to the right region).
                int32_t firstIdx = 0;
                for (int32_t i = 0; i < count; i++) {
                    if (AMotionEvent_getPointerId(ev, i) == firstFingerId) {
                        firstIdx = i;
                        break;
                    }
                }
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(AMotionEvent_getX(ev, firstIdx),
                                    AMotionEvent_getY(ev, firstIdx));

                // Carry zoom via the horizontal wheel axis.  Using H-wheel (X)
                // instead of Ctrl+V-wheel avoids any key-state timing dependency:
                // MouseWheelH is accumulated per-frame independently of KeyCtrl,
                // so it is safe even when MOVE and POINTER_UP land in the same
                // pre-frame input batch.
                // Positive delta (fingers apart) = zoom in = positive wheel.
                // Sensitivity: ~60 px of finger separation = 1 scroll unit.
                if (fabsf(delta) > 0.5f) {
                    io.AddMouseWheelEvent(delta / 60.0f, 0.0f);
                }
                return 1;
            }

            if (action == AMOTION_EVENT_ACTION_POINTER_UP ||
                action == AMOTION_EVENT_ACTION_UP         ||
                action == AMOTION_EVENT_ACTION_CANCEL) {
                state = State::IDLE;
                return 1;
            }

            return 1;  // consume all other events while pinching
        }

        void reset() {
            state = State::IDLE;
            firstFingerId = -1;
            lastDist = 0.0f;
        }

    private:
        float fingerDist() const {
            float dx = p[1][0] - p[0][0];
            float dy = p[1][1] - p[0][1];
            return std::sqrt(dx * dx + dy * dy);
        }
    };

    static GestureRecognizer gestureRecognizer;

    // ---------------------------------------------------------------------------
    // Single-finger drag-scroll recognizer.
    //
    // ImGui widgets arm on mouse-down, so the only safe way to touch-scroll is to
    // withhold the press from ImGui until the gesture is classified:
    //   - mostly-vertical move past the slop   -> pan the scrollable window under
    //     the finger directly (1:1 tracking + fling); ImGui never sees the press,
    //     so no widget can activate under a scroll gesture;
    //   - mostly-horizontal move past the slop -> replay the press at the
    //     touch-down position and stream events normally (sliders keep working);
    //   - motionless hold                      -> replay the press (kept for
    //     press-and-hold interactions such as menu drag-reorder);
    //   - finger up before any of the above    -> replay press+release as a tap;
    //     the input queue trickles it over two frames so the click lands.
    //
    // Touches with no scrollable window under them (waterfall, top bar), on a
    // scrollbar, from a mouse/stylus, or blocked by a modal pass through with no
    // added latency. Timing and fling velocity use hardware event timestamps
    // (including historical samples), so classification survives render stalls.
    // Runs on the app thread like everything else here — no locking.
    // ---------------------------------------------------------------------------
    struct TouchScrollRecognizer {
        enum class State { IDLE, PENDING, PANNING, PASSTHROUGH, FLING };
        State state = State::IDLE;

        static constexpr float TOUCH_SLOP_DP         = 8.0f;    // Android standard touch slop
        static constexpr int   HOLD_TIMEOUT_MS       = 200;     // motionless press commits as press/long-press
        static constexpr float MIN_FLING_VEL_DP      = 50.0f;   // dp/s (Android ViewConfiguration)
        static constexpr float MAX_FLING_VEL_DP      = 6000.0f; // dp/s
        static constexpr float FLING_STOP_VEL_DP     = 30.0f;   // dp/s
        static constexpr float FLING_DECAY_TAU_SEC   = 0.4f;    // exponential decay time constant

        // Returns 1 if the event was consumed (do NOT forward to ImGui).
        int32_t handle(AInputEvent* ev) {
            if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
            if (!ImGui::GetCurrentContext()) { state = State::IDLE; return 0; }

            int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;

            // A second finger means pinch: drop any gesture in progress and let
            // the pinch recognizer (next in the chain) take over.
            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                state = State::IDLE;
                return 0;
            }

            if (action == AMOTION_EVENT_ACTION_DOWN) {
                if (state != State::FLING) state = State::IDLE; // stale gesture (missed UP)
                // Mice and styluses keep desktop-style interaction.
                int32_t tool = AMotionEvent_getToolType(ev, 0);
                if (tool != AMOTION_EVENT_TOOL_TYPE_FINGER && tool != AMOTION_EVENT_TOOL_TYPE_UNKNOWN) return 0;
                float x = AMotionEvent_getX(ev, 0);
                float y = AMotionEvent_getY(ev, 0);
                ImGuiWindow* target = findScrollTarget(ImVec2(x, y));
                if (!target) return 0; // an unrelated fling (if any) keeps running
                // Touching the flinging window catches it: stop, and swallow the
                // tap so stopping a fling can't click a widget.
                bool caughtFling = (state == State::FLING) && (target->ID == targetId);
                targetId = target->ID;
                suppressTap = caughtFling;
                downX = x;
                downY = y;
                lastY = y;
                pendingScrollDelta = 0.0f;
                downTime = std::chrono::steady_clock::now();
                sampleHead = sampleCount = 0;
                pushSample(y, AMotionEvent_getEventTime(ev));
                state = State::PENDING;
                return 1; // withhold the press until the gesture is classified
            }

            if (state == State::IDLE || state == State::FLING) return 0;

            if (action == AMOTION_EVENT_ACTION_MOVE) {
                float x = AMotionEvent_getX(ev, 0);
                float y = AMotionEvent_getY(ev, 0);
                // Touch panels sample faster than events are delivered; the
                // intermediate samples ride along as history. Feed them all so
                // fling velocity is computed at full sensor rate.
                int32_t histCount = (int32_t)AMotionEvent_getHistorySize(ev);
                for (int32_t h = 0; h < histCount; h++) {
                    pushSample(AMotionEvent_getHistoricalY(ev, 0, h), AMotionEvent_getHistoricalEventTime(ev, h));
                }
                pushSample(y, AMotionEvent_getEventTime(ev));

                if (state == State::PENDING) {
                    float dx = x - downX;
                    float dy = y - downY;
                    if (std::max(fabsf(dx), fabsf(dy)) >= style::dp(TOUCH_SLOP_DP)) {
                        if (fabsf(dy) > fabsf(dx)) {
                            state = State::PANNING;
                            lastY = y; // eat the slop distance: no content jump
                        }
                        else {
                            beginPassthrough();
                            return 0; // widget sees the press, then this motion
                        }
                    }
                    return 1;
                }
                if (state == State::PANNING) {
                    pendingScrollDelta += lastY - y; // content follows the finger
                    lastY = y;
                    return 1;
                }
                return 0; // PASSTHROUGH: stream normally
            }

            if (action == AMOTION_EVENT_ACTION_UP) {
                State s = state;
                state = State::IDLE;
                if (s == State::PENDING) {
                    if (!suppressTap) replayTap();
                    return 1;
                }
                if (s == State::PANNING) {
                    pushSample(AMotionEvent_getY(ev, 0), AMotionEvent_getEventTime(ev));
                    float v = -fingerVelocity(); // finger up = content up = scroll down
                    if (fabsf(v) >= style::dp(MIN_FLING_VEL_DP)) {
                        float maxVel = style::dp(MAX_FLING_VEL_DP);
                        flingVel = std::clamp(v, -maxVel, maxVel);
                        state = State::FLING;
                    }
                    return 1;
                }
                return 0; // PASSTHROUGH: ImGui gets the release
            }

            if (action == AMOTION_EVENT_ACTION_CANCEL) {
                // Gesture stolen by the system: no tap, no fling.
                int32_t consumed = (state == State::PASSTHROUGH) ? 0 : 1;
                state = State::IDLE;
                return consumed;
            }

            return (state == State::PASSTHROUGH) ? 0 : 1;
        }

        // Per-frame work: the hold timeout fires without events, pan deltas are
        // applied once per frame, and the fling animates. Call before NewFrame;
        // the scroll target set here is consumed by the window's Begin().
        void tick() {
            if (!ImGui::GetCurrentContext()) { state = State::IDLE; return; }
            if (state == State::IDLE) return; // nothing to animate; lastTickTime is refreshed on PENDING/PANNING before any fling
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - lastTickTime).count();
            lastTickTime = now;

            if (state == State::PENDING) {
                if (now - downTime >= std::chrono::milliseconds(HOLD_TIMEOUT_MS)) {
                    beginPassthrough();
                }
            }
            else if (state == State::PANNING) {
                if (pendingScrollDelta != 0.0f) {
                    // Overflow past the edges is dropped, which rebases the
                    // gesture: pulling back responds immediately.
                    applyScroll(pendingScrollDelta);
                    pendingScrollDelta = 0.0f;
                }
            }
            else if (state == State::FLING) {
                dt = std::min(dt, 0.1f); // first-tick / stall protection
                bool moved = applyScroll(flingVel * dt);
                flingVel *= expf(-dt / FLING_DECAY_TAU_SEC);
                if (!moved || fabsf(flingVel) < style::dp(FLING_STOP_VEL_DP)) {
                    state = State::IDLE;
                }
            }
        }

        // All remaining fields are initialized when the next DOWN starts.
        void reset() { state = State::IDLE; }

    private:
        ImGuiID targetId = 0;            // scrolled window, by ID: windows can vanish mid-gesture
        float downX = 0.0f, downY = 0.0f;
        float lastY = 0.0f;              // finger y already accounted for in pendingScrollDelta
        float pendingScrollDelta = 0.0f; // finger motion accumulated between frames
        bool suppressTap = false;        // touch caught a fling: swallow the tap
        float flingVel = 0.0f;           // scroll velocity in px/s (positive scrolls down)
        std::chrono::steady_clock::time_point downTime;
        std::chrono::steady_clock::time_point lastTickTime;

        // Finger samples for fling velocity, hardware-timestamped.
        static constexpr int MAX_SAMPLES = 32;
        static constexpr int64_t VELOCITY_HORIZON_NS = 100000000; // 100 ms
        struct Sample { float y; int64_t tNs; };
        Sample samples[MAX_SAMPLES];
        int sampleHead = 0, sampleCount = 0;

        // Resolve the window a vertical drag at `pos` should scroll, mirroring
        // ImGui's mouse-wheel routing. NULL means "don't intercept".
        static ImGuiWindow* findScrollTarget(ImVec2 pos) {
            ImGuiContext& g = *GImGui;

            // Top-most window under the touch. FindHoveredWindow() is not usable
            // here: it works off g.IO.MousePos, which is stale before touch-down.
            ImGuiWindow* hit = NULL;
            for (int i = g.Windows.Size - 1; i >= 0; i--) {
                ImGuiWindow* w = g.Windows[i];
                if (!w->Active || w->Hidden) continue;
                if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue;
                if (!w->OuterRectClipped.Contains(pos)) continue;
                hit = w;
                break;
            }
            if (!hit) return NULL;

            // A modal blocks windows outside its hierarchy (match hover rules).
            ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
            if (modal && !ImGui::IsWindowChildOf(hit->RootWindow, modal, true)) return NULL;

            // A titled root window moves only from its title bar. Do not let a
            // scrollable body make the touch recognizer swallow that handle.
            ImGuiWindow* root = hit->RootWindow;
            if (!(root->Flags & ImGuiWindowFlags_NoTitleBar) && root->TitleBarRect().Contains(pos)) return NULL;

            // Scrollbars keep their native thumb-drag behavior.
            if ((hit->ScrollbarY && ImGui::GetWindowScrollbarRect(hit, ImGuiAxis_Y).Contains(pos)) ||
                (hit->ScrollbarX && ImGui::GetWindowScrollbarRect(hit, ImGuiAxis_X).Contains(pos))) return NULL;

            // Climb to the window the wheel would scroll.
            ImGuiWindow* w = hit;
            while ((w->Flags & ImGuiWindowFlags_ChildWindow) && (w->ScrollMax.y <= 0.0f || (w->Flags & ImGuiWindowFlags_NoScrollWithMouse))) {
                w = w->ParentWindow;
            }
            if (!w || w->ScrollMax.y <= 0.0f || (w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) return NULL;

            // The waterfall-controls column is all vertical sliders; if it ever
            // overflows, drag-scrolling would fight them. Leave it native.
            if (strstr(w->Name, "WaterfallControls")) return NULL;
            return w;
        }

        // Returns true if the scroll moved by (approximately) the full delta.
        bool applyScroll(float delta) {
            ImGuiWindow* w = ImGui::FindWindowByID(targetId);
            if (!w || !w->Active) return false;
            float desired = w->Scroll.y + delta;
            float clamped = ImClamp(desired, 0.0f, w->ScrollMax.y);
            if (clamped != w->Scroll.y) ImGui::SetScrollY(w, clamped);
            return fabsf(desired - clamped) < 0.5f;
        }

        void beginPassthrough() {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(downX, downY);
            io.AddMouseButtonEvent(0, true);
            state = State::PASSTHROUGH;
        }

        void replayTap() {
            // Queued as pos+down, then up; the trickle queue spreads the pair
            // across two frames so the widget sees a full click.
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(downX, downY);
            io.AddMouseButtonEvent(0, true);
            io.AddMouseButtonEvent(0, false);
        }

        void pushSample(float y, int64_t tNs) {
            samples[sampleHead] = { y, tNs };
            sampleHead = (sampleHead + 1) % MAX_SAMPLES;
            sampleCount = std::min(sampleCount + 1, MAX_SAMPLES);
        }

        // Finger velocity in px/s over the last ~100 ms of samples.
        float fingerVelocity() const {
            if (sampleCount < 2) return 0.0f;
            int newestIdx = (sampleHead + MAX_SAMPLES - 1) % MAX_SAMPLES;
            Sample newest = samples[newestIdx];
            Sample oldest = newest;
            for (int i = 1; i < sampleCount; i++) {
                const Sample& s = samples[(newestIdx + MAX_SAMPLES - i) % MAX_SAMPLES];
                if (newest.tNs - s.tNs > VELOCITY_HORIZON_NS) break;
                oldest = s;
            }
            int64_t dtNs = newest.tNs - oldest.tNs;
            if (dtNs < 1000000) return 0.0f; // < 1 ms: not enough signal
            return (newest.y - oldest.y) * 1e9f / (float)dtNs;
        }
    };

    static TouchScrollRecognizer touchScrollRecognizer;

    static void resetInputRecognizers() {
        gestureRecognizer.reset();
        touchScrollRecognizer.reset();
    }

    // Back key: one press dismisses one UI layer (popup/modal, credits).
    // With nothing left to dismiss, the app moves to the background instead
    // of finishing (process and config stay warm; APP_CMD_PAUSE stops the
    // SDR as usual). Exiting the app goes through holding the menu button.
    static void handleBackKey() {
        // While the sleep timer has dimmed the screen, Back is a wake
        // gesture like any touch — don't dismiss UI the user can't see.
        if (sleepScreenDimmed) {
            resetSleepToActive();
            return;
        }
        if (!gui::mainWindow.handleBackPress()) {
            callActivityVoidMethod("moveAppToBack");
        }
    }

    int32_t handleInputEvent(struct android_app* app, AInputEvent* inputEvent) {
        // Back must never reach the default handler, which would finish the
        // activity. Act on UP so a canceled gesture (predictive-back peek,
        // long-press) doesn't trigger; consume DOWN too.
        if (AInputEvent_getType(inputEvent) == AINPUT_EVENT_TYPE_KEY &&
            AKeyEvent_getKeyCode(inputEvent) == AKEYCODE_BACK) {
            if (AKeyEvent_getAction(inputEvent) == AKEY_EVENT_ACTION_UP &&
                !(AKeyEvent_getFlags(inputEvent) & AKEY_EVENT_FLAG_CANCELED)) {
                sleepResetMotionPending = true; // Back is user activity too
                handleBackKey();
            }
            return 1;
        }

        // If the sleep timer has dimmed/darkened the screen, intercept touch to wake.
        // Also keep consuming until ACTION_UP once we started swallowing a gesture,
        // because sleepScreenDimmed may be cleared asynchronously before ACTION_UP arrives.
        bool motion = AInputEvent_getType(inputEvent) == AINPUT_EVENT_TYPE_MOTION;
        if (sleepScreenDimmed || consumingWakeGesture) {
            if (motion) {
                int32_t action = AMotionEvent_getAction(inputEvent) & AMOTION_EVENT_ACTION_MASK;
                if (action == AMOTION_EVENT_ACTION_DOWN && sleepScreenDimmed) {
                    flog::info("Sleep: touch detected, resetting to active");
                    consumingWakeGesture = true;
                    resetSleepToActive();
                }
                else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
                    consumingWakeGesture = false;
                }
                return 1;  // consume motion events while screen is dimmed or dark
            }
            return 0;  // let non-motion events (volume, back, etc.) pass to the default handler
        }

        // Flag any motion event so the render-loop timer can issue the JNI call.
        // The actual resetSleepToActive() is throttled to once per second from renderLoop(),
        // which gives trailing-edge semantics: the reset fires after the last event of a
        // burst, not just on the first one.
        if (motion) {
            sleepResetMotionPending = true;
        }

        if (touchScrollRecognizer.handle(inputEvent)) return 1;
        if (gestureRecognizer.handle(inputEvent)) return 1;
        return ImGui_ImplAndroid_HandleInputEvent(inputEvent);
    }

    int aquireWindow() {
        while (!app->window) {
            flog::warn("Waiting on the shitty window thing"); std::this_thread::sleep_for(std::chrono::milliseconds(30));
            int out_events;
            struct android_poll_source* out_data;

            while (true) {
                int poll_result = ALooper_pollOnce(0, NULL, &out_events, (void**)&out_data);
                if (poll_result == ALOOPER_POLL_TIMEOUT || poll_result == ALOOPER_POLL_WAKE || poll_result == ALOOPER_POLL_ERROR) {
                    break;
                }

                // Process one event
                if (out_data != NULL) { out_data->process(app, out_data); }

                // Exit the app by returning from within the infinite loop
                if (app->destroyRequested != 0) {
                    return -1;
                }
            }
        }
        ANativeWindow_acquire(app->window);
        return 0;
    }

    int init(std::string resDir) {
        flog::warn("Backend init");
        float userScaleFactor = common::configUserScaleFactor();

        // Get window
        aquireWindow();

        // EGL Init
        {
            _EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (_EglDisplay == EGL_NO_DISPLAY)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");

            if (eglInitialize(_EglDisplay, 0, 0) != EGL_TRUE)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglInitialize() returned with an error");

            const EGLint egl_attributes[] = { EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
            EGLint num_configs = 0;
            if (eglChooseConfig(_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglChooseConfig() returned with an error");
            if (num_configs == 0)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglChooseConfig() returned 0 matching config");

            // Get the first matching config
            EGLConfig egl_config;
            eglChooseConfig(_EglDisplay, egl_attributes, &egl_config, 1, &num_configs);
            EGLint egl_format;
            eglGetConfigAttrib(_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &egl_format);
            ANativeWindow_setBuffersGeometry(app->window, 0, 0, egl_format);

            const EGLint egl_context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            _EglContext = eglCreateContext(_EglDisplay, egl_config, EGL_NO_CONTEXT, egl_context_attributes);

            if (_EglContext == EGL_NO_CONTEXT)
                __android_log_print(ANDROID_LOG_ERROR, _LogTag, "%s", "eglCreateContext() returned EGL_NO_CONTEXT");

            _EglSurface = eglCreateWindowSurface(_EglDisplay, egl_config, app->window, NULL);
            eglMakeCurrent(_EglDisplay, _EglSurface, _EglSurface, _EglContext);
        }

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        // Disable loading/saving of .ini file from disk.
        // FIXME: Consider using LoadIniSettingsFromMemory() / SaveIniSettingsToMemory() to save in appropriate location for Android.
        io.IniFilename = NULL;

        // Setup Platform/Renderer backends
        ImGui_ImplAndroid_Init(app->window);
        ImGui_ImplOpenGL3_Init("#version 300 es");

        common::initScaleState(scaleState, resDir, getContentScale(), userScaleFactor);
        return 0;
    }

    void beginFrame() {
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        if (ImGui::GetIO().Fonts->TexPixelsRGBA32) {
            ImGui::GetIO().Fonts->ClearTexData();
        }
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
    }

    void render(bool vsync) {
        auto dSize = ImGui::GetIO().DisplaySize;
        common::renderOpenGLFrame((int)dSize.x, (int)dSize.y);
        eglSwapBuffers(_EglDisplay, _EglSurface);
    }

    // No screen pos to detect
    void getMouseScreenPos(double& x, double& y) { x = 0; y = 0; }
    void setMouseScreenPos(double x, double y) {}

    int renderLoop() {
        while (true) {
            int out_events;
            struct android_poll_source* out_data;

            while (true) {
                int poll_result = ALooper_pollOnce(0, NULL, &out_events, (void**)&out_data);
                if (poll_result == ALOOPER_POLL_TIMEOUT || poll_result == ALOOPER_POLL_WAKE || poll_result == ALOOPER_POLL_ERROR) {
                    break;
                }

                // Process one event
                if (out_data != NULL) { out_data->process(app, out_data); }

                // Exit the app by returning from within the infinite loop
                if (app->destroyRequested != 0) {
                    flog::warn("ASKED TO EXIT");
                    exited = true;

                    saveLifecycleState("application exit");

                    // Stop SDR
                    gui::mainWindow.setPlayState(false);
                    return 0;
                }
            }

            if (_EglDisplay == EGL_NO_DISPLAY) { continue; }

            if (!pauseRendering && !sleepRenderPaused) {
                sleepBlackFrameSent = false;

                // Keep the sleep-dimming timer alive during user interaction.
                // sleepResetMotionPending is set by handleInputEvent on any motion event.
                // The flag is cleared here so the JNI call fires on the trailing edge of
                // each gesture burst — even if the burst lasted less than one second.
                if (sleepResetMotionPending && !sleepScreenDimmed) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - sleepResetLastCall >= std::chrono::seconds(1)) {
                        sleepResetMotionPending = false;
                        resetSleepToActive();
                    }
                }

                // Poll Unicode characters via JNI
                // FIXME: do not call this every frame because of JNI overhead
                PollUnicodeChars();

                common::applyPendingScaleChanges(scaleState, getContentScale(), true);

                // Back arriving through OnBackInvokedCallback (Android 16+ /
                // targetSdk 36): the callback runs on the UI thread, so it only
                // sets a flag; the dismiss chain runs here on the app thread,
                // like the legacy KEYCODE_BACK path in handleInputEvent.
                if (backInvokedPending.exchange(false)) {
                    sleepResetMotionPending = true;
                    handleBackKey();
                }

                // Drag-scroll per-frame work: hold timeout, pan application,
                // fling. Must run before NewFrame so the scroll target it sets
                // is consumed by the target window's Begin() this frame.
                touchScrollRecognizer.tick();

                // Initiate a new frame
                beginFrame();

                ImGuiIO& io = ImGui::GetIO();
                auto dsize = io.DisplaySize;

                // Open on-screen (soft) input if requested by Dear ImGui
                static bool WantTextInputLast = false;
                if (io.WantTextInput && !WantTextInputLast)
                ShowSoftKeyboardInput();
                WantTextInputLast = io.WantTextInput;

                // Render
                common::drawMainWindow(dsize);
                render();
            }
            else if (sleepRenderPaused && !pauseRendering) {
                // Back during the DARK phase is a wake gesture, like touch.
                if (backInvokedPending.exchange(false)) {
                    resetSleepToActive();
                }

                // Sleep-mode pause: submit one black frame, then idle.
                // EGL surface stays alive, GPU goes idle.
                if (!sleepBlackFrameSent) {
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    eglSwapBuffers(_EglDisplay, _EglSurface);
                    sleepBlackFrameSent = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }

        return 0;
    }

    float getContentScale() {
        if (!app || !app->config) return 3.0f;
        int32_t density = AConfiguration_getDensity(app->config);
        if (density <= 0 || density == ACONFIGURATION_DENSITY_ANY || density == ACONFIGURATION_DENSITY_NONE)
            return 3.0f;
        return density / 160.0f;
    }

    int end() {
        // Cleanup
        touchScrollRecognizer.reset();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();

        // Destroy all
        if (_EglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (_EglContext != EGL_NO_CONTEXT) { eglDestroyContext(_EglDisplay, _EglContext); }
            if (_EglSurface != EGL_NO_SURFACE) { eglDestroySurface(_EglDisplay, _EglSurface); }
            eglTerminate(_EglDisplay);
        }

        _EglDisplay = EGL_NO_DISPLAY;
        _EglContext = EGL_NO_CONTEXT;
        _EglSurface = EGL_NO_SURFACE;

        if (app->window) { ANativeWindow_release(app->window); }

        return 0;
    }

    int ShowSoftKeyboardInput()  { return callActivityVoidMethod("showSoftInput")     ? 0 : -1; }

    static int getPreferredAudioDeviceId(const char* methodName) {
        JniSession jni;
        if (!jni.valid()) return 0;
        jmethodID method = jni.env->GetStaticMethodID(jni.clazz, methodName, "(Landroid/content/Context;)I");
        if (!method) return 0;
        return (int)jni.env->CallStaticIntMethod(jni.clazz, method, app->activity->clazz);
    }

    int getPreferredAudioOutputDeviceId() {
        return getPreferredAudioDeviceId("getPreferredAudioOutputDeviceId");
    }

    void setAudioOutputUsesOpenSLES(bool usesOpenSLES) {
        audioOutputOpenSLES.store(usesOpenSLES, std::memory_order_relaxed);
    }

    bool audioOutputUsesOpenSLES() {
        return audioOutputOpenSLES.load(std::memory_order_relaxed);
    }

    UsbDeviceLease::UsbDeviceLease(const std::vector<DevVIDPID>& allowedVidPids) {
        acquire(allowedVidPids);
    }

    UsbDeviceLease::~UsbDeviceLease() {
        reset();
    }

    bool UsbDeviceLease::acquire(const std::vector<DevVIDPID>& allowedVidPids) {
        if (!reset())
            return false;
        handle = getUsbDeviceHandle(allowedVidPids);
        return handle.valid();
    }

    bool UsbDeviceLease::reset() {
        if (!handle.valid()) {
            handle = {};
            return true;
        }
        if (!releaseUsbDeviceHandle(handle)) {
            flog::error("UsbDeviceLease::reset(): Java-side USB release failed for fd={}; retaining handle to avoid orphan", handle.fd);
            return false;
        }
        handle = {};
        return true;
    }

    static UsbDeviceHandle getUsbDeviceHandle(const std::vector<DevVIDPID>& allowedVidPids) {
        UsbDeviceHandle handle;

        JniSession jni;
        if (!jni.valid()) return handle;

        jmethodID method = jni.env->GetStaticMethodID(
            jni.clazz,
            "getOpenUsbDeviceHandleByVidPid",
            "(Landroid/content/Context;II)Ljava/lang/String;"
        );
        if (!method) return handle;

        for (const auto& vp : allowedVidPids) {
            jstring descriptor = (jstring)jni.env->CallStaticObjectMethod(
                jni.clazz, method, app->activity->clazz, (jint)vp.vid, (jint)vp.pid
            );
            if (!descriptor) continue;

            const char* utf = jni.env->GetStringUTFChars(descriptor, NULL);
            std::string value = utf ? utf : "";
            jni.env->ReleaseStringUTFChars(descriptor, utf);
            jni.env->DeleteLocalRef(descriptor);

            auto separator = value.find('|');
            if (separator == std::string::npos) continue;

            handle.fd   = std::stoi(value.substr(0, separator));
            handle.path = value.substr(separator + 1);
            handle.vid  = vp.vid;
            handle.pid  = vp.pid;
            return handle;
        }

        return handle;
    }

    static bool releaseUsbDeviceHandle(const UsbDeviceHandle& handle) {
        if (handle.fd < 0) return true;

        JniSession jni;
        if (!jni.env) return false; // attach failed; nothing to log
        if (!jni.clazz) {
            flog::error("releaseUsbDeviceHandle(): GetObjectClass() failed for fd={}", handle.fd);
            return false;
        }

        jmethodID method = jni.env->GetStaticMethodID(jni.clazz, "releaseOpenUsbDeviceHandle", "(I)V");
        if (!method) {
            flog::error("releaseUsbDeviceHandle(): GetStaticMethodID() failed for fd={}", handle.fd);
            return false;
        }

        jni.env->CallStaticVoidMethod(jni.clazz, method, (jint)handle.fd);
        return true;
    }

    bool hasUsbDeviceAvailable(const std::vector<DevVIDPID>& allowedVidPids) {
        UsbDeviceLease handle(allowedVidPids);
        return handle.valid();
    }

    // Unfortunately, the native KeyEvent implementation has no getUnicodeChar() function.
    // Therefore, we implement the processing of KeyEvents in MainActivity.kt and poll
    // the resulting Unicode characters here via JNI and send them to Dear ImGui.
    int PollUnicodeChars() {
        JniSession jni;
        if (!jni.valid()) return -1;

        jmethodID method_id = jni.env->GetMethodID(jni.clazz, "pollUnicodeChar", "()I");
        if (!method_id) return -2;

        // Send the actual characters to Dear ImGui
        ImGuiIO& io = ImGui::GetIO();
        jint unicode_character;
        while ((unicode_character = jni.env->CallIntMethod(app->activity->clazz, method_id)) != 0)
            io.AddInputCharacter(unicode_character);

        return 0;
    }

    std::string getAppFilesDir() {
        JniSession jni;
        if (!jni.env)   throw std::runtime_error("Could not attach to JNI thread");
        if (!jni.clazz) throw std::runtime_error("Could not get MainActivity class");

        jmethodID method_id = jni.env->GetMethodID(jni.clazz, "getAppDir", "()Ljava/lang/String;");
        if (!method_id) throw std::runtime_error("Could not get getAppDir method ID");

        jstring jstr = (jstring)jni.env->CallObjectMethod(app->activity->clazz, method_id);

        const char* _str = jni.env->GetStringUTFChars(jstr, NULL);
        std::string str(_str);
        jni.env->ReleaseStringUTFChars(jstr, _str);

        return str;
    }

    const std::vector<DevVIDPID> AIRSPY_VIDPIDS = {
        { 0x1d50, 0x60a1 }
    };

    const std::vector<DevVIDPID> AIRSPYHF_VIDPIDS = {
        { 0x03EB, 0x800C }
    };

    const std::vector<DevVIDPID> HACKRF_VIDPIDS = {
        { 0x1d50, 0x604b },
        { 0x1d50, 0x6089 },
        { 0x1d50, 0xcc15 }
    };

    const std::vector<DevVIDPID> HYDRASDR_VIDPIDS = {
        { 0x1d50, 0x60a1 },
        { 0x38af, 0x0001 }
    };

    const std::vector<DevVIDPID> QMX_VIDPIDS = {
        { 0x0483, 0xA34C }
    };

    const std::vector<DevVIDPID> RTL_SDR_VIDPIDS = {
        { 0x0bda, 0x2832 },
        { 0x0bda, 0x2838 },
        { 0x0413, 0x6680 },
        { 0x0413, 0x6f0f },
        { 0x0458, 0x707f },
        { 0x0ccd, 0x00a9 },
        { 0x0ccd, 0x00b3 },
        { 0x0ccd, 0x00b4 },
        { 0x0ccd, 0x00b5 },
        { 0x0ccd, 0x00b7 },
        { 0x0ccd, 0x00b8 },
        { 0x0ccd, 0x00b9 },
        { 0x0ccd, 0x00c0 },
        { 0x0ccd, 0x00c6 },
        { 0x0ccd, 0x00d3 },
        { 0x0ccd, 0x00d7 },
        { 0x0ccd, 0x00e0 },
        { 0x1554, 0x5020 },
        { 0x15f4, 0x0131 },
        { 0x15f4, 0x0133 },
        { 0x185b, 0x0620 },
        { 0x185b, 0x0650 },
        { 0x185b, 0x0680 },
        { 0x1b80, 0xd393 },
        { 0x1b80, 0xd394 },
        { 0x1b80, 0xd395 },
        { 0x1b80, 0xd397 },
        { 0x1b80, 0xd398 },
        { 0x1b80, 0xd39d },
        { 0x1b80, 0xd3a4 },
        { 0x1b80, 0xd3a8 },
        { 0x1b80, 0xd3af },
        { 0x1b80, 0xd3b0 },
        { 0x1d19, 0x1101 },
        { 0x1d19, 0x1102 },
        { 0x1d19, 0x1103 },
        { 0x1d19, 0x1104 },
        { 0x1f4d, 0xa803 },
        { 0x1f4d, 0xb803 },
        { 0x1f4d, 0xc803 },
        { 0x1f4d, 0xd286 },
        { 0x1f4d, 0xd803 }
    };
}

extern "C" {
    void android_main(struct android_app* app) {
        // Save app instance
        app->onAppCmd = backend::handleAppCmd;
        app->onInputEvent = backend::handleInputEvent;
        backend::app = app;

        // Check if this is the first time we run or not
        if (backend::initialized) {
            flog::warn("android_main called again");
            backend::doPartialInit();
            backend::pauseRendering = false;
            backend::renderLoop();
            return;
        }
        backend::initialized = true;

        // Grab files dir
        std::string appdir = backend::getAppFilesDir();

        // Call main
        char* rootpath = new char[appdir.size() + 1];
        strcpy(rootpath, appdir.c_str());
        char arg0[] = "";
        char arg1[] = "-r";
        char* dummy[] = { arg0, arg1, rootpath };
        sdrpp_main(3, dummy);
    }

    // JNI native method: MainActivity.nativeSetSleepRenderPaused(boolean)
    // Directly sets the C++ flag - zero overhead, no JNI field lookup.
    JNIEXPORT void JNICALL Java_org_ok1iak_sdriak_MainActivity_nativeSetSleepRenderPaused(
        JNIEnv* env, jobject thiz, jboolean paused) {
        backend::sleepRenderPaused = (bool)paused;
        if (!paused) {
            backend::sleepBlackFrameSent = false;
        }
    }

    // JNI native method: MainActivity.nativeSetSleepScreenDimmed(boolean)
    // True during DIM and DARK phases; used as guard for touch/resume wake.
    JNIEXPORT void JNICALL Java_org_ok1iak_sdriak_MainActivity_nativeSetSleepScreenDimmed(
        JNIEnv* env, jobject thiz, jboolean dimmed) {
        backend::sleepScreenDimmed = (bool)dimmed;
    }
}

// JNI native method: MainActivity.nativeOnBackInvoked()
// Runs on the UI thread; the app thread consumes the flag in renderLoop().
extern "C" JNIEXPORT void JNICALL Java_org_ok1iak_sdriak_MainActivity_nativeOnBackInvoked(JNIEnv*, jobject) {
    backend::backInvokedPending = true;
}

extern "C" JNIEXPORT void JNICALL Java_org_ok1iak_sdriak_MainActivity_notifyUsbHotplugChangedNative(JNIEnv*, jobject) {
    backend::usbHotplugGeneration.fetch_add(1, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL Java_org_ok1iak_sdriak_MainActivity_notifyAudioRoutingChangedNative(JNIEnv*, jobject) {
    backend::audioRoutingGeneration.fetch_add(1, std::memory_order_relaxed);
}
