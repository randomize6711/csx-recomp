// The server's side of the server browser (docs/pc_multiplayer.md "N3"; src/net/query.h, master.h): a listen or
// dedicated server answers information queries on its game port and, when given master servers, registers with them.
//   - queries arrive on the server endpoint's own socket (the transport hands connectionless packets here); the
//     answer is built from what the engine shows (refreshed once a second on the game thread, never per query):
//     the hostname, the map (sv.name), the active client_t entries split into players and bots (fakeclient), the
//     slots (maxclients), sv_password set or not, Player Collision, friendly fire, round time, time limit, rounds;
//     the players query adds their names, scores (edict v.frags), pings and connection times. No secret: no password,
//     no key but the public identity, no address but the server's own;
//   - a server bound to one address (a numbered test instance, CSX_NET_BIND) does not receive broadcasts on it: a LAN
//     listener beside it on all addresses (SO_REUSEADDR on both) takes the broadcast queries; the answer goes from the
//     server's own socket, so the browser lists the address it will connect to;
//   - masters: csx-server's "master" setting (CSX_SERVER_MASTER) or, for a listen server, PC settings
//     net_master_register 1 and net_master; names resolved off the game thread; a LAN-only server registers nowhere;
//   - the dedicated server waits for its next tick on the socket (ServerQueryWait), so an answer leaves when the
//     query arrives and the browser's ping is the network's, not the tick's.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/master.h"
#include "net/query.h"
#include "net/transport.h"

