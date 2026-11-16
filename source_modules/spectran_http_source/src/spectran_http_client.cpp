#include "spectran_http_client.h"
#include <utils/flog.h>
#include <inttypes.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <exception>
#include <system_error>

SpectranHTTPClient::SpectranHTTPClient(std::string host, int port, dsp::stream<dsp::complex_t>* stream) {
    this->stream = stream;

    // Connect to server. Called from the GUI thread (Connect button); bound
    // DNS + TCP connect instead of freezing the UI for the OS-level timeout.
    this->host = host;
    this->port = port;
    sock = net::connect(host, port, 5000);
    http = net::http::Client(sock);

    // Send sttream request
    net::http::RequestHeader rqhdr(net::http::METHOD_GET, "/stream?format=float32", host);
    http.sendRequestHeader(rqhdr);

    // Receive for response
    net::http::ResponseHeader rshdr;
    http.recvResponseHeader(rshdr, 5000);

    // Check the status
    if (rshdr.getStatusCode() != net::http::STATUS_CODE_OK) {
        flog::error("HTTP request did not return ok: {}", rshdr.getStatusString());
        throw std::runtime_error("HTTP request did not return ok");
    }
}

void SpectranHTTPClient::startWorker() {
    // Start chunk worker
    workerThread = std::thread(&SpectranHTTPClient::worker, this);
}

void SpectranHTTPClient::streaming(bool enabled) {
    streamingEnabled = enabled;
}

bool SpectranHTTPClient::isOpen() {
    return sock->isOpen();
}

void SpectranHTTPClient::close() {
    sock->close();
    stream->stopWriter();
    if (workerThread.joinable()) { workerThread.join(); }
    stream->clearWriteStop();
}

void SpectranHTTPClient::setCenterFrequency(uint64_t freq) {
    // Connect to control endpoint (TODO: Switch to an always connected endpoint).
    // Bound the connect: a stale control endpoint must not hang the tuning path.
    auto controlSock = net::connect(host, port, 5000);
    auto controlHttp = net::http::Client(controlSock);

    // Encode request body
    net::http::RequestHeader rqhdr(net::http::METHOD_PUT, "/remoteconfig", host);
    char buf[1024];
    sprintf(buf, "{\"receiverName\": \"Block_IQDemodulator_0\", \"simpleconfig\": {\"main\": {\"centerfreq\": %" PRIu64 ", \"samplerate\": %" PRIu64 ", \"spanfreq\": %" PRIu64 "}}}", freq, _samplerate, _samplerate);
    std::string data = buf;
    char lenBuf[16];
    sprintf(lenBuf, "%" PRIu64, (uint64_t)data.size());

    // Setup request headers
    rqhdr.setField("Content-Length", lenBuf);

    // Send request
    controlHttp.sendRequestHeader(rqhdr);
    controlSock->sendstr(data);

    // Receive response
    net::http::ResponseHeader rshdr;
    controlHttp.recvResponseHeader(rshdr, 5000);

    // Log error if there is one
    if (rshdr.getStatusCode() < 200 || rshdr.getStatusCode() >= 300) {
        flog::debug("Response: {}", rshdr.getStatusString());
    }
}

namespace {
    // Reads the integer after `key` in a flat JSON object. Returns false if the
    // key is absent or no digits follow it, never throws: the metadata is peer
    // data. A fractional part ("46080000.0") stops the parse, as std::stoll did.
    bool parseIntField(const std::string& json, const char* key, int64_t& value) {
        const size_t keyPos = json.find(key);
        if (keyPos == std::string::npos) { return false; }
        const char* p = json.data() + keyPos + strlen(key);
        const char* end = json.data() + json.size();
        while (p < end && (*p == ' ' || *p == '\t')) { p++; }
        const auto result = std::from_chars(p, end, value);
        return result.ec == std::errc() && result.ptr != p;
    }
}

void SpectranHTTPClient::worker() {
    // Backstop: an exception escaping a std::thread terminates the process,
    // and everything this loop reads comes from the network.
    try {
        workerLoop();
    }
    catch (const std::exception& e) {
        flog::error("Spectran HTTP worker stopped: {}", e.what());
    }
    catch (...) {
        flog::error("Spectran HTTP worker stopped on an unknown exception");
    }
}

