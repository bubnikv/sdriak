#include "http.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <inttypes.h>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../ascii.h"

namespace net::http {
    namespace {
        bool containsHeaderToken(const std::string& value, const std::string& token) {
            size_t start = 0;
            while (start <= value.size()) {
                const size_t end = value.find(',', start);
                std::string part = ascii::trim(value.substr(
                    start,
                    end == std::string::npos ? std::string::npos : end - start));
                if (ascii::equalsIgnoreCase(part, token)) {
                    return true;
                }
                if (end == std::string::npos) { break; }
                start = end + 1;
            }
            return false;
        }

        bool parseContentLength(const std::string& value, size_t& length) {
            if (value.empty()) { return false; }
            try {
                size_t consumed = 0;
                length = std::stoull(value, &consumed);
                return consumed == value.size();
            }
            catch (...) {
                return false;
            }
        }

        int remainingMsOrThrow(std::chrono::steady_clock::time_point deadline, const char* what) {
            using namespace std::chrono;
            const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
            if (remaining <= 0) {
                throw std::runtime_error(std::string("http: ") + what + " timed out");
            }
            return static_cast<int>(std::min<int64_t>(remaining, 1000));
        }

        std::string requestPathFor(const url::HttpHostPort& endpoint) {
            if (endpoint.path.empty()) { return "/"; }
            if (endpoint.path[0] == '?' || endpoint.path[0] == '#') {
                return "/" + endpoint.path;
            }
            return endpoint.path;
        }

