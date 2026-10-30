// Game modes (N6C Deathmatch, N6D Gun Game, N6E the registry; docs/n6_modes.md). The match's mode comes with its
// command line ("+pc_gamemode classic|deathmatch|gungame": Custom Game's Match Options row, the party's Rules,
// csx-server's game_mode; pc_menu.cpp's PcConsoleCommand) and is kept until another is given. Everything here runs on
// the server (this game's code in csx-server, a listen server, Custom Game's own): a network client needs nothing.
//
// Classic is the title's own. The others are built on the game's rules object (CHalfLifeMultiplay; its Think 0x147620
// runs every server frame, hooked at 0x147639 with the object in esi):
//   - no round ends: the win check (CheckWinConditions 0x144E20: the teams' deaths, the bomb's explosion and defusal,
//     the hostages' rescue all end a round through it) returns at once (its own early exit, 0x144E44 -> 0x145E43),
//     and the round's time running out does nothing (0x147DC3 -> 0x1482A8). An objective still plays out (a planted
//     bomb explodes) but ends no round. The match ends by its time limit (mp_timelimit: Think's own check) or here;
//   - respawn: a player on a team, joined (player +0x1FC, the joining state, 0), dead (entvars deadflag +0x170 at
//     least DEAD_DEAD) for kRespawnDelay seconds spawns again (the player's Spawn, vtable +0: the team's spawn points,
//     the default items as at a round's start), bots alike;
//   - spawn protection: kProtect seconds without damage (entvars takedamage +0x16C 0) and a glow in the team's colour
//     (renderfx +0x15C glow shell), both put back after;
//   - Deathmatch: each spawn gives $16000 (AddAccount 0x11B710) and restarts the buy time (the rules' round start,
//     +0x30, set to now; the game's own check measures the buy time from it), so a player buys in the spawn's buy zone
//     after every death. The match ends when a player reaches the frag limit (pc_fraglimit; 0 none): the game's own
//     "end in a moment" (rules +0x2BC set to now: Think calls GoToIntermission, vtable +0x110, after its delay);
//   - Gun Game: a ladder of weapons (kWeapons in order, or pc_gungame_order's). Each spawn and each level strips the player (RemoveAllItems 0x118DB0,
//     the suit kept) and gives the knife and the level's weapon (GiveNamedItem 0x120720) with full ammunition (the
//     player's GiveAmmo, vtable +0x70); money is taken (nothing can be bought). A kill (the player's frags, entvars
//     +0x164, going up) is a level; a kill at the last level (the knife) wins the match, announced to everyone, and the
//     match ends as above. A team kill or a suicide takes a frag and no level.
// Messages go to the players as the game's centre texts (TextMsg, HUD_PRINTCENTER).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;

constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010, kClientEdict = 0x4B9C,
                   kClientFake = 0x2548, kClientName = 0x4D10;
constexpr uint32_t kGlobals = 0x527CE8;  // gpGlobals (time first)
constexpr uint32_t kMalloc = 0x22D86A;
constexpr uint32_t kRemoveAllItems = 0x118DB0, kGiveNamedItem = 0x120720, kAddAccount = 0x11B710;
constexpr uint32_t kMessageBegin = 0x527B58, kMessageEnd = 0x527B5C, kWriteByte = 0x527B60, kWriteString = 0x527B78,
                   kTextMsg = 0x5529EC;  // the engine's functions (pointers) and the game's TextMsg number
// entvars (edict +0x80)
constexpr uint32_t kRenderAmt = 0x14C, kRenderColor = 0x150, kRenderFx = 0x15C, kFrags = 0x164, kTakeDamage = 0x16C,
                   kDeadFlag = 0x170;
// the player
constexpr uint32_t kTeam = 0x1E0, kJoining = 0x1FC;
constexpr float kRespawnDelay = 2.5f, kProtect = 2.f, kEndDelay = 0.f;

struct Mode {
    const char* id;
    const char* label;
};
constexpr Mode kModes[] = {{"classic", "Classic"}, {"deathmatch", "Deathmatch"}, {"gungame", "Gun Game"}};

