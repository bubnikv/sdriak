#include "FreqModeSync.h"

#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <imgui.h>
#include <module.h>
#include <qmx/QmxDevice.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <utils/optionlist.h>

#ifdef __ANDROID__
#include <android/api-level.h>
#include <android_backend.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "qmx_source",
    /* Description:     */ "Direct QMX USB source module for SDRIAK",
    /* Author:          */ "OK1IAK",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;

namespace {
    struct AudioChoice {
        std::string id;
        std::string label;

        bool operator==(const AudioChoice& other) const {
            return id == other.id;
        }
    };

    struct SerialChoice {
        std::string path;
        std::string label;

        bool operator==(const SerialChoice& other) const {
            return path == other.path;
        }
    };
}

class QMXSourceModule : public ModuleManager::Instance {
public:
    QMXSourceModule(std::string name) {
        this->name = std::move(name);

        // Not served headless: the CAT status/VFO sync machinery is pumped by
        // MainWindow::onFrameDraw, which never fires on a server, and the
        // audio/serial device selection is local-hardware UI. Skip
        // registration so the source doesn't appear in the server's list.
        // Also we serve QMX with our own qmxserver module, which is more network efficient (UDP, lower latency).
        if (core::args["server"].b()) { return; }

        sampleRate = qmx::kSampleRate;
        sync.setDevice(&device);

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();
        loadConfig();

        frameDrawHandler.ctx = this;
        frameDrawHandler.handler = frameDraw;
        gui::mainWindow.onFrameDraw.bindHandler(&frameDrawHandler);

        sigpath::sourceManager.registerSource("QMX", &handler);
    }

