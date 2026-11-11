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
#include <set>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "common/guest_tables_layout.h"
#include "game/client_context.h"
#include "game/heap_owner.h"
#include "game/intern_pool.h"
#include "game/local_loopback.h"
#include "game/native_split_core.h"
#include "game/native_split_lifecycle.h"
#include "game/net_buffers.h"
#include "game/net_engine.h"
#include "game/shared_once.h"
#include "game/table_fingerprint.h"
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
constexpr uint32_t kTimer = 0xFC870;              // a timer (cdecl, ebx an id): its once-per-process guarded block
constexpr uint32_t kTimerGuard = 0x5C9660;        // its function-local static's guard bit (FC87B, FC88D)
constexpr uint32_t kTimerPeriod = 0x5C9640;       // under the guard: the counter frequency's reciprocal (FC8C6)
constexpr uint32_t kTempEntInit = 0x3A8A0;        // CL_TempEntInit: the tempent pool cleared and its free list linked
constexpr uint32_t kEngineFunctions = 0x371610;   // the engine's cl_enginefunc_t table, as Host_Init hands it (0x1C69F)
constexpr uint32_t kActivationCheck = 0x269A0;    // cls.state 3 / 4 with the signon complete -> 5 (docs 15.10)
constexpr uint32_t kUpdateClientData = 0x1CC10;   // CL_UpdateClientData (HUD_UpdateClientData; state 5 only)
constexpr uint32_t kClsSignon = 0x784800;         // cls.signon
constexpr uint32_t kScoreboardRows = 0x1B9820;    // the scoreboard's rows into the shared "Scoreboard" screen (stdcall)
// the console's command buffer (docs 15.12): cmd_text, a sizebuf_t (+4 flags, +8 data, +0xC maxsize, +0x10 cursize)
constexpr uint32_t kCmdFlags = 0x748664, kCmdData = 0x748668, kCmdMax = 0x74866C, kCmdSize = 0x748670;
constexpr uint32_t kCbufExecute = 0x3BB50;        // Cbuf_Execute
constexpr uint32_t kShutdownServer = 0x5C9D0;     // Host_ShutdownServer (cdecl crash)
constexpr uint32_t kXbuiCommand = 0xD7450;        // the "xbui" console command (Cmd_AddCommand at 0xD772E)
constexpr uint32_t kVoiceIdle = 0x101FD0;         // Voice_Idle (cdecl frametime; 0x5D46C in a player's client half)
constexpr uint32_t kVoiceAssign = 0x102620;       // a voice channel for the speaker in edi (-1 none)
constexpr uint32_t kVoiceData = 0x102760;         // a voice channel's data added (cdecl channel, data, length)
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
// every shared change a new player's initialisation makes listed (CSX_NATIVE_SPLIT_CHANGES=1; else the first 16), for
// tools/shared_changes.py's classification (docs 15.12)
bool ChangesAll() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_CHANGES");
        return v && *v == '1';
    }();
    return on;
}
// diagnostics: which player leaves after CSX_NATIVE_SPLIT_LEAVE_AFTER seconds: 0 (unset) the last added one, removed
// by the process; N (1..4) player N, through its own "disconnect" console line run in its own turn (the real path:
// its CL_Disconnect, then its removal between frames; player 1 departs, the others play on)
int LeaveWho() {
    static const int n = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_LEAVE_PLAYER");
        int x = v ? std::atoi(v) : 0;
        return x >= 1 && x <= nsplit::kMaxPlayers ? x : 0;
    }();
    return n;
}
// diagnostics: remove the last added player after this many seconds in the match, add one again after as many more
double LeaveAfter() {
    static const double s = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_LEAVE_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    return s;
}

// a refused player's blocks released (docs 15.8): off unless CSX_NATIVE_SPLIT_ROLLBACK=1. The plan is always made and
// reported; releasing frees only what nothing the scan sees names, and the scan cannot see a pointer the host or the
// kernel keeps, so it is not done by default
bool RollbackRelease() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_ROLLBACK");
        return v && *v == '1';
    }();
    return on;
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
// their fingerprints as compiled (table_fingerprint.h; tools/check_tables.py)
[[gnu::used]] const tablefp::Stamp kProtectedStamp = {"CSXFP1:protect", tablefp::Ranges(kProtected),
                                                      uint32_t(sizeof(kProtected) / sizeof(kProtected[0])),
                                                      tablefp::Bytes(kProtected)};
[[gnu::used]] const tablefp::Stamp kAddressesStamp = {"CSXFP1:address", tablefp::Words(kTakenAddresses),
                                                      uint32_t(sizeof(kTakenAddresses) / sizeof(kTakenAddresses[0])), 0};
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
uint64_t g_moves = 0, g_foreign_moves = 0;  // realloc's moves tracked; of another owner's block (an error, docs 15.8)
bool g_add_failed = false;  // a player's initialisation failed its checks: no further players added this session
// the shared front end (docs 15.10): its driver, the depth of its scope (its maintenance running as the process), and
// what was done: rebuilds run by the driver, withheld in another player's turn, UI commands withheld, frees of a
// player's block refused in the scope, the front end's element code reached in another player's turn
nsplit::SharedUiOwner g_ui;
int g_ui_scope = 0;
enum UiGate { kUiRows, kUiText, kUiMenu, kUiClose, kUiCommand, kUiXbui, kUiGates };
constexpr const char* kUiGateName[kUiGates] = {"scoreboard rows (0x1B9820)", "element texts (0xD96A0)",
                                                 "menu opens (0xCAB40)", "screen closes (0xCA0B0)",
                                                 "front-end commands (0xCA6F0)", "xbui commands (0xD7450)"};
