#include <utils/networking.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <recorder_interface.h>
#include <meteor_demodulator_interface.h>
#include <config.h>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <radio_interface.h>
#include <atomic>
#include <memory>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define MAX_COMMAND_LENGTH 8192

SDRPP_MOD_INFO{
    /* Name:            */ "rigctl_server",
    /* Description:     */ "My fancy new module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

enum {
    RECORDER_TYPE_RECORDER,
    RECORDER_TYPE_METEOR_DEMODULATOR
};

ConfigManager config;

class SigctlServerModule : public ModuleManager::Instance {
private:
    struct ClientSession {
        explicit ClientSession(net::Conn connection)
            : connection(std::move(connection)) {}

        bool isOpen() const {
            return active.load() && connection && connection->isOpen();
        }

        int read(int count, bool enforceSize = false) {
            if (!isOpen()) { return -1; }
            return connection->read(count, dataBuf, enforceSize);
        }

        bool write(size_t count, uint8_t* data) {
            std::lock_guard lck(ioMtx);
            if (!isOpen()) { return false; }
            if (!connection->write((int)count, data)) {
                active = false;
                connection->close();
                return false;
            }
            return true;
        }

        void close() {
            active = false;
            if (connection) { connection->close(); }
        }

        uint8_t dataBuf[1024];
        std::string command;

    private:
        // Declared after parser state so the connection is destroyed first
        // and any in-flight operation has finished before those buffers die.
        net::Conn connection;
        std::atomic<bool> active{true};
        std::mutex ioMtx;
    };

public:
    SigctlServerModule(std::string name) {
        this->name = name;

        {
            auto configAccess = config.edit();
            ConfigManager::EditSection inst = configAccess.section(name);

            // Seed whatever this instance is missing, which for one never seen
            // before is the whole block.
            inst.ensure("host", "localhost");
            inst.ensure("port", 4532);
            inst.ensure("tuning", true);
            inst.ensure("recording", false);
            inst.ensure("autoStart", false);
            inst.ensure("vfo", "");
            inst.ensure("recorder", "");

            std::string host;
            if (inst.tryGet("host", host)) { strcpy(hostname, host.c_str()); }
            inst.tryGet("port", port);
            inst.tryGet("tuning", tuningEnabled);
            inst.tryGet("recording", recordingEnabled);
            inst.tryGet("autoStart", autoStart);
            inst.tryGet("vfo", selectedVfo);
            inst.tryGet("recorder", selectedRecorder);
        }

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~SigctlServerModule() {
        gui::menu.removeEntry(name);
        sigpath::vfoManager.onVfoCreated.unbindHandler(&vfoCreatedHandler);
        sigpath::vfoManager.onVfoDeleted.unbindHandler(&vfoDeletedHandler);
        core::moduleManager.onInstanceCreated.unbindHandler(&modChangedHandler);
        core::moduleManager.onInstanceDeleted.unbindHandler(&modChangedHandler);
        stopServer();
    }

    void postInit() {
        // Refresh modules
        refreshModules();

        // Select VFO and recorder from config
        selectVfoByName(selectedVfo);
        selectRecorderByName(selectedRecorder);

        // Bind handlers
        vfoCreatedHandler.handler = _vfoCreatedHandler;
        vfoCreatedHandler.ctx = this;
        vfoDeletedHandler.handler = _vfoDeletedHandler;
        vfoDeletedHandler.ctx = this;
        modChangedHandler.handler = _modChangeHandler;
        modChangedHandler.ctx = this;
        sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);
        sigpath::vfoManager.onVfoDeleted.bindHandler(&vfoDeletedHandler);
        core::moduleManager.onInstanceCreated.bindHandler(&modChangedHandler);
        core::moduleManager.onInstanceDeleted.bindHandler(&modChangedHandler);

        // If autostart is enabled, start the server
        if (autoStart) { startServer(); }
    }

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
        SigctlServerModule* _this = (SigctlServerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool connected = false;
        bool listening = false;
        {
            std::lock_guard lck(_this->sessionMtx);
            connected = _this->session && _this->session->isOpen();
            listening = _this->listener && _this->listener->isListening();
        }

        if (listening) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigctl_srv_host_", _this->name), _this->hostname, 1023)) {
            config.edit().section(_this->name).set("host", std::string(_this->hostname));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigctl_srv_port_", _this->name), &_this->port, 0, 0)) {
            config.edit().section(_this->name).set("port", _this->port);
        }
        if (listening) { style::endDisabled(); }

        ImGui::LeftLabel("Controlled VFO");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        {
            std::lock_guard lck(_this->vfoMtx);
            if (ImGui::Combo(CONCAT("##_rigctl_srv_vfo_", _this->name), &_this->vfoId, _this->vfoNamesTxt.c_str())) {
                _this->selectVfoByName(_this->vfoNames[_this->vfoId], false);
                if (!_this->selectedVfo.empty()) {
                    config.edit().section(_this->name).set("vfo", _this->selectedVfo);
                }
            }
        }

        ImGui::LeftLabel("Controlled Recorder");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        {
            std::lock_guard lck(_this->vfoMtx);
            if (ImGui::Combo(CONCAT("##_rigctl_srv_rec_", _this->name), &_this->recorderId, _this->recorderNamesTxt.c_str())) {
                _this->selectRecorderByName(_this->recorderNames[_this->recorderId], false);
                if (!_this->selectedRecorder.empty()) {
                    config.edit().section(_this->name).set("recorder", _this->selectedRecorder);
                }
            }
        }

        ImGui::BeginTable(CONCAT("Stop##_rigctl_srv_tbl_", _this->name), 2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Checkbox(CONCAT("Tuning##_rigctl_srv_tune_ena_", _this->name), &_this->tuningEnabled)) {
            config.edit().section(_this->name).set("tuning", _this->tuningEnabled);
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Checkbox(CONCAT("Recording##_rigctl_srv_tune_ena_", _this->name), &_this->recordingEnabled)) {
            config.edit().section(_this->name).set("recording", _this->recordingEnabled);
        }
        ImGui::EndTable();

        if (ImGui::Checkbox(CONCAT("Listen on startup##_rigctl_srv_auto_lst_", _this->name), &_this->autoStart)) {
            config.edit().section(_this->name).set("autoStart", _this->autoStart);
        }

        if (listening && ImGui::ActionButton(CONCAT("Stop##_rigctl_srv_stop_", _this->name))) {
            _this->stopServer();
        }
        else if (!listening && ImGui::ActionButton(CONCAT("Start##_rigctl_srv_stop_", _this->name))) {
            _this->startServer();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (connected) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
        }
        else if (listening) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }
    }

    void startServer() {
        net::Listener newListener;
        try {
            newListener = net::listen(hostname, port);
        }
        catch (const std::exception& e) {
            flog::error("Could not start rigctl server: {}", e.what());
            return;
        }

        // A previous, no longer listening listener is released only after
        // sessionMtx: its destructor joins its accept worker, which itself
        // takes sessionMtx in clientHandler().
        net::Listener oldListener;
        {
            std::lock_guard lck(sessionMtx);
            stopping = false;
            oldListener = std::move(listener);
            listener = std::move(newListener);
            listener->acceptAsync(clientHandler, this);
        }
    }

    void stopServer() {
        std::shared_ptr<ClientSession> oldSession;
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

    void refreshModules() {
        vfoNames.clear();
        vfoNamesTxt.clear();
        recorderNames.clear();
        recorderNamesTxt.clear();

        // List recording capable modules
        for (auto const& [_name, inst] : core::moduleManager.instances) {
            std::string mod = core::moduleManager.getInstanceModuleName(_name);
            if (mod != "recorder" && mod != "meteor_demodulator") { continue; }
            recorderNames.push_back(_name);
            recorderNamesTxt += _name;
            recorderNamesTxt += '\0';
        }

        // List VFOs
        for (auto const& [_name, vfo] : gui::waterfall.vfos) {
            vfoNames.push_back(_name);
            vfoNamesTxt += _name;
            vfoNamesTxt += '\0';
        }
    }

    void selectVfoByName(std::string _name, bool lock = true) {
        if (vfoNames.empty()) {
            if (lock) { std::lock_guard lck(vfoMtx); }
            selectedVfo.clear();
            return;
        }

        // Find the ID of the VFO, if not found, select first VFO in the list
        auto vfoIt = std::find(vfoNames.begin(), vfoNames.end(), _name);
        if (vfoIt == vfoNames.end()) {
            selectVfoByName(vfoNames[0]);
            return;
        }

        // Select the VFO
        {
            if (lock) { std::lock_guard lck(vfoMtx); }
            vfoId = std::distance(vfoNames.begin(), vfoIt);
            selectedVfo = _name;
        }
    }

    void selectRecorderByName(std::string _name, bool lock = true) {
        if (recorderNames.empty()) {
            if (lock) { std::lock_guard lck(recorderMtx); }
            selectedRecorder.clear();
            return;
        }

        // Find the ID of the VFO, if not found, select first VFO in the list
        auto recIt = std::find(recorderNames.begin(), recorderNames.end(), _name);
        if (recIt == recorderNames.end()) {
            selectRecorderByName(recorderNames[0]);
            return;
        }

        std::string type = core::modComManager.getModuleName(_name);


        // Select the VFO
        {
            if (lock) { std::lock_guard lck(recorderMtx); }
            recorderId = std::distance(recorderNames.begin(), recIt);
            selectedRecorder = _name;
            if (type == "meteor_demodulator") {
                recorderType = RECORDER_TYPE_METEOR_DEMODULATOR;
            }
            else {
                recorderType = RECORDER_TYPE_RECORDER;
            }
        }
    }

    static void _vfoCreatedHandler(VFOManager::VFO* vfo, void* ctx) {
        SigctlServerModule* _this = (SigctlServerModule*)ctx;
        _this->refreshModules();
        _this->selectVfoByName(_this->selectedVfo);
    }

    static void _vfoDeletedHandler(std::string _name, void* ctx) {
        SigctlServerModule* _this = (SigctlServerModule*)ctx;
        _this->refreshModules();
        _this->selectVfoByName(_this->selectedVfo);
    }

    static void _modChangeHandler(std::string _name, void* ctx) {
        SigctlServerModule* _this = (SigctlServerModule*)ctx;
        _this->refreshModules();
        _this->selectRecorderByName(_this->selectedRecorder);
    }

    static void clientHandler(net::Conn _client, void* ctx) {
        SigctlServerModule* _this = (SigctlServerModule*)ctx;
        auto newSession = std::make_shared<ClientSession>(std::move(_client));
        std::shared_ptr<ClientSession> oldSession;
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

        // The listener's one worker services the current client directly.
        // This preserves the old one-client-at-a-time behavior without the
        // ConnClass read and write worker threads.
        while (newSession->isOpen()) {
            int count = newSession->read(sizeof(newSession->dataBuf), false);
            if (count <= 0) { break; }
            _this->processData(newSession.get(), count, newSession->dataBuf);
        }
        newSession->close();

        {
            std::lock_guard lck(_this->sessionMtx);
            if (_this->session == newSession) { _this->session.reset(); }
            if (!_this->stopping && _this->listener && _this->listener->isListening()) {
                _this->listener->acceptAsync(clientHandler, _this);
            }
        }
    }

    void processData(ClientSession* client, int count, uint8_t* data) {
        for (int i = 0; i < count; i++) {
            if (data[i] == '\n') {
                commandHandler(client, client->command);
                client->command.clear();
                continue;
            }
            if (client->command.size() < MAX_COMMAND_LENGTH) { client->command += (char)data[i]; }
        }
    }

    // Hamlib mode names. The app's canonical spellings (RADIO_IFACE_MODE_NAMES)
    // are used unchanged except for one, and that is the protocol's requirement
    // rather than a naming preference, so it lives here and not in the shared
    // table: Hamlib's RIG_MODE_FM is documented as "narrow" band FM, with
    // RIG_MODE_WFM as the broadcast one, so our NFM goes out as "FM".
    static const char* hamlibModeName(int mode) {
        if (mode == RADIO_IFACE_MODE_NFM) { return "FM"; }
        // Unreachable: the caller seeds `mode` with a valid value, and the radio
        // only ever writes selectedDemodID, which selectDemodByID() assigns from
        // an ID instantiateDemod() accepted. An out-of-range selectedDemodID
        // (corrupt radio_config.json) leaves selectedDemod NULL, and the module
        // rejects the whole command in that state. Still guarded rather than
        // asserted alone, because an NDEBUG build must not put radioModeName()'s
        // "--" placeholder on the wire for a client to parse as a mode.
        assert(radioModeValid(mode));
        if (!radioModeValid(mode)) { return "RAW"; }
        return radioModeName(mode);
    }

    static int hamlibModeFromName(const std::string& name) {
        // radioModeFromName() happens to accept "FM" as well, but that is
        // leniency for hand-edited data and may go away; here it is the
        // protocol's requirement, so it is stated rather than relied upon.
        if (name == "FM") { return RADIO_IFACE_MODE_NFM; }
        return radioModeFromName(name.c_str());
    }

    // The `M ?` capability list: every mode the server accepts, in enum order,
    // under the same names `m` reports. Derived from the canonical table rather
    // than written out, because the hand-written literal this replaces was left
    // behind when CWR was appended to the enum -- it advertised eight modes
    // while hamlibModeFromName() accepted nine, and while this module's own
    // dump_state already set RIG_MODE_CWR (0x80) in its mode bitfield and
    // offered CW|CWR filter widths. Building it here is what keeps the three
    // from disagreeing again.
    static const std::string& hamlibModeList() {
        static const std::string list = [] {
            std::string s;
            for (int i = 0; i < _RADIO_IFACE_MODE_COUNT; i++) {
                if (i) { s += ' '; }
                s += hamlibModeName(i);
            }
            s += '\n';
            return s;
        }();
        return list;
    }

    static bool isVfoToken(const std::string& s) {
        return s == "VFO" || s == "VFOA" || s == "VFOB" || s == "VFOC" || s == "currVFO" ||
               s == "Main" || s == "Sub" || s == "MEM" || s == "TX" || s == "RX" || s == "None";
    }

    void commandHandler(ClientSession* session, std::string cmd) {
        // Keep the existing response sites concise while binding every reply
        // to the session that supplied this command, never a module-global
        // "current client" that a reconnect could replace.
        ClientSession* client = session;
        std::string corr = "";
        std::vector<std::string> parts;
        bool lastWasSpace = false;
        std::string resp = "";

        // Split command into parts and remove excess spaces
        for (char c : cmd) {
            if (lastWasSpace && c == ' ') { continue; }
            else if (c == ' ') {
                parts.push_back(corr);
                corr.clear();
                lastWasSpace = true;
            }
            else {
                lastWasSpace = false;
                corr += c;
            }
        }
        if (!corr.empty()) {
            parts.push_back(corr);
        }

        // NOTE: THIS STUFF ISN'T THREADSAFE AND WILL LIKELY BREAK.

        // If the command is empty, do nothing
        if (parts.size() == 0) { return; }

        // Hamlib clients in VFO mode (WSJT-X <= 2.6, JS8Call) insert a VFO
        // argument after the command (e.g. "F VFOA 7074000"). Drop it so the
        // argument counts match, except for set_vfo where it IS the argument.
        if (parts.size() > 1 && isVfoToken(parts[1]) && parts[0] != "V" && parts[0] != "\\set_vfo") {
            parts.erase(parts.begin() + 1);
        }

        // If the command is a compound command, execute each one separately
        if (parts[0].size() > 1 && parts[0][0] != '\\' && parts[0] != "AOS" && parts[0] != "LOS") {
            std::string arguments;
            if (parts.size() > 1) { arguments = cmd.substr(parts[0].size()); }
            for (char c : parts[0]) {
                commandHandler(session, c + arguments);
            }
            return;
        }

        flog::info("Rigctl command: '{0}'", cmd);

        // Otherwise, execute the command
        if (parts[0] == "F" || parts[0] == "\\set_freq") {
            std::lock_guard lck(vfoMtx);

            // if number of arguments isn't correct, return error
            if (parts.size() != 2) {
                resp = "RPRT 1\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // If not controlling the VFO, return
            if (!tuningEnabled) {
                resp = "RPRT 0\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // Parse frequency and assign it to the VFO
            long long freq = std::stoll(parts[1]);
            gui::mainWindow.runOnUiThread([vfo = selectedVfo, freq] {
                tuner::tune(tuner::TUNER_MODE_NORMAL, vfo, freq);
            });
            resp = "RPRT 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "f" || parts[0] == "\\get_freq") {
            std::lock_guard lck(vfoMtx);

            // Get center frequency of the SDR
            double freq = gui::waterfall.getCenterFrequency();

            // Add the offset of the VFO if it exists
            if (sigpath::vfoManager.vfoExists(selectedVfo)) {
                freq += sigpath::vfoManager.getOffset(selectedVfo);
            }

            // Respond with the frequency
            char buf[128];
            sprintf(buf, "%" PRIu64 "\n", (uint64_t)freq);
            client->write(strlen(buf), (uint8_t*)buf);
        }
        else if (parts[0] == "M" || parts[0] == "\\set_mode") {
            std::lock_guard lck(vfoMtx);
            resp = "RPRT 0\n";

            // If client is querying, respond accordingly
            if (parts.size() >= 2 && parts[1] == "?") {
                resp = hamlibModeList();
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // if number of arguments isn't correct, return error
            if (parts.size() != 3) {
                resp = "RPRT 1\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // Check that the bandwidth is an integer (0 or -1 for default bandwidth)
            int pos = 0;
            for (char c : parts[2]) {
                if (!std::isdigit(c) && !(c == '-' && !pos)) {
                    resp = "RPRT 1\n";
                    client->write(resp.size(), (uint8_t*)resp.c_str());
                    return;
                }
                pos++;
            }

            const std::string& newModeStr = parts[1];
            float newBandwidth = std::atoi(parts[2].c_str());
            
            int newMode = hamlibModeFromName(newModeStr);
            if (newMode < 0) {
                resp = "RPRT 1\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // If tuning is enabled, set the mode and optionally the bandwidth
            if (!selectedVfo.empty() && core::modComManager.getModuleName(selectedVfo) == "radio" && tuningEnabled) {
                core::modComManager.callInterface(selectedVfo, RADIO_IFACE_CMD_SET_MODE, &newMode, NULL);
                if (newBandwidth > 0) {
                    core::modComManager.callInterface(selectedVfo, RADIO_IFACE_CMD_SET_BANDWIDTH, &newBandwidth, NULL);
                }
            }

            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "m" || parts[0] == "\\get_mode") {
            std::lock_guard lck(vfoMtx);
            resp = "RAW\n";

            // Replace raw with the mode if there is an active VFO and it belongs to a radio
            if (!selectedVfo.empty() && core::modComManager.getModuleName(selectedVfo) == "radio") {
                // The radio leaves the output untouched when no demod is
                // selected, so initialize to a value with a known name.
                int mode = RADIO_IFACE_MODE_RAW;
                core::modComManager.callInterface(selectedVfo, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                resp = std::string(hamlibModeName(mode)) + "\n";
            }

            // Append the bandwidth of the VFO if there is one
            if (!selectedVfo.empty()) {
                double bw = sigpath::vfoManager.getBandwidth(selectedVfo);
                resp += std::to_string(std::isnan(bw) ? 0 : (int)bw) + "\n";
            }
            else {
                resp += "0\n";
            }

            // Send baack the response
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "V" || parts[0] == "\\set_vfo") {
            std::lock_guard lck(vfoMtx);
            resp = "RPRT 0\n";

            // if number of arguments isn't correct or the VFO is not "VFO", return error
            if (parts.size() != 2) {
                resp = "RPRT 1\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            if (parts[1] == "?") {
                resp = "VFO\n";
            }
            else if (!isVfoToken(parts[1])) {
                resp = "RPRT 1\n";
            }

            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "v" || parts[0] == "\\get_vfo") {
            std::lock_guard lck(vfoMtx);
            resp = "VFO\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "t" || parts[0] == "\\get_ptt") {
            // Receive only, PTT is always off
            resp = "0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "T" || parts[0] == "\\set_ptt") {
            // Accept and ignore, there is no transmitter to key
            resp = "RPRT 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "\\chk_vfo") {
            std::lock_guard lck(vfoMtx);
            resp = "CHKVFO 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "s") {
            std::lock_guard lck(vfoMtx);
            resp = "0\nVFOA\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "S") {
            std::lock_guard lck(vfoMtx);
            resp = "RPRT 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "AOS" || parts[0] == "\\recorder_start") {
            std::lock_guard lck(recorderMtx);

            // If not controlling the recorder, return
            if (!recordingEnabled) {
                resp = "RPRT 0\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // Send the command to the selected recorder
            if (recorderType == RECORDER_TYPE_METEOR_DEMODULATOR) {
                core::modComManager.callInterface(selectedRecorder, METEOR_DEMODULATOR_IFACE_CMD_START, NULL, NULL);
            }
            else {
                core::modComManager.callInterface(selectedRecorder, RECORDER_IFACE_CMD_START, NULL, NULL);
            }

            // Respond with a success
            resp = "RPRT 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "LOS" || parts[0] == "\\recorder_stop") {
            std::lock_guard lck(recorderMtx);

            // If not controlling the recorder, return
            if (!recordingEnabled) {
                resp = "RPRT 0\n";
                client->write(resp.size(), (uint8_t*)resp.c_str());
                return;
            }

            // Send the command to the selected recorder
            if (recorderType == RECORDER_TYPE_METEOR_DEMODULATOR) {
                core::modComManager.callInterface(selectedRecorder, METEOR_DEMODULATOR_IFACE_CMD_STOP, NULL, NULL);
            }
            else {
                core::modComManager.callInterface(selectedRecorder, RECORDER_IFACE_CMD_STOP, NULL, NULL);
            }

            // Respond with a success
            resp = "RPRT 0\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else if (parts[0] == "q" || parts[0] == "\\quit") {
            // Will close automatically
        }
        else if (parts[0] == "\\start") {
            gui::mainWindow.setPlayState(true);
        }
        else if (parts[0] == "\\stop") {
            gui::mainWindow.setPlayState(false);
        }
        else if (parts[0] == "\\dump_state") {
            std::lock_guard lck(vfoMtx);
            resp =
                /* rigctl protocol version */
                "0\n"
                /* rigctl model */
                "2\n"
                /* ITU region */
                "1\n"
                /* RX/TX frequency ranges
                * start, end, modes, low_power, high_power, vfo, ant
                *  start/end - Start/End frequency [Hz]
                *  modes - Bit field of RIG_MODE's (AM|AMS|CW|CWR|USB|LSB|FM|WFM)
                *  low_power/high_power - Lower/Higher RF power in mW,
                *                         -1 for no power (ie. rx list)
                *  vfo - VFO list equipped with this range (RIG_VFO_A)
                *  ant - Antenna list equipped with this range, 0 means all
                *  FIXME: get limits from receiver
                */
                "0.000000 10000000000.000000 0x2ef -1 -1 0x1 0x0\n"
                /* End of RX frequency ranges. */
                "0 0 0 0 0 0 0\n"
                /* End of TX frequency ranges. SDRIAK is receiver-only. */
                "0 0 0 0 0 0 0\n"
                /* Tuning steps: modes, tuning_step */
                "0xef 1\n"
                "0xef 0\n"
                /* End of tuning steps */
                "0 0\n"
                /* Filter sizes: modes, width
                * FIXME: get filter sizes from presets
                */
                "0x82 500\n"    /* CW | CWR normal */
                "0x82 200\n"    /* CW | CWR narrow */
                "0x82 2000\n"   /* CW | CWR wide */
                "0x221 10000\n" /* AM | AMS | FM normal */
                "0x221 5000\n"  /* AM | AMS | FM narrow */
                "0x221 20000\n" /* AM | AMS | FM wide */
                "0x0c 2700\n"   /* SSB normal */
                "0x0c 1400\n"   /* SSB narrow */
                "0x0c 3900\n"   /* SSB wide */
                "0x40 160000\n" /* WFM normal */
                "0x40 120000\n" /* WFM narrow */
                "0x40 200000\n" /* WFM wide */
                /* End of filter sizes  */
                "0 0\n"
                /* max_rit  */
                "0\n"
                /* max_xit */
                "0\n"
                /* max_ifshift */
                "0\n"
                /* Announces (bit field list) */
                "0\n" /* RIG_ANN_NONE */
                /* Preamp list in dB, 0 terminated */
                "0\n"
                /* Attenuator list in dB, 0 terminated */
                "0\n"
                /* Bit field list of get functions */
                "0\n" /* RIG_FUNC_NONE */
                /* Bit field list of set functions */
                "0\n" /* RIG_FUNC_NONE */
                /* Bit field list of get level */
                "0x40000020\n" /* RIG_LEVEL_SQL | RIG_LEVEL_STRENGTH */
                /* Bit field list of set level */
                "0x20\n" /* RIG_LEVEL_SQL */
                /* Bit field list of get parm */
                "0\n" /* RIG_PARM_NONE */
                /* Bit field list of set parm */
                "0\n" /* RIG_PARM_NONE */;
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        // This get_powerstat stuff is a wordaround for WSJT-X 2.7.0
        else if (parts[0] == "\\get_powerstat") {
            resp = "1\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
        }
        else {
            // If command is not recognized, return error
            flog::error("Rigctl client sent invalid command: '{0}'", cmd);
            resp = "RPRT 1\n";
            client->write(resp.size(), (uint8_t*)resp.c_str());
            return;
        }
    }

    std::string name;
    bool enabled = true;

    char hostname[1024];
    int port = 4532;
    net::Listener listener;
    std::shared_ptr<ClientSession> session;
    std::mutex sessionMtx;
    bool stopping = true;

    EventHandler<std::string> modChangedHandler;
    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
    EventHandler<std::string> vfoDeletedHandler;

    std::vector<std::string> vfoNames;
    std::string vfoNamesTxt;
    std::vector<std::string> recorderNames;
    std::string recorderNamesTxt;
    std::mutex vfoMtx;
    std::mutex recorderMtx;

    std::string selectedVfo = "";
    std::string selectedRecorder = "";
    int vfoId = 0;
    int recorderId = 0;
    int recorderType = RECORDER_TYPE_RECORDER;

    bool tuningEnabled = true;
    bool recordingEnabled = false;
    bool autoStart = false;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rigctl_server_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SigctlServerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SigctlServerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
