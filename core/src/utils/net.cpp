#include "net.h"
#include "net_connect.h"
#include "net_resolve.h"
#include <string.h>
#include <codecvt>
#include <stdexcept>
#include <chrono>

#ifdef _WIN32
#define WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <net/if.h>
#include <sys/ioctl.h>
#if defined(__ANDROID__) && __ANDROID_API__ < 24
#include <dlfcn.h>
#endif
#define WOULD_BLOCK (errno == EWOULDBLOCK)
#endif

namespace net {
    // === Private functions ===

    static SockHandle_t invalidSocket() {
#ifdef _WIN32
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

    static bool validSocket(SockHandle_t sock) {
#ifdef _WIN32
        return sock != INVALID_SOCKET;
#else
        return sock >= 0;
#endif
    }

    void init() {
        detail::ensureInit();
    }

    void closeSocket(SockHandle_t sock) {
        if (!validSocket(sock)) { return; }
#ifdef _WIN32
        shutdown(sock, SD_BOTH);
        closesocket(sock);
#else
        shutdown(sock, SHUT_RDWR);
        close(sock);
#endif
    }

    bool setNonblocking(SockHandle_t sock) {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(sock, FIONBIO, &enabled) == 0;
#else
        int flags = fcntl(sock, F_GETFL, 0);
        return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

#ifndef _WIN32
    IP_t ipv4FromSockaddr(const sockaddr* addr) {
        return ntohl(((const sockaddr_in*)addr)->sin_addr.s_addr);
    }
#endif

    // === Address functions ===

    Address::Address() {
        memset(&addr, 0, sizeof(addr));
    }

    Address::Address(const std::string& host, int port) {
        // Initialize WSA if needed
        init();
        
        // Lookup host
        hostent* ent = gethostbyname(host.c_str());
        if (!ent || !ent->h_addr_list[0]) {
            throw std::runtime_error("Unknown host");
        }
        
        // Build address
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = *(uint32_t*)ent->h_addr_list[0];
        addr.sin_port = htons(port);
    }

    Address::Address(IP_t ip, int port) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);
    }

