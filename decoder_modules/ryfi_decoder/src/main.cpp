#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <filesystem>
#include <gui/widgets/folder_select.h>
#include <gui/widgets/constellation_diagram.h>
#include "ryfi/receiver.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "ryfi_decoder",
    /* Description:     */ "RyFi decoder for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

#define INPUT_BANDWIDTH     600e3
#define INPUT_SAMPLE_RATE   1000e3
#define INPUT_BAUDRATE      500e3

#define SYMBOL_DIAG_RATE    30
#define SYMBOL_DIAG_COUNT   1024

class RyFiDecoderModule : public ModuleManager::Instance {
public:
    RyFiDecoderModule(std::string name) {
        this->name = name;

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, INPUT_BANDWIDTH, INPUT_SAMPLE_RATE, INPUT_BANDWIDTH, INPUT_BANDWIDTH, true);
        rx.init(vfo->output, INPUT_BAUDRATE, INPUT_SAMPLE_RATE);
        rx.softTap->init(SYMBOL_DIAG_COUNT, (int)(INPUT_BAUDRATE / SYMBOL_DIAG_RATE), symTapHandler, this);
        rx.onPacket.bind(&RyFiDecoderModule::packetHandler, this);

        rx.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~RyFiDecoderModule() {
        rx.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0), INPUT_BANDWIDTH, INPUT_SAMPLE_RATE, INPUT_BANDWIDTH, INPUT_BANDWIDTH, true);

        rx.setInput(vfo->output);

        rx.start();

        enabled = true;
    }

    void disable() {
        rx.stop();

        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    void packetHandler(ryfi::Packet pkt) {
        flog::debug("Got a {} byte packet!", pkt.size());
    }

    static void menuHandler(void* ctx) {
        RyFiDecoderModule* _this = (RyFiDecoderModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        _this->constDiagram.draw();

        if (!_this->enabled) { style::endDisabled(); }
    }

    // Runs on the deframer's thread; tryWrite() drops the frame rather than
    // wait for the GUI.
    static void symTapHandler(const dsp::complex_t* data, int count, void* ctx) {
        RyFiDecoderModule* _this = (RyFiDecoderModule*)ctx;
        _this->constDiagram.tryWrite(data, count);
    }

    std::string name;
    bool enabled = true;

    // DSP Chain
    VFOManager::VFO* vfo;
    ryfi::Receiver rx;

    ImGui::ConstellationDiagram constDiagram;
};

MOD_EXPORT void _INIT_() {

}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RyFiDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RyFiDecoderModule*)instance;
}

MOD_EXPORT void _END_() {

}