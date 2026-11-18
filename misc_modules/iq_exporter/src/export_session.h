#pragma once
// Packetizer and socket of one IQ export client, kept separate from the
// module so it can be tested without the GUI and signal path.
#include <utils/net.h>
#include <dsp/types.h>
#include <dsp/buffer/buffer.h>
#include <volk/volk.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

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

namespace iq_exporter {
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

        void consume(dsp::complex_t* data, int count) {
            // Only close() competes for mtx, and it retires the session, so a
            // block skipped here never leaves a gap inside a live packet.
            std::unique_lock lck(mtx, std::try_to_lock);
            if (!lck.owns_lock() || !isOpen()) { return; }

            if (protocol != PROTOCOL_UDP) {
                probePeerLocked();
                if (!isOpen()) { return; }
            }

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

        // IQ export peers normally send nothing; a zero-byte TCP read is the
        // only way to notice an orderly peer shutdown before a send fails.
        // Done here, on the DSP thread under mtx, rather than by the GUI each
        // frame, so the status display never contends with packet output.
        void probePeerLocked() {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextProbe) { return; }
            nextProbe = now + std::chrono::milliseconds(100);
            uint8_t dummy;
            socket->recv(&dummy, 1, false, net::NONBLOCKING);
            if (!socket->isOpen()) { active = false; }
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
        std::chrono::steady_clock::time_point nextProbe{};
        std::atomic<bool> active{true};
        std::mutex mtx;
    };
}