uint64_t g_ui_shared[kUiGates] = {}, g_ui_withheld[kUiGates] = {};
uint64_t g_ui_scope_kept = 0, g_ui_elements_in_turn = 0;
uint64_t g_preludes[nsplit::kMaxPlayers] = {};  // per player: its activation check run in its own turn
// the console's lines (docs 15.12): each player's own queue; the player whose queue runs (its lines in the title's
// buffer meanwhile, the shared buffer's kept aside: g_swap), -1 none; lines routed by origin; queue runs per player
nsplit::CommandQueues g_cmds;
int g_cmd_running = -1;
struct CbufSwap;
CbufSwap* g_swap = nullptr;
uint64_t g_cmd_routed[nsplit::kOrigins] = {}, g_cmd_runs[nsplit::kMaxPlayers] = {}, g_cmd_dropped = 0;
// the players' clients watched between frames (a kick, a disconnect, a failed connect), the frames cut short in a
// player's turn, the shutdowns refused or deferred in a turn, player 1's departures, the session's end deferred to
// the frame's end; what leaving players left behind
nsplit::ClientWatch g_watch[nsplit::kMaxPlayers];
uint64_t g_aborts = 0, g_refused_shutdowns = 0, g_departures = 0;
bool g_end_session = false;
nsplit::LeakBudget g_leaks;
// a leaving player's cleanup: its blocks another names (shared data, another context, another owner's block), never
// freed by its destructors (the free skipped and logged); how many such frees were refused
std::set<uint32_t> g_release_keep;
bool g_releasing = false;
uint64_t g_release_kept = 0;
// the voice channels' scope (as the process; docs 15.12): its depth; run in it, not run in another context's turn
int g_voice_scope = 0;
uint64_t g_voice_shared = 0, g_voice_withheld = 0;
uint64_t g_pad_stores_kept = 0;  // IN_Init's stores to the shared pad bridge skipped in another player's init
// a scope's depth kept right when a longjmp (Host_EndGame / Host_Error) unwinds the host frames through it
struct Depth {
    int& d;
    explicit Depth(int& x) : d(x) { d++; }
    ~Depth() { d--; }
};
nsplit::Once g_timer_once = nsplit::Once::Pending;  // the timer's one-time block (docs 15.7): players only once done
bool g_timer_block_only = false;                    // the process's call of the timer: its guarded block, then return
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
    // the shared front end's maintenance and the world's shared-once operations: the process's (docs 15.10, 15)
    bool in_turn = csx::ClientContextInTurn();
    int r = nsplit::AllocationOwner(g_ui_scope > 0 || g_voice_scope > 0, g_world.Running(), g_init_owner, in_turn,
                                    in_turn ? PlayerOfContext(csx::ClientContextActive()) : -1);
    return r >= 0 ? r : heapown::kProcess;
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

// a heap block as the pool check sees it (intern_pool.h)
internpool::BlockInfo PoolBlock(uint32_t va) {
    const heapown::Block* b = g_blocks.At(va);
    return b ? internpool::BlockInfo{true, b->owner, b->size} : internpool::BlockInfo{};
}

