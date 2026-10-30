// The engine's network security for PC multiplayer (docs/pc_multiplayer.md "N1 security"; the findings in
// analysis/netcode_engine.md). The transport below (src/net) already refuses spoofed sources, replays and malformed
// datagrams; these close the engine's own gaps:
//   - the connect challenge is checked (SV_CheckChallenge 0x98CE0 returned 1 on every path)
//   - rcon is refused from the network (a plaintext password in a connectionless packet; N2: "admin" over the
//     player's encrypted connection instead, server_admin.cpp); the listen server's own loopback keeps it
//   - the info replies no longer carry the session's Xbox key, key id and host address
//   - a network client's userinfo loses any "*" key (the server's own: *bot); its player identity stays with the
//     transport (csx::NetPeerIdentity, the whole key), not in the userinfo every player receives (256 bytes at most)
//   - a network client cannot run the host's privileged commands (kick, pause; the cheats unless sv_cheats)
//   - a client only runs the commands a network server sends it (svc_stufftext) that are on a short list
//   - the last player slot is lag compensated (SV_SetupMove skipped entity number maxclients)
//   - a dropped network client is told the reason through the transport
//   - Player Collision reaches a network client's prediction (the match's setting, sent at signon)
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "common/mods.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "game/stuff_filter.h"
#include "net/transport.h"

namespace {
using kernel::R16;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kNaLoopback = 1, kNaIp = 3;
constexpr uint32_t kArgc = 0x4253E8, kArgv = 0x4251A8;           // Cmd_Argc, Cmd_Argv's table
constexpr uint32_t kChallenges = 0x63B0C0, kChallengeCount = 1024, kChallengeSize = 0x1C;  // {netadr, challenge, time}
constexpr uint32_t kRejectConnection = 0x98910;                  // SV_RejectConnection(edi adr; fmt, ...), cdecl
constexpr uint32_t kNetFrom = 0x6BA5E0;                           // net_from
constexpr uint32_t kHostClient = 0x6CDFEC, kClientChannelType = 0x24, kClientAddress = 0x24;
constexpr uint32_t kSvCheats = 0x379790 + 0xC;                    // sv_cheats' value (float)
constexpr uint32_t kGetSpace = 0x3ECE0;                           // SZ_GetSpace(buf, length), cdecl
constexpr uint8_t kSvcStufftext = 9;
constexpr uint32_t kComToken = 0x747DC0;                         // com_token (COM_Parse 0x3EF80)

float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}

std::string Argv(uint32_t i) {
    if (i >= R32(kArgc)) return {};
    uint32_t p = R32(kArgv + 4 * i);
    return p ? kernel::ReadCString(p, 1024) : std::string();
}

std::string AddressText(uint32_t adr) {
    const uint8_t* ip = static_cast<const uint8_t*>(kernel::HostPtr(adr + 4));
    uint16_t p = R16(adr + 0x12);
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]) + ":" + std::to_string(uint16_t(p >> 8 | p << 8));
}

// the challenge the server gave this address (getchallenge / challenge rcon), -1 for none: the table's entry with the
// same type and IP (as NET_CompareBaseAdr: the port is not compared)
int64_t ChallengeFor(uint32_t adr) {
    for (uint32_t i = 0; i < kChallengeCount; i++) {
        uint32_t e = kChallenges + i * kChallengeSize;
        if (R32(e) == R32(adr) && R32(e + 4) == R32(adr + 4)) return R32(e + 0x14);
    }
    return -1;
}

// a guest string below the stack for a call (as pc_console.cpp's Print)
uint32_t StackString(X86Context& ctx, uint32_t below, const std::string& s) {
    uint32_t va = (ctx.esp - below - uint32_t(s.size())) & ~15u;
    std::memcpy(kernel::HostPtr(va), s.c_str(), s.size() + 1);
    return va;
}

void Reject(X86Context& ctx, uint32_t adr, const std::string& text) {
    uint32_t esp = ctx.esp, edi = ctx.edi;
    uint32_t fmt = StackString(ctx, 0x600, text);
    ctx.esp = fmt - 32;
    ctx.edi = adr;
    kernel::CallGuest(ctx, kRejectConnection, {fmt});
    ctx.esp = esp;
    ctx.edi = edi;
}

bool NetworkClient(uint32_t client) {
    return client && R32(client + 0x20 + 4) == kNaIp;
}