    std::string Address::getIPStr() const {
        char buf[128];
        IP_t ip = getIP();
        sprintf(buf, "%d.%d.%d.%d", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        return buf;
    }

    IP_t Address::getIP() const {
        return htonl(addr.sin_addr.s_addr);
    }

    void Address::setIP(IP_t ip) {
        addr.sin_addr.s_addr = htonl(ip);
    }

    int Address::getPort() const {
        return htons(addr.sin_port);
    }

    void Address::setPort(int port) {
        addr.sin_port = htons(port);
    }

    // === Socket functions ===

    Socket::Socket(SockHandle_t sock, const Address* raddr) {
        this->sock = sock;
        if (raddr) {
            this->raddr = new Address(*raddr);
        }
    }

    Socket::~Socket() {
        close();
        if (raddr) { delete raddr; }
    }

    void Socket::close() {
        // Every error path and the destructor funnels through here. Taking
        // the handle exactly once prevents a concurrent recv/send failure and
        // an external stop from closing a recycled native descriptor twice.
        closeSocket(sock.exchange(invalidSocket()));
    }

    bool Socket::isOpen() {
        return validSocket(sock.load());
    }

    SocketType Socket::type() {
        return raddr ? SOCKET_TYPE_UDP : SOCKET_TYPE_TCP;
    }

    int Socket::send(const uint8_t* data, size_t len, const Address* dest) {
        SockHandle_t handle = sock.load();
        if (!validSocket(handle)) { return -1; }

        // Send data
        int err = sendto(handle, (const char*)data, len, 0, (sockaddr*)(dest ? &dest->addr : (raddr ? &raddr->addr : NULL)), sizeof(sockaddr_in));

        // On error, close socket
        if (err <= 0 && !WOULD_BLOCK) {
            close();
            return err;
        }

        return err;
    }

    int Socket::sendstr(const std::string& str, const Address* dest) {
        return send((const uint8_t*)str.c_str(), str.length(), dest);
    }

    int Socket::recv(uint8_t* data, size_t maxLen, bool forceLen, int timeout, Address* dest) {
        SockHandle_t handle = sock.load();
        if (!validSocket(handle)) { return -1; }

        // Create FD set
        fd_set set;
        FD_ZERO(&set);
        
        int read = 0;
        bool blocking = (timeout != NONBLOCKING);
        do {
            // Wait for data or error if 
            if (blocking) {
                // Enable FD in set
                FD_SET(handle, &set);

                // Set timeout
                timeval tv;
                tv.tv_sec = timeout / 1000;
                tv.tv_usec = (timeout - tv.tv_sec*1000) * 1000;

                // Wait for data
                int err = select((int)(handle + 1), &set, NULL, &set, (timeout > 0) ? &tv : NULL);
                if (err <= 0) { return err; }
            }

            // Receive
            int addrLen = sizeof(sockaddr_in);
            int err = ::recvfrom(handle, (char*)&data[read], maxLen - read, 0,(sockaddr*)(dest ? &dest->addr : NULL), (socklen_t*)(dest ? &addrLen : NULL));
            if (err <= 0 && !WOULD_BLOCK) {
                close();
                return err;
            }
            read += err;
        }
        while (blocking && forceLen && read < maxLen);
        return read;
    }

    int Socket::recvline(std::string& str, int maxLen, int timeout, Address* dest) {
        // Disallow nonblocking mode
        if (!timeout) { return -1; }
        
        str.clear();
        int read = 0;
        while (!maxLen || read < maxLen) {
            char c;
            int err = recv((uint8_t*)&c, 1, false, timeout, dest);
            if (err <= 0) { return err; }
            read++;
            if (c == '\n') { break; }
            str += c;
        }
        return read;
    }

    // === Listener functions ===

    Listener::Listener(SockHandle_t sock) {
        this->sock = sock;
    }

    Listener::~Listener() {
        stop();
    }

    void Listener::stop() {
        closeSocket(sock.exchange(invalidSocket()));
    }

    bool Listener::listening() {
        return validSocket(sock.load());
    }

    std::shared_ptr<Socket> Listener::accept(Address* dest, int timeout) {
        SockHandle_t handle = sock.load();
        if (!validSocket(handle)) { return NULL; }

        // Create FD set
        fd_set set;
        FD_ZERO(&set);
        FD_SET(handle, &set);

        // Define timeout
        timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout - tv.tv_sec*1000) * 1000;

        // Wait for data or error
        if (timeout != NONBLOCKING) {
            int err = select((int)(handle + 1), &set, NULL, &set, (timeout > 0) ? &tv : NULL);
            if (err <= 0) { return NULL; }
        }

        // Accept
        int addrLen = sizeof(sockaddr_in);
        SockHandle_t s = ::accept(handle, (sockaddr*)(dest ? &dest->addr : NULL), (socklen_t*)(dest ? &addrLen : NULL));
        if ((int)s < 0) {
            if (!WOULD_BLOCK) { stop(); }
            return NULL;
        }

        // Enable nonblocking mode
        if (!setNonblocking(s)) {
            closeSocket(s);
            throw std::runtime_error("Could not configure non-blocking socket");
        }

        return std::make_shared<Socket>(s);
    }

    // === Creation functions ===

