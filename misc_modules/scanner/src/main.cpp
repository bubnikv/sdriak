#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <gui/widgets/narrow_iq_notice.h>
#include <chrono>
#include <atomic>

SDRPP_MOD_INFO{
    /* Name:            */ "scanner",
    /* Description:     */ "Frequency scanner for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

class ScannerModule : public ModuleManager::Instance {
public:
    ScannerModule(std::string name) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~ScannerModule() {
        gui::menu.removeEntry(name);
        stop();
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        ScannerModule* _this = (ScannerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Snapshot once: worker() can clear running mid-frame from its own
        // thread, and reading the guard twice would leave the BeginDisabled/
        // EndDisabled pair below unbalanced (the #1437 stack-mismatch pattern).
        bool running = _this->running.load(std::memory_order_relaxed);

        ImGui::NarrowIQNotice();
        if (running) { ImGui::BeginDisabled(); }
        ImGui::LeftLabel("Start");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##start_freq_scanner", &_this->startFreq, 100.0, 100000.0, "%0.0f")) {
            _this->startFreq = round(_this->startFreq);
        }
        ImGui::LeftLabel("Stop");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##stop_freq_scanner", &_this->stopFreq, 100.0, 100000.0, "%0.0f")) {
            _this->stopFreq = round(_this->stopFreq);
        }
        ImGui::LeftLabel("Interval");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##interval_scanner", &_this->interval, 100.0, 100000.0, "%0.0f")) {
            _this->interval = round(_this->interval);
        }
        ImGui::LeftLabel("Passband Ratio (%)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##pb_ratio_scanner", &_this->passbandRatio, 1.0, 10.0, "%0.0f")) {
            _this->passbandRatio = std::clamp<double>(round(_this->passbandRatio), 1.0, 100.0);
        }
        ImGui::LeftLabel("Tuning Time (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##tuning_time_scanner", &_this->tuningTime, 100, 1000)) {
            _this->tuningTime = std::clamp<int>(_this->tuningTime, 100, 10000.0);
        }
        ImGui::LeftLabel("Linger Time (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##linger_time_scanner", &_this->lingerTime, 100, 1000)) {
            _this->lingerTime = std::clamp<int>(_this->lingerTime, 100, 10000.0);
        }
        if (running) { ImGui::EndDisabled(); }

        ImGui::LeftLabel("Level");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::SliderFloat("##scanner_level", &_this->level, -150.0, 0.0);

        ImGui::BeginTable(("scanner_bottom_btn_table" + _this->name).c_str(), 2, 0, ImVec2(ImGui::BeginActionRow(), 0));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("<<##scanner_back_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            std::lock_guard<std::mutex> lck(_this->scanMtx);
            _this->reverseLock = true;
            _this->receiving = false;
            _this->scanUp = false;
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button((">>##scanner_forw_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            std::lock_guard<std::mutex> lck(_this->scanMtx);
            _this->reverseLock = true;
            _this->receiving = false;
            _this->scanUp = true;
        }
        ImGui::EndTable();

        if (!running) {
            if (ImGui::ActionButton("Start##scanner_start")) {
                _this->start();
            }
            ImGui::Text("Status: Idle");
        }
        else {
            if (ImGui::ActionButton("Stop##scanner_start")) {
                _this->stop();
            }
            if (_this->receiving) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: Receiving");
            }
            else if (_this->tuning) {
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Status: Tuning");
            }
            else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Status: Scanning");
            }
        }
    }

    void start() {
        if (running) { return; }
        // The worker can exit on its own (e.g. it clears running when no VFO is
        // selected), which leaves workerThread joinable-but-unjoined. Reap it
        // before reassigning, or std::thread::operator= calls std::terminate.
        if (workerThread.joinable()) { workerThread.join(); }
        current = startFreq;
        running = true;
        workerThread = std::thread(&ScannerModule::worker, this);
    }

    void stop() {
        // Don't gate the join on 'running': a self-terminated worker has already
        // cleared it while leaving the thread joinable, and destroying a joinable
        // std::thread (via ~ScannerModule) would call std::terminate.
        running = false;
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    void worker() {
        // 10Hz scan loop
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lck(scanMtx);
                auto now = std::chrono::high_resolution_clock::now();

                // Enforce tuning
                if (gui::waterfall.selectedVFO.empty()) {
                    running = false;
                    return;
                }
                tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, current);

                // Check if we are waiting for a tune
                if (tuning) {
                    flog::warn("Tuning");
                    if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime)).count() > tuningTime) {
                        tuning = false;
                    }
                    continue;
                }

                // Get FFT data
                int dataWidth = 0;
                float* data = gui::waterfall.acquireLatestFFT(dataWidth);
                if (!data) { continue; }

                // Get gather waterfall data
                double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
                double wfWidth = gui::waterfall.getViewBandwidth();
                double wfStart = wfCenter - (wfWidth / 2.0);
                double wfEnd = wfCenter + (wfWidth / 2.0);

                // Gather VFO data
                double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);

                if (receiving) {
                    flog::warn("Receiving");
                
                    float maxLevel = getMaxLevel(data, current, vfoWidth, dataWidth, wfStart, wfWidth);
                    if (maxLevel >= level) {
                        lastSignalTime = now;
                    }
                    else if ((std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime)).count() > lingerTime) {
                        receiving = false;
                    }
                }
                else {
                    flog::warn("Seeking signal");
                    double bottomLimit = current;
                    double topLimit = current;
                    
                    // Search for a signal in scan direction
                    if (findSignal(scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, vfoWidth, data, dataWidth)) {
                        gui::waterfall.releaseLatestFFT();
                        continue;
                    }
                    
                    // Search for signal in the inverse scan direction if direction isn't enforced
                    if (!reverseLock) {
                        if (findSignal(!scanUp, bottomLimit, topLimit, wfStart, wfEnd, wfWidth, vfoWidth, data, dataWidth)) {
                            gui::waterfall.releaseLatestFFT();
                            continue;
                        }
                    }
                    else { reverseLock = false; }
                    

                    // There is no signal on the visible spectrum, tune in scan direction and retry
                    if (scanUp) {
                        current = topLimit + interval;
                        if (current > stopFreq) { current = startFreq; }
                    }
                    else {
                        current = bottomLimit - interval;
                        if (current < startFreq) { current = stopFreq; }
                    }

                    // If the new current frequency is outside the visible bandwidth, wait for retune
                    if (current - (vfoWidth/2.0) < wfStart || current + (vfoWidth/2.0) > wfEnd) {
                        lastTuneTime = now;
                        tuning = true;
                    }
                }

                // Release FFT Data
                gui::waterfall.releaseLatestFFT();
            }
        }
    }

    bool findSignal(bool scanDir, double& bottomLimit, double& topLimit, double wfStart, double wfEnd, double wfWidth, double vfoWidth, float* data, int dataWidth) {
        bool found = false;
        double freq = current;
        for (freq += scanDir ? interval : -interval;
            scanDir ? (freq <= stopFreq) : (freq >= startFreq);
            freq += scanDir ? interval : -interval) {

            // Check if signal is within bounds
            if (freq - (vfoWidth/2.0) < wfStart) { break; }
            if (freq + (vfoWidth/2.0) > wfEnd) { break; }

            if (freq < bottomLimit) { bottomLimit = freq; }
            if (freq > topLimit) { topLimit = freq; }
            
            // Check signal level
            float maxLevel = getMaxLevel(data, freq, vfoWidth * (passbandRatio * 0.01f), dataWidth, wfStart, wfWidth);
            if (maxLevel >= level) {
                found = true;
                receiving = true;
                current = freq;
                break;
            }
        }
        return found;
    }

    float getMaxLevel(float* data, double freq, double width, int dataWidth, double wfStart, double wfWidth) {
        double low = freq - (width/2.0);
        double high = freq + (width/2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        float max = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (data[i] > max) { max = data[i]; }
        }
        return max;
    }

    std::string name;
    bool enabled = true;
    
    // Written by start()/stop() (GUI thread) and by worker() itself; read in the
    // worker's loop condition. Atomic so the stop handshake is well-defined.
    std::atomic<bool> running = false;
    //std::string selectedVFO = "Radio";
    double startFreq = 88000000.0;
    double stopFreq = 108000000.0;
    double interval = 100000.0;
    double current = 88000000.0;
    double passbandRatio = 10.0;
    int tuningTime = 250;
    int lingerTime = 1000.0;
    float level = -50.0;
    bool receiving = true;
    bool tuning = false;
    bool scanUp = true;
    bool reverseLock = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::thread workerThread;
    std::mutex scanMtx;
};

MOD_EXPORT void _INIT_() {
    // Nothing here
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ScannerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ScannerModule*)instance;
}

MOD_EXPORT void _END_() {
    // Nothing here
}