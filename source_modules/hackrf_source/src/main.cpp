#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <gui/smgui.h>

#ifndef __ANDROID__
#include <libhackrf/hackrf.h>
#else
#include <android_backend.h>
#include <hackrf.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hackrf_source",
    /* Description:     */ "HackRF source module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

const char* AGG_MODES_STR = "Off\0Low\0High\0";

const char* sampleRatesTxt = "20MHz\00016MHz\00010MHz\0008MHz\0005MHz\0004MHz\0002MHz\000";

const int sampleRates[] = {
    20000000,
    16000000,
    10000000,
    8000000,
    5000000,
    4000000,
    2000000,
};

const int bandwidths[] = {
    1750000,
    2500000,
    3500000,
    5000000,
    5500000,
    6000000,
    7000000,
    8000000,
    9000000,
    10000000,
    12000000,
    14000000,
    15000000,
    20000000,
    24000000,
    28000000,
};

const char* bandwidthsTxt = "1.75MHz\0"
                            "2.5MHz\0"
                            "3.5MHz\0"
                            "5MHz\0"
                            "5.5MHz\0"
                            "6MHz\0"
                            "7MHz\0"
                            "8MHz\0"
                            "9MHz\0"
                            "10MHz\0"
                            "12MHz\0"
                            "14MHz\0"
                            "15MHz\0"
                            "20MHz\0"
                            "24MHz\0"
                            "28MHz\0"
                            "Auto\0";

class HackRFSourceModule : public ModuleManager::Instance {
public:
    HackRFSourceModule(std::string name) {
        this->name = name;

        hackrf_error initErr = (hackrf_error)hackrf_init();
        if (initErr == HACKRF_SUCCESS) {
            initialized = true;
        }
        else {
            flog::error("Could not initialize HackRF library: {0}", hackrf_error_name(initErr));
        }

        // Select the last samplerate option
        sampleRate = 2000000;
        srId = 6;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        if (initialized) { refresh(); }

        std::string confSerial = config.read().value("device", std::string());
        selectBySerial(confSerial);

        sigpath::sourceManager.registerSource("HackRF", &handler);
    }

