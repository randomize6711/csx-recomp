// Native split-screen's local players in the game (docs/native_splitscreen.md, M2): two to four logical clients of the
// built-in (listen) server in this one process, each in its own client context (client_context.cpp), joined over the
// engine's loopback with a channel per player (local_loopback.h).
//
// Off unless CSX_NATIVE_SPLIT=N (2..4 local players). Off, the game runs as before: the loopback, the registrations
// and the heap are the title's; the address hooks below compare loopback ports, which are all 0 then, so they change
// nothing.
//
// On:
//   - the heap: every guest heap block's owner (the player whose turn or initialisation allocated it, else the
//     process) and kind (from the code that allocated it, kSiteRules: a player's object, a shared registry, shared
//     scratch) are tracked (heap_owner.h); a free or a destruction of another player's owned object is refused and
//     logged as an error, before the destructor runs where the code is known (the pre-destructor checks below);
//   - the loopback: NET_SendPacket (0x6CC70) and NET_GetLoopPacket (0x6BAF0) use the host's channels: the active
//     player's client to the server, the server to the player its address's port names (the port is the player's
//     slot; player 1's is 0, the address it always had); NET_CompareAdr (0x6B3C0) and SV_ReadPackets (0x9C722) compare
//     a loopback address's port, so each local player is its own server client;
//   - a player added (between frames, once player 1 is in its match), by explicit steps (docs 13.2): a context from
//     the boot image (no heap object in it: nothing of player 1's to inherit); in it, CL_Init's per-player state (the
//     list heads and datagram buffer CL_Init sets, cls.state disconnected, player 1's userinfo text with its own name),
//     then the client game's Initialize (0x18B7A0) and HUD_Init (0x18B800), the registrations they repeat answered by
//     the first player's (a cvar: the registered one; the engine's Cmd_AddCommand, fatal after host_initialized:
//     skipped; commands and message hooks are already idempotent); then checked against player 1's state at the end of
//     Host_Init (every object player 1's initialisation made must be its own; nothing inherited); then its client
//     connects to the built-in server on its own turn, the shared registries of the connect built once (the delta
//     descriptions: a player after the first parses them into its own slot);
//   - a player removed: its server slot dropped (SV_DropClient, not CL_Disconnect, which would shut the server down),
//     its client entities freed, its packets forgotten, its context removed;
//   - until M3 / M4: an added player draws nothing (SCR_UpdateScreen skipped in its turn), reads no pad or mouse (the
//     look routine 0x19E160 and the pad keys 0x19DFD0 skipped in its turn) and starts no sound (0x100F10 skipped):
//     the window, the controls and the speakers stay player 1's.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "common/guest_tables_layout.h"
#include "game/client_context.h"
#include "game/heap_owner.h"
#include "game/local_loopback.h"
#include "game/native_split_core.h"
#include "game/net_buffers.h"
#include "game/net_engine.h"
#include "game/shared_once.h"
#include "kernel/kernel.h"

namespace dispatch {
X86Func* Find(uint32_t guest);  // the recompiler's function entries (sdk/runtime/cpu/dispatch.cpp)
}

namespace {
using kernel::R16;
using kernel::R32;
using kernel::W16;
using kernel::W32;

// engine addresses (analysis/netcode_engine.md, docs/native_splitscreen.md M2)
constexpr uint32_t kClsState = 0x7812E0;          // cls.state: 1 disconnected, 2 connecting, 3 connected, 5 active
constexpr uint32_t kClsUserinfo = 0x785148;       // cls.userinfo[256]
constexpr uint32_t kSvActive = 0x5EF7C0;          // sv.active
constexpr uint32_t kNetFrom = 0x6BA5E0;           // net_from (netadr_t: type +0, port +0x12)
constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010;  // svs.clients
constexpr uint32_t kClientEntities = 0x551890;    // cl_entities (malloc'd at the connect, 0x2E2E0)
constexpr uint32_t kCvarFindVar = 0x41F20;        // Cvar_FindVar (cdecl name): cvar_t*
constexpr uint32_t kInfoSetValue = 0x65760;       // Info_SetValueForKey (cdecl s, key, value, maxsize)
constexpr uint32_t kDropClient = 0x5C600;         // SV_DropClient (cdecl client, crash, format, ...)
constexpr uint32_t kFree = 0x22C609;              // the C runtime's free (cdecl)
constexpr uint32_t kClientInitialize = 0x18B7A0;  // the client game's Initialize (cdecl engine functions, version)
constexpr uint32_t kHudInit = 0x18B800;           // the client game's HUD_Init
constexpr uint32_t kEngineFunctions = 0x371610;   // the engine's cl_enginefunc_t table, as Host_Init hands it (0x1C69F)
constexpr uint32_t kLoopback = 1;                 // netadrtype_t NA_LOOPBACK
constexpr int kClsActive = 5, kClsDisconnected = 1;

// the kinds of allocations by the code that made them (heap_owner.h Kind; docs 13.4): a site is malloc's caller's
// return address (or calloc's). Anything else is owned: by the player whose turn or initialisation allocated it, else
// by the process.
constexpr heapown::SiteRule kSiteRules[] = {
    {0x1BF2A, 0x1BF2B, heapown::Kind::SharedRegistry, "a cvar_t the client's pfnRegisterVariable (0x1BEF0) registers"},
    {0x2C716, 0x2C717, heapown::Kind::SharedRegistry, "a user message entry (HookUserMsg 0x2C690)"},
    {0x3C92A, 0x3C92B, heapown::Kind::SharedRegistry, "a command entry (the client's command registration 0x3C8C0)"},
    {0x1CB0D, 0x1CB0E, heapown::Kind::SharedRegistry, "the HUD sprite registry (ClientDLL_HudVidInit 0x1CAC0)"},
    {0x2FB00, 0x2FCB2, heapown::Kind::SharedRegistry, "a delta description (svc_deltadescription 0x2FAE0)"},
    {0x44800, 0x44A00, heapown::Kind::SharedRegistry, "a delta description's fields (0x448F0)"},
    {0x3C70D, 0x3C70E, heapown::Kind::SharedScratch, "a command argument (Cmd_TokenizeString 0x3C620)"},
    {0x3C7EE, 0x3C7EF, heapown::Kind::SharedScratch, "a command argument (Cmd_TokenizeString 0x3C620)"},
    {0x42000, 0x43000, heapown::Kind::SharedScratch, "a cvar's value string (cvar.c)"},
    {0x931A5, 0x931A6, heapown::Kind::SharedScratch, "the renderer's transparent-entity list (0x93180, every connect)"},
    {0xC8F30, 0xC8FA0, heapown::Kind::SharedScratch, "a memory pool's entry (0xC8F30; pools 0x55231C / 0x552324 are "
                                                       "released by level, 0xC8B10, by whoever runs: the loading)"},
    {0xC9130, 0xC9170, heapown::Kind::SharedScratch, "a memory pool's entry with its 16-byte header (0xC9130, pool "
                                                       "0x55232C; the caller gets the block + 0x10)"},
};
constexpr size_t kSiteRuleCount = sizeof(kSiteRules) / sizeof(kSiteRules[0]);
heapown::Kind KindOf(uint32_t site) { return heapown::KindOfSite(site, kSiteRules, kSiteRuleCount); }

int Wanted() {
    static const int n = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT");
        int x = v ? std::atoi(v) : 0;
        return x >= 2 && x <= nsplit::kMaxPlayers && !csx::Dedicated() ? x : 0;
    }();
    return n;
}
bool GuardOn() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_CTX_GUARD");
        return v && *v == '1';
    }();
    return on;
}
// diagnostics: remove the last added player after this many seconds in the match, add one again after as many more
double LeaveAfter() {
    static const double s = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_LEAVE_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    return s;
}

