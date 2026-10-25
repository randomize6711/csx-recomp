// The engine's network security for PC multiplayer (docs/pc_multiplayer.md "N1 security"; the findings in
// analysis/netcode_engine.md). The transport below (src/net) already refuses spoofed sources, replays and malformed
// datagrams; these close the engine's own gaps:
//   - the connect challenge is checked (SV_CheckChallenge 0x98CE0 returned 1 on every path)
//   - the rcon challenge is checked (it was never read)
//   - the info replies no longer carry the session's Xbox key, key id and host address
//   - a network client's userinfo loses any "*" key (the server's own: *bot, *pid) and gains *pid, its player
//     identity's fingerprint, when it proved one
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

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
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

// what a network server may run on its clients through svc_stufftext: what Counter-Strike's server sends (found in
// play and in the game DLL's CLIENT_COMMAND strings) and the PC port's own; anything else is logged and dropped
const std::set<std::string> kStuffAllowed = {
    "reconnect", "retry", "disconnect", "fullserverinfo", "echo", "spk", "speak", "play", "playvol", "mp3", "cd",
    "lastinv", "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8", "slot9", "slot10",
    "-attack", "-attack2", "+attack", "-use", "-jump", "-duck", "-reload", "-speed", "-forward", "-back",
    "-moveleft", "-moveright", "-left", "-right", "-lookup", "-lookdown", "-strafe", "-showscores",
    "cl_minmodels", "spec_mode", "spec_pip", "spec_autodirector", "spec_drawnames", "spec_drawcone", "spec_drawstatus",
    "hud_takesshots", "hud_centerid", "cl_righthand", "cl_corpsestay", "voice_modenable", "voice_scale",
    "room_type", "say", "say_team", "fov", "default_fov", "hideradar", "drawradar", "unpause", "pc_player_collision",
    "firstperson", "thirdperson", "bot_menu", "menuselect", "showbriefing", "chooseteam", "buy", "buyequip",
    "cl_dlmax", "cl_lc", "cl_lw", "cl_updaterate", "rate", "cl_cmdrate", "ex_interp", "gl_spriteblend",
};

// the commands of one stufftext line split as Cbuf_Execute splits them (';' and newlines, outside quotes)
std::string FilterStuff(const std::string& text, std::vector<std::string>& dropped) {
    std::string out, cmd;
    bool quoted = false;
    auto flush = [&](char sep) {
        std::string c = cmd;
        cmd.clear();
        size_t a = c.find_first_not_of(" \t");
        if (a == std::string::npos) {
            if (sep) out += sep;
            return;
        }
        size_t e = c.find_first_of(" \t", a);
        std::string name = c.substr(a, e == std::string::npos ? std::string::npos : e - a);
        std::string lower = name;
        for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (kStuffAllowed.count(lower)) {
            out += c;
            if (sep) out += sep;
        } else {
            dropped.push_back(c.substr(a));
        }
    };
    for (char ch : text) {
        if (ch == '"') quoted = !quoted;
        if (!quoted && (ch == ';' || ch == '\n')) {
            flush(ch);
            continue;
        }
        cmd += ch;
    }
    flush(0);
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

// [[hook]] at 0x9BAC9 in the rcon check (0x9BA70), after the password tests: the rcon challenge (Cmd_Argv(1)) must
// be the one net_from was given ("challenge rcon"); true goes to the refusal (0x9BBA6)
bool RconChallenge(X86Context&, uint8_t*) {
    int64_t want = ChallengeFor(kNetFrom);
    std::string arg = Argv(1);
    bool ok = R32(kNetFrom) == kNaLoopback || (want >= 0 && !arg.empty() && std::strtoul(arg.c_str(), nullptr, 10) == want);
    if (!ok) LOG_FIRST(16, "net: rcon from %s refused: bad challenge", AddressText(kNetFrom).c_str());
    return !ok;
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
// a network client's "*" keys are removed (setinfo refuses them; the connect line did not), its identity added
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
    if (auto id = csx::NetPeerIdentity(peer)) out += "\\*pid\\" + *id;
    if (out.size() > 0x3FE) out.resize(0x3FE);
    std::memcpy(kernel::HostPtr(buf), out.c_str(), out.size() + 1);
    if (stripped) LOGW("net: %s sent %d \"*\" userinfo keys, removed", peer.c_str(), stripped);
}

// [[hook]] at 0xAB4ED in SV_ParseStringCommand (0xAB4E0: esi the command, eax SV_IsClientCommandAllowed's verdict, 1 an
// engine command): a network client may not kick, pause or use the cheats (without sv_cheats), which the engine let
// any client run; true ends the command (0xAB541)
bool ClientCommandAllowed(X86Context& ctx, uint8_t*) {
    if (ctx.eax != 1 || !NetworkClient(R32(kHostClient))) return false;
    std::string c = kernel::ReadCString(ctx.esi, 128);
    std::string name = c.substr(0, c.find_first_of(" \t"));
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
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
    std::string kept = FilterStuff(text, dropped);
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
    std::string cmd = std::string("pc_player_collision ") + (csx::PlayerCollisionOn() ? "1" : "0") + "\n";
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