// a refused player's rollback (heap_owner.h PlanRollback, docs 15.8), after switching back to player 1: the plan
// reported; its releasable blocks freed only with CSX_NATIVE_SPLIT_ROLLBACK=1 (as the failed player: its own frees).
// The number freed
size_t Rollback(X86Context& ctx, int k) {
    constexpr uint32_t kDataLo = 0x36C8A0, kDataHi = 0x7D6F20;
    heapown::RollbackPlan plan = heapown::PlanRollback(
        g_blocks, k,
        [](auto f) {
            for (uint32_t va = kDataLo; va + 4 <= kDataHi; va += 4) f(va, R32(va));
        },
        [](uint32_t va) { return R32(va); });
    std::map<std::string, std::pair<size_t, size_t>> why;  // kept: reason -> (blocks, bytes)
    for (const heapown::RollbackItem& r : plan.keep) {
        auto& w = why[r.reason];
        w.first++;
        w.second += r.size;
    }
    LOGI("native split: player %d's rollback: %zu of its blocks releasable (%zu bytes), %zu kept%s", k + 1,
         plan.release.size(), plan.ReleaseBytes(), plan.keep.size(),
         RollbackRelease() ? "" : " (releasing is off: CSX_NATIVE_SPLIT_ROLLBACK=1)");
    for (const auto& [reason, nb] : why)
        LOGI("native split:   kept: %zu block(s), %zu bytes: %s", nb.first, nb.second, reason.c_str());
    for (size_t i = 0; i < plan.keep.size() && i < 24; i++)
        LOGI("native split:   kept 0x%08X (%u bytes, site 0x%08X, %s): %s 0x%08X", plan.keep[i].va, plan.keep[i].size,
             plan.keep[i].site, heapown::Name(plan.keep[i].kind), plan.keep[i].reason, plan.keep[i].named_at);
    for (size_t i = 0; i < plan.release.size() && i < 24; i++)
        LOGI("native split:   releasable 0x%08X (%u bytes, site 0x%08X)", plan.release[i].va, plan.release[i].size,
             plan.release[i].site);
    if (!RollbackRelease()) return 0;
    size_t freed = 0;
    int owner = g_init_owner;
    g_init_owner = k;  // its own frees (NativeSplitHeapFree: never another's)
    for (const heapown::RollbackItem& r : plan.release) {
        const heapown::Block* b = g_blocks.At(r.va);
        if (!b || b->owner != k) continue;  // gone since the plan: never freed twice
        kernel::CallGuest(ctx, kFree, {r.va});
        freed += g_blocks.At(r.va) == nullptr;
    }
    g_init_owner = owner;
    return freed;
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
    // a step cut short (a Sys_Error or Host_Error's longjmp unwinding through the guest call): player 1's context made
    // live again, the player's initialisation failed (fail closed: no further player this session); what it made is
    // left behind (reported by the next report as its blocks, never freed: the step's state is not known)
    struct Unwound {
        int k, context;
        ~Unwound() {
            if (!std::uncaught_exceptions() || g_players.At(k).state != nsplit::State::Initialising) return;
            g_init_owner = -1;
            csx::ClientContextSwitch(0);
            csx::ClientContextRemove(context);
            g_players.InitFailed(k);
            g_add_failed = true;
            auto [n, bytes] = g_blocks.Orphan(k);
            LOGE("native split: player %d's initialisation was cut short (a longjmp through its steps): not added, its "
                 "context removed; %zu of its objects (%zu bytes) left behind", k + 1, n, bytes);
        }
    } unwound{k, context};
    std::vector<heapown::Span> spans = Spans();
    size_t inherited = 0, own = 0, missing = 0, foreign = 0, unlinked = 0, values = 0, corrupted = 0;
    std::vector<heapown::Range> protect = ProtectedRanges(), shared = SharedDataRanges(spans);
    std::vector<uint8_t> protect_before, shared_before;
    internpool::State pool_before;
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
            pool_before = internpool::Capture(GuestByte);
            g_init_owner = k;
            break;
        case nsplit::InitStep::ClientState:
            ClientInitState();
            // the tempent pool's free list (0x3AEF00, 250 TEMPENTITY, a player's region): player 1's start-up linked it
            // through CL_ClearState (0x28EE1), which the new player first runs at its own svc_serverinfo; the start-up
            // check needs it now (the sixth run, docs 15.6). A leaf writing only the pool (tests/region_model_test.py)
            kernel::CallGuest(ctx, kTempEntInit, {});
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
            // the string-intern pool (docs 15.8): its growth fields are not compared byte for byte (region_evidence.py
            // REVIEWED_POOL); its invariants are, and a broken one fails the player as a protected change does
            {
                internpool::State pool_after = internpool::Capture(GuestByte);
                std::vector<std::string> bad = internpool::Check(pool_before, pool_after, PoolBlock);
                for (const std::string& b : bad) {
                    corrupted++;
                    LOGE("native split: player %d's initialisation broke the string-intern pool 0x%08X: %s", k + 1,
                         internpool::kPool, b.c_str());
                }
                if (bad.empty())
                    LOGI("native split: player %d's initialisation: the string-intern pool 0x%08X holds %u strings "
                         "(%u added, %u bytes; its buffer %s, the process's), every earlier index unchanged",
                         k + 1, internpool::kPool, pool_after.h.count, pool_after.h.count - pool_before.h.count,
                         pool_after.h.strings_size - pool_before.h.strings_size,
                         pool_after.h.strings == pool_before.h.strings ? "in place" : "moved");
            }
            {
                std::vector<heapown::Range> changes = heapown::Changed(shared, shared_before, GuestByte);
                size_t bytes = 0;
                for (const heapown::Range& c : changes) bytes += c.size;
                if (!changes.empty())
                    LOGW("native split: player %d's initialisation changed %zu shared byte(s) in %zu run(s) outside "
                         "the protected state (reported, not failed)",
                         k + 1, bytes, changes.size());
                for (size_t i = 0; i < changes.size() && (i < 16 || ChangesAll()); i++)
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
        size_t released = Rollback(ctx, k);
        auto [n, bytes] = g_blocks.Orphan(k);
        LOGE("native split: player %d's initialisation failed its checks (%zu inherited, %zu missing, %zu not its own, "
             "%zu start-up links lacking, %zu protected changes): not added; %zu of its objects released, %zu (%zu "
             "bytes) left behind",
             k + 1, inherited, missing, foreign, unlinked, corrupted, released, n, bytes);
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
         "(%llu skipped), %llu reallocs moved (%llu of another owner's block), %zu heap blocks tracked",
         (unsigned long long)g_ch.Dropped(), (unsigned long long)g_read_outside_turn,
         (unsigned long long)g_foreign_frees, (unsigned long long)g_skipped_frees, (unsigned long long)g_moves,
         (unsigned long long)g_foreign_moves, g_blocks.Count());
    std::string gates;
    for (int i = 0; i < kUiGates; i++) {
        char item[128];
        std::snprintf(item, sizeof item, "%s%s %llu by the driver, %llu withheld", i ? "; " : "", kUiGateName[i],
                      (unsigned long long)g_ui_shared[i], (unsigned long long)g_ui_withheld[i]);
        gates += item;
    }
    LOGI("native split: the shared front end, driven by player %d: %s; a player's block kept in its scope %llu; its "
         "element code in another player's turn %llu",
         g_ui.driver + 1, gates.c_str(), (unsigned long long)g_ui_scope_kept, (unsigned long long)g_ui_elements_in_turn);
    for (int k = 1; k < nsplit::kMaxPlayers; k++)
        if (g_players.At(k).state != nsplit::State::Empty)
            LOGI("native split: player %d's activation check and client data update ran in its own turn %llu times",
                 k + 1, (unsigned long long)g_preludes[k]);
    {
        std::string runs;
        for (int k = 0; k < nsplit::kMaxPlayers; k++) {
            if (g_players.At(k).state == nsplit::State::Empty) continue;
            char item[96];
            std::snprintf(item, sizeof item, "%splayer %d %llu (%zu bytes waiting, %llu refused full)", runs.empty() ? "" : ", ",
                          k + 1, (unsigned long long)g_cmd_runs[k], g_cmds.Size(k), (unsigned long long)g_cmds.Overflows(k));
            runs += item;
        }
        LOGI("native split: console lines routed: %llu the process's, %llu the server's, %llu a player's own, %llu the "
             "shared front end's; players' own runs: %s; %llu bytes dropped with leaving players",
             (unsigned long long)g_cmd_routed[int(nsplit::Origin::Process)],
             (unsigned long long)g_cmd_routed[int(nsplit::Origin::Server)],
             (unsigned long long)g_cmd_routed[int(nsplit::Origin::Player)],
             (unsigned long long)g_cmd_routed[int(nsplit::Origin::FrontEnd)], runs.c_str(),
             (unsigned long long)g_cmd_dropped);
    }
    LOGI("native split: the lifecycle: %llu frames cut short in a player's turn, %llu server shutdowns refused in a "
         "player's turn, %llu departures of player 1, %d leaves (%zu bytes left behind, the bound %zu); the voice channels "
         "run as the process %llu times, withheld in another context's turn %llu; cleanup frees of a named block kept %llu; "
         "IN_Init's stores to the shared pad bridge skipped in another player's initialisation %llu",
         (unsigned long long)g_aborts, (unsigned long long)g_refused_shutdowns, (unsigned long long)g_departures,
         g_leaks.leaves, g_leaks.left_behind, g_leaks.limit, (unsigned long long)g_voice_shared,
         (unsigned long long)g_voice_withheld, (unsigned long long)g_release_kept,
         (unsigned long long)g_pad_stores_kept);
    for (sharedonce::Op op : {sharedonce::Op::WorldRenderer, sharedonce::Op::SignonUi, sharedonce::Op::Materials})
        LOGI("native split: %s: built %llu, a player's part only %llu, failed %llu, as the title %llu",
             sharedonce::Name(op), (unsigned long long)g_world.Runs(op), (unsigned long long)g_world.Skipped(op),
             (unsigned long long)g_world.Failed(op), (unsigned long long)g_world.TitleRuns(op));
}

// what of player k's own blocks someone else names (heap_owner.h PlanRollback): the shared data (the image's .data with
// player 1's live regions: called from player 1's context), every other context's saved copy, another owner's block;
// followed through its own blocks. Those its cleanup must not free (its destructors' frees of them are skipped)
std::set<uint32_t> NamedElsewhere(int k, int context, size_t& released_bytes, size_t& kept_bytes) {
    constexpr uint32_t kDataLo = 0x36C8A0, kDataHi = 0x7D6F20;
    std::vector<heapown::Span> spans = Spans();
    heapown::RollbackPlan plan = heapown::PlanRollback(
        g_blocks, k,
        [&](auto f) {
            for (uint32_t va = kDataLo; va + 4 <= kDataHi; va += 4) f(va, R32(va));
            for (int c = 1; c < csx::ClientContextCount(); c++) {
                if (c == context || c == csx::ClientContextActive()) continue;
                const std::vector<uint8_t>& copy = csx::ClientContextSaved(c);
                for (const heapown::Span& sp : spans)
                    for (uint32_t o = 0; o + 4 <= sp.size && sp.offset + o + 4 <= copy.size(); o += 4) {
                        uint32_t v;
                        std::memcpy(&v, copy.data() + sp.offset + o, 4);
                        f(sp.va + o, v);
                    }
            }
        },
        [](uint32_t va) { return R32(va); });
    std::set<uint32_t> keep;
    kept_bytes = 0;
    for (const heapown::RollbackItem& r : plan.keep) {
        keep.insert(r.va);
        kept_bytes += r.size;
    }
    released_bytes = plan.ReleaseBytes();
    return keep;
}