// the image as the start-up check knows it (docs 14.6): the recompiler's function entries, the static addresses the
// code takes, and the protected start-up state (generated by tools/region_model.py)
constexpr uint32_t kTakenAddresses[] = {
#include "game/client_context_addresses.inc"
};
struct ProtectedRange {
    uint32_t va, size;
    const char* what;
};
constexpr ProtectedRange kProtected[] = {
#include "game/client_context_protected.inc"
};
bool IsFunctionEntry(uint32_t v) { return dispatch::Find(v) != nullptr; }
uint8_t GuestByte(uint32_t va) { return *static_cast<const uint8_t*>(kernel::HostPtr(va)); }
std::vector<heapown::Range> ProtectedRanges() {
    std::vector<heapown::Range> out;
    for (const ProtectedRange& p : kProtected) out.push_back({p.va, p.size});
    return out;
}
const char* ProtectedWhat(uint32_t va) {
    for (const ProtectedRange& p : kProtected)
        if (va >= p.va && va - p.va < p.size) return p.what;
    return "?";
}
// .data outside the player's regions: the shared state a new player's steps may change (reported, not failed:
// the shared registries and services legitimately change; the protected part above fails)
std::vector<heapown::Range> SharedDataRanges(const std::vector<heapown::Span>& spans) {
    constexpr uint32_t kDataLo = 0x36C8A0, kDataHi = 0x7D6F20;
    std::vector<std::pair<uint32_t, uint32_t>> held;
    for (const heapown::Span& sp : spans) held.push_back({sp.va, sp.va + sp.size});
    std::sort(held.begin(), held.end());
    std::vector<heapown::Range> out;
    uint32_t at = kDataLo;
    for (auto [lo, hi] : held) {
        if (hi <= at || lo >= kDataHi) continue;
        if (lo > at) out.push_back({at, lo - at});
        at = std::max(at, hi);
    }
    if (at < kDataHi) out.push_back({at, kDataHi - at});
    return out;
}

loopnet::Channels g_ch;
heapown::Blocks g_blocks;
nsplit::Players g_players;
nsplit::Session g_session;   // the built-in server's host (docs 15): its disconnect ends the match
sharedonce::World g_world;   // the world's shared-once operations, by map generation (the process's, docs 15)
bool g_tracking = false;   // heap ownership tracked (native split-screen on, from the process's start)
int g_init_owner = -1;     // the player whose initialisation is running (its allocations are its own), -1 none
uint64_t g_foreign_frees = 0, g_skipped_frees = 0;
bool g_add_failed = false;  // a player's initialisation failed its checks: no further players added this session
std::vector<uint8_t> g_reference;         // player 1's regions at the end of Host_Init (the check of a new player)
std::vector<heapown::Pointer> g_reference_objects;  // the heap pointers in it: the objects a new player must have
// per player: packets its client sent to the server, the server sent to it, it read, the server read from it; packets
// a client read outside its own turn (should never happen)
uint64_t g_sent[nsplit::kMaxPlayers], g_to[nsplit::kMaxPlayers], g_read[nsplit::kMaxPlayers], g_server_read[nsplit::kMaxPlayers];
uint64_t g_read_outside_turn = 0;
std::chrono::steady_clock::time_point g_last_report{};
constexpr uint32_t kClPlayernum = 0x771224;  // cl.playernum: the slot the server gave this client
std::chrono::steady_clock::time_point g_joined_at{}, g_left_at{};

// the player a context belongs to (players' slots and contexts are kept in step: slot k may have context < k after a
// removal), -1 none
int PlayerOfContext(int context) {
    for (int k = 0; k < nsplit::kMaxPlayers; k++)
        if (g_players.At(k).state != nsplit::State::Empty && g_players.At(k).context == context) return k;
    return -1;
}

// whose allocation this is (a player's slot): the player initialising, the player whose turn it is, else the process
int OwnerNow() {
    if (g_world.Running()) return heapown::kProcess;  // a shared-once operation: the world's, whoever runs it
    if (g_init_owner > 0) return g_init_owner;
    if (csx::ClientContextInTurn()) {
        int k = PlayerOfContext(csx::ClientContextActive());
        return k >= 0 ? k : heapown::kProcess;
    }
    return heapown::kProcess;
}

// strings for a guest call, put below the stack (as server_admin.cpp's Drop): their addresses; esp moved below them
struct GuestStrings {
    X86Context& ctx;
    uint32_t esp;
    explicit GuestStrings(X86Context& c) : ctx(c), esp(c.esp) {}
    uint32_t Put(const std::string& s) {
        uint32_t at = (ctx.esp - 0x100 - uint32_t(s.size())) & ~15u;
        std::memcpy(kernel::HostPtr(at), s.c_str(), s.size() + 1);
        ctx.esp = at - 16;
        return at;
    }
    ~GuestStrings() { ctx.esp = esp; }
};

// the regions laid end to end, as a context's copy is (the switcher's order)
std::vector<heapown::Span> Spans() {
    std::vector<heapown::Span> spans;
    size_t at = 0;
    for (const ctx::Region& r : csx::ClientContextRegions()) {
        spans.push_back({at, r.start, r.size});
        at += r.size;
    }
    return spans;
}

// CL_Init's per-player state (0x2BA10, its writes at 0x2C141..0x2C1A6 and the userinfo keys 0x2BA3C..0x2BBA4; docs
// 13.1): what the process's initialisation writes into a player's regions that no per-player function redoes. Every
// value is an address inside the player's own regions or a constant, the same for every player.
void ClientInitState() {
    W32(0x749DC4, 0x749D44);  // two list heads of cl pointing at themselves (0x2C15E..0x2C168)
    W32(0x749DC8, 0x749D44);
    W32(0x749E4C, 0x749DCC);  // (0x2C14F..0x2C159)
    W32(0x749E50, 0x749DCC);
    W32(0x783808, 0x32AFF4);  // cls.datagram: its name, flags 0, data = its buffer 0x78381C, maxsize 4000, cursize 0
    W16(0x78380C, 0);
    W32(0x783810, 0x78381C);
    W32(0x783814, 0xFA0);
    W32(0x783818, 0);
    W32(0x785BA0, 0);         // (0x2C1A0 / 0x2C1A6)
    W32(0x785BA4, 0);
    W32(kClsState, kClsDisconnected);  // SV_SetMaxclients (0xA19E4): a client that is not a dedicated server's
}

