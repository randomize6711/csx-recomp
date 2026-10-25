// PC sessions in the engine (docs/pc_multiplayer.md, N1): the parts of the Xbox lobby the engine's own connection
// went through, taken out of its path.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/transport.h"

// [[hook]] at 0x29D3F in CL_Connect_f (0x29CD0): unless the argument is "local", the engine copied the address the
// system link lobby resolved (0x554334) instead of its argument, and an empty one left the server name empty, so
// `connect host:port` sent nothing. The argument (edx) is used: true goes to the argument's copy at 0x29D67.
namespace {
bool g_connecting = false;   // CL_Connect_f is closing the old connection before the new one
bool g_lobby_off = false;    // the system link lobby was not set up (a PC direct connect)
bool g_joined = false;       // this process's client connected to a network server (and so loaded its world)
std::string g_reconnect;     // a server that relaunched into another map: the client relaunches to rejoin it
constexpr uint32_t kRelaunch = 0xBF0E0;  // the title's relaunch (cdecl: the launch data), as 0x5F750 calls it

// the client relaunches into `+connect server` (the title's own way into a match, as the system link join did with
// "+connect 0.0.0.0"): its map comes from the server like any join; the title's texture and model tables are those
// of a fresh boot (an engine map change without a relaunch filled the 1500 texture slots: FINDINGS.md)
[[noreturn]] void RelaunchToServer(X86Context& ctx, const std::string& server) {
    std::string line = "+connect " + server;
    for (const char* drive : {"Z:\\releasemap.txt", "T:\\releasemap.txt"}) {
        std::string path = kernel::ResolvePath(drive, 0);
        if (path.empty()) continue;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(line.c_str(), std::streamsize(line.size() + 1));
    }
    LOGI("net: relaunching to rejoin %s", server.c_str());
    uint32_t data = guest_tables::kTables[guest_tables::k_net_launch].va;
    std::memset(kernel::HostPtr(data), 0, 0xC00);
    kernel::W32(data, 4);
    kernel::CallGuest(ctx, kRelaunch, {data});
    std::abort();  // the relaunch does not return
}
}  // namespace

namespace csx {
// the dedicated server's map change (admin map): the title relaunched into "+map NAME" as its own map change does;
// RelaunchNotify tells the players to rejoin, csx-server rebuilds the match line with the new map
void RelaunchToMap(X86Context& ctx, const std::string& map) {
    std::string line = "+map " + map;
    if (std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0); !path.empty()) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(line.c_str(), std::streamsize(line.size() + 1));
    }
    LOGI("net: relaunching into %s", map.c_str());
    uint32_t data = guest_tables::kTables[guest_tables::k_net_launch].va;
    std::memset(kernel::HostPtr(data), 0, 0xC00);
    kernel::W32(data, 4);
    kernel::CallGuest(ctx, kRelaunch, {data});
    std::abort();
}
}  // namespace csx

bool ClConnectAddress(X86Context& ctx, uint8_t*) {
    std::string target = kernel::ReadCString(ctx.edx, 240);
    // an address without a port gets the PC port (the engine's default was 1001, the Xbox's)
    if (target.find(':') == std::string::npos && !target.empty()) {
        target += ":" + std::to_string(csx::NetServerPort());
        uint32_t buf = guest_tables::kTables[guest_tables::k_net_connect].va;
        std::memcpy(kernel::HostPtr(buf), target.c_str(), target.size() + 1);
        ctx.edx = buf;
    }
    LOGI("net: connect %s", target.c_str());
    g_connecting = true;
    return true;
}

// [[hook]] at 0x29D22 in CL_Connect_f, its argument in edx (only ebx pushed): a process loads one world. A second one
// in the same process (a connect while connected, a retry, a connect after a disconnect) ran the GL texture table on
// past its end: the map's sprites came back under new names (the same name with other dimensions is renamed at
// 0x4897F, "sprites" to "sprjtes") and their records overwrote com_argv (0x7482E4), crashing COM_CheckParm at the next
// signon (net_reconnect). The map change already relaunches for the same reason; so does this connect: the client
// relaunches into `+connect ADDRESS` between frames (HostFrameLobby), its connection closed by the relaunch. True skips
// the connect (0x29E89, its exit with ebx pushed). "local" (the listen server's own client) is left alone.
bool ClConnectRelaunch(X86Context& ctx, uint8_t*) {
    if (!g_joined) return false;
    std::string target = kernel::ReadCString(ctx.edx, 240);
    if (target.empty() || target == "local") return false;
    LOGI("net: connect %s in a process that already joined a server: relaunching to join it", target.c_str());
    g_reconnect = target;
    return true;
}

namespace csx {
bool NetClientOnly() {
    return g_lobby_off;
}

void NetServerJoined() {
    g_joined = true;
}

void NetServerClosed(const std::string& peer, int reason, const std::string& text) {
    if (net::Reason(reason) == net::Reason::MapChange) {
        LOGI("net: %s changes map (%s): rejoining after it", peer.c_str(), text.c_str());
        g_reconnect = peer;
    }
}
}  // namespace csx

// [[hook]] at 0x29019 in CL_Disconnect, after it sent "dropclient": the client's transport connection is closed too
// (the server is told at once instead of timing it out)
void ClDisconnectTransport(X86Context&, uint8_t*) {
    csx::NetClientClose();
}