    ~QMXSourceModule() {
        // Server mode: the constructor returned before binding/registering.
        if (core::args["server"].b()) { return; }
        gui::mainWindow.onFrameDraw.unbindHandler(&frameDrawHandler);
        stop(this);
        sigpath::sourceManager.unregisterSource("QMX");
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
    void loadConfig() {
        double loadedFreq = 7000000.0;
        bool loadedSyncVfo = false;
        {
            auto configAccess = config.read();
            configAccess.tryGet("frequency", loadedFreq);
            configAccess.tryGet("syncVfo", loadedSyncVfo);
#ifndef __ANDROID__
            configAccess.tryGet("audioDevice", selectedAudioDevice);
            configAccess.tryGet("serialPort", selectedSerialPort);
#else
            configAccess.tryGet("device", selectedAndroidDevice);
#endif
        }
        freq = loadedFreq;
        sync.setSyncVfo(loadedSyncVfo);

#ifndef __ANDROID__
        if (!selectedAudioDevice.empty())
            selectAudioDevice(selectedAudioDevice);
        else
            selectPreferredAudioDevice();
        if (!selectedSerialPort.empty())
            selectSerialPort(selectedSerialPort);
        else
            selectFirstSerialPort();
#else
        selectAndroidDeviceByName(selectedAndroidDevice);
#endif
    }

    void refresh() {
#ifndef __ANDROID__
        audioDevices.clear();
        for (const auto& device : qmx::QmxDevice::listAudioDevices())
            audioDevices.define(device.id, device.label, { device.id, device.label });

        serialPorts.clear();
        for (const auto& port : qmx::QmxDevice::listSerialPorts())
            serialPorts.define(port.path, port.label, { port.path, port.label });
#else
        androidDevices.clear();
        androidDeviceListTxt.clear();
        selectedAndroidDevice.clear();
        androidDevId = 0;
        androidUsbHandle.reset();
        if (!backend::hasUsbDeviceAvailable(backend::QMX_VIDPIDS)) {
            return;
        }
        androidDevices.push_back("QMX USB");
        androidDeviceListTxt += androidDevices.back();
        androidDeviceListTxt += '\0';
#endif
    }

#ifndef __ANDROID__
    void selectPreferredAudioDevice() {
        if (audioDevices.empty()) {
            selectedAudioDevice.clear();
            return;
        }

        for (int i = 0; i < audioDevices.size(); ++i) {
            const auto device = audioDevices.value(i);
            std::string upper = device.label;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (upper.find("QMX") != std::string::npos || upper.find("QDX") != std::string::npos) {
                selectAudioDevice(audioDevices.key(i));
                return;
            }
        }

        selectAudioDevice(audioDevices.key(0));
    }

    void selectAudioDevice(const std::string& deviceId) {
        if (audioDevices.empty() || !audioDevices.keyExists(deviceId)) {
            selectedAudioDevice.clear();
            return;
        }

        selectedAudioDevice = deviceId;
        audioDevId = audioDevices.keyId(deviceId);
    }

    void selectFirstSerialPort() {
        if (serialPorts.empty()) {
            selectedSerialPort.clear();
            return;
        }
        selectSerialPort(serialPorts.key(0));
    }

    void selectSerialPort(const std::string& portName) {
        if (serialPorts.empty() || !serialPorts.keyExists(portName)) {
            selectedSerialPort.clear();
            return;
        }

        selectedSerialPort = portName;
        serialPortId = serialPorts.keyId(portName);
    }
#else
    void selectAndroidFirstDevice() {
        if (!androidDevices.empty()) {
            selectAndroidDeviceById(0);
        }
        else {
            selectedAndroidDevice.clear();
            androidDevId = 0;
        }
    }

    void selectAndroidDeviceByName(const std::string& name) {
        for (int i = 0; i < static_cast<int>(androidDevices.size()); ++i) {
            if (androidDevices[i] == name) {
                selectAndroidDeviceById(i);
                return;
            }
        }
        selectAndroidFirstDevice();
    }

    void selectAndroidDeviceById(int id) {
        if (id < 0 || id >= static_cast<int>(androidDevices.size())) {
            selectedAndroidDevice.clear();
            androidDevId = 0;
            return;
        }
        androidDevId = id;
        selectedAndroidDevice = androidDevices[id];
    }

    void refreshAndroidDeviceSelection(const std::string& preferredDevice = "") {
        std::string deviceToRestore = preferredDevice.empty() ? selectedAndroidDevice : preferredDevice;
        refresh();
        selectAndroidDeviceByName(deviceToRestore);

        config.edit().set("device", selectedAndroidDevice);

        lastAndroidUsbHotplugGeneration = backend::usbHotplugGeneration.load(std::memory_order_relaxed);
    }

    void refreshAndroidDeviceSelectionIfNeeded() {
        if (running)
            return;

        int generation = backend::usbHotplugGeneration.load(std::memory_order_relaxed);
        if (generation == lastAndroidUsbHotplugGeneration)
            return;

        refreshAndroidDeviceSelection();
    }
#endif
    static void menuSelected(void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        core::setInputSampleRate(self->sampleRate);
        // 100 kHz - 60 MHz; the manager shifts this by the tuning offset for
        // up/down-converters.
        sigpath::sourceManager.setTuningLimits(100000.0, 60000000.0); // 100 kHz - 60 MHz
    }

    static void menuDeselected(void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        sigpath::sourceManager.clearTuningLimits();
    }

    static void start(void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        if (self->running)
            return;

        if (self->catTransmitActive.exchange(false, std::memory_order_relaxed)) { sigpath::txState.emit(false); }

        qmx::StartOptions options;
#ifndef __ANDROID__
        if (self->selectedAudioDevice.empty()) {
            flog::error("QMXSourceModule: No QMX audio device selected");
            return;
        }
        options.audioDeviceId = self->selectedAudioDevice;
        options.serialPort = self->selectedSerialPort;
#else
        self->refreshAndroidDeviceSelectionIfNeeded();
        if (self->selectedAndroidDevice.empty()) {
            flog::error("QMXSourceModule: No authorized QMX USB device available on Android");
            return;
        }
        // Acquire a fresh USB handle for each start.
        if (!self->androidUsbHandle.acquire(backend::QMX_VIDPIDS)) {
            flog::error("QMXSourceModule: Failed to acquire USB device handle");
            return;
        }
        options.androidUsb.fd = self->androidUsbHandle.fd();
#endif

        self->sync.start(self->sync.getSyncVfo());

        std::string error;
        if (!self->device.start(options, &QMXSourceModule::sampleHandler, self, &QMXSourceModule::statusHandler, self, &error)) {
            flog::error("QMXSourceModule: {}", error);
#ifdef __ANDROID__
            self->androidUsbHandle.reset();
#endif
            return;
        }

        self->running = true;

        flog::info("QMXSourceModule '{}': Start!", self->name);
    }

    static void stop(void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        if (!self->running)
            return;

        self->running = false;
        if (self->catTransmitActive.exchange(false, std::memory_order_relaxed)) { sigpath::txState.emit(false); }
        self->stream.stopWriter();
        self->device.stop();
        self->stream.clearWriteStop();
        self->sync.stop();

#ifdef __ANDROID__
        self->androidUsbHandle.reset();
#endif

        flog::info("QMXSourceModule '{}': Stop!", self->name);
    }

    // freq is the center IQ frequency.
    static void tune(double freq, void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        self->freq = freq;
        self->sync.onIqCenterChanged(freq);

        config.edit().set("frequency", freq);
    }

    static void menuHandler(void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        // This source is never registered on a headless server (see the
        // constructor), so this menu only ever renders locally with a live
        // ImGui context.

#ifndef __ANDROID__
        if (self->running)
            SmGui::BeginDisabled();

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_qmx_audio_dev_", self->name), &self->audioDevId, self->audioDevices.txt)) {
            std::string dev = self->audioDevices.key(self->audioDevId);
            self->selectAudioDevice(dev);
            config.edit().set("audioDevice", dev);
        }

        float refreshBtnWidth = std::max(90.0f, ImGui::CalcTextSize("Refresh").x + (ImGui::GetStyle().FramePadding.x * 2.0f) + 4.0f);
        float serialComboWidth = ImGui::GetContentRegionAvail().x - refreshBtnWidth - ImGui::GetStyle().ItemSpacing.x;
        SmGui::SetNextItemWidth(std::max(1.0f, serialComboWidth));
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_qmx_serial_dev_", self->name), &self->serialPortId, self->serialPorts.txt)) {
            std::string port = self->serialPorts.key(self->serialPortId);
            self->selectSerialPort(port);
            config.edit().set("serialPort", port);
        }

