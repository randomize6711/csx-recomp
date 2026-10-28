// PC sessions in the engine (docs/pc_multiplayer.md, N1): the parts of the Xbox lobby the engine's own connection
// went through, taken out of its path.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/local_server.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/transport.h"
#include "net/wire.h"

// [[hook]] at 0x29D3F in CL_Connect_f (0x29CD0): unless the argument is "local", the engine copied the address the
// system link lobby resolved (0x554334) instead of its argument, and an empty one left the server name empty, so
// `connect host:port` sent nothing. The argument (edx) is used: true goes to the argument's copy at 0x29D67.
namespace {
bool g_connecting = false;   // CL_Connect_f is closing the old connection before the new one
bool g_lobby_off = false;    // the system link lobby was not set up (a PC direct connect)
bool g_joined = false;       // this process's client connected to a network server (and so loaded its world)
std::string g_reconnect;     // a server that relaunched into another map: the client relaunches to rejoin it
std::string g_match_line;    // the party's lobby started a match (N4.8): the title relaunches into it
bool g_front_end = false;    // the party's leader returns to its lobby (N4.8): the title relaunches into the front end
std::string g_close_text;    // why the transport connection to the server ended (its peer's text or the reason)
std::string g_refusal;       // a refusal the server printed (bad password, banned, kicked)
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
    // the game's own local server (N4.7): the launch data its menu match started with, so the game comes back in
    // that match's mode (Single Player's pause menu and pause, not System Link's)
    if (csx::LocalServerRunning()) {
        std::ifstream f(app::ExeDir() / "local_server" / "launch_match.bin", std::ios::binary);
        std::vector<char> page(0xC00);
        if (f.read(page.data(), std::streamsize(page.size()))) std::memcpy(kernel::HostPtr(data), page.data(), page.size());
    }
    kernel::CallGuest(ctx, kRelaunch, {data});
    std::abort();  // the relaunch does not return
}
// back to the front end (a browser join that ended): no match line, the launch data the title's own end of a session
// writes (0xE6370: 1, then the page); the browser reopens with the message (pc_browser.cpp's PcBrowserMenuFrame). The
// front end's launch data (FrontEndLaunch) is also what a menu match whose local server did not start relaunches with
// by_mode: the screen the match was started from, as the title's own end of a session does (0xE6370: the launch data's
// second word from the game mode 0x5259E0, 2 3 4 5 -> 2 3 6 5, or 0x5259E4 when set): Single Player's match options,
// Host a Match's. Without it (0) the front end opens its main screen, or (a match from Single Player that did not
// start) Training
uint32_t FrontEndLaunch(bool by_mode = false, uint32_t screen_now = 0) {
    for (const char* drive : {"Z:\\releasemap.txt", "T:\\releasemap.txt"}) {
        std::string path = kernel::ResolvePath(drive, 0);
        if (!path.empty()) std::remove(path.c_str());
    }
    uint32_t data = guest_tables::kTables[guest_tables::k_net_launch].va;
    std::memset(kernel::HostPtr(data), 0, 0xC00);
    kernel::W32(data, 1);
    if (by_mode) {
        uint32_t mode = kernel::R32(0x5259E0), set = kernel::R32(0x5259E4);
        static const uint32_t kScreen[] = {2, 3, 6, 5};
        uint32_t screen = screen_now ? screen_now : set ? set : mode >= 2 && mode <= 5 ? kScreen[mode - 2] : 0;
        kernel::W32(data + 4, screen);
    }
    return data;
}

// the message the front end shows on its return: the browser's (a join's end), or the local server's (CSX_LOCAL_
// SERVER_MESSAGE: the match's server did not start or stopped; pc_browser.cpp)
void FrontEndMessage(const char* var, const std::string& message) {
#ifdef _WIN32
    _putenv_s(var, message.c_str());
#else
    setenv(var, message.c_str(), 1);
#endif
}

