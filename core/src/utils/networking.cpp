#include <utils/networking.h>
#include <assert.h>
#include <utils/flog.h>
#include <utils/net_connect.h>
#include <utils/net_resolve.h>
#include <stdexcept>
#include <chrono>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#endif

namespace net {
    static void closeSocket(Socket sock) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
    }

    static void shutdownSocket(Socket sock) {
#ifdef _WIN32
        ::shutdown(sock, SD_BOTH);
#else
        ::shutdown(sock, SHUT_RDWR);
#endif
    }

    ConnClass::ConnClass(Socket sock, struct sockaddr_in raddr, bool udp) {
        _sock = sock;
        _udp = udp;
        remoteAddr = raddr;
        connectionOpen = true;
    }

    ConnClass::~ConnClass() {
        ConnClass::close();

        // Only the owner destroys a connection, and nobody else can still be
        // inside read()/write() on it then, so this is the one safe point to
        // release the descriptor. A close() from another thread only shuts it
        // down; releasing it there could let a concurrent recv()/send() hit a
        // descriptor number already reused by an unrelated open.
        std::lock_guard lck(closeMtx);
        if (!socketReleased) {
            socketReleased = true;
            closeSocket(_sock);
        }
    }

    void ConnClass::close() {
        std::lock_guard lck(closeMtx);
        // Set stopWorkers to true
        {
            std::lock_guard lck1(readQueueMtx);
            std::lock_guard lck2(writeQueueMtx);
            stopWorkers = true;
        }

        // Notify the workers of the change
        readQueueCnd.notify_all();
        writeQueueCnd.notify_all();

        // Wake any thread blocked in recv()/send() on this connection, even
        // if the peer already disconnected (which clears connectionOpen before
        // we get here). The descriptor itself is released by the destructor.
        if (!socketShutdown) {
            socketShutdown = true;
            shutdownSocket(_sock);
        }

        // Wait for the theads to terminate
        if (readWorkerThread.joinable() && readWorkerThread.get_id() != std::this_thread::get_id()) { readWorkerThread.join(); }
        if (writeWorkerThread.joinable() && writeWorkerThread.get_id() != std::this_thread::get_id()) { writeWorkerThread.join(); }

        {
            std::lock_guard lck(connectionOpenMtx);
            connectionOpen = false;
        }
        connectionOpenCnd.notify_all();
    }

    bool ConnClass::isOpen() {
        return connectionOpen;
    }

    void ConnClass::waitForEnd() {
        // Wait with the same mutex the writers hold when clearing
        // connectionOpen, otherwise the notify can slip between the predicate
        // check and the wait and this blocks forever.
        std::unique_lock lck(connectionOpenMtx);
        connectionOpenCnd.wait(lck, [this]() { return !connectionOpen; });
    }

    int ConnClass::read(int count, uint8_t* buf, bool enforceSize) {
        if (!connectionOpen) { return -1; }
        std::lock_guard lck(readMtx);
        int ret;

        if (_udp) {
            socklen_t fromLen = sizeof(remoteAddr);
            ret = recvfrom(_sock, (char*)buf, count, 0, (struct sockaddr*)&remoteAddr, &fromLen);
            if (ret <= 0) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return -1;
            }
            return count;
        }

        int beenRead = 0;
        while (beenRead < count) {
            ret = recv(_sock, (char*)&buf[beenRead], count - beenRead, 0);

            if (ret <= 0) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return -1;
            }

            if (!enforceSize) { return ret; }

            beenRead += ret;
        }

        return beenRead;
    }

    bool ConnClass::write(int count, uint8_t* buf) {
        if (!connectionOpen) { return false; }
        std::lock_guard lck(writeMtx);
        int ret;

        if (_udp) {
            ret = sendto(_sock, (char*)buf, count, 0, (struct sockaddr*)&remoteAddr, sizeof(remoteAddr));
            if (ret <= 0) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
            }
            return (ret > 0);
        }

        int beenWritten = 0;
        while (beenWritten < count) {
            ret = send(_sock, (char*)&buf[beenWritten], count - beenWritten, 0);
            if (ret <= 0) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return false;
            }
            beenWritten += ret;
        }
        
        return true;
    }

    bool ConnClass::setLivenessChecks(int idleSec, int intervalSec, int probes, int userTimeoutMs) {
        if (_udp) { return false; }
#ifdef _WIN32
        DWORD on = 1;
        if (setsockopt(_sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&on, sizeof(on)) != 0) { return false; }
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
        DWORD idle = idleSec, interval = intervalSec, count = probes;
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&idle, sizeof(idle));
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&interval, sizeof(interval));
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPCNT, (const char*)&count, sizeof(count));
#endif
        (void)userTimeoutMs;