void AddPlayer(X86Context& ctx, int k) {
    std::string name = "Player " + std::to_string(k + 1);
    // player 1's userinfo text (keys CL_Init set from the settings: plain text, no pointer), before switching
    std::vector<uint8_t> userinfo(0x100);
    std::memcpy(userinfo.data(), kernel::HostPtr(kClsUserinfo), userinfo.size());
    int context = csx::ClientContextAdd();  // the boot image
    if (context <= 0) {
        LOGE("native split: no context for player %d (the region list failed its checks)", k + 1);
        return;
    }
    if (!g_players.Begin(k, context, name)) {
        LOGE("native split: player %d cannot start (slot %s)", k + 1, nsplit::Name(g_players.At(k).state));
        csx::ClientContextRemove(context);
        return;
    }
    std::vector<heapown::Span> spans = Spans();
    size_t inherited = 0, own = 0, missing = 0, foreign = 0, unlinked = 0, values = 0, corrupted = 0;
    std::vector<heapown::Range> protect = ProtectedRanges(), shared = SharedDataRanges(spans);
    std::vector<uint8_t> protect_before, shared_before;
    auto t0 = std::chrono::steady_clock::now();
    for (nsplit::InitStep step : nsplit::InitOrder()) {
        switch (step) {
        case nsplit::InitStep::BootImage:
            // the boot image holds no heap pointer: checked, not assumed
            inherited = heapown::HeapPointers(csx::ClientContextSaved(context), spans, g_blocks).size();
            if (inherited)
                LOGE("native split: player %d's boot image holds %zu heap pointers (it must hold none)", k + 1, inherited);
            csx::ClientContextSwitch(context);
            // the shared state as it is before this player's steps (the regions now hold the new player's)
            protect_before = heapown::Snapshot(protect, GuestByte);
            shared_before = heapown::Snapshot(shared, GuestByte);
            g_init_owner = k;
            break;
        case nsplit::InitStep::ClientState:
            ClientInitState();
            break;
        case nsplit::InitStep::Userinfo: {
            std::memcpy(kernel::HostPtr(kClsUserinfo), userinfo.data(), userinfo.size());
            GuestStrings gs(ctx);
            uint32_t key = gs.Put("name"), value = gs.Put(name);
            kernel::CallGuest(ctx, kInfoSetValue, {kClsUserinfo, key, value, 0x100});
            break;
        }
        case nsplit::InitStep::Initialize:
            kernel::CallGuest(ctx, kClientInitialize, {kEngineFunctions, 7});
            break;
        case nsplit::InitStep::HudInit:
            kernel::CallGuest(ctx, kHudInit, {});
            g_init_owner = -1;
            break;
        case nsplit::InitStep::Check:
            // against player 1's state at the end of Host_Init: every object player 1's initialisation made (a heap
            // pointer of its regions then) must be the new player's own, or a shared registry's entry; one it lacks,
            // or one naming another's object, fails the initialisation (an initialisation step missing, an object
            // shared)
            for (const heapown::Pointer& p : g_reference_objects) {
                uint32_t mine = R32(p.va);
                const heapown::Block* b = mine ? g_blocks.At(g_blocks.Holding(mine)) : nullptr;
                if (!mine) {
                    missing++;
                    LOGE("native split: player %d has no object at 0x%08X (player 1's initialisation made one: block "
                         "0x%08X, %s)",
                         k + 1, p.va, p.block,
                         g_blocks.At(p.block) ? heapown::Name(g_blocks.At(p.block)->kind) : "freed since");
                } else if (b && (b->kind != heapown::Kind::Owned || b->owner == k)) {
                    own++;
                } else {
                    foreign++;
                    LOGE("native split: player %d's 0x%08X names 0x%08X, not its own (owner %d, %s)", k + 1, p.va, mine,
                         b ? b->owner : -2, b ? heapown::Name(b->kind) : "no known block");
                }
            }
            // the start-up values (docs 13.8): what the process's start-up set in player 1's regions that this
            // player's own steps left at the boot value. A link into the image fails (a list head, a static object's
            // pointer: shared state in a player's regions, or a step it lacks); a plain value is reported, by run
            {
                heapown::ValueMap map;
                map.function = IsFunctionEntry;
                map.taken = kTakenAddresses;
                map.taken_count = sizeof(kTakenAddresses) / sizeof(kTakenAddresses[0]);
                map.regions = &spans;
                std::vector<heapown::StartupGap> gaps = heapown::StartupGaps(
                    csx::ClientContextInitial(), g_reference, spans, g_blocks, map, [](uint32_t va) { return R32(va); });
                std::vector<ctx::Region> regions = csx::ClientContextRegions();
                auto region_of = [&](uint32_t va) -> const char* {
                    for (const ctx::Region& r : regions)
                        if (va >= r.start && va < r.start + r.size) return r.what;
                    return "?";
                };
                for (size_t i = 0; i < gaps.size();) {
                    size_t j = i + 1;
                    while (j < gaps.size() && gaps[j].va == gaps[j - 1].va + 4 && gaps[j].link == gaps[i].link &&
                           region_of(gaps[j].va) == region_of(gaps[i].va))
                        j++;
                    if (gaps[i].link) {
                        unlinked += j - i;
                        LOGE("native split: player %d lacks %zu start-up link(s) at 0x%08X.. (player 1's 0x%08X, a %s; "
                             "boot 0x%08X; %s)",
                             k + 1, j - i, gaps[i].va, gaps[i].reference, heapown::Name(gaps[i].kind), gaps[i].boot,
                             region_of(gaps[i].va));
                    } else {
                        values += j - i;
                        LOGW("native split: player %d lacks %zu start-up value(s) at 0x%08X.. (player 1's 0x%08X, "
                             "boot 0x%08X; %s)",
                             k + 1, j - i, gaps[i].va, gaps[i].reference, gaps[i].boot, region_of(gaps[i].va));
                    }
                    i = j;
                }
            }
            // the shared state (docs 14.6): the protected start-up state must be as it was (a shared table rebuilt, a
            // list relinked by this player's steps fails it); any other shared change is reported
            for (const heapown::Range& c : heapown::Changed(protect, protect_before, GuestByte)) {
                corrupted++;
                LOGE("native split: player %d's initialisation changed protected start-up state at 0x%08X (%u "
                     "byte(s); %s)",
                     k + 1, c.va, c.size, ProtectedWhat(c.va));
            }
            {
                std::vector<heapown::Range> changes = heapown::Changed(shared, shared_before, GuestByte);
                size_t bytes = 0;
                for (const heapown::Range& c : changes) bytes += c.size;
                if (!changes.empty())
                    LOGW("native split: player %d's initialisation changed %zu shared byte(s) in %zu run(s) outside "
                         "the protected state (reported, not failed)",
                         k + 1, bytes, changes.size());
                for (size_t i = 0; i < changes.size() && i < 16; i++)
                    LOGW("native split:   shared change at 0x%08X (%u byte(s))", changes[i].va, changes[i].size);
            }
            break;
        }
    }
    if (inherited || missing || foreign || unlinked || corrupted) {
        csx::ClientContextSwitch(0);
        csx::ClientContextRemove(context);
        g_players.InitFailed(k);
        g_add_failed = true;
        auto [n, bytes] = g_blocks.Orphan(k);
        LOGE("native split: player %d's initialisation failed its checks (%zu inherited, %zu missing, %zu not its own, "
             "%zu start-up links lacking, %zu protected changes): not added; %zu of its objects (%zu bytes) left behind",
             k + 1, inherited, missing, foreign, unlinked, corrupted, n, bytes);
        return;
    }
    csx::ClientContextSwitch(0);
    g_players.Initialised(k);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOGI("native split: player %d (\"%s\") built in context %d from the boot image: of the %zu objects player 1's "
         "initialisation made, %zu are its own, %zu missing, %zu not its own; start-up links lacking %zu, values "
         "lacking %zu (warned); protected changes %zu; initialised in %.2f ms; it joins on its turn",
         k + 1, name.c_str(), context, g_reference_objects.size(), own, missing, foreign, unlinked, values, corrupted,
         ms);
}