[[noreturn]] void RelaunchToFrontEnd(X86Context& ctx, const std::string& message, const char* var = "CSX_BROWSER_MESSAGE",
                                     bool by_mode = false) {
    uint32_t data = FrontEndLaunch(by_mode);
    if (!message.empty()) FrontEndMessage(var, message);
    LOGI("net: back to the front end%s%s", message.empty() ? "" : ": ", message.c_str());
    kernel::CallGuest(ctx, kRelaunch, {data});
    std::abort();
}

// split-screen player 1 starts a match from the front end (docs/splitscreen.md): its server takes the local players;
// an offline match (Single Player) on this machine's loopback only, Host a Match on the network as alone. Its bot
// count leaves room for four local players whatever the number playing now (4 local players and 61 bots fill the 65
// slots): the title sets its bots at a match's start and never removes one for a joining player (bot_kick, TODO
// 1e3), so a controller's START during the match, or a map change with more players, still finds a slot. With the
// compositor running even one player hosts so (START joins a second); the game alone in its window is unchanged.
void SplitMatchFromMenus(bool host) {
    if (csx::SplitPlayer() != 1) return;
    // a party's match (N4.8) has room for its own players exactly (65 - them for bots); otherwise four
    int kLocal = 4;
    if (const char* p = std::getenv("CSX_PARTY_MATCH"); p && std::atoi(p) >= 1 && std::atoi(p) <= 4) kLocal = std::atoi(p);
    int players = csx::SplitPlayers();
    setenv("CSX_MENU_HOST", "1", 1);
    if (host) unsetenv("CSX_SERVER_LOOPBACK");
    else setenv("CSX_SERVER_LOOPBACK", "1", 1);
    std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0), line;
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        std::getline(f, line, '\0');
    }
    size_t q = line.find("+bot_quota ");
    if (q != std::string::npos) {
        size_t v = q + 11, e = line.find(' ', v);
        int bots = std::atoi(line.substr(v, e - v).c_str()), room = 65 - kLocal;
        if (bots > room) {
            line = line.substr(0, v) + std::to_string(room) + (e == std::string::npos ? "" : line.substr(e));
            std::ofstream(path, std::ios::binary | std::ios::trunc) << line << '\0';
            LOGI("net: split-screen: %d bots asked, %d (65 slots: room for 4 local players, %d now)", bots, room, players);
        }
    }
    LOGI("net: split-screen: the match hosts %d local players (up to 4)%s", players, host ? "" : " (loopback only)");
}

bool BrowserJoin() {
    return std::getenv("CSX_BROWSER_JOIN") != nullptr && !csx::Dedicated();
}

// what the player is told when a browser join ends
std::string ReturnMessage() {
    if (!g_refusal.empty()) return g_refusal;
    if (!g_close_text.empty()) return "Disconnected: " + g_close_text;
    return g_joined ? "The connection to the server was lost." : "The server did not let you join.";
}
}  // namespace

