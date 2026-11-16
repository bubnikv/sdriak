#include "sdriak_server_client.h"
#include <volk/volk.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <utils/flog.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <utils/proto/pbkdf2_sha256.h>

using namespace std::chrono_literals;

namespace {
    constexpr int SAMPLE_STREAM_HEADER_SIZE = 8;
    // Sized by the wire limit, not the local STREAM_BUFFER_SIZE, so a desktop
    // server's full-size blocks are accepted by a 32-bit Android client. The
    // decompressor splits each one to its output stream's capacity.
    constexpr int DECOMP_INPUT_BUFFER_SIZE = SERVER_PROTOCOL_MAX_BLOCK * sizeof(dsp::complex_t) + SAMPLE_STREAM_HEADER_SIZE;
    // Dead-link watchdog for heartbeat-capable servers, which send traffic at
    // least every 5 s. Mirrors the server's own HEARTBEAT_TIMEOUT.
    constexpr int LINK_TIMEOUT_MS = 15000;
}

namespace server {
    Client::Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const std::string& password, const std::atomic<bool>* cancellation) {
        AuthKey authKey{};
        AuthKey* authKeyPtr = nullptr;
        if (!password.empty()) {
            deriveAuthKey(password, authKey);
            authKeyPtr = &authKey;
        }

        try {
            init(sock, out, authKeyPtr, cancellation);
        }
        catch (...) {
            std::fill(authKey.begin(), authKey.end(), 0);
            throw;
        }
        std::fill(authKey.begin(), authKey.end(), 0);
    }

    Client::Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const AuthKey& authKey, const std::atomic<bool>* cancellation) {
        init(sock, out, &authKey, cancellation);
    }

    void deriveAuthKey(const std::string& password, AuthKey& authKey) {
        crypto::pbkdf2Sha256((const uint8_t*)password.data(), password.size(),
            (const uint8_t*)SERVER_AUTH_SALT, sizeof(SERVER_AUTH_SALT) - 1,
            SERVER_AUTH_PBKDF2_ITERATIONS, authKey.data(), authKey.size());
    }

    void Client::init(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const AuthKey* authKey, const std::atomic<bool>* cancellation) {
        this->sock = sock;
        output = out;
        if (cancellation && cancellation->load()) {
            throw std::runtime_error("Connection cancelled");
        }

        // Allocate buffers
        rbuffer = std::make_unique<uint8_t[]>(SERVER_MAX_PACKET_SIZE);
        sbuffer = std::make_unique<uint8_t[]>(SERVER_MAX_PACKET_SIZE);

        // Initialize headers
        r_pkt_hdr = reinterpret_cast<PacketHeader*>(rbuffer.get());
        r_pkt_data = rbuffer.get() + sizeof(PacketHeader);
        r_cmd_hdr = (CommandHeader*)r_pkt_data;
        r_cmd_data = rbuffer.get() + sizeof(PacketHeader) + sizeof(CommandHeader);

        s_pkt_hdr = reinterpret_cast<PacketHeader*>(sbuffer.get());
        s_pkt_data = sbuffer.get() + sizeof(PacketHeader);
        s_cmd_hdr = (CommandHeader*)s_pkt_data;
        s_cmd_data = sbuffer.get() + sizeof(PacketHeader) + sizeof(CommandHeader);

        // Initialize decompressor
        dctx.reset(ZSTD_createDCtx());
        if (!dctx) {
            throw std::runtime_error("Failed to create ZSTD decompressor context");
        }

        // Initialize DSP
        decompIn.setBufferSize(DECOMP_INPUT_BUFFER_SIZE);
        decompIn.clearWriteStop();
        decomp.init(&decompIn);
        chain.init(&decomp.out, output);
        chain.setPrebufferMsec(rxPrebufferMsec, false, false);
        decomp.start();
        chain.start();

        // Start worker thread
        workerThread = std::thread(&Client::worker, this);

        int res = hello(authKey, cancellation);
        if (res == 0) { res = getUI(cancellation); }
        if (res < 0) {
            // Close client
            close();

            // Throw error
            switch (res) {
            case CONN_ERR_TIMEOUT:
                throw std::runtime_error("Timed out");
            case CONN_ERR_BUSY:
                throw std::runtime_error("Server busy");
            case CONN_ERR_PROTOCOL:
                throw std::runtime_error("Incompatible SDRIAK server protocol");
            case CONN_ERR_AUTH:
                throw std::runtime_error("Authentication failed");
            case CONN_ERR_CANCELLED:
                throw std::runtime_error("Connection cancelled");
            default:
                throw std::runtime_error("Unknown error");
            }
        }
    }

    Client::~Client() {
        close();
    }

    void Client::showMenu() {
        std::string diffId = "";
        SmGui::DrawListElem diffValue;
        bool syncRequired = false;
        {
            std::lock_guard<std::mutex> lck(dlMtx);
            dl.draw(diffId, diffValue, syncRequired);
        }

        if (!diffId.empty()) {
            // Save ID
            SmGui::DrawListElem elemId;
            elemId.type = SmGui::DRAW_LIST_ELEM_TYPE_STRING;
            elemId.str = diffId;

            auto sendUIAction = [&]() {
                std::lock_guard lck(sendMtx);
                int size = 0;
                s_cmd_data[size++] = syncRequired;
                size += SmGui::DrawList::storeItem(elemId, &s_cmd_data[size], SERVER_MAX_PACKET_SIZE - size);
                size += SmGui::DrawList::storeItem(diffValue, &s_cmd_data[size], SERVER_MAX_PACKET_SIZE - size);
                sendCommandLocked(COMMAND_UI_ACTION, size);
            };

            if (syncRequired) {
                flog::warn("Action requires resync");
                auto waiter = awaitCommandAck(COMMAND_UI_ACTION);
                sendUIAction();
                if (waiter->await(PROTOCOL_TIMEOUT_MS)) {
                    const auto& payload = waiter->data();
                    std::lock_guard lck(dlMtx);
                    dl.load((void*)payload.data(), (int)payload.size());
                }
                else {
                    // Drop the abandoned waiter, otherwise a late ACK for this
                    // request would be delivered to a later same-command waiter.
                    forgetCommandAck(waiter);
                    flog::error("Timeout out after asking for UI");
                }
                flog::warn("Resync done");
            }
            else {
                flog::warn("Action does not require resync");
                sendUIAction();
            }
        }
    }

    void Client::syncRemoteState(bool force) {
        // Consume the dirty flags before snapshotting, so an update the
        // worker publishes in between is not lost: it sets the flag again
        // and the next frame picks it up.
        bool applySamplerate = samplerateDirty.exchange(false) || force;
        bool applyLimits = tuningLimitsDirty.exchange(false) || force;
        if (!applySamplerate && !applyLimits) { return; }

        double samplerate;
        bool limitsEnabled;
        double limitMin, limitMax;
        {
            std::lock_guard lck(pushedStateMtx);
            samplerate = currentSampleRate;
            limitsEnabled = remoteTuningLimitsEnabled;
            limitMin = remoteTuningLimitMin;
            limitMax = remoteTuningLimitMax;
        }

        // Apply outside the mutex: setInputSampleRate reconfigures the DSP
        // chain and setTuningLimits touches GUI state.
        if (applySamplerate) {
            core::setInputSampleRate(samplerate);
        }
        if (applyLimits) {
            if (limitsEnabled) {
                // Native domain; the local SourceManager shifts by the local
                // tuning offset before constraining gui::freqSelect.
                sigpath::sourceManager.setTuningLimits(limitMin, limitMax);
            }
            else {
                sigpath::sourceManager.clearTuningLimits();
            }
        }
    }

    void Client::setFrequency(double freq) {
        if (!isOpen()) { return; }
        // Register the waiter before sending, so an ACK arriving right away
        // cannot be missed.
        auto waiter = awaitCommandAck(COMMAND_SET_FREQUENCY);
        {
            std::lock_guard lck(sendMtx);
            // memcpy: s_cmd_data is not 8-byte aligned (header offsets).
            memcpy(s_cmd_data, &freq, sizeof(double));
            sendCommandLocked(COMMAND_SET_FREQUENCY, sizeof(double));
        }
        if (!waiter->await(PROTOCOL_TIMEOUT_MS)) { forgetCommandAck(waiter); }
    }

    double Client::getSampleRate() {
        std::lock_guard lck(pushedStateMtx);
        return currentSampleRate;
    }

    void Client::applyRxPrebufferModeLocked(bool resetBuffer) {
        chain.setSampleRate(currentSampleRate);
        // The chain runs from the constructor until close(), so the mode
        // switch is always live here.
        chain.setPrebufferMsec(rxPrebufferMsec, resetBuffer, true);
    }

    void Client::setSampleType(dsp::compression::PCMType type) {
        if (!isOpen()) { return; }
        std::lock_guard lck(sendMtx);
        s_cmd_data[0] = type;
        sendCommandLocked(COMMAND_SET_SAMPLE_TYPE, 1);
    }

    void Client::setCompression(bool enabled) {
        if (!isOpen()) { return; }
        std::lock_guard lck(sendMtx);
        s_cmd_data[0] = enabled;
        sendCommandLocked(COMMAND_SET_COMPRESSION, 1);
    }

    void Client::setRxPrebufferMsec(int msec) {
        std::lock_guard lck(pushedStateMtx);
        rxPrebufferMsec = std::clamp(msec, 0, MAX_RX_PREBUFFER_MSEC);
        applyRxPrebufferModeLocked(true);
    }

    int Client::getRxPrebufferPercent() {
        // -1 when the prebuffer is bypassed (no buffer to fill).
        return chain.getPercentFull();
    }

    void Client::start() {
        if (!isOpen()) { return; }
        {
            std::lock_guard lck(pushedStateMtx);
            applyRxPrebufferModeLocked(true);
        }
        sendCommand(COMMAND_START, 0);
        getUI();
    }

    void Client::stop() {
        if (!isOpen()) { return; }
        sendCommand(COMMAND_STOP, 0);
        getUI();
    }

    void Client::close() {
        // Stop worker
        decompIn.stopWriter();
        if (sock) { sock->close(); }
        if (workerThread.joinable()) { workerThread.join(); }
        decompIn.clearWriteStop();

        // Stop DSP
        chain.stop();
        decomp.stop();
    }

    bool Client::isOpen() {
        return sock && sock->isOpen();
    }

    void Client::worker() {
        while (true) {
            // A handshaken heartbeat-capable server sends traffic at least
            // every heartbeat interval, so bound the wait and treat a long
            // silence as a dead link (silent network drop, issue #1462).
            // Other servers are legitimately silent while stopped: wait
            // forever and rely on close() waking the recv via shutdown().
            bool watchdog = protocolReady.load() && serverHeartbeat.load();

            // Receive header
            int rres = sock->recv(rbuffer.get(), sizeof(PacketHeader), true, watchdog ? LINK_TIMEOUT_MS : net::NO_TIMEOUT);
            if (rres <= 0) {
                // recv() only returns 0 with the socket still open on a
                // timeout; on close/error it closes the socket first.
                if (rres == 0 && watchdog && sock->isOpen()) {
                    flog::error("SDRIAK server link timed out (no traffic for {0} ms), disconnecting", LINK_TIMEOUT_MS);
                }
                break;
            }

            // Validate the server-supplied size before using it as a receive
            // length: rbuffer is a fixed SERVER_MAX_PACKET_SIZE allocation, so
            // an out-of-range size would overflow it (and a value below the
            // header size would wrap the unsigned subtraction below).
            if (r_pkt_hdr->size < sizeof(PacketHeader) || r_pkt_hdr->size > SERVER_MAX_PACKET_SIZE) {
                flog::error("Invalid packet size from server: {0}", r_pkt_hdr->size);
                break;
            }

            // Receive remaining data
            if (sock->recv(&rbuffer[sizeof(PacketHeader)], r_pkt_hdr->size - sizeof(PacketHeader), true, PROTOCOL_TIMEOUT_MS) <= 0) {
                break;
            }

            // Increment data counter
            bytes += r_pkt_hdr->size;
            const size_t payloadSize = (size_t)r_pkt_hdr->size - sizeof(PacketHeader);

            // Decode packet. Worker discipline: only parse and cache into
            // mutex-protected client state; SourceManager/GUI applies happen
            // on the GUI thread via syncRemoteState().
            if (r_pkt_hdr->type == PACKET_TYPE_COMMAND) {
                if (r_pkt_hdr->size < sizeof(PacketHeader) + sizeof(CommandHeader)) {
                    flog::error("Invalid command packet size from server: {0}", r_pkt_hdr->size);
                    break;
                }

                // TODO: Move to command handler
                if (r_cmd_hdr->cmd == COMMAND_SET_SAMPLERATE && r_pkt_hdr->size == sizeof(PacketHeader) + sizeof(CommandHeader) + sizeof(double)) {
                    // memcpy: r_cmd_data is not 8-byte aligned (header offsets).
                    double samplerate;
                    memcpy(&samplerate, r_cmd_data, sizeof(double));
                    // Don't trust the server: the prebuffer sizes its ring
                    // from samplerate x prebuffer duration, so NaN, infinity
                    // or an enormous rate would become an undefined
                    // float-to-size_t conversion or a huge allocation that
                    // throws out of this thread. With the duration capped at
                    // MAX_RX_PREBUFFER_MSEC, bounding the rate bounds the ring.
                    if (!std::isfinite(samplerate) || samplerate <= 0.0 || samplerate > MAX_SERVER_SAMPLERATE) {
                        flog::error("Ignoring invalid samplerate from server: {0}", samplerate);
                    }
                    else {
                        {
                            std::lock_guard lck(pushedStateMtx);
                            currentSampleRate = samplerate;
                            chain.setSampleRate(samplerate);
                        }
                        samplerateDirty = true;
                    }
                }
                else if (r_cmd_hdr->cmd == COMMAND_SET_TUNING_LIMITS && r_pkt_hdr->size == sizeof(PacketHeader) + sizeof(CommandHeader) + 1 + 2 * sizeof(double)) {
                    bool enabled = r_cmd_data[0];
                    double limitMin, limitMax;
                    memcpy(&limitMin, &r_cmd_data[1], sizeof(double));
                    memcpy(&limitMax, &r_cmd_data[1 + sizeof(double)], sizeof(double));
                    // Don't trust the server: non-finite or inverted limits
                    // would become garbage (uint64_t) casts in the frequency
                    // selector and could lock the user out of tuning.
                    if (enabled && (!std::isfinite(limitMin) || !std::isfinite(limitMax) || limitMin > limitMax)) {
                        flog::error("Ignoring invalid tuning limits from server: [{0}, {1}]", limitMin, limitMax);
                    }
                    else {
                        {
                            std::lock_guard lck(pushedStateMtx);
                            remoteTuningLimitsEnabled = enabled;
                            remoteTuningLimitMin = limitMin;
                            remoteTuningLimitMax = limitMax;
                        }
                        tuningLimitsDirty = true;
                    }
                }
                else if (r_cmd_hdr->cmd == COMMAND_DISCONNECT) {
                    flog::error("Asked to disconnect by the server");
                    serverBusy = true;
                    cancelAllWaiters();
                }
                else if (r_cmd_hdr->cmd == COMMAND_AUTH_CHALLENGE && r_pkt_hdr->size == sizeof(PacketHeader) + sizeof(CommandHeader) + SERVER_AUTH_CHALLENGE_SIZE) {
                    {
                        std::lock_guard lck(authMtx);
                        memcpy(authChallenge.data(), r_cmd_data, authChallenge.size());
                        authChallengeReady = true;
                    }
                    authCnd.notify_all();
                }
                else if (r_cmd_hdr->cmd == COMMAND_HEARTBEAT && r_pkt_hdr->size == sizeof(PacketHeader) + sizeof(CommandHeader) + sizeof(uint32_t)) {
                    std::lock_guard lck(sendMtx);
                    memcpy(s_cmd_data, r_cmd_data, sizeof(uint32_t));
                    sendCommandAckLocked(COMMAND_HEARTBEAT, sizeof(uint32_t));
                }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_COMMAND_ACK) {
                if (r_pkt_hdr->size < sizeof(PacketHeader) + sizeof(CommandHeader)) {
                    flog::error("Invalid command ACK packet size from server: {0}", r_pkt_hdr->size);
                    break;
                }

                // Copy the payload into the matching waiters and wake them.
                int payloadSize = (int)r_pkt_hdr->size - (int)(sizeof(PacketHeader) + sizeof(CommandHeader));
                if (payloadSize < 0) { payloadSize = 0; }
                std::lock_guard lck(waitersMtx);
                for (auto it = commandAckWaiters.begin(); it != commandAckWaiters.end();) {
                    if (it->second == r_cmd_hdr->cmd) {
                        it->first->notify(r_cmd_data, payloadSize);
                        it = commandAckWaiters.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_BASEBAND) {
                if (payloadSize < SAMPLE_STREAM_HEADER_SIZE || payloadSize > (size_t)DECOMP_INPUT_BUFFER_SIZE) {
                    flog::error("Invalid baseband packet size from server: {0}", payloadSize);
                    break;
                }
                memcpy(decompIn.writeBuf, r_pkt_data, payloadSize);
                if (!decompIn.swap((int)payloadSize)) { break; }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_BASEBAND_COMPRESSED) {
                size_t outCount = ZSTD_decompressDCtx(dctx.get(), decompIn.writeBuf, (size_t)DECOMP_INPUT_BUFFER_SIZE, r_pkt_data, payloadSize);
                if (ZSTD_isError(outCount)) {
                    flog::error("Failed to decompress server baseband packet: {0}", ZSTD_getErrorName(outCount));
                    break;
                }
                if (outCount > (size_t)DECOMP_INPUT_BUFFER_SIZE || (outCount != 0 && outCount < SAMPLE_STREAM_HEADER_SIZE)) {
                    flog::error("Invalid decompressed baseband size from server: {0}", outCount);
                    break;
                }
                if (outCount) {
                    if (!decompIn.swap((int)outCount)) { break; }
                }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_ERROR) {
                uint8_t err = (r_pkt_hdr->size > sizeof(PacketHeader)) ? r_pkt_data[0] : ERROR_INVALID_PACKET;
                flog::error("SDRIAK Server Error: {0}", err);
                if (err == ERROR_AUTH_REQUIRED || err == ERROR_AUTH_FAILED) {
                    authFailed = true;
                    authCnd.notify_all();
                    cancelAllWaiters();
                }
                if (!protocolReady.load() && (err == ERROR_PROTOCOL_MISMATCH || err == ERROR_INVALID_COMMAND)) {
                    cancelAllWaiters();
                }
            }
            else {
                flog::error("Invalid packet type: {0}", r_pkt_hdr->type);
            }
        }

        // Connection is gone. Close the socket so isOpen() reports it — the
        // GUI stops issuing commands into the dead session (each of which
        // would block for the full protocol timeout) and shows Disconnected —
        // then release anyone still waiting for an ACK.
        sock->close();
        cancelAllWaiters();
        authCnd.notify_all();
    }

    int Client::hello(const AuthKey* authKey, const std::atomic<bool>* cancellation) {
        if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
        if (!isOpen()) { return CONN_ERR_TIMEOUT; }

        auto waiter = awaitCommandAck(COMMAND_HELLO);
        {
            std::lock_guard lck(sendMtx);
            HelloPayload hello = makeHelloPayload(SERVER_PROTOCOL_CAP_HEARTBEAT | SERVER_PROTOCOL_CAP_AUTH);
            memcpy(s_cmd_data, &hello, sizeof(hello));
            sendCommandLocked(COMMAND_HELLO, sizeof(hello));
        }

        if (!waiter->await(PROTOCOL_TIMEOUT_MS, cancellation)) {
            forgetCommandAck(waiter);
            if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
            if (serverBusy) { return CONN_ERR_BUSY; }
            return CONN_ERR_PROTOCOL;
        }

        const auto& payload = waiter->data();
        if (payload.size() != sizeof(HelloPayload)) { return CONN_ERR_PROTOCOL; }

        HelloPayload hello;
        memcpy(&hello, payload.data(), sizeof(hello));
        if (!isCompatibleHello(hello)) { return CONN_ERR_PROTOCOL; }

        // The worker's dead-link watchdog only arms once protocolReady is
        // also set: the server doesn't heartbeat during the handshake.
        serverHeartbeat = (hello.capabilities & SERVER_PROTOCOL_CAP_HEARTBEAT) != 0;

        if ((hello.capabilities & SERVER_PROTOCOL_CAP_AUTH) != 0) {
            if (!authKey) { return CONN_ERR_AUTH; }
            int res = authenticate(*authKey, cancellation);
            if (res < 0) { return res; }
        }

        protocolReady = true;
        return 0;
    }

    int Client::authenticate(const AuthKey& authKey, const std::atomic<bool>* cancellation) {
        std::array<uint8_t, SERVER_AUTH_CHALLENGE_SIZE> challenge{};
        {
            std::unique_lock lck(authMtx);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(PROTOCOL_TIMEOUT_MS);
            while (!authChallengeReady && !authFailed.load() && isOpen()) {
                if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) { return CONN_ERR_TIMEOUT; }
                authCnd.wait_until(lck, std::min(deadline, now + std::chrono::milliseconds(50)));
            }
            if (authFailed.load() || !authChallengeReady || !isOpen()) { return CONN_ERR_AUTH; }
            challenge = authChallenge;
            authChallengeReady = false;
        }

        std::array<uint8_t, SERVER_AUTH_RESPONSE_SIZE> response{};
        crypto::hmacSha256(authKey.data(), authKey.size(), challenge.data(), challenge.size(), response.data());

        authFailed = false;
        auto waiter = awaitCommandAck(COMMAND_AUTH_RESPONSE);
        {
            std::lock_guard lck(sendMtx);
            memcpy(s_cmd_data, response.data(), response.size());
            sendCommandLocked(COMMAND_AUTH_RESPONSE, (int)response.size());
        }

        std::fill(response.begin(), response.end(), 0);

        if (!waiter->await(PROTOCOL_TIMEOUT_MS, cancellation)) {
            forgetCommandAck(waiter);
            if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
            // The server answers a valid response with COMMAND_DISCONNECT if
            // another client claimed the slot during the handshake.
            if (serverBusy.load()) { return CONN_ERR_BUSY; }
            return authFailed.load() ? CONN_ERR_AUTH : CONN_ERR_TIMEOUT;
        }

        return 0;
    }

    int Client::getUI(const std::atomic<bool>* cancellation) {
        if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
        if (!isOpen()) { return -1; }
        auto waiter = awaitCommandAck(COMMAND_GET_UI);
        sendCommand(COMMAND_GET_UI, 0);
        if (!waiter->await(PROTOCOL_TIMEOUT_MS, cancellation)) {
            // Drop the abandoned waiter, otherwise a late ACK for this request
            // would be delivered to a later same-command waiter.
            forgetCommandAck(waiter);
            if (cancellation && cancellation->load()) { return CONN_ERR_CANCELLED; }
            if (!serverBusy) { flog::error("Timeout out after asking for UI"); };
            return serverBusy ? CONN_ERR_BUSY : CONN_ERR_TIMEOUT;
        }
        const auto& payload = waiter->data();
        std::lock_guard lck(dlMtx);
        dl.load((void*)payload.data(), (int)payload.size());
        return 0;
    }

    void Client::sendPacketLocked(PacketType type, int len) {
        s_pkt_hdr->type = type;
        s_pkt_hdr->size = sizeof(PacketHeader) + len;
        sock->send(sbuffer.get(), s_pkt_hdr->size);
    }

    void Client::sendCommandLocked(Command cmd, int len) {
        s_cmd_hdr->cmd = cmd;
        sendPacketLocked(PACKET_TYPE_COMMAND, sizeof(CommandHeader) + len);
    }

    void Client::sendCommandAckLocked(Command cmd, int len) {
        s_cmd_hdr->cmd = cmd;
        sendPacketLocked(PACKET_TYPE_COMMAND_ACK, sizeof(CommandHeader) + len);
    }

    void Client::sendCommand(Command cmd, int len) {
        std::lock_guard lck(sendMtx);
        sendCommandLocked(cmd, len);
    }

    void Client::sendCommandAck(Command cmd, int len) {
        std::lock_guard lck(sendMtx);
        sendCommandAckLocked(cmd, len);
    }

    std::shared_ptr<PacketWaiter> Client::awaitCommandAck(Command cmd) {
        auto waiter = std::make_shared<PacketWaiter>();
        std::lock_guard lck(waitersMtx);
        commandAckWaiters[waiter] = cmd;
        return waiter;
    }

    void Client::cancelAllWaiters() {
        std::lock_guard lck(waitersMtx);
        for (auto& [waiter, cmd] : commandAckWaiters) {
            waiter->cancel();
        }
        commandAckWaiters.clear();
    }

    void Client::forgetCommandAck(const std::shared_ptr<PacketWaiter>& waiter) {
        std::lock_guard lck(waitersMtx);
        commandAckWaiters.erase(waiter);
    }

    void Client::dHandler(dsp::complex_t *data, int count, void *ctx) {
        Client* _this = (Client*)ctx;
        memcpy(_this->output->writeBuf, data, count * sizeof(dsp::complex_t));
        _this->output->swap(count);
    }

    std::shared_ptr<Client> connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out, const std::string& password, int timeoutMS, const std::atomic<bool>* cancellation) {
        return std::make_shared<Client>(net::connect(host, port, timeoutMS), out, password, cancellation);
    }

    std::shared_ptr<Client> connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out, const AuthKey& authKey, int timeoutMS, const std::atomic<bool>* cancellation) {
        return std::make_shared<Client>(net::connect(host, port, timeoutMS), out, authKey, cancellation);
    }
}
