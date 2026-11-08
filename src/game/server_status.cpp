// The dedicated server's telemetry for the server tool (docs/server_tool.md "Telemetry format"): with
// CSX_SERVER_TELEMETRY=1, once a second, one line on stdout:
//   @csx status {JSON}
// The tool is csx-server's parent and the only reader of its stdout, so this is a local channel, never a port. The
// values are the engine's and the game's own, read on the game thread at the host frame's start (DedicatedTick):
//   - the slots: svs.clients (0x636CE4, 0x5010 bytes each), active +0, the address +0x20 (type +4: 3 NA_IP), the
//     connection time +0x40, fakeclient +0x2548, latency +0x25A0 (seconds), the edict +0x4B9C, the name +0x4D10;
//   - the player (edict +0x7C, the game's CBasePlayer): its team +0x1E0 (1 T, 2 CT, 3 spectator, 0 none) and deaths
//     +0x724; its score is entvars.frags (edict +0x80 +0x164). These are what the game's ScoreInfo message sends
//     (0x1489BC..0x1489ED: WRITE_SHORT of pev->frags, m_iDeaths +0x724, 0, m_iTeam +0x1E0);
//   - the map (sv.name 0x5EF7F0, sv.active 0x5EF7C0), the password (sv_password 0x3797A4), the transport's totals.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "common/csx.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"

std::string PcRotationNextMap();  // pc_modes.cpp

namespace {
using kernel::R16;
using kernel::R32;

constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010;
constexpr uint32_t kSvActive = 0x5EF7C0, kSvName = 0x5EF7F0;
constexpr uint32_t kRealtime = 0x6CE040;    // realtime (double)
constexpr uint32_t kSvPassword = 0x3797A4;  // cvar_t: string at +4
constexpr uint32_t kNaIp = 3;
constexpr uint32_t kTeam = 0x1E0, kDeaths = 0x724, kFrags = 0x80 + 0x164;

void Escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20 || c >= 0x7F) {  // names are the engine's bytes: anything else as \u00XX
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else out += char(c);
    }
    out += '"';
}

std::string Fixed(double v, int decimals) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

std::string Setting(const char* key) {  // csx-server's effective setting (CSX_SERVER_SETTINGS "key value" lines)
    const char* s = std::getenv("CSX_SERVER_SETTINGS");
    if (!s) return {};
    std::string all = std::string("\n") + s, k = std::string("\n") + key + " ";
    size_t at = all.find(k);
    if (at == std::string::npos) return {};
    at += k.size();
    return all.substr(at, all.find('\n', at) - at);
}

std::string Address(uint32_t cl) {
    const uint8_t* ip = static_cast<const uint8_t*>(kernel::HostPtr(cl + 0x24 + 4));
    uint16_t p = R16(cl + 0x24 + 0x12);
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]) + ":" + std::to_string(uint16_t(p >> 8 | p << 8));
}

float F32(uint32_t va) {
    float v;
    std::memcpy(&v, kernel::HostPtr(va), 4);
    return std::isfinite(v) ? v : 0.f;
}

const char* TeamName(uint32_t team) {
    switch (team) {
    case 1: return "T";
    case 2: return "CT";
    case 3: return "SPEC";
    default: return "-";
    }
}

}  // namespace

