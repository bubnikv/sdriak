#include "sdriak_server_client.h"
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <utils/optionlist.h>
#include <utils/async_connector.h>
#include <gui/dialogs/dialog_box.h>
#include <gui/remote/recent.h>
#include <gui/remote/tcp_probe.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdriak_server_source",
    /* Description:     */ "SDRIAK Server source module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class SDRIAKServerSourceModule : public ModuleManager::Instance {
public:
    SDRIAKServerSourceModule(std::string name) {
        this->name = name;

        // Yeah no server-ception, sorry...
        if (core::args["server"].b()) { return; }

        // Initialize lists
        sampleTypeList.define("Int8", dsp::compression::PCM_TYPE_I8);
        sampleTypeList.define("Int16", dsp::compression::PCM_TYPE_I16);
        sampleTypeList.define("Float32", dsp::compression::PCM_TYPE_F32);
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);

        prebufferList.define("Disabled", 0);
        prebufferList.define("50 ms", 50);
        prebufferList.define("100 ms", 100);
        prebufferList.define("250 ms", 250);
        prebufferList.define("500 ms", 500);
        prebufferList.define("1000 ms", 1000);
        rxPrebufferId = prebufferList.valueId(100);

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        // Load config
        {
            auto configAccess = config.read();
            std::string hostStr;
            if (configAccess.tryGet("hostname", hostStr)) { strcpy(hostname, hostStr.c_str()); }
            configAccess.tryGet("port", port);
            configAccess.tryGet("viaProxy", viaProxy);
        }
        loadPasswordForServer();

        // Per-frame GUI-thread pump for server-pushed state (samplerate,
        // tuning limits) cached by the client's network worker. Bound in
        // menuSelected, unbound in menuDeselected.
        frameDrawHandler.ctx = this;
        frameDrawHandler.handler = frameDraw;

        sigpath::sourceManager.registerSource("SDRIAK Server", &handler);

        // The remote browser runs in the GUI only.
        if (!core::args["server"].b()) {
            remote::RemoteHandler remoteHandler;
            remoteHandler.kind = remote::RemoteKind::SDRIAKServer;
            remoteHandler.sourceName = "SDRIAK Server";
            remoteHandler.supportsTls = false;
            remoteHandler.probe = [](const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
                return remote::probeTcp(entry, cancel);
            };
            remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
            sigpath::remoteRegistry.registerHandler(remoteHandler);
        }
    }

    ~SDRIAKServerSourceModule() {
        // Server mode: the constructor returned before registering.
        if (core::args["server"].b()) { return; }
        // If still selected, unregisterSource fires menuDeselected, which
        // unbinds frameDrawHandler.
        stop(this);
        // Stop the connect worker before members it uses are destroyed.
        connector.shutdown();
        if (!core::args["server"].b()) { sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::SDRIAKServer); }
        sigpath::sourceManager.unregisterSource("SDRIAK Server");
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
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        // Select/deselect only ever runs on the GUI thread, outside
        // onFrameDraw.emit() (source combo, startup, module unregister), so
        // mutating the handler list here is safe. SourceManager strictly
        // pairs the two calls, keeping bind/unbind balanced.
        gui::mainWindow.onFrameDraw.bindHandler(&_this->frameDrawHandler);
        if (_this->client) {
            // Force re-apply of samplerate and tuning limits; another local
            // source may have overwritten them while it was selected.
            _this->client->syncRemoteState(true);
        }
        gui::mainWindow.playButtonLocked = !(_this->client && _this->client->isOpen());
        flog::info("SDRIAKServerSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        _this->connector.cancel();
        gui::mainWindow.onFrameDraw.unbindHandler(&_this->frameDrawHandler);
        gui::mainWindow.playButtonLocked = false;
        // Release the remote limits so they don't leak into the next source.
        sigpath::sourceManager.clearTuningLimits();
        flog::info("SDRIAKServerSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void frameDraw(MainWindow::FrameDrawArgs, void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        // Only bound while this source is selected, so this can't stomp
        // another source's samplerate or tuning limits.
        if (_this->connected()) {
            _this->client->syncRemoteState();
        }
    }

    static void start(void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        if (_this->running) { return; }

        // Try to connect if not already connected (Play button is locked anyway so not sure why I put this here)
        if (!_this->connected()) {
            _this->tryConnect();
            if (!_this->connected()) { return; }
        }

        // Set configuration
        _this->client->setFrequency(_this->freq);
        _this->client->start();

        _this->running = true;
        flog::info("SDRIAKServerSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        if (!_this->running) { return; }

        if (_this->connected()) { _this->client->stop(); }

        _this->running = false;
        flog::info("SDRIAKServerSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;
        if (_this->running && _this->connected()) {
            _this->client->setFrequency(freq);
        }
        _this->freq = freq;
        flog::info("SDRIAKServerSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SDRIAKServerSourceModule* _this = (SDRIAKServerSourceModule*)ctx;

        _this->pollConnection();

        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool connected = _this->connected();
        const bool connecting = _this->connector.connecting();
        gui::mainWindow.playButtonLocked = !connected;

        ImGui::GenericDialog("##sdrpp_srv_src_err_dialog", _this->serverBusy, GENERIC_DIALOG_BUTTONS_OK, [=](){
            ImGui::TextUnformatted("This server is already in use.");
        });
        ImGui::GenericDialog("##sdrpp_srv_src_auth_dialog", _this->authFailed, GENERIC_DIALOG_BUTTONS_OK, [=](){
            ImGui::TextUnformatted("Authentication failed.");
        });

        if (connected || connecting) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##sdrpp_srv_srv_host_", _this->name), _this->hostname, 1023)) {
            config.edit().set("hostname", _this->hostname);
            _this->loadPasswordForServer();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##sdrpp_srv_srv_port_", _this->name), &_this->port, 0, 0)) {
            config.edit().set("port", _this->port);
            _this->loadPasswordForServer();
        }
        // CONNECT through the Network proxy; many proxies allow it only to 443.
        if (ImGui::Checkbox(CONCAT("Through the proxy##sdrpp_srv_srv_proxy_", _this->name), &_this->viaProxy)) {
            config.edit().set("viaProxy", _this->viaProxy);
        }
        ImGui::LeftLabel("Password");
        ImGui::FillWidth();
        if (ImGui::InputTextWithHint(CONCAT("##sdrpp_srv_srv_password_", _this->name), _this->savedAuthKeyValid ? "Saved" : "", _this->password, sizeof(_this->password), ImGuiInputTextFlags_Password)) {
            _this->savePasswordForServer();
        }
        if (_this->savedAuthKeyValid && _this->password[0] == 0) {
            if (ImGui::Button(CONCAT("Forget saved password##sdrpp_srv_srv_password_forget_", _this->name), ImVec2(menuWidth, 0))) {
                _this->clearPasswordForServer();
            }
        }
        if (connected || connecting) { style::endDisabled(); }

        if (_this->running) { style::beginDisabled(); }
        if (connecting) { style::beginDisabled(); }
        if (!connected && ImGui::Button("Connect##sdrpp_srv_source", ImVec2(menuWidth, 0))) {
            _this->tryConnect();
        }
        else if (connected && ImGui::Button("Disconnect##sdrpp_srv_source", ImVec2(menuWidth, 0))) {
            _this->client->close();
        }
        if (connecting) { style::endDisabled(); }
        if (_this->running) { style::endDisabled(); }


        if (connected) {
            ImGui::LeftLabel("Sample type");
            ImGui::FillWidth();
            if (ImGui::Combo("##sdrpp_srv_source_samp_type", &_this->sampleTypeId, _this->sampleTypeList.txt)) {
                _this->client->setSampleType(_this->sampleTypeList[_this->sampleTypeId]);

                // Save config
                config.edit().section("servers", _this->devConfName).set("sampleType", _this->sampleTypeList.key(_this->sampleTypeId));
            }
            
            if (ImGui::Checkbox("Compression", &_this->compression)) {
                _this->client->setCompression(_this->compression);

                // Save config
                config.edit().section("servers", _this->devConfName).set("compression", _this->compression);
            }

            ImGui::LeftLabel("RX prebuffer");
            ImGui::FillWidth();
            if (ImGui::Combo("##sdrpp_srv_source_rx_prebuf", &_this->rxPrebufferId, _this->prebufferList.txt)) {
                int prebufferMsec = _this->prebufferList[_this->rxPrebufferId];
                _this->client->setRxPrebufferMsec(prebufferMsec);
                config.edit().section("servers", _this->devConfName).set("rxPrebuffer", prebufferMsec);
            }

            bool dummy = true;
            style::beginDisabled();
            ImGui::Checkbox("Full IQ", &dummy);
            style::endDisabled();

            // Calculate datarate
            _this->frametimeCounter += ImGui::GetIO().DeltaTime;
            if (_this->frametimeCounter >= 0.2f) {
                _this->datarate = ((float)_this->client->bytes.exchange(0) / (_this->frametimeCounter * 1024.0f * 1024.0f)) * 8;
                _this->frametimeCounter = 0;
            }

            ImGui::TextUnformatted("Status:");
            ImGui::SameLine();
            int bufferPercent = _this->client->getRxPrebufferPercent();
            if (bufferPercent >= 0) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected (%.3f Mbit/s, %d%% buffer)", _this->datarate, bufferPercent);
            }
            else {
                // Prebuffer disabled: the stream is a live bypass.
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected (%.3f Mbit/s, live)", _this->datarate);
            }

            ImGui::CollapsingHeader("Source [REMOTE]", ImGuiTreeNodeFlags_DefaultOpen);

            _this->client->showMenu();
        }
        else {
            ImGui::TextUnformatted("Status:");
            ImGui::SameLine();
            ImGui::TextUnformatted(connecting ? "Connecting..." : "Not connected (--.--- Mbit/s)");
            if (!connecting && !_this->connector.lastError().empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Connection failed: %s", _this->connector.lastError().c_str());
            }
        }
    }

    bool connected() {
        return client && client->isOpen();
    }

    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        if (client && client->isOpen()) { client->close(); }
        std::snprintf(hostname, sizeof hostname, "%s", entry.target.host.c_str());
        port = entry.target.port;
        {
            auto configAccess = config.edit();
            configAccess.set("hostname", std::string(hostname));
            configAccess.set("port", port);
        }
        loadPasswordForServer();
        tryConnect();
    }

    void tryConnect() {
        if (connector.connecting()) { return; }

        if (port <= 0 || port > 65535) {
            connector.setError("Port must be between 1 and 65535");
            return;
        }

        if (client) {
            client->close();
            client.reset();
        }
        serverBusy = false;
        authFailed = false;

        const std::string host = hostname;
        const int targetPort = port;
        auto authKey = std::make_shared<server::AuthKey>();
        const bool useAuth = savedAuthKeyValid || password[0] != 0;
        if (savedAuthKeyValid) {
            *authKey = savedAuthKey;
        }
        else if (useAuth) {
            server::deriveAuthKey(std::string(password), *authKey);
        }

        const bool throughProxy = viaProxy;
        connector.begin([this, host, targetPort, authKey, useAuth, throughProxy]() {
            // Zero the key however the factory exits.
            struct KeyGuard {
                std::shared_ptr<server::AuthKey> key;
                ~KeyGuard() { std::fill(key->begin(), key->end(), 0); }
            } keyGuard{ authKey };

            return useAuth
                ? server::connect(host, targetPort, &stream, *authKey, 3000, connector.cancelToken(), throughProxy)
                : server::connect(host, targetPort, &stream, std::string(), 3000, connector.cancelToken(), throughProxy);
        });
    }

    void pollConnection() {
        std::shared_ptr<server::Client> newClient;
        auto res = connector.poll(newClient);
        if (res == ServerConnector::Result::FAILED) {
            const std::string& error = connector.lastError();
            flog::error("Could not connect to SDR: {}", error);
            if (error == "Server busy") { serverBusy = true; }
            if (error == "Authentication failed") { authFailed = true; }
            return;
        }
        if (res != ServerConnector::Result::CONNECTED) { return; }

        try {
            client = std::move(newClient);
            deviceInit();
            if (!core::args["server"].b()) {
                remote::recordRecent(remote::manualEntry(remote::RemoteKind::SDRIAKServer, remote::RemoteTarget{ "tcp", hostname, port, "" }));
            }
        }
        catch (const std::exception& e) {
            flog::error("Could not initialize SDR connection: {}", e.what());
            connector.setError(e.what());
            client.reset();
        }
    }

    void deviceInit() {
        // Generate the config name
        devConfName = serverConfigName();

        // Load settings. The read access unlocks on the way out of the scope,
        // including through an exception, so no hand-written unwind is needed.
        {
            auto configAccess = config.read();
            ConfigManager::ReadSection srv = configAccess.section("servers", devConfName);
            sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);
            std::string key;
            if (srv.tryGet("sampleType", key) && sampleTypeList.keyExists(key)) { sampleTypeId = sampleTypeList.keyId(key); }
            srv.tryGet("compression", compression);
            rxPrebufferId = prebufferList.valueId(100);
            int prebufferMsec = 0;
            if (srv.tryGet("rxPrebuffer", prebufferMsec) && prebufferList.valueExists(prebufferMsec)) { rxPrebufferId = prebufferList.valueId(prebufferMsec); }
        }

        // Set settings
        client->setSampleType(sampleTypeList[sampleTypeId]);
        client->setCompression(compression);
        client->setRxPrebufferMsec(prebufferList[rxPrebufferId]);
    }

    static std::string makeServerConfigName(const char* host, int port) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s:%05d", host, port);
        return std::string(buf);
    }

    std::string serverConfigName() const {
        return makeServerConfigName(hostname, port);
    }

    static std::string authRecordVersion() {
        return std::string(server::SERVER_AUTH_SALT);
    }

    static std::string authKeyToHex(const server::AuthKey& authKey) {
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.resize(authKey.size() * 2);
        for (size_t i = 0; i < authKey.size(); i++) {
            out[i * 2] = hex[(authKey[i] >> 4) & 0x0f];
            out[i * 2 + 1] = hex[authKey[i] & 0x0f];
        }
        return out;
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    }

    static bool authKeyFromHex(const std::string& hex, server::AuthKey& authKey) {
        if (hex.size() != authKey.size() * 2) { return false; }
        for (size_t i = 0; i < authKey.size(); i++) {
            int hi = hexDigit(hex[i * 2]);
            int lo = hexDigit(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                std::fill(authKey.begin(), authKey.end(), 0);
                return false;
            }
            authKey[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }

    static json makePasswordRecord(const server::AuthKey& authKey) {
        json record = json::object();
        record["version"] = authRecordVersion();
        record["key"] = authKeyToHex(authKey);
        return record;
    }

    static bool loadPasswordRecord(const json& record, server::AuthKey& authKey) {
        if (!record.is_object() ||
            !record.contains("version") || !record["version"].is_string() ||
            !record.contains("key") || !record["key"].is_string()) {
            return false;
        }

        std::string version = record["version"];
        if (version != authRecordVersion()) { return false; }

        std::string key = record["key"];
        return authKeyFromHex(key, authKey);
    }

    void clearSavedAuthKey() {
        std::fill(savedAuthKey.begin(), savedAuthKey.end(), 0);
        savedAuthKeyValid = false;
    }

    void savePasswordForServer() {
        if (password[0] == 0) {
            clearSavedAuthKey();
            config.edit().section("servers", serverConfigName()).erase("password");
        }
        else {
            // PBKDF2 is deliberately expensive. Derive and serialize before
            // acquiring the config mutex; the edit itself is one JSON write.
            server::deriveAuthKey(std::string(password), savedAuthKey);
            savedAuthKeyValid = true;
            json record = makePasswordRecord(savedAuthKey);
            config.edit().section("servers", serverConfigName()).set("password", record);
        }
    }

    void clearPasswordForServer() {
        password[0] = 0;
        savePasswordForServer();
    }

    void loadPasswordForServer() {
        password[0] = 0;
        clearSavedAuthKey();

        const std::string configName = serverConfigName();
        json record = config.read().section("servers", configName).value("password", json());
        if (record.is_null()) { return; }

        server::AuthKey authKey{};
        if (record.is_object() && loadPasswordRecord(record, authKey)) {
            savedAuthKey = authKey;
            savedAuthKeyValid = true;
            std::fill(authKey.begin(), authKey.end(), 0);
            return;
        }

        // Unreadable record: drop it rather than keep prompting against it.
        auto configAccess = config.edit();
        ConfigManager::EditSection srv = configAccess.section("servers", configName);
        // Do not erase a value that another thread replaced after our snapshot.
        if (srv.value("password", json()) == record) {
            srv.erase("password");
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;

    double freq;
    bool serverBusy = false;
    bool authFailed = false;

    float datarate = 0;
    float frametimeCounter = 0;

    char hostname[1024];
    char password[256] = {};
    server::AuthKey savedAuthKey{};
    bool savedAuthKeyValid = false;
    int port = 50000;
    bool viaProxy = false;
    std::string devConfName = "";

    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    EventHandler<MainWindow::FrameDrawArgs> frameDrawHandler;

    OptionList<std::string, dsp::compression::PCMType> sampleTypeList;
    OptionList<std::string, int> prebufferList;
    int sampleTypeId;
    int rxPrebufferId;
    bool compression = false;

    std::shared_ptr<server::Client> client;
    // Declared after every member its factory uses (stream, client), so its
    // destructor joins the connect worker before those are destroyed.
    using ServerConnector = AsyncConnector<std::shared_ptr<server::Client>>;
    ServerConnector connector;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["hostname"] = "localhost";
    def["port"] = 5259;
    def["servers"] = json::object();
    config.setPath(core::args["root"].s() + "/sdriak_server_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SDRIAKServerSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDRIAKServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