    std::map<std::string, InterfaceInfo> listInterfaces() {
        // Init library if needed
        init();

        std::map<std::string, InterfaceInfo> ifaces;
#ifdef _WIN32
        // Pre-allocate buffer
        ULONG size = sizeof(IP_ADAPTER_ADDRESSES);
        PIP_ADAPTER_ADDRESSES addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

        // Reallocate to real size
        if (GetAdaptersAddresses(AF_INET, 0, NULL, addresses, &size) == ERROR_BUFFER_OVERFLOW) {
            addresses = (PIP_ADAPTER_ADDRESSES)realloc(addresses, size);
            if (GetAdaptersAddresses(AF_INET, 0, NULL, addresses, &size)) {
                throw std::exception("Could not list network interfaces");
            }
        }

        // Save data
        std::wstring_convert<std::codecvt_utf8<wchar_t>> utfConv;
        for (auto iface = addresses; iface; iface = iface->Next) {
            InterfaceInfo info;
            auto ip = iface->FirstUnicastAddress;
            if (!ip || ip->Address.lpSockaddr->sa_family != AF_INET) { continue; }
            info.address = ntohl(*(uint32_t*)&ip->Address.lpSockaddr->sa_data[2]);
            info.netmask = ~((1 << (32 - ip->OnLinkPrefixLength)) - 1);
            info.broadcast = info.address | (~info.netmask);
            ifaces[utfConv.to_bytes(iface->FriendlyName)] = info;
        }
        
        // Free tables
        free(addresses);
#else
#if defined(__ANDROID__) && __ANDROID_API__ < 24
        // Resolve getifaddrs/freeifaddrs at runtime (available on API 24+ devices)
        using getifaddrs_t  = int  (*)(struct ifaddrs**);
        using freeifaddrs_t = void (*)(struct ifaddrs*);
        static auto dyn_getifaddrs  = (getifaddrs_t)dlsym(RTLD_DEFAULT, "getifaddrs");
        static auto dyn_freeifaddrs = (freeifaddrs_t)dlsym(RTLD_DEFAULT, "freeifaddrs");

        if (dyn_getifaddrs && dyn_freeifaddrs) {
            // Device has getifaddrs — full enumeration
            struct ifaddrs* addresses = NULL;
            dyn_getifaddrs(&addresses);
            for (auto iface = addresses; iface; iface = iface->ifa_next) {
                if (!iface->ifa_addr || !iface->ifa_netmask) { continue; }
                if (iface->ifa_addr->sa_family != AF_INET) { continue; }
                InterfaceInfo info;
                info.address = ipv4FromSockaddr(iface->ifa_addr);
                info.netmask = ipv4FromSockaddr(iface->ifa_netmask);
                info.broadcast = info.address | (~info.netmask);
                ifaces[iface->ifa_name] = info;
            }
            dyn_freeifaddrs(addresses);
        } else {
            // ioctl fallback — IPv4 only (API 23 devices)
            SockHandle_t querySock = socket(AF_INET, SOCK_DGRAM, 0);
            if (querySock >= 0) {
                char buffer[16384];
                struct ifconf ifacesConf;
                memset(&ifacesConf, 0, sizeof(ifacesConf));
                ifacesConf.ifc_len = sizeof(buffer);
                ifacesConf.ifc_buf = buffer;

                if (ioctl(querySock, SIOCGIFCONF, &ifacesConf) == 0) {
                    for (char* ptr = buffer; ptr < (buffer + ifacesConf.ifc_len); ptr += sizeof(struct ifreq)) {
                        auto* iface = (struct ifreq*)ptr;
                        if (iface->ifr_addr.sa_family != AF_INET) { continue; }

                        struct ifreq netmaskReq;
                        memset(&netmaskReq, 0, sizeof(netmaskReq));
                        strncpy(netmaskReq.ifr_name, iface->ifr_name, IFNAMSIZ - 1);
                        if (ioctl(querySock, SIOCGIFNETMASK, &netmaskReq) != 0 || netmaskReq.ifr_netmask.sa_family != AF_INET) { continue; }

                        InterfaceInfo info;
                        info.address = ipv4FromSockaddr(&iface->ifr_addr);
                        info.netmask = ipv4FromSockaddr(&netmaskReq.ifr_netmask);
                        info.broadcast = info.address | (~info.netmask);
                        ifaces[iface->ifr_name] = info;
                    }
                }

                ::close(querySock);
            }
        }
#else
        // Get iface list
        struct ifaddrs* addresses = NULL;
        getifaddrs(&addresses);

        // Save data
        for (auto iface = addresses; iface; iface = iface->ifa_next) {
            if (!iface->ifa_addr || !iface->ifa_netmask) { continue; }
            if (iface->ifa_addr->sa_family != AF_INET) { continue; }
            InterfaceInfo info;
            info.address = ipv4FromSockaddr(iface->ifa_addr);
            info.netmask = ipv4FromSockaddr(iface->ifa_netmask);
            info.broadcast = info.address | (~info.netmask);
            ifaces[iface->ifa_name] = info;
        }

        // Free iface list
        freeifaddrs(addresses);
#endif
#endif

        return ifaces;
    }

    std::shared_ptr<Listener> listen(const Address& addr) {
        // Init library if needed
        init();

        // Create socket
        SockHandle_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        // TODO: Support non-blockign mode

#ifndef _WIN32
        // Allow port reusing if the app was killed or crashed
        // and the socket is stuck in TIME_WAIT state.
        // This option has a different meaning on Windows,
        // so we use it only for non-Windows systems
        int enable = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
            closeSocket(s);
            throw std::runtime_error("Could not configure socket");
            return NULL;
        }
#endif

        // Bind socket to the port
        if (bind(s, (sockaddr*)&addr.addr, sizeof(sockaddr_in))) {
            closeSocket(s);
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        // Enable listening
        if (::listen(s, SOMAXCONN) != 0) {
            throw std::runtime_error("Could start listening for connections");
            return NULL;
        }

        // Enable nonblocking mode
        if (!setNonblocking(s)) {
            closeSocket(s);
            throw std::runtime_error("Could not configure non-blocking socket");
        }

        // Return listener class
        return std::make_shared<Listener>(s);
    }

