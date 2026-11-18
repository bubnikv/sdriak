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
#include <atomic>
#include <memory>
#include <mutex>

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

namespace {
    class ExportSession {
    public:
        ExportSession(std::shared_ptr<net::Socket> socket, Protocol protocol,
                      SampleType sampleType, int packetSize)
            : socket(std::move(socket)), protocol(protocol), sampleType(sampleType),
              packetSize(packetSize), sampleSize(bytesPerSample(sampleType)) {
            buffer = dsp::buffer::alloc<uint8_t>(packetSize);
        }

        ~ExportSession() {
            close();
            dsp::buffer::free(buffer);
        }

        bool isOpen() const {
            return active.load() && socket && socket->isOpen();
        }

        // Retiring a session waits for at most the DSP block currently being
        // converted. The packetizer and its socket therefore disappear as one
        // unit, and no packet can straddle two clients.
        void close() {
            std::lock_guard lck(mtx);
            if (!active.exchange(false)) { return; }
            fillBytes = 0;
            sendOffset = 0;
            packetPending = false;
            if (socket) { socket->close(); }
        }

        // Preserve the old status probe without allowing it to race packet
        // transmission. IQ export peers normally send no data; a zero-byte
        // TCP read is used only to notice an orderly peer shutdown.
        bool probeOpen() {
            std::unique_lock lck(mtx, std::try_to_lock);
            if (!lck.owns_lock()) { return isOpen(); }
            if (!isOpen()) { return false; }
            if (protocol != PROTOCOL_UDP) {
                uint8_t dummy;
                if (socket->recv(&dummy, 1, false, net::NONBLOCKING) == 0) {
                    active = false;
                    return false;
                }
            }
            return isOpen();
        }

        void consume(dsp::complex_t* data, int count) {
            std::unique_lock lck(mtx, std::try_to_lock);
            if (!lck.owns_lock() || !isOpen()) { return; }

            // A non-blocking TCP send may have stopped part-way through a
            // packet. Finish that packet before accepting more samples; if
            // the socket is still full, drop this input block rather than
            // corrupting the byte stream with a new packet.
            if (packetPending && !flushTcpLocked()) { return; }

            while (count > 0 && active.load()) {
                const int remainingSamples = (packetSize - fillBytes) / sampleSize;
                const int n = std::min(count, remainingSamples);
                uint8_t* dst = &buffer[fillBytes];
                switch (sampleType) {
                case SAMPLE_TYPE_INT8:
                    volk_32f_s32f_convert_8i((int8_t*)dst, (float*)data, 128.0f, n * 2);
                    break;
                case SAMPLE_TYPE_INT16:
                    volk_32f_s32f_convert_16i((int16_t*)dst, (float*)data, 32768.0f, n * 2);
                    break;
                case SAMPLE_TYPE_INT32:
                    volk_32f_s32f_convert_32i((int32_t*)dst, (float*)data, 2147483647.0f, n * 2);
                    break;
                case SAMPLE_TYPE_FLOAT32:
                    memcpy(dst, data, n * sizeof(dsp::complex_t));
                    break;
                }
                data += n;
                count -= n;
                fillBytes += n * sampleSize;

                if (fillBytes != packetSize) { continue; }

                if (protocol == PROTOCOL_UDP) {
                    // A UDP packet is indivisible. A short/error send drops
                    // this datagram, as before, without carrying state into
                    // the next one.
                    socket->send(buffer, packetSize);
                    fillBytes = 0;
                    if (!socket->isOpen()) { active = false; }
                    continue;
                }

                packetPending = true;
                sendOffset = 0;
                if (!flushTcpLocked()) { return; }
            }
        }

    private:
        static int bytesPerSample(SampleType type) {
            switch (type) {
            case SAMPLE_TYPE_INT8:    return sizeof(int8_t) * 2;
            case SAMPLE_TYPE_INT16:   return sizeof(int16_t) * 2;
            case SAMPLE_TYPE_INT32:   return sizeof(int32_t) * 2;
            case SAMPLE_TYPE_FLOAT32: return sizeof(dsp::complex_t);
            default:                  return 0;
            }
        }

        bool flushTcpLocked() {
            while (packetPending && sendOffset < packetSize) {
                const int sent = socket->send(&buffer[sendOffset], packetSize - sendOffset);
                if (sent > 0) {
                    sendOffset += sent;
                    continue;
                }
                if (!socket->isOpen()) {
                    active = false;
                    packetPending = false;
                    fillBytes = 0;
                }
                return false;
            }
            packetPending = false;
            sendOffset = 0;
            fillBytes = 0;
            return true;
        }