namespace csx {

void ServerTelemetry(X86Context&) {
    static const bool on = std::getenv("CSX_SERVER_TELEMETRY") && *std::getenv("CSX_SERVER_TELEMETRY") == '1';
    if (!on) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static uint64_t frames_before = ServerFrames();
    static double ms_sum = 0, ms_max = 0;
    static unsigned ms_n = 0;
    static uint64_t in_before = 0, out_before = 0;
    static bool have_before = false;
    double ms = ServerFrameMs();
    ms_sum += ms, ms_max = std::fmax(ms_max, ms), ms_n++;
    auto now = clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    if (dt < 1.0) return;
    last = now;
    uint64_t frames = ServerFrames();
    double tick_hz = double(frames - frames_before) / dt;
    frames_before = frames;

    std::string j = "{\"v\":1";
    bool active = R32(kSvActive) != 0;
    j += std::string(",\"state\":\"") + (active ? "running" : "loading") + "\"";
    std::time_t t = std::time(nullptr);
    j += ",\"time\":" + std::to_string((long long)t);
    const char* started = std::getenv("CSX_SERVER_STARTED");
    double uptime = started ? double(t) - std::atof(started) : 0;
    j += ",\"uptime\":" + Fixed(std::fmax(0, uptime), 0);
    j += ",\"map\":";
    Escape(j, active ? kernel::ReadCString(kSvName, 64) : std::string());
    std::string next = PcRotationNextMap();
    j += ",\"next_map\":";
    if (next.empty()) j += "null";
    else Escape(j, next);
    j += ",\"frames\":" + std::to_string(frames);
    j += ",\"tick_hz\":" + Fixed(tick_hz, 1);
    j += ",\"frame_ms\":" + Fixed(ms_n ? ms_sum / ms_n : 0, 2) + ",\"frame_ms_max\":" + Fixed(ms_max, 2);
    ms_sum = ms_max = 0, ms_n = 0;

    uint32_t n = R32(kMaxClients);
    if (n > 256) n = 0;
    double realtime;
    std::memcpy(&realtime, kernel::HostPtr(kRealtime), 8);
    int humans = 0, bots = 0;
    std::string players = "[";
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cl = R32(kClients) + i * kClientSize;
        if (!R32(cl)) continue;
        bool bot = R32(cl + 0x2548) != 0;
        bool network = R32(cl + 0x24) == kNaIp;
        (bot ? bots : humans)++;
        uint32_t edict = R32(cl + 0x4B9C);
        uint32_t player = edict ? R32(edict + 0x7C) : 0;
        if (players.size() > 1) players += ",";
        players += "{\"slot\":" + std::to_string(i + 1) + ",\"name\":";
        Escape(players, kernel::ReadCString(cl + 0x4D10, 32));
        players += std::string(",\"team\":\"") + (player ? TeamName(R32(player + kTeam)) : "-") + "\"";
        players += ",\"score\":" + std::to_string(edict ? int(std::lround(F32(edict + kFrags))) : 0);
        players += ",\"deaths\":" + std::to_string(player ? int32_t(R32(player + kDeaths)) : 0);
        players += ",\"ping\":" + (bot || !network ? std::string("null")
                                                   : std::to_string(int(std::lround(F32(cl + 0x25A0) * 1000))));
        players += std::string(",\"bot\":") + (bot ? "true" : "false");
        if (network) {
            std::string addr = Address(cl);
            players += ",\"address\":";
            Escape(players, addr);
            auto key = NetPeerIdentity(addr);
            players += ",\"key\":";
            if (key) Escape(players, *key);
            else players += "null";
        }
        players += ",\"seconds\":" + Fixed(std::fmax(0.0, realtime - double(F32(cl + 0x40))), 0) + "}";
    }
    players += "]";
    j += ",\"max_players\":" + std::to_string(n) + ",\"humans\":" + std::to_string(humans) +
         ",\"bots\":" + std::to_string(bots);
    j += ",\"port\":" + std::to_string(ServerEndpointPort());
    j += ",\"key\":";
    Escape(j, ServerIdentityKey());
    std::string pw = R32(kSvPassword + 4) ? kernel::ReadCString(R32(kSvPassword + 4), 64) : std::string();
    j += std::string(",\"password\":") + (!pw.empty() && pw != "none" ? "true" : "false");
    const char* lan = std::getenv("CSX_NET_LAN_ONLY");
    j += std::string(",\"lan_only\":") + (lan && *lan == '1' ? "true" : "false");
    j += ",\"game_mode\":";
    Escape(j, Setting("game_mode"));
    uint64_t in = 0, out = 0;
    size_t conns = 0;
    if (ServerNetTotals(in, out, conns)) {
        j += ",\"net\":{\"connections\":" + std::to_string(conns);
        j += ",\"in_bps\":" + (have_before ? Fixed(double(in - in_before) / dt, 0) : std::string("null"));
        j += ",\"out_bps\":" + (have_before ? Fixed(double(out - out_before) / dt, 0) : std::string("null"));
        j += ",\"in_total\":" + std::to_string(in) + ",\"out_total\":" + std::to_string(out) + "}";
        in_before = in, out_before = out, have_before = true;
    } else {
        j += ",\"net\":null";
    }
    j += ",\"players\":" + players + "}";
    std::string line = "@csx status " + j + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);  // a closed pipe (the tool gone) is an error here, not a signal (SIGPIPE is ignored)
}

}  // namespace csx