    std::shared_ptr<Listener> listen(std::string host, int port) {
        return listen(Address(host, port));
    }

    std::shared_ptr<Socket> connect(const Address& addr) {
        return connect(addr, NO_TIMEOUT);
    }

    std::shared_ptr<Socket> connect(const Address& addr, int timeout) {
        // Init library if needed
        init();

        // Create socket
        SockHandle_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (s == INVALID_SOCKET) {
#else
        if (s < 0) {
#endif
            throw std::runtime_error("Could not create socket: " + detail::osErrorString(detail::lastOsError()));
        }

        if (timeout < 0) {
            // Connect to server, blocking until the OS-level timeout
            if (::connect(s, (sockaddr*)&addr.addr, sizeof(sockaddr_in))) {
                int connectError = detail::lastOsError();
                closeSocket(s);
                throw std::runtime_error("Could not connect: " + detail::osErrorString(connectError));
            }
            if (!setNonblocking(s)) {
                closeSocket(s);
                throw std::runtime_error("Could not configure non-blocking socket");
            }
            return std::make_shared<Socket>(s);
        }

        // Bounded connect: run the connect in nonblocking mode and wait for
        // the result ourselves instead of letting the OS block the caller for
        // its own multi-tens-of-seconds timeout (issue #1462). The socket
        // stays nonblocking, as Socket expects.
        if (!setNonblocking(s)) {
            closeSocket(s);
            throw std::runtime_error("Could not configure non-blocking socket");
        }
        int connectionError = 0;
        switch (detail::finishNonblockingConnect(s, addr.addr, timeout, connectionError)) {
        case detail::ConnectStatus::TIMEOUT:
            closeSocket(s);
            throw std::runtime_error("Connection timed out");
        case detail::ConnectStatus::FAILED:
            closeSocket(s);
            throw std::runtime_error("Could not connect: " + detail::osErrorString(connectionError));
        case detail::ConnectStatus::SUCCESS:
            break;
        }

        // Return socket class
        return std::make_shared<Socket>(s);
    }

    std::shared_ptr<Socket> connect(std::string host, int port) {
        return connect(std::move(host), port, NO_TIMEOUT);
    }

    std::shared_ptr<Socket> connect(std::string host, int port, int timeout) {
        init();

        if (timeout < 0) {
            return connect(Address(host, port), timeout);
        }

        const auto started = std::chrono::steady_clock::now();
        sockaddr_in resolved{};
        detail::ResolveStatus resolveStatus = detail::resolveIPv4(host, port, timeout, resolved);
        if (resolveStatus == detail::ResolveStatus::TIMEOUT) {
            throw std::runtime_error("Host name resolution timed out");
        }
        if (resolveStatus != detail::ResolveStatus::SUCCESS) {
            throw std::runtime_error("Unknown host");
        }

        int remainingMS = detail::remainingTimeoutMs(started, timeout);
        if (remainingMS <= 0) {
            throw std::runtime_error("Connection timed out");
        }

        Address address;
        address.addr = resolved;
        return connect(address, remainingMS);
    }

    std::shared_ptr<Socket> openudp(const Address& raddr, const Address& laddr, bool allowBroadcast) {
        // Init library if needed
        init();

        // Create socket
        SockHandle_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        // If the remote address is multicast, allow multicast connections
        #ifdef _WIN32
                const char enable = allowBroadcast;
        #else
                int enable = allowBroadcast;
        #endif
        if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(int)) < 0) {
            closeSocket(s);
            throw std::runtime_error("Could not enable broadcast on socket");
            return NULL;
        }

        // Bind socket to local port
        if (bind(s, (sockaddr*)&laddr.addr, sizeof(sockaddr_in))) {
            closeSocket(s);
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }
        
        // Return socket class
        return std::make_shared<Socket>(s, &raddr);
    }

    std::shared_ptr<Socket> openudp(std::string rhost, int rport, const Address& laddr, bool allowBroadcast) {
        return openudp(Address(rhost, rport), laddr, allowBroadcast);
    }

    std::shared_ptr<Socket> openudp(const Address& raddr, std::string lhost, int lport, bool allowBroadcast) {
        return openudp(raddr, Address(lhost, lport), allowBroadcast);
    }

    std::shared_ptr<Socket> openudp(std::string rhost, int rport, std::string lhost, int lport, bool allowBroadcast) {
        return openudp(Address(rhost, rport), Address(lhost, lport), allowBroadcast);
    }
}