struct Weapon {
    uint32_t name;  // the game's own "weapon_..." text (checked before use)
    const char* text;
    uint32_t ammo;  // the game's ammunition name, 0 none
    int amount;
};
// the game's strings (default.xbe .rdata)
constexpr Weapon kWeapons[] = {
    {0x33E804, "weapon_glock18", 0x33ACE9, 120},   {0x33E814, "weapon_usp", 0x347280, 100},
    {0x33E7E8, "weapon_p228", 0x3468C4, 52},       {0x33E7F4, "weapon_deagle", 0x3468E0, 35},
    {0x33E7C4, "weapon_fiveseven", 0x3468CC, 100}, {0x33E7D8, "weapon_elite", 0x33ACE9, 120},
    {0x33E7B8, "weapon_m3", 0x346904, 32},         {0x33E7A8, "weapon_xm1014", 0x346904, 32},
    {0x33E78C, "weapon_tmp", 0x33ACE9, 120},       {0x33E770, "weapon_mac10", 0x347280, 100},
    {0x33E798, "weapon_mp5navy", 0x33ACE9, 120},   {0x33E760, "weapon_ump45", 0x347280, 100},
    {0x33E780, "weapon_p90", 0x3468CC, 100},       {0x33E6E0, "weapon_galil", 0x3468FC, 90},
    {0x33E6D0, "weapon_famas", 0x3468FC, 90},      {0x33E754, "weapon_ak47", 0x3468E8, 90},
    {0x33E738, "weapon_m4a1", 0x3468FC, 90},       {0x33E744, "weapon_sg552", 0x3468FC, 90},
    {0x33E72C, "weapon_aug", 0x3468FC, 90},        {0x33E71C, "weapon_scout", 0x3468E8, 90},
    {0x33E710, "weapon_awp", 0x3468D4, 30},        {0x33E6C4, "weapon_m249", 0x3468F0, 200},
    {0x347288, "weapon_knife", 0, 0},
};
constexpr const Weapon& kKnife = kWeapons[sizeof(kWeapons) / sizeof(kWeapons[0]) - 1];

// the ladder (pc_gungame_order; the default: kWeapons in order), the knife always last
std::vector<const Weapon*> DefaultLadder() {
    std::vector<const Weapon*> l;
    for (const Weapon& w : kWeapons) l.push_back(&w);
    return l;
}
std::vector<const Weapon*> g_ladder = DefaultLadder();
int Levels() { return int(g_ladder.size()); }
const Weapon& Level(int i) { return *g_ladder[size_t(std::clamp(i, 0, Levels() - 1))]; }

std::string g_mode = "classic";
int g_fraglimit = 0;
uint32_t g_rules = 0;  // the rules object Think last ran for
bool g_ended = false;  // the match's end asked for (until the next map)

struct Player {
    uint32_t edict = 0;
    bool dead = false;
    float dead_since = 0, protect_until = 0;
    bool protecting = false;
    float fx = 0, amt = 0;
    float color[3] = {};
    int level = 0;
    float frags = 0;
    bool armed = false;  // Gun Game: given its level's weapon since its spawn
};
std::array<Player, 65> g_players;

float Now() { return kernel::G<float>(R32(kGlobals)); }
float& F(uint32_t at) { return kernel::G<float>(at); }

const Mode* FindMode(const std::string& id) {
    for (const Mode& m : kModes)
        if (id == m.id) return &m;
    return nullptr;
}

// guest calls with registers of their own: the hook's context put back after
struct Saved {
    X86Context& ctx;
    X86Context copy;
    explicit Saved(X86Context& c) : ctx(c), copy(c) { ctx.esp -= 0x80; }
    ~Saved() { ctx = copy; }
};

// the game's centre text to one player's edict, or everyone (0)
void Centre(X86Context& ctx, uint32_t edict, const std::string& text) {
    static uint32_t buffer = 0;  // one guest buffer, reused (the message is written at once)
    if (!buffer) buffer = kernel::CallGuest(ctx, kMalloc, {256});
    if (!buffer) return;
    Saved saved(ctx);
    std::string t = text.substr(0, 250);
    std::memcpy(kernel::HostPtr(buffer), t.c_str(), t.size() + 1);
    kernel::CallGuest(ctx, R32(kMessageBegin), {edict ? 1u : 2u, R32(kTextMsg), 0, edict});
    kernel::CallGuest(ctx, R32(kWriteByte), {4});  // HUD_PRINTCENTER
    kernel::CallGuest(ctx, R32(kWriteString), {buffer});
    kernel::CallGuest(ctx, R32(kMessageEnd), {});
}

