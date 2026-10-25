// The engine's UDP sockets on the PC transport (src/net/transport.h, docs/pc_multiplayer.md "N1"). The engine keeps
// its netchan, connectionless packets and loopback; what it sent through its own sockets (NET_SendPacket ->
// NET_SendLong -> sendto, recvfrom -> NET_GetLong) goes through a transport endpoint instead:
//   UDP_OpenSocket (0x6CDE0)  replaced: the server socket is a server endpoint on the PC port (27015 unless set), the
//                             client socket a client endpoint on any port; the engine gets a handle standing for it
//   NET_SendPacket (0x6CC70)  hooked after its loopback branch: packets to an IP address go to the endpoint
//   0x6C340                   replaced: the next packet the endpoint delivered, into in_message / in_from
//   NET_CloseIP (0x6D3C6)     the endpoint closed (its peers told) instead of closesocket
//   NET_GetLocalAddress (0x6D28C) the endpoint's port instead of getsockname
// The engine's split packets (0xFFFFFFFE) are no longer used: the transport fragments, up to 64 KB.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "common/csx.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/transport.h"

namespace {
using kernel::R16;
using kernel::R32;
using kernel::W16;
using kernel::W32;

constexpr uint32_t kHandle = 0x7C5A0000;   // + the engine socket (0 client, 1 server): not a real socket
constexpr uint32_t kSockets = 0x551E20;    // ip_sockets[NS_CLIENT, NS_SERVER]
constexpr uint32_t kInMessage = 0x6A95A0;  // sizebuf: data +8, maxsize +0xC, cursize +0x10
constexpr uint32_t kInFrom = 0x6BA5A0;     // netadr_t: type +0, ip +4, port +0x12 (network order)
constexpr uint32_t kConPrintf = 0x40790;
constexpr uint32_t kServerOpenReturn = 0x6D049;  // UDP_OpenSocket's call for the server socket returns here
constexpr uint32_t kNaBroadcast = 2, kNaIp = 3;

std::unique_ptr<net::Endpoint> g_ep[2];

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

net::Address FromNetadr(uint32_t adr) {
    net::Address a;
    std::memcpy(a.ip.data(), kernel::HostPtr(adr + 4), 4);
    uint16_t p = R16(adr + 0x12);
    a.port = uint16_t(p >> 8 | p << 8);
    return a;
}

void ToNetadr(uint32_t adr, const net::Address& a) {
    std::memset(kernel::HostPtr(adr), 0, 0x14);
    W32(adr, kNaIp);
    std::memcpy(kernel::HostPtr(adr + 4), a.ip.data(), 4);
    W16(adr + 0x12, uint16_t(a.port >> 8 | a.port << 8));
}

void Print(X86Context& ctx, const std::string& text) {
    uint32_t esp = ctx.esp;
    uint32_t str = (esp - 0x400 - uint32_t(text.size())) & ~15u, fmt = str - 16;
    std::memcpy(kernel::HostPtr(str), text.c_str(), text.size() + 1);
    std::memcpy(kernel::HostPtr(fmt), "%s\n", 4);
    ctx.esp = fmt - 16;
    kernel::CallGuest(ctx, kConPrintf, {fmt, str});
    ctx.esp = esp;
}

net::Config MakeConfig() {
    net::Config c;
    c.require_identity = pc::Get("net_require_identity", "0") == "1";
    return c;
}

std::optional<net::Identity> LoadIdentity() {
    if (pc::Get("net_identity", "1") != "1") return std::nullopt;
    std::string path = (app::ExeDir() / "identity.key").string();
    std::string error;
    auto id = net::Identity::LoadOrCreate(path, error);
    if (!id) {
        LOGW("net: no player identity (%s)", error.c_str());
        return std::nullopt;
    }
    LOGI("net: player identity %s (%s)", net::Identity::Fingerprint(id->public_key).c_str(), path.c_str());
    return id;
}

// CSX_LOG_NET=1: once a second, what each endpoint sent and received in it, its connections, fragmentation, loss and
// round trip, and how long the server took to build and send its clients' messages (SV_SendClientMessages)
struct SendTiming {
    double start = 0, total = 0, worst = 0;
    unsigned calls = 0;
} g_send;

void LogStats() {
    static const bool on = std::getenv("CSX_LOG_NET") && *std::getenv("CSX_LOG_NET") == '1';
    if (!on) return;
    static double last = Now();
    static net::Stats before[2];
    double now = Now();
    if (now - last < 1.0) return;
    double dt = now - last;
    last = now;
    for (int i = 1; i >= 0; i--) {
        if (!g_ep[i]) continue;
        const net::Stats& a = g_ep[i]->GetStats();
        net::Stats& b = before[i];
        LOGI("net stats: %s %zu connections, out %.1f KB/s in %llu datagrams (%llu messages, %llu fragmented, largest %llu "
             "bytes), in %.1f KB/s in %llu datagrams; lost %llu, replays %llu, auth %llu, malformed %llu, reassembly "
             "timeouts %llu",
             i ? "server" : "client", g_ep[i]->ConnectionCount(), (a.bytes_out - b.bytes_out) / 1024.0 / dt,
             (unsigned long long)(a.datagrams_out - b.datagrams_out), (unsigned long long)(a.messages_out - b.messages_out),
             (unsigned long long)(a.fragmented_out - b.fragmented_out), (unsigned long long)a.largest_message_out,
             (a.bytes_in - b.bytes_in) / 1024.0 / dt, (unsigned long long)(a.datagrams_in - b.datagrams_in),
             (unsigned long long)(a.lost_in - b.lost_in), (unsigned long long)(a.dropped_replay - b.dropped_replay),
             (unsigned long long)(a.dropped_auth - b.dropped_auth), (unsigned long long)(a.dropped_malformed - b.dropped_malformed),
             (unsigned long long)(a.reassembly_timeouts - b.reassembly_timeouts));
        b = a;
    }
    if (g_send.calls) {
        LOGI("net stats: SV_SendClientMessages %u calls, %.3f ms each on average, %.3f ms at most", g_send.calls,
             g_send.total / g_send.calls * 1000, g_send.worst * 1000);
        g_send = SendTiming{};
    }
}

// connection events: logged, shown in the console, and acted on (net_session.cpp: a map change reconnects)
void Events(X86Context& ctx, int sock) {
    net::Endpoint::Event e;
    while (g_ep[sock]->PollEvent(e)) {
        std::string who = e.peer.ToString();
        if (e.kind == net::Endpoint::Event::Connected) {
            LOGI("net: %s %s connected%s%s", sock ? "client" : "server", who.c_str(),
                 e.identity.empty() ? "" : ", identity ", e.identity.c_str());
            if (sock == 0) csx::NetServerJoined();
            continue;
        }
        // the reason only when the text does not already say it (a peer's Disconnect carries its own text)
        std::string reason = net::ReasonText(e.reason);
        auto same = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        };
        std::string detail = same(e.text, reason) ? "" : " (" + reason + ")";
        if (e.remote_closed && reason.find("peer") == std::string::npos) detail += " by the peer";
        LOGI("net: %s %s closed: %s%s", sock ? "client" : "server", who.c_str(), e.text.c_str(), detail.c_str());
        if (sock == 0) {
            Print(ctx, "Disconnected from " + who + ": " + e.text);
            csx::NetServerClosed(who, int(e.reason), e.text);
        }
    }
}

}  // namespace

