#include "rtl_tcp_client.h"

namespace rtltcp {
    Client::Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* stream) {
        this->sock = sock;
        this->stream = stream;

        // Start worker
        workerThread = std::thread(&Client::worker, this);
    }

    Client::~Client() {
        close();
    }

    bool Client::isOpen() {
        return sock->isOpen();
    }

    void Client::close() {
        sock->close();
        stream->stopWriter();
        if (workerThread.joinable()) {
            workerThread.join();
        }
        stream->clearWriteStop();
    }

    void Client::setFrequency(double freq) {
        sendCommand(1, freq);
    }

    void Client::setSampleRate(double sr) {
        sendCommand(2, sr);
        bufferSize = dsp::sourceBlockSize(sr, *stream);
    }

    void Client::setGainMode(int mode) {
        sendCommand(3, mode);
    }

    void Client::setGain(double gain) {
        sendCommand(4, gain);
    }

    void Client::setPPM(int ppm) {
        sendCommand(5, (uint32_t)ppm);
    }

    void Client::setAGCMode(int mode) {
        sendCommand(8, mode);
    }

    void Client::setDirectSampling(int mode) {
        sendCommand(9, mode);
    }

    void Client::setOffsetTuning(bool enabled) {
        sendCommand(10, enabled);
    }

    void Client::setGainIndex(int index) {
        sendCommand(13, index);
    }

    void Client::setBiasTee(bool enabled) {
        sendCommand(14, enabled);
    }

    void Client::sendCommand(uint8_t command, uint32_t param) {
        Command cmd = { command, htonl(param) };
        sock->send((uint8_t*)&cmd, sizeof(Command));
    }

    void Client::worker() {
        // Two bytes per IQ sample; bufferSize is bounded by the stream.
        uint8_t* buffer = dsp::buffer::alloc<uint8_t>((size_t)stream->capacity() * 2);

        while (true) {
            // Read data
            int count = sock->recv(buffer, bufferSize * 2, true);
            if (count <= 0) { break; }

            // Convert to complex float
            int scount = count/2;
            for (int i = 0; i < scount; i++) {
                stream->writeBuf[i].re = ((double)buffer[i * 2] - 128.0) / 128.0;
                stream->writeBuf[i].im = ((double)buffer[(i * 2) + 1] - 128.0) / 128.0;
            }

            // Swap buffer
            if (!stream->swap(scount)) { break; }
        }

        dsp::buffer::free(buffer);
    }

    std::shared_ptr<Client> connect(dsp::stream<dsp::complex_t>* stream, std::string host, int port) {
        // Called from the GUI thread (Play button); bound DNS + TCP connect
        // instead of freezing the UI for the OS-level connect timeout.
        auto sock = net::connect(host, port, 5000);
        return std::make_shared<Client>(sock, stream);
    }
}