        void sendAll(std::shared_ptr<Socket>& sock, const std::string& data,
                     std::chrono::steady_clock::time_point deadline) {
            size_t sent = 0;
            while (sent < data.size()) {
                int n = sock->send(reinterpret_cast<const uint8_t*>(data.data()) + sent, data.size() - sent);
                if (n > 0) {
                    sent += static_cast<size_t>(n);
                    continue;
                }
                if (!sock->isOpen()) {
                    throw std::runtime_error("http: request send failed");
                }
                remainingMsOrThrow(deadline, "request send");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // Read header lines into `data` until a blank terminator (bare "\r"
        // for CRLF servers, or empty for LF-only servers).
        //   1     blank terminator seen (full header complete)
        //   0     recvline returned 0 (timeout / peer closed before terminator)
        //         OR the socket was already closed when called
        //   <0    recvline returned a negative error code
        // Throws if maxSize is exceeded. The timeout for each recvline is
        // taken from nextTimeoutMs, allowing per-call or deadline-based
        // policy. Distinguishing 1 from 0 is important: recvline also returns
        // 0 on success-with-zero-bytes-read, so the caller cannot use return
        // value 0 alone to confirm the header is complete.
        int recvHeaderBytes(Socket& sock, std::string& data,
                            const std::function<int()>& nextTimeoutMs,
                            size_t maxSize, int maxLineLen) {
            while (sock.isOpen()) {
                std::string line;
                const int n = sock.recvline(line, maxLineLen, nextTimeoutMs());
                if (n <= 0) { return n; }
                if (line.empty() || line == "\r") { return 1; }
                data += line + "\n";
                if (data.size() > maxSize) {
                    throw std::runtime_error("http: header too large");
                }
            }
            return 0;
        }

        ResponseHeader recvResponseHeader(std::shared_ptr<Socket>& sock,
                                          std::chrono::steady_clock::time_point deadline) {
            std::string data;
            const int rc = recvHeaderBytes(*sock, data,
                [&] { return remainingMsOrThrow(deadline, "header read"); },
                64 * 1024, 8192);
            if (rc != 1) {
                throw std::runtime_error("http: response header read failed");
            }
            ResponseHeader header;
            header.deserialize(data);
            return header;
        }

        void appendBytes(std::shared_ptr<Socket>& sock, std::string& body, size_t count, size_t maxBody,
                         std::chrono::steady_clock::time_point deadline) {
            if (body.size() + count > maxBody) {
                throw std::runtime_error("http: response body too large");
            }

            std::vector<uint8_t> buf(1024);
            while (count > 0) {
                const size_t want = std::min(buf.size(), count);
                int n = sock->recv(buf.data(), want, false, remainingMsOrThrow(deadline, "body read"));
                if (n <= 0) {
                    throw std::runtime_error("http: response body read failed");
                }
                body.append(reinterpret_cast<const char*>(buf.data()), n);
                count -= static_cast<size_t>(n);
            }
        }

        std::string recvChunkedBody(std::shared_ptr<Socket>& sock, size_t maxBody,
                                    std::chrono::steady_clock::time_point deadline) {
            std::string body;
            while (true) {
                std::string line;
                int n = sock->recvline(line, 8192, remainingMsOrThrow(deadline, "chunk header read"));
                if (n <= 0) {
                    throw std::runtime_error("http: chunk header read failed");
                }

                line = ascii::trim(line);
                const auto extension = line.find(';');
                if (extension != std::string::npos) { line.resize(extension); }

                size_t consumed = 0;
                size_t chunkSize = 0;
                try {
                    chunkSize = std::stoull(line, &consumed, 16);
                }
                catch (...) {
                    throw std::runtime_error("http: malformed chunk size");
                }
                if (consumed != line.size()) {
                    throw std::runtime_error("http: malformed chunk size");
                }

                if (chunkSize == 0) {
                    while (true) {
                        std::string trailer;
                        int trailerLen = sock->recvline(trailer, 8192, remainingMsOrThrow(deadline, "chunk trailer read"));
                        if (trailerLen <= 0) {
                            throw std::runtime_error("http: chunk trailer read failed");
                        }
                        if (trailer == "\r" || trailer.empty()) { break; }
                    }
                    break;
                }

                appendBytes(sock, body, chunkSize, maxBody, deadline);

                std::string crlf;
                int crlfLen = sock->recvline(crlf, 2, remainingMsOrThrow(deadline, "chunk terminator read"));
                if (crlfLen <= 0 || crlf != "\r") {
                    throw std::runtime_error("http: malformed chunk terminator");
                }
            }
            return body;
        }

        std::string recvBody(std::shared_ptr<Socket>& sock, ResponseHeader& header, size_t maxBody,
                             std::chrono::steady_clock::time_point deadline) {
            if (containsHeaderToken(getHeaderValue(header, "Transfer-Encoding"), "chunked")) {
                return recvChunkedBody(sock, maxBody, deadline);
            }

            size_t contentLength = 0;
            const bool hasContentLength = parseContentLength(getHeaderValue(header, "Content-Length"), contentLength);
            if (hasContentLength && contentLength > maxBody) {
                throw std::runtime_error("http: response body too large");
            }

            std::string body;
            std::vector<uint8_t> buf(1024);
            while (body.size() < maxBody) {
                size_t want = buf.size();
                if (hasContentLength) {
                    if (body.size() >= contentLength) { break; }
                    want = std::min(want, contentLength - body.size());
                }

                int n = sock->recv(buf.data(), want, false, remainingMsOrThrow(deadline, "body read"));
                if (n < 0) {
                    throw std::runtime_error("http: response body read failed");
                }
                if (n == 0) {
                    if (hasContentLength && body.size() < contentLength) {
                        throw std::runtime_error("http: response body ended before Content-Length");
                    }
                    break;
                }
                body.append(reinterpret_cast<const char*>(buf.data()), n);
            }

            if (!hasContentLength && body.size() >= maxBody) {
                throw std::runtime_error("http: response body too large");
            }
            return body;
        }
    }

    std::string MessageHeader::serialize() {
        std::string data;

        // Add start line
        data += serializeStartLine() + "\r\n";

        // Add fields
        for (const auto& [key, value] : fields) {
            data += key + ": " + value + "\r\n";
        }

        // Add terminator
        data += "\r\n";

        return data;
    }

    void MessageHeader::deserialize(const std::string& data) {
        // Clear existing fields
        fields.clear();

        // Parse first line
        std::string line;
        int offset = readLine(data, line);
        deserializeStartLine(line);

        // Parse fields
        while (offset < data.size()) {
            // Read line
            offset = readLine(data, line, offset);

            // If empty line, the header is done
            if (line.empty()) { break; }

            // Read until first ':' for the key
            int klen = 0;
            for (; klen < line.size(); klen++) {
                if (line[klen] == ':') { break; }
            }
            
            // Find offset of value
            int voff = klen + 1;
            for (; voff < line.size(); voff++) {
                if (line[voff] != ' ' && line[voff] != '\t') { break; }
            }

            // Save field. RFC 7230 §3.2.2: repeated field-names with the
            // same name must be combined into a single comma-separated
            // value. (Set-Cookie is the documented exception, but this
            // client does not consume cookies.)
            std::string name = line.substr(0, klen);
            std::string value = line.substr(voff);
            auto [it, inserted] = fields.emplace(std::move(name), std::move(value));
            if (!inserted) {
                it->second += ", ";
                it->second += line.substr(voff);
            }
        }
    }

    MessageHeader::FieldMap& MessageHeader::getFields() {
        return fields;
    }

    const MessageHeader::FieldMap& MessageHeader::getFields() const {
        return fields;
    }

    bool MessageHeader::hasField(const std::string& name) const {
        return fields.find(name) != fields.end();
    }

    std::string MessageHeader::getField(const std::string& name) const {
        const auto it = fields.find(name);
        return it == fields.end() ? std::string{} : it->second;
    }

    void MessageHeader::setField(const std::string& name, const std::string& value) {
        fields[name] = value;
    }

    void MessageHeader::clearField(const std::string& name) {
        fields.erase(name);
    }

    int MessageHeader::readLine(const std::string& str, std::string& line, int start) {
        const auto lf = str.find('\n', start);
        const int len = lf == std::string::npos
                            ? static_cast<int>(str.size()) - start
                            : static_cast<int>(lf) - start;
        const bool cr = len > 0 && str[start + len - 1] == '\r';
        line = str.substr(start, len - (cr ? 1 : 0));
        return lf == std::string::npos ? static_cast<int>(str.size()) : start + len + 1;
    }

    RequestHeader::RequestHeader(Method method, std::string uri, std::string host) {
        this->method = method;
        this->uri = uri;
        setField("Host", host);
    }

    RequestHeader::RequestHeader(const std::string& data) {
        deserialize(data);
    }

    Method RequestHeader::getMethod() {
        return method;
    }

    void RequestHeader::setMethod(Method method) {
        this->method = method;
    }

    std::string RequestHeader::getURI() {
        return uri;
    }

    void RequestHeader::setURI(const std::string& uri) {
        this->uri = uri;
    }

    void RequestHeader::deserializeStartLine(const std::string& data) {
        // Request line: METHOD SP URI SP HTTP/<version>
        const auto methodEnd = data.find(' ');
        if (methodEnd == std::string::npos) {
            throw std::runtime_error("http: malformed request line");
        }
        const std::string methodStr = data.substr(0, methodEnd);
        const auto uriStart = data.find_first_not_of(" \t", methodEnd);
        if (uriStart == std::string::npos) {
            throw std::runtime_error("http: malformed request line");
        }
        const auto uriEnd = data.find(' ', uriStart);
        if (uriEnd == std::string::npos) {
            throw std::runtime_error("http: malformed request line");
        }
        uri = data.substr(uriStart, uriEnd - uriStart);

        method = METHOD_GET;
        bool methodFound = false;
        for (const auto& e : MethodStrings) {
            if (e.name == methodStr) {
                method = e.method;
                methodFound = true;
                break;
            }
        }
        if (!methodFound) {
            throw std::runtime_error("http: unknown HTTP method '" + methodStr + "'");
        }
    }

    std::string RequestHeader::serializeStartLine() {
        // TODO: Allow to specify version
        const auto name = methodName(method);
        return std::string(name) + " " + uri + " HTTP/1.1";
    }

    ResponseHeader::ResponseHeader(StatusCode statusCode) {
        this->statusCode = statusCode;
        const auto name = statusCodeName(statusCode);
        this->statusString = name.empty() ? std::string("UNKNOWN") : std::string(name);
    }

    ResponseHeader::ResponseHeader(StatusCode statusCode, const std::string& statusString) {
        this->statusCode = statusCode;
        this->statusString = statusString;
    }

    ResponseHeader::ResponseHeader(const std::string& data) {
        deserialize(data);
    }

    StatusCode ResponseHeader::getStatusCode() const {
        return statusCode;
    }

    void ResponseHeader::setStatusCode(StatusCode statusCode) {
        this->statusCode = statusCode;
    }

    std::string ResponseHeader::getStatusString() const {
        return statusString;
    }

    void ResponseHeader::setStatusString(const std::string& statusString) {
        this->statusString = statusString;
    }

    void ResponseHeader::deserializeStartLine(const std::string& data) {
        // Status line: HTTP/<version> SP <code> SP <reason-phrase>
        // The reason phrase may contain spaces, so it is the entire remainder.
        const auto versionEnd = data.find(' ');
        if (versionEnd == std::string::npos) {
            throw std::runtime_error("http: malformed status line");
        }
        size_t codeStart = data.find_first_not_of(" \t", versionEnd);
        if (codeStart == std::string::npos) {
            throw std::runtime_error("http: malformed status line");
        }
        const auto codeEnd = data.find(' ', codeStart);
        const std::string codeStr = data.substr(codeStart,
            codeEnd == std::string::npos ? std::string::npos : codeEnd - codeStart);
        statusCode = static_cast<StatusCode>(std::stoi(codeStr));

        if (codeEnd == std::string::npos) {
            statusString.clear();
        }
        else {
            const size_t reasonStart = data.find_first_not_of(" \t", codeEnd);
            statusString = reasonStart == std::string::npos ? std::string{} : data.substr(reasonStart);
        }
    }

    std::string ResponseHeader::serializeStartLine() {
        return "HTTP/1.1 " + std::to_string(static_cast<int>(statusCode)) + " " + statusString;
    }

    ChunkHeader::ChunkHeader(size_t length) {
        this->length = length;
    }

    ChunkHeader::ChunkHeader(const std::string& data) {
        deserialize(data);
    }

    std::string ChunkHeader::serialize() {
        char buf[64];
        snprintf(buf, sizeof buf, "%zX\r\n", length);
        return buf;
    }

    void ChunkHeader::deserialize(const std::string& data) {
        // RFC 7230 §4.1: chunk-size [ ";" chunk-ext ]. We only need the size;
        // any extension after the first ';' is discarded.
        const auto extension = data.find(';');
        const std::string sizeStr = ascii::trim(
            extension == std::string::npos ? data : data.substr(0, extension));
        if (sizeStr.empty()) {
            throw std::runtime_error("http: malformed chunk size");
        }
        size_t consumed = 0;
        length = std::stoull(sizeStr, &consumed, 16);
        if (consumed != sizeStr.size()) {
            throw std::runtime_error("http: malformed chunk size");
        }
    }

    size_t ChunkHeader::getLength() {
        return length;
    }

    void ChunkHeader::setLength(size_t length) {
        this->length = length;
    }

    Client::Client(std::shared_ptr<Socket> sock) {
        this->sock = sock;
    }

    int Client::sendRequestHeader(RequestHeader& req) {
        return sock->sendstr(req.serialize());
    }

    int Client::recvRequestHeader(RequestHeader& req, int timeout) {
        // Non-blocking mode not alloowed
        if (!timeout) { return -1; }

        // Read response
        std::string respData;
        int err = recvHeader(respData, timeout);
        if (err) { return err; }

        // Deserialize
        req.deserialize(respData);
        return 0; // Might wanna return size instead
    }

    int Client::sendResponseHeader(ResponseHeader& resp) {
        return sock->sendstr(resp.serialize());
    }

    int Client::recvResponseHeader(ResponseHeader& resp, int timeout) {
        // Non-blocking mode not alloowed
        if (!timeout) { return -1; }

        // Read response
        std::string respData;
        int err = recvHeader(respData, timeout);
        if (err) { return err; }

        // Deserialize
        resp.deserialize(respData);
        return 0; // Might wanna return size instead
    }

    int Client::sendChunkHeader(ChunkHeader& chdr) {
        return sock->sendstr(chdr.serialize());
    }

    int Client::recvChunkHeader(ChunkHeader& chdr, int timeout) {
        // The chunk-size line comes from the peer. Bound it: recvline() with
        // a max of 0 reads without limit, so a server that never sends '\n'
        // could grow this string until the process runs out of memory. A
        // chunk size is at most 16 hex digits; the cap leaves room for chunk
        // extensions.
        constexpr int MAX_CHUNK_LINE = 4096;
        std::string respData;
        int err = sock->recvline(respData, MAX_CHUNK_LINE, timeout);
        if (err <= 0) { return err; }
        // recvline() counts the '\n' but does not store it, so a full read
        // with nothing dropped means the line hit the cap unterminated.
        if (err == MAX_CHUNK_LINE && (int)respData.size() == err) { return -1; }
        if (!respData.empty() && respData[respData.size()-1] == '\r') {
            respData.pop_back();
        }
        if (respData.empty()) { return -1; }
        // deserialize() throws on a malformed or out-of-range size. Callers
        // run this on worker threads, where an escaping exception terminates
        // the process, so report it as a protocol error instead.
        try {
            chdr.deserialize(respData);
        }
        catch (const std::exception&) {
            return -1;
        }
        return 0;
    }

    int Client::recvHeader(std::string& data, int timeout) {
        // Per-call timeout, with the same 64 KiB total / 8 KiB line caps as
        // recvResponseHeader(): the header is peer data, and an unterminated
        // or endless one would otherwise grow `data` without limit.
        //   helper rc == 1   saw terminator → API success (0)
        //   helper rc <  0   hard recvline error → pass through
        //   helper rc == 0   recvline returned 0 mid-headers (timeout /
        //                    peer-close before terminator) → treat as a hard
        //                    failure (-1) so the caller's `if (err)` check
        //                    rejects the partial header instead of feeding
        //                    truncated bytes to deserialize().
        //   helper throws    header too large → hard failure (-1); callers
        //                    run on worker threads and expect an error code.
        int rc;
        try {
            rc = recvHeaderBytes(*sock, data, [timeout] { return timeout; },
                                 64 * 1024, 8192);
        }
        catch (const std::exception&) {
            return -1;
        }
        if (rc == 1) { return 0; }
        if (rc < 0)  { return rc; }
        return -1;
    }

    std::string hostHeaderFor(const url::HttpHostPort& endpoint) {
        return endpoint.port == 80 ? endpoint.host : (endpoint.host + ":" + std::to_string(endpoint.port));
    }

    std::string getHeaderValue(const MessageHeader& header, const std::string& name) {
        return ascii::trim(header.getField(name));
    }

    bool isRedirectStatus(int status) {
        return status == STATUS_CODE_MOVED_PERMANENTLY ||
               status == STATUS_CODE_FOUND ||
               status == STATUS_CODE_SEE_OTHER ||
               status == STATUS_CODE_TEMP_REDIRECT ||
               status == STATUS_CODE_PERMANENT_REDIRECT;
    }

    std::optional<url::HttpHostPort> resolveRedirectLocation(
        const url::HttpHostPort& current,
        const ResponseHeader& response) {
        if (!isRedirectStatus(static_cast<int>(response.getStatusCode()))) {
            return std::nullopt;
        }
        const std::string location = getHeaderValue(response, "Location");
        if (location.empty()) {
            return std::nullopt;
        }
        return url::resolveHttpLocation(current, location);
    }

    Response get(const url::HttpHostPort& endpoint, const RequestOptions& options) {
        // The whole request (connect, send, header, body, across redirects) is
        // governed by a single finite deadline; there is no unbounded mode. A
        // non-positive timeout would block the connect on the OS default yet
        // expire the deadline before the first send/read, so reject it.
        if (options.timeoutMs <= 0) {
            throw std::invalid_argument("http: timeoutMs must be positive");
        }

        url::HttpHostPort current = endpoint;
        current.path = requestPathFor(current);

        for (int redirectsLeft = options.maxRedirects; redirectsLeft >= 0; redirectsLeft--) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(options.timeoutMs);
            // Bound connection establishment (DNS + TCP) by the same request
            // timeout that governs the response below.
            auto sock = net::connect(current.host, current.port, options.timeoutMs);

            RequestHeader request(METHOD_GET, requestPathFor(current), hostHeaderFor(current));
            request.setField("Connection", "close");
            for (const auto& [key, value] : options.headers) {
                request.setField(key, value);
            }

            sendAll(sock, request.serialize(), deadline);

            Response response;
            response.header = recvResponseHeader(sock, deadline);
            response.endpoint = current;

            const int status = static_cast<int>(response.header.getStatusCode());
            if (isRedirectStatus(status)) {
                auto redirected = resolveRedirectLocation(current, response.header);
                if (redirectsLeft == 0 || !redirected) {
                    sock->close();
                    throw std::runtime_error("http: redirect cannot be followed: " +
                                             getHeaderValue(response.header, "Location"));
                }
                sock->close();
                current = *redirected;
                current.path = requestPathFor(current);
                continue;
            }

            response.body = recvBody(sock, response.header, options.maxBody, deadline);
            sock->close();
            return response;
        }

        throw std::runtime_error("http: redirect limit exceeded");
    }
}