void RemovePlayer(X86Context& ctx, int k) {
    const nsplit::Player p = g_players.At(k);
    if (!g_players.Leave(k)) return;
    // docs 15.12 (native_split_lifecycle.h CleanupOrder): nothing reaches it any more first
    if (uint32_t cl = R32(kSvActive) ? ServerClientOf(k) : 0) {
        GuestStrings gs(ctx);
        uint32_t fmt = gs.Put("%s"), reason = gs.Put("left the split-screen game");
        kernel::CallGuest(ctx, kDropClient, {cl, 0, fmt, reason});
    }
    g_ch.Forget(k);
    if (size_t n = g_cmds.Drop(k)) {
        g_cmd_dropped += n;
        LOGI("native split: player %d's %zu bytes of console lines dropped (never run in another context)", k + 1, n);
    }
    if (int was = g_ui.driver; g_ui.Left(k, g_players) != was)
        LOGI("native split: the shared front end is driven by player %d now (player %d leaves)", g_ui.driver + 1, k + 1);
    g_session.host = nsplit::SessionHost(g_players);
    // what of its own another names, from player 1's context (before its own is made live)
    size_t plan_free = 0, plan_kept = 0;
    g_release_keep = NamedElsewhere(k, p.context, plan_free, plan_kept);
    LOGI("native split: player %d's cleanup: %zu of its blocks (%zu bytes) named by another (kept), %zu bytes named by "
         "nobody else", k + 1, g_release_keep.size(), plan_kept, plan_free);
    // its objects, released by the title's own code in its own context, each checked to be its own first
    csx::ClientContextSwitch(p.context);
    g_init_owner = k;
    g_releasing = true;
    size_t released = 0;
    auto own = [&](uint32_t obj) {
        const heapown::Block* b = obj ? g_blocks.At(obj) : nullptr;
        nsplit::Release r = nsplit::ReleaseCheck(b != nullptr, b ? b->owner : heapown::Blocks::kNone,
                                                 b && b->kind == heapown::Kind::Owned, k, g_release_keep.count(obj) > 0);
        if (r != nsplit::Release::Release && r != nsplit::Release::None)
            LOGI("native split: player %d's object 0x%08X %s", k + 1, obj, nsplit::Name(r));
        return r == nsplit::Release::Release;
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
    g_releasing = false;
    g_release_keep.clear();
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
    g_watch[k] = {};
    g_leaks.Left(leaked_bytes);
    LOGI("native split: player %d left: its server slot dropped, %zu of its objects released, %zu left (%zu bytes), its "
         "context removed; left behind by leaving players so far %zu bytes in %d leave(s)",
         k + 1, released, leaked, leaked_bytes, g_leaks.left_behind, g_leaks.leaves);
    if (!g_leaks.MayJoin() && !g_add_failed) {
        g_add_failed = true;
        LOGE("native split: leaving players have left %zu bytes behind (the bound %zu): no player joins again this "
             "session", g_leaks.left_behind, g_leaks.limit);
    }
    // player 1 departed and nobody plays any more: the session ends between frames (as the process)
    if (g_players.At(0).state == nsplit::State::Departed && g_players.Playing() == 0) g_end_session = true;
}

// player 1's client left while another plays (docs 15.12): it departs, its context kept (the switcher's first: the
// window's, the process's console runs there); the server, the world and the process's systems stay; the shared front
// end and the session's host go to the lowest player still playing; no slot or identity renumbered. Its objects are
// the process's (its initialisation's) or its turns': none released (its context stays)
void DepartPlayer1() {
    if (!g_players.Depart0()) return;
    g_departures++;
    g_watch[0] = {};
    if (size_t n = g_cmds.Drop(0)) g_cmd_dropped += n;
    g_ui.Left(0, g_players);
    g_session.host = nsplit::SessionHost(g_players);
    size_t n = 0, bytes = 0;
    g_blocks.ForEach([&](uint32_t, const heapown::Block& b) {
        if (b.owner == 0) {
            n++;
            bytes += b.size;
        }
    });
    LOGI("native split: player 1 left the match (its client disconnected): the built-in server and the world stay for %d "
         "other player(s); the shared front end is driven by player %d, the session's host is player %d; its context "
         "kept, %zu of its turns' objects (%zu bytes) kept with it",
         g_players.Playing(), g_ui.driver + 1, g_session.host + 1, n, bytes);
}

// the session ended by the last player playing, not player 1 (nsplit::Shutdown::Defer): between frames, in player 1's
// context, as the process: the players removed, then the server shut down as the title's (its CL_Disconnect in player
// 1's context, its world freed with nobody's context in it); player 1 the title's own player again
void EndSession(X86Context& ctx) {
    g_end_session = false;
    for (int k = nsplit::kMaxPlayers - 1; k > 0; k--) {
        nsplit::State s = g_players.At(k).state;
        if (s == nsplit::State::Connecting || s == nsplit::State::Active) RemovePlayer(ctx, k);
    }
    bool active = R32(kSvActive) != 0;
    if (active) kernel::CallGuest(ctx, kShutdownServer, {0});
    g_players.Return0();
    g_session.host = 0;
    g_ui.driver = 0;
    g_watch[0] = {};
    LOGI("native split: the session ended (its last player left): %s; player 1 is the title's own player again",
         active ? "the built-in server shut down as the process, from player 1's context" : "no server was running");
}

}  // namespace

bool NativeSplitOn() { return Wanted() >= 2; }