// the server client whose loopback address names player k, 0 none
uint32_t ServerClientOf(int k) {
    uint32_t base = R32(kClients);
    int max = int(R32(kMaxClients));
    for (int i = 0; base && i < max; i++) {
        uint32_t cl = base + uint32_t(i) * kClientSize;
        bool used = R32(cl) || R32(cl + 0x4) || R32(cl + 0xC);
        if (used && R32(cl + 0x24) == kLoopback && R16(cl + 0x36) == loopnet::PortOf(k)) return cl;
    }
    return 0;
}

int SlotOf(uint32_t cl) {
    uint32_t base = R32(kClients);
    return cl && base ? int((cl - base) / kClientSize) : -1;
}

// a dword of player k's client state: live for the active player, else from its saved copy
uint32_t PlayerDword(int k, uint32_t va) {
    const nsplit::Player& p = g_players.At(k);
    if (p.context == csx::ClientContextActive()) return R32(va);
    long off = csx::ClientContextOffsetOf(va);
    std::vector<uint8_t>& copy = csx::ClientContextSaved(p.context);
    uint32_t v = 0;
    if (off >= 0 && size_t(off) + 4 <= copy.size()) std::memcpy(&v, copy.data() + off, 4);
    return v;
}

void Report() {
    for (int k = 0; k < nsplit::kMaxPlayers; k++) {
        const nsplit::Player& p = g_players.At(k);
        if (p.state == nsplit::State::Empty) continue;
        uint32_t cl = ServerClientOf(k);
        LOGI("native split: player %d %s, context %d, client state %u, its cl.playernum %u; server slot %d (userid %u); "
             "packets: sent %llu, read by the server %llu, to it %llu, read %llu",
             k + 1, nsplit::Name(p.state), p.context, PlayerDword(k, kClsState), PlayerDword(k, kClPlayernum),
             SlotOf(cl), cl ? R32(cl + 0x4BA4) : 0, (unsigned long long)g_sent[k], (unsigned long long)g_server_read[k],
             (unsigned long long)g_to[k], (unsigned long long)g_read[k]);
    }
    uint64_t switches = 0;
    double switch_ms = csx::ClientContextSwitchMs(switches);
    LOGI("native split: context switches %llu, %.4f ms each on average", (unsigned long long)switches,
         switches ? switch_ms / double(switches) : 0.0);
    LOGI("native split: %llu packets dropped by full channels, %llu read outside their player's turn, %llu foreign frees "
         "(%llu skipped), %zu heap blocks tracked",
         (unsigned long long)g_ch.Dropped(), (unsigned long long)g_read_outside_turn,
         (unsigned long long)g_foreign_frees, (unsigned long long)g_skipped_frees, g_blocks.Count());
    for (sharedonce::Op op : {sharedonce::Op::WorldRenderer, sharedonce::Op::SignonUi, sharedonce::Op::Materials})
        LOGI("native split: %s: built %llu, a player's part only %llu, failed %llu, as the title %llu",
             sharedonce::Name(op), (unsigned long long)g_world.Runs(op), (unsigned long long)g_world.Skipped(op),
             (unsigned long long)g_world.Failed(op), (unsigned long long)g_world.TitleRuns(op));
}

void RemovePlayer(X86Context& ctx, int k) {
    const nsplit::Player p = g_players.At(k);
    if (!g_players.Leave(k)) return;
    if (uint32_t cl = ServerClientOf(k)) {
        GuestStrings gs(ctx);
        uint32_t fmt = gs.Put("%s"), reason = gs.Put("left the split-screen game");
        kernel::CallGuest(ctx, kDropClient, {cl, 0, fmt, reason});
    }
    g_ch.Forget(k);
    // its objects, released by the title's own code in its own context, each checked to be its own first
    csx::ClientContextSwitch(p.context);
    g_init_owner = k;
    size_t released = 0;
    auto own = [&](uint32_t obj) {
        const heapown::Block* b = obj ? g_blocks.At(obj) : nullptr;
        return b && b->kind == heapown::Kind::Owned && b->owner == k;
    };
    if (uint32_t ents = R32(kClientEntities); own(ents)) {  // its client entities (0x2E2E0)
        kernel::CallGuest(ctx, kFree, {ents});
        W32(kClientEntities, 0);
        released++;
    }
    for (uint32_t va : {0x552C70u, 0x552C9Cu}) {  // deleting destructors through the vtable (as 0x1A7F7E, 0x1B667A)
        uint32_t obj = R32(va);
        if (!own(obj)) continue;
        uint32_t ecx = ctx.ecx;
        ctx.ecx = obj;
        kernel::CallGuest(ctx, R32(R32(obj)), {1});
        ctx.ecx = ecx;
        W32(va, 0);
        released++;
    }
    if (uint32_t obj = R32(0x552CA8); own(obj)) {  // its destructor 0x1BAD90 (edi the object) and free, as 0x1BAE40
        uint32_t edi = ctx.edi;
        ctx.edi = obj;
        kernel::CallGuest(ctx, 0x1BAD90, {});
        ctx.edi = edi;
        kernel::CallGuest(ctx, kFree, {obj});
        W32(0x552CA8, 0);
        released++;
    }
    for (uint32_t node = R32(0x5623D0); own(node);) {  // the HUD element list's nodes (as gHUD.Init 0x1A6A10)
        uint32_t next = R32(node + 4);
        kernel::CallGuest(ctx, kFree, {node});
        released++;
        node = next;
    }
    W32(0x5623D0, 0);
    {  // its delta descriptions' slot (0x45670 frees a slot's description, esi the slot)
        const guest_tables::Table& t = guest_tables::kTables[guest_tables::k_native_split_delta_slots];
        uint32_t slot = t.va + uint32_t(k) * t.elem;
        if (own(R32(slot))) {
            uint32_t esi = ctx.esi;
            ctx.esi = slot;
            kernel::CallGuest(ctx, 0x45670, {});
            ctx.esi = esi;
            released++;
        }
    }
    g_init_owner = -1;
    csx::ClientContextSwitch(0);
    // what it still owns is a leak: an error, with where it was allocated
    std::map<uint32_t, size_t> sites;
    g_blocks.ForEach([&](uint32_t, const heapown::Block& b) {
        if (b.kind == heapown::Kind::Owned && b.owner == k) sites[b.site]++;
    });
    auto [leaked, leaked_bytes] = g_blocks.Orphan(k);
    if (leaked) {
        std::string where;
        for (auto& [site, n] : sites) {
            char item[32];
            std::snprintf(item, sizeof item, " 0x%X x%zu", site, n);
            where += item;
        }
        LOGE("native split: player %d left %zu objects (%zu bytes) unreleased, allocated at:%s", k + 1, leaked,
             leaked_bytes, where.substr(0, 400).c_str());
    }
    csx::ClientContextRemove(p.context);
    g_players.ContextRemoved(p.context);
    g_players.Released(k);
    LOGI("native split: player %d left: its server slot dropped, %zu of its objects released, %zu left (%zu bytes), its "
         "context removed",
         k + 1, released, leaked, leaked_bytes);
}

}  // namespace