void SpectranHTTPClient::workerLoop() {
    while (sock->isOpen()) {
        // Get chunk header
        net::http::ChunkHeader chdr;
        int err = http.recvChunkHeader(chdr, 5000);
        if (err < 0) { return; }

        // If null length, finish
        size_t clen = chdr.getLength();
        if (!clen) { return; }

        // Read JSON. clen is a peer-controlled size_t and recvline() takes an
        // int: narrowing can turn a length into 0, which recvline() treats as
        // "no limit", or into a negative. Cap the metadata line explicitly;
        // the real line is a few hundred bytes.
        constexpr size_t MAX_METADATA_LINE = 64 * 1024;
        const int lineCap = (int)std::min(clen, MAX_METADATA_LINE);
        std::string jsonData;
        int jlen = sock->recvline(jsonData, lineCap, 5000);
        if (jlen <= 0) {
            flog::error("Couldn't read JSON metadata");
            return;
        }
        // recvline() counts the '\n' but does not store it: a full read with
        // nothing dropped means the line was cut off at the cap.
        if (jlen == lineCap && (int)jsonData.size() == jlen) {
            flog::error("JSON metadata line too long or unterminated");
            return;
        }

        // Decode JSON (yes, this is hacky, but it must be extremely fast).
        // Every field is peer data: a missing or malformed one rejects the
        // chunk instead of throwing out of this thread.
        int64_t startFreq = 0;
        int64_t endFreq = 0;
        int64_t sampleFreq = 0;
        if (!parseIntField(jsonData, "\"startFrequency\":", startFreq) ||
            !parseIntField(jsonData, "\"endFrequency\":", endFreq)) {
            flog::error("Malformed JSON metadata");
            return;
        }
        const bool sampleFreqReceived = parseIntField(jsonData, "\"sampleFrequency\":", sampleFreq);

        // Validate the protocol ranges before any arithmetic. Bounding both
        // edges to [0, MAX_FREQ] makes the span and the midpoint below exact
        // int64_t operations; the center must also be non-negative, since it
        // is published as a uint64_t. The samplerate configures the DSP
        // chain, so an enormous one is rejected as well.
        constexpr int64_t MAX_FREQ = 1'000'000'000'000;    // 1 THz
        constexpr int64_t MAX_SAMPLERATE = 1'000'000'000;  // 1 GS/s
        if (startFreq < 0 || endFreq > MAX_FREQ || startFreq > endFreq) {
            flog::error("Invalid frequency range in metadata: [{}, {}]", startFreq, endFreq);
            return;
        }
        int64_t samplerate = sampleFreqReceived ? sampleFreq : (endFreq - startFreq);
        if (samplerate <= 0 || samplerate > MAX_SAMPLERATE) {
            flog::error("Invalid samplerate in metadata: {}", samplerate);
            return;
        }

        // Calculate and update center freq
        // Rounds half up, like round() on the exact midpoint.
        int64_t centerFreq = startFreq + (endFreq - startFreq + 1) / 2;
        if (centerFreq != _centerFreq) {
            flog::debug("New center freq: {}", centerFreq);
            _centerFreq = centerFreq;
            onCenterFrequencyChanged(centerFreq);
        }
        if (samplerate != _samplerate) {
            flog::debug("New samplerate: {}", samplerate);
            _samplerate = samplerate;
            onSamplerateChanged(samplerate);
        }

        // Read (and check for) record separator
        uint8_t rs;
        int rslen = sock->recv(&rs, 1, true, 5000);
        if (rslen != 1 || rs != 0x1E) {
            flog::error("Missing record separator");
            return;
        }

        // The chunk length comes from the server. What follows the JSON line
        // and the record separator is sample data of that remaining length,
        // so it is validated before anything is read into writeBuf.
        if (clen < (size_t)jlen + 1) {
            flog::error("Chunk shorter than its metadata");
            return;
        }
        size_t remaining = clen - ((size_t)jlen + 1);

        // Read it in pieces of at most one stream buffer, publishing each.
        // capBytes is a whole number of complex samples, so only the final
        // piece can end in a partial sample, which is dropped as before.
        // writeBuf changes on every swap, so it is re-read per piece.
        const size_t capBytes = (size_t)stream->capacity() * sizeof(dsp::complex_t);
        while (remaining > 0) {
            const size_t want = std::min(remaining, capBytes);
            uint8_t* buf = (uint8_t*)stream->writeBuf;
            size_t got = 0;
            while (got < want) {
                int read = sock->recv(&buf[got], want - got, true);
                if (read <= 0) {
                    flog::error("Recv failed while reading data");
                    return;
                }
                got += (size_t)read;
            }
            remaining -= want;

            const int sampCount = (int)(got / sizeof(dsp::complex_t));
            if (streamingEnabled && sampCount > 0) {
                if (!stream->swap(sampCount)) { return; }
            }
        }
        
        // Read trailing CRLF
        std::string dummy;
        sock->recvline(dummy, 2);
        if (dummy != "\r") {
            flog::error("Missing trailing CRLF");
            return;
        }
    }
}