// client_context.cpp, at the end of Host_Init: player 1's initialised regions, kept to check a new player's (the
// objects its initialisation made: the heap pointers of that state); what the heap holds now is the process's
void NativeSplitReference(X86Context& ctx, const std::vector<uint8_t>& state) {
    // the process's one-time work first (docs 15.7): the timer's guarded block, run by its own code as the title's
    // first call would, so no player's initialisation sets it (it is protected start-up state from here on)
    uint32_t guard = R32(kTimerGuard);
    bool ran = false;
    if (!(guard & 1)) {
        g_timer_block_only = true;
        kernel::CallGuest(ctx, kTimer, {0});  // its argument and ebx are read only after the block (0xFC8CC on)
        g_timer_block_only = false;
        ran = true;
    }
    g_timer_once = nsplit::TimerOnce(guard, ran, R32(kTimerGuard), R32(kTimerPeriod));
    if (nsplit::MayAddPlayers(g_timer_once))
        LOGI("native split: the timer's one-time block (0xFC870) %s as the process before any player: guard 0x%08X, "
             "period 0x%08X", nsplit::Name(g_timer_once), R32(kTimerGuard), R32(kTimerPeriod));
    else
        LOGE("native split: the timer's one-time block (0xFC870) %s (guard 0x%08X, period 0x%08X): no player is added",
             nsplit::Name(g_timer_once), R32(kTimerGuard), R32(kTimerPeriod));
    LOGI("native split: protected ranges %u (%u bytes, fingerprint %016llX), taken addresses %u (fingerprint %016llX)",
         kProtectedStamp.count, kProtectedStamp.bytes, (unsigned long long)kProtectedStamp.hash, kAddressesStamp.count,
         (unsigned long long)kAddressesStamp.hash);
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
    // the clients that left since the last frame (a kick, the server's disconnect, their own disconnect, a connect that
    // failed): removed here, from player 1's context, outside the turns (docs 15.12). Player 1's departs only while
    // another plays (alone, it is the title's own: its menus)
    for (int k = 0; k < nsplit::kMaxPlayers; k++) {
        nsplit::State s = g_players.At(k).state;
        if (s != nsplit::State::Connecting && s != nsplit::State::Active) continue;
        if (!g_watch[k].Dropped(PlayerDword(k, kClsState))) continue;
        if (k == 0) {
            if (nsplit::OthersRemain(g_players, 0)) DepartPlayer1();
            continue;
        }
        LOGI("native split: player %d's client left the match (cls.state %u): removed", k + 1, PlayerDword(k, kClsState));
        RemovePlayer(ctx, k);
    }
    if (g_end_session) EndSession(ctx);
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
    if (!g_add_failed && g_leaks.MayJoin() && nsplit::MayAddPlayers(g_timer_once) && R32(kSvActive) &&
        int(R32(kClsState)) == kClsActive && g_players.Count() < Wanted() && g_players.Initialising() < 0) {
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
            if (int who = LeaveWho() - 1; who >= 0) {
                // its own "disconnect", run in its own turn (the real path)
                if (g_cmds.Add(who, "disconnect\n"))
                    LOGI("native split: player %d leaves (diagnostics): its own \"disconnect\" queued", who + 1);
            } else {
                RemovePlayer(ctx, last);
            }
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

// [[hook]] at 0x22E26A and 0x22E2B5 in realloc (0x22E1E7), just after each of its two heap paths' RtlReAllocateHeap
// (0x205CFF; 0x22E2B5 is also where a too large size lands, eax 0): eax the block (0: failed, the old one untouched),
// [ebp+8] the old block, esi the size asked (rounded up on the small-block path), [ebp+4] realloc's caller. The heap
// moves the block itself, seen by neither _heap_alloc nor free (docs 15.8): the object keeps its owner. A move of
// another owner's block is an error (it cannot be refused: the old block is already gone), but the string-intern
// pool's own two buffers (intern_pool.h: shared, appended by whoever opens a file)
void NativeSplitHeapRealloc(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || !ctx.eax) return;
    uint32_t old = R32(ctx.ebp + 8), site = R32(ctx.ebp + 4);
    int mover = OwnerNow();
    const heapown::Block* b = g_blocks.At(old);
    bool pool = old == R32(internpool::kPool + internpool::kStrings) || old == R32(internpool::kPool + internpool::kNodes);
    if (b && !pool && heapown::FreeIsForeign(*b, mover)) {
        g_foreign_moves++;
        LOGE("native split: %s moves block 0x%08X of %s (realloc to 0x%08X, return 0x%08X)",
             mover >= 0 ? ("player " + std::to_string(mover + 1)).c_str() : "the process", old,
             b->owner >= 0 ? ("player " + std::to_string(b->owner + 1)).c_str() : "the process's initialisation",
             ctx.eax, site);
    }
    g_moves++;
    g_blocks.Moved(old, ctx.eax, ctx.esi, mover, site, KindOf(site));
}

// [[hook]] at 0x22C609, free's entry: the block at [esp+4]; on_true "ret": a free by a player of a block that is not
// its own is not done (heap_owner.h SkipFree; player 1 included since docs 15.10), nor one of a player's blocks in the
// shared front end's scope (SkipFreeInSharedScope), reported as an error every time
bool NativeSplitHeapFree(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    uint32_t va = R32(ctx.esp + 4);
    if (!va) return false;
    int freeing = OwnerNow();
    const heapown::Block* b = g_blocks.At(va);
    int owner = b ? b->owner : heapown::Blocks::kNone;
    // a leaving player's cleanup: its own block that another names is not freed by its destructors (docs 15.12)
    if (g_releasing && b && g_release_keep.count(va)) {
        g_release_kept++;
        LOG_FIRST(16, "native split: player %d's cleanup keeps block 0x%08X (%u bytes, site 0x%08X): named by another "
                      "(return 0x%08X)", owner + 1, va, b->size, b->site, R32(ctx.esp));
        return true;
    }
    if ((g_ui_scope || g_voice_scope) && b && b->kind == heapown::Kind::Owned && heapown::SkipFreeInSharedScope(owner)) {
        g_ui_scope_kept++;
        g_foreign_frees++;
        g_skipped_frees++;
        LOGE("native split: the shared front end would free block 0x%08X of player %d (skipped: a player's object, "
             "not the shared UI's; return 0x%08X)", va, owner + 1, R32(ctx.esp));
        return true;
    }
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
    if (acting < 0 || !va) return true;  // the process; player 1 too is checked (docs 15.10)
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
// session's world (docs 15), decided by where it is asked (native_split_core.h ShutdownDecision, docs 15.12). On
// (false: it runs): outside the players' turns, or in player 1's turn alone. Its own CL_Disconnect, Host_ClearMemory
// and the hunk's release free the world, its models and the game DLL's entities, so the other local players are
// removed first (their server slots dropped, their objects released, their contexts removed: none left pointing into
// a freed world), the world's shared-once operations forgotten, the host's loopback channels emptied with the title's
// queues; player 1, if it had departed, the title's own player again. The removal switches contexts: only from player
// 1's context (the switcher's first). on_true "ret": in a player's turn while another plays, refused (that player's
// client leaves alone: the disconnect command, Host_EndGame or Host_Error in its turn); in the last player's turn when
// it is not player 1, deferred to the frame's end (EndSession, from player 1's context)
bool NativeSplitShutdownServer(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    if (R32(kSvActive)) {
        bool in_turn = csx::ClientContextInTurn();
        int acting = in_turn ? PlayerOfContext(csx::ClientContextActive()) : -1;
        nsplit::Shutdown d = nsplit::ShutdownDecision(in_turn, acting, g_players);
        if (d != nsplit::Shutdown::Proceed) {
            g_refused_shutdowns += d == nsplit::Shutdown::Refuse;
            if (d == nsplit::Shutdown::Defer) g_end_session = true;
            LOGI("native split: player %d's turn asks the built-in server to shut down (return 0x%08X): %s (%d local "
                 "player(s) playing)", acting + 1, R32(ctx.esp), nsplit::Name(d), g_players.Playing());
            return true;
        }
        int n = 0;
        auto order = nsplit::TeardownOrder(g_players, 0, n);
        if (n && csx::ClientContextActive() == 0) {
            for (int i = 0; i < n; i++) RemovePlayer(ctx, order[size_t(i)]);
            LOGI("native split: the server shuts down: %d other local player%s removed first, the world's shared state "
                 "forgotten", n, n == 1 ? "" : "s");
        } else if (n) {
            LOGE("native split: the server shuts down in player %d's turn: %d other local player%s not removed (only "
                 "player 1's context removes players)", PlayerOfContext(csx::ClientContextActive()) + 1, n,
                 n == 1 ? "" : "s");
        }
        g_world.Invalidate();
        g_end_session = false;
        if (g_players.Return0()) LOGI("native split: player 1 is the title's own player again (the session ends)");
        g_session.host = 0;
        g_ui.driver = 0;
    }
    g_ch.Clear();
    return false;
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

// [[hook]] at 0x19DF25 and 0x19DF43, IN_Init's stores to the shared pad bridge's loop bound 0x543A04 and previous button
// mask 0x5439B8 (10 bytes each; native_split_core.h SkipSharedPadStore, docs 15.14). In another player's
// initialisation (on_true the next instruction): not stored
bool NativeSplitPadBridgeStore(X86Context&, uint8_t*) {
    if (!nsplit::SkipSharedPadStore(g_players.Initialising() > 0)) return false;
    g_pad_stores_kept++;
    LOG_FIRST(2, "native split: IN_Init's store to the shared pad bridge skipped in another player's initialisation");
    return true;
}

// ---- a player's activation, in its own turn (docs 15.10) ----------------------------------------------------------

// client_context.cpp, at the start of a player's input turn (0x5D3B5, before its CL_Move): for every context but player
// 1's, _Host_Frame's per-player prelude, which the title runs before the turns (0x5D3A6 / 0x5D3B0) and so only in
// player 1's context (native_split_core.h PreludeInTurn): the activation check 0x269A0, then CL_UpdateClientData
// 0x1CC10, the real functions in this player's context, their own conditions unchanged (nothing is set here)
void RunPlayerLines(X86Context& ctx, int k);

void NativeSplitTurnPrelude(X86Context& ctx) {
    if (!NativeSplitOn()) return;
    int c = csx::ClientContextActive();
    int k = PlayerOfContext(c);
    if (!nsplit::PreludeInTurn(c)) {
        RunPlayerLines(ctx, k);  // player 1: its own console lines (docs 15.12); the title's prelude already ran
        return;
    }
    uint32_t before = R32(kClsState), signon = R32(kClsSignon);
    kernel::CallGuest(ctx, kActivationCheck, {});
    uint32_t after = R32(kClsState);
    if (after != before)
        LOGI("native split: player %d's activation check (0x269A0) in its own turn: client state %u -> %u (signon %u)",
             k + 1, before, after, signon);
    if (after != nsplit::Activation(before, signon))
        LOGE("native split: player %d's activation check gave client state %u, its rule %u (from %u, signon %u)", k + 1,
             after, nsplit::Activation(before, signon), before, signon);
    RunPlayerLines(ctx, k);  // its own console lines, where the title's prelude runs the buffer (0x5D3AB)
    kernel::CallGuest(ctx, kUpdateClientData, {});
    if (k >= 0) g_preludes[k]++;
}

// ---- the shared front end: one driver, run as the process (docs 15.10) --------------------------------------------

// the acting player for the shared front end's decision: the player whose turn it is (-1 outside the turns)
static int TurnPlayer() { return csx::ClientContextInTurn() ? PlayerOfContext(csx::ClientContextActive()) : -1; }

// a front-end routine reached from a player's turn (native_split_core.h SharedUiOwner): outside the turns (or inside
// its own scope) as the title's (false: the code runs on); in the driver's turn run here, in the shared front end's
// scope (its allocations and frees the process's), as `fn` with `args` (eax its result); in another player's turn not
// run. true: done here (the hook's on_true path skips the original)
static bool SharedUiGate(X86Context& ctx, UiGate gate, uint32_t fn, std::initializer_list<uint32_t> args) {
    if (!NativeSplitOn() || g_ui_scope) return false;
    int k = TurnPlayer();
    switch (g_ui.Decide(csx::ClientContextInTurn(), k)) {
        case nsplit::SharedUi::AsTitle: return false;
        case nsplit::SharedUi::Withheld:
            g_ui_withheld[gate]++;
            LOG_FIRST(8, "native split: player %d's turn reached the shared front end's %s: not run (its driver is "
                         "player %d)", k + 1, kUiGateName[gate], g_ui.driver + 1);
            return true;
        case nsplit::SharedUi::AsShared:
            break;
    }
    {
        Depth scope(g_ui_scope);  // kept right through a longjmp's unwinding
        ctx.eax = kernel::CallGuest(ctx, fn, args.begin(), args.size());
    }
    g_ui_shared[gate]++;
    return true;
}

// [[hook]] at 0x1B9359 in the scoreboard's update 0x1B92F0 (after its own per-player work: its fields cleared, its
// players sorted, 0x1B4E30 / 0x1B9370), at `push esi; call 0x1B9820`, on_true 0x1B935F (past both): 0x1B9820 (stdcall,
// the panel) frees and rebuilds the rows of the shared "Scoreboard" screen's list (the panel's +0xBC, 0x1B91E0)
bool NativeSplitScoreboardRows(X86Context& ctx, uint8_t*) {
    return SharedUiGate(ctx, kUiRows, kScoreboardRows, {ctx.esi});
}

// [[hook]] at 0xD96A0, a front-end element's text set (stdcall: the text; esi the element; 0x1B4000 writes the shared
// screens' texts through it), on_true 0xD96B5 (its `ret 4`)
bool NativeSplitUiText(X86Context& ctx, uint8_t*) { return SharedUiGate(ctx, kUiText, 0xD96A0, {R32(ctx.esp + 4)}); }

// [[hook]] at 0xCAB40, the front end's scoreboard or team menu open (edi kept; it queues "open ..." and clears the
// Scoreboard screen's +0xBC), on_true "ret"
bool NativeSplitUiMenu(X86Context& ctx, uint8_t*) { return SharedUiGate(ctx, kUiMenu, 0xCAB40, {}); }

// [[hook]] at 0x1BA732 in 0x1BA710 (a team change's scoreboard), at `call 0xCA0B0` (the shared screen stack's current
// screen closed), on_true 0x1BA737
bool NativeSplitUiCloseScreen(X86Context& ctx, uint8_t*) { return SharedUiGate(ctx, kUiClose, 0xCA0B0, {}); }

// [[hook]] at 0xCA6F0, the client game's front-end command (edx the command: "xbui <command>", or "!<line>", appended to
// the shared console's buffer 0x748660 by Cbuf_AddText 0x3B970, run at the next frame's start in player 1's context; no
// allocation): on_true "ret" in another player's turn than the driver's (a menu opened or closed on the shared screens,
// or a line run as player 1's), logged with its text
bool NativeSplitUiCommand(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || g_ui_scope) return false;
    int k = TurnPlayer();
    nsplit::SharedUi d = g_ui.Decide(csx::ClientContextInTurn(), k);
    if (d == nsplit::SharedUi::AsShared) g_ui_shared[kUiCommand]++;
    if (d != nsplit::SharedUi::Withheld) return false;
    char text[96] = "";
    if (ctx.edx) std::snprintf(text, sizeof text, "%s", static_cast<const char*>(kernel::HostPtr(ctx.edx)));
    // its own console line ("!slot1", "!impulse 101", ...): to its own queue, its xbui parts gated where they run
    if (nsplit::FrontEndCommand(text, false) == nsplit::FrontEnd::Run) return false;
    g_ui_withheld[kUiCommand]++;
    LOG_FIRST(16, "native split: player %d's front-end command \"%s\" withheld (the shared front end is player %d's)",
              k + 1, text, g_ui.driver + 1);
    return true;
}

// [[hook]] at 0xDD520 (a front-end list's clear) and 0xD5550 (a front-end row's destructor), the shared screens'
// element code the eighth run's frees came from (docs 15.10): log only. Reached in another player's turn than the
// driver's, it is shared UI code that path does not yet withhold (an error, with its caller)
void NativeSplitUiElements(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || g_ui_scope) return;
    int k = TurnPlayer();
    if (g_ui.Decide(csx::ClientContextInTurn(), k) != nsplit::SharedUi::Withheld) return;
    g_ui_elements_in_turn++;
    LOG_FIRST(16, "native split: the shared front end's element code ran in player %d's turn (return 0x%08X)", k + 1,
              R32(ctx.esp));
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

// [[hook]] at 0xFC8CC in the timer 0xFC870, where its guarded once-per-process block (FC87F..FC8C6) joins the path
// that skips it (FC87D): on_true 0xFC954, its epilogue (pop edi; add esp, 8; ret: the frame as at FC8CC). True only
// in the process's own call at the end of Host_Init (NativeSplitReference, docs 15.7): the block, nothing after it.
// Every other call goes on as the title's
bool NativeSplitTimerBlockOnly(X86Context&, uint8_t*) { return g_timer_block_only; }

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
    if (!NativeSplitOn() || !R32(kSvActive)) return false;
    bool in_turn = csx::ClientContextInTurn();
    nsplit::Shutdown d = nsplit::ShutdownDecision(in_turn, ActingPlayer(), g_players);
    if (d == nsplit::Shutdown::Proceed) return false;
    if (d == nsplit::Shutdown::Defer) g_end_session = true;
    LOGI("native split: player %d's client disconnects: the built-in server keeps running (its shutdown %s)",
         ActingPlayer() + 1, nsplit::Name(d));
    return true;
}

// the disconnecting client is not the window's (an added player's), or another local player plays: the shared parts
// its CL_Disconnect would end are kept (native_split_core.h LeaveKeepsShared; docs 15.12)
bool NativeSplitLeaveKeepsShared() {
    if (!NativeSplitOn()) return false;
    int acting = ActingPlayer();
    return acting != 0 || nsplit::LeaveKeepsShared(csx::ClientContextInTurn(), acting, g_players);
}

// [[hook]] at 0x28F4F in CL_Disconnect, its call of the sound system's stop-everything 0x106380 (ebx the one sound
// object 0x553ED8): on_true 0x28F54 (not called) when the shared parts are kept: another local player's sounds go on
bool NativeSplitDisconnectSound(X86Context&, uint8_t*) { return NativeSplitLeaveKeepsShared(); }

// [[hook]] at 0x29072 in CL_Disconnect: the free of the renderer's transparent-entity array 0x551EF4 and its size
// 0x551EF0 (one renderer: shared scratch, reallocated by every connect, 0x93180): on_true 0x29090 (kept) for another
// local player than the host, whose disconnect would leave the host's renderer without it until its next connect
bool NativeSplitDisconnectRenderer(X86Context&, uint8_t*) { return NativeSplitLeaveKeepsShared(); }

// ---- the console's lines, routed by where they are added (docs 15.12) -------------------------------------------
// native_split_lifecycle.h RouteText: a line added in a player's turn is that player's (its queue, run in its own turn
// where the title's prelude runs the buffer); the process's, the server's and the shared front end's go to the title's
// one buffer, run once a frame at 0x5D3AB in player 1's context, outside the turns, as before.

namespace {
// a player's lines run by the title's own Cbuf_Execute: they are put in the title's buffer (its data, cursize) for
// the run, the shared buffer's lines kept aside meanwhile (a line the shared front end adds then goes to them); what a
// "wait" left goes back to the front of its queue; the shared lines are put back after, also when a longjmp unwinds
// through the run
struct CbufSwap {
    int k, was;
    uint32_t data, max;
    uint16_t flags;
    std::string shared;
    CbufSwap(int player, const std::string& lines) : k(player), was(g_cmd_running) {
        data = R32(kCmdData);
        max = R32(kCmdMax);
        flags = R16(kCmdFlags);
        uint32_t size = std::min(R32(kCmdSize), max);
        shared.assign(static_cast<const char*>(kernel::HostPtr(data)), size);
        uint32_t n = uint32_t(std::min<size_t>(lines.size(), max - 1));
        std::memcpy(kernel::HostPtr(data), lines.data(), n);
        W32(kCmdSize, n);
        if (n < lines.size()) g_cmds.Return(k, lines.substr(n));  // never: a queue is bounded below the buffer's size
        g_cmd_running = k;
        g_swap = this;
    }
    ~CbufSwap() {
        uint32_t left = std::min(R32(kCmdSize), max);
        if (left) g_cmds.Return(k, std::string(static_cast<const char*>(kernel::HostPtr(data)), left));
        uint32_t n = uint32_t(std::min<size_t>(shared.size(), max - 1));
        std::memcpy(kernel::HostPtr(data), shared.data(), n);
        W32(kCmdSize, n);
        W16(kCmdFlags, flags);
        g_cmd_running = was;
        g_swap = nullptr;
    }
};

// a line for the title's buffer (mode 0 Cbuf_AddText, 1 Cbuf_InsertText, 2 the newline-wrapped insert): true when
// routed here (the original skipped), false when the title's own code appends it
bool RouteLine(X86Context&, uint32_t text, int mode) {
    if (!NativeSplitOn() || !text || !R32(kCmdData)) return false;
    // a player's initialisation (AddPlayer, in its context, outside the turns) adds its own lines, as its turn would
    bool own = csx::ClientContextInTurn() || g_init_owner > 0;
    int player = csx::ClientContextInTurn() ? TurnPlayer() : g_init_owner > 0 ? g_init_owner : -1;
    nsplit::Route r = nsplit::RouteText(g_cmd_running, g_ui_scope > 0 || g_voice_scope > 0,
                                        csx::ClientContextInServerFrame(), own, player);
    g_cmd_routed[int(r.origin)]++;
    std::string s = GuestString(text, nsplit::kQueueMax);
    if (mode == 2) s = "\n" + s + "\n";
    if (r.origin == nsplit::Origin::FrontEnd && g_swap) {
        // a player's lines are in the title's buffer: the shared front end's line goes to the shared lines kept aside
        if (g_swap->shared.size() + s.size() < g_swap->max)
            g_swap->shared = mode ? s + g_swap->shared : g_swap->shared + s;
        else
            LOG_FIRST(8, "native split: the shared console buffer is full: \"%.40s\" dropped", s.c_str());
        return true;
    }
    if (r.native) return false;
    bool ok = mode ? g_cmds.Insert(r.player, s) : g_cmds.Add(r.player, s);
    if (!ok)
        LOG_FIRST(8, "native split: player %d's console lines are full: \"%.40s\" dropped (as Cbuf_AddText's overflow)",
                  r.player + 1, s.c_str());
    return true;
}
}  // namespace

// [[hook]] at 0x3B970 Cbuf_AddText (edi the text), 0x3B9D0 Cbuf_InsertText (eax) and 0x3BA80 (the newline-wrapped
// insert, cdecl text): on_true "ret" when the line is a player's own (to its queue)
bool NativeSplitCbufAdd(X86Context& ctx, uint8_t*) { return RouteLine(ctx, ctx.edi, 0); }
bool NativeSplitCbufInsert(X86Context& ctx, uint8_t*) { return RouteLine(ctx, ctx.eax, 1); }
bool NativeSplitCbufInsertLine(X86Context& ctx, uint8_t*) { return RouteLine(ctx, R32(ctx.esp + 4), 2); }

// player k's queued lines run now, in its own context, by the title's Cbuf_Execute (its own lines' lines with them)
void RunPlayerLines(X86Context& ctx, int k) {
    if (k < 0 || g_cmd_running >= 0 || !g_cmds.Size(k) || !R32(kCmdData) || R32(kCmdMax) < 2) return;
    std::string lines = g_cmds.Take(k);
    CbufSwap swap(k, lines);
    g_cmd_runs[k]++;
    kernel::CallGuest(ctx, kCbufExecute, {});
}

// [[hook]] at 0x3BB50, Cbuf_Execute's entry (before pc_keys.cpp's PcKeysFrame there): on_true "ret" in a player's turn
// (a line added and run at once: the player's own queue runs, never the shared buffer in its context)
bool NativeSplitCbufExecute(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return false;
    int k = TurnPlayer();
    if (!nsplit::ExecuteRunsQueue(g_cmd_running, csx::ClientContextInTurn(), k)) return false;
    RunPlayerLines(ctx, k);
    return true;
}

// pc_keys.cpp: this Cbuf_Execute is a local player's run (its lines, or in its turn): no PC keys or console fed to it
bool NativeSplitCbufForPlayer() {
    return NativeSplitOn() && (g_cmd_running >= 0 || TurnPlayer() >= 0);
}

// [[hook]] at 0xD7450, the "xbui" console command (its arguments from Cmd_Argv): the shared front end's screens
// opened, closed, swapped (native_split_core.h SharedUiOwner): outside the turns as the title's (the shared buffer,
// player 1's context); in the driver's turn (its own lines) in the shared front end's scope, as the process; in
// another player's turn not run (on_true "ret", logged): its "!" lines' xbui parts
bool NativeSplitUiXbui(X86Context& ctx, uint8_t*) { return SharedUiGate(ctx, kUiXbui, kXbuiCommand, {}); }

// client_context.cpp: a frame cut short (Host_EndGame / Host_Error's longjmp) in a player's turn; player 1's context is
// live again. That player's client, if it left, is removed between frames as any leaving client
void NativeSplitAborted(int context) {
    g_aborts++;
    g_cmd_running = -1;
    g_swap = nullptr;
    int k = PlayerOfContext(context);
    LOGE("native split: the frame was cut short (Host_EndGame / Host_Error) in player %d's turn (context %d): player 1's "
         "context is live again", k + 1, context);
}

// ---- the voice channels: one set, the process's (docs 15.12) ---------------------------------------------------

namespace {
bool VoiceGate(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t withheld_eax) {
    if (!NativeSplitOn() || g_voice_scope) return false;
    switch (nsplit::VoiceDecide(csx::ClientContextInTurn(), csx::ClientContextActive())) {
    case nsplit::SharedUi::AsTitle: return false;
    case nsplit::SharedUi::Withheld:
        g_voice_withheld++;
        ctx.eax = withheld_eax;
        return true;
    case nsplit::SharedUi::AsShared: break;
    }
    {
        Depth scope(g_voice_scope);
        ctx.eax = kernel::CallGuest(ctx, fn, args.begin(), args.size());
    }
    g_voice_shared++;
    return true;
}
}  // namespace

// [[hook]] at 0x101FD0 Voice_Idle (cdecl frametime), 0x102620 the speaker's channel (edi the speaker; -1 none) and
// 0x102760 its data added (cdecl channel, data, length): on_true "ret". In context 0's turn run here in the voice
// scope (the codecs the process's); in another context's turn not run (no channel: -1; the parse read the data)
bool NativeSplitVoiceIdle(X86Context& ctx, uint8_t*) { return VoiceGate(ctx, kVoiceIdle, {R32(ctx.esp + 4)}, 0); }
bool NativeSplitVoiceAssign(X86Context& ctx, uint8_t*) { return VoiceGate(ctx, kVoiceAssign, {}, 0xFFFFFFFFu); }
bool NativeSplitVoiceData(X86Context& ctx, uint8_t*) {
    return VoiceGate(ctx, kVoiceData, {R32(ctx.esp + 4), R32(ctx.esp + 8), R32(ctx.esp + 12)}, 0);
}

// ---- a kick or a drop of a local player (docs 15.12) -------------------------------------------------------------

// [[hook]] at 0x30932 in the client's svc_disconnect handler (0x30910: its reason printed, then Host_EndGame("Server
// disconnected"), whose longjmp to _Host_Frame's setjmp is not a continuation here: a guest trap), the call itself
// (its argument pushed; on_true 0x30937, the caller's `add esp, 0xC`): for a local player whose leaving keeps the shared
// parts (an added player's client, or player 1's while another plays), its own CL_Disconnect (0x28F20) instead, in
// its own context, its turn: its connection ended alone (the server's shutdown refused there, its sounds and the
// renderer's list kept; native_split_core.h ShutdownDecision); it is removed (or departs) between frames. Player 1
// alone: the title's own
bool NativeSplitServerDisconnected(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || !csx::ClientContextInTurn() || !NativeSplitLeaveKeepsShared()) return false;
    int k = PlayerOfContext(csx::ClientContextActive());
    kernel::CallGuest(ctx, 0x28F20, {});
    LOGI("native split: player %d's server disconnected it (svc_disconnect: a kick or a drop): its client disconnects "
         "alone (cls.state %u), the others play on", k + 1, R32(kClsState));
    return true;
}