bool NativeSplitOn() { return Wanted() >= 2; }

// client_context.cpp, at the end of Host_Init: player 1's initialised regions, kept to check a new player's (the
// objects its initialisation made: the heap pointers of that state); what the heap holds now is the process's
void NativeSplitReference(const std::vector<uint8_t>& state) {
    g_blocks.OwnAll(heapown::kProcess);
    g_reference = state;
    g_reference_objects = heapown::HeapPointers(g_reference, Spans(), g_blocks);
    LOGI("native split: %d local players asked; %zu heap blocks from the process's initialisation; player 1's "
         "regions hold %zu heap pointers (each new player must have its own)",
         Wanted(), g_blocks.Count(), g_reference_objects.size());
}

// client_context.cpp, after the last player's client turn: players added or removed between frames
void NativeSplitFrameEnd(X86Context& ctx) {
    if (!NativeSplitOn() || !csx::ClientContextHasInitial() || g_reference.empty()) return;
    // the players connecting: joined when their client is active
    for (int k = 1; k < nsplit::kMaxPlayers; k++) {
        const nsplit::Player& p = g_players.At(k);
        if (p.state != nsplit::State::Connecting) continue;
        long off = csx::ClientContextOffsetOf(kClsState);
        std::vector<uint8_t>& copy = csx::ClientContextSaved(p.context);
        uint32_t st = 0;
        if (off >= 0) std::memcpy(&st, copy.data() + off, 4);
        if (int(st) == kClsActive) {
            g_players.Joined(k);
            g_joined_at = std::chrono::steady_clock::now();
            uint32_t cl = ServerClientOf(k);
            LOGI("native split: player %d is in the match: server slot %d, userid %u, its cl.playernum %u (player 1's "
                 "%u)", k + 1, SlotOf(cl), cl ? R32(cl + 0x4BA4) : 0, PlayerDword(k, kClPlayernum), R32(kClPlayernum));
        }
    }
    // player 1 in its match with the built-in server: the others added, one a frame
    if (!g_add_failed && R32(kSvActive) && int(R32(kClsState)) == kClsActive && g_players.Count() < Wanted() &&
        g_players.Initialising() < 0) {
        if (int k = g_players.FreeSlot(); k > 0) AddPlayer(ctx, k);
    }
    auto now_r = std::chrono::steady_clock::now();
    if (g_players.Count() > 1 && now_r - g_last_report > std::chrono::seconds(10)) {
        g_last_report = now_r;
        Report();
    }
    // diagnostics: the last player leaves, and comes back
    if (double s = LeaveAfter(); s > 0) {
        auto now = std::chrono::steady_clock::now();
        int last = -1;
        for (int k = nsplit::kMaxPlayers - 1; k > 0; k--)
            if (g_players.At(k).state == nsplit::State::Active) {
                last = k;
                break;
            }
        if (last > 0 && g_joined_at != std::chrono::steady_clock::time_point{} &&
            now - g_joined_at > std::chrono::duration<double>(s)) {
            RemovePlayer(ctx, last);
            g_joined_at = {};
        }
    }
}

// ---- the heap's owners ----------------------------------------------------------------------------------------------

// [[hook]] at 0x22D83D, _heap_alloc's ret (0x22D817; malloc and new come through it): the block in eax, its size at
// [esp+4]
void NativeSplitHeapAlloc(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    // the allocating code: malloc's caller when the frame above is malloc's (0x22D83E returns to 0x22D879; malloc's own
    // return address then at [esp+0x14]), else whatever called 0x22D83E
    uint32_t up = R32(ctx.esp + 8);
    uint32_t site = up == 0x22D879 ? R32(ctx.esp + 0x14) : up;
    g_blocks.Allocated(ctx.eax, R32(ctx.esp + 4), OwnerNow(), site, KindOf(site));
}

// [[hook]] at the ret of the engine's general allocators 0xC8E90 (zeroed, 0xC8ED1), 0xC9170 (0xC9195), 0xC91A0
// (0xC91CC): the block in eax (mov eax, edx), [esp] their caller. (0xC9130 is a pool's: it returns the block + 0x10
// and its entries are classed by kSiteRules.) malloc's caller is the wrapper for every block
// they make (the file loader's, the cvars', the client's, the server's), so the block's site and kind are its
// caller's instead: an allocation's kind is what asked for it, not the helper in between.
void NativeSplitAllocWrapperRet(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || !ctx.eax) return;
    uint32_t site = R32(ctx.esp);
    g_blocks.Reattribute(ctx.eax, site, KindOf(site));
}

// [[hook]] at 0x235F2D in calloc (0x235EFA), after its HeapAlloc: the block in eax, its size in esi
void NativeSplitHeapCalloc(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    uint32_t site = R32(ctx.esp + 4);  // calloc's caller (only esi pushed above it)
    g_blocks.Allocated(ctx.eax, ctx.esi, OwnerNow(), site, KindOf(site));
}