    ~HackRFSourceModule() {
        stop(this);
        if (initialized) {
            hackrf_error err = (hackrf_error)hackrf_exit();
            if (err != HACKRF_SUCCESS) {
                flog::error("Could not shut down HackRF library: {0}", hackrf_error_name(err));
            }
        }
        sigpath::sourceManager.unregisterSource("HackRF");
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

    void refresh() {
        if (!initialized) {
            flog::error("Tried to refresh HackRF devices before the library was initialized");
            return;
        }

        devList.clear();
        devListTxt = "";

#ifndef __ANDROID__
        hackrf_device_list_t* _devList = hackrf_device_list();
        if (_devList == NULL) {
            flog::error("Could not enumerate HackRF devices: {0}", hackrf_error_name(HACKRF_ERROR_LIBUSB));
            return;
        }

        for (int i = 0; i < _devList->devicecount; i++) {
            // Skip devices that are in use
            if (_devList->serial_numbers[i] == NULL) { continue; }

            // Save the device serial number
            std::string serial = _devList->serial_numbers[i];
            devList.push_back(serial);
            devListTxt += serial.length() > 16 ? serial.substr(16) : serial;
            devListTxt += '\0';
        }

        hackrf_device_list_free(_devList);
#else
        if (!backend::hasUsbDeviceAvailable(backend::HACKRF_VIDPIDS)) { return; }
        std::string fakeName = "HackRF USB";
        devList.push_back("fake_serial");
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst() {
        if (devList.size() != 0) {
            selectBySerial(devList[0]);
            return;
        }
        selectedSerial = "";
    }

    void selectBySerial(std::string serial) {
        if (std::find(devList.begin(), devList.end(), serial) == devList.end()) {
            selectFirst();
            return;
        }

        // Set default values
        srId = 0;
        sampleRate = 2000000;
        biasT = false;
        amp = false;
        lna = 0;
        vga = 0;
        bwId = 16;

        {
            auto configAccess = config.edit();
            ConfigManager::EditSection dev = configAccess.section("devices", serial);

            // Seed whatever this device is missing, which for a device never seen
            // before is the whole block.
            dev.ensure("sampleRate", 2000000);
            dev.ensure("biasT", false);
            dev.ensure("amp", false);
            dev.ensure("lnaGain", 0);
            dev.ensure("vgaGain", 0);
            dev.ensure("bandwidth", 16);

            // Load from config if available and validate
            int psr = 0;
            if (dev.tryGet("sampleRate", psr)) {
                for (int i = 0; i < 7; i++) {
                    if (sampleRates[i] == psr) {
                        sampleRate = psr;
                        srId = i;
                    }
                }
            }
            dev.tryGet("biasT", biasT);
            dev.tryGet("amp", amp);
            dev.tryGet("lnaGain", lna);
            dev.tryGet("vgaGain", vga);
            if (dev.tryGet("bandwidth", bwId)) {
                bwId = std::clamp<int>(bwId, 0, 16);
            }
        }

        selectedSerial = serial;
    }

private:
#ifdef __ANDROID__
    void refreshAndroidSelection() {
        std::string previousSerial = selectedSerial;
        refresh();
        selectBySerial(previousSerial);
        core::setInputSampleRate(sampleRate);
        lastAndroidUsbHotplugGeneration = backend::usbHotplugGeneration.load(std::memory_order_relaxed);
    }

    void refreshAndroidSelectionIfNeeded() {
        if (running) {
            return;
        }

        int generation = backend::usbHotplugGeneration.load(std::memory_order_relaxed);
        if (generation == lastAndroidUsbHotplugGeneration) {
            return;
        }

        refreshAndroidSelection();
    }
#endif

    static void menuSelected(void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("HackRFSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;
        flog::info("HackRFSourceModule '{0}': Menu Deselect!", _this->name);
    }

    int bandwidthIdToBw(int id) {
        if (id == 16) { return hackrf_compute_baseband_filter_bw(sampleRate); }
        return bandwidths[id];
    }

    bool settingApplied(const char* setting, int result, bool warning = false) {
        hackrf_error err = (hackrf_error)result;
        if (err == HACKRF_SUCCESS) { return true; }

        if (warning) {
            flog::warn("Could not set HackRF {0} {1}: {2}", selectedSerial, setting, hackrf_error_name(err));
        }
        else {
            flog::error("Could not set HackRF {0} {1}: {2}", selectedSerial, setting, hackrf_error_name(err));
        }
        return false;
    }

    static void start(void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;
        if (_this->running) { return; }
        if (!_this->initialized) {
            flog::error("Tried to start HackRF source before the library was initialized");
            return;
        }
#ifdef __ANDROID__
        _this->refreshAndroidSelectionIfNeeded();
#endif
        if (_this->selectedSerial == "") {
            flog::error("Tried to start HackRF source with empty serial");
            return;
        }

#ifndef __ANDROID__
        hackrf_error err = (hackrf_error)hackrf_open_by_serial(_this->selectedSerial.c_str(), &_this->openDev);
#else
        if (!_this->androidUsbHandle.acquire(backend::HACKRF_VIDPIDS)) {
            flog::error("Tried to start HackRF source without a valid USB handle");
            return;
        }
        hackrf_error err = (hackrf_error)hackrf_open_by_fd(_this->androidUsbHandle.fd(), &_this->openDev);
#endif
        if (err != HACKRF_SUCCESS) {
            flog::error("Could not open HackRF {0}: {1}", _this->selectedSerial, hackrf_error_name(err));
#ifdef __ANDROID__
            _this->androidUsbHandle.reset();
#endif
            return;
        }

        auto closeAfterStartFailure = [&]() {
            hackrf_error closeErr = (hackrf_error)hackrf_close(_this->openDev);
            _this->openDev = NULL;
            if (closeErr != HACKRF_SUCCESS) {
                flog::error("Could not close HackRF {0} after source start failure: {1}", _this->selectedSerial, hackrf_error_name(closeErr));
            }
#ifdef __ANDROID__
            _this->androidUsbHandle.reset();
#endif
        };

        if (!_this->settingApplied("sample rate", hackrf_set_sample_rate(_this->openDev, _this->sampleRate))) {
            closeAfterStartFailure();
            return;
        }
        if (!_this->settingApplied("baseband filter bandwidth", hackrf_set_baseband_filter_bandwidth(_this->openDev, _this->bandwidthIdToBw(_this->bwId)))) {
            closeAfterStartFailure();
            return;
        }
        if (!_this->settingApplied("frequency", hackrf_set_freq(_this->openDev, _this->freq))) {
            closeAfterStartFailure();
            return;
        }

        _this->settingApplied("antenna power", hackrf_set_antenna_enable(_this->openDev, _this->biasT), true);
        _this->settingApplied("RF amplifier", hackrf_set_amp_enable(_this->openDev, _this->amp), true);
        _this->settingApplied("LNA gain", hackrf_set_lna_gain(_this->openDev, _this->lna), true);
        _this->settingApplied("VGA gain", hackrf_set_vga_gain(_this->openDev, _this->vga), true);

        err = (hackrf_error)hackrf_start_rx(_this->openDev, callback, _this);
        if (err != HACKRF_SUCCESS) {
            flog::error("Could not start HackRF {0}: {1}", _this->selectedSerial, hackrf_error_name(err));
            closeAfterStartFailure();
            return;
        }

        _this->running = true;
        flog::info("HackRFSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->stream.stopWriter();
        // TODO: Stream stop
        hackrf_error err = (hackrf_error)hackrf_close(_this->openDev);
        _this->openDev = NULL;
        if (err != HACKRF_SUCCESS) {
            flog::error("Could not close HackRF {0}: {1}", _this->selectedSerial, hackrf_error_name(err));
        }
        _this->stream.clearWriteStop();
#ifdef __ANDROID__
        _this->androidUsbHandle.reset();
#endif
        flog::info("HackRFSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;
        if (_this->running) {
            _this->settingApplied("frequency", hackrf_set_freq(_this->openDev, freq));
        }
        _this->freq = freq;
        flog::info("HackRFSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        HackRFSourceModule* _this = (HackRFSourceModule*)ctx;

#ifdef __ANDROID__
        _this->refreshAndroidSelectionIfNeeded();
#endif
        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_hackrf_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectBySerial(_this->devList[_this->devId]);
            core::setInputSampleRate(_this->sampleRate);
            config.edit().set("device", _this->selectedSerial);
        }

        if (SmGui::Combo(CONCAT("##_hackrf_sr_sel_", _this->name), &_this->srId, sampleRatesTxt)) {
            _this->sampleRate = sampleRates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            config.edit().section("devices", _this->selectedSerial).set("sampleRate", _this->sampleRate);
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hackrf_refr_", _this->name))) {
#ifdef __ANDROID__
            _this->refreshAndroidSelection();
#else
            _this->refresh();
            _this->selectBySerial(_this->selectedSerial);
            core::setInputSampleRate(_this->sampleRate);
#endif
        }

        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_hackrf_bw_sel_", _this->name), &_this->bwId, bandwidthsTxt)) {
            if (_this->running) {
                _this->settingApplied("baseband filter bandwidth", hackrf_set_baseband_filter_bandwidth(_this->openDev, _this->bandwidthIdToBw(_this->bwId)), true);
            }
            config.edit().section("devices", _this->selectedSerial).set("bandwidth", _this->bwId);
        }

        SmGui::LeftLabel("LNA Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_hackrf_lna_", _this->name), &_this->lna, 0, 40, 8, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (_this->running) {
                _this->settingApplied("LNA gain", hackrf_set_lna_gain(_this->openDev, _this->lna), true);
            }
            config.edit().section("devices", _this->selectedSerial).set("lnaGain", (int)_this->lna);
        }

        SmGui::LeftLabel("VGA Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_hackrf_vga_", _this->name), &_this->vga, 0, 62, 2, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (_this->running) {
                _this->settingApplied("VGA gain", hackrf_set_vga_gain(_this->openDev, _this->vga), true);
            }
            config.edit().section("devices", _this->selectedSerial).set("vgaGain", (int)_this->vga);
        }

        if (SmGui::Checkbox(CONCAT("Bias-T##_hackrf_bt_", _this->name), &_this->biasT)) {
            if (_this->running) {
                _this->settingApplied("antenna power", hackrf_set_antenna_enable(_this->openDev, _this->biasT), true);
            }
            config.edit().section("devices", _this->selectedSerial).set("biasT", _this->biasT);
        }

        if (SmGui::Checkbox(CONCAT("Amp Enabled##_hackrf_amp_", _this->name), &_this->amp)) {
            if (_this->running) {
                _this->settingApplied("RF amplifier", hackrf_set_amp_enable(_this->openDev, _this->amp), true);
            }
            config.edit().section("devices", _this->selectedSerial).set("amp", _this->amp);
        }
    }

    static int callback(hackrf_transfer* transfer) {
        HackRFSourceModule* _this = (HackRFSourceModule*)transfer->rx_ctx;
        volk_8i_s32f_convert_32f((float*)_this->stream.writeBuf, (int8_t*)transfer->buffer, 128.0f, transfer->valid_length);
        if (!_this->stream.swap(transfer->valid_length / 2)) { return -1; }
        return 0;
    }

    std::string name;
    hackrf_device* openDev = NULL;
    bool initialized = false;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    int sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    std::string selectedSerial = "";
    int devId = 0;
    int srId = 0;
    int bwId = 16;
    bool biasT = false;
    bool amp = false;
    float lna = 0;
    float vga = 0;

#ifdef __ANDROID__
    backend::UsbDeviceLease androidUsbHandle;
    int lastAndroidUsbHotplugGeneration = 0;
#endif

    std::vector<std::string> devList;
    std::string devListTxt;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/hackrf_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HackRFSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HackRFSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