// [[hook]] at 0xE7040, the front end's map change (cdecl: the map name), which relaunches the host into the next
// map: the clients are told first, so they rejoin (MapChange) instead of timing out
void MapChangeNotify(X86Context& ctx, uint8_t*) {
    std::string map = kernel::ReadCString(kernel::R32(ctx.esp + 4), 64);
    LOGI("net: map change to %s: the clients are told", map.c_str());
    csx::NetDisconnectServer(int(net::Reason::MapChange), map);
}

// [[hook]] at 0xBF0E0, the title's relaunch: every remaining connection ends. A map change is a relaunch into
// "+map NAME" (whichever way the match got there: the game DLL's changelevel, the engine's map command at the end of
// a match), written to Z:\releasemap.txt just before: the clients are told MapChange and rejoin; anything else (a
// quit, a new match from the menus) is a shutdown
void RelaunchNotify(X86Context&, uint8_t*) {
    std::string line;
    if (std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0); !path.empty()) {
        std::ifstream f(path, std::ios::binary);
        std::getline(f, line, '\0');
    }
    if (line.rfind("+map ", 0) == 0) {
        std::string map = line.substr(5, line.find(' ', 5) == std::string::npos ? std::string::npos : line.find(' ', 5) - 5);
        LOGI("net: relaunching into %s: the clients are told to rejoin", map.c_str());
        csx::NetDisconnectServer(int(net::Reason::MapChange), map);
    } else {
        csx::NetDisconnectServer(int(net::Reason::ServerShutdown), "server shut down");
    }
    csx::NetClientClose();
}

// [[hook]] at 0x290B3 in CL_Disconnect (0x28F20): a client that was connected opens the Xbox "LostHost" screen
// (0xE9320), whose own render loop never returns unless the title relaunches. The disconnect that `connect` makes
// first is not a lost host (also when Host_ShutdownServer calls CL_Disconnect again inside it): true skips the
// screen (0x290BA). A dedicated server has no client to lose its host: the map command's own CL_Disconnect
// (0x5FD50) at its start would open the screen and never return.
bool ClDisconnectLostHost(X86Context&, uint8_t*) {
    return g_connecting || !g_reconnect.empty() || csx::Dedicated();
}

// [[hook]] at 0x29DA7 in CL_Connect_f, after its CL_Disconnect (which Host_ShutdownServer re-enters)
void ClConnectDisconnected(X86Context&, uint8_t*) {
    g_connecting = false;
}

// [[hook]] at 0xADA36 in the title's boot (0xAD8A0): a command line without "+map" set the system link lobby up as a
// joining client (0xF0640), which searches for the session it rejoins and, not finding one, opens "LostHost" with its
// own render loop for good. A PC direct connect (`+connect host:port`, not the lobby's "+connect 0.0.0.0") needs no
// lobby: true skips the setup (0xADA40) and the lobby stays idle. The command line is at esp+0x6C.
bool BootLobbyJoin(X86Context& ctx, uint8_t*) {
    uint32_t line = kernel::R32(ctx.esp + 0x6C);
    if (!line) return false;
    std::string s = kernel::ReadCString(line, 1024);
    bool direct = s.find("+connect ") != std::string::npos && s.find("0.0.0.0") == std::string::npos;
    if (direct) LOGI("net: direct connect at boot, the system link lobby not set up");
    g_lobby_off = direct;
    return direct;
}

// [[hook]] at 0x5D5D7 in Host_Frame (0x5D5D0): the lobby's per-frame update (0xE9C00 on 0x554208) is skipped when the
// lobby was not set up; it would wait in accept() on a lobby socket nobody opened for listening (true: 0x5D5E1)
bool HostFrameLobby(X86Context& ctx, uint8_t*) {
    if (!g_reconnect.empty()) RelaunchToServer(ctx, g_reconnect);  // between frames: no engine code is mid-way
    static uint32_t paused = 0;
    if (kernel::R32(0x5EF7C4) != paused) {
        paused = kernel::R32(0x5EF7C4);
        LOGI("net: sv.paused %u", paused);
    }
    return g_lobby_off;
}

// [[hook]] at 0xED270, the lobby's message send (thiscall, 3 arguments, ret 0xC): without a lobby there is no peer;
// on a lobby TCP socket nobody connected it ended the process with SIGPIPE (a team choice sends one). True returns at
// once (the ret 0xC at 0xED2F8) with eax 0.
bool LobbySend(X86Context& ctx, uint8_t*) {
    if (!g_lobby_off) return false;
    ctx.eax = 0;
    return true;
}

// [[hook]] at 0x407C5 in Con_Printf (0x40790), after its vsnprintf: with CSX_LOG_CONSOLE=1 every console line goes
// to the log too (the text at esp+0x10), for connection diagnostics
void ConPrintfLog(X86Context& ctx, uint8_t*) {
    static const bool on = [] {
        const char* e = std::getenv("CSX_LOG_CONSOLE");
        return e && *e == '1';
    }();
    if (!on) return;
    std::string text = kernel::ReadCString(ctx.esp + 0x10, 0x1000);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!text.empty()) LOGI("console: %s", text.c_str());
}

// [[hook]] at 0xCA017, 0xCA07B and 0xCD913: the front end pauses the server while one of its screens is open (the
// pause menu, the spectator screen after the host's player dies), as the console's single player and listen server
// did: Cbuf_AddText("pause\n") (0x33DB98). With players on the network that froze the match for all of them (their
// connection timed out): the request is skipped while the server has network clients (true jumps past the call)
bool UiPause(X86Context&, uint8_t*) {
    if (!csx::NetServerHasClients()) return false;
    LOG_FIRST(4, "net: the menu's pause skipped: players are connected over the network");
    return true;
}