namespace csx {

uint16_t NetServerPort() {
    if (const char* e = std::getenv("CSX_NET_PORT")) {
        int p = std::atoi(e);
        if (p > 0 && p < 65536) return uint16_t(p);
    }
    int p = std::atoi(pc::Get("net_port", "27015").c_str());
    return p > 0 && p < 65536 ? uint16_t(p) : 27015;
}

bool NetHosting() {
    if (NetClientOnly()) return false;
    auto on = [](const char* e) { return e && *e == '1'; };
    return on(std::getenv("CSX_NET_HOST")) || pc::Get("net_host", "0") == "1" || app::Instance() > 0 ||
           std::getenv("CSX_NET_BIND") != nullptr;
}

void StartNetwork() {
    // an exit tells the peers (a closed window, the end of a test) instead of leaving them to time out
    std::atexit([] {
        for (auto& e : g_ep)
            if (e) e->DisconnectAll(e->GetRole() == net::Endpoint::Role::Server ? net::Reason::ServerShutdown
                                                                                : net::Reason::Closed);
    });
#ifndef _WIN32
    // a write to a closed socket returns an error instead of ending the process (the window build reports it)
    struct sigaction old {};
    if (sigaction(SIGPIPE, nullptr, &old) == 0 && old.sa_handler == SIG_DFL) std::signal(SIGPIPE, SIG_IGN);
#endif
}

bool NetConnected(const std::string& peer) {
    auto a = net::Address::Parse(peer, NetServerPort());
    return a && g_ep[0] && g_ep[0]->Connected(*a);
}

bool NetServerHasClients() {
    return g_ep[1] && g_ep[1]->ConnectionCount() > 0;
}

void NetDisconnectServer(int reason, const std::string& text) {
    if (g_ep[1]) g_ep[1]->DisconnectAll(net::Reason(reason), text);
}

void NetDisconnectClient(const std::string& peer, int reason, const std::string& text) {
    if (!g_ep[1]) return;
    if (auto a = net::Address::Parse(peer, NetServerPort())) g_ep[1]->Disconnect(*a, net::Reason(reason), text);
}

void NetClientClose() {
    if (g_ep[0]) g_ep[0]->DisconnectAll(net::Reason::Closed);
}

std::optional<std::string> NetPeerIdentity(const std::string& peer) {
    auto a = net::Address::Parse(peer, NetServerPort());
    if (!a || !g_ep[1]) return std::nullopt;
    return g_ep[1]->PeerIdentity(*a);
}

void NetPump() {
    for (auto& e : g_ep)
        if (e) e->Pump();
}

}  // namespace csx