// userinfo "\key\value..." pairs
std::vector<std::pair<std::string, std::string>> ParseInfo(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size() && s[i] == '\\') {
        size_t k = s.find('\\', i + 1);
        if (k == std::string::npos) break;
        size_t v = s.find('\\', k + 1);
        out.emplace_back(s.substr(i + 1, k - i - 1), s.substr(k + 1, v == std::string::npos ? std::string::npos : v - k - 1));
        if (v == std::string::npos) break;
        i = v;
    }
    return out;
}

}  // namespace

// SV_CheckChallenge (0x98CE0: esi the sender's netadr; the challenge is Cmd_Argv(2) of the connect line): 1 when the
// challenge is the one this address was given (a loopback client needs none), else the connection is rejected
X86_FUNC(sub_00098CE0) {
    (void)base;
    uint32_t adr = ctx.esi;
    uint32_t ok = 1;
    if (adr && R32(adr) != kNaLoopback) {
        std::string arg = Argv(2);
        int64_t want = ChallengeFor(adr);
        uint32_t got = uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        if (want < 0 || arg.empty() || got != uint32_t(want)) {
            LOG_FIRST(32, "net: connect from %s refused: %s challenge", AddressText(adr).c_str(),
                      want < 0 ? "no" : "a wrong");
            Reject(ctx, adr, want < 0 ? "No challenge for your address.\n" : "Bad challenge.\n");
            ok = 0;
        }
    }
    ctx.eax = ok;
    ctx.esp += 4;
}

// [[hook]] at 0x9A055 (getchallenge, SVC_GetChallenge 0x99D90) and 0x9A2E4 (challenge rcon, 0x9A150), the new
// entry's time stored from eax: the engine stored (int)realtime and gives a new address the entry with the lowest
// time (strictly lower: jge at 0x99E4D). For the server's first second every time was 0, as an empty entry's, so the
// "oldest" was always entry 0: clients asking in that second (all of them rejoining after a map change's relaunch)
// overwrote each other's challenge and all but the last were refused "No challenge for your address" (net_join_early).
// The time is the issue order instead (1, 2, ...): the oldest is the one issued longest ago, never one just given.
void ChallengeTime(X86Context& ctx, uint8_t*) {
    static uint32_t issued = 0;
    issued = issued < 0x7FFFFFFE ? issued + 1 : 1;
    ctx.eax = issued;
}

// [[hook]] at 0x9BAC9 in the rcon check (0x9BA70), after the password tests (an empty rcon_password was refused
// before): rcon from the network is refused whatever its password and challenge, the loopback's (a listen server's
// own console) allowed; true goes to the refusal (0x9BBA6)
bool RconChallenge(X86Context&, uint8_t*) {
    return R32(kNetFrom) != kNaLoopback;
}

// [[hook]] at 0x9BBA6, the rcon check's refusal (too few arguments, an empty rcon_password, or RconChallenge): the
// engine logs none of them
void RconRefused(X86Context&, uint8_t*) {
    if (R32(kNetFrom) == kNaLoopback) return;
    LOG_FIRST(16, "net: rcon from %s refused: rcon is off (its password travels in plaintext); use \"admin\" with an "
                  "admins.cfg key", AddressText(kNetFrom).c_str());
}

// [[hook]] at 0x9ADE3, 0x9AE18, 0x9AE3C (SVC_Info's three MSG_WriteString of the session key id, key and host XNADDR,
// esi the bytes): written empty; and at 0x9A8B3, 0x9A8FB, 0x9A931 (SVC_InfoString's Info_SetValueForKey of the same,
// the value at esp+8): an empty value leaves the key out
void InfoNoKey(X86Context& ctx, uint8_t*) {
    kernel::G<uint8_t>(ctx.esi) = 0;
}
void InfoStringNoKey(X86Context& ctx, uint8_t*) {
    kernel::G<uint8_t>(R32(ctx.esp + 8)) = 0;
}

