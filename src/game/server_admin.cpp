// Remote administration (docs/pc_multiplayer.md "N2 administration"): the "admin" client command, over the player's
// encrypted, authenticated connection, from a player whose identity key (the transport's, the whole key) is listed in
// admins.cfg with the commands it may run. No password exists to send or guess; the engine's rcon (a plaintext
// password in a connectionless packet) is refused outright (net_security.cpp RconChallenge). Without admins.cfg no
// one is an admin. A short list of commands, each checked, logged (the run log and logs/admin.log) and run at the
// next tick, not mid-way through the engine's command parsing:
//   help, status, say TEXT, kick SLOT [REASON], ban SLOT [MINUTES], unban KEY|IP, map NAME, restart [SECONDS],
//   bots N, bot_difficulty N, friendly_fire on|off, round_time MINUTES, password TEXT|-
// admins.cfg: "KEY PERMISSIONS [# comment]" lines; KEY a player's 64 hex character identity (its log line at
// connect, admin status), PERMISSIONS "all" or a comma list of status, say, kick, ban, map, restart, bots, settings.
// bans.txt: "KEY|IP UNTIL [# comment]" lines (UNTIL in Unix seconds, 0 for ever), checked at each connection.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/transport.h"

namespace {
using kernel::R16;
using kernel::R32;

constexpr uint32_t kHostClient = 0x6CDFEC;
constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010;
constexpr uint32_t kClientPrintf = 0x5C3E0;  // SV_ClientPrintf (cdecl: format, ...): to host_client
constexpr uint32_t kDropClient = 0x5C600;    // SV_DropClient (cdecl: client, crash, format, ...)
constexpr uint32_t kNaIp = 3;

const std::vector<std::string> kPermissions = {"status", "say", "kick", "ban", "map", "restart", "bots", "settings"};

struct Command {
    const char* name;
    const char* permission;
    const char* usage;
};
const std::vector<Command> kCommands = {
    {"help", nullptr, "help: the commands this key may run"},
    {"status", "status", "status: every player's slot, name, address and identity"},
    {"say", "say", "say TEXT: a message to every player"},
    {"kick", "kick", "kick SLOT [REASON]: a network player off the server"},
    {"ban", "ban", "ban SLOT [MINUTES]: kick and refuse the player's identity (or address) for MINUTES (0: for ever)"},
    {"unban", "ban", "unban KEY|IP: lift a ban"},
    {"map", "map", "map NAME: change the map (every player rejoins)"},
    {"restart", "restart", "restart [SECONDS]: restart the round (1..15 s, 1)"},
    {"bots", "bots", "bots N: the bots from the next map (0..max_players: the title sets them at a match's start)"},
    {"bot_difficulty", "bots", "bot_difficulty N: 0 easy .. 3 expert, from the next map"},
    {"friendly_fire", "settings", "friendly_fire on|off"},
    {"round_time", "settings", "round_time MINUTES: 1..9"},
    {"password", "settings", "password TEXT|-: the join password (- for none)"},
};

std::string Path(const char* name) {
    return (app::ExeDir() / name).string();
}

// admins.cfg, read again when it changes
struct Admins {
    std::filesystem::file_time_type stamp{};
    bool present = false;
    std::map<std::string, std::set<std::string>> keys;  // key -> permissions
} g_admins;

void LoadAdmins() {
    std::error_code ec;
    auto path = Path("admins.cfg");
    auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (g_admins.present) LOGI("admin: admins.cfg removed: remote administration off");
        g_admins = Admins{};
        return;
    }
    if (g_admins.present && stamp == g_admins.stamp) return;
    g_admins = Admins{};
    g_admins.present = true;
    g_admins.stamp = stamp;
    std::ifstream in(path);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        number++;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream w(line);
        std::string key, perms;
        if (!(w >> key)) continue;
        for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!(w >> perms) || !net::Identity::ParseFingerprint(key)) {
            LOGW("admin: admins.cfg:%d ignored (want: a 64 hex character player key and its permissions)", number);
            continue;
        }
        std::set<std::string> set;
        std::stringstream ps(perms);
        for (std::string p; std::getline(ps, p, ',');) {
            if (p == "all") set.insert(kPermissions.begin(), kPermissions.end());
            else if (std::find(kPermissions.begin(), kPermissions.end(), p) != kPermissions.end()) set.insert(p);
            else LOGW("admin: admins.cfg:%d: unknown permission \"%s\" ignored", number, p.c_str());
        }
        g_admins.keys[key] = set;
    }
    LOGI("admin: admins.cfg: %zu admin keys", g_admins.keys.size());
}