namespace {
using kernel::R32;

constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010;
constexpr uint32_t kSvActive = 0x5EF7C0, kSvName = 0x5EF7F0;
constexpr uint32_t kRealtime = 0x6CE040;    // realtime (double)
constexpr uint32_t kSvPassword = 0x3797A4;  // cvar_t: string at +4
constexpr uint32_t kCvarFindVar = 0x41F20;  // Cvar_FindVar (cdecl: name) -> cvar_t*
constexpr uint32_t kNaIp = 3;

std::unique_ptr<net::QueryServer> g_query;
std::unique_ptr<net::Registrar> g_registrar;
std::unique_ptr<net::DatagramSocket> g_lan;  // the LAN listener beside a server bound to one address
net::Endpoint* g_ep = nullptr;
std::mutex g_masters_lock;
std::vector<net::Address> g_masters;  // resolved off the game thread
bool g_masters_ready = false;
double g_info_at = -1e9;

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::string GuestString(uint32_t va, size_t max) {
    std::string s;
    for (size_t i = 0; va && i < max; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

// a cvar's address by name (Cvar_FindVar, its name copied below the guest stack), cached: cvars never move
uint32_t FindCvar(X86Context& ctx, const char* name) {
    static std::vector<std::pair<std::string, uint32_t>> cache;
    for (auto& [n, a] : cache)
        if (n == name) return a;
    uint32_t esp = ctx.esp, len = uint32_t(std::strlen(name));
    uint32_t str = (esp - 0x400 - len) & ~15u;
    std::memcpy(kernel::HostPtr(str), name, len + 1);
    ctx.esp = str - 16;
    uint32_t cvar = kernel::CallGuest(ctx, kCvarFindVar, {str});
    ctx.esp = esp;
    if (cvar) cache.push_back({name, cvar});
    return cvar;
}

float CvarValue(X86Context& ctx, const char* name) {
    uint32_t c = FindCvar(ctx, name);
    float v = 0;
    if (c) std::memcpy(&v, kernel::HostPtr(c + 0xC), 4);
    return std::isfinite(v) ? v : 0;
}

// the server's name: this build registers no "hostname" cvar (the command line's +hostname is dropped), so it comes
// from csx-server's settings (CSX_SERVER_SETTINGS "name ..."), or for a listen server from PC setting
// net_server_name, else its host player's name; a local server the game started for its match (N4.7) is named as that
// game's listen server was (its PC settings are the game's)
std::string ServerName(const std::string& host_player) {
    const char* s = std::getenv("CSX_SERVER_OWNER_KEY") ? nullptr : std::getenv("CSX_SERVER_SETTINGS");
    if (s) {  // "key value" lines
        std::string all = s;
        size_t at = all.rfind("name ", 0) == 0 ? 0 : all.find("\nname ");
        if (at != std::string::npos) {
            at += all[at] == '\n' ? 6 : 5;
            return all.substr(at, all.find('\n', at) - at);
        }
    }
    std::string name = pc::Get("net_server_name", "");
    if (!name.empty()) return name;
    return host_player.empty() ? "Counter-Strike Xbox PC" : host_player + "'s match";
}

uint16_t Clamp16(float v) { return uint16_t(std::lround(std::fmin(std::fmax(v, 0.f), 65535.f))); }

// what the engine shows now
void Refresh(X86Context& ctx) {
    net::ServerInfo info;
    std::vector<net::PlayerInfo> players;
    uint32_t n = R32(kMaxClients);
    double realtime;
    std::memcpy(&realtime, kernel::HostPtr(kRealtime), 8);
    std::string first_name;
    for (uint32_t i = 0; i < n && n <= 255; i++) {
        uint32_t cl = R32(kClients) + i * kClientSize;
        if (!R32(cl)) continue;
        net::PlayerInfo p;
        p.name = GuestString(cl + 0x4D10, 32);
        p.bot = R32(cl + 0x2548) != 0;
        uint32_t edict = R32(cl + 0x4B9C);
        float frags = 0, connected = 0, latency = 0;
        if (edict) std::memcpy(&frags, kernel::HostPtr(edict + 0x1E4), 4);
        std::memcpy(&connected, kernel::HostPtr(cl + 0x40), 4);
        std::memcpy(&latency, kernel::HostPtr(cl + 0x25A0), 4);
        p.score = int16_t(std::isfinite(frags) ? std::fmax(-32768.f, std::fmin(32767.f, frags)) : 0);
        p.seconds = uint32_t(std::fmax(0.0, realtime - connected));
        p.ping = p.bot || R32(cl + 0x24) != kNaIp ? 0 : Clamp16(latency * 1000);
        (p.bot ? info.bots : info.humans)++;
        if (first_name.empty() && !p.bot) first_name = p.name;
        players.push_back(std::move(p));
    }
    info.max_players = uint8_t(n <= 255 ? n : 255);
    info.map = R32(kSvActive) ? GuestString(kSvName, 64) : "";
    info.name = ServerName(first_name);
    std::string pw = GuestString(R32(kSvPassword + 4), 64);
    info.password = !pw.empty() && pw != "none";
    info.dedicated = csx::Dedicated();
    info.collision = csx::PlayerCollisionOn();
    info.friendly_fire = CvarValue(ctx, "mp_friendlyfire") != 0;
    info.round_seconds = Clamp16(CvarValue(ctx, "mp_roundtime") * 60);
    info.time_limit = Clamp16(CvarValue(ctx, "mp_timelimit"));
    info.max_rounds = Clamp16(CvarValue(ctx, "mp_maxrounds"));
    const char* lan = std::getenv("CSX_NET_LAN_ONLY");
    info.lan_only = lan ? *lan == '1' : pc::Get("net_lan_only", "0") == "1";
    if (info.Total() > info.max_players) info.max_players = uint8_t(std::min<int>(255, info.Total()));
    g_query->Set(std::move(info), std::move(players));
}

void Send(const net::Address& to, const std::vector<uint8_t>& d) {
    if (g_ep) g_ep->SendDatagram(to, d.data(), d.size());
}

void Connectionless(const net::Address& from, net::Type type, const uint8_t* p, size_t n) {
    if (g_registrar && g_registrar->Handle(from, type, p, n, Send)) return;
    if (type != net::Type::Query || !g_query) return;
    auto a = g_query->Answer(from, p, n, Now());
    if (!a.empty()) Send(from, a);
}

void PumpLan() {
    if (!g_lan) return;
    uint8_t buf[2048];
    net::Address from;
    for (int i = 0; i < 256; i++) {
        int n = g_lan->Receive(from, buf, sizeof(buf));
        if (n < 0) break;
        if (size_t(n) > net::kPrefix && buf[5] == uint8_t(net::Type::Query)) Connectionless(from, net::Type::Query, buf, size_t(n));
    }
}

// the master servers named in the settings, resolved on a thread of their own (a name's lookup can take seconds)
void ResolveMasters(const std::string& list) {
    std::thread([list] {
        std::vector<net::Address> out;
        size_t at = 0;
        while (at <= list.size()) {
            size_t end = list.find_first_of(", ", at);
            std::string item = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!item.empty()) {
                if (auto a = net::Address::Parse(item, net::kMasterPort)) out.push_back(*a);
                else LOGW("net: the master server %s cannot be resolved", item.c_str());
            }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        std::lock_guard<std::mutex> lock(g_masters_lock);
        g_masters = std::move(out);
        g_masters_ready = true;
    }).detach();
}

}  // namespace

namespace csx {

void ServerQueryStart(net::Endpoint& ep, const net::Identity& id, bool bound_to_one) {
    g_ep = &ep;
    // a server on this machine's loopback only (an offline match's local server: N4.7, the party's Local Only, N4.8)
    // is found by no one: no LAN discovery listener (which would answer on every address), no master server, no
    // queries answered; its local players join it by address with its key pinned
    if (const char* lb = std::getenv("CSX_SERVER_LOOPBACK"); lb && *lb == '1') {
        LOGI("net: a server on this machine's loopback only: not announced, no queries answered");
        return;
    }
    net::QueryServer::Config qc;
    const char* lan = std::getenv("CSX_NET_LAN_ONLY");
    qc.lan_only = lan ? *lan == '1' : pc::Get("net_lan_only", "0") == "1";
    g_query = std::make_unique<net::QueryServer>(qc, id);
    ep.SetConnectionless(Connectionless);
    if (bound_to_one) {
        std::string error;
        g_lan = net::OpenUdp(ep.Local().port, error, nullptr, false, true);
        if (g_lan) LOGI("net: LAN discovery listener on UDP port %u (all addresses)", ep.Local().port);
        else LOGW("net: no LAN discovery listener: %s", error.c_str());
    }
    std::string masters;
    if (const char* m = std::getenv("CSX_SERVER_MASTER")) masters = m;
    else if (pc::Get("net_master_register", "0") == "1") masters = pc::Get("net_master", "");
    if (!masters.empty() && qc.lan_only) {
        LOGI("net: a LAN-only server registers with no master server");
    } else if (!masters.empty()) {
        g_registrar = std::make_unique<net::Registrar>(id, std::vector<net::Address>{}, Now);
        g_registrar->log = [](const std::string& s) { LOGI("net: %s", s.c_str()); };
        ResolveMasters(masters);
        LOGI("net: master servers %s", masters.c_str());
    }
    LOGI("net: answering server queries on UDP port %u", ep.Local().port);
    // split-screen player 1 hosts (offline against bots, Host a Match): the other local players join it here
    if (csx::SplitPlayer() == 1) {
        net::Address a = ep.Local();
        if (a.ip == std::array<uint8_t, 4>{0, 0, 0, 0}) a.ip = {127, 0, 0, 1};
        csx::SplitSend(csx::kSplitMatch, "host " + a.ToString() + " " + net::Identity::Fingerprint(id.public_key));
    }
}

void ServerQueryStop() {
    if (g_registrar) g_registrar->Leave(Send);
    g_registrar.reset();
    g_query.reset();
    g_lan.reset();
    g_ep = nullptr;
}

// the game thread, at every receive of the server socket: the information refreshed once a second, the LAN listener
// read, the registration's heartbeats
void ServerQueryFrame(X86Context& ctx) {
    if (!g_query) return;
    double now = Now();
    if (now - g_info_at >= 1.0) {
        g_info_at = now;
        Refresh(ctx);
    }
    PumpLan();
    if (g_registrar) {
        {
            std::lock_guard<std::mutex> lock(g_masters_lock);
            if (g_masters_ready) {
                g_registrar->SetMasters(g_masters);
                g_masters_ready = false;
            }
        }
        g_registrar->Pump(Send);
    }
}

// the dedicated server's wait for its next tick: on its socket, answering queries as they arrive
void ServerQueryWait(double seconds) {
    if (!g_ep) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        return;
    }
    double end = Now() + seconds;
    for (double left = seconds; left > 0; left = end - Now()) {
        g_ep->Wait(g_lan ? std::min(left, 0.002) : left);
        g_ep->Pump();
        PumpLan();
    }
}

}  // namespace csx