void Give(X86Context& ctx, uint32_t player, const Weapon& w) {
    if (kernel::ReadCString(w.name, 32) != w.text) {
        LOG_FIRST(8, "pc mode: %s not at %08X, not given", w.text, w.name);
        return;
    }
    {
        Saved s(ctx);
        ctx.eax = w.name;
        ctx.edi = player;
        kernel::CallGuest(ctx, kGiveNamedItem, {});
    }
    if (!w.ammo) return;
    Saved s(ctx);
    ctx.ecx = player;
    kernel::CallGuest(ctx, R32(R32(player) + 0x70), {uint32_t(w.amount), w.ammo, uint32_t(w.amount)});
}

void Arm(X86Context& ctx, uint32_t player, int level) {
    {
        Saved s(ctx);
        ctx.eax = player;
        kernel::CallGuest(ctx, kRemoveAllItems, {0});  // the suit kept
    }
    if (level < Levels() - 1) Give(ctx, player, kKnife);
    Give(ctx, player, Level(level));
}

void SetMoney(X86Context& ctx, uint32_t player, int32_t money) {
    int32_t have = int32_t(R32(player + 0x1E4));
    if (have == money) return;
    Saved s(ctx);
    ctx.esi = player;
    ctx.eax = uint32_t(money - have);
    kernel::CallGuest(ctx, kAddAccount, {1});
}

std::string Name(uint32_t client) { return kernel::ReadCString(client + kClientName, 32); }

void EndMatch(const std::string& why) {
    if (g_ended || !g_rules) return;
    g_ended = true;
    F(g_rules + 0x2BC) = Now() + kEndDelay;  // Think: GoToIntermission after the game's delay
    LOGI("pc mode: the match ends (%s)", why.c_str());
}

void Spawned(X86Context& ctx, uint32_t client, uint32_t player, uint32_t pev, Player& p) {
    p.protect_until = Now() + kProtect;
    if (!p.protecting) {
        p.fx = F(pev + kRenderFx);
        p.amt = F(pev + kRenderAmt);
        for (int i = 0; i < 3; i++) p.color[i] = F(pev + kRenderColor + 4 * i);
    }
    p.protecting = true;
    F(pev + kTakeDamage) = 0;  // DAMAGE_NO
    F(pev + kRenderFx) = 19;   // kRenderFxGlowShell
    F(pev + kRenderAmt) = 20;
    bool ct = R32(player + kTeam) == 2;
    F(pev + kRenderColor) = ct ? 60.f : 200.f;
    F(pev + kRenderColor + 4) = ct ? 120.f : 60.f;
    F(pev + kRenderColor + 8) = ct ? 255.f : 40.f;
    if (g_mode == "deathmatch") {
        SetMoney(ctx, player, 16000);
        F(g_rules + 0x30) = Now();  // the buy time from now
    } else if (g_mode == "gungame") {
        SetMoney(ctx, player, 0);
        Arm(ctx, player, p.level);
        p.armed = true;
        if (!R32(client + kClientFake))
            Centre(ctx, R32(client + kClientEdict),
                   "Gun Game: level " + std::to_string(p.level + 1) + " of " + std::to_string(Levels()) + ", " +
                       (Level(p.level).text + 7));
    }
}

