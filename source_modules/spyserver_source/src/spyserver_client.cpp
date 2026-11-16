#include <spyserver_client.h>
#include <volk/volk.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <utils/flog.h>

using namespace std::chrono_literals;

namespace spyserver {
    SpyServerClientClass::SpyServerClientClass(net::Conn conn, dsp::stream<dsp::complex_t>* out) {
        readBuf = new uint8_t[SPYSERVER_MAX_MESSAGE_BODY_SIZE];
        writeBuf = new uint8_t[SPYSERVER_MAX_MESSAGE_BODY_SIZE];
        client = std::move(conn);
        output = out;

        output->clearWriteStop();

        sendHandshake("SDRIAK");

        client->readAsync(sizeof(SpyServerMessageHeader), (uint8_t*)&receivedHeader, dataHandler, this);
    }

    SpyServerClientClass::~SpyServerClientClass() {
        close();
        delete[] readBuf;
        delete[] writeBuf;
    }

    void SpyServerClientClass::startStream() {
        output->clearWriteStop();
        setSetting(SPYSERVER_SETTING_STREAMING_ENABLED, true);
    }

    void SpyServerClientClass::stopStream() {
        output->stopWriter();
        setSetting(SPYSERVER_SETTING_STREAMING_ENABLED, false);
    }

    void SpyServerClientClass::close() {
        output->stopWriter();
        client->close();
    }

    bool SpyServerClientClass::isOpen() {
        return client->isOpen();
    }

    int SpyServerClientClass::computeDigitalGain(int serverBits, int deviceGain, int decimationId) {
        if (devInfo.DeviceType == SPYSERVER_DEVICE_AIRSPY_ONE) {
            return (devInfo.MaximumGainIndex - deviceGain) + (decimationId * 3.01f);
        }
        else if (devInfo.DeviceType == SPYSERVER_DEVICE_AIRSPY_HF) {
            return decimationId * 3.01f;
        }
        else if (devInfo.DeviceType == SPYSERVER_DEVICE_RTLSDR) {
            return decimationId * 3.01f;
        }
        else {
            // Error, unknown device
            return -1;
        }
    }

    bool SpyServerClientClass::waitForDevInfo(int timeoutMS) {
        std::unique_lock lck(deviceInfoMtx);
        auto now = std::chrono::system_clock::now();
        deviceInfoCnd.wait_until(lck, now + (timeoutMS * 1ms), [this]() { return deviceInfoAvailable; });
        return deviceInfoAvailable;
    }

    void SpyServerClientClass::sendCommand(uint32_t command, void* data, int len) {
        SpyServerCommandHeader* hdr = (SpyServerCommandHeader*)writeBuf;
        hdr->CommandType = command;
        hdr->BodySize = len;
        memcpy(&writeBuf[sizeof(SpyServerCommandHeader)], data, len);
        client->write(sizeof(SpyServerCommandHeader) + len, writeBuf);
    }

    void SpyServerClientClass::sendHandshake(std::string appName) {
        int totSize = sizeof(SpyServerClientHandshake) + appName.size();
        uint8_t* buf = new uint8_t[totSize];

        SpyServerClientHandshake* cmdHandshake = (SpyServerClientHandshake*)buf;
        cmdHandshake->ProtocolVersion = SPYSERVER_PROTOCOL_VERSION;

        memcpy(&buf[sizeof(SpyServerClientHandshake)], appName.c_str(), appName.size());
        sendCommand(SPYSERVER_CMD_HELLO, buf, totSize);

        delete[] buf;
    }

    void SpyServerClientClass::setSetting(uint32_t setting, uint32_t arg) {
        SpyServerSettingTarget target;
        target.Setting = setting;
        target.Value = arg;
        sendCommand(SPYSERVER_CMD_SET_SETTING, &target, sizeof(SpyServerSettingTarget));
    }

    int SpyServerClientClass::readSize(int count, uint8_t* buffer) {
        int read = 0;
        int len = 0;
        while (read < count) {
            len = client->read(count - read, &buffer[read]);
            if (len <= 0) { return len; }
            read += len;
        }
        return read;
    }