// UDP_OpenSocket (0x6CDE0, cdecl: the bind address text, the port, reuse): a transport endpoint; the handle stands for
// it in ip_sockets. The server's is on the PC port (net_port, CSX_NET_PORT; 27015), the client's on any free port.
X86_FUNC(sub_0006CDE0) {
    (void)base;
    int sock = R32(ctx.esp) == kServerOpenReturn ? 1 : 0;
    ctx.eax = 0;
    if (sock == 1 && !csx::NetHosting()) {
        // a match takes network players only when asked (--host, PC setting net_host, CSX_NET_HOST=1; the tests'
        // numbered instances): no port is opened otherwise, and a process started to join a server hosts nothing
        LOG_FIRST(1, "net: %s, no server endpoint", csx::NetClientOnly() ? "client only" : "not hosting");
        ctx.esp += 4;
        return;
    }
    if (!g_ep[sock]) {
        uint16_t port = sock ? csx::NetServerPort() : 0;
        std::string error;
        // a numbered instance (--instance N, the multi-process tests) binds 127.0.0.N as its system link does
        // (CSX_NET_BIND=a.b.c.d instead: the tests' many clients past the 8 numbered instances)
        net::Address local;
        local.ip = {127, 0, 0, uint8_t(app::Instance())};
        bool bind = app::Instance() > 0;
        if (const char* b = std::getenv("CSX_NET_BIND"))
            if (auto a = net::Address::Parse(b, 0)) local = *a, bind = true;
        auto s = net::OpenUdp(port, error, bind ? &local : nullptr);
        if (!s) {
            LOGE("net: cannot open the %s socket: %s", sock ? "server" : "client", error.c_str());
        } else {
            LOGI("net: %s endpoint on UDP port %u", sock ? "server" : "client", s->Local().port);
            g_ep[sock] = std::make_unique<net::Endpoint>(sock ? net::Endpoint::Role::Server : net::Endpoint::Role::Client,
                                                         std::move(s), MakeConfig(), Now);
            if (!sock) {
                g_ep[sock]->SetIdentity(LoadIdentity());
                // PC rates (the engine's were 2003's: rate 4000 bytes a second, 20 updates and 30 commands a second);
                // userinfo cvars, so a server learns them at connect or by the engine's setinfo after
                std::string rates = "rate " + pc::Get("net_rate", "100000") + "\ncl_updaterate " +
                                    pc::Get("net_update_rate", "30") + "\ncl_cmdrate " + pc::Get("net_cmd_rate", "60") + "\n";
                csx::QueueEngineCommand(rates);
            }
        }
    }
    if (g_ep[sock]) ctx.eax = kHandle + uint32_t(sock);
    ctx.esp += 4;
}

// 0x6C340 (cdecl: the engine socket): the next packet for it, into in_message and in_from; 1, or 0 for none
X86_FUNC(sub_0006C340) {
    (void)base;
    uint32_t sock = R32(ctx.esp + 4);
    uint32_t result = 0;
    if (sock <= 1 && g_ep[sock] && R32(kSockets + 4 * sock) == kHandle + sock) {
        Events(ctx, int(sock));
        LogStats();
        net::Address from;
        std::vector<uint8_t> data;
        while (g_ep[sock]->Receive(from, data)) {
            uint32_t dst = R32(kInMessage + 8), max = R32(kInMessage + 0xC);
            if (data.size() > max) {
                LOG_FIRST(16, "net: a %zu byte packet from %s dropped (the engine's message holds %u)", data.size(),
                          from.ToString().c_str(), max);
                continue;
            }
            std::memcpy(kernel::HostPtr(dst), data.data(), data.size());
            W32(kInMessage + 0x10, uint32_t(data.size()));
            ToNetadr(kInFrom, from);
            result = 1;
            break;
        }
    }
    ctx.eax = result;
    ctx.esp += 4;
}