namespace csx {
// the server browser's join (pc_browser.cpp): the title relaunches into it between frames
void RequestJoin(const std::string& server) {
    g_reconnect = server;
}

// the party lobby's Start (pc_lobby.cpp): the title relaunches into its match line between frames
void RequestMatch(const std::string& line) {
    g_match_line = line;
}

void RequestFrontEnd() {
    g_front_end = true;
}

// the party's lobby starts its match (N4.8, pc_lobby.cpp): the title relaunches into the lobby's "+map ..." line from
// a front end screen, as Single Player's Start does (launch data 3: a Single Player match's mode), so RelaunchNotify
// starts its local server and the game joins it (docs/splitscreen.md 17)
[[noreturn]] void RelaunchIntoMatch(X86Context& ctx, const std::string& line) {
    // Z: as the title's own Start writes it (RelaunchNotify turns it into the local server's "+connect"); a T: copy
    // left from a rejoin would be read first
    if (std::string t = kernel::ResolvePath("T:\\releasemap.txt", 0); !t.empty()) std::remove(t.c_str());
    if (std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0); !path.empty()) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(line.c_str(), std::streamsize(line.size() + 1));
    }
    uint32_t data = guest_tables::kTables[guest_tables::k_net_launch].va;
    std::memset(kernel::HostPtr(data), 0, 0xC00);
    kernel::W32(data, 3);
    kernel::CallGuest(ctx, kRelaunch, {data});
    std::abort();
}

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
    if (net::Reason(reason) != net::Reason::MapChange && net::Reason(reason) != net::Reason::Closed) g_close_text = text;
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
void RelaunchNotify(X86Context& ctx, uint8_t*) {
    // a match started from the front end's menus: System Link's (Multiplayer's) Host a Match hosts it for the network
    // (a PC listen server: its port open, LAN discovery answered); any other menu match does not (CSX_MENU_HOST)
    bool menu_match = false, hosted = false;
    if (uint32_t screen = kernel::R32(0x5523D4)) {
        std::string name = kernel::ReadCString(kernel::R32(screen), 64);
        if (name.rfind("SuperLauncher_", 0) == 0) {
            // (the party's lobby, N4.8: its Host LAN Match, CSX_PARTY_HOST; its Local Only is offline)
            bool host = name.rfind("SuperLauncher_LocalServer", 0) == 0 ||
                        (name.rfind("SuperLauncher_PCLobby", 0) == 0 && std::getenv("CSX_PARTY_HOST") &&
                         *std::getenv("CSX_PARTY_HOST") == '1');
#ifdef _WIN32
            _putenv_s("CSX_MENU_HOST", host ? "1" : "");
#else
            if (host) setenv("CSX_MENU_HOST", "1", 1);
            else unsetenv("CSX_MENU_HOST");
#endif
            if (host) LOGI("net: a match hosted from Multiplayer (%s): its server takes network players", name.c_str());
            // a match that is not the party lobby's (N4.8) leaves the party's settings behind
            // (the join password is the party's only when it was the lobby's: a browser join's own is kept)
            if (name.rfind("SuperLauncher_PCLobby", 0) != 0) {
                const char* pp = std::getenv("CSX_PARTY_PASSWORD");
                const char* np = std::getenv("CSX_NET_PASSWORD");
                if (pp && np && *pp && !std::strcmp(pp, np)) unsetenv("CSX_NET_PASSWORD");
                for (const char* v : {"CSX_PARTY_MATCH", "CSX_PARTY_HOST", "CSX_PARTY_PASSWORD", "CSX_PARTY_TEAM", "CSX_PARTY_LOBBY",
                                      "CSX_PARTY_MODE"})
                    unsetenv(v);
            }
            SplitMatchFromMenus(host);
            menu_match = true, hosted = host;
        }
    }
    std::string line;
    if (std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0); !path.empty()) {
        std::ifstream f(path, std::ios::binary);
        std::getline(f, line, '\0');
    }
    // the match's server: this game's local server ends unless the relaunch rejoins it (a map change); a match from
    // the menus runs in a new one (N4.7, local_server.cpp): the game relaunches into "+connect" to it, not "+map"
    // (the pause menu's Quit relaunches with the front end's launch data, type 1 (0xE6370), and the match's line left
    // in the file: the game is leaving whatever the line says)
    uint32_t launch = kernel::R32(ctx.esp + 4);
    bool to_front_end = launch && kernel::R32(launch) == 1;
    if (csx::LocalServerRunning() && (to_front_end || line != "+connect " + csx::LocalServerAddress()))
        csx::LocalServerStop(menu_match ? "a new match from the menus" : "the game left its match");
    if (menu_match && line.rfind("+map ", 0) == 0 && csx::LocalServerWanted()) {
        std::string connect, error;
        std::string path = kernel::ResolvePath("Z:\\releasemap.txt", 0);
        if (csx::LocalServerStart(line, hosted, connect, error)) {
            std::ofstream(path, std::ios::binary | std::ios::trunc).write(connect.c_str(), std::streamsize(connect.size() + 1));
            // this launch data again for the game's rejoins after the server's map changes (RelaunchToServer)
            if (uint32_t launch_data = kernel::R32(ctx.esp + 4))
                std::ofstream(app::ExeDir() / "local_server" / "launch_match.bin", std::ios::binary | std::ios::trunc)
                    .write(static_cast<const char*>(kernel::HostPtr(launch_data)), 0xC00);
            LOGI("net: relaunching into %s in its local server: the game joins it (%s)",
                 line.substr(5, line.find(' ', 5) == std::string::npos ? std::string::npos : line.find(' ', 5) - 5).c_str(),
                 connect.c_str());
            line = connect;
        } else {
            // not started: the front end again, with why (the listen server is PC setting server_process 0)
            LOGE("net: the match's local server did not start: back to the menus (%s)", error.c_str());
            FrontEndMessage("CSX_LOCAL_SERVER_MESSAGE", error);
            // (the match's mode is not set yet: the menu it was started from, Host a Match's (6) or Single Player's (3))
            kernel::W32(ctx.esp + 4, FrontEndLaunch(true, hosted ? 6 : 3));
            line.clear();
        }
    }
    // split-screen player 1: the next boot's aspect, its viewport's in a match, the window's in the menus
    if (csx::SplitPlayer() == 1) csx::SplitPrepareRelaunch(line.rfind("+map ", 0) == 0 || line.rfind("+connect ", 0) == 0);
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
bool ClDisconnectLostHost(X86Context& ctx, uint8_t*) {
    if (g_connecting || !g_reconnect.empty() || csx::Dedicated()) return true;
    // split-screen players 2..4 play only where player 1 does: their match over, their process ends (the compositor
    // starts them again for the next one)
    if (csx::SplitPlayer() > 1) {
        LOGI("net: split-screen player %d's match ended: %s", csx::SplitPlayer(), ReturnMessage().c_str());
        // the compositor told why: a refusal (full, password, banned) gives a player added by START its slot back
        csx::SplitSend(csx::kSplitMatch, std::string(g_refusal.empty() ? "ended " : "refused ") + ReturnMessage());
        // at once, without the static destructors: run while the game's threads go on, they crashed the process
        // (SIGSEGV or SIGABRT up to a minute later, N4.6)
        LOGI("net: split-screen player %d's process ends", csx::SplitPlayer());
        logx::Flush();
        std::_Exit(0);
    }
    // a match in this game's local server (N4.7, local_server.cpp) ended for its player (it left, or the server
    // went): the server stopped, the game back in its menus (with why, unless the player left)
    if (csx::LocalServerRunning()) {
        std::string why;
        bool alive = csx::LocalServerAlive(why);
        std::string message = !alive ? "The match's server stopped (" + why + ")." : g_close_text.empty() ? "" : ReturnMessage();
        csx::LocalServerStop("the player's match ended");
        RelaunchToFrontEnd(ctx, message, "CSX_LOCAL_SERVER_MESSAGE", true);
    }
    // a join from the server browser: back to its list with why, instead of the console's LostHost screen. At once,
    // as that screen does (its own loop ends only in a relaunch): the disconnect's callers do not expect to go on
    if (BrowserJoin()) RelaunchToFrontEnd(ctx, ReturnMessage());
    return false;
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

// a match with other players on the network or this PC: a direct connect (a client of a PC server), a match hosted
// for the network (Multiplayer > Host a Match) or split-screen. The pause menu offers Change Teams there, as the
// title's own system link menus do (pc_menu.cpp); a Single Player match alone keeps its menu as it was
bool PcNetworkMatch() {
    const char* host = std::getenv("CSX_MENU_HOST");
    return g_lobby_off || (host && *host == '1') || csx::SplitPlayer() > 0;
}

// [[hook]] at 0x5D5D7 in Host_Frame (0x5D5D0): the lobby's per-frame update (0xE9C00 on 0x554208) is skipped when the
// lobby was not set up; it would wait in accept() on a lobby socket nobody opened for listening (true: 0x5D5E1)
bool HostFrameLobby(X86Context& ctx, uint8_t*) {
    if (!g_reconnect.empty()) RelaunchToServer(ctx, g_reconnect);  // between frames: no engine code is mid-way
    if (!g_match_line.empty()) csx::RelaunchIntoMatch(ctx, std::exchange(g_match_line, std::string()));
    if (std::exchange(g_front_end, false)) RelaunchToFrontEnd(ctx, "");
    // this game's local server ended by itself (a crash): back to the menus with why, not after the connection's
    // timeout
    if (std::string why; !csx::LocalServerAlive(why))
        RelaunchToFrontEnd(ctx, "The match's server stopped (" + why + ").", "CSX_LOCAL_SERVER_MESSAGE", true);
    csx::LocalServerPauseFrame(csx::MenuOpen());
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
    if (!on && !BrowserJoin() && csx::SplitPlayer() <= 1) return;
    std::string text = kernel::ReadCString(ctx.esp + 0x10, 0x1000);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (on && !text.empty()) LOGI("console: %s", text.c_str());
    // the server's refusals as the engine prints them, for the browser's message (a join from the browser); a chat
    // line ("name : text") is never one
    if (text.find(" : ") != std::string::npos) return;
    auto has = [&](const char* s) { return text.find(s) != std::string::npos; };
    if (has("Invalid server password")) g_refusal = "The password is not the server's.";
    else if (has("banned") || has("Banned")) g_refusal = "You are banned from this server.";
    else if (has("Server is full") || has("server is full")) g_refusal = "The server is full.";
    else if (has("kicked") || has("Kicked")) g_refusal = net::wire::CleanText(text, 100);
}

// [[hook]] at 0xCA017, 0xCA07B and 0xCD913: the front end pauses the server while one of its screens is open (the
// pause menu, the spectator screen after the host's player dies), as the console's single player and listen server
// did: Cbuf_AddText("pause\n") (0x33DB98). With players on the network that froze the match for all of them (their
// connection timed out): the request is skipped while the server has network clients (true jumps past the call)
bool UiPause(X86Context&, uint8_t*) {
    if (csx::LocalServerRunning()) return true;  // a local server's pause: OwnerPauseOpen / OwnerPauseClose
    if (!csx::NetServerHasClients()) return false;
    LOG_FIRST(4, "net: the menu's pause skipped: players are connected over the network");
    return true;
}

// [[hook]] at 0xCA00F and 0xCA073, where the front end decides to pause (its screen opened, the server not paused:
// 0xCA017) or to unpause (closed, paused: 0xCA07B) by the server's sv.paused (0x5EF7C4). A game whose match is in its
// local server has no server of its own (its sv.paused is always 0): its pause goes to its server as the owner's
// (pc_owner pause, while no other human plays there); true skips the title's check and call (0xCA021, 0xCA085)
bool OwnerPauseOpen(X86Context&, uint8_t*) {
    if (!csx::LocalServerRunning()) return false;
    csx::LocalServerPause(true);
    return true;
}

bool OwnerPauseClose(X86Context&, uint8_t*) {
    if (!csx::LocalServerRunning()) return false;
    csx::LocalServerPause(false);
    return true;
}

// [[hook]] at 0x3CA25 in Cmd_ExecuteString (0x3CA10: its text at esp+8 after its push, cmd_source 0x748648 set,
// the text tokenized): a command typed or queued in a game whose match is in its local server, which is the server's
// (its settings, the pause menu's Restart Match and Change Map), goes there as the owner's (local_server.cpp); true
// returns at once (0x3CB27)
bool OwnerRoute(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kCmdSource = 0x748648;  // 1: src_command (this console, the command buffer)
    if (kernel::R32(kCmdSource) != 1 || !csx::LocalServerRunning()) return false;
    return csx::LocalServerRoute(kernel::ReadCString(kernel::R32(ctx.esp + 8), 512));
}
