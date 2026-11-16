#include <volk/volk.h>
#include <cstring>
#include <chrono>
#include <cassert>

#define ENET_IMPLEMENTATION
#include <enet/enet.h>

#include <qmxserver_client.h>

enum class CatCommandID : uint16_t {
    // Set local oscillator frequency in Hz.
    // int64_t frequency
    SetFreq,
    // Set the CW TX frequency in Hz.
    // int64_t frequency
    SetCWTxFreq,
    // Set the CW keyer speed in Words per Minute.
    // Limited to <5, 45>
    // uint8_t
    SetCWKeyerSpeed,
    // KeyerMode mode
    // uint8_t
    SetKeyerMode,
    // Delay of the dit sent after dit played, to avoid hot switching of the AMP relay, in microseconds. Maximum time is 15ms.
    // Relay hang after the last dit, in microseconds. Maximum time is 10 seconds.
    // bool enabled, uint32_t delay, uint32_t hang
    SetAMPControl,
    // CW phase & amplitude balance and output power.
    // double phase_balance_deg, double amplitude_balance, double power
    SetIQBalanceAndPower,
};

using namespace std::chrono_literals;

namespace qmxserver {

    QmxServerClientClass::QmxServerClientClass(ENetHost *client, ENetPeer* peer, ENetAddress address, dsp::stream<dsp::complex_t>* out) :
        m_client(client),
        m_peer(peer),
        m_address(address)
    {
        readBuf = new uint8_t[QMXSERVER_MAX_MESSAGE_BODY_SIZE];
        writeBuf = new uint8_t[QMXSERVER_MAX_MESSAGE_BODY_SIZE];
        // client = std::move(conn);
        output = out;

        output->clearWriteStop();

        //sendHandshake("SDRIAK");

        //client->readAsync(sizeof(QmxServerMessageHeader), (uint8_t*)&receivedHeader, dataHandler, this);
        m_thread = std::thread(&QmxServerClientClass::run, this);
    }

    QmxServerClientClass::~QmxServerClientClass() {
        close();
        enet_deinitialize();
        delete[] readBuf;
        delete[] writeBuf;
    }

    void QmxServerClientClass::startStream() {
        output->clearWriteStop();
        setSetting(QMXSERVER_SETTING_STREAMING_ENABLED, true);
    }

    void QmxServerClientClass::stopStream() {
        output->stopWriter();
        setSetting(QMXSERVER_SETTING_STREAMING_ENABLED, false);
    }

    void QmxServerClientClass::close() {
        output->stopWriter();
        if (m_thread.joinable()) {
            m_exit_thread = true;
            if (m_thread.joinable())
                m_thread.join();
            // Destroy packets, which were not sent to the server.
            std::lock_guard<std::mutex> lock(m_mutex);
            for (ENetPacket *packet : m_queue)
                enet_packet_destroy(packet);
            m_queue.clear();
            m_connected = false;
        }
    }

    bool QmxServerClientClass::isOpen() {
        return m_connected;
    }

    int QmxServerClientClass::computeDigitalGain(int serverBits, int deviceGain, int decimationId) {
#if 0
        if (devInfo.DeviceType == QMXSERVER_DEVICE_AIRQMX_ONE) {
            return (devInfo.MaximumGainIndex - deviceGain) + (decimationId * 3.01f);
        }
        else if (devInfo.DeviceType == QMXSERVER_DEVICE_AIRQMX_HF) {
            return decimationId * 3.01f;
        }
        else if (devInfo.DeviceType == QMXSERVER_DEVICE_RTLSDR) {
            return decimationId * 3.01f;
        }
        else {
            // Error, unknown device
            return -1;
        }
#endif
        return 1.f;
    }