// [[hook]] at 0x6CCCC in NET_SendPacket (0x6CC70), after its loopback branch (ebx the engine socket, ebp the address
// type, edi the data, the length at esp+0x24, the netadr_t at esp+0x28): the transport sends it; true returns
// through the function's end (0x6CDC6)
bool NetSendTransport(X86Context& ctx, uint8_t*) {
    uint32_t sock = ctx.ebx;
    if (sock > 1 || !g_ep[sock] || R32(kSockets + 4 * sock) != kHandle + sock) return false;
    uint32_t len = R32(ctx.esp + 0x24);
    if (ctx.ebp == kNaBroadcast) {
        LOG_FIRST(4, "net: a broadcast of %u bytes not sent (no LAN broadcast on the PC transport yet)", len);
        return true;
    }
    if (ctx.ebp != kNaIp) return false;  // the engine's own error path
    net::Address to = FromNetadr(ctx.esp + 0x28);
    if (!g_ep[sock]->Send(to, static_cast<const uint8_t*>(kernel::HostPtr(ctx.edi)), len))
        LOG_FIRST(32, "net: a %u byte packet to %s not sent (%s)", len, to.ToString().c_str(),
                  sock ? "no connection from that address" : "too large, the connect queue full, or the server just closed us");
    return true;
}

// [[hook]] at 0x6D3C6 in NET_CloseIP (esi the ip_sockets entry, eax its handle): an endpoint is closed with its peers
// told (the server's as shutting down); true skips the closesocket (0x6D3CC clears the entry)
bool NetCloseSocket(X86Context& ctx, uint8_t*) {
    for (uint32_t sock = 0; sock < 2; sock++) {
        if (ctx.eax != kHandle + sock) continue;
        if (g_ep[sock]) {
            g_ep[sock]->DisconnectAll(sock ? net::Reason::ServerShutdown : net::Reason::Closed);
            g_ep[sock].reset();
            LOGI("net: %s endpoint closed", sock ? "server" : "client");
        }
        return true;
    }
    return false;
}

// [[hook]] at 0x6D28C in NET_GetLocalAddress, its getsockname(server socket, &addr, &length) with the three arguments
// pushed: the server endpoint's port into addr (else the engine marks TCP/IP disabled and later opens no client
// socket); true continues at 0x6D291 with eax 0 (success) and the arguments popped as the stdcall would
bool NetLocalAddress(X86Context& ctx, uint8_t*) {
    uint32_t handle = R32(ctx.esp);
    if (!(handle == kHandle + 1 && g_ep[1]) && !(handle == 0 && g_ep[0])) return false;
    uint16_t port = handle ? g_ep[1]->Local().port : 0;  // a client-only process has no server port
    W16(R32(ctx.esp + 4) + 2, uint16_t(port >> 8 | port << 8));
    ctx.esp += 12;
    ctx.eax = 0;
    return true;
}

// SV_ClampRate (0x9C1E0, ecx the client, its netchan rate a double at +0x48): sv_maxrate caps the client's rate,
// sv_minrate raises it, each when set (above 0); the engine also capped sv_maxrate itself at 20000 bytes a second,
// which a PC link outgrows: the cap is the client's own limit now (1000000, config/net_patches.toml)
X86_FUNC(sub_0009C1E0) {
    (void)base;
    constexpr uint32_t kMaxRateValue = 0x37968C + 0xC, kMinRateValue = 0x379678 + 0xC;
    float maxr, minr;
    std::memcpy(&maxr, kernel::HostPtr(kMaxRateValue), 4);
    std::memcpy(&minr, kernel::HostPtr(kMinRateValue), 4);
    double rate;
    std::memcpy(&rate, kernel::HostPtr(ctx.ecx + 0x48), 8);
    if (maxr > 0 && maxr < rate) rate = std::min<double>(maxr, 1000000.0);
    if (minr > 0 && minr > rate) rate = std::max<double>(minr, 1000.0);
    std::memcpy(kernel::HostPtr(ctx.ecx + 0x48), &rate, 8);
    ctx.esp += 4;
}

// [[hook]] at 0x9E3F0 (SV_SendClientMessages' entry) and 0x9E6B6 (its tail jump out): its time, for CSX_LOG_NET
void NetSendBegin(X86Context&, uint8_t*) {
    g_send.start = Now();
}
void NetSendEnd(X86Context&, uint8_t*) {
    double d = Now() - g_send.start;
    g_send.total += d;
    g_send.worst = std::max(g_send.worst, d);
    g_send.calls++;
}