        std::shared_ptr<net::Socket> socket;
        Protocol protocol;
        SampleType sampleType;
        int packetSize;
        int sampleSize;
        uint8_t* buffer = NULL;
        int fillBytes = 0;
        int sendOffset = 0;
        bool packetPending = false;
        std::atomic<bool> active{true};
        std::mutex mtx;
    };
}

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

        if (listenWorkerThread.joinable()) { listenWorkerThread.join(); }

        std::shared_ptr<net::Listener> newListener;
        std::shared_ptr<ExportSession> newSession;

        // Start listening or open UDP socket
        try {
            if (proto == PROTOCOL_TCP_SERVER) {
                // Create listener
                newListener = net::listen(hostname, port);
            }
            else if (proto == PROTOCOL_TCP_CLIENT) {
                // Connect to TCP server. Runs on the GUI thread, so bound
                // DNS + TCP connect instead of freezing the UI for the
                // OS-level connect timeout.
                newSession = std::make_shared<ExportSession>(
                    net::connect(hostname, port, 5000), proto, sampType, packetSize);
            }
            else {
                // Open UDP socket
                newSession = std::make_shared<ExportSession>(
                    net::openudp(hostname, port, "0.0.0.0", 0, true), proto, sampType, packetSize);
            }
        }
        catch (const std::exception& e) {
            flog::error("[IQExporter] Could not start socket: {}", e.what());
            errorStr = e.what();
            showError = true;
            return;
        }

        {
            std::lock_guard lck(sessionMtx);
            stopping = false;
            listener = newListener;
            session = newSession;
        }

        if (newListener) {
            try {
                listenWorkerThread = std::thread(&IQExporterModule::listenWorker, this,
                                                 newListener, sampType, packetSize);
            }
            catch (const std::exception& e) {
                std::shared_ptr<ExportSession> failedSession;
                {
                    std::lock_guard lck(sessionMtx);
                    stopping = true;
                    listener.reset();
                    failedSession = std::move(session);
                }
                newListener->stop();
                if (failedSession) { failedSession->close(); }
                flog::error("[IQExporter] Could not start listener worker: {}", e.what());
                errorStr = e.what();
                showError = true;
                return;
            }
        }

        running = true;
    }

    void stop() {
        if (!running && !listenWorkerThread.joinable()) { return; }

        std::shared_ptr<net::Listener> oldListener;
        std::shared_ptr<ExportSession> oldSession;
        {
            std::lock_guard lck(sessionMtx);
            stopping = true;
            oldListener = std::move(listener);
            oldSession = std::move(session);
        }

        // Never close or join while holding sessionMtx: an accept that won
        // the race must be able to observe `stopping`, reject its session,
        // and let the worker terminate.
        if (oldSession) { oldSession->close(); }
        if (oldListener) { oldListener->stop(); }
        if (listenWorkerThread.joinable()) { listenWorkerThread.join(); }

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

        bool sockOpen = false;
        bool listening = false;
        {
            std::lock_guard lck(_this->sessionMtx);
            sockOpen = _this->session && _this->session->probeOpen();
            listening = _this->listener && _this->listener->listening();
        }

        // Status text
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (sockOpen) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), (_this->proto == PROTOCOL_TCP_SERVER || _this->proto == PROTOCOL_TCP_CLIENT) ? "Connected" : "Sending");
        }
        else if (listening) {
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

    void listenWorker(std::shared_ptr<net::Listener> runListener,
                      SampleType runSampleType, int runPacketSize) {
        try {
            while (true) {
                // Accept a client
                auto newSock = runListener->accept();
                if (!newSock) { break; }

                auto newSession = std::make_shared<ExportSession>(
                    std::move(newSock), PROTOCOL_TCP_SERVER, runSampleType, runPacketSize);
                std::shared_ptr<ExportSession> oldSession;
                bool reject = false;
                {
                    std::lock_guard lck(sessionMtx);
                    reject = stopping || listener != runListener;
                    if (!reject) {
                        oldSession = std::move(session);
                        session = newSession;
                    }
                }
                if (reject) { newSession->close(); }
                if (oldSession) { oldSession->close(); }
            }
        }
        catch (const std::exception& e) {
            runListener->stop();
            flog::error("[IQExporter] Listener worker stopped: {}", e.what());
        }
    }

    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        IQExporterModule* _this = (IQExporterModule*)ctx;
        std::shared_ptr<ExportSession> activeSession;
        {
            std::lock_guard lck(_this->sessionMtx);
            activeSession = _this->session;
        }
        if (activeSession) { activeSession->consume(data, count); }
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

    std::thread listenWorkerThread;

    std::mutex sessionMtx;
    bool stopping = true;
    std::shared_ptr<ExportSession> session;
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