// [[hook]] at 0x22C609, free's entry: the block at [esp+4]; on_true "ret": a free by player 2..4 of a block that is not
// its own is not done (heap_owner.h SkipFree), reported as an error every time
bool NativeSplitHeapFree(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    uint32_t va = R32(ctx.esp + 4);
    if (!va) return false;
    int freeing = OwnerNow();
    const heapown::Block* b = g_blocks.At(va);
    int owner = b ? b->owner : heapown::Blocks::kNone;
    if (b && heapown::FreeIsForeign(*b, freeing)) {
        g_foreign_frees++;
        bool skip = heapown::SkipFree(owner, freeing);
        g_skipped_frees += skip;
        LOGE("native split: %s %s block 0x%08X of %s (return 0x%08X)",
             freeing >= 0 ? ("player " + std::to_string(freeing + 1)).c_str() : "the process",
             skip ? "would free (skipped: a leak, not a block freed under its owner)" : "frees", va,
             owner >= 0 ? ("player " + std::to_string(owner + 1)).c_str() : "the process's initialisation",
             R32(ctx.esp));
        if (skip) return true;
    }
    g_blocks.Freed(va);
    return false;
}

// ---- before a destructor: another player's object is never destroyed --------------------------------------------

// whether the acting player (in its initialisation or turn) may destroy the object at va; logged (an error: the
// player's state names another's object) when not
bool MayDestroyHere(uint32_t va, const char* what) {
    int acting = OwnerNow();
    if (acting <= 0 || !va) return true;
    const heapown::Block* b = g_blocks.At(va);
    int owner = b ? b->owner : heapown::Blocks::kNone;
    if (b ? heapown::MayDestroy(*b, acting) : heapown::MayDestroy(owner, acting)) return true;
    LOGE("native split: player %d would destroy %s 0x%08X of %s: not destroyed, the reference dropped", acting + 1, what,
         va, owner >= 0 ? ("player " + std::to_string(owner + 1)).c_str()
                        : owner == heapown::kProcess ? "the process's initialisation" : "no known allocation");
    return false;
}

// [[hook]] at 0x1A7F7E in 0x1A7F70 (`delete [0x552C70]`, ecx the object): false (on_false 0x1A7F84, the pointer then
// cleared) when it is not the acting player's
bool NativeSplitDestroy552C70(X86Context& ctx, uint8_t*) { return MayDestroyHere(ctx.ecx, "the client object 0x552C70"); }

// [[hook]] at 0x1B667A in 0x1B6670 (`delete [0x552C9C]`, ecx the object): false (on_false 0x1B6680, cleared)
bool NativeSplitDestroy552C9C(X86Context& ctx, uint8_t*) { return MayDestroyHere(ctx.ecx, "the client object 0x552C9C"); }

// [[hook]] at 0x1BAE4B in 0x1BAE40 (the object at 0x552CA8: its destructor 0x1BAD90 and free, esi the object): false
// (on_false 0x1BAE67, past the push / pop around them) with the pointer cleared here
bool NativeSplitDestroy552CA8(X86Context& ctx, uint8_t*) {
    if (MayDestroyHere(ctx.esi, "the client object 0x552CA8")) return true;
    W32(0x552CA8, 0);
    return false;
}

// [[hook]] at 0x1A6A0A in gHUD.Init (0x1A65A0), before it frees the HUD element list at gHUD + 0 (edi gHUD, eax the
// first node): false (on_false 0x1A6A26, the list head cleared) when the list is not the acting player's
bool NativeSplitDestroyHudList(X86Context& ctx, uint8_t*) { return MayDestroyHere(ctx.eax, "the HUD element list"); }

// ---- the connect's shared registries, built once ------------------------------------------------------------------

// [[hook]] at 0x2FBA4 in svc_deltadescription's handler (0x2FAE0), esi the shared registry slot for the description's
// name (0x46070): when a player after the first receives a description already registered (player 1's connect built
// the registry, the server sends every client the same), its handler gets that player's own slot
// (native_split_delta_slots) instead: it reads the message exactly as before, but frees and rebuilds only that player's
// slot, never the shared registry player 1's client and the server use
void NativeSplitDeltaSlot(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    const guest_tables::Table& t = guest_tables::kTables[guest_tables::k_native_split_delta_slots];
    int k = OwnerNow();
    uint32_t slot = nsplit::DeltaSlot(ctx.esi, ctx.esi ? R32(ctx.esi) : 0, k, t.va, t.elem);
    if (slot == ctx.esi) return;
    ctx.esi = slot;
    LOG_FIRST(4, "native split: player %d's delta description goes to its own slot 0x%08X, the shared registry kept",
              k + 1, slot);
}

// ---- the loopback ------------------------------------------------------------------------------------------------

// [[hook]] at 0x6CC70, NET_SendPacket's entry (ebx the socket, 1 server / 0 client; ecx the data; [esp+4] the length;
// the address by value from [esp+8], its port at [esp+0x1A]), on_true "ret": a loopback packet into the host's
// channels (the server's to the player its port names, a client's from the active player)
bool NativeSplitSend(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || R32(ctx.esp + 8) != kLoopback) return false;
    uint32_t len = R32(ctx.esp + 4);
    const uint8_t* data = static_cast<const uint8_t*>(kernel::HostPtr(ctx.ecx));
    if (ctx.ebx == 1) {
        int k = loopnet::PlayerOf(R16(ctx.esp + 0x1A));
        if (k < 0 || !g_ch.ToClient(k, data, len))
            LOG_FIRST(8, "native split: the server's loopback packet to port %u has no player", R16(ctx.esp + 0x1A));
        else
            g_to[k]++;
    } else {
        int k = csx::ClientContextActive();
        if (k != 0 && !csx::ClientContextInTurn())
            LOG_FIRST(8, "native split: player %d's client sent outside its turn", k + 1);
        int player = PlayerOfContext(k) >= 0 ? PlayerOfContext(k) : 0;
        g_ch.ToServer(player, data, len);
        g_sent[player]++;
    }
    return true;
}

// [[hook]] at 0x6BAF0, NET_GetLoopPacket's entry (ecx the socket, edi the message (sizebuf), ebx net_from), on_true
// "ret" with eax 1 (a packet) or 0: the host's channels (the server's inbox, or the active player's)
bool NativeSplitGet(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    std::vector<uint8_t> data;
    int from = -1;
    bool got;
    if (ctx.ecx == 1) {
        got = g_ch.ServerGet(from, data);
        if (got && from >= 0) g_server_read[from]++;
    } else {
        int k = PlayerOfContext(csx::ClientContextActive());
        got = k >= 0 && g_ch.ClientGet(k, data);
        if (got) {
            g_read[k]++;
            if (k != 0 && !csx::ClientContextInTurn()) {
                g_read_outside_turn++;
                LOG_FIRST(8, "native split: player %d's client read a packet outside its turn", k + 1);
            }
        }
        from = 0;
    }
    ctx.eax = 0;
    if (!got) return true;
    uint32_t max = R32(ctx.edi + netbuf::kMaxSize);
    if (data.size() > max) {
        LOG_FIRST(16, "native split: a loopback datagram of %zu bytes dropped (the message holds %u)", data.size(), max);
        return true;
    }
    std::memcpy(kernel::HostPtr(R32(ctx.edi + netbuf::kData)), data.data(), data.size());
    W32(ctx.edi + netbuf::kCurSize, uint32_t(data.size()));
    std::memset(kernel::HostPtr(ctx.ebx), 0, 0x14);
    W32(ctx.ebx, kLoopback);
    W16(ctx.ebx + 0x12, loopnet::PortOf(from < 0 ? 0 : from));
    ctx.eax = 1;
    return true;
}