        SmGui::SameLine();
        if (SmGui::Button(CONCAT("Refresh##_qmx_refr_", self->name), ImVec2(refreshBtnWidth, 0))) {
            self->refresh();
            self->selectAudioDevice(self->selectedAudioDevice);
            self->selectSerialPort(self->selectedSerialPort);
        }

        if (self->running)
            SmGui::EndDisabled();

        SmGui::Text("IQ Audio:");
        SmGui::SameLine();
        if (self->selectedAudioDevice.empty())
            SmGui::Text("Not selected");
        else
            SmGui::Text(self->audioDevices.value(self->audioDevId).label.c_str());

        SmGui::Text("CAT:");
        SmGui::SameLine();
        if (self->selectedSerialPort.empty())
            SmGui::Text("Manual tune only");
        else if (self->running)
            SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), self->serialPorts.value(self->serialPortId).label.c_str());
        else
            SmGui::Text(self->serialPorts.value(self->serialPortId).label.c_str());
#else
        self->refreshAndroidDeviceSelectionIfNeeded();
        if (self->running)
            SmGui::BeginDisabled();

        float refreshBtnWidth = std::max(90.0f, ImGui::CalcTextSize("Refresh").x + (ImGui::GetStyle().FramePadding.x * 2.0f) + 4.0f);
        float deviceComboWidth = ImGui::GetContentRegionAvail().x - refreshBtnWidth - ImGui::GetStyle().ItemSpacing.x;
        SmGui::SetNextItemWidth(std::max(1.0f, deviceComboWidth));
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_qmx_android_dev_", self->name), &self->androidDevId, self->androidDeviceListTxt.c_str())) {
            self->selectAndroidDeviceById(self->androidDevId);
            config.edit().set("device", self->selectedAndroidDevice);
        }

        SmGui::SameLine();
        if (SmGui::Button(CONCAT("Refresh##_qmx_refr_", self->name), ImVec2(refreshBtnWidth, 0)))
            self->refreshAndroidDeviceSelection();

        if (self->running)
            SmGui::EndDisabled();

        SmGui::Text("Device:");
        SmGui::SameLine();
        if (self->selectedAndroidDevice.empty())
            SmGui::Text("Not connected");
        else if (self->running)
            SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), self->selectedAndroidDevice.c_str());
        else
            SmGui::Text(self->selectedAndroidDevice.c_str());

        if (android_get_device_api_level() < __ANDROID_API_P__ || backend::audioOutputUsesOpenSLES()) {
            SmGui::TextColored(
                ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Disable 'USB audio routing' in Android developer options."
            );
        }
