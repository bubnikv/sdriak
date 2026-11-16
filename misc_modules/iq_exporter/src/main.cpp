#include <utils/net.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <algorithm>
#include <dsp/sink/handler_sink.h>
#include <volk/volk.h>
#include <signal_path/signal_path.h>
#include <gui/dialogs/dialog_box.h>
#include <core.h>

SDRPP_MOD_INFO{
    /* Name:            */ "iq_exporter",
    /* Description:     */ "Export raw IQ through TCP or UDP",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// Largest selectable packet, in bytes. Packet sizes are powers of two from 8,
// so every one is a whole number of samples of every sample type.
#define MAX_PACKET_SIZE 32768

enum Mode {
    MODE_NONE = -1,
    MODE_BASEBAND,
    MODE_VFO
};

enum Protocol {
    PROTOCOL_TCP_SERVER,
    PROTOCOL_TCP_CLIENT,
    PROTOCOL_UDP
};

enum SampleType {
    SAMPLE_TYPE_INT8,
    SAMPLE_TYPE_INT16,
    SAMPLE_TYPE_INT32,
    SAMPLE_TYPE_FLOAT32
};

class IQExporterModule : public ModuleManager::Instance {
public:
    IQExporterModule(std::string name) {
        this->name = name;

        // Define operating modes
        modes.define("Baseband", MODE_BASEBAND);
        modes.define("VFO", MODE_VFO);

        // Define VFO samplerates
        for (int i = 3000; i <= 192000; i <<= 1) {
            samplerates.define(i, getSrScaled(i), i);
        }
        for (int i = 250000; i < 1000000; i += 250000) {
            samplerates.define(i, getSrScaled(i), i);
        }
        for (int i = 1000000; i < 10000000; i += 500000) {
            samplerates.define(i, getSrScaled(i), i);
        }
        for (int i = 10000000; i <= 100000000; i += 5000000) {
            samplerates.define(i, getSrScaled(i), i);
        }

        // Define protocols
        protocols.define("TCP (Server)", PROTOCOL_TCP_SERVER);
        protocols.define("TCP (Client)", PROTOCOL_TCP_CLIENT);
        protocols.define("UDP", PROTOCOL_UDP);

        // Define sample types
        sampleTypes.define("Int8", SAMPLE_TYPE_INT8);
        sampleTypes.define("Int16", SAMPLE_TYPE_INT16);
        sampleTypes.define("Int32", SAMPLE_TYPE_INT32);
        sampleTypes.define("Float32", SAMPLE_TYPE_FLOAT32);

        // Define packet sizes
        for (int i = 8; i <= MAX_PACKET_SIZE; i <<= 1) {
            char buf[16];
            sprintf(buf, "%d Bytes", i);
            packetSizes.define(i, buf, i);
        }

        // Load config
        bool autoStart = false;
        Mode nMode = MODE_BASEBAND;
        {
            auto configAccess = config.read();
            ConfigManager::ReadSection inst = configAccess.section(name);
            std::string modeStr;
            if (inst.tryGet("mode", modeStr) && modes.keyExists(modeStr)) { nMode = modes.value(modes.keyId(modeStr)); }
            int sr = 0;
            if (inst.tryGet("samplerate", sr) && samplerates.keyExists(sr)) { samplerate = samplerates.value(samplerates.keyId(sr)); }
            std::string protoStr;
            if (inst.tryGet("protocol", protoStr) && protocols.keyExists(protoStr)) { proto = protocols.value(protocols.keyId(protoStr)); }
            std::string sampTypeStr;
            if (inst.tryGet("sampleType", sampTypeStr) && sampleTypes.keyExists(sampTypeStr)) { sampType = sampleTypes.value(sampleTypes.keyId(sampTypeStr)); }
            int size = 0;
            if (inst.tryGet("packetSize", size) && packetSizes.keyExists(size)) { packetSize = packetSizes.value(packetSizes.keyId(size)); }
            std::string hostStr;
            if (inst.tryGet("host", hostStr)) { strcpy(hostname, hostStr.c_str()); }
            if (inst.tryGet("port", port)) { port = std::clamp<int>(port, 1, 65535); }
            inst.tryGet("running", autoStart);
        }

        // Set menu IDs
        modeId = modes.valueId(nMode);
        srId = samplerates.valueId(samplerate);
        protoId = protocols.valueId(proto);
        sampTypeId = sampleTypes.valueId(sampType);
        packetSizeId = packetSizes.valueId(packetSize);

        // Packet assembly buffer: one packet, whatever the input block size.
        buffer = dsp::buffer::alloc<uint8_t>(MAX_PACKET_SIZE);

        // Init DSP. The handler cuts packets itself (see dataHandler()).
        handler.init(&iqStream, dataHandler, this);

        // Set operating mode
        setMode(nMode);

        // Start if needed
        if (autoStart) { start(); }

        // Register menu entry
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~IQExporterModule() {
        // Un-register menu entry
        gui::menu.removeEntry(name);

        // Stop networking
        stop();

        // Stop DSP
        setMode(MODE_NONE);

        // Free buffer
        dsp::buffer::free(buffer);
    }

    void postInit() {}

    void enable() {
        // Rebind streams and start DSP
        setMode(mode, true);

        // Restart networking if it was running
        if (wasRunning) { start(); }

        // Mark as running
        enabled = true;
    }

    void disable() {
        // Save running state
        wasRunning = running;

        // Stop networking
        stop();

        // Stop the DSP and unbind streams
        setMode(MODE_NONE);

        // Mark as disabled
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        if (running) { return; }

        // Acquire lock on the socket
        std::lock_guard lck1(sockMtx);

        // Start listening or open UDP socket
        try {
            if (proto == PROTOCOL_TCP_SERVER) {
                // Create listener
                listener = net::listen(hostname, port);

                // Start listen worker
                listenWorkerThread = std::thread(&IQExporterModule::listenWorker, this);
            }
            else if (proto == PROTOCOL_TCP_CLIENT) {
                // Connect to TCP server. Runs on the GUI thread, so bound
                // DNS + TCP connect instead of freezing the UI for the
                // OS-level connect timeout.
                sock = net::connect(hostname, port, 5000);
            }
            else {
                // Open UDP socket
                sock = net::openudp(hostname, port, "0.0.0.0", 0, true);
            }
        }
        catch (const std::exception& e) {
            flog::error("[IQExporter] Could not start socket: {}", e.what());
            errorStr = e.what();
            showError = true;
            return;
        }

        running = true;
    }

    void stop() {
        if (!running) { return; }

        // Acquire lock on the socket
        std::lock_guard lck1(sockMtx);

        // Stop listening or close UDP socket
        if (proto == PROTOCOL_TCP_SERVER) {
            // Stop listener
            if (listener) {
                listener->stop();
            }

            // Wait for worker to stop
            if (listenWorkerThread.joinable()) { listenWorkerThread.join(); }

            // Free listener
            listener.reset();

            // Close socket and free it
            if (sock) {
                sock->close();
                sock.reset();
            }
        }
        else {
            // Close socket and free it
            if (sock) {
                sock->close();
                sock.reset();
            }
        }

        running = false;
    }

private:
    std::string getSrScaled(double sr) {
        char buf[1024];
        if (sr >= 1000000.0) {
            sprintf(buf, "%.1lf MS/s", sr / 1000000.0);
        }
        else if (sr >= 1000.0) {
            sprintf(buf, "%.1lf KS/s", sr / 1000.0);
        }
        else {
            sprintf(buf, "%.1lf S/s", sr);
        }
        return std::string(buf);
    }

    static void menuHandler(void* ctx) {
        IQExporterModule* _this = (IQExporterModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Error message box
        ImGui::GenericDialog("##iq_exporter_err_", _this->showError, GENERIC_DIALOG_BUTTONS_OK, [=](){
            ImGui::Text("Error: %s", _this->errorStr.c_str());
        });
        
        if (!_this->enabled) { ImGui::BeginDisabled(); }
        
        if (_this->running) { ImGui::BeginDisabled(); }

        // Mode selector
        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(("##iq_exporter_mode_" + _this->name).c_str(), &_this->modeId, _this->modes.txt)) {
            _this->setMode(_this->modes.value(_this->modeId));
            config.edit().section(_this->name).set("mode", _this->modes.key(_this->modeId));
        }

        // In VFO mode, show samplerate selector
        if (_this->mode == MODE_VFO) {
            ImGui::LeftLabel("Samplerate");
            ImGui::FillWidth();
            if (ImGui::Combo(("##iq_exporter_sr_" + _this->name).c_str(), &_this->srId, _this->samplerates.txt)) {
                _this->samplerate = _this->samplerates.value(_this->srId);
                if (_this->vfo) {
                    _this->vfo->setBandwidthLimits(_this->samplerate, _this->samplerate, true);
                    _this->vfo->setSampleRate(_this->samplerate, _this->samplerate);
                }
                config.edit().section(_this->name).set("samplerate", _this->samplerates.key(_this->srId));
            }
        }

        // Mode protocol selector
        ImGui::LeftLabel("Protocol");
        ImGui::FillWidth();
        if (ImGui::Combo(("##iq_exporter_proto_" + _this->name).c_str(), &_this->protoId, _this->protocols.txt)) {
            _this->proto = _this->protocols.value(_this->protoId);
            config.edit().section(_this->name).set("protocol", _this->protocols.key(_this->protoId));
        }

        // Sample type selector
        ImGui::LeftLabel("Sample type");
        ImGui::FillWidth();
        if (ImGui::Combo(("##iq_exporter_samp_" + _this->name).c_str(), &_this->sampTypeId, _this->sampleTypes.txt)) {
            _this->sampType = _this->sampleTypes.value(_this->sampTypeId);
            config.edit().section(_this->name).set("sampleType", _this->sampleTypes.key(_this->sampTypeId));
        }

        // Packet size selector
        ImGui::LeftLabel("Packet size");
        ImGui::FillWidth();
        if (ImGui::Combo(("##iq_exporter_pkt_sz_" + _this->name).c_str(), &_this->packetSizeId, _this->packetSizes.txt)) {
            _this->packetSize = _this->packetSizes.value(_this->packetSizeId);
            config.edit().section(_this->name).set("packetSize", _this->packetSizes.key(_this->packetSizeId));
        }

        // Hostname and port field
        if (ImGui::InputText(("##iq_exporter_host_" + _this->name).c_str(), _this->hostname, sizeof(_this->hostname))) {
            config.edit().section(_this->name).set("host", _this->hostname);
        }
        ImGui::SameLine();
        ImGui::FillWidth();
        if (ImGui::InputInt(("##iq_exporter_port_" + _this->name).c_str(), &_this->port, 0, 0)) {
            _this->port = std::clamp<int>(_this->port, 1, 65535);
            config.edit().section(_this->name).set("port", _this->port);
        }

        if (_this->running) { ImGui::EndDisabled(); }

        // Start/Stop buttons
        if (_this->running || (!_this->enabled && _this->wasRunning)) {
            if (ImGui::Button(("Stop##iq_exporter_stop_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->stop();
                config.edit().section(_this->name).set("running", false);
            }
        }
        else {
            if (ImGui::Button(("Start##iq_exporter_start_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->start();
                config.edit().section(_this->name).set("running", true);
            }
        }

        // Check if the socket is open by attempting a read
        bool sockOpen;
        {
            uint8_t dummy;
            sockOpen = !(!_this->sock || !_this->sock->isOpen() || (_this->proto != PROTOCOL_UDP && _this->sock->recv(&dummy, 1, false, net::NONBLOCKING) == 0));
        }

        // Status text
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (sockOpen) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), (_this->proto == PROTOCOL_TCP_SERVER || _this->proto == PROTOCOL_TCP_CLIENT) ? "Connected" : "Sending");
        }
        else if (_this->listener && _this->listener->listening()) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
        }
        else if (!_this->enabled) {
            ImGui::TextUnformatted("Disabled");
        }
        else {
            // If we're idle and still supposed to be running, the server has closed the connection (TODO: kinda jank...)
            if (_this->running) { _this->stop(); }

            ImGui::TextUnformatted("Idle");
        }

        if (!_this->enabled) { ImGui::EndDisabled(); }
    }

    void setMode(Mode newMode, bool forceSet = false) {
        // If there is no mode to change, do nothing
        if (!forceSet && mode == newMode) { return; }

        // Stop the DSP
        handler.stop();

        // Delete VFO or unbind IQ stream
        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = NULL;
        }
        if (streamBound) {
            sigpath::iqFrontEnd.unbindIQStream(&iqStream);
            streamBound = false;
        }

        // If the mode was none, we're done
        if (newMode == MODE_NONE) { return; }

        // Create VFO or bind IQ stream
        if (newMode == MODE_VFO) {
            // Create VFO
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, samplerate, samplerate, samplerate, samplerate, true);

            // Set its output as the input to the DSP
            handler.setInput(vfo->output);
        }
        else {
            // Bind IQ stream
            sigpath::iqFrontEnd.bindIQStream(&iqStream);
            streamBound = true;

            // Set its output as the input to the DSP
            handler.setInput(&iqStream);
        }

        // Start DSP
        handler.start();

        // Update mode
        mode = newMode;
        modeId = modes.valueId(newMode);
    }

    void listenWorker() {
        while (true) {
            // Accept a client
            auto newSock = listener->accept();
            if (!newSock) { break; }

            // Update socket
            {
                std::lock_guard lck(sockMtx);
                sock = newSock;
            }
        }
    }

    int sampleSize() {
        switch (sampType) {
        case SAMPLE_TYPE_INT8:
            return sizeof(int8_t)*2;
        case SAMPLE_TYPE_INT16:
            return sizeof(int16_t)*2;
        case SAMPLE_TYPE_INT32:
            return sizeof(int32_t)*2;
        case SAMPLE_TYPE_FLOAT32:
            return sizeof(dsp::complex_t);
        default:
            return -1;
        }
    }

    // Runs on the handler's thread, straight off the input stream. Converts
    // into `buffer` and sends it each time it holds exactly packetSize bytes,
    // so packets are cut in bytes, where they are defined, whatever block
    // size the input delivers. The conversion is the only copy.
    //
    // packetSize and sampType are only read with sockMtx held and a socket
    // open. The menu cannot change them while running, and start() takes
    // sockMtx after any change, which orders the two threads.
    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        IQExporterModule* _this = (IQExporterModule*)ctx;

        // Try to acquire lock on socket
        if (!_this->sockMtx.try_lock()) { _this->pktFill = 0; return; }

        // If not valid or open, give up
        if (!_this->sock || !_this->sock->isOpen()) {
            _this->pktFill = 0;
            _this->sockMtx.unlock();
            return;
        }

        // Start a fresh packet on a new client or new settings, so a packet
        // never mixes sample formats or straddles two connections.
        if (_this->sock.get() != _this->pktSock || _this->packetSize != _this->pktSize || _this->sampType != _this->pktType) {
            _this->pktSock = _this->sock.get();
            _this->pktSize = _this->packetSize;
            _this->pktType = _this->sampType;
            _this->pktFill = 0;
        }

        const int sampSize = _this->sampleSize();
        const int pktSamples = _this->pktSize / sampSize;
        while (count > 0) {
            int n = std::min<int>(count, pktSamples - _this->pktFill);
            uint8_t* dst = &_this->buffer[_this->pktFill * sampSize];
            switch (_this->pktType) {
            case SAMPLE_TYPE_INT8:
                volk_32f_s32f_convert_8i((int8_t*)dst, (float*)data, 128.0f, n*2);
                break;
            case SAMPLE_TYPE_INT16:
                volk_32f_s32f_convert_16i((int16_t*)dst, (float*)data, 32768.0f, n*2);
                break;
            case SAMPLE_TYPE_INT32:
                volk_32f_s32f_convert_32i((int32_t*)dst, (float*)data, 2147483647.0f, n*2);
                break;
            case SAMPLE_TYPE_FLOAT32:
                memcpy(dst, data, n * sizeof(dsp::complex_t));
                break;
            }
            data += n;
            count -= n;
            _this->pktFill += n;

            // Send a complete packet
            if (_this->pktFill == pktSamples) {
                _this->sock->send(_this->buffer, _this->pktSize);
                _this->pktFill = 0;
            }
        }

        // Unlock socket mutex
        _this->sockMtx.unlock();
    }

    std::string name;
    bool enabled = true;

    Mode mode = MODE_NONE;
    int modeId;
    int samplerate = 1000000.0;
    int srId;
    Protocol proto = PROTOCOL_TCP_SERVER;
    int protoId;
    SampleType sampType = SAMPLE_TYPE_INT16;
    int sampTypeId;
    int packetSize = 1024;
    int packetSizeId;
    char hostname[1024] = "localhost";
    int port = 1234;
    bool running = false;
    bool wasRunning = false;

    bool showError = false;
    std::string errorStr = "";

    OptionList<std::string, Mode> modes;
    OptionList<int, int> samplerates;
    OptionList<std::string, Protocol> protocols;
    OptionList<std::string, SampleType> sampleTypes;
    OptionList<int, int> packetSizes;

    VFOManager::VFO* vfo = NULL;
    bool streamBound = false;
    dsp::stream<dsp::complex_t> iqStream;
    dsp::sink::Handler<dsp::complex_t> handler;

    // Packet being assembled. Touched only by dataHandler(), on the handler
    // thread; a dropped block resets it, so a packet never has a gap.
    uint8_t* buffer = NULL;
    int pktFill = 0;                    // samples already in `buffer`
    int pktSize = 0;                    // bytes, as of the packet in progress
    SampleType pktType = SAMPLE_TYPE_INT16;
    const net::Socket* pktSock = NULL;  // socket the packet is meant for

    std::thread listenWorkerThread;

    std::mutex sockMtx;
    std::shared_ptr<net::Socket> sock;
    std::shared_ptr<net::Listener> listener;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    std::string root = (std::string)core::args["root"];
    config.setPath(root + "/iq_exporter_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new IQExporterModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (IQExporterModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
