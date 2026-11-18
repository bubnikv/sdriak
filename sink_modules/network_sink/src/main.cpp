#include <utils/networking.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <config.h>
#include <gui/style.h>
#include <core.h>
#include <atomic>
#include <memory>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "network_sink",
    /* Description:     */ "Network sink module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

enum {
    SINK_MODE_TCP,
    SINK_MODE_UDP
};

const char* sinkModesTxt = "TCP\0UDP\0";

namespace {
    class NetworkSinkSession {
    public:
        explicit NetworkSinkSession(net::Conn connection)
            : connection(std::move(connection)) {}

        bool isOpen() const {
            return active.load() && connection && connection->isOpen();
        }

        bool write(int count, uint8_t* data) {
            if (!isOpen()) { return false; }
            if (!connection->write(count, data)) {
                active = false;
                connection->close();
                return false;
            }
            return true;
        }

        void waitForEnd() {
            if (connection) { connection->waitForEnd(); }
        }

        void close() {
            active = false;
            if (connection) { connection->close(); }
        }

    private:
        net::Conn connection;
        std::atomic<bool> active{true};
    };
}

class NetworkSink : SinkManager::Sink {
public:
    NetworkSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;

        // Load config
        bool startNow = false;
        {
            auto configAccess = config.edit();
            ConfigManager::EditSection stream = configAccess.section(_streamName);

            // Seed whatever this stream is missing, which for a stream never seen
            // before is the whole block.
            stream.ensure("hostname", "localhost");
            stream.ensure("port", 7355);
            stream.ensure("protocol", (int)SINK_MODE_UDP); // UDP
            stream.ensure("sampleRate", 48000.0);
            stream.ensure("stereo", false);
            stream.ensure("listening", false);

            std::string host;
            if (stream.tryGet("hostname", host)) { strcpy(hostname, host.c_str()); }
            stream.tryGet("port", port);
            stream.tryGet("protocol", modeId);
            stream.tryGet("sampleRate", sampleRate);
            stream.tryGet("stereo", stereo);
            stream.tryGet("listening", startNow);
        }

        netBuf = new int16_t[STREAM_BUFFER_SIZE];

        packer.init(_stream->sinkOut, 512);
        s2m.init(&packer.out);
        monoSink.init(&s2m.out, monoHandler, this);
        stereoSink.init(&packer.out, stereoHandler, this);


        // Create a list of sample rates
        for (int sr = 12000; sr < 200000; sr += 12000) {
            sampleRates.push_back(sr);
        }
        for (int sr = 11025; sr < 192000; sr += 11025) {
            sampleRates.push_back(sr);
        }

        // Sort sample rate list
        std::sort(sampleRates.begin(), sampleRates.end(), [](double a, double b) { return (a < b); });

        // Generate text list for UI
        char buffer[128];
        int id = 0;
        int _48kId;
        bool found = false;
        for (auto sr : sampleRates) {
            sprintf(buffer, "%d", (int)sr);
            sampleRatesTxt += buffer;
            sampleRatesTxt += '\0';
            if (sr == sampleRate) {
                srId = id;
                found = true;
            }
            if (sr == 48000.0) { _48kId = id; }
            id++;
        }
        if (!found) {
            srId = _48kId;
            sampleRate = 48000.0;
        }
        _stream->setSampleRate(sampleRate);

        // Start if needed
        if (startNow) { startServer(); }
    }

    ~NetworkSink() {
        stopServer();
        delete[] netBuf;
    }

    void start() {
        if (running) {
            return;
        }
        doStart();
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        doStop();
        running = false;
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool connected = false;
        bool listening = false;
        {
            std::lock_guard lck(sessionMtx);
            connected = session && session->isOpen();
            listening = connected || (listener && listener->isListening());
        }

        if (listening) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_network_sink_host_", _streamName), hostname, 1023)) {
            config.edit().section(_streamName).set("hostname", hostname);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_network_sink_port_", _streamName), &port, 0, 0)) {
            config.edit().section(_streamName).set("port", port);
        }

        ImGui::LeftLabel("Protocol");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_sink_mode_", _streamName), &modeId, sinkModesTxt)) {
            config.edit().section(_streamName).set("protocol", modeId);
        }

        if (listening) { style::endDisabled(); }

        ImGui::LeftLabel("Samplerate");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_sink_sr_", _streamName), &srId, sampleRatesTxt.c_str())) {
            sampleRate = sampleRates[srId];
            _stream->setSampleRate(sampleRate);
            packer.setSampleCount(sampleRate / 60);
            config.edit().section(_streamName).set("sampleRate", sampleRate);
        }

        if (ImGui::Checkbox(CONCAT("Stereo##_network_sink_stereo_", _streamName), &stereo)) {
            stop();
            start();
            config.edit().section(_streamName).set("stereo", stereo);
        }

        if (listening && ImGui::Button(CONCAT("Stop##_network_sink_stop_", _streamName), ImVec2(menuWidth, 0))) {
            stopServer();
            config.edit().section(_streamName).set("listening", false);
        }
        else if (!listening && ImGui::Button(CONCAT("Start##_network_sink_stop_", _streamName), ImVec2(menuWidth, 0))) {
            startServer();
            config.edit().section(_streamName).set("listening", true);
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (connected) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), (modeId == SINK_MODE_TCP) ? "Connected" : "Sending");
        }
        else if (listening) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }
    }