void Frame(X86Context& ctx) {
    uint32_t n = R32(kMaxClients);
    float now = Now();
    for (uint32_t i = 0; i < n && i < g_players.size(); i++) {
        uint32_t client = R32(kClients) + i * kClientSize;
        Player& p = g_players[i];
        uint32_t edict = R32(client) ? R32(client + kClientEdict) : 0;
        uint32_t player = edict ? R32(edict + 0x7C) : 0;
        if (!player || edict != p.edict) {  // a slot emptied or another player in it
            p = Player();
            p.edict = player ? edict : 0;
            if (!player) continue;
            p.frags = F(edict + 0x80 + kFrags);
        }
        uint32_t pev = edict + 0x80;
        uint32_t team = R32(player + kTeam);
        bool joined = (team == 1 || team == 2) && R32(player + kJoining) == 0;
        int32_t dead = int32_t(R32(pev + kDeadFlag));
        float frags = F(pev + kFrags);
        if (g_mode == "gungame" && joined && frags > p.frags) {
            int gained = int(std::lround(frags - p.frags));
            if (p.level >= Levels() - 1) {
                std::string who = Name(client);
                Centre(ctx, 0, who + " wins Gun Game!");
                EndMatch(who + " won Gun Game");
            } else {
                p.level = std::min(Levels() - 1, p.level + std::max(1, gained));
                LOGI("pc mode: %s reaches level %d (%s)", Name(client).c_str(), p.level + 1, Level(p.level).text);
                if (dead == 0) {
                    Arm(ctx, player, p.level);
                    if (!R32(client + kClientFake))
                        Centre(ctx, edict, "Level " + std::to_string(p.level + 1) + " of " + std::to_string(Levels()) +
                                               ": " + (Level(p.level).text + 7));
                }
            }
        }
        p.frags = frags;
        if (g_mode == "deathmatch" && g_fraglimit > 0 && frags >= float(g_fraglimit))
            EndMatch(Name(client) + " reached the frag limit, " + std::to_string(g_fraglimit));
        if (p.protecting && (now >= p.protect_until || dead)) {
            p.protecting = false;
            if (!dead) F(pev + kTakeDamage) = 2;  // DAMAGE_AIM, a player's own
            F(pev + kRenderFx) = p.fx;
            F(pev + kRenderAmt) = p.amt;
            for (int k = 0; k < 3; k++) F(pev + kRenderColor + 4 * k) = p.color[k];
        }
        if (!joined) continue;
        if (dead == 0) {
            if (p.dead) {  // spawned again (by the game: a round's restart, or here)
                p.dead = false;
                Spawned(ctx, client, player, pev, p);
            } else if (g_mode == "gungame" && !p.armed) {  // alive since before the mode saw it
                SetMoney(ctx, player, 0);
                Arm(ctx, player, p.level);
                p.armed = true;
            }
            continue;
        }
        if (!p.dead) {
            p.dead = true;
            p.armed = false;
            p.dead_since = now;
        }
        if (dead < 2 || now < p.dead_since + kRespawnDelay || g_ended) continue;
        {
            Saved s(ctx);
            ctx.ecx = player;
            kernel::CallGuest(ctx, R32(R32(player)), {});  // Spawn
        }
        if (int32_t(R32(pev + kDeadFlag)) == 0) {
            p.dead = false;
            LOG_FIRST(64, "pc mode: %s respawned (%s)", Name(client).c_str(), g_mode.c_str());
            Spawned(ctx, client, player, pev, p);
        } else {
            p.dead_since = now;  // refused: tried again after the delay
            LOG_FIRST(16, "pc mode: %s's respawn refused (deadflag %d)", Name(client).c_str(),
                      int(R32(pev + kDeadFlag)));
        }
    }
}

}  // namespace

// "pc_gamemode classic|deathmatch|gungame" (pc_menu.cpp's PcConsoleCommand): the match's command line, a server's
// console
void PcGameModeCommand(const std::string& arg) {
    const Mode* m = FindMode(arg);
    if (!m) {
        LOGW("pc mode: no game mode \"%s\" (classic, deathmatch, gungame)", arg.c_str());
        return;
    }
    if (g_mode != m->id) {
        g_players.fill(Player());
        g_ended = false;
    }
    g_mode = m->id;
    LOGI("pc mode: %s", m->label);
}