#else
        int on = 1;
        if (setsockopt(_sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) { return false; }
#if defined(TCP_KEEPIDLE)
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPIDLE, &idleSec, sizeof(idleSec));
#elif defined(TCP_KEEPALIVE)
        // macOS names the idle time TCP_KEEPALIVE.
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPALIVE, &idleSec, sizeof(idleSec));
#endif
#ifdef TCP_KEEPINTVL
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec));
#endif
#ifdef TCP_KEEPCNT
        setsockopt(_sock, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
#endif
#ifdef TCP_USER_TIMEOUT
        unsigned int timeout = (unsigned int)userTimeoutMs;
        setsockopt(_sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout));
#else
        (void)userTimeoutMs;
#endif
#endif
        return true;
    }

    void ConnClass::readAsync(int count, uint8_t* buf, void (*handler)(int count, uint8_t* buf, void* ctx), void* ctx, bool enforceSize) {
        if (!connectionOpen) { return; }
        // Create entry
        ConnReadEntry entry;
        entry.count = count;
        entry.buf = buf;
        entry.handler = handler;
        entry.ctx = ctx;
        entry.enforceSize = enforceSize;

        // Add entry to queue and create the worker only if this connection
        // actually uses asynchronous reads. Write-only users otherwise paid
        // for two idle threads per connection.
        {
            std::lock_guard lck(readQueueMtx);
            if (stopWorkers || !connectionOpen) { return; }
            readQueue.push_back(entry);
            if (!readWorkerThread.joinable()) {
                readWorkerThread = std::thread(&ConnClass::readWorker, this);
            }
        }

        // Notify read worker
        readQueueCnd.notify_all();
    }

    void ConnClass::writeAsync(int count, uint8_t* buf) {
        if (!connectionOpen) { return; }
        // Create entry
        ConnWriteEntry entry;
        entry.count = count;
        entry.buf = buf;

        // Add entry to queue
        {
            std::lock_guard lck(writeQueueMtx);
            if (stopWorkers || !connectionOpen) { return; }
            writeQueue.push_back(entry);
            if (!writeWorkerThread.joinable()) {
                writeWorkerThread = std::thread(&ConnClass::writeWorker, this);
            }
        }

        // Notify write worker
        writeQueueCnd.notify_all();
    }

    void ConnClass::readWorker() {
        while (true) {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(readQueueMtx);
            readQueueCnd.wait(lck, [this]() { return (readQueue.size() > 0 || stopWorkers); });
            if (stopWorkers || !connectionOpen) { return; }

            // Pop first element off the list
            ConnReadEntry entry = readQueue[0];
            readQueue.erase(readQueue.begin());
            lck.unlock();

            // Read from socket and send data to the handler
            int ret = read(entry.count, entry.buf, entry.enforceSize);
            if (ret <= 0) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return;
            }
            entry.handler(ret, entry.buf, entry.ctx);
        }
    }

    void ConnClass::writeWorker() {
        while (true) {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(writeQueueMtx);
            writeQueueCnd.wait(lck, [this]() { return (writeQueue.size() > 0 || stopWorkers); });
            if (stopWorkers || !connectionOpen) { return; }

            // Pop first element off the list
            ConnWriteEntry entry = writeQueue[0];
            writeQueue.erase(writeQueue.begin());
            lck.unlock();

            // Write to socket
            if (!write(entry.count, entry.buf)) {
                {
                    std::lock_guard lck(connectionOpenMtx);
                    connectionOpen = false;
                }
                connectionOpenCnd.notify_all();
                return;
            }
        }
    }


    ListenerClass::ListenerClass(Socket listenSock) {
        sock = listenSock;
        listening = true;
        acceptWorkerThread = std::thread(&ListenerClass::worker, this);
    }

    ListenerClass::~ListenerClass() {
        close();
    }

    Conn ListenerClass::accept() {
        if (!listening) { return NULL; }
        std::lock_guard lck(acceptMtx);
        Socket _sock;

        // Accept socket
        _sock = ::accept(sock, NULL, NULL);
#ifdef _WIN32
        if (_sock < 0 || _sock == SOCKET_ERROR) {
#else
        if (_sock < 0) {
#endif
            listening = false;
            throw std::runtime_error("Could not accept connection");
            return NULL;
        }

        return Conn(new ConnClass(_sock));
    }

    void ListenerClass::acceptAsync(void (*handler)(Conn conn, void* ctx), void* ctx) {
        if (!listening) { return; }
        // Create entry
        ListenerAcceptEntry entry;
        entry.handler = handler;
        entry.ctx = ctx;

        // Add entry to queue
        {
            std::lock_guard lck(acceptQueueMtx);
            acceptQueue.push_back(entry);
        }

        // Notify write worker
        acceptQueueCnd.notify_all();
    }

    void ListenerClass::close() {
        std::lock_guard lckc(closeMtx);
        {
            std::lock_guard lck(acceptQueueMtx);
            stopWorker = true;
        }
        acceptQueueCnd.notify_all();

        // Wake the accept worker first and release the descriptor only once
        // it has left accept(), so it can never accept on a reused number.
        // listening may already be false if accept() failed; the socket still
        // has to be released.
        if (!socketShutdown) {
            socketShutdown = true;
            shutdownSocket(sock);
        }

        // A handler running on the worker may close its own listener; that
        // thread cannot be joined from itself, and the destructor, running on
        // another thread, joins it and releases the descriptor.
        bool onWorker = acceptWorkerThread.get_id() == std::this_thread::get_id();
        if (acceptWorkerThread.joinable() && !onWorker) { acceptWorkerThread.join(); }

        if (!onWorker && !socketReleased) {
            socketReleased = true;
            closeSocket(sock);
        }

        listening = false;
    }

    bool ListenerClass::isListening() {
        return listening;
    }

    void ListenerClass::worker() {
        while (true) {
            // Wait for wakeup and exit if it's for terminating the thread
            std::unique_lock lck(acceptQueueMtx);
            acceptQueueCnd.wait(lck, [this]() { return (acceptQueue.size() > 0 || stopWorker); });
            if (stopWorker || !listening) { return; }

            // Pop first element off the list
            ListenerAcceptEntry entry = acceptQueue[0];
            acceptQueue.erase(acceptQueue.begin());
            lck.unlock();

            // Read from socket and send data to the handler
            try {
                Conn client = accept();
                if (!client) {
                    listening = false;
                    return;
                }
                entry.handler(std::move(client), entry.ctx);
            }
            catch (const std::exception& e) {
                listening = false;
                return;
            }
        }
    }


    Conn connect(std::string host, uint16_t port) {
        return connect(std::move(host), port, -1);
    }

    Conn connect(std::string host, uint16_t port, int timeoutMS) {
        detail::ensureInit();

        const auto started = std::chrono::steady_clock::now();

        // Resolve the address within the same overall deadline as connect().
        sockaddr_in addr{};
        detail::ResolveStatus resolveStatus = detail::resolveIPv4(host, port, timeoutMS, addr);
        if (resolveStatus != detail::ResolveStatus::SUCCESS) {
            if (resolveStatus == detail::ResolveStatus::TIMEOUT) {
                throw std::runtime_error("Host name resolution timed out");
            }
            throw std::runtime_error("Could not resolve host");
        }

        // Create a socket only after resolution, so a resolver setup failure
        // cannot leak the handle.
        Socket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (sock == INVALID_SOCKET) {
#else
        if (sock < 0) {
#endif
            throw std::runtime_error("Could not create socket");
        }

        if (timeoutMS < 0) {
            if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                int connectError = detail::lastOsError();
                closeSocket(sock);
                throw std::runtime_error("Could not connect to host: " + detail::osErrorString(connectError));
            }
            return Conn(new ConnClass(sock));
        }

        // Make the handshake non-blocking, so an unreachable server cannot
        // hold the GUI thread until the OS-level TCP retry timeout expires.
#ifdef _WIN32
        u_long nonBlocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) {
            closeSocket(sock);
            throw std::runtime_error("Could not configure non-blocking socket");
        }
#else
        int oldFlags = fcntl(sock, F_GETFL, 0);
        if (oldFlags < 0 || fcntl(sock, F_SETFL, oldFlags | O_NONBLOCK) < 0) {
            closeSocket(sock);
            throw std::runtime_error("Could not configure non-blocking socket");
        }
#endif

        int remainingMS = detail::remainingTimeoutMs(started, timeoutMS);
        if (remainingMS <= 0) {
            closeSocket(sock);
            throw std::runtime_error("Connection timed out");
        }

        int connectionError = 0;
        switch (detail::finishNonblockingConnect(sock, addr, remainingMS, connectionError)) {
        case detail::ConnectStatus::TIMEOUT:
            closeSocket(sock);
            throw std::runtime_error("Connection timed out");
        case detail::ConnectStatus::FAILED:
            closeSocket(sock);
            throw std::runtime_error("Could not connect to host: " + detail::osErrorString(connectionError));
        case detail::ConnectStatus::SUCCESS:
            break;
        }

        // ConnClass expects a regular blocking stream socket for its workers.
#ifdef _WIN32
        nonBlocking = 0;
        if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) {
            closeSocket(sock);
            throw std::runtime_error("Could not restore blocking socket mode");
        }
