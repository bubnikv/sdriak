#include <rtl_tcp_client.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <gui/remote/recent.h>
#include <gui/remote/tcp_probe.h>
#include <atomic>
#include <cstdio>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rtl_tcp_source",
    /* Description:     */ "RTL-TCP source module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 1, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class RTLTCPSourceModule : public ModuleManager::Instance {
public:
    RTLTCPSourceModule(std::string name) {
        this->name = name;

        // Define samplerates
        samplerates.define(250e3, "250KHz", 250e3);
        samplerates.define(1.024e6, "1.024MHz", 1.024e6);
        samplerates.define(1.536e6, "1.536MHz", 1.536e6);
        samplerates.define(1.792e6, "1.792MHz", 1.792e6);
        samplerates.define(1.92e6, "1.92MHz", 1.92e6);
        samplerates.define(2.048e6, "2.048MHz", 2.048e6);
        samplerates.define(2.16e6, "2.16MHz", 2.16e6);
        samplerates.define(2.4e6, "2.4MHz", 2.4e6);
        samplerates.define(2.56e6, "2.56MHz", 2.56e6);
        samplerates.define(2.88e6, "2.88MHz", 2.88e6);
        samplerates.define(3.2e6, "3.2MHz", 3.2e6);

        // Define direct sampling modes
        directSamplingModes.define(0, "Disabled", 0);
        directSamplingModes.define(1, "I branch", 1);
        directSamplingModes.define(2, "Q branch", 2);

        // Select the default samplerate instead of id 0
        srId = samplerates.valueId(2.4e6);

        // Load config
        {
            auto configAccess = config.read();
            std::string hostStr;
            if (configAccess.tryGet("host", hostStr)) { strcpy(ip, hostStr.c_str()); }
            configAccess.tryGet("port", port);
            double sr = 0.0;
            if (configAccess.tryGet("sampleRate", sr) && samplerates.keyExists(sr)) { srId = samplerates.keyId(sr); }
            int mode = 0;
            if (configAccess.tryGet("directSamplingMode", mode) && directSamplingModes.keyExists(mode)) { directSamplingId = directSamplingModes.keyId(mode); }
            configAccess.tryGet("ppm", ppm);
            configAccess.tryGet("gainIndex", gain);
            configAccess.tryGet("biasTee", biasTee);
            configAccess.tryGet("offsetTuning", offsetTuning);
        }

        // Update samplerate
        sampleRate = samplerates[srId];

        // Register source
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("RTL-TCP", &handler);

        // The remote browser runs in the GUI only.
        if (!core::args["server"].b()) {
            remote::RemoteHandler remoteHandler;
            remoteHandler.kind = remote::RemoteKind::RtlTcp;
            remoteHandler.sourceName = "RTL-TCP";
            remoteHandler.supportsTls = false;
            remoteHandler.probe = [](const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
                return remote::probeTcp(entry, cancel, 12);
            };
            remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
            sigpath::remoteRegistry.registerHandler(remoteHandler);
        }
    }

    ~RTLTCPSourceModule() {
        if (!core::args["server"].b()) { sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::RtlTcp); }
        stop(this);
        sigpath::sourceManager.unregisterSource("RTL-TCP");
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
    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        std::snprintf(ip, sizeof ip, "%s", entry.target.host.c_str());
        port = entry.target.port;
        {
            auto configAccess = config.edit();
            configAccess.set("host", std::string(ip));
            configAccess.set("port", port);
        }
        gui::mainWindow.setPlayState(true);
    }

    static void menuSelected(void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("RTLTCPSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;
        flog::info("RTLTCPSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;
        if (_this->running) { return; }
        
        // Connect to the server
        try {
            _this->client = rtltcp::connect(&_this->stream, _this->ip, _this->port);
            if (!core::args["server"].b()) {
                remote::recordRecent(remote::manualEntry(remote::RemoteKind::RtlTcp, remote::RemoteTarget{ "tcp", _this->ip, _this->port, "" }));
            }
        }
        catch (const std::exception& e) {
            flog::error("Could connect to RTL-TCP server: {}", e.what());
            return;
        }
        
        // Sync settings
        _this->client->setFrequency(_this->freq);
        _this->client->setSampleRate(_this->sampleRate);
        _this->client->setPPM(_this->ppm);
        _this->client->setDirectSampling(_this->directSamplingId);
        _this->client->setAGCMode(_this->rtlAGC);
        _this->client->setBiasTee(_this->biasTee);
        _this->client->setOffsetTuning(_this->offsetTuning);
        if (_this->tunerAGC) {
            _this->client->setGainMode(0);
        }
        else {
            _this->client->setGainMode(1);
            _this->client->setGainIndex(_this->gain);
        }

        _this->running = true;
        flog::info("RTLTCPSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->client->close();
        _this->running = false;
        flog::info("RTLTCPSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;
        if (_this->running) {
            _this->client->setFrequency(freq);
        }
        _this->freq = freq;
        flog::info("RTLTCPSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        RTLTCPSourceModule* _this = (RTLTCPSourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        if (SmGui::InputText(CONCAT("##_ip_select_", _this->name), _this->ip, 1024)) {
            config.edit().set("host", std::string(_this->ip));
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_port_select_", _this->name), &_this->port, 0)) {
            config.edit().set("port", _this->port);
        }

        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtltcp_sr_", _this->name), &_this->srId, _this->samplerates.txt)) {
            _this->sampleRate = _this->samplerates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            config.edit().set("sampleRate", _this->sampleRate);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Direct Sampling");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtltcp_ds_", _this->name), &_this->directSamplingId, "Disabled\0I branch\0Q branch\0")) {
            if (_this->running) {
                _this->client->setDirectSampling(_this->directSamplingId);
                _this->client->setGainIndex(_this->gain);
            }
            config.edit().set("directSamplingMode", _this->directSamplingId);
        }

        SmGui::LeftLabel("PPM Correction");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_rtltcp_ppm_", _this->name), &_this->ppm, 1, 10)) {
            if (_this->running) {
                _this->client->setPPM(_this->ppm);
            }
            config.edit().set("ppm", _this->ppm);
        }

        if (_this->tunerAGC) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_gain_select_", _this->name), &_this->gain, 0, 28, SmGui::FMT_STR_NONE)) {
            if (_this->running) {
                _this->client->setGainIndex(_this->gain);
            }
            config.edit().set("gainIndex", _this->gain);
        }
        if (_this->tunerAGC) { SmGui::EndDisabled(); }

        if (SmGui::Checkbox(CONCAT("Bias-T##_biast_select_", _this->name), &_this->biasTee)) {
            if (_this->running) {
                _this->client->setBiasTee(_this->biasTee);
            }
            config.edit().set("biasTee", _this->biasTee);
        }

        if (SmGui::Checkbox(CONCAT("Offset Tuning##_biast_select_", _this->name), &_this->offsetTuning)) {
            if (_this->running) {
                _this->client->setOffsetTuning(_this->offsetTuning);
            }
            config.edit().set("offsetTuning", _this->offsetTuning);
        }

        if (SmGui::Checkbox("RTL AGC", &_this->rtlAGC)) {
            if (_this->running) {
                _this->client->setAGCMode(_this->rtlAGC);
                if (!_this->rtlAGC) {
                    _this->client->setGainIndex(_this->gain);
                }
            }
            config.edit().set("rtlAGC", _this->rtlAGC);
        }

        SmGui::ForceSync();
        if (SmGui::Checkbox("Tuner AGC", &_this->tunerAGC)) {
            if (_this->running) {
                _this->client->setGainMode(!_this->tunerAGC);
                if (!_this->tunerAGC) {
                    _this->client->setGainIndex(_this->gain);
                }
            }
            config.edit().set("tunerAGC", _this->tunerAGC);
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
    std::shared_ptr<rtltcp::Client> client;
    bool running = false;
    double freq;

    char ip[1024] = "localhost";
    int port = 1234;
    int srId = 0;
    int directSamplingId = 0;
    int ppm = 0;
    int gain = 0;
    bool biasTee = false;
    bool offsetTuning = false;
    bool rtlAGC = false;
    bool tunerAGC = false;

    OptionList<double, double> samplerates;
    OptionList<int, int> directSamplingModes;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rtl_tcp_config.json");
    config.load(json({}));
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RTLTCPSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RTLTCPSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
