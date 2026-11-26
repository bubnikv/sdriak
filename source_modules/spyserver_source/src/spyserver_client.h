#pragma once
#include <utils/networking.h>
#include <spyserver_protocol.h>
#include <dsp/stream.h>
#include <dsp/buffer/prebuffer.h>
#include <dsp/types.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace spyserver {
    // Protocol facts behind the FFT, PING and CLIENT_SYNC handling were
    // measured on public servers: doc/research/spyserver-streams.md. The
    // session order and the IQ meter follow jp's spyserver_vfo_source
    // (SDRPlusPlus-jp); see doc/research/forks/jp.md section 4.
    class SpyServerClientClass {
    public:
        using Clock = std::chrono::steady_clock;

        // One FFT line (UINT8_FFT), already in dB. The frame carries no
        // geometry: the caller tags it with the centre and stage it has
        // committed. Valid only during the callback.
        struct FftFrame {
            const float* bins = nullptr;
            int count = 0;
            Clock::time_point arrival;
            // IQ samples published before this frame arrived.
            uint64_t iqIndex = 0;
        };

        // Raw IQ levels since the last readIqMeter(): the largest
        // |sample| relative to full scale, and how many sample values sat on
        // a rail (0/255 for UInt8, +-32767 for Int16).
        struct IqMeter {
            float peak = 0.0f;
            uint64_t railed = 0;
            uint64_t values = 0;
        };

        SpyServerClientClass(net::Conn conn, dsp::stream<dsp::complex_t>* out, bool publishIq = true);
        ~SpyServerClientClass();

        bool waitForDevInfo(int timeoutMS);

        void startStream();
        void stopStream();

        // Any thread: controls are sent in order by the connection writer.
        void setSetting(uint32_t setting, uint32_t arg);

        // Any thread. Sends PING; onPong runs on the network thread with
        // answered = true and the stream position of the PONG (the index of
        // the first IQ sample published after it), or with answered = false
        // once kPongTimeout passed without one. PONGs carry no id: each
        // answers the oldest outstanding PING. An IQ setting sent just
        // before takes effect at that position.
        void ping(std::function<void(bool answered, uint64_t iqIndex)> onPong);
        static constexpr std::chrono::milliseconds kPongTimeout{1000};

        void close();
        bool isOpen();

        int computeDigitalGain(int serverBits, int deviceGain, int decimationId);

        // The last CLIENT_SYNC; false until one arrived. The server sends it
        // after FFT and decimation changes, not after IQ_FREQUENCY, and its
        // IQ/FFT centre fields can be stale (DeviceCenterFrequency is not).
        bool clientSync(SpyServerClientSync& out) const;
        // False once a CLIENT_SYNC said so; a second client joining takes
        // control from the first as well. True until the first sync.
        bool canControl() const { return m_canControl.load(); }
        // CLIENT_SYNC messages received so far. One follows each FFT or
        // decimation change at about the time it takes effect, so a count
        // past the one at a send confirms that send.
        uint64_t syncCount() const { return m_syncCount.load(); }

        // dB scale requested with FFT_DB_OFFSET / FFT_DB_RANGE, used to
        // convert UInt8 bins: dB = -offset - range * (1 - b/255). Set it
        // with the settings.
        void setFftScale(int dbOffset, int dbRange) {
            m_fftDbOffset = dbOffset;
            m_fftDbRange = dbRange;
        }
        // Network thread: every UInt8 FFT frame.
        std::function<void(const FftFrame&)> onFft;

        IqMeter readIqMeter();

        // IQ samples published to the output stream so far.
        uint64_t samplesPublished() const { return m_samplesPublished.load(); }
        void scheduleMarker(uint64_t sample, std::function<void()> callback) {
            m_position.schedule(sample, std::move(callback));
        }
        // Samples in the last IQ message: the block size downstream sees.
        int lastMessageSamples() const { return m_lastMessageSamples.load(); }

        SpyServerDeviceInfo devInfo;

    private:
        bool sendCommand(uint32_t command, const void* data, int len);
        void sendHandshake(std::string appName);
        struct Control {
            uint32_t setting = 0;
            uint32_t value = 0;
            std::function<void(bool, uint64_t)> onPong;
        };
        void enqueueControl(Control control);
        void controlLoop();

        int readSize(int count, uint8_t* buffer);

        static void dataHandler(int count, uint8_t* buf, void* ctx);

        void handleFft(const uint8_t* data, int count);
        void handlePong();
        void expirePings();
        void meterUInt8(const uint8_t* data, size_t values);
        void meterInt16(const int16_t* data, size_t values);

        net::Conn client;

        uint8_t* readBuf;
        std::vector<uint8_t> writeBuf;
        std::mutex m_controlMtx;
        std::condition_variable m_controlCv;
        std::deque<Control> m_controls;
        bool m_stopControl = false;
        std::atomic<bool> m_controlFailed{false};
        std::thread m_controlThread;

        bool deviceInfoAvailable = false;
        std::mutex deviceInfoMtx;
        std::condition_variable deviceInfoCnd;

        mutable std::mutex m_syncMtx;
        bool m_syncValid = false;
        SpyServerClientSync m_sync{};
        std::atomic<bool> m_canControl{true};
        std::atomic<uint64_t> m_syncCount{0};

        std::atomic<int> m_fftDbOffset{0};
        std::atomic<int> m_fftDbRange{127};
        std::vector<float> m_fftDb;   // network thread
        bool m_dint4Logged = false;   // network thread

        struct PendingPing {
            Clock::time_point sentAt;
            std::function<void(bool, uint64_t)> onPong;
        };
        std::mutex m_pingMtx;
        std::deque<PendingPing> m_pings;

        std::mutex m_meterMtx;
        IqMeter m_meter;

        std::atomic<uint64_t> m_samplesPublished{0};
        std::atomic<int> m_lastMessageSamples{0};
        std::mutex m_publishMtx;
        bool m_acceptIq = true;
        dsp::buffer::StreamPosition m_position;

        SpyServerMessageHeader receivedHeader;

        dsp::stream<dsp::complex_t>* output;
    };

    typedef std::unique_ptr<SpyServerClientClass> SpyServerClient;

    // timeoutMS bounds DNS resolution and the TCP connect; negative for the
    // OS default (tens of seconds — avoid on the GUI thread).
    // viaProxy: through the Network proxy with CONNECT (net::proxy::connect).
    SpyServerClient connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out,
                            int timeoutMS = -1, bool viaProxy = false, bool publishIq = true);

}