// [[hook]] at 0x9967E in SV_ConnectClient, the userinfo copied to esp+0x98 (0x400 bytes), the sender at esp+0x24:
// a network client's "*" keys are removed (setinfo refuses them; the connect line did not); its identity is logged
void ConnectUserinfo(X86Context& ctx, uint8_t*) {
    uint32_t adr = ctx.esp + 0x24, buf = ctx.esp + 0x98;
    if (R32(adr) != kNaIp) return;
    std::string info = kernel::ReadCString(buf, 0x3FF), out;
    int stripped = 0;
    for (auto& [k, v] : ParseInfo(info)) {
        if (!k.empty() && k[0] == '*') {
            stripped++;
            continue;
        }
        out += "\\" + k + "\\" + v;
    }
    std::string peer = AddressText(adr);
    if (auto id = csx::NetPeerIdentity(peer)) LOGI("net: %s connects with player identity %s", peer.c_str(), id->c_str());
    if (out.size() > 0x3FE) out.resize(0x3FE);
    std::memcpy(kernel::HostPtr(buf), out.c_str(), out.size() + 1);
    if (stripped) LOGW("net: %s sent %d \"*\" userinfo keys, removed", peer.c_str(), stripped);
}

// [[hook]] at 0xAB4ED in SV_ParseStringCommand (0xAB4E0: esi the command, eax SV_IsClientCommandAllowed's verdict, 1 an
// engine command): a network client's "admin" goes to the server's administration; it may not kick, pause or use the
// cheats (without sv_cheats), which the engine let any client run; true ends the command (0xAB541)
bool ClientCommandAllowed(X86Context& ctx, uint8_t*) {
    // the name as the engine's own check (0xAA680) read it: COM_Parse's token (com_token 0x747DC0), which skips every
    // character up to a space and strips quotes; splitting the text again here let " kick 2" or "\"kick\" 2" through
    std::string name = kernel::ReadCString(kComToken, 128);
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    static const bool log_cmds = std::getenv("CSX_LOG_CLIENTCMDS") && *std::getenv("CSX_LOG_CLIENTCMDS") == '1';
    if (log_cmds)  // every client's string command (a debugging aid: what the menus send)
        LOGI("net: client command from %s: %s", kernel::ReadCString(R32(kHostClient) + 0x4D10, 32).c_str(),
             kernel::ReadCString(ctx.esi, 200).c_str());
    // Condition Zero's extra radio lines (N5C, pc_radio.cpp): any client's, the listen server's own player's too
    if (name == "csx_radio") return csx::RadioCommand(ctx, R32(kHostClient), kernel::ReadCString(ctx.esi, 512)) || true;
    if (!NetworkClient(R32(kHostClient))) return false;
    // remote administration (N2, server_admin.cpp): handled here, never the game DLL's
    if (name == "admin") return csx::AdminCommand(ctx, R32(kHostClient), kernel::ReadCString(ctx.esi, 512)) || true;
    // a local server's owner (N4.7): its menu's host actions (server_admin.cpp OwnerCommand checks who sent it)
    if (name == "pc_owner") return csx::OwnerCommand(ctx, R32(kHostClient), kernel::ReadCString(ctx.esi, 512)) || true;
    // the game code's "changemap" (0x13F92C) relaunches the server into the map its own front end chose (0x526EC0): a
    // listen server's host menu; from a network client (any, on a match in game mode 2 or 3) it changed the server's
    // map. A local server's owner sends pc_owner changemap
    if (name == "changemap") {
        LOG_FIRST(16, "net: %s's \"changemap\" refused (the host's menu command, not for network clients)",
                  kernel::ReadCString(R32(kHostClient) + 0x4D10, 32).c_str());
        return true;
    }
    if (ctx.eax != 1) return false;
    static const std::set<std::string> host_only = {"kick", "pause", "setpause", "unpause"};
    static const std::set<std::string> cheats = {"god", "noclip", "notarget", "fly"};
    bool refuse = host_only.count(name) || (cheats.count(name) && F(kSvCheats) == 0);
    if (refuse) {
        uint32_t cl = R32(kHostClient);
        LOG_FIRST(32, "net: %s's \"%s\" refused (not for network clients)", kernel::ReadCString(cl + 0x4D10, 32).c_str(),
                  name.c_str());
    }
    return refuse;
}