// "pc_gungame_order NAME NAME ...": Gun Game's ladder, weapon names without "weapon_" (commas or spaces between),
// the knife added last when not given; "default" (or nothing) the default ladder. A name not known refuses the order
void PcGunGameOrderCommand(const std::string& arg) {
    std::string text = arg;
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    std::vector<const Weapon*> ladder;
    for (std::string w; in >> w;) {
        if (w == "default") {
            ladder.clear();
            break;
        }
        const Weapon* found = nullptr;
        for (const Weapon& k : kWeapons)
            if (w == k.text + 7 || w == k.text) found = &k;
        if (!found) {
            LOGW("pc mode: Gun Game ladder refused: no weapon \"%s\" (glock18 usp p228 deagle fiveseven elite m3 xm1014 "
                 "tmp mac10 mp5navy ump45 p90 galil famas ak47 m4a1 sg552 aug scout awp m249 knife)", w.c_str());
            return;
        }
        if (found == &kKnife) continue;  // always last, once
        ladder.push_back(found);
    }
    ladder.push_back(&kKnife);
    g_ladder = ladder.size() > 1 ? ladder : DefaultLadder();
    std::string names;
    for (const Weapon* w : g_ladder) names += std::string(names.empty() ? "" : " ") + (w->text + 7);
    LOGI("pc mode: Gun Game ladder: %s (%d levels)", names.c_str(), Levels());
}

// "pc_fraglimit N": Deathmatch's frag limit (0 none)
void PcFragLimitCommand(const std::string& arg) {
    g_fraglimit = std::clamp(std::atoi(arg.c_str()), 0, 999);
    LOGI("pc mode: frag limit %d", g_fraglimit);
}

namespace csx {
std::string GameMode() { return g_mode; }
}  // namespace csx

void PcGameplayThink();  // pc_gameplay.cpp

// [[hook]] at 0x147639 (config/game.toml), the rules' Think (0x147620) with the rules object in esi: every server frame
void PcModesThink(X86Context& ctx, uint8_t*) {
    if (g_rules != ctx.esi) {  // a new map's rules
        g_rules = ctx.esi;
        g_ended = false;
        g_players.fill(Player());
    }
    PcGameplayThink();
    if (g_mode == "classic") return;
    Frame(ctx);
}

// [[hook]] at 0x144E44 in CheckWinConditions: true returns (no round is won or lost but in Classic)
bool PcModesNoRoundWin(X86Context&, uint8_t*) { return g_mode != "classic"; }

// [[hook]] at 0x147DC3 in Think, the round's time out: true leaves the round running
bool PcModesNoRoundTime(X86Context&, uint8_t*) { return g_mode != "classic"; }

// Map rotation (N9, csx-server's map_rotation, the server GUI's): "pc_rotation MAP,MAP,..." on the server. When a
// match ends the game's ChangeLevel (CHalfLifeMultiplay's: the next map from mapcycle.txt, the same map
// without one) changes to the map in its buffer (CHANGE_LEVEL at 0x14A8BA, the name at [esp]); with a rotation the
// name written there is the one after the current map (the first when the current is not in it). Empty: the
// game's own.
namespace {
std::vector<std::string> g_rotation;
constexpr uint32_t kSvName = 0x5EF7F0;
}  // namespace

void PcRotationCommand(const std::string& arg) {
    std::string text = arg;
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    std::vector<std::string> maps;
    for (std::string m; in >> m;) {
        if (m.size() > 31 || m.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            LOGW("pc rotation: \"%s\" is not a map name: the rotation is unchanged", m.c_str());
            return;
        }
        maps.push_back(m);
    }
    g_rotation = maps;
    LOGI("pc rotation: %s", maps.empty() ? "none (the game's own)" : text.c_str());
}

// [[hook]] at 0x14A8BA (config/game.toml): CHANGE_LEVEL about to run with the next map's name at [esp]
void PcRotationNext(X86Context& ctx, uint8_t*) {
    if (g_rotation.empty()) return;
    uint32_t buf = kernel::R32(ctx.esp);
    std::string now = kernel::ReadCString(kSvName, 64);
    size_t i = 0;
    for (size_t k = 0; k < g_rotation.size(); k++)
        if (g_rotation[k] == now) i = (k + 1) % g_rotation.size();
    const std::string& next = g_rotation[i];
    std::memcpy(kernel::HostPtr(buf), next.c_str(), next.size() + 1);
    LOGI("pc rotation: %s -> %s", now.c_str(), next.c_str());
}