// [[hook]] at 0x6B3CD in NET_CompareAdr (0x6B3C0: two netadr_t by value at [esp+4] and [esp+0x18]), where two
// loopback addresses compare equal (jz 0x6B418): false (on_false 0x6B41E, "different") when their ports differ.
// Without native split-screen every loopback port is 0.
bool NativeSplitCompareAdr(X86Context& ctx, uint8_t*) {
    return loopnet::AddressesMatch(ctx.eax, R16(ctx.esp + 0x16), R16(ctx.esp + 0x2A));
}

// [[hook]] at 0x9C722 in SV_ReadPackets, where a loopback packet matches the client being tried (edx net_from's type,
// esi the client, its address from +0x24, port +0x36): false (on_false 0x9C823, the next client) when the ports differ
bool NativeSplitReadPackets(X86Context& ctx, uint8_t*) {
    return loopnet::AddressesMatch(ctx.edx, R16(ctx.esi + 0x36), R16(kNetFrom + 0x12));
}

// [[hook]] at 0x5C9D0, Host_ShutdownServer's entry (it returns at once when sv.active is 0, 0x5C9DA): the end of the
// session's world (docs 15). Its own CL_Disconnect, Host_ClearMemory and the hunk's release free the world, its models
// and the game DLL's entities, so the other local players are removed first (their server slots dropped, their
// objects released, their contexts removed: none left pointing into a freed world), the world's shared-once
// operations forgotten (the next world builds them again), the host's loopback channels emptied with the title's
// queues. The removal switches contexts: only from the host's context; in another player's turn it is refused and
// reported (an error: that player's state then outlives the world)
void NativeSplitShutdownServer(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    if (R32(kSvActive)) {
        int n = 0;
        auto order = nsplit::TeardownOrder(g_players, g_session.host, n);
        int acting = PlayerOfContext(csx::ClientContextActive());
        if (n && acting == g_session.host) {
            for (int i = 0; i < n; i++) RemovePlayer(ctx, order[size_t(i)]);
            LOGI("native split: the server shuts down: %d other local player%s removed first, the world's shared state "
                 "forgotten", n, n == 1 ? "" : "s");
        } else if (n) {
            LOGE("native split: the server shuts down in player %d's turn: %d other local player%s not removed (only "
                 "the host's context removes players)", acting + 1, n, n == 1 ? "" : "s");
        }
        g_world.Invalidate();
    }
    g_ch.Clear();
}

// ---- registrations during another player's initialisation ---------------------------------------------------------

// [[hook]] at 0x1BEF0, the client's pfnRegisterVariable (cdecl name, value, flags): it registers a new cvar_t each
// time, and a name already registered is refused, leaving the client an unregistered copy whose value nothing sets.
// While another player's initialisation runs (on_true "ret"): the registered cvar_t in eax
bool NativeSplitRegisterVariable(X86Context& ctx, uint8_t*) {
    bool init = g_players.Initialising() > 0;
    if (!init) return false;
    uint32_t existing = kernel::CallGuest(ctx, kCvarFindVar, {R32(ctx.esp + 4)});
    if (nsplit::RegisterPolicy(true, existing != 0) != nsplit::Registration::ReuseExisting) return false;
    ctx.eax = existing;
    return true;
}

// [[hook]] at 0x3C760, Cmd_AddCommand's entry: Sys_Error after host_initialized. While another player's
// initialisation runs (on_true "ret"): not called again (the first player's command stands)
bool NativeSplitAddCommand(X86Context& ctx, uint8_t*) {
    if (nsplit::EngineCommandPolicy(g_players.Initialising() > 0) != nsplit::Registration::Skip) return false;
    LOG_FIRST(8, "native split: Cmd_AddCommand(\"%s\") skipped in another player's initialisation",
              reinterpret_cast<const char*>(kernel::HostPtr(R32(ctx.esp + 4))));
    return true;
}

// ---- until M3 / M4: an added player's turn draws, hears and reads nothing ------------------------------------------

bool AddedPlayersTurn() { return NativeSplitOn() && csx::ClientContextActive() != 0; }

// [[hook]] at 0x5D51B in _Host_Frame, before SCR_UpdateScreen's call: false (on_false 0x5D535) skips it
bool NativeSplitScreen(X86Context&, uint8_t*) { return !AddedPlayersTurn(); }

// [[hook]] at 0x19E160, the look routine's entry (the right stick and the mouse's look), and 0x19DFD0, the pad's
// buttons as keys: on_true "ret" skips them
bool NativeSplitLook(X86Context&, uint8_t*) { return AddedPlayersTurn() && csx::ClientContextInTurn(); }
bool NativeSplitPadKeys(X86Context&, uint8_t*) { return AddedPlayersTurn() && csx::ClientContextInTurn(); }

// [[hook]] at 0x100F10, S_StartDynamicSound's entry: on_true "ret" skips it
bool NativeSplitSound(X86Context&, uint8_t*) { return AddedPlayersTurn() && csx::ClientContextInTurn(); }

// ---- the world's shared-once operations (docs 15) ----------------------------------------------------------------
// A client's connect builds world-wide state the title made once per map: here it is built once per map generation
// (sharedonce::World), by whichever local player gets there first and as the process (its allocations and frees are
// the process's, never tied to that player's lifetime); every other player of the generation does only its own part.

namespace {
constexpr uint32_t kClServercount = 0x74A058;  // cl.servercount (svc_serverinfo, 0x2E854)
constexpr uint32_t kClWorldmodel = 0x778A68;   // cl.worldmodel (the signon, 0x2D3AE)
constexpr uint32_t kNewMap = 0x4F8D0;          // R_NewMap
constexpr uint32_t kViewReset = 0xB6770;       // R_NewMap's call of a player's own view reset: shake, cl.sf's fade
constexpr uint32_t kWaterAmp = 0x3774D0;       // gl_wateramp's value (the cvar 0x3774C4 + 0xC), R_NewMap 0x4FA4D
constexpr uint32_t kUiUnload = 0xC4770;        // the UI bundle unload (edx the bundle's name)
constexpr uint32_t kPieMenuUi = 0x32C880;      // "PieMenuUI", the signon's edx (0x2D3EA)
constexpr uint32_t kMaterialsLoad = 0x1DC0C0;  // the client game's material list load (no arguments)

int ActingPlayer() {
    int k = PlayerOfContext(csx::ClientContextActive());
    return k >= 0 ? k : 0;
}

sharedonce::Generation WorldGeneration() { return {R32(kClServercount), R32(kClWorldmodel), ""}; }

// a Run of a shared-once operation: finished with its result when it returns; cut short (unwound), a failure (the
// next caller of the generation runs it again)
struct SharedRun {
    sharedonce::Op op;
    bool ok = false;
    explicit SharedRun(sharedonce::Op o) : op(o) {}
    ~SharedRun() { g_world.Finished(op, ok); }
};

std::string GuestString(uint32_t va, size_t max) {
    const char* s = static_cast<const char*>(kernel::HostPtr(va));
    size_t n = 0;
    while (n < max && s[n]) n++;
    return std::string(s, n);
}
}  // namespace