    void SpyServerClientClass::dataHandler(int count, uint8_t* buf, void* ctx) {
        SpyServerClientClass* _this = (SpyServerClientClass*)ctx;

        if (count < sizeof(SpyServerMessageHeader)) {
            _this->readSize(sizeof(SpyServerMessageHeader) - count, &buf[count]);
        }

        // BodySize comes straight off the wire and readBuf is a fixed
        // SPYSERVER_MAX_MESSAGE_BODY_SIZE allocation. Anything larger is a
        // broken or hostile server: reading it would overrun the heap, and
        // skipping it is not possible without trusting the same field.
        const uint32_t bodySize = _this->receivedHeader.BodySize;
        if (bodySize > SPYSERVER_MAX_MESSAGE_BODY_SIZE) {
            flog::error("SpyServer: message body of {0} bytes exceeds the {1}-byte protocol limit, disconnecting",
                        bodySize, (uint32_t)SPYSERVER_MAX_MESSAGE_BODY_SIZE);
            _this->output->stopWriter();
            _this->client->close();
            return;
        }

        // An empty body is legal. readSize(0) returns 0, which must not be
        // mistaken for a disconnect.
        if (bodySize > 0) {
            int size = _this->readSize((int)bodySize, _this->readBuf);
            if (size <= 0) {
                flog::error("SpyServer: disconnected");
                return;
            }
        }

        //printf("MSG Proto: 0x%08X, MsgType: 0x%08X, StreamType: 0x%08X, Seq: 0x%08X, Size: %d\n", _this->receivedHeader.ProtocolID, _this->receivedHeader.MessageType, _this->receivedHeader.StreamType, _this->receivedHeader.SequenceNumber, _this->receivedHeader.BodySize);

        int mtype = _this->receivedHeader.MessageType & 0xFFFF;
        int mflags = (_this->receivedHeader.MessageType & 0xFFFF0000) >> 16;

        // Publish sampCount samples in pieces no larger than the output stream
        // holds. A full 1 MiB 8-bit message is 524288 samples, more than a
        // stream on 32-bit Android (and twice what one did at 262144 anywhere).
        // convert(offset, n, dst) writes samples [offset, offset + n).
        auto publish = [_this](int sampCount, auto convert) {
            dsp::stream<dsp::complex_t>* out = _this->output;
            for (int done = 0; done < sampCount;) {
                const int n = (std::min)(sampCount - done, out->capacity());
                convert(done, n, out->writeBuf);
                if (!out->swap(n)) { return; }
                done += n;
            }
        };

        if (mtype == SPYSERVER_MSG_TYPE_DEVICE_INFO) {
            {
                // A short body leaves the remaining fields zero instead of
                // reading whatever an earlier message left in readBuf.
                SpyServerDeviceInfo info{};
                memcpy(&info, _this->readBuf, (std::min)((size_t)bodySize, sizeof(info)));
                std::lock_guard lck(_this->deviceInfoMtx);
                _this->devInfo = info;
                _this->deviceInfoAvailable = true;
            }
            _this->deviceInfoCnd.notify_all();
        }
        else if (mtype == SPYSERVER_MSG_TYPE_UINT8_IQ) {
            const int sampCount = (int)(bodySize / (sizeof(uint8_t) * 2));
            const float gain = pow(10, (double)mflags / 20.0);
            const float scale = 1.0f / (gain * 128.0f);
            const uint8_t* src = _this->readBuf;
            publish(sampCount, [src, scale](int offset, int n, dsp::complex_t* dst) {
                const uint8_t* in = &src[2 * offset];
                for (int i = 0; i < n; i++) {
                    dst[i].re = ((float)in[2 * i] - 128.0f) * scale;
                    dst[i].im = ((float)in[(2 * i) + 1] - 128.0f) * scale;
                }
            });
        }
        else if (mtype == SPYSERVER_MSG_TYPE_INT16_IQ) {
            const int sampCount = (int)(bodySize / (sizeof(int16_t) * 2));
            const float gain = pow(10, (double)mflags / 20.0);
            const int16_t* src = (const int16_t*)_this->readBuf;
            publish(sampCount, [src, gain](int offset, int n, dsp::complex_t* dst) {
                volk_16i_s32f_convert_32f((float*)dst, &src[2 * offset], 32768.0 * gain, n * 2);
            });
        }
        else if (mtype == SPYSERVER_MSG_TYPE_INT24_IQ) {
            flog::error("SpyServer: 24-bit IQ format not supported");
            return;
        }
        else if (mtype == SPYSERVER_MSG_TYPE_FLOAT_IQ) {
            const int sampCount = (int)(bodySize / sizeof(dsp::complex_t));
            const float gain = pow(10, (double)mflags / 20.0);
            const float* src = (const float*)_this->readBuf;
            publish(sampCount, [src, gain](int offset, int n, dsp::complex_t* dst) {
                volk_32f_s32f_multiply_32f((float*)dst, &src[2 * offset], gain, n * 2);
            });
        }

        _this->client->readAsync(sizeof(SpyServerMessageHeader), (uint8_t*)&_this->receivedHeader, dataHandler, _this);
    }

    SpyServerClient connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out, int timeoutMS) {
        net::Conn conn = net::connect(host, port, timeoutMS);
        if (!conn) {
            return NULL;
        }
        return SpyServerClient(new SpyServerClientClass(std::move(conn), out));
    }
}
