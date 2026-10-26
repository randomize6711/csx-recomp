// The transport's UDP socket (transport.h DatagramSocket) on the host's own socket API: POSIX on Linux, Winsock on
// Windows. Non-blocking, IPv4, close-on-exec (a title relaunch execs the process image: the socket must not outlive
// it and hold the port, FINDINGS.md "PC multiplayer investigation").
#include "net/browser.h"
#include "net/transport.h"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {
namespace {

#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle kInvalid = INVALID_SOCKET;
void CloseHandle_(Handle s) { closesocket(s); }
std::string LastError() { return "error " + std::to_string(WSAGetLastError()); }
bool StartSockets() {
    static const bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}
#else
using Handle = int;
constexpr Handle kInvalid = -1;
void CloseHandle_(Handle s) { ::close(s); }
std::string LastError() { return std::strerror(errno); }
bool StartSockets() { return true; }
#endif

sockaddr_in ToSockaddr(const Address& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    std::memcpy(&s.sin_addr, a.ip.data(), 4);
    s.sin_port = htons(a.port);
    return s;
}

Address FromSockaddr(const sockaddr_in& s) {
    Address a;
    std::memcpy(a.ip.data(), &s.sin_addr, 4);
    a.port = ntohs(s.sin_port);
    return a;
}

class UdpSocket : public DatagramSocket {
public:
    explicit UdpSocket(Handle s) : s_(s) {
        sockaddr_in me{};
        socklen_t len = sizeof(me);
        if (getsockname(s_, reinterpret_cast<sockaddr*>(&me), &len) == 0) local_ = FromSockaddr(me);
    }
    ~UdpSocket() override { CloseHandle_(s_); }
    bool Send(const Address& to, const uint8_t* data, size_t len) override {
        sockaddr_in d = ToSockaddr(to);
#ifdef _WIN32
        int n = sendto(s_, reinterpret_cast<const char*>(data), int(len), 0, reinterpret_cast<sockaddr*>(&d), sizeof(d));
#else
        ssize_t n = sendto(s_, data, len, MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&d), sizeof(d));
#endif
        return n == ssize_t(len);
    }
    int Receive(Address& from, uint8_t* buf, size_t max) override {
        for (;;) {
            sockaddr_in src{};
            socklen_t len = sizeof(src);
#ifdef _WIN32
            int n = recvfrom(s_, reinterpret_cast<char*>(buf), int(max), 0, reinterpret_cast<sockaddr*>(&src), &len);
            if (n < 0) {
                int e = WSAGetLastError();
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // an ICMP port unreachable, an oversized datagram
                return -1;
            }
#else
            ssize_t n = recvfrom(s_, buf, max, MSG_TRUNC, reinterpret_cast<sockaddr*>(&src), &len);
            if (n < 0) {
                if (errno == EINTR || errno == ECONNREFUSED) continue;
                return -1;
            }
            if (size_t(n) > max) continue;  // larger than any transport datagram: dropped whole
#endif
            if (src.sin_family != AF_INET) continue;
            from = FromSockaddr(src);
            return int(n);
        }
    }
    Address Local() const override { return local_; }
    void Wait(double seconds) override {
        int ms = int(seconds * 1000);
#ifdef _WIN32
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s_, &r);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        select(0, &r, nullptr, nullptr, &tv);
#else
        pollfd p{s_, POLLIN, 0};
        poll(&p, 1, ms);
#endif
    }

private:
    Handle s_;
    Address local_;
};

}  // namespace

std::unique_ptr<DatagramSocket> OpenUdp(uint16_t port, std::string& error, const Address* bind_to, bool broadcast,
                                        bool shared) {
    if (!StartSockets()) {
        error = "sockets unavailable";
        return nullptr;
    }
#ifdef _WIN32
    Handle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
    Handle s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
#endif
    if (s == kInvalid) {
        error = "socket: " + LastError();
        return nullptr;
    }
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    // an ICMP port unreachable must not fail the next receive (SIO_UDP_CONNRESET off)
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    int big = 1 << 20;  // a server sends bursts of fragments to many clients
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    int one = 1;
    if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    if (shared) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_to) std::memcpy(&me.sin_addr, bind_to->ip.data(), 4);
    me.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
        error = "port " + std::to_string(port) + ": " + LastError();
        CloseHandle_(s);
        return nullptr;
    }
    return std::make_unique<UdpSocket>(s);
}

std::vector<Address> LanBroadcastAddresses() {
    std::vector<Address> out;
    auto add = [&](const Address& a) {
        for (const Address& b : out)
            if (b.ip == a.ip) return;
        out.push_back(a);
    };
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
                add(FromSockaddr(*reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)));
        }
        freeifaddrs(list);
    }
#endif
    Address all, loop;
    all.ip = {255, 255, 255, 255};
    loop.ip = {127, 255, 255, 255};  // servers on this machine (the loopback interface has no broadcast flag)
    add(all);
    add(loop);
    return out;
}

std::optional<Address> Address::Parse(const std::string& text, uint16_t default_port) {
    std::string host = text;
    uint16_t port = default_port;
    if (auto colon = text.rfind(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        std::string p = text.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        unsigned long v = std::stoul(p);
        if (v == 0 || v > 65535) return std::nullopt;
        port = uint16_t(v);
    }
    if (host.empty()) return std::nullopt;
    if (!StartSockets()) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
    Address a = FromSockaddr(*reinterpret_cast<sockaddr_in*>(res->ai_addr));
    freeaddrinfo(res);
    a.port = port;
    return a;
}

}  // namespace net
