#pragma once
#include <utils/net.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <queue>
#include <server_protocol.h>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <dsp/buffer/prebuffer.h>
#include <dsp/compression/sample_stream_decompressor.h>
#include <dsp/sink.h>
#include <zstd.h>
#include <chrono>

#define PROTOCOL_TIMEOUT_MS             10000

namespace server {
    using AuthKey = std::array<uint8_t, SERVER_AUTH_RESPONSE_SIZE>;

    void deriveAuthKey(const std::string& password, AuthKey& authKey);

    // One-shot ACK rendezvous between the GUI thread and the network worker.
    // The worker copies the ACK payload into the waiter, so the waiter stays
    // valid on its own: notify() never blocks and the worker never has to
    // pause to keep its receive buffer intact for the awaiting thread.
    class PacketWaiter {
    public:
        // Returns true if the ACK arrived, false on timeout or cancel.
        bool await(int timeout, const std::atomic<bool>* cancellation = nullptr) {
            std::unique_lock lck(mtx);
            if (!cancellation) {
                // No token to poll (every post-handshake call): plain wait,
                // woken directly by notify()/cancel().
                cnd.wait_for(lck, std::chrono::milliseconds(timeout), [this]() { return dataReady || canceled; });
                return dataReady && !canceled;
            }
            // The token is a bare atomic with no way to wake this condition
            // variable, so its arrival can only be observed by polling.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            while (!dataReady && !canceled) {
                if (cancellation && cancellation->load()) { return false; }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) { return false; }
                const auto pollDeadline = std::min(deadline, now + std::chrono::milliseconds(50));
                cnd.wait_until(lck, pollDeadline, [this]() { return dataReady || canceled; });
            }
            return dataReady && !canceled;
        }

        // Worker thread: copy the ACK payload into the waiter and wake the
        // awaiting thread. Never blocks, so it is safe to call while holding
        // the owner's waiter-map mutex.
        void notify(const uint8_t* data, size_t size) {
            {
                std::lock_guard lck(mtx);
                payload.assign(data, data + size);
                dataReady = true;
            }
            cnd.notify_all();
        }

        void cancel() {
            {
                std::lock_guard lck(mtx);
                canceled = true;
            }
            cnd.notify_all();
        }

        // Only valid after await() returned true. Owned by the waiter, so it
        // survives whatever the worker receives next.
        const std::vector<uint8_t>& data() const { return payload; }

    private:
        bool dataReady = false;
        bool canceled = false;
        std::vector<uint8_t> payload;

        std::condition_variable cnd;
        std::mutex mtx;
    };

    enum ConnectionError {
        CONN_ERR_TIMEOUT    = -1,
        CONN_ERR_BUSY       = -2,
        CONN_ERR_PROTOCOL   = -3,
        CONN_ERR_AUTH       = -4,
        CONN_ERR_CANCELLED  = -5
    };

    class Client {
    public:
        struct ZstdDCtxDeleter {
            void operator()(ZSTD_DCtx* ctx) const {
                if (ctx) { ZSTD_freeDCtx(ctx); }
            }
        };

        Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const std::string& password, const std::atomic<bool>* cancellation = nullptr);
        Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const AuthKey& authKey, const std::atomic<bool>* cancellation = nullptr);
        ~Client();

        void showMenu();

        void setFrequency(double freq);
        double getSampleRate();

        void setSampleType(dsp::compression::PCMType type);
        void setCompression(bool enabled);
        void setRxPrebufferMsec(int msec);
        int getRxPrebufferPercent();

        void start();
        void stop();

        // GUI thread only. Apply the server-pushed state (samplerate, tuning
        // limits) cached by the network worker. With force, re-apply even if
        // nothing new arrived — used on re-selection, when another local
        // source may have overwritten the samplerate or limits.
        void syncRemoteState(bool force = false);

        void close();
        bool isOpen();

        std::atomic<int> bytes{0};
        std::atomic<bool> serverBusy{false};
        std::atomic<bool> protocolReady{false};
        // Server advertised SERVER_PROTOCOL_CAP_HEARTBEAT in its hello ACK.
        // Once the handshake is done, such a server sends traffic at least
        // every heartbeat interval, so the worker may treat a long silence
        // as a dead link (issue #1462).
        std::atomic<bool> serverHeartbeat{false};

    private:
        void init(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out, const AuthKey* authKey, const std::atomic<bool>* cancellation);
        void worker();
        void applyRxPrebufferModeLocked(bool resetBuffer);

        int hello(const AuthKey* authKey, const std::atomic<bool>* cancellation);
        int authenticate(const AuthKey& authKey, const std::atomic<bool>* cancellation);
        int getUI(const std::atomic<bool>* cancellation = nullptr);

        void sendPacketLocked(PacketType type, int len);
        void sendCommandLocked(Command cmd, int len);
        void sendCommandAckLocked(Command cmd, int len);
        void sendCommand(Command cmd, int len);
        void sendCommandAck(Command cmd, int len);

        std::shared_ptr<PacketWaiter> awaitCommandAck(Command cmd);
        void cancelAllWaiters();
        // Remove a waiter that will never be completed (e.g. after a timeout),
        // so a late ACK can't be delivered to a later same-command waiter.
        void forgetCommandAck(const std::shared_ptr<PacketWaiter>& waiter);

        // Guards commandAckWaiters. Held by the worker across notify(), which
        // is safe because notify() never blocks (the payload is copied into
        // the waiter instead of being read from the worker's buffer).
        std::mutex waitersMtx;
        std::map<std::shared_ptr<PacketWaiter>, Command> commandAckWaiters;

        std::mutex authMtx;
        std::condition_variable authCnd;
        std::array<uint8_t, SERVER_AUTH_CHALLENGE_SIZE> authChallenge{};
        bool authChallengeReady = false;
        std::atomic<bool> authFailed{false};

        static void dHandler(dsp::complex_t *data, int count, void *ctx);

        std::shared_ptr<net::Socket> sock;

        dsp::stream<uint8_t> decompIn;
        dsp::compression::SampleStreamDecompressor decomp;
        dsp::buffer::PrebufferedLink<dsp::complex_t> chain;
        dsp::stream<dsp::complex_t>* output;

        std::unique_ptr<uint8_t[]> rbuffer;
        std::unique_ptr<uint8_t[]> sbuffer;

        PacketHeader* r_pkt_hdr = NULL;
        uint8_t* r_pkt_data = NULL;
        CommandHeader* r_cmd_hdr = NULL;
        uint8_t* r_cmd_data = NULL;

        PacketHeader* s_pkt_hdr = NULL;
        uint8_t* s_pkt_data = NULL;
        CommandHeader* s_cmd_hdr = NULL;
        uint8_t* s_cmd_data = NULL;
        std::mutex sendMtx;

        SmGui::DrawList dl;
        std::mutex dlMtx;

        std::unique_ptr<ZSTD_DCtx, ZstdDCtxDeleter> dctx;

        std::thread workerThread;

        // Server-pushed state: the network worker only caches it here; the
        // GUI thread applies it via syncRemoteState().
        std::mutex pushedStateMtx;
        // Upper bounds on server-pushed rate and user prebuffer duration.
        // Their product bounds the prebuffer ring (about 1.1 x 64 M complex
        // samples, ~560 MB, at both limits). 64 MS/s at 8-bit IQ already
        // saturates a gigabit link.
        static constexpr double MAX_SERVER_SAMPLERATE = 64e6;
        static constexpr int MAX_RX_PREBUFFER_MSEC = 1000;
        double currentSampleRate = 1000000.0;
        int rxPrebufferMsec = 100;
        // Remote tuning limits (native domain), last received from the server.
        bool remoteTuningLimitsEnabled = false;
        double remoteTuningLimitMin = 0.0;
        double remoteTuningLimitMax = 0.0;
        std::atomic<bool> samplerateDirty{false};
        std::atomic<bool> tuningLimitsDirty{false};
    };

    // timeoutMS bounds DNS resolution and the TCP connect (NO_TIMEOUT for the
    // OS default); cancellation, when given, lets the protocol handshake be
    // aborted early (see AsyncConnector::cancelToken()).
    std::shared_ptr<Client> connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out, const std::string& password = "", int timeoutMS = net::NO_TIMEOUT, const std::atomic<bool>* cancellation = nullptr);
    std::shared_ptr<Client> connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out, const AuthKey& authKey, int timeoutMS = net::NO_TIMEOUT, const std::atomic<bool>* cancellation = nullptr);
}