#endif
        // Display any device error in red.
        if (std::string error = self->device.lastError(); !error.empty())
            SmGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), error.c_str());

        //self->sync.tick();

        bool syncVfoCb = self->sync.getSyncVfo();
        SmGui::ForceSync();
        if (SmGui::Checkbox(CONCAT("Sync VFO##_qmx_sync_vfo_", self->name), &syncVfoCb)) {
            self->sync.setSyncVfo(syncVfoCb);
            config.edit().set("syncVfo", syncVfoCb);
        }

        SmGui::Separator();

        const auto& st = self->sync.currentStatus();
        if (st.empty()) {
            SmGui::Text("CAT Status:");
            SmGui::SameLine();
            SmGui::Text(self->running ? "Waiting for QMX status" : "Unavailable");
            return;
        }

        SmGui::Text("State:");
        SmGui::SameLine();
        if (st.hasTransmit() && st.transmit)
            SmGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "TX");
        else if (st.hasTransmit())
            SmGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "RX");
        else
            SmGui::Text("Unknown");

        SmGui::Text("Mode:");
        SmGui::SameLine();
        SmGui::Text(formatModeLabel(st).c_str());

        SmGui::Text("Rig Freq:");
        SmGui::SameLine();
        if (st.hasFrequency())
            SmGui::TextF("%.0f Hz", static_cast<double>(st.frequency));
        else
            SmGui::Text("Unknown");

        if (st.hasRxVfo() || st.hasTxVfo() || st.hasSplit()) {
            SmGui::Text("VFO:");
            SmGui::SameLine();
            SmGui::TextF("RX %s  TX %s  Split %s",
                         formatVfoLabel(st.rxVfo),
                         formatVfoLabel(st.txVfo),
                         st.hasSplit() ? (st.split ? "On" : "Off") : "?");
        }

        if (st.hasRit() || st.hasRitEnabled()) {
            SmGui::Text("RIT:");
            SmGui::SameLine();
            if (st.hasRit())
                SmGui::TextF("%s %d Hz", st.hasRitEnabled() ? (st.ritEnabled ? "On" : "Off") : "", st.ritHz);
            else
                SmGui::Text(st.ritEnabled ? "On" : "Off");
        }

        if (st.hasSMeter() && (!st.hasTransmit() || !st.transmit)) {
            SmGui::Text("S-Meter:");
            SmGui::SameLine();
            SmGui::TextF("%d dB", st.sMeterDb);
        }

        if (st.hasPower()) {
            SmGui::Text("Power:");
            SmGui::SameLine();
            SmGui::TextF("%.1f W", st.powerTenthsW / 10.0f);
        }

        if (st.hasSWR()) {
            SmGui::Text("SWR:");
            SmGui::SameLine();
            SmGui::TextF("%.2f:1", st.swrHundredths / 100.0f);
        }
    }

    static void sampleHandler(const qmx::IQSample* samples, std::size_t count, void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        if (!self->running || !samples || count == 0)
            return;

        if (self->catTransmitActive.load(std::memory_order_relaxed))
            std::fill_n(self->stream.writeBuf, count, dsp::complex_t{});
        else {
            for (std::size_t i = 0; i < count; ++i) {
                self->stream.writeBuf[i].re = samples[i].i;
                self->stream.writeBuf[i].im = samples[i].q;
            }
        }
        self->stream.swap(static_cast<int>(count));
    }

    static void statusHandler(const qmx::QmxStatus& status, void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        if (status.hasTransmit()) {
            bool wasActive = self->catTransmitActive.exchange(status.transmit, std::memory_order_relaxed);
            // Notify listeners (e.g. noise reduction freezes its noise profile
            // during TX) on transitions only.
            if (wasActive != status.transmit) { sigpath::txState.emit(status.transmit); }
        }
        self->sync.onStatusReceived(status);
    }

    static void frameDraw(MainWindow::FrameDrawArgs, void* ctx) {
        auto* self = static_cast<QMXSourceModule*>(ctx);
        self->sync.tick();
    }
    static const char* formatVfoLabel(int vfo) {
        if (vfo == 0)
            return "A";
        if (vfo == 1)
            return "B";
        return "?";
    }

    static std::string formatModeLabel(const qmx::QmxStatus& status) {
        if (status.hasMode()) {
            // Modes the app also has go through the same QMX->SDRIAK mapping the
            // sync uses, so the panel cannot name a mode differently from the
            // radio menu it is driving.
            int radioMode = FreqModeSync::qmxModeToRadioIface(status.mode);
            if (radioMode >= 0) { return radioModeName(radioMode); }
            // Rig modes with no demodulator here, so no canonical name exists.
            switch (status.mode) {
            case qmx::QmxMode::FSK:
                return "FSK";
            case qmx::QmxMode::FSKR:
                return "FSKR";
            default:
                return "Unknown";
            }
        }
        return "Unknown";
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double sampleRate = qmx::kSampleRate;
    double freq = 7000000.0;

    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    qmx::QmxDevice device;
    FreqModeSync sync;
    EventHandler<MainWindow::FrameDrawArgs> frameDrawHandler;
    std::atomic<bool> catTransmitActive{ false };

#ifndef __ANDROID__
    OptionList<std::string, AudioChoice> audioDevices;
    OptionList<std::string, SerialChoice> serialPorts;
    std::string selectedAudioDevice;
    std::string selectedSerialPort;
    int audioDevId = 0;
    int serialPortId = 0;
#else
    std::vector<std::string> androidDevices;
    std::string androidDeviceListTxt;
    std::string selectedAndroidDevice;
    int androidDevId = 0;
    backend::UsbDeviceLease androidUsbHandle;
    int lastAndroidUsbHotplugGeneration = 0;
#endif
};

MOD_EXPORT void _INIT_() {
    json def = json::object();
    def["frequency"] = 7000000.0;
    def["syncVfo"] = false;
#ifndef __ANDROID__
    def["audioDevice"] = "";
    def["serialPort"] = "";
#else
    def["device"] = "";
#endif
    config.setPath(core::args["root"].s() + "/qmx_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new QMXSourceModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete static_cast<QMXSourceModule*>(instance);
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