// bans.txt: key or ip -> until (0 for ever)
std::map<std::string, long long> ReadBans() {
    std::map<std::string, long long> m;
    std::ifstream in(Path("bans.txt"));
    std::string line;
    while (std::getline(in, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream w(line);
        std::string who;
        long long until = 0;
        if (w >> who) {
            w >> until;
            m[who] = until;
        }
    }
    return m;
}

void WriteBans(const std::map<std::string, long long>& m) {
    std::ofstream out(Path("bans.txt"), std::ios::trunc);
    out << "# KEY|IP UNTIL (Unix seconds, 0 for ever): players the server refuses (csx-server admin ban / unban)\n";
    for (auto& [who, until] : m) out << who << " " << until << "\n";
}

void AdminLog(const std::string& text) {
    LOGI("admin: %s", text.c_str());
    std::error_code ec;
    std::filesystem::create_directories(app::ExeDir() / "logs", ec);
    std::ofstream out((app::ExeDir() / "logs" / "admin.log").string(), std::ios::app);
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    out << stamp << " " << text << "\n";
}

// refused attempts per address: 5 a minute are answered and logged, the rest dropped (one log line a minute)
struct Attempts {
    std::deque<double> times;
    double quiet_logged = 0;
};
std::map<std::string, Attempts> g_attempts;

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Throttled(const std::string& ip) {
    double now = Seconds();
    Attempts& a = g_attempts[ip];
    while (!a.times.empty() && now - a.times.front() > 60) a.times.pop_front();
    if (a.times.size() >= 5) {
        if (now - a.quiet_logged > 60) {
            a.quiet_logged = now;
            AdminLog("refused attempts from " + ip + " over 5 a minute: dropped unanswered");
        }
        return true;
    }
    a.times.push_back(now);
    return false;
}

thread_local bool g_console_reply = false;  // the command is the server console's (N9): replies to the log

// text printed in the admin's console (host_client is the admin while its command is parsed)
void Reply(X86Context& ctx, const std::string& text) {
    std::istringstream lines(text);
    if (g_console_reply) {
        for (std::string l; std::getline(lines, l);) LOGI("admin console: %s", l.substr(0, 200).c_str());
        return;
    }
    for (std::string l; std::getline(lines, l);) {
        l = l.substr(0, 200) + "\n";
        uint32_t esp = ctx.esp;
        uint32_t str = (esp - 0x400 - uint32_t(l.size())) & ~15u, fmt = str - 16;
        std::memcpy(kernel::HostPtr(str), l.c_str(), l.size() + 1);
        std::memcpy(kernel::HostPtr(fmt), "%s", 3);
        ctx.esp = fmt - 16;
        kernel::CallGuest(ctx, kClientPrintf, {fmt, str});
        ctx.esp = esp;
    }
}

std::string ClientAddress(uint32_t cl) {
    uint32_t adr = cl + 0x20 + 4;
    const uint8_t* ip = static_cast<const uint8_t*>(kernel::HostPtr(adr + 4));
    uint16_t p = R16(adr + 0x12);
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]) + ":" + std::to_string(uint16_t(p >> 8 | p << 8));
}
bool Network(uint32_t cl) {
    return R32(cl + 0x20 + 4) == kNaIp;
}
std::string Name(uint32_t cl) {
    return kernel::ReadCString(cl + 0x4D10, 32);
}
uint32_t ClientAt(int slot) {  // 1-based slot of an active client, 0 for none
    uint32_t n = R32(kMaxClients);
    if (slot < 1 || uint32_t(slot) > n || n > 256) return 0;
    uint32_t cl = R32(kClients) + uint32_t(slot - 1) * kClientSize;
    return R32(cl) ? cl : 0;
}

// text for the engine's command line: no quotes, semicolons or line breaks
std::string Clean(std::string s, size_t max) {
    for (char& c : s)
        if (c == '"' || c == ';' || c == '\n' || c == '\r' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = ' ';
    if (s.size() > max) s.resize(max);
    return s;
}

bool Number(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 6 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::atoi(s.c_str());
    return out >= lo && out <= hi;
}

// a csx-server setting for the next map: its "key value" line in CSX_SERVER_SETTINGS (server_main.cpp), which the
// title's relaunch into the next map rebuilds the match line from
bool SetNextMapSetting(const std::string& key, const std::string& value) {
    const char* kept = std::getenv("CSX_SERVER_SETTINGS");
    if (!kept) return false;
    std::istringstream in(kept);
    std::string out, line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.rfind(key + " ", 0) == 0) line = key + " " + value, found = true;
        out += line + "\n";
    }
    if (!found) out += key + " " + value + "\n";