// [[hook]] at 0x30A88 in CL_ParseStuffText (0x30A80: edi the text, before Cbuf_AddText): from a network server only
// the listed commands run (a server could otherwise run anything on its clients: bind, exec, quit, ...)
void StuffTextFilter(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kClientChannelType = 0x7812E8 + 4;  // cls.netchan.remote_address.type
    if (R32(kClientChannelType) != kNaIp) return;
    std::string text = kernel::ReadCString(ctx.edi, 2048);
    std::vector<std::string> dropped;
    std::string kept = csx::FilterStuff(text, dropped);
    for (auto& d : dropped) LOG_FIRST(64, "net: the server's command \"%s\" not run (not on the list)", d.c_str());
    if (kept != text) std::memcpy(kernel::HostPtr(ctx.edi), kept.c_str(), kept.size() + 1);
}

// [[hook]] at 0xAB078 and 0xAB227, the two loops of SV_SetupMove over the other players' entity numbers (eax / edi
// the number, edx / the global the maxclients): the original skipped number == maxclients (`>=`), so the player in
// the last slot was never moved back for lag compensation. true skips (only numbers past maxclients)
bool LagCompSkip1(X86Context& ctx, uint8_t*) {
    if (ctx.eax == ctx.edx) LOG_FIRST(1, "net: lag compensation includes the last player slot (entity %u)", ctx.eax);
    return int32_t(ctx.eax) > int32_t(ctx.edx);
}
bool LagCompSkip2(X86Context& ctx, uint8_t*) {
    return int32_t(ctx.edi) > int32_t(R32(0x636CE8));
}

// [[hook]] at 0x5C6BB in SV_DropClient (ebp the client, its reason formatted at esp+0x2C), after svc_disconnect went
// out: a network client's transport connection ends with the reason (the engine sent none)
void DropClientReason(X86Context& ctx, uint8_t*) {
    uint32_t cl = ctx.ebp;
    if (!NetworkClient(cl)) return;
    std::string reason = kernel::ReadCString(ctx.esp + 0x2C, 256);
    while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r')) reason.pop_back();
    // the engine's own texts (Host_ShutdownServer, SV_CheckTimeouts, kick) as the transport's reasons, so both ends
    // log the cause (Closed reads "closed by the peer")
    net::Reason r = reason == "Kicked"                 ? net::Reason::Kicked
                    : reason == "Server shutting down" ? net::Reason::ServerShutdown
                    : reason == "Timed out"            ? net::Reason::Timeout
                                                       : net::Reason::Closed;
    csx::NetDisconnectClient(AddressText(cl + 0x20 + 4), int(r), reason);
}

// [[hook]] at 0x982F6 in SV_New_f, after SV_SendServerinfo filled the signon message (a sizebuf at esp+0x14): a
// network client is sent the match's Player Collision (pc_collision.cpp), which its own prediction needs
void SignonPlayerCollision(X86Context& ctx, uint8_t*) {
    if (!NetworkClient(R32(kHostClient))) return;
    // and what this server serves of the PC port's own (N5C: Condition Zero's extra radio lines, pc_radio.cpp)
    // and its content mods (N8, common/mods.h), which every player of its matches needs
    std::string mods = csx::mods::ContentSignature();
    std::string cmd = std::string("pc_player_collision ") + (csx::PlayerCollisionOn() ? "1" : "0") +
                      "\npc_server_features radio2\npc_server_mods \"" + (mods.empty() ? "none" : mods) + "\"\n";
    uint32_t msg = ctx.esp + 0x14;
    uint32_t esp = ctx.esp, eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    ctx.esp -= 0x40;
    uint32_t p = kernel::CallGuest(ctx, kGetSpace, {msg, uint32_t(cmd.size() + 2)});
    ctx.esp = esp;
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    if (!p) return;
    kernel::G<uint8_t>(p) = kSvcStufftext;
    std::memcpy(kernel::HostPtr(p + 1), cmd.c_str(), cmd.size() + 1);
}

// [[hook]] at 0x98910, SV_RejectConnection's entry (edi the address; its format at [esp+4]), and 0x98A40, the bad
// password's refusal SV_CheckPassword (0x991D0) sends: the engine logs neither, so a refused player's cause was only
// on its own screen; the server's log names it ("net: 1.2.3.4:5 refused: ...")
void LogRejectConnection(X86Context& ctx, uint8_t*) {
    std::string why = kernel::ReadCString(R32(ctx.esp + 4), 200);
    while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
    LOGI("net: %s refused: %s", AddressText(ctx.edi).c_str(), why.c_str());
}
void LogRejectPassword(X86Context& ctx, uint8_t*) {
    LOGI("net: %s refused: bad password (sv_password)", AddressText(ctx.edi).c_str());
}