    bool QmxServerClientClass::waitForDevInfo(int timeoutMS) {
#if 0
        std::unique_lock lck(deviceInfoMtx);
        auto now = std::chrono::system_clock::now();
        deviceInfoCnd.wait_until(lck, now + (timeoutMS * 1ms), [this]() { return deviceInfoAvailable; });
        return deviceInfoAvailable;
#endif
        return true;
    }

    
    void QmxServerClientClass::setSetting(uint32_t setting, uint32_t arg) {
        /*
        QmxServerSettingTarget target;
        target.Setting = setting;
        target.Value = arg;
        sendCommand(QMXSERVER_CMD_SET_SETTING, &target, sizeof(QmxServerSettingTarget));
        */ 
    }

#if 0
    void QmxServerClientClass::sendCommand(uint32_t command, void* data, int len) {
        QmxServerCommandHeader* hdr = (QmxServerCommandHeader*)writeBuf;
        hdr->CommandType = command;
        hdr->BodySize = len;
        memcpy(&writeBuf[sizeof(QmxServerCommandHeader)], data, len);
        //client->write(sizeof(QmxServerCommandHeader) + len, writeBuf);
    }

    void QmxServerClientClass::sendHandshake(std::string appName) {
        int totSize = sizeof(QmxServerClientHandshake) + appName.size();
        uint8_t* buf = new uint8_t[totSize];

        QmxServerClientHandshake* cmdHandshake = (QmxServerClientHandshake*)buf;
        cmdHandshake->ProtocolVersion = QMXSERVER_PROTOCOL_VERSION;

        memcpy(&buf[sizeof(QmxServerClientHandshake)], appName.c_str(), appName.size());
        sendCommand(QMXSERVER_CMD_HELLO, buf, totSize);

        delete[] buf;
    }

    int QmxServerClientClass::readSize(int count, uint8_t* buffer) {
        int read = 0;
        int len = 0;
        while (read < count) {
            //len = client->read(count - read, &buffer[read]);
            if (len <= 0) { return len; }
            read += len;
        }
        return read;
    }

    void QmxServerClientClass::dataHandler(int count, uint8_t* buf, void* ctx) {
        QmxServerClientClass* _this = (QmxServerClientClass*)ctx;

        if (count < sizeof(QmxServerMessageHeader)) {
            _this->readSize(sizeof(QmxServerMessageHeader) - count, &buf[count]);
        }

        int size = _this->readSize(_this->receivedHeader.BodySize, _this->readBuf);
        if (size <= 0) {
            printf("ERROR: Disconnected\n");
            return;
        }

        //printf("MSG Proto: 0x%08X, MsgType: 0x%08X, StreamType: 0x%08X, Seq: 0x%08X, Size: %d\n", _this->receivedHeader.ProtocolID, _this->receivedHeader.MessageType, _this->receivedHeader.StreamType, _this->receivedHeader.SequenceNumber, _this->receivedHeader.BodySize);

        int mtype = _this->receivedHeader.MessageType & 0xFFFF;
        int mflags = (_this->receivedHeader.MessageType & 0xFFFF0000) >> 16;

        if (mtype == QMXSERVER_MSG_TYPE_DEVICE_INFO) {
            {
                std::lock_guard lck(_this->deviceInfoMtx);
                QmxServerDeviceInfo* _devInfo = (QmxServerDeviceInfo*)_this->readBuf;
                _this->devInfo = *_devInfo;
                _this->deviceInfoAvailable = true;
            }
            _this->deviceInfoCnd.notify_all();
        }
        else if (mtype == QMXSERVER_MSG_TYPE_UINT8_IQ) {
            int sampCount = _this->receivedHeader.BodySize / (sizeof(uint8_t) * 2);
            float gain = pow(10, (double)mflags / 20.0);
            float scale = 1.0f / (gain * 128.0f);
            for (int i = 0; i < sampCount; i++) {
                _this->output->writeBuf[i].re = ((float)_this->readBuf[(2 * i)] - 128.0f) * scale;
                _this->output->writeBuf[i].im = ((float)_this->readBuf[(2 * i) + 1] - 128.0f) * scale;
            }
            _this->output->swap(sampCount);
        }
        else if (mtype == QMXSERVER_MSG_TYPE_INT16_IQ) {
            int sampCount = _this->receivedHeader.BodySize / (sizeof(int16_t) * 2);
            float gain = pow(10, (double)mflags / 20.0);
            volk_16i_s32f_convert_32f((float*)_this->output->writeBuf, (int16_t*)_this->readBuf, 32768.0 * gain, sampCount * 2);
            _this->output->swap(sampCount);
        }
        else if (mtype == QMXSERVER_MSG_TYPE_INT24_IQ) {
            printf("ERROR: IQ format not supported\n");
            return;
        }
        else if (mtype == QMXSERVER_MSG_TYPE_FLOAT_IQ) {
            int sampCount = _this->receivedHeader.BodySize / sizeof(dsp::complex_t);
            float gain = pow(10, (double)mflags / 20.0);
            volk_32f_s32f_multiply_32f((float*)_this->output->writeBuf, (float*)_this->readBuf, gain, sampCount * 2);
            _this->output->swap(sampCount);
        }

        //_this->client->readAsync(sizeof(QmxServerMessageHeader), (uint8_t*)&_this->receivedHeader, dataHandler, _this);
    }
#endif

