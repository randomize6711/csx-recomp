#include "netaddr.h"

#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace tool {

std::vector<LocalAddress> LocalAddresses() {
    std::vector<LocalAddress> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a,
                             &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 a, &size) != NO_ERROR)
            a = nullptr;
    }
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
            out.push_back({ip, name, a->IfType == IF_TYPE_SOFTWARE_LOOPBACK});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof(ip));
            out.push_back({ip, i->ifa_name ? i->ifa_name : "", (i->ifa_flags & IFF_LOOPBACK) != 0});
        }
        freeifaddrs(list);
    }
#endif
    std::stable_sort(out.begin(), out.end(), [](const LocalAddress& a, const LocalAddress& b) { return !a.loopback && b.loopback; });
    return out;
}

}  // namespace tool