// [[hook]] at 0x4F8D0, R_NewMap's entry (cdecl, no arguments; the signon 0x2D3F4 its only caller), on_true "ret".
// Its shared part (the light styles, r_worldentity 0x6E8020, the world's leaves and texture chains, the particles
// 0x830F0, the decals 0x53020, the lightmaps 0x529C0, the skies 0x57E50, the stale textures 0x486F0) is the world's:
// built once for the generation (the hook calls R_NewMap itself, as the process); a second client of the same world
// would clear player 1's decal pool under the surfaces' decal chains, add a second poly per surface and leak the
// lightmaps' buffers. Its per-player part (the view reset 0xB6770 and cl_entities[0]+0x222) runs for every client.
bool NativeSplitNewMap(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    sharedonce::Generation g = WorldGeneration();
    switch (g_world.Begin(sharedonce::Op::WorldRenderer, g)) {
    case sharedonce::Action::Inner:
    case sharedonce::Action::Title:
        return false;
    case sharedonce::Action::PerPlayerOnly: {
        kernel::CallGuest(ctx, kViewReset, {});
        if (uint32_t ents = R32(kClientEntities)) W32(ents + 0x222, R32(kWaterAmp));
        LOG_FIRST(4, "native split: player %d's R_NewMap: the world (spawn count %u) is built; its own part only",
                  ActingPlayer() + 1, g.spawncount);
        return true;
    }
    case sharedonce::Action::Run: {
        SharedRun run(sharedonce::Op::WorldRenderer);
        kernel::CallGuest(ctx, kNewMap, {});
        run.ok = true;
        LOGI("native split: the world (spawn count %u, model 0x%08X) built by R_NewMap in player %d's signon, as the "
             "process", g.spawncount, g.world, ActingPlayer() + 1);
        return true;
    }
    }
    return false;
}

// [[hook]] at 0x2D3EA in the signon (0x2D350), before `mov edx, "PieMenuUI"; call 0xC4770` (an unload of that UI
// bundle: its D3D textures freed, its buffer back to the pool 0x553ED0): on_true 0x2D3F4. Once for the generation,
// as the process; a second client's would free textures the one UI may be using
bool NativeSplitSignonUi(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    switch (g_world.Begin(sharedonce::Op::SignonUi, WorldGeneration())) {
    case sharedonce::Action::Inner:
    case sharedonce::Action::Title:
        return false;
    case sharedonce::Action::PerPlayerOnly:
        return true;
    case sharedonce::Action::Run: {
        SharedRun run(sharedonce::Op::SignonUi);
        uint32_t edx = ctx.edx;
        ctx.edx = kPieMenuUi;
        kernel::CallGuest(ctx, kUiUnload, {});
        ctx.edx = edx;
        run.ok = true;
        return true;
    }
    }
    return false;
}

// [[hook]] at 0x1AE5FA in the HUD's map poll (0x1AE430, esi gHUD), the call of the material list load 0x1DC0C0 when
// the level name differs from the player's gHUD+0x24 (edi, just copied): on_true 0x1AE5FF. The list (0x57F090, read
// through 0x380DA4) is one for the process, its content from the map's files only: loaded once per map name, as the
// process (its nodes the process's: no player's turn frees another's, none orphaned when a player leaves); the
// player's own copy of the name and the weather reset 0x1CCD50 after it still run. The console's mat_reload
// (0x1DBDB0) is not keyed: it reloads as the title does
bool NativeSplitMaterials(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    sharedonce::Generation g{0, 0, GuestString(ctx.edi, 64)};
    switch (g_world.Begin(sharedonce::Op::Materials, g)) {
    case sharedonce::Action::Inner:
    case sharedonce::Action::Title:
        return false;
    case sharedonce::Action::PerPlayerOnly:
        LOG_FIRST(4, "native split: player %d's map poll: the material list of %s is loaded; not again",
                  ActingPlayer() + 1, g.name.c_str());
        return true;
    case sharedonce::Action::Run: {
        SharedRun run(sharedonce::Op::Materials);
        kernel::CallGuest(ctx, kMaterialsLoad, {});
        run.ok = true;
        LOGI("native split: the material list of %s loaded in player %d's turn, as the process", g.name.c_str(),
             ActingPlayer() + 1);
        return true;
    }
    }
    return false;
}

// ---- a connection's and the session's lifecycle (docs 15) -------------------------------------------------------

bool NativeSplitAddedPlayerActive() { return NativeSplitOn() && ActingPlayer() != g_session.host; }

// [[hook]] at 0x2EDB3 in CL_ParseServerInfo (0x2E780): a secure server's hash differs from the client's own (always:
// 0x43EB9C is never set) and the client would start the secure-module validation (its flag 0x5518A8 set, the
// "!ModuleC.dll" request, B24E0's reset of the one shared machine 0x4E50B0 at 0x2EE42): on_true 0x2EE5A (the flag
// cleared, "sendres", as an insecure server's client) for a player other than the session's host. The hash and the key
// before it are read as before (the message stays in step)
bool NativeSplitModuleValidation(X86Context&, uint8_t*) {
    if (!NativeSplitOn() || nsplit::StartsModuleValidation(g_session, ActingPlayer())) return false;
    LOG_FIRST(4, "native split: player %d starts no secure-module validation (the process's one machine stays the "
                 "host's)", ActingPlayer() + 1);
    return true;
}

// [[hook]] at 0x29029 in CL_Disconnect (0x28F20), `jz` past Host_ShutdownServer (0x2902C) when sv.active is 0: on_true
// 0x29034 (the server kept) when the disconnecting client is not the session's host: another local player's
// disconnect (a kick, svc_disconnect, a stuffed "disconnect") ends its own connection, never the server and world the
// others are in
bool NativeSplitDisconnectServer(X86Context&, uint8_t*) {
    if (!NativeSplitOn() || !R32(kSvActive) || g_session.DisconnectEndsServer(ActingPlayer())) return false;
    LOGI("native split: player %d's client disconnects: the built-in server keeps running", ActingPlayer() + 1);
    return true;
}

// [[hook]] at 0x29072 in CL_Disconnect: the free of the renderer's transparent-entity array 0x551EF4 and its size
// 0x551EF0 (one renderer: shared scratch, reallocated by every connect, 0x93180): on_true 0x29090 (kept) for another
// local player than the host, whose disconnect would leave the host's renderer without it until its next connect
bool NativeSplitDisconnectRenderer(X86Context&, uint8_t*) { return NativeSplitAddedPlayerActive(); }