private:
    void doStart() {
        packer.start();
        if (stereo) {
            stereoSink.start();
        }
        else {
            flog::warn("Starting");
            s2m.start();
            monoSink.start();
        }
    }

    void doStop() {
        packer.stop();
        s2m.stop();
        monoSink.stop();
        stereoSink.stop();
    }

    void startServer() {
        net::Listener newListener;
        std::shared_ptr<NetworkSinkSession> newSession;
        try {
            if (modeId == SINK_MODE_TCP) {
                newListener = net::listen(hostname, port);
            }
            else {
                newSession = std::make_shared<NetworkSinkSession>(
                    net::openUDP("0.0.0.0", port, hostname, port, false));
            }
        }
        catch (const std::exception& e) {
            flog::error("Failed to open socket: {}", e.what());
            return;
        }

        // Anything left from a previous run is released only after
        // sessionMtx: a listener's destructor joins its accept worker, which
        // itself takes sessionMtx in clientHandler().
        net::Listener oldListener;
        std::shared_ptr<NetworkSinkSession> oldSession;
        {
            std::lock_guard lck(sessionMtx);
            stopping = false;
            oldListener = std::move(listener);
            oldSession = std::move(session);
            listener = std::move(newListener);
            session = std::move(newSession);
            if (listener) { listener->acceptAsync(clientHandler, this); }
        }
        if (oldSession) { oldSession->close(); }
    }

    void stopServer() {
        std::shared_ptr<NetworkSinkSession> oldSession;
        net::Listener oldListener;
        {
            std::lock_guard lck(sessionMtx);
            stopping = true;
            oldSession = std::move(session);
            oldListener = std::move(listener);
        }
        if (oldSession) { oldSession->close(); }
        if (oldListener) { oldListener->close(); }
    }

    static void monoHandler(float* samples, int count, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;
        std::shared_ptr<NetworkSinkSession> activeSession;
        {
            std::lock_guard lck(_this->sessionMtx);
            activeSession = _this->session;
        }
        if (!activeSession || !activeSession->isOpen()) { return; }
        std::lock_guard bufferLck(_this->netBufMtx);

        volk_32f_s32f_convert_16i(_this->netBuf, (float*)samples, 32768.0f, count);

        activeSession->write(count * sizeof(int16_t), (uint8_t*)_this->netBuf);
    }

    static void stereoHandler(dsp::stereo_t* samples, int count, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;
        std::shared_ptr<NetworkSinkSession> activeSession;
        {
            std::lock_guard lck(_this->sessionMtx);
            activeSession = _this->session;
        }
        if (!activeSession || !activeSession->isOpen()) { return; }
        std::lock_guard bufferLck(_this->netBufMtx);

        volk_32f_s32f_convert_16i(_this->netBuf, (float*)samples, 32768.0f, count * 2);

        activeSession->write(count * 2 * sizeof(int16_t), (uint8_t*)_this->netBuf);
    }

    static void clientHandler(net::Conn client, void* ctx) {
        NetworkSink* _this = (NetworkSink*)ctx;

        auto newSession = std::make_shared<NetworkSinkSession>(std::move(client));
        std::shared_ptr<NetworkSinkSession> oldSession;
        bool reject = false;
        {
            std::lock_guard lck(_this->sessionMtx);
            reject = _this->stopping;
            if (!reject) {
                oldSession = std::move(_this->session);
                _this->session = newSession;
            }
        }
        if (oldSession) { oldSession->close(); }
        if (reject) {
            newSession->close();
            return;
        }

        newSession->waitForEnd();
        newSession->close();

        {
            std::lock_guard lck(_this->sessionMtx);
            if (_this->session == newSession) { _this->session.reset(); }
            if (!_this->stopping && _this->listener && _this->listener->isListening()) {
                _this->listener->acceptAsync(clientHandler, _this);
            }
        }
    }

    SinkManager::Stream* _stream;
    dsp::buffer::Packer<dsp::stereo_t> packer;
    dsp::convert::StereoToMono s2m;
    dsp::sink::Handler<float> monoSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;

    std::string _streamName;

    int srId = 0;
    bool running = false;

    char hostname[1024];
    int port = 4242;

    int modeId = 1;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 48000;
    bool stereo = false;

    int16_t* netBuf;
    std::mutex netBufMtx;

    net::Listener listener;
    std::shared_ptr<NetworkSinkSession> session;
    std::mutex sessionMtx;
    bool stopping = true;
};

class NetworkSinkModule : public ModuleManager::Instance {
public:
    NetworkSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Network", provider);
    }

    ~NetworkSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Network");
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
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new NetworkSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/network_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    NetworkSinkModule* instance = new NetworkSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (NetworkSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