#ifdef _WIN32
    return _putenv_s("CSX_SERVER_SETTINGS", out.c_str()) == 0;
#else
    return setenv("CSX_SERVER_SETTINGS", out.c_str(), 1) == 0;
#endif
}

// actions run at the next tick (DedicatedTick's hook point, outside any command parsing)
std::mutex g_lock;
std::vector<std::function<void(X86Context&)>> g_pending;

void Later(std::function<void(X86Context&)> f) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pending.push_back(std::move(f));
}

void Drop(X86Context& ctx, uint32_t cl, const std::string& reason) {
    uint32_t esp = ctx.esp;
    std::string r = reason.substr(0, 100);
    uint32_t str = (esp - 0x400 - uint32_t(r.size())) & ~15u, fmt = str - 16;
    std::memcpy(kernel::HostPtr(str), r.c_str(), r.size() + 1);
    std::memcpy(kernel::HostPtr(fmt), "%s", 3);
    ctx.esp = fmt - 16;
    kernel::CallGuest(ctx, kDropClient, {cl, 0, fmt, str});
    ctx.esp = esp;
}

}  // namespace

namespace csx {

// a network player's transport connection ended on the server (it closed, timed out, or was replaced by a new
// connection of the same player identity: a game restarted after a crash): its engine client is dropped instead of
// after the engine's own timeout, so its slot is free at once (a client that already left is not active any more and
// nothing happens)
void DropNetworkClient(X86Context& ctx, const std::string& address, const std::string& reason, bool now) {
    auto drop = [address, reason](X86Context& c) {
        // every slot the engine counts as taken (its free-slot search, 0x99470: +0, +4 or +0xC set), also a client
        // still joining (not yet active)
        for (uint32_t slot = 1; slot <= R32(kMaxClients) && slot <= 256; slot++) {
            uint32_t cl = R32(kClients) + (slot - 1) * kClientSize;
            bool taken = R32(cl) || R32(cl + 4) || R32(cl + 0xC);
            if (taken && Network(cl) && ClientAddress(cl) == address) {
                LOGI("net: %s's client (slot %u, \"%s\") dropped: %s", address.c_str(), slot, Name(cl).c_str(), reason.c_str());
                Drop(c, cl, reason);
            }
        }
    };
    // now: while the engine reads its packets (as a client's own "disconnect" is handled), before the replacing
    // connection's join in the same frame finds the server full; otherwise at the next tick
    if (now) drop(ctx);
    else Later(drop);
}

// at a connection: a banned identity or address is refused (net_engine.cpp)
bool Banned(const std::string& identity, const std::string& ip, std::string& why) {
    auto bans = ReadBans();
    long long now = std::time(nullptr);
    for (const std::string& who : {identity, ip}) {
        if (who.empty()) continue;
        auto it = bans.find(who);
        if (it != bans.end() && (it->second == 0 || it->second > now)) {
            why = it->second == 0 ? "banned from this server" : "banned from this server for a while";
            return true;
        }
    }
    return false;
}

// the "admin" client command (net_security.cpp ClientCommandAllowed): true when it was one (handled here)
bool AdminCommand(X86Context& ctx, uint32_t cl, const std::string& text) {
    // cl 0: the dedicated server's own console (its stdin: the server GUI, tools/csx_server_gui.py), the server's
    // owner, every permission (it can stop the server anyway)
    const bool owner = cl == 0;
    struct ReplyMode {
        bool was = g_console_reply;
        explicit ReplyMode(bool on) { g_console_reply = on; }
        ~ReplyMode() { g_console_reply = was; }
    } reply_mode(owner);
    std::string ip = owner ? std::string("console") : ClientAddress(cl), name = owner ? std::string("server console") : Name(cl);
    std::string ip_only = ip.substr(0, ip.find(':'));
    if (!owner && !Network(cl)) return false;
    std::istringstream w(text);
    std::string word, cmd;
    w >> word >> cmd;
    std::string rest;
    std::getline(w, rest);
    rest.erase(0, rest.find_first_not_of(' '));
    for (char& c : cmd) c = char(std::tolower(static_cast<unsigned char>(c)));
    LoadAdmins();
    auto key = csx::NetPeerIdentity(ip);
    std::string who = "\"" + name + "\" " + ip + " key " + (key ? *key : std::string("none"));
    auto refuse = [&](const std::string& why) {
        if (Throttled(ip_only)) return true;
        // a password given to a refused command is not written down
        AdminLog("refused " + who + ": \"" + (cmd == "password" ? std::string("admin password ...") : Clean(text, 120)) + "\": " + why);
        Reply(ctx, "admin: " + why);
        return true;
    };
    static const std::set<std::string> kAll(kPermissions.begin(), kPermissions.end());
    if (!owner && !g_admins.present) return refuse("remote administration is not configured on this server");
    if (!owner && !key) return refuse("no player identity (net_identity 1 is needed to administer)");
    auto it = owner ? g_admins.keys.end() : g_admins.keys.find(*key);
    if (!owner && it == g_admins.keys.end()) return refuse("this player key is not an admin");
    const std::set<std::string>& perms = owner ? kAll : it->second;
    const Command* c = nullptr;
    for (const Command& k : kCommands)
        if (cmd == k.name) c = &k;
    if (!c) return refuse("unknown admin command \"" + Clean(cmd, 32) + "\" (admin help)");
    if (c->permission && !perms.count(c->permission)) return refuse(std::string("not permitted: ") + c->permission);
    auto done = [&](const std::string& what) {
        AdminLog(who + " ran \"" + Clean(text, 160) + "\": " + what);
        Reply(ctx, "admin: " + what);
        return true;
    };
    auto usage = [&]() {
        Reply(ctx, std::string("admin: usage: ") + c->usage);
        return true;
    };
    if (cmd == "help") {
        std::string s;
        for (const Command& k : kCommands)
            if (!k.permission || perms.count(k.permission)) s += std::string("admin ") + k.usage + "\n";
        Reply(ctx, s);
        return true;
    }
    if (cmd == "status") {
        std::string s;
        uint32_t n = R32(kMaxClients);
        int players = 0;
        for (uint32_t i = 0; i < n && n <= 256; i++) {
            uint32_t p = R32(kClients) + i * kClientSize;
            if (!R32(p)) continue;
            players++;
            bool bot = R32(p + 0x2548) != 0;
            auto id = Network(p) ? csx::NetPeerIdentity(ClientAddress(p)) : std::nullopt;
            s += "#" + std::to_string(i + 1) + " \"" + Name(p) + "\" " +
                 (bot ? std::string("BOT") : Network(p) ? ClientAddress(p) + " " + (id ? *id : "no identity") : "local") + "\n";
        }
        Reply(ctx, s + std::to_string(players) + " of " + std::to_string(n) + " slots in use");
        AdminLog(who + " ran \"status\"");
        return true;
    }
    if (cmd == "say") {
        if (rest.empty()) return usage();
        std::string msg = Clean(rest, 120);
        csx::QueueEngineCommand("say \"" + msg + "\"\n");
        return done("said \"" + msg + "\"");
    }
    if (cmd == "kick" || cmd == "ban") {
        std::istringstream a(rest);
        std::string slot_text, extra;
        a >> slot_text;
        std::getline(a, extra);
        extra.erase(0, extra.find_first_not_of(' '));
        int slot = 0, minutes = 0;
        if (!Number(slot_text, 1, 65, slot)) return usage();
        uint32_t target = ClientAt(slot);
        if (!target || !Network(target)) return Reply(ctx, "admin: slot " + slot_text + " is no network player (admin status)"), true;
        std::string target_ip = ClientAddress(target), target_name = Name(target);
        if (cmd == "ban") {
            if (!extra.empty() && !Number(extra, 0, 525600, minutes)) return usage();
            auto id = csx::NetPeerIdentity(target_ip);
            std::string banned = id ? *id : target_ip.substr(0, target_ip.find(':'));
            auto bans = ReadBans();
            bans[banned] = minutes ? (long long)std::time(nullptr) + minutes * 60LL : 0;
            WriteBans(bans);
            extra = "banned";
            done("banned \"" + target_name + "\" (" + banned + ") " +
                 (minutes ? "for " + std::to_string(minutes) + " minutes" : std::string("for ever")));
        } else {
            done("kicked \"" + target_name + "\" " + target_ip);
        }
        std::string reason = extra.empty() ? "Kicked" : Clean(extra, 60);
        Later([target, target_ip, reason](X86Context& c2) {
            // still the same player (the slot may have been taken since)
            if (R32(target) && Network(target) && ClientAddress(target) == target_ip) Drop(c2, target, reason);
        });
        return true;
    }
    if (cmd == "unban") {
        std::string target = Clean(rest, 64);
        for (char& ch : target) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        auto bans = ReadBans();
        if (!bans.erase(target)) return Reply(ctx, "admin: no ban for " + target), true;
        WriteBans(bans);
        return done("unbanned " + target);
    }
    if (cmd == "map") {
        std::string map = rest;
        if (map.empty() || map.size() > 31 || map.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            return usage();
        if (!csx::Dedicated()) return Reply(ctx, "admin: map changes are for the dedicated server (csx-server)"), true;
        const char* game = std::getenv("GAME_DIR");
        if (!game || !std::filesystem::exists(std::filesystem::path(game) / (map + ".SXWad")))
            return Reply(ctx, "admin: no map " + map + " on this server"), true;
        done("map change to " + map);
        Later([map](X86Context& c2) { csx::RelaunchToMap(c2, map); });
        return true;
    }
    if (cmd == "restart") {
        int s = 1;
        if (!rest.empty() && !Number(rest, 1, 15, s)) return usage();
        csx::QueueEngineCommand("sv_restartround " + std::to_string(s) + "\n");
        return done("round restart in " + std::to_string(s) + " s");
    }
    if (cmd == "bots" || cmd == "bot_difficulty") {
        // the title sets its bots at a match's start (bot_quota changed later is kept to the start's count, bots
        // added are removed again): the server's setting for the next map, which the relaunch's match line takes
        int v = 0;
        if (cmd == "bots" ? !Number(rest, 0, int(R32(kMaxClients)), v) : !Number(rest, 0, 3, v)) return usage();
        if (!csx::Dedicated()) return Reply(ctx, "admin: " + cmd + " is for the dedicated server (csx-server)"), true;
        if (!SetNextMapSetting(cmd, std::to_string(v))) return Reply(ctx, "admin: the server's settings were not found"), true;
        return done(cmd + " " + std::to_string(v) + " from the next map (admin map NAME, or the match's end)");
    }
    if (cmd == "friendly_fire") {
        if (rest != "on" && rest != "off") return usage();
        csx::QueueEngineCommand(std::string("mp_friendlyfire ") + (rest == "on" ? "1" : "0") + "\n");
        return done("friendly fire " + rest);
    }
    if (cmd == "round_time") {
        int v = 0;
        if (!Number(rest, 1, 9, v)) return usage();
        csx::QueueEngineCommand("mp_roundtime " + std::to_string(v) + "\n");
        return done("round time " + std::to_string(v) + " minutes (from the next round)");
    }
    if (cmd == "password") {
        if (rest.empty() || rest.size() > 32 || rest.find_first_of(" \";\\") != std::string::npos) return usage();
        csx::QueueEngineCommand(rest == "-" ? std::string("sv_password \"\"\n") : "sv_password " + rest + "\n");
        AdminLog(who + " ran \"admin password ...\": " + (rest == "-" ? "password removed" : "password changed"));
        Reply(ctx, std::string("admin: ") + (rest == "-" ? "password removed" : "password changed"));
        return true;
    }
    return usage();
}

// a local server (N4.7, local_server.cpp): the identity of the game that started it for its match, which takes the
// menu's host actions that a listen server ran in that game's own process
bool OwnedServer() {
    const char* k = std::getenv("CSX_SERVER_OWNER_KEY");
    return Dedicated() && k && std::strlen(k) == 64;
}

// the owner's client slot (0-based), -1 while it is not connected (state_probe.cpp: the host's player)
int OwnerSlot() {
    const char* owner = std::getenv("CSX_SERVER_OWNER_KEY");
    // (no clients yet: a server still booting, asked from the tests' probe thread, had none to read: N4.8's LAN test
    // crashed there)
    if (!owner || !R32(kClients)) return -1;
    for (uint32_t slot = 1; slot <= R32(kMaxClients) && slot <= 256; slot++) {
        uint32_t cl = R32(kClients) + (slot - 1) * kClientSize;
        if (!R32(cl) || !Network(cl)) continue;
        auto key = csx::NetPeerIdentity(ClientAddress(cl));
        if (key && *key == owner) return int(slot - 1);
    }
    return -1;
}

// the owner's pause: the match's simulation stopped (OwnerPausedFrame) while the network goes on. Not the engine's
// pause (sv.paused, svc_setpause): on this dedicated server it also stopped every packet to the clients, and the
// owner's game, a network client, timed out on its own server
std::atomic<bool> g_owner_paused{false};

void SetPause(bool on) {
    g_owner_paused = on;
}

// the server is paused (the owner's pause menu) and a human other than the owner is connected (a split-screen
// player added by START, a LAN player): the match goes on for them (net_engine.cpp, at a connection)
void OwnerPauseCheck(X86Context&) {
    if (!OwnedServer() || !g_owner_paused) return;
    SetPause(false);
    LOGI("owner: the match unpaused: another player connected");
}

bool OwnerPaused() { return g_owner_paused; }

// "pc_owner VERB ..." (net_security.cpp ClientCommandAllowed): only from the owner's identity, on this machine
//   restart          the pause menu's Restart Match (HostRestart, 0xE9110: its game code 0x143730, the rules' complete reset)
//   changemap MAP    the pause menu's Change Map (the map the owner's menu chose; a relaunch as admin map's)
//   console HEX      a console line of the owner's for the server (sv_, mp_, bot_ ... : local_server.cpp), one command
//   pause on|off     the pause menu's pause, while no other human is connected: the simulation stopped (bots, entities,
//                    the match's clock), the network not
bool OwnerCommand(X86Context& ctx, uint32_t cl, const std::string& text) {
    std::string ip = ClientAddress(cl);
    std::istringstream w(text);
    std::string word, verb, arg;
    w >> word >> verb;
    std::getline(w, arg);
    arg.erase(0, arg.find_first_not_of(' '));
    auto key = csx::NetPeerIdentity(ip);
    if (!OwnedServer() || !key || *key != std::getenv("CSX_SERVER_OWNER_KEY") || ip.rfind("127.", 0) != 0) {
        LOG_FIRST(16, "owner: \"%s\" from \"%s\" %s refused: not this server's owner", Clean(verb, 16).c_str(),
                  Name(cl).c_str(), ip.c_str());
        return true;
    }
    if (verb == "restart") {
        LOGI("owner: Restart Match");
        Later([](X86Context& c) { kernel::CallGuest(c, 0xE9110, {}); });  // HostRestart (its jump to 0x143730)
        return true;
    }
    if (verb == "changemap") {
        const char* game = std::getenv("GAME_DIR");
        if (arg.empty() || arg.size() > 31 || arg.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
            !game || !std::filesystem::exists(std::filesystem::path(game) / (arg + ".SXWad"))) {
            LOGW("owner: Change Map to \"%s\" refused: no such map", Clean(arg, 32).c_str());
            return true;
        }
        LOGI("owner: Change Map to %s", arg.c_str());
        Later([arg](X86Context& c) { csx::RelaunchToMap(c, arg); });
        return true;
    }
    if (verb == "console") {
        std::string line;
        bool ok = arg.size() % 2 == 0 && arg.size() <= 2 * 200 && arg.find_first_not_of("0123456789abcdef") == std::string::npos;
        for (size_t i = 0; ok && i < arg.size(); i += 2) {
            char ch = char(std::stoi(arg.substr(i, 2), nullptr, 16));
            ok = ch >= 0x20 && ch < 0x7F && ch != ';';
            line += ch;
        }
        if (!ok || line.empty()) {
            LOGW("owner: a console line refused (one printable command, no ';')");
            return true;
        }
        LOGI("owner: console \"%s\"", line.c_str());
        csx::QueueEngineCommand(line + "\n");
        return true;
    }
    if (verb == "pause") {
        if (arg == "on") {
            for (uint32_t slot = 1; slot <= R32(kMaxClients) && slot <= 256; slot++) {
                uint32_t other = R32(kClients) + (slot - 1) * kClientSize;
                if (other != cl && R32(other) && Network(other)) {
                    LOGI("owner: the pause menu's pause skipped: \"%s\" is playing too", Name(other).c_str());
                    return true;
                }
            }
            LOGI("owner: paused (the pause menu)");
            SetPause(true);
        } else if (arg == "off") {
            LOGI("owner: unpaused (the pause menu)");
            SetPause(false);
        }
        return true;
    }
    LOGW("owner: unknown command \"%s\"", Clean(verb, 32).c_str());
    return true;
}

// the admin actions due (DedicatedTick, the host frame's start)
void RunAdminActions(X86Context& ctx) {
    std::vector<std::function<void(X86Context&)>> due;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        due.swap(g_pending);
    }
    for (auto& f : due) f(ctx);
}

}  // namespace csx