    // Set local oscillator frequency in Hz.
    bool QmxServerClientClass::set_freq(int64_t frequency)
    {
        if (! m_connected)
            return false;
        CatCommandID cmd { CatCommandID::SetFreq };
        char buf[10];
        memcpy(buf,     &cmd,       2);
        memcpy(buf + 2, &frequency, 8);
        this->send_packet(buf, sizeof(buf));
        return true;
    }

    void QmxServerClientClass::run() {
        while (!m_exit_thread) {
            // Pump packets prepared by the UI thread to enet.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (ENetPacket* packet : m_queue)
                    enet_peer_send(m_peer, 1, packet);
                m_queue.clear();
            }

            // Block maximum 50 miliseconds.
            ENetEvent event;
            int eventStatus = enet_host_service(m_client, &event, 50);
            // If we had some event that interested us
            if (eventStatus > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    m_connected = true;
                    // printf("(Client) We got a new connection from %x\n", event.peer->address.host);
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    if (event.channelID == 0) {
                        // The length comes from the peer: check it, don't
                        // assert it. An assert let a bad packet abort a debug
                        // build; anything other than one full block is dropped.
                        if (event.packet->dataLength == EXT_BLOCKLEN * 2 * 2) {
                            float gain = 1.0;
                            volk_16i_s32f_convert_32f((float*)this->output->writeBuf, (int16_t*)event.packet->data, 32768.0 * gain, EXT_BLOCKLEN * 2);
                            this->output->swap(EXT_BLOCKLEN);
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    //                printf("(Client) %s disconnected.\n", event.peer->data);
                    // Reset client's information
                    //                event.peer->data = NULL;
                    m_connected = false;
                    enet_host_connect(m_client, &m_address, 2, 0);
                    //if (peer == nullptr) {
                        // fprintf(stderr, "No available peers for initializing an ENet connection");
                        // exit(EXIT_FAILURE);
                    //}
                    break;
                }
            }
        }
    }

    void QmxServerClientClass::send_packet(char *buf, int buflen)
    {
        this->send_packet(enet_packet_create(buf, buflen, ENET_PACKET_FLAG_RELIABLE));
    }

    void QmxServerClientClass::send_packet(ENetPacket* packet)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.emplace_back(packet);
    }

    QmxServerClient connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out) {
        if (enet_initialize() != 0) {
//            ::MessageBoxA(nullptr, "An error occured while initializing ENet.", "ExtIO_Omnia error", MB_OK | MB_ICONERROR);
            return {};
        }

        // Incoming: up to 288 kB/s (24 bit IQ at 48 kHz, 2 channels).
        // Outgoing: negligible (CAT commands only).
        // 0 = unlimited — do not let ENet throttle or drop packets.
        ENetHost *client = enet_host_create(nullptr, 1, 2, 0, 0);
        if (client == nullptr) {
            //fprintf(stderr, "An error occured while trying to create an ENet server host\n");
            return {};
        }

        ENetAddress address;
        enet_address_set_host_new(&address, host.c_str());
        address.port = port;

        // Connect and user service
        ENetPeer *peer = enet_host_connect(client, &address, 2, 0);
        if (peer == nullptr) {
            // fprintf(stderr, "No available peers for initializing an ENet connection");
            return {};
        }

        return QmxServerClient(new QmxServerClientClass(client, peer, address, out));
    }
}