#else
        if (fcntl(sock, F_SETFL, oldFlags) < 0) {
            closeSocket(sock);
            throw std::runtime_error("Could not restore blocking socket mode");
        }
#endif

        return Conn(new ConnClass(sock));
    }

    Listener listen(std::string host, uint16_t port) {
        Socket listenSock;

        detail::ensureInit();

        // Create a socket
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock < 0) {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

#ifndef _WIN32
        // Allow port reusing if the app was killed or crashed
        // and the socket is stuck in TIME_WAIT state.
        // This option has a different meaning on Windows,
        // so we use it only for non-Windows systems
        int enable = 1;
        if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
            closeSocket(listenSock);
            throw std::runtime_error("Could not configure socket");
            return NULL;
        }
#endif

        // Get address from hostname/ip
        hostent* remoteHost = gethostbyname(host.c_str());
        if (remoteHost == NULL || remoteHost->h_addr_list[0] == NULL) {
            closeSocket(listenSock);
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t* naddr = (uint32_t*)remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = *naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Bind socket
        if (bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            closeSocket(listenSock);
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        // Listen
        if (::listen(listenSock, SOMAXCONN) != 0) {
            closeSocket(listenSock);
            throw std::runtime_error("Could not listen");
            return NULL;
        }

        return Listener(new ListenerClass(listenSock));
    }

    Conn openUDP(std::string host, uint16_t port, std::string remoteHost, uint16_t remotePort, bool bindSocket) {
        Socket sock;

        detail::ensureInit();

        // Create a socket
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            throw std::runtime_error("Could not create socket");
            return NULL;
        }

        // Get address from local hostname/ip
        hostent* _host = gethostbyname(host.c_str());
        if (_host == NULL || _host->h_addr_list[0] == NULL) {
            closeSocket(sock);
            throw std::runtime_error("Could get address from host");
            return NULL;
        }

        // Get address from remote hostname/ip
        hostent* _remoteHost = gethostbyname(remoteHost.c_str());
        if (_remoteHost == NULL || _remoteHost->h_addr_list[0] == NULL) {
            closeSocket(sock);
            throw std::runtime_error("Could get address from host");
            return NULL;
        }
        uint32_t* rnaddr = (uint32_t*)_remoteHost->h_addr_list[0];

        // Create host address
        struct sockaddr_in addr;
        addr.sin_addr.s_addr = INADDR_ANY; //*naddr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Create remote host address
        struct sockaddr_in raddr;
        raddr.sin_addr.s_addr = *rnaddr;
        raddr.sin_family = AF_INET;
        raddr.sin_port = htons(remotePort);

        // Bind socket
        if (bindSocket) {
            int err = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
            if (err < 0) {
                closeSocket(sock);
                throw std::runtime_error("Could not bind socket");
                return NULL;
            }
        }

        return Conn(new ConnClass(sock, raddr, true));
    }
}
