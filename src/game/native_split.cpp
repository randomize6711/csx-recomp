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
#include "common/pc_settings.h"
#include "core/input.h"
#include "game/native_split_view.h"
#include "xdk/d3d8.h"

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
// diagnostics (docs 15.19): a departed player 1 rejoins this many seconds after its departure (0, unset: it stays
// departed until the session ends, as before); its own client's reconnect, in its kept context
double RejoinAfter() {
    static const double s = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_REJOIN_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    return s;
}
// diagnostics: remove the last added player after this many seconds in the match, add one again after as many more
double LeaveAfter() {
    static const double s = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_LEAVE_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    return s;
}

// diagnostics (docs 15.23): how many of the timed leaves happen: 1 (unset) one, N that many, 0 one after every join
// (the repetition the earlier scenarios used)
int LeaveTimes() {
    static const int n = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_LEAVE_TIMES");
        int x = v ? std::atoi(v) : 1;
        return x >= 0 ? x : 1;
    }();
    return n;
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
uint64_t g_foreign_frees = 0, g_skipped_frees = 0, g_double_frees = 0, g_stale_names = 0, g_cleanup_refused = 0;
// the VGUI hierarchy (docs 15.17): a process-wide vector's grow run as the process from a player's turn (its depth;
// how many; its old buffer a player's, not freed), the process's panels that are parents of a player's panel, a
// leaving player's viewport detached from its parent or kept on it, a shared parent's stale children reported
int g_vector_scope = 0;
uint64_t g_vector_grows = 0, g_vector_free_skipped = 0, g_detached = 0, g_detach_kept = 0, g_stale_children = 0;
// what the process's panels' setParent / addChild / removeChild calls did (docs 15.18: nsplit::Link), each call once;
// the cleanup's own detachments among the detachments; the App focus fields cleared at a leave; a leaving player's
// objects among App's tick signals or surfaces (kept, reported)
uint64_t g_links[nsplit::kLinks] = {};
nsplit::LinkTracker g_track;  // docs 15.20: removals pending until re-added (a cycle) or the frame's end (permanent)
uint64_t g_cleanup_detaches = 0, g_focus_cleared = 0, g_app_kept = 0;
bool g_cleanup_detaching = false;
std::set<uint32_t> g_shared_parents;
// the cleanup's destructors by outcome (nsplit::Step) and the frees made through them: the leaving player's own blocks
// freed while its cleanup runs; frees of a block that is no block start but inside one (reported, never seen by the
// planner's rules: docs 15.18)
uint64_t g_steps[4] = {};
uint64_t g_released_blocks = 0, g_released_bytes = 0, g_interior_frees = 0;
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
// player 1's rejoins (docs 15.19): begun, in the match again, refused (by their reason's first), its departures'
// server slot dropped by the cleanup (its own "dropclient" had not), its stale loopback packets dropped
uint64_t g_p1_rejoins = 0, g_p1_rejoined = 0, g_p1_rejoin_refused = 0, g_p1_slot_drops = 0, g_p1_stale_packets = 0;
std::chrono::steady_clock::time_point g_p1_departed_at{};
// player 1 departed in this session (its server still the one its first connect registered the delta descriptions
// with): its connects from here on parse them into its own slot (native_split_core.h DeltaSlot); the session's end
// (the server shut down) makes it the title's own again
bool g_p1_rejoin_session = false;
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
// the departures pending until the frame's end (native_split_core.h Departures, docs 15.15): recorded at a confirmed
// disconnect in the player's own turn; the cause of the CL_Disconnect running now (svc_disconnect's: a kick); the
// reconnects held (CL_CheckForResend, the connect packet), the console bytes dropped for a held player, the removals
nsplit::Departures g_departing;
nsplit::Leave g_disconnect_cause = nsplit::Leave::Disconnect;
uint64_t g_resends_held = 0, g_connects_held = 0, g_held_lines = 0, g_pending_removals = 0, g_watch_removals = 0;
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
// docs 15.23: player 1's memory over its departures, the serial its latest connection began at, the diagnostic leaves
nsplit::Player1Retention g_p1_ret;
uint64_t g_p1_conn_serial = 0;
int g_diag_leaves = 0;

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
    int r = nsplit::AllocationOwner(g_ui_scope > 0 || g_voice_scope > 0 || g_vector_scope > 0, g_world.Running(),
                                    g_init_owner, in_turn,
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

uint32_t ServerClientOf(int k);
int SlotOf(uint32_t cl);
uint64_t g_add_waits = 0, g_add_stale_packets = 0;
void AddPlayer(X86Context& ctx, int k) {
    // docs 15.22: no server slot may still name its loopback port; what is still queued on it is an earlier
    // connection's (dropped)
    if (nsplit::AddWaitsForSlot(R32(kSvActive) != 0, ServerClientOf(k) != 0)) {
        if (!g_add_waits++)
            LOGW("native split: player %d is not added yet: server slot %d still names its loopback port (an earlier "
                 "connection's) [watch:stale-slot]", k + 1, SlotOf(ServerClientOf(k)));
        return;
    }
    if (size_t stale = g_ch.ClientPending(k)) {
        g_add_stale_packets += stale;
        LOGI("native split: player %d's loopback port held %zu packet(s) of an earlier connection: dropped", k + 1, stale);
    }
    g_ch.Forget(k);
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
            g_leaks.Abandoned(bytes, g_blocks.LeftBehindBytes());
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
                LOGE("native split: player %d's boot image holds %zu heap pointers (it must hold none) [own:boot-pointer]",
                     k + 1, inherited);
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
            // a fresh context names nothing a player that left handed to the process (docs 15.16): a stale reference,
            // reported (an error), not failed (the block is kept, never freed while named: no use after a free)
            for (const heapown::Pointer& s : heapown::LeftBehindNames(Spans(), g_blocks, [](uint32_t va) { return R32(va); })) {
                if (++g_stale_names <= 16)
                    LOGE("native split: player %d's 0x%08X names 0x%08X in block 0x%08X a player that left handed to the "
                         "process (stale) [own:stale-left]", k + 1, s.va, s.value, s.block);
            }
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
                    LOGE("native split: player %d's 0x%08X names 0x%08X, not its own (owner %d%s, %s) [own:not-own]", k + 1,
                         p.va, mine, b ? b->owner : -2, b && b->left_by >= 0 ? ", left behind by a player" : "",
                         b ? heapown::Name(b->kind) : "no known block");
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
                     "byte(s); %s) [own:protected]",
                     k + 1, c.va, c.size, ProtectedWhat(c.va));
            }
            // the string-intern pool (docs 15.8): its growth fields are not compared byte for byte (region_evidence.py
            // REVIEWED_POOL); its invariants are, and a broken one fails the player as a protected change does
            {
                internpool::State pool_after = internpool::Capture(GuestByte);
                std::vector<std::string> bad = internpool::Check(pool_before, pool_after, PoolBlock);
                for (const std::string& b : bad) {
                    corrupted++;
                    LOGE("native split: player %d's initialisation broke the string-intern pool 0x%08X: %s [own:protected]",
                         k + 1, internpool::kPool, b.c_str());
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
        g_leaks.Abandoned(bytes, g_blocks.LeftBehindBytes());
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

// the shared vectors the title grows with 0xB52B0 ({count, capacity, data}; headers outside every player's regions:
// the VGUI text-image cache 0x553844 (0xB4AC0), the VGUI font list 0x5620A8 (0x10CC00), two static ones). A grow run
// in a player's turn frees the process's buffer (a foreign free, skipped) and leaves the vector on that player's
// buffer (docs 15.16, the twelfth run's 0x00C93FF0). Reported (a warning) whenever a buffer is a player's or one a
// leaving player left behind; each buffer once
void WatchSharedVectors() {
    static const struct {
        uint32_t va;
        const char* what;
    } kVectors[] = {{0x553844, "the VGUI text-image cache"}, {0x5620A8, "the VGUI font list"},
                    {0x384B60, "a static vector 0x384B60"}, {0x384D28, "a static vector 0x384D28"}};
    static std::set<uint32_t> seen;
    for (const auto& v : kVectors) {
        uint32_t data = R32(v.va + 8);
        const heapown::Block* b = data ? g_blocks.At(data) : nullptr;
        if (!b || (b->owner < 0 && b->left_by < 0) || !seen.insert(data).second) continue;
        LOGW("native split: %s (0x%08X, %u of %u entries) is on block 0x%08X of %s%d (site 0x%X): a process-wide vector "
             "grown in a player's turn [watch:shared-vector]",
             v.what, v.va, R32(v.va), R32(v.va + 4), data, b->owner >= 0 ? "player " : "the process, left behind by player ",
             (b->owner >= 0 ? b->owner : b->left_by) + 1, b->site);
    }
}

// ---- the VGUI hierarchy's shared parents (docs 15.17; native_split_lifecycle.h) ------------------------------------

// a guest word that may be read: in the image, or inside a known heap block (R32 itself checks nothing)
bool Readable(uint32_t va) {
    if (heapown::IsImageAddress(va) && va + 4 <= heapown::kImageHi) return true;
    uint32_t start = va >= 0x10000 ? g_blocks.Holding(va) : 0;
    const heapown::Block* b = start ? g_blocks.At(start) : nullptr;
    return b && va + 4 <= start + b->size;
}
uint32_t ReadOr0(uint32_t va) { return Readable(va) ? R32(va) : 0; }

// a VGUI panel: a readable object whose vtable (in the image) has the Panel's setParent and removeChild
bool IsPanel(uint32_t va) {
    uint32_t vt = va ? ReadOr0(va) : 0;
    return vt && heapown::IsImageAddress(vt) &&
           nsplit::IsPanelVtable(ReadOr0(vt + nsplit::kPanelSetParentSlot), ReadOr0(vt + nsplit::kPanelRemoveChildSlot));
}

int OwnerOfHolding(uint32_t va) {
    uint32_t start = va >= 0x10000 ? g_blocks.Holding(va) : 0;
    const heapown::Block* b = start ? g_blocks.At(start) : nullptr;
    return b ? b->owner : heapown::Blocks::kNone;
}

std::string OwnerName(int owner) {
    if (owner >= 0) return "player " + std::to_string(owner + 1);
    return owner == heapown::kProcess ? "the process" : "no known block";
}

// a panel of the process's (a process block, or one in the image): the shared parents (docs 15.17)
bool ProcessPanel(uint32_t p) {
    if (!p) return false;
    int o = OwnerOfHolding(p);
    return (o == heapown::kProcess || (o == heapown::Blocks::kNone && heapown::IsImageAddress(p))) && IsPanel(p);
}

// how many times child is among parent's children (0 when the vector is not sane)
int ListedIn(uint32_t parent, uint32_t child) {
    int32_t n = int32_t(ReadOr0(parent + nsplit::kPanelChildren)), cap = int32_t(ReadOr0(parent + nsplit::kPanelChildren + 4));
    uint32_t data = ReadOr0(parent + nsplit::kPanelChildren + 8);
    if (n <= 0 || n > cap || cap > 4096 || !data || !Readable(data) || !Readable(data + 4 * uint32_t(n - 1))) return 0;
    int listed = 0;
    for (int32_t i = 0; i < n; i++) listed += R32(data + 4 * uint32_t(i)) == child;
    return listed;
}

// a setParent / addChild of a panel whose old or new parent is the process's, classified at its entry (docs 15.18:
// nsplit::ClassifyLink): every call counted once by what it does; a re-attachment (the child already the parent's:
// the title takes it off and appends it again, no change) only counted; a structural change logged for the first few
// when a player is involved, a duplicate attempt as a warning
void PanelLink(uint32_t child, uint32_t old, uint32_t parent, const char* how, uint32_t ret) {
    if (!child) return;
    bool shared_new = ProcessPanel(parent), shared_old = old && old != parent && ProcessPanel(old);
    if (!shared_new && !shared_old) return;
    nsplit::Link l = nsplit::ClassifyLink(old, parent, shared_new ? ListedIn(parent, child) : 0);
    if (shared_new) g_shared_parents.insert(parent);
    if (shared_old) g_shared_parents.insert(old);
    // docs 15.20: setParent(0) off a process panel is a removal pending until the frame's end; an attachment completes
    // a pending removal of the same child (a temporary remove / re-add: counted, not logged)
    if (l == nsplit::Link::Detach) {
        g_track.Removed(old, child, ret);
        return;
    }
    if (shared_new && l != nsplit::Link::Duplicate && l != nsplit::Link::Nothing) l = g_track.Attached(parent, child, l, ret);
    g_links[int(l)]++;
    if (l == nsplit::Link::Reattach || l == nsplit::Link::Nothing || l == nsplit::Link::Cycle) return;
    int acting = OwnerNow(), child_owner = OwnerOfHolding(child);
    uint32_t on = parent ? parent : old;
    uint32_t data = ReadOr0(on + nsplit::kPanelChildren + 8);
    if (l == nsplit::Link::Duplicate) {
        LOG_FIRST(16, "native split: %s's turn %s panel 0x%08X (%s's) to the process's panel 0x%08X: %s (its parent "
                      "0x%08X, listed %d) [watch:shared-parent]",
                  OwnerName(acting).c_str(), how, child, OwnerName(child_owner).c_str(), parent, nsplit::Name(l), old,
                  ListedIn(parent, child));
        return;
    }
    if (acting < 0 && child_owner < 0) return;  // the process's own tree: counted, not logged
    LOG_FIRST(24, "native split: %s's turn %s panel 0x%08X (%s's): %s, parent 0x%08X -> 0x%08X; the process's panel "
                  "0x%08X: %d of %d children, buffer 0x%08X (%s's) [watch:shared-parent]",
              OwnerName(acting).c_str(), how, child, OwnerName(child_owner).c_str(), nsplit::Name(l), old, parent, on,
              int32_t(ReadOr0(on + nsplit::kPanelChildren)), int32_t(ReadOr0(on + nsplit::kPanelChildren + 4)), data,
              OwnerName(OwnerOfHolding(data)).c_str());
}

// the frame's end (docs 15.20): the removals no re-add completed are permanent detachments: counted, a player's (or one
// a player's turn made) logged for the first few
void SettleLinks() {
    g_track.ForEachPending([](uint32_t parent, uint32_t child, uint32_t ret) {
        g_links[int(nsplit::Link::Detach)]++;
        int child_owner = OwnerOfHolding(child);
        if (child_owner < 0) return;  // the process's own tree: counted, not logged
        LOG_FIRST(16, "native split: panel 0x%08X (%s's) permanently detached from the process's panel 0x%08X (removal "
                      "returning to 0x%08X; not added again in its frame) [watch:shared-parent]",
                  child, OwnerName(child_owner).c_str(), parent, ret);
    });
    g_track.Settle();
}

// the shared parents' children (native_split_lifecycle.h StaleChildren): a departed player's panel, one that is no
// known block any more, an entry twice: an error, once each [own:stale-child]
void WatchSharedParents() {
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    for (auto it = g_shared_parents.begin(); it != g_shared_parents.end();) {
        uint32_t parent = *it;
        if (!IsPanel(parent)) {
            it = g_shared_parents.erase(it);
            continue;
        }
        ++it;
        uint32_t data = ReadOr0(parent + nsplit::kPanelChildren + 8);
        int32_t cap = int32_t(ReadOr0(parent + nsplit::kPanelChildren + 4));
        if (data && !(Readable(data) && (cap <= 0 || Readable(data + 4 * uint32_t(cap - 1))))) continue;
        for (const nsplit::ChildFinding& f : nsplit::StaleChildren(parent, g_blocks, [](uint32_t va) { return ReadOr0(va); })) {
            if (!seen.insert({parent, f.child}).second) continue;
            g_stale_children++;
            char why[64];
            if (f.left_by >= 0)
                std::snprintf(why, sizeof why, "a panel player %d left behind", f.left_by + 1);
            else
                std::snprintf(why, sizeof why, "%s", f.left_by == -2 ? "no known block (freed)" : "a panel");
            LOGE("native split: the process's panel 0x%08X keeps child 0x%08X: %s%s (%d of %d children) [own:stale-child]",
                 parent, f.child, why, f.duplicate ? ", listed twice" : "", int32_t(ReadOr0(parent + nsplit::kPanelChildren)),
                 cap);
        }
    }
}

void ReportRender();  // the views' diagnostics (M3, below)
void FinishViews();   // the window frame's end (M3, below)

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
    ReportRender();
    LOGI("native split: %llu packets dropped by full channels, %llu read outside their player's turn, %llu foreign frees "
         "(%llu skipped), %llu reallocs moved (%llu of another owner's block), %zu heap blocks tracked",
         (unsigned long long)g_ch.Dropped(), (unsigned long long)g_read_outside_turn,
         (unsigned long long)g_foreign_frees, (unsigned long long)g_skipped_frees, (unsigned long long)g_moves,
         (unsigned long long)g_foreign_moves, g_blocks.Count());
    {
        using L = nsplit::Link;
        auto n = [](L l) { return (unsigned long long)g_links[int(l)]; };
        LOGI("native split: the VGUI hierarchy (the process's panels): %llu new attachments, %llu permanent detachments "
             "(%llu by the cleanup), %llu temporary remove / re-add cycles (%llu by 0x108BB0's move to the front), %llu "
             "re-attachments (no change), %llu parent changes, %llu duplicate attachment attempts; "
             "%llu process-wide vectors grown as the process; failed ownership operations: %llu old buffers of a player "
             "kept, %llu viewports kept on their parent, %llu stale children; %llu viewports detached; %zu shared "
             "parents watched; App focus fields cleared %llu, a leaving player's objects kept among App's tick signals "
             "or surfaces %llu",
             n(L::New), n(L::Detach), (unsigned long long)g_cleanup_detaches, n(L::Cycle),
             (unsigned long long)g_track.Reorders(), n(L::Reattach), n(L::Change), n(L::Duplicate), (unsigned long long)g_vector_grows, (unsigned long long)g_vector_free_skipped,
             (unsigned long long)g_detach_kept, (unsigned long long)g_stale_children, (unsigned long long)g_detached,
             g_shared_parents.size(), (unsigned long long)g_focus_cleared, (unsigned long long)g_app_kept);
    }
    LOGI("native split: ownership: %llu double frees skipped, %llu stale names of left-behind blocks in fresh contexts, "
         "%llu cleanup calls refused, %llu frees inside a block (not its start)", (unsigned long long)g_double_frees,
         (unsigned long long)g_stale_names, (unsigned long long)g_cleanup_refused, (unsigned long long)g_interior_frees);
    LOGI("native split: the cleanup's destructors: %llu not considered, %llu considered and skipped, %llu invoked and "
         "returned, %llu invoked and failed; %llu of the leaving players' blocks (%llu bytes) freed by their cleanups",
         (unsigned long long)g_steps[0], (unsigned long long)g_steps[1], (unsigned long long)g_steps[2],
         (unsigned long long)g_steps[3], (unsigned long long)g_released_blocks, (unsigned long long)g_released_bytes);
    WatchSharedVectors();
    WatchSharedParents();
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
         "player's turn, %llu departures of player 1, %d leaves (%zu bytes left behind in all, %zu still allocated, player 1's growth "
         "over its rejoins %zu, the bound %zu on those); the voice channels "
         "run as the process %llu times, withheld in another context's turn %llu; cleanup frees of a named block kept %llu; "
         "IN_Init's stores to the shared pad bridge skipped in another player's initialisation %llu",
         (unsigned long long)g_aborts, (unsigned long long)g_refused_shutdowns, (unsigned long long)g_departures,
         g_leaks.leaves, g_leaks.left_behind, g_leaks.outstanding, g_leaks.dormant, g_leaks.limit, (unsigned long long)g_voice_shared,
         (unsigned long long)g_voice_withheld, (unsigned long long)g_release_kept,
         (unsigned long long)g_pad_stores_kept);
    LOGI("native split: player 1: %s; departures %llu (its server slot dropped by the cleanup %llu: its own "
         "\"dropclient\" had not), rejoins begun %llu, in the match again %llu, refused %llu; stale loopback packets "
         "dropped %llu", nsplit::Name(g_players.At(0).state), (unsigned long long)g_departures,
         (unsigned long long)g_p1_slot_drops, (unsigned long long)g_p1_rejoins, (unsigned long long)g_p1_rejoined,
         (unsigned long long)g_p1_rejoin_refused, (unsigned long long)g_p1_stale_packets);
    LOGI("native split: departures: %llu recorded at a confirmed disconnect (%llu repeats), removed at the frame's end "
         "%llu (by the watch alone %llu); a held player's reconnects skipped: CL_CheckForResend %llu, connect packets "
         "%llu; its console bytes dropped %llu",
         (unsigned long long)g_departing.Marks(), (unsigned long long)g_departing.Repeats(),
         (unsigned long long)g_pending_removals, (unsigned long long)g_watch_removals,
         (unsigned long long)g_resends_held, (unsigned long long)g_connects_held, (unsigned long long)g_held_lines);
    for (sharedonce::Op op : {sharedonce::Op::WorldRenderer, sharedonce::Op::SignonUi, sharedonce::Op::Materials})
        LOGI("native split: %s: built %llu, a player's part only %llu, failed %llu, as the title %llu",
             sharedonce::Name(op), (unsigned long long)g_world.Runs(op), (unsigned long long)g_world.Skipped(op),
             (unsigned long long)g_world.Failed(op), (unsigned long long)g_world.TitleRuns(op));
}

// why a leaving player's blocks are kept (docs 15.16, 15.18): each kept block followed back through the kept blocks of
// its own that name it to its root, the word that names the first of them: a word of the shared data or of an active
// player's regions, a live process block, another active player's block, a block an earlier departed player left
// behind, or a shared kind. Each root logged with its evidence (nsplit::EvidenceOf: verified, unverified pointer-like
// bytes, unclassified), whether its word names the block's start (direct) or a place inside it (interior), and the
// longest chain from it; each kept block is counted under one root only (the first the scan reached it from). The
// verified roots' directly named blocks are returned (`verified`): what they keep alive is counted apart. The
// string-intern pool's search key (0x5622B0, the last find's string) named on its own line
void LogRetention(int k, const heapown::RollbackPlan& plan, std::vector<uint32_t>& verified) {
    std::map<uint32_t, const heapown::RollbackItem*> by;
    for (const heapown::RollbackItem& r : plan.keep) by[r.va] = &r;
    struct Root {
        size_t blocks = 0, bytes = 0;
        const heapown::RollbackItem* first = nullptr;  // the block it names directly (the first found)
        nsplit::RootKind kind = nsplit::RootKind::StaticWord;
        nsplit::Evidence evidence = nsplit::Evidence::Unclassified;
        uint32_t container = 0, container_size = 0, container_site = 0;  // a block root: the block holding the word
        std::vector<uint32_t> chain;  // the longest: from the block it names to the deepest it keeps
    };
    std::map<std::string, Root> roots;
    std::map<std::string, Root> reasons;
    size_t by_evidence[3][2] = {};
    std::set<uint32_t> seeds;
    for (const heapown::RollbackItem& r : plan.keep) {
        const heapown::RollbackItem* at = &r;
        std::vector<uint32_t> path = {r.va};
        for (int hops = 0; hops < 4096 && at->reason && std::strstr(at->reason, "kept block of its own"); hops++) {
            auto it = by.find(g_blocks.Holding(at->named_at));
            if (it == by.end() || it->second == at) break;
            at = it->second;
            path.push_back(at->va);
        }
        nsplit::RootKind kind;
        uint32_t site = at->site, cstart = 0;
        const heapown::Block* cb = nullptr;
        char key[160];
        if (!at->reason || std::strstr(at->reason, "shared kind")) {
            kind = nsplit::RootKind::SharedKind;
            std::snprintf(key, sizeof key, "a shared kind (site 0x%X)", at->site);
        } else if (std::strstr(at->reason, "shared data")) {
            kind = csx::ClientContextOffsetOf(at->named_at) >= 0 ? nsplit::RootKind::RegionWord : nsplit::RootKind::StaticWord;
            const nsplit::StaticArea* area = nsplit::StaticAreaOf(at->named_at);
            std::snprintf(key, sizeof key, "the word at 0x%08X%s%s%s", at->named_at,
                          at->named_at == 0x5622B0 ? " (the string-intern pool's search key)" : "",
                          area && at->named_at != 0x5622B0 ? ", in " : "",
                          area && at->named_at != 0x5622B0 ? area->what : "");
        } else if ((cb = g_blocks.At(cstart = g_blocks.Holding(at->named_at)))) {
            kind = cb->owner >= 0     ? nsplit::RootKind::PlayerBlock
                   : cb->left_by >= 0 ? nsplit::RootKind::LeftBehindBlock
                                      : nsplit::RootKind::ProcessBlock;
            site = cb->site;
            if (kind == nsplit::RootKind::PlayerBlock)
                std::snprintf(key, sizeof key, "a block of player %d (site 0x%X)", cb->owner + 1, cb->site);
            else if (kind == nsplit::RootKind::LeftBehindBlock)
                std::snprintf(key, sizeof key, "a block player %d left behind (site 0x%X)", cb->left_by + 1, cb->site);
            else
                std::snprintf(key, sizeof key, "a process block (site 0x%X)", cb->site);
        } else {
            kind = nsplit::RootKind::StaticWord;
            std::snprintf(key, sizeof key, "0x%08X", at->named_at);
        }
        Root& root = roots[key];
        if (!root.first) {
            root.first = at, root.kind = kind;
            root.evidence = nsplit::EvidenceOf(kind, at->named_at, site);
            if (cb) root.container = cstart, root.container_size = cb->size, root.container_site = cb->site;
        }
        if (path.size() > root.chain.size()) root.chain.assign(path.rbegin(), path.rend());
        root.blocks++, root.bytes += r.size;
        by_evidence[int(root.evidence)][0]++, by_evidence[int(root.evidence)][1] += r.size;
        if (root.evidence == nsplit::Evidence::Verified) seeds.insert(at->va);
        reasons[r.reason ? r.reason : "?"].blocks++, reasons[r.reason ? r.reason : "?"].bytes += r.size;
    }
    verified.assign(seeds.begin(), seeds.end());
    for (const auto& [why, n] : reasons)
        LOGI("native split: player %d's cleanup keeps %zu block(s), %zu bytes: %s", k + 1, n.blocks, n.bytes, why.c_str());
    LOGI("native split: player %d's cleanup: its kept blocks by their root's evidence (each under one root): verified %zu "
         "(%zu bytes), unverified pointer-like bytes %zu (%zu bytes), unclassified %zu (%zu bytes); every one kept",
         k + 1, by_evidence[0][0], by_evidence[0][1], by_evidence[1][0], by_evidence[1][1], by_evidence[2][0],
         by_evidence[2][1]);
    std::vector<std::pair<std::string, Root>> top(roots.begin(), roots.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    for (size_t i = 0; i < top.size() && i < 12; i++) {
        const Root& r = top[i].second;
        const heapown::RollbackItem* f = r.first;
        char how[96], where[96] = "", chain[200] = "";
        if (r.kind == nsplit::RootKind::SharedKind)
            std::snprintf(how, sizeof how, "its kind");
        else if (f->named_value == f->va)
            std::snprintf(how, sizeof how, "directly (0x%08X)", f->named_value);
        else
            std::snprintf(how, sizeof how, "at +0x%X (0x%08X: an interior pointer)", f->named_value - f->va, f->named_value);
        if (r.container)
            std::snprintf(where, sizeof where, "; the word at 0x%08X, +0x%X of its %u-byte block%s", f->named_at,
                          f->named_at - r.container, r.container_size,
                          nsplit::ByteSiteOf(r.container_site) ? " (file bytes)" : "");
        size_t used = 0;
        for (size_t j = 0; j < r.chain.size() && used + 40 < sizeof chain; j++) {
            if (r.chain.size() > 5 && j == 3) {
                used += size_t(std::snprintf(chain + used, sizeof chain - used, " -> ..."));
                j = r.chain.size() - 2;
                continue;
            }
            const heapown::Block* cbk = g_blocks.At(r.chain[j]);
            used += size_t(std::snprintf(chain + used, sizeof chain - used, "%s0x%08X (site 0x%X)", j ? " -> " : "",
                                         r.chain[j], cbk ? cbk->site : 0));
        }
        const nsplit::PersistentSite* ps = nsplit::PersistentSiteOf(f->site);
        LOGI("native split:   kept through %s [%s, %s]: %zu block(s), %zu bytes (it names block 0x%08X, %u bytes from "
             "site 0x%X%s%s, %s%s); the longest chain %zu block(s): %s",
             top[i].first.c_str(), nsplit::Name(r.kind), nsplit::Name(r.evidence), r.blocks, r.bytes, f->va, f->size,
             f->site, IsPanel(f->va) ? ", a VGUI panel" : "", ps ? ", persistent by the original" : "", how, where,
             r.chain.size(), chain);
    }
    const heapown::Block* key = g_blocks.At(g_blocks.Holding(R32(0x5622B0)));
    LOGI("native split: player %d's cleanup: the string-intern pool's search key names %s", k + 1,
         !key ? "no heap block" : key->owner == k ? "one of its blocks (kept: named)" : "another owner's block");
}

// what of player k's own blocks someone else names (heap_owner.h PlanRollback): the shared data (the image's .data with
// player 1's live regions: called from player 1's context), every other context's saved copy, another owner's block;
// followed through its own blocks. Those its cleanup must not free (its destructors' frees of them are skipped)
std::set<uint32_t> NamedElsewhere(int k, int context, size_t& released_bytes, size_t& kept_bytes,
                                  std::vector<uint32_t>& verified) {
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
    LogRetention(k, plan, verified);
    return keep;
}

// docs 15.21: the leaving player's particle pool (site 0x1F9BBE) and every word outside its own state that names a
// place in it: reported (bytes or a reference) with the dry run's verdict; the pool is kept whatever it says
uint64_t g_pool_verdicts[4] = {}, g_pools_reclaimed = 0;
// docs 15.21: experimental, off unless CSX_NATIVE_SPLIT_RECLAIM_POOL=1 (never in the permitted runs)
bool ReclaimPoolOn() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_RECLAIM_POOL");
        return v && v[0] == '1' && !v[1];
    }();
    return on;
}
struct PoolSeen {
    uint32_t pool = 0, size = 0;
    nsplit::PoolReclaim verdict = nsplit::PoolReclaim::NotFound;
};
PoolSeen PoolDryRun(int k, int context) {
    constexpr uint32_t kDataLo = 0x36C8A0, kDataHi = 0x7D6F20;
    uint32_t pool = 0, size = 0;
    g_blocks.ForEach([&](uint32_t va, const heapown::Block& b) {
        if (!pool && b.owner == k && b.site == nsplit::kPoolSite) pool = va, size = b.size;
    });
    size_t bytes_n = 0, other_n = 0;
    uint32_t first_other = 0;
    std::string first_what;
    auto seen = [&](uint32_t at, uint32_t v, nsplit::Evidence e, const char* what) {
        if (v < pool || v - pool >= size) return;
        if (e == nsplit::Evidence::Unverified) {
            bytes_n++;
            return;
        }
        if (!other_n++) first_other = at, first_what = what;
    };
    if (pool) {
        for (uint32_t va = kDataLo; va + 4 <= kDataHi; va += 4) {
            uint32_t v = R32(va);
            if (v < pool || v - pool >= size) continue;
            const nsplit::StaticArea* a = nsplit::StaticAreaOf(va);
            bool region = csx::ClientContextOffsetOf(va) >= 0;
            seen(va, v, a ? a->evidence : nsplit::Evidence::Unclassified,
                 region ? "a word of player 1's regions" : a ? a->what : "a word of the process's static data");
        }
        std::vector<heapown::Span> spans = Spans();
        for (int c = 1; c < csx::ClientContextCount(); c++) {
            if (c == context || c == csx::ClientContextActive()) continue;
            const std::vector<uint8_t>& copy = csx::ClientContextSaved(c);
            for (const heapown::Span& sp : spans)
                for (uint32_t o = 0; o + 4 <= sp.size && sp.offset + o + 4 <= copy.size(); o += 4) {
                    uint32_t v;
                    std::memcpy(&v, copy.data() + sp.offset + o, 4);
                    seen(sp.va + o, v, nsplit::Evidence::Unclassified, "a word of another context's regions");
                }
        }
        g_blocks.ForEach([&](uint32_t va, const heapown::Block& b) {
            if (b.owner == k || !Readable(va) || !Readable(va + b.size - 1)) return;
            nsplit::Evidence e = nsplit::ByteSiteOf(b.site) ? nsplit::Evidence::Unverified : nsplit::Evidence::Unclassified;
            for (uint32_t o = 0; o + 4 <= b.size; o += 4) seen(va + o, R32(va + o), e, "a word of another owner's block");
        });
    }
    nsplit::PoolReclaim v = nsplit::PoolVerdict(pool != 0, bytes_n, other_n);
    g_pool_verdicts[int(v)]++;
    char first[160] = "";
    if (other_n) std::snprintf(first, sizeof first, " (the first at 0x%08X, %s)", first_other, first_what.c_str());
    LOGI("native split: player %d's particle pool 0x%08X (%u bytes, site 0x%X): named by %zu word(s) of unverified "
         "bytes, %zu other word(s)%s: %s; %s [watch:pool]",
         k + 1, pool, size, nsplit::kPoolSite, bytes_n, other_n, first, nsplit::Name(v),
         nsplit::ReclaimsPool(ReclaimPoolOn(), v) ? "EXPERIMENTAL: its block is freed by the cleanup"
                                                   : "dry run only, the pool kept");
    return {pool, size, v};
}

// a cleanup's guest call (native_split_lifecycle.h kCleanupCalls, docs 15.16): exactly the contract's registers set to
// `object` and exactly its arguments pushed; a call through a vtable made only when the object's vtable is its
// constructor's, to the contract's function (never a target read from guest memory). False (nothing called; the
// object kept and reported) when the call would not meet its contract
bool CallCleanup(X86Context& ctx, nsplit::CleanupFn id, uint32_t object, std::initializer_list<uint32_t> args,
                 int log_player = -1) {
    const nsplit::CleanupCall& c = nsplit::Contract(id);
    uint8_t set = 0;
    uint32_t saved[6] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi};
    if (c.regs & nsplit::kRegEax) ctx.eax = object, set |= nsplit::kRegEax;
    if (c.regs & nsplit::kRegEcx) ctx.ecx = object, set |= nsplit::kRegEcx;
    if (c.regs & nsplit::kRegEdx) ctx.edx = object, set |= nsplit::kRegEdx;
    if (c.regs & nsplit::kRegEbx) ctx.ebx = object, set |= nsplit::kRegEbx;
    if (c.regs & nsplit::kRegEsi) ctx.esi = object, set |= nsplit::kRegEsi;
    if (c.regs & nsplit::kRegEdi) ctx.edi = object, set |= nsplit::kRegEdi;
    bool ok = nsplit::MeetsContract(c, set, args.size()) && c.id == id;
    uint32_t vt = (c.vtable || c.slot >= 0) && object ? ReadOr0(object) : 0;
    uint32_t entry = c.slot >= 0 && heapown::IsImageAddress(vt) ? ReadOr0(vt + uint32_t(c.slot)) : 0;
    if (ok && !nsplit::VtableMatches(c, vt)) ok = false;
    if (ok && !nsplit::SlotMatches(c, entry)) ok = false;
    if (ok && log_player >= 0)  // docs 15.18: the call entered (its contract met); "returned" is logged after it
        LOGI("native split: player %d's cleanup: %s (0x%X) for 0x%08X entered (%s the object, %zu argument(s))",
             log_player + 1, c.name, c.fn, object,
             c.regs & nsplit::kRegEcx ? "ecx" : c.regs & nsplit::kRegEsi ? "esi" : c.regs & nsplit::kRegEax ? "eax"
             : c.regs ? "a register" : "no register:", args.size());
    if (ok) kernel::CallGuest(ctx, c.fn, args.begin(), args.size());
    ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4], ctx.edi = saved[5];
    if (!ok) {
        g_cleanup_refused++;
        LOGE("native split: the cleanup does not call %s (0x%08X) for 0x%08X: %s; the object kept [own:cleanup-refused]",
             c.name, c.fn, object,
             c.vtable && vt != c.vtable ? "its vtable is not its constructor's"
             : c.slot >= 0 && entry != c.fn ? "its vtable does not have the function at the slot"
                                            : "the call would not meet its contract");
    }
    return ok;
}

// a leaving player's viewport (its 0x552B7C, read from its saved context) taken off the process's panel it is a child
// of, by the title's own removeChild (slot 0x4C, as setParent does; native_split_lifecycle.h DetachCheck, the contract
// DetachChild), before what of its own another names is planned: never deleted (no destructor; its block left behind)
void DetachViewport(X86Context& ctx, int k, int context) {
    uint32_t vp = 0;
    if (context == csx::ClientContextActive()) {
        vp = R32(nsplit::kViewport);
    } else if (long off = csx::ClientContextOffsetOf(nsplit::kViewport); off >= 0) {
        const std::vector<uint8_t>& copy = csx::ClientContextSaved(context);
        if (size_t(off) + 4 <= copy.size()) std::memcpy(&vp, copy.data() + off, 4);
    }
    nsplit::PanelView v;
    const heapown::Block* b = vp ? g_blocks.At(vp) : nullptr;
    v.known = b != nullptr;
    v.owner = b ? b->owner : heapown::Blocks::kNone;
    v.panel = b && IsPanel(vp);
    v.parent = b ? ReadOr0(vp + nsplit::kPanelParent) : 0;
    uint32_t data = 0;
    if (v.parent) {
        v.parent_owner = OwnerOfHolding(v.parent);
        v.parent_panel = IsPanel(v.parent);
        v.count = int32_t(ReadOr0(v.parent + nsplit::kPanelChildren));
        v.capacity = int32_t(ReadOr0(v.parent + nsplit::kPanelChildren + 4));
        data = ReadOr0(v.parent + nsplit::kPanelChildren + 8);
        v.data_ok = data && v.capacity > 0 && v.capacity <= 4096 && Readable(data) &&
                    Readable(data + 4 * uint32_t(v.capacity - 1));
        if (v.data_ok && v.count >= 0 && v.count <= v.capacity)
            for (int32_t i = 0; i < v.count; i++) v.listed += R32(data + 4 * uint32_t(i)) == vp;
    }
    nsplit::Detach d = nsplit::DetachCheck(vp, v, k);
    if (d == nsplit::Detach::None) return;
    bool called = false;
    if (d == nsplit::Detach::Detach) {
        g_cleanup_detaching = true;
        called = CallCleanup(ctx, nsplit::CleanupFn::DetachChild, v.parent, {vp});
        g_cleanup_detaching = false;
    }
    if (called) {
        int32_t after = int32_t(R32(v.parent + nsplit::kPanelChildren));
        int still = 0;
        for (int32_t i = 0; i < after && i < v.capacity; i++) still += R32(data + 4 * uint32_t(i)) == vp;
        bool done = !still && after == v.count - 1 && R32(vp + nsplit::kPanelParent) == 0 &&
                    R32(v.parent + nsplit::kPanelChildren + 8) == data;
        g_shared_parents.insert(v.parent);
        g_detached += done;
        g_detach_kept += !done;
        if (done)
            LOGI("native split: player %d's viewport 0x%08X detached from the process's panel 0x%08X (removeChild 0x%X): "
                 "its children %d -> %d of %d, buffer 0x%08X unchanged; the viewport not deleted (no destructor: left "
                 "behind) [watch:shared-parent]",
                 k + 1, vp, v.parent, nsplit::kPanelRemoveChild, v.count, after, v.capacity, data);
        else
            LOGE("native split: player %d's viewport 0x%08X: removeChild on 0x%08X did not take it off (children %d -> "
                 "%d, still listed %d) [own:stale-child]",
                 k + 1, vp, v.parent, v.count, after, still);
        return;
    }
    g_detach_kept++;
    if (v.parent && v.parent_owner != k) g_shared_parents.insert(v.parent);  // watched: a kept child is reported
    LOGW("native split: player %d's viewport 0x%08X not detached: %s (parent 0x%08X of %s, %d of %d children, listed %d)",
         k + 1, vp, nsplit::Name(d), v.parent, OwnerName(v.parent_owner).c_str(), v.count, v.capacity, v.listed);
}

// the VGUI App's focus fields naming a panel of the leaving player cleared (docs 15.18: nsplit::ClearFocus; 0 as App's
// removeSurfaceBase 0x109B20 stores, nothing called), and its objects among App's tick signals or surfaces reported
// (kept: no verified removal). The App is the static object 0x384B58 only when 0x552740 names it with App's vtable
void ClearAppFocus(int k) {
    if (R32(nsplit::kAppInstance) != nsplit::kApp || R32(nsplit::kApp) != nsplit::kAppVtable) {
        LOGW("native split: player %d's cleanup: the VGUI App is not the known one (0x552740 0x%08X, vtable 0x%08X): its "
             "focus fields not read", k + 1, R32(nsplit::kAppInstance), R32(nsplit::kApp));
        return;
    }
    for (const nsplit::FocusField& f : nsplit::kAppFocus) {
        uint32_t va = nsplit::kApp + f.offset, v = R32(va);
        if (!v || !nsplit::ClearFocus(IsPanel(v), OwnerOfHolding(v), k)) continue;
        W32(va, 0);
        g_focus_cleared++;
        LOGI("native split: player %d's panel 0x%08X was the VGUI App's %s (0x%08X): cleared to 0 as App's "
             "removeSurfaceBase (0x109B20) does, the panel not called [watch:app-focus]",
             k + 1, v, f.name, va);
    }
    for (auto [off, what] : {std::pair{nsplit::kAppTickSignals, "tick signals"}, std::pair{nsplit::kAppSurfaces, "surfaces"}}) {
        uint32_t hdr = nsplit::kApp + off;
        int32_t n = int32_t(R32(hdr)), cap = int32_t(R32(hdr + 4));
        uint32_t data = R32(hdr + 8);
        if (n <= 0 || n > cap || cap > 4096 || !Readable(data) || !Readable(data + 4 * uint32_t(n - 1))) continue;
        for (int32_t i = 0; i < n; i++) {
            uint32_t v = R32(data + 4 * uint32_t(i));
            if (OwnerOfHolding(v) != k) continue;
            g_app_kept++;
            LOGW("native split: player %d's object 0x%08X is among the VGUI App's %s (%d of %d): kept, no verified "
                 "removal (App reaches it every tick) [watch:app-kept]",
                 k + 1, v, what, i + 1, n);
        }
    }
}

// a cleanup destructor run and its outcome logged (docs 15.18, nsplit::Step): not considered (no object at `holder`),
// considered and skipped (`skip`: why; or its call's contract not met), invoked (CallCleanup logs "entered") and
// returned with its object as its contract leaves it (what it freed counted), or invoked and failed (an error). Never
// reported as run because the cleanup around it completed. True: invoked and returned
bool RunDestructor(X86Context& ctx, int k, nsplit::CleanupFn id, uint32_t holder, uint32_t obj, const char* skip,
                   std::initializer_list<uint32_t> args, nsplit::Post post) {
    const nsplit::CleanupCall& c = nsplit::Contract(id);
    if (!obj) {
        g_steps[int(nsplit::Step::NotConsidered)]++;
        LOGI("native split: player %d's cleanup: %s (0x%X) not considered: no object (0x%08X holds 0)", k + 1, c.name,
             c.fn, holder);
        return false;
    }
    const heapown::Block* b = g_blocks.At(obj);
    std::string whose = b ? OwnerName(b->owner) : heapown::IsImageAddress(obj) ? "the image" : "no known block";
    if (skip) {
        g_steps[int(nsplit::Step::Skipped)]++;
        LOGI("native split: player %d's cleanup: %s (0x%X) for 0x%08X (%s's, from 0x%08X) considered, skipped: %s; the "
             "object kept", k + 1, c.name, c.fn, obj, whose.c_str(), holder, skip);
        return false;
    }
    uint64_t n0 = g_released_blocks, b0 = g_released_bytes, kept0 = g_release_kept;
    if (!CallCleanup(ctx, id, obj, args, k)) {
        g_steps[int(nsplit::Step::Skipped)]++;
        LOGI("native split: player %d's cleanup: %s (0x%X) for 0x%08X considered, skipped: its call would not meet its "
             "contract; the object kept", k + 1, c.name, c.fn, obj);
        return false;
    }
    bool after = post != nsplit::Post::NotABlock && g_blocks.At(obj) != nullptr;
    nsplit::Step st = nsplit::AfterCall(true, post, after);
    g_steps[int(st)]++;
    if (st == nsplit::Step::Returned)
        LOGI("native split: player %d's cleanup: %s (0x%X) for 0x%08X invoked and returned: %llu of its blocks (%llu "
             "bytes) freed through it, %llu frees of a named block kept; the object %s",
             k + 1, c.name, c.fn, obj, (unsigned long long)(g_released_blocks - n0),
             (unsigned long long)(g_released_bytes - b0), (unsigned long long)(g_release_kept - kept0),
             post == nsplit::Post::NotABlock ? "a static slot" : after ? "still allocated (its free follows)" : "freed");
    else
        LOGE("native split: player %d's cleanup: %s (0x%X) for 0x%08X invoked and failed: it returned with its object %s "
             "(%s) [own:cleanup-failed]",
             k + 1, c.name, c.fn, obj, after ? "still allocated" : "gone",
             post == nsplit::Post::Deletes ? "a deleting destructor frees it" : "it must leave it allocated");
    return st == nsplit::Step::Returned;
}

// docs 15.20: the heap's count of what leaving players left that is still allocated can never be more than what they
// left; if it is, the two disagree (an accounting fault): reported once, and the gate still uses the heap's count (the
// larger)
void CheckLeakAccounting() {
    static bool told = false;
    if (g_leaks.Consistent() || told) return;
    told = true;
    LOGW("native split: the retention accounting disagrees: %zu bytes still allocated of %zu left behind (the bound uses "
         "the larger) [watch:leak-accounting]", g_leaks.outstanding, g_leaks.left_behind);
}

void RemovePlayer(X86Context& ctx, int k) {
    const nsplit::Player p = g_players.At(k);
    if (!g_players.Leave(k)) return;
    // docs 15.12 (native_split_lifecycle.h CleanupOrder): nothing reaches it any more first
    if (uint32_t cl = R32(kSvActive) ? ServerClientOf(k) : 0) {
        GuestStrings gs(ctx);
        uint32_t fmt = gs.Put("%s"), reason = gs.Put("left the split-screen game");
        CallCleanup(ctx, nsplit::CleanupFn::DropClient, 0, {cl, 0, fmt, reason});
    }
    g_ch.Forget(k);
    if (size_t n = g_cmds.Drop(k)) {
        g_cmd_dropped += n;
        LOGI("native split: player %d's %zu bytes of console lines dropped (never run in another context)", k + 1, n);
    }
    if (int was = g_ui.driver; g_ui.Left(k, g_players) != was)
        LOGI("native split: the shared front end is driven by player %d now (player %d leaves)", g_ui.driver + 1, k + 1);
    g_session.host = nsplit::SessionHost(g_players);
    // its viewport off the process's root panel (docs 15.17), from player 1's context; the VGUI App's focus on a panel
    // of its own cleared (docs 15.18)
    DetachViewport(ctx, k, p.context);
    ClearAppFocus(k);
    // its memory before its cleanup (docs 15.18): its own blocks, and what earlier leaves left that is still allocated
    size_t live_n = 0, live_bytes = 0, earlier_n = 0, earlier_bytes = 0;
    g_blocks.ForEach([&](uint32_t, const heapown::Block& b) {
        if (b.owner == k) live_n++, live_bytes += b.size;
        else if (b.left_by >= 0) earlier_n++, earlier_bytes += b.size;
    });
    uint64_t released0 = g_released_blocks, released_bytes0 = g_released_bytes;
    // what of its own another names, from player 1's context (before its own is made live); what the verified roots
    // keep alive on their own
    size_t plan_free = 0, plan_kept = 0;
    std::vector<uint32_t> verified_seeds;
    g_release_keep = NamedElsewhere(k, p.context, plan_free, plan_kept, verified_seeds);
    const PoolSeen pool = PoolDryRun(k, p.context);  // docs 15.21: a report (its free experimental, off)
    const std::set<uint32_t> kept_by_plan = g_release_keep;
    const std::set<uint32_t> verified_reach =
        heapown::ReachableFrom(g_blocks, k, verified_seeds, [](uint32_t va) { return ReadOr0(va); });
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
    auto release = [&](uint32_t obj) {  // ReleaseCheck without its log: a 0x552C70 child's
        const heapown::Block* b = obj ? g_blocks.At(obj) : nullptr;
        return nsplit::ReleaseCheck(b != nullptr, b ? b->owner : heapown::Blocks::kNone,
                                    b && b->kind == heapown::Kind::Owned, k, g_release_keep.count(obj) > 0);
    };
    auto skip_of = [&](uint32_t obj) -> const char* {  // why a destructor of obj may not run (nullptr: it may)
        nsplit::Release r = release(obj);
        return r == nsplit::Release::Release ? nullptr : nsplit::Name(r);
    };
    if (uint32_t ents = R32(kClientEntities); own(ents)) {  // its client entities (0x2E2E0)
        if (CallCleanup(ctx, nsplit::CleanupFn::Free, 0, {ents})) {
            W32(kClientEntities, 0);
            released++;
        }
    }
    // deleting destructors through the vtable (as 0x1A7F7E, 0x1B667A: ecx the object, the delete flag 1); 0x552C70's
    // own objects (+0x1C..+0x38, deleted by its destructor through their vtables) each released by the same rule first
    for (auto [va, id] : {std::pair{0x552C70u, nsplit::CleanupFn::Delete552C70},
                          std::pair{0x552C9Cu, nsplit::CleanupFn::Delete552C9C}}) {
        uint32_t obj = R32(va);
        const char* skip = obj ? skip_of(obj) : nullptr;
        if (obj && !skip && id == nsplit::CleanupFn::Delete552C70) {
            nsplit::Release each[nsplit::k552C70ChildCount];
            for (uint32_t i = 0; i < nsplit::k552C70ChildCount; i++)
                each[i] = release(R32(obj + nsplit::k552C70Children + 4 * i));
            if (!nsplit::ChildrenReleasable(each, nsplit::k552C70ChildCount))
                skip = "an object it deletes is not released (named by another, not its own or a shared kind)";
        }
        if (RunDestructor(ctx, k, id, va, obj, skip, {1}, nsplit::Post::Deletes)) {
            W32(va, 0);
            released++;
        }
    }
    // its destructor 0x1BAD90 (esi the object, as 0x1BAE40 / 0x1A6C80; it leaves the object allocated) and its free
    if (uint32_t obj = R32(0x552CA8);
        RunDestructor(ctx, k, nsplit::CleanupFn::Destroy552CA8, 0x552CA8, obj, obj ? skip_of(obj) : nullptr, {},
                      nsplit::Post::Keeps)) {
        CallCleanup(ctx, nsplit::CleanupFn::Free, 0, {obj});
        W32(0x552CA8, 0);
        released++;
    }
    for (uint32_t node = R32(0x5623D0); own(node);) {  // the HUD element list's nodes (as gHUD.Init 0x1A6A10)
        uint32_t next = R32(node + 4);
        if (!CallCleanup(ctx, nsplit::CleanupFn::Free, 0, {node})) break;
        released++;
        node = next;
    }
    W32(0x5623D0, 0);
    {  // its delta descriptions' slot (0x45670 frees a slot's description, esi the slot, as 0x2FBAD)
        const guest_tables::Table& t = guest_tables::kTables[guest_tables::k_native_split_delta_slots];
        uint32_t slot = t.va + uint32_t(k) * t.elem, desc = R32(slot);
        if (RunDestructor(ctx, k, nsplit::CleanupFn::FreeDeltaSlot, slot, desc ? slot : 0, desc ? skip_of(desc) : nullptr,
                          {}, nsplit::Post::NotABlock))
            released++;
    }
    // docs 15.21, experimental (off unless CSX_NATIVE_SPLIT_RECLAIM_POOL=1): its particle pool's one block freed by
    // the title's free, as its own, when only bytes name it; never its destructor
    if (pool.pool && nsplit::ReclaimsPool(ReclaimPoolOn(), pool.verdict) && g_blocks.At(pool.pool) &&
        g_blocks.At(pool.pool)->owner == k && g_blocks.At(pool.pool)->site == nsplit::kPoolSite) {
        g_release_keep.erase(pool.pool);
        if (CallCleanup(ctx, nsplit::CleanupFn::Free, 0, {pool.pool}, k) && !g_blocks.At(pool.pool)) {
            g_pools_reclaimed++;
            released++;
            LOGW("native split: player %d's particle pool 0x%08X (%u bytes) freed by the cleanup (EXPERIMENTAL "
                 "reclamation; its CMiniMem object kept) [watch:pool-reclaimed]", k + 1, pool.pool, pool.size);
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
    // its memory accounted (docs 15.18: nsplit::Retained, each block once): released by its cleanup; left behind as
    // persistent by the original, reachable from verified roots, kept conservatively, or named by nobody; with the
    // sites that leave the most
    {
        size_t cat[5][2] = {};
        cat[int(nsplit::Retained::Released)][0] = size_t(g_released_blocks - released0);
        cat[int(nsplit::Retained::Released)][1] = size_t(g_released_bytes - released_bytes0);
        std::map<uint32_t, std::pair<size_t, size_t>> by_site;
        g_blocks.ForEach([&](uint32_t va, const heapown::Block& b) {
            if (b.owner != k) return;
            nsplit::Retained r = nsplit::Categorize(nsplit::PersistentSiteOf(b.site) != nullptr, kept_by_plan.count(va) > 0,
                                                    verified_reach.count(va) > 0);
            cat[int(r)][0]++, cat[int(r)][1] += b.size;
            by_site[b.site].first++, by_site[b.site].second += b.size;
        });
        using R = nsplit::Retained;
        LOGI("native split: player %d's memory at its leave: %zu blocks (%zu bytes) live before its cleanup; released %zu "
             "(%zu bytes); left behind: persistent by the original %zu (%zu bytes), reachable from verified roots %zu "
             "(%zu bytes), kept conservatively %zu (%zu bytes), named by nobody (no release mechanism) %zu (%zu bytes); "
             "earlier departures' blocks still allocated %zu (%zu bytes); each block counted once",
             k + 1, live_n, live_bytes, cat[int(R::Released)][0], cat[int(R::Released)][1], cat[int(R::Persistent)][0],
             cat[int(R::Persistent)][1], cat[int(R::Verified)][0], cat[int(R::Verified)][1],
             cat[int(R::Conservative)][0], cat[int(R::Conservative)][1], cat[int(R::Unreferenced)][0],
             cat[int(R::Unreferenced)][1], earlier_n, earlier_bytes);
        std::vector<std::pair<uint32_t, std::pair<size_t, size_t>>> top(by_site.begin(), by_site.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        for (size_t i = 0; i < top.size() && i < 12; i++) {
            const nsplit::PersistentSite* ps = nsplit::PersistentSiteOf(top[i].first);
            LOGI("native split:   left behind from site 0x%X: %zu block(s), %zu bytes%s%s", top[i].first,
                 top[i].second.first, top[i].second.second, ps ? ": " : "", ps ? ps->what : "");
        }
    }
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
    WatchSharedVectors();  // a process-wide vector left on its buffer (kept: named)
    WatchSharedParents();  // a shared parent still listing its viewport (not detached)
    csx::ClientContextRemove(p.context);
    g_players.ContextRemoved(p.context);
    g_players.Released(k);
    g_watch[k] = {};
    g_departing.Clear(k);  // the slot empty: a later join there is a fresh player, its client the title's own again
    if (size_t n = g_cmds.Drop(k)) g_cmd_dropped += n;  // nothing of it survives its removal
    g_leaks.Left(leaked_bytes, g_blocks.LeftBehindBytes());
    LOGI("native split: player %d left: its server slot dropped, %zu of its objects released, %zu left (%zu bytes), its "
         "context removed; left behind by leaving players so far %zu bytes in %d leave(s), %zu of them (%zu blocks) "
         "still allocated, %zu released by the game since",
         k + 1, released, leaked, leaked_bytes, g_leaks.left_behind, g_leaks.leaves, g_leaks.outstanding,
         g_blocks.LeftBehindCount(), g_leaks.ReleasedSince());
    CheckLeakAccounting();
    if (long more = g_leaks.Projected(); more >= 0)
        LOGI("native split: the retention bound: %zu of %zu bytes still held by leaving players after %d leave(s) (%zu "
             "left in all), %zu a leave on average: about %ld more leave(s) before it stops joins (a projection from "
             "the leaves so far, not a guaranteed outcome)",
             g_leaks.outstanding, g_leaks.limit, g_leaks.leaves, g_leaks.left_behind,
             g_leaks.outstanding / size_t(g_leaks.leaves), more);
    if (!g_leaks.MayJoin() && !g_add_failed) {
        g_add_failed = true;
        LOGE("native split: leaving players still hold %zu bytes (the bound %zu; %zu left in all): no player joins again "
             "this session", g_leaks.outstanding, g_leaks.limit, g_leaks.left_behind);
    }
    // player 1 departed and nobody plays any more: the session ends between frames (as the process)
    if (g_players.At(0).state == nsplit::State::Departed && g_players.Playing() == 0) g_end_session = true;
}

// player 1's UI while it is away and when it rejoins (docs 15.19), reported, never changed: its viewport (the client
// game's, made in its context at start-up) stays on the process's root panel with its context, for its rejoin (its
// client game is not initialised again, so nothing would attach it again); App's focus fields and lists may name its
// panels, which stay allocated (no use after free; App sets its focus again every tick). From player 1's context
void ReportPlayer1Ui(const char* when) {
    uint32_t vp = R32(nsplit::kViewport);
    uint32_t parent = vp && g_blocks.At(vp) ? ReadOr0(vp + nsplit::kPanelParent) : 0;
    int listed = 0;
    if (parent) {
        int32_t n = int32_t(ReadOr0(parent + nsplit::kPanelChildren)), cap = int32_t(ReadOr0(parent + nsplit::kPanelChildren + 4));
        uint32_t data = ReadOr0(parent + nsplit::kPanelChildren + 8);
        if (n > 0 && n <= cap && cap <= 4096 && Readable(data) && Readable(data + 4 * uint32_t(n - 1)))
            for (int32_t i = 0; i < n; i++) listed += R32(data + 4 * uint32_t(i)) == vp;
    }
    std::string focus;
    if (R32(nsplit::kAppInstance) == nsplit::kApp && R32(nsplit::kApp) == nsplit::kAppVtable)
        for (const nsplit::FocusField& f : nsplit::kAppFocus) {
            uint32_t v = R32(nsplit::kApp + f.offset);
            if (v && OwnerOfHolding(v) == 0) focus += std::string(focus.empty() ? "" : ", ") + f.name;
        }
    LOGI("native split: player 1's UI %s: its viewport 0x%08X %s the process's panel 0x%08X (listed %d), kept with its "
         "context; App's focus fields naming its panels: %s (left: they stay allocated) [watch:p1-ui]",
         when, vp, listed == 1 ? "on" : "NOT on", parent, listed, focus.empty() ? "none" : focus.c_str());
}

// player 1's client left while another plays (docs 15.12): it departs, its context kept (the switcher's first: the
// window's, the process's console runs there); the server, the world and the process's systems stay; the shared front
// end and the session's host go to the lowest player still playing; no slot or identity renumbered. Its objects are
// the process's (its initialisation's) or its turns': none released (its context stays)
void DepartPlayer1(X86Context& ctx) {
    if (!g_players.Depart0()) return;
    g_departures++;
    g_p1_departed_at = std::chrono::steady_clock::now();
    g_p1_rejoin_session = true;
    g_watch[0] = {};
    g_departing.Clear(0);  // held from here on as Departed (native_split_core.h Held) until the session's end
    if (size_t n = g_cmds.Drop(0)) g_cmd_dropped += n;
    // its server slot (docs 15.19): its CL_Disconnect's "dropclient" reached the server in this frame's server half
    // (its input turn comes first); a slot still naming its loopback port is dropped as an added player's is
    // (SV_DropClient as the process, from player 1's context, outside the turns), never left to a later connect
    if (uint32_t cl = R32(kSvActive) ? ServerClientOf(0) : 0) {
        g_p1_slot_drops++;
        int slot = SlotOf(cl);
        GuestStrings gs(ctx);
        uint32_t fmt = gs.Put("%s"), reason = gs.Put("left the split-screen game");
        CallCleanup(ctx, nsplit::CleanupFn::DropClient, 0, {cl, 0, fmt, reason});
        LOGW("native split: player 1's server slot %d was still in use after its disconnect: dropped by the cleanup "
             "(SV_DropClient) [watch:p1-slot]", slot);
    }
    // what is still queued to or from its loopback port is of the ended connection (the server's disconnect message
    // included): dropped, so its next connection starts with none
    size_t stale = g_ch.ClientPending(0);
    g_ch.Forget(0);
    g_p1_stale_packets += stale;
    g_ui.Left(0, g_players);
    g_session.host = nsplit::SessionHost(g_players);
    size_t n = 0, bytes = 0, since_n = 0, since_bytes = 0;
    std::map<uint32_t, nsplit::SiteSum> by_site;
    g_blocks.ForEach([&](uint32_t, const heapown::Block& b) {
        if (b.owner == 0) {
            n++;
            bytes += b.size;
            if (b.serial > g_p1_conn_serial) {
                since_n++;
                since_bytes += b.size;
                nsplit::SiteSum& t = by_site[b.site];
                t.site = b.site;
                t.blocks++;
                t.bytes += b.size;
            }
        }
    });
    // docs 15.23: what it holds measured against its first departure; the growth counted against the leak bound
    g_p1_ret.Departed(bytes);
    g_leaks.dormant = g_p1_ret.Growth();
    std::string sites;
    for (const nsplit::SiteSum& t : nsplit::TopSites(by_site, 8))
    {
        char b[96];
        std::snprintf(b, sizeof b, "%s0x%06X %zu block(s) %zu bytes", sites.empty() ? "" : ", ", t.site, t.blocks,
                      t.bytes);
        sites += b;
    }
    LOGI("native split: player 1's memory at departure %d: %zu bytes held by its turns' objects (%zu at its first "
         "departure: %zu more, %zu in the last cycle; counted against the bound %zu with the %zu bytes leaving players "
         "still hold); from its latest connection %zu block(s), %zu bytes, by site: %s [watch:p1-retained]",
         g_p1_ret.departures, bytes, g_p1_ret.baseline, g_p1_ret.Growth(), g_p1_ret.last_cycle, g_leaks.limit,
         g_leaks.outstanding, since_n, since_bytes, sites.empty() ? "none" : sites.c_str());
    LOGI("native split: player 1 left the match (its client disconnected): the built-in server and the world stay for %d "
         "other player(s); the shared front end is driven by player %d, the session's host is player %d; its context "
         "kept, %zu of its turns' objects (%zu bytes) kept with it; its server slot %s; %zu stale packet(s) to it "
         "dropped",
         g_players.Playing(), g_ui.driver + 1, g_session.host + 1, n, bytes,
         ServerClientOf(0) ? "still in use [watch:p1-slot]" : "free", stale);
    ReportPlayer1Ui("at its departure");
}

// a departed player 1 rejoins (docs 15.19; native_split_core.h MayRejoin0), between frames in its own context: held no
// longer, its client connects again by the title's own CL_CheckForResend in its next turn (state 1 with the server
// active: "localhost"), in its kept context (nothing rebuilt, no process object made again). Its watch, console
// lines and loopback packets start empty; the shared front end and the session's host stay where they are (no
// hand-over while the others play). False (logged once a reason) when not allowed
bool RejoinPlayer1() {
    nsplit::Rejoin r = nsplit::MayRejoin0(g_players, g_departing, R32(kSvActive) != 0, ServerClientOf(0) != 0,
                                          R32(kClsState));
    if (r != nsplit::Rejoin::Allowed) {
        static nsplit::Rejoin last = nsplit::Rejoin::Allowed;
        if (r != last) {
            g_p1_rejoin_refused++;
            LOGW("native split: player 1 does not rejoin yet: %s", nsplit::Name(r));
        }
        last = r;
        return false;
    }
    // docs 15.23: one more cycle at the last one's growth kept within the leak bound (fail closed: it stays departed)
    if (!g_leaks.MayRejoin(g_p1_ret.NextCycle())) {
        static bool told = false;
        if (!told) {
            told = true;
            g_p1_rejoin_refused++;
            LOGW("native split: player 1 does not rejoin yet: the leak bound %zu would be passed (leaving players hold "
                 "%zu bytes, its own growth %zu, the next cycle about %zu)", g_leaks.limit, g_leaks.outstanding,
                 g_leaks.dormant, g_p1_ret.NextCycle());
        }
        return false;
    }
    g_watch[0] = {};
    if (size_t n = g_cmds.Drop(0)) g_cmd_dropped += n;
    size_t stale = g_ch.ClientPending(0);
    g_ch.Forget(0);
    g_p1_stale_packets += stale;
    g_p1_conn_serial = g_blocks.Serial();
    g_players.Rejoin0();
    g_p1_rejoins++;
    LOGI("native split: player 1 rejoins in its own context 0 (kept, not rebuilt): its client connects again to the "
         "built-in server in its turn; %zu stale packet(s) dropped; the shared front end stays with player %d, the "
         "session's host player %d", stale, g_ui.driver + 1, g_session.host + 1);
    return true;
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
    g_departing.Clear(0);
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
    if (NativeSplitOn()) FinishViews();
    if (!NativeSplitOn() || !csx::ClientContextHasInitial() || g_reference.empty()) return;
    SettleLinks();
    // the clients that left since the last frame (a kick, the server's disconnect, their own disconnect, a connect that
    // failed): removed here, from player 1's context, outside the turns (docs 15.12). Player 1's departs only while
    // another plays (alone, it is the title's own: its menus)
    for (int k = 0; k < nsplit::kMaxPlayers; k++) {
        nsplit::State s = g_players.At(k).state;
        if (s != nsplit::State::Connecting && s != nsplit::State::Active) continue;
        bool pending = g_departing.Pending(k);
        bool dropped = g_watch[k].Dropped(PlayerDword(k, kClsState));
        if (!nsplit::RemoveAtFrameEnd(pending, dropped)) continue;
        if (k == 0) {
            if (nsplit::Player1Exit(g_players) == nsplit::P1Exit::Depart) {
                DepartPlayer1(ctx);
            } else {
                // nobody else plays any more: player 1 the title's own again (never held alone; a rejoin still
                // connecting too)
                bool was_rejoining = s == nsplit::State::Connecting;
                g_departing.Clear(0);
                g_players.Return0();
                g_watch[0] = {};
                if (pending || was_rejoining)
                    LOGI("native split: player 1's %s dropped: nobody else plays (the title's own player)",
                         was_rejoining ? "rejoin" : "departure");
            }
            continue;
        }
        (pending ? g_pending_removals : g_watch_removals)++;
        LOGI("native split: player %d's client left the match (cls.state %u): removed (%s)", k + 1,
             PlayerDword(k, kClsState), pending ? nsplit::Name(g_departing.Why(k)) : "seen by the watch");
        RemovePlayer(ctx, k);
    }
    if (g_end_session) EndSession(ctx);
    // player 1 departed: it rejoins after CSX_NATIVE_SPLIT_REJOIN_AFTER seconds (diagnostics; docs 15.19)
    if (double s = RejoinAfter(); s > 0 && g_players.At(0).state == nsplit::State::Departed &&
                                  std::chrono::steady_clock::now() - g_p1_departed_at > std::chrono::duration<double>(s))
        RejoinPlayer1();
    // the players connecting (player 1 rejoining too): joined when their client is active (player 1's context is the
    // live one here; another's from its saved copy)
    for (int k = 0; k < nsplit::kMaxPlayers; k++) {
        const nsplit::Player& p = g_players.At(k);
        if (p.state != nsplit::State::Connecting) continue;
        if (int(PlayerDword(k, kClsState)) == kClsActive) {
            g_players.Joined(k);
            g_joined_at = std::chrono::steady_clock::now();
            if (k == 0) {
                g_p1_rejoined++;
                ReportPlayer1Ui("in the match again");
            }
            uint32_t cl = ServerClientOf(k);
            LOGI("native split: player %d is in the match: server slot %d, userid %u, its cl.playernum %u (player 1's "
                 "%u)", k + 1, SlotOf(cl), cl ? R32(cl + 0x4BA4) : 0, PlayerDword(k, kClPlayernum), R32(kClPlayernum));
        }
    }
    // player 1 in its match with the built-in server: the others added, one a frame; the bound on what leaving players
    // still hold (docs 15.20), read from the heap's count now
    g_leaks.Observe(g_blocks.LeftBehindBytes());
    CheckLeakAccounting();
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
            now - g_joined_at > std::chrono::duration<double>(s) && nsplit::DiagLeaveDue(g_diag_leaves, LeaveTimes())) {
            g_diag_leaves++;
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
        LOGE("native split: %s moves block 0x%08X of %s (realloc to 0x%08X, return 0x%08X) [own:realloc-foreign]",
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
    // a process-wide vector's grow run as the process (docs 15.17): its old buffer a player's (made before, or left
    // behind and not yet the process's) is not freed by the process
    if (g_vector_scope && b && owner >= 0) {
        g_vector_free_skipped++;
        g_foreign_frees++;
        g_skipped_frees++;
        LOGE("native split: a process-wide vector's grow (as the process) would free block 0x%08X of player %d (skipped: "
             "a player's buffer under the process's vector; return 0x%08X; %u bytes from site 0x%X) [own:vector-free]",
             va, owner + 1, R32(ctx.esp), b->size, b->site);
        return true;
    }
    if ((g_ui_scope || g_voice_scope) && b && b->kind == heapown::Kind::Owned && heapown::SkipFreeInSharedScope(owner)) {
        g_ui_scope_kept++;
        g_foreign_frees++;
        g_skipped_frees++;
        LOGE("native split: the shared front end would free block 0x%08X of player %d (skipped: a player's object, "
             "not the shared UI's; return 0x%08X) [own:ui-free]", va, owner + 1, R32(ctx.esp));
        return true;
    }
    if (b && heapown::FreeIsForeign(*b, freeing)) {
        g_foreign_frees++;
        bool skip = heapown::SkipFree(owner, freeing);
        g_skipped_frees += skip;
        char whose[64];
        if (owner >= 0)
            std::snprintf(whose, sizeof whose, "player %d", owner + 1);
        else if (b->left_by >= 0)
            std::snprintf(whose, sizeof whose, "the process (left behind by player %d)", b->left_by + 1);
        else
            std::snprintf(whose, sizeof whose, "the process's initialisation");
        std::string who = freeing >= 0 ? "player " + std::to_string(freeing + 1) : "the process";
        if (skip)
            LOGE("native split: %s would free (skipped: a leak, not a block freed under its owner) block 0x%08X of %s "
                 "(return 0x%08X; the block's %u bytes from site 0x%X, serial %llu) [own:free-blocked]",
                 who.c_str(), va, whose, R32(ctx.esp), b->size, b->site, (unsigned long long)b->serial);
        else
            LOGE("native split: %s frees block 0x%08X of %s (return 0x%08X; the block's %u bytes from site 0x%X, serial "
                 "%llu) [own:free-foreign]",
                 who.c_str(), va, whose, R32(ctx.esp), b->size, b->site, (unsigned long long)b->serial);
        // where (docs 15.16): the registers (a vector's grow 0xB52B0 has its header in ebx) and the return addresses
        // on the guest stack, for the first few
        if (g_foreign_frees <= 8) kernel::LogGuestStack(ctx, 48);
        if (skip) return true;
    }
    // a known block's start freed a second time (not allocated again since): never freed again (docs 15.16)
    if (!b && g_blocks.WasFreed(va)) {
        g_double_frees++;
        LOGE("native split: %s frees block 0x%08X a second time (return 0x%08X): skipped [own:free-double]",
             freeing >= 0 ? ("player " + std::to_string(freeing + 1)).c_str() : "the process", va, R32(ctx.esp));
        if (g_double_frees <= 8) kernel::LogGuestStack(ctx, 48);
        return true;
    }
    // a free of a place inside a known block, not its start (docs 15.18: what the particle pool's destructor would do):
    // reported, the title's own free left as it is
    if (!b) {
        if (uint32_t in = g_blocks.Holding(va)) {
            const heapown::Block* h = g_blocks.At(in);
            g_interior_frees++;
            LOG_FIRST(16, "native split: %s frees 0x%08X: inside block 0x%08X (+0x%X of its %u bytes, %s's, site 0x%X), "
                          "not its start (return 0x%08X; not skipped) [watch:free-interior]",
                      OwnerName(freeing).c_str(), va, in, va - in, h->size, OwnerName(h->owner).c_str(), h->site,
                      R32(ctx.esp));
        }
    }
    if (g_releasing && b && g_init_owner >= 0 && owner == g_init_owner) g_released_blocks++, g_released_bytes += b->size;
    g_blocks.Freed(va);
    return false;
}

// ---- the VGUI hierarchy's shared vectors and parents (docs 15.17) -------------------------------------------------

// whether a guest address is in the players' regions (each context's own copy)
bool InPlayerRegions(uint32_t va) { return csx::ClientContextOffsetOf(va) >= 0; }

// [[hook]] at 0xB52B0, the vector grow (ebx the header {count, capacity, data}, eax the count wanted; no stack
// argument, a plain ret; ebx, esi, edi, ebp kept, eax, ecx, edx undefined after a grow: its free's), on_true "ret": a
// grow from a player's turn or initialisation of a vector whose header is the process's (in a process block, or a
// static outside the players' regions: native_split_lifecycle.h VectorGrowOwner) is run here as the process, so its
// new buffer is the process's and its old one (the process's) is freed by its owner. Otherwise (no grow, the process
// already, a player's vector) the title's own code runs on. CallGuest puts every register back (as a grow leaves
// eax, ecx, edx undefined, and the others kept)
bool NativeSplitVectorGrow(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || g_vector_scope) return false;
    uint32_t hdr = ctx.ebx;
    int32_t wanted = int32_t(ctx.eax), cap = Readable(hdr + 4) ? int32_t(R32(hdr + 4)) : 0;
    if (wanted <= cap) return false;
    int acting = OwnerNow();
    if (acting < 0) return false;
    uint32_t start = hdr >= 0x10000 ? g_blocks.Holding(hdr) : 0;
    const heapown::Block* hb = start ? g_blocks.At(start) : nullptr;
    if (nsplit::VectorGrowOwner(wanted, cap, hb != nullptr, hb ? hb->owner : heapown::Blocks::kNone,
                                heapown::IsImageAddress(hdr), InPlayerRegions(hdr), acting,
                                hb && hb->left_by >= 0) != nsplit::GrowAs::Process)
        return false;
    g_vector_grows++;
    uint32_t old = ReadOr0(hdr + 8);
    bool panel = hb && hdr == start + nsplit::kPanelChildren && IsPanel(start);
    if (panel) g_shared_parents.insert(start);
    LOG_FIRST(16, "native split: %s's turn grows %s 0x%08X's vector (header 0x%08X: %d of %d entries, to %d; buffer "
                  "0x%08X of %s) as the process (return 0x%08X) [watch:shared-vector]",
              OwnerName(acting).c_str(), panel ? "the process's panel" : hb ? "the process's block" : "the static",
              hb ? start : hdr, hdr, int32_t(ReadOr0(hdr)), cap, wanted, old, OwnerName(OwnerOfHolding(old)).c_str(),
              R32(ctx.esp));
    {
        Depth scope(g_vector_scope);  // kept right through a longjmp's unwinding
        kernel::CallGuest(ctx, nsplit::kVectorGrow, {});
    }
    return true;
}

// [[hook]] at 0x10ACC0, a panel's setParent (thiscall: ecx the panel, [esp+4] the new parent; slot 0x40): counted
// (docs 15.18), nothing changed
void NativeSplitPanelSetParent(X86Context& ctx, uint8_t*) {
    if (NativeSplitOn())
        PanelLink(ctx.ecx, ReadOr0(ctx.ecx + nsplit::kPanelParent), R32(ctx.esp + 4), "sets the parent of", R32(ctx.esp));
}

// [[hook]] at 0x10AD00, a panel's addChild (thiscall: ecx the parent, [esp+4] the child; slot 0x44): counted
void NativeSplitPanelAddChild(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    uint32_t child = R32(ctx.esp + 4);
    PanelLink(child, ReadOr0(child + nsplit::kPanelParent), ctx.ecx, "adds", R32(ctx.esp));
}

// [[hook]] at 0x10ADC0, a panel's removeChild (thiscall: ecx the parent, [esp+4] the child, [esp] the return; slot
// 0x4C): a removal from a process panel, unless it is setParent's / addChild's / insertChildAt's own first step
// (nsplit::PartOfLink: counted there). Pending until the frame's end (docs 15.20: a re-add of the same child makes it a
// temporary cycle, such as 0x108BB0's move to the front); the cleanup's own detach permanent at once
void NativeSplitPanelRemoveChild(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    uint32_t parent = ctx.ecx, child = R32(ctx.esp + 4);
    if (nsplit::PartOfLink(R32(ctx.esp)) || !ProcessPanel(parent) || !ListedIn(parent, child)) return;
    g_shared_parents.insert(parent);
    if (g_cleanup_detaching) {
        g_links[int(nsplit::Link::Detach)]++;
        g_cleanup_detaches++;
        return;
    }
    g_track.Removed(parent, child, R32(ctx.esp));
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
    LOGE("native split: player %d would destroy %s 0x%08X of %s: not destroyed, the reference dropped [own:destroy-foreign]",
         acting + 1, what,
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
    uint32_t slot = nsplit::DeltaSlot(ctx.esi, ctx.esi ? R32(ctx.esi) : 0, k, t.va, t.elem, g_p1_rejoin_session);
    if (slot == ctx.esi) return;
    ctx.esi = slot;
    if (k == 0)  // player 1 rejoined (docs 15.19): its own count of lines
        LOG_FIRST(4, "native split: player 1's delta description goes to its own slot 0x%08X, the shared registry kept "
                     "(it rejoined the running server)", slot);
    else
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
        g_p1_rejoin_session = false;  // a new server registers its descriptions with player 1's connect again
        g_end_session = false;
        if (g_players.Return0()) LOGI("native split: player 1 is the title's own player again (the session ends)");
        g_departing.Clear(0);  // nobody held once the server is gone (player 1 the title's own)
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

// ---- M3: every drawing player's own view (docs 16; native_split_view.h) ---------------------------------------------

namespace {
// on unless CSX_NATIVE_SPLIT_RENDER=0 (M2's: player 1's view only, the window's)
bool RenderViews() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_RENDER");
        return !(v && v[0] == '0');
    }();
    return on;
}
// two views side by side instead of top and bottom (pc_settings "split_layout", the multi-process split's setting)
bool VerticalViews() {
    static const bool v = pc::Get("split_layout", "horizontal") == "vertical";
    return v;
}

std::array<int, nsplit::kMaxPlayers> ContextsOfPlayers() {
    std::array<int, nsplit::kMaxPlayers> c{};
    for (int k = 0; k < nsplit::kMaxPlayers; k++)
        c[size_t(k)] = g_players.At(k).state == nsplit::State::Empty ? -1 : g_players.At(k).context;
    return c;
}
nsplit::View ViewNow() { return nsplit::ViewOf(g_players, ContextsOfPlayers(), csx::ClientContextActive(), VerticalViews()); }

// this frame's view (set at each client turn's start), the render diagnostics
nsplit::View g_view_now;
int g_view_player = -1;
struct RenderStats {
    uint64_t frames = 0, views = 0, hidden = 0, finished = 0, skipped_draws = 0, shared_skipped = 0, panels_skipped = 0;
    uint64_t by_views[nsplit::kMaxPlayers + 1] = {};
    double turn_ms[nsplit::kMaxPlayers] = {};
    uint64_t turns[nsplit::kMaxPlayers] = {};
    std::chrono::steady_clock::time_point turn_start{};
    int last_views = -1;
} g_render;

}  // namespace

// client_context.cpp, at each player's client half (0x5D3F0): its view of the window frame. A player that draws: its
// viewport, first / last of the frame (d3d::SetSplitView: its Swap is placed there, the last one presents); one that
// does not (departed, initialising): a view placed nowhere, so nothing its turn presents reaches the window
void NativeSplitClientTurn(X86Context&) {
    if (!NativeSplitOn() || !RenderViews()) return;
    nsplit::View v = ViewNow();
    g_view_now = v;
    g_view_player = PlayerOfContext(csx::ClientContextActive());
    gpu::SplitView sv;
    sv.on = true;
    sv.player = g_view_player < 0 ? 0 : g_view_player;
    sv.views = v.views;
    nsplit::Rect conf = nsplit::ConfiguredView(Wanted(), VerticalViews());
    sv.frame_w = conf.w;
    sv.frame_h = conf.h;
    if (v.draws) {
        sv.first = v.first;
        sv.last = v.last;
        sv.x = v.rect.x;
        sv.y = v.rect.y;
        sv.w = v.rect.w;
        sv.h = v.rect.h;
        g_render.views++;
    } else {
        sv.first = sv.last = false;
        sv.w = sv.h = 0;
        g_render.hidden++;
    }
    d3d::SetSplitView(sv);
    g_render.turn_start = std::chrono::steady_clock::now();
    if (v.views != g_render.last_views) {
        g_render.last_views = v.views;
        LOGI("native split: the window shows %d view(s) (%s)%s", v.views,
             v.views == 2 ? (VerticalViews() ? "side by side" : "top and bottom") : v.views <= 1 ? "the whole window" : "2x2",
             v.views == 0 ? ": nobody draws" : "");
    }
}

// client_context.cpp, after each player's client half (0x5D54A): its turn's time (its view's drawing with the rest of
// its client half)
void NativeSplitClientTurnEnd() {
    if (!NativeSplitOn() || !RenderViews() || g_view_player < 0) return;
    int k = g_view_player;
    g_render.turn_ms[k] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_render.turn_start).count();
    g_render.turns[k]++;
    g_view_player = -1;
}

namespace {
// the frame's end: a window frame whose last view never swapped presented as it stands (d3d::FinishSplitFrame)
void FinishViews() {
    if (!RenderViews()) return;
    g_render.frames++;
    if (g_render.last_views >= 0 && g_render.last_views <= nsplit::kMaxPlayers) g_render.by_views[g_render.last_views]++;
    if (d3d::FinishSplitFrame()) {
        g_render.finished++;
        LOG_FIRST(8, "native split: the window frame's last view did not swap: presented as it stood [watch:view-missing]");
    }
}

void ReportRender() {
    if (!RenderViews()) return;
    d3d::SplitCounters c = d3d::SplitStats();
    std::string per;
    for (int k = 0; k < nsplit::kMaxPlayers; k++)
        if (g_render.turns[k]) {
            char b[96];
            std::snprintf(b, sizeof b, "%splayer %d %.3f ms a turn (%llu)", per.empty() ? "" : ", ", k + 1,
                          g_render.turn_ms[k] / double(g_render.turns[k]), (unsigned long long)g_render.turns[k]);
            per += b;
        }
    nsplit::Rect conf = nsplit::ConfiguredView(Wanted(), VerticalViews());
    LOGI("native split: views: %llu frames (%llu of 1 view, %llu of 2, %llu of 3-4, %llu of none); %llu views placed, "
         "%llu hidden turns; swaps: %llu placed, %llu window frames presented at their last view, %llu finished "
         "without it, %llu repeated in one view, %llu not shown (a backend that does not compose); draws skipped: %llu "
         "SCR_UpdateScreen, %llu shared, %llu other players' panels; the configured view %.3f x %.3f of the window; %s",
         (unsigned long long)g_render.frames, (unsigned long long)g_render.by_views[1],
         (unsigned long long)g_render.by_views[2],
         (unsigned long long)(g_render.by_views[3] + g_render.by_views[4]), (unsigned long long)g_render.by_views[0],
         (unsigned long long)g_render.views, (unsigned long long)g_render.hidden, (unsigned long long)c.views,
         (unsigned long long)c.frames, (unsigned long long)c.finished, (unsigned long long)c.repeats,
         (unsigned long long)c.skipped, (unsigned long long)g_render.skipped_draws,
         (unsigned long long)g_render.shared_skipped, (unsigned long long)g_render.panels_skipped, conf.w, conf.h,
         per.empty() ? "no turns timed" : per.c_str());
}
}  // namespace

// [[hook]] at 0x5D51B in _Host_Frame, before SCR_UpdateScreen's call: false (on_false 0x5D535) skips it. M3: a player
// that draws (native_split_view.h Draws) runs it in its turn; M2 (CSX_NATIVE_SPLIT_RENDER=0): player 1's only
bool NativeSplitScreen(X86Context&, uint8_t*) {
    if (!NativeSplitOn()) return true;
    if (!RenderViews()) return !AddedPlayersTurn();
    bool draws = ViewNow().draws;
    if (!draws) g_render.skipped_draws++;
    return draws;
}

// [[hook]] at 0x55F87 and 0xB4660 (Con_Draw), 0x55F94 (the game menus 0xCD7D0) and 0x55FA7 (the front end's screens
// 0xF4400, ecx 0x5616D0): the shared console and front end drawn once a window frame, in the driver's turn
// (native_split_view.h DrawsShared); on_true skips the call
bool NativeSplitSharedDraw(X86Context&, uint8_t*) {
    if (!NativeSplitOn() || !RenderViews() || !csx::ClientContextInTurn()) return false;
    if (nsplit::DrawsShared(g_view_now.views, TurnPlayer(), g_ui.driver)) return false;
    g_render.shared_skipped++;
    return true;
}
bool NativeSplitDrawsShared() {
    return !NativeSplitOn() || !RenderViews() || !csx::ClientContextInTurn() ||
           nsplit::DrawsShared(g_view_now.views, TurnPlayer(), g_ui.driver);
}

// [[hook]] at 0x10A830, a VGUI panel's paint traverse (thiscall, one bool: ret 4 at 0x10A98D): another player's viewport
// (vtable 0x35FD10) is not painted in this player's turn: its paint (0x1B4E70) draws the 3D view and the HUD of
// whoever's turn runs it (native_split_view.h PaintsPanel)
bool NativeSplitPaint(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn() || !RenderViews()) return false;
    uint32_t panel = ctx.ecx;
    bool viewport = panel && Readable(panel) && R32(panel) == nsplit::kViewportVtable;
    if (nsplit::PaintsPanel(csx::ClientContextInTurn(), viewport, panel, R32(nsplit::kViewport))) return false;
    g_render.panels_skipped++;
    return true;
}

// ---- M4's minimum: an added player's own controller in its turn (docs 16) ----------------------------------------------
// The look routine (0x19E160) and the pad bridge (0x19DFD0) read the title's one active pad, [0x3852F8] (an index into
// the pads the title polls, 0x384F9C + 0xE0 each). In an added player's turns its own port is set there (the window's
// port k: its kth other controller; native_split_view.h PadPort) and the bridge's previous-button mask 0x5439B8 (the
// process's, not in a player's regions) is that player's own, swapped in for the turn and back after it; player 1's
// stay the title's. Without a controller on its port the player reads nothing, as in M2. The look routine's own state
// (0x5439A0.., 0x5437D0..) is already in each player's regions. Off with CSX_NATIVE_SPLIT_INPUT=0.
namespace {
constexpr uint32_t kActivePad = 0x3852F8, kBridgeMask = 0x5439B8;
bool InputRouted() {
    static const bool on = [] {
        const char* v = std::getenv("CSX_NATIVE_SPLIT_INPUT");
        return !(v && v[0] == '0');
    }();
    return on;
}
struct PadTurn {
    bool routed = false;   // this turn is an added player's with its controller connected
    int player = -1;
    uint32_t title_pad = 0, title_mask = 0;  // the process's values, put back after the turn
    uint32_t mask[nsplit::kMaxPlayers] = {};  // each added player's bridge mask between its turns
    uint64_t turns = 0, unrouted = 0;
} g_pad_turn;
}  // namespace

// client_context.cpp, at each input or client turn's start (after the switch): an added player's port and mask in
void NativeSplitPadTurnBegin() {
    g_pad_turn.routed = false;
    if (!NativeSplitOn() || !InputRouted()) return;
    int k = PlayerOfContext(csx::ClientContextActive());
    if (k <= 0 || !nsplit::AddedPlayerReadsPad(true, csx::ClientContextInTurn())) return;
    uint32_t port = nsplit::PadPort(k, R32(kActivePad));
    if (!input::Poll(int(port)).connected) {
        g_pad_turn.unrouted++;
        return;
    }
    g_pad_turn.routed = true;
    g_pad_turn.player = k;
    g_pad_turn.title_pad = R32(kActivePad);
    g_pad_turn.title_mask = R32(kBridgeMask);
    W32(kActivePad, port);
    W32(kBridgeMask, g_pad_turn.mask[k]);
    g_pad_turn.turns++;
    LOG_FIRST(4, "native split: player %d reads its own controller (port %u) in its turns", k + 1, port);
}
// at the turn's end (before the next switch), and when a turn is cut short: the process's values back
void NativeSplitPadTurnEnd() {
    if (!g_pad_turn.routed) return;
    g_pad_turn.mask[g_pad_turn.player] = R32(kBridgeMask);
    W32(kActivePad, g_pad_turn.title_pad);
    W32(kBridgeMask, g_pad_turn.title_mask);
    g_pad_turn.routed = false;
}

// [[hook]] at 0x19E160, the look routine's entry (the right stick and the mouse's look), and 0x19DFD0, the pad's
// buttons as keys: on_true "ret" skips them in an added player's turn unless its own controller is routed in
bool NativeSplitLook(X86Context&, uint8_t*) {
    return AddedPlayersTurn() && csx::ClientContextInTurn() && !g_pad_turn.routed;
}
bool NativeSplitPadKeys(X86Context&, uint8_t*) {
    return AddedPlayersTurn() && csx::ClientContextInTurn() && !g_pad_turn.routed;
}

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

// a player's departure confirmed in its own turn (docs 15.15, native_split_core.h Departures): recorded once; the rest
// of its console lines' run (the lines after its "disconnect", in the title's buffer now) and its queue dropped
static void MarkDeparture(int k) {
    if (!g_departing.Mark(k, g_players, g_disconnect_cause)) return;
    size_t dropped = g_cmds.Drop(k);
    if (g_cmd_running == k) {  // Cbuf_Execute re-reads cmd_text's size (0x3BC84) after each line: the run ends here
        dropped += std::min(R32(kCmdSize), R32(kCmdMax));
        W32(kCmdSize, 0);
    }
    g_held_lines += dropped;
    LOGI("native split: player %d's departure recorded (%s, cls.state %u): its client does not connect again, %zu bytes "
         "of its console lines dropped; it is removed at the frame's end", k + 1, nsplit::Name(g_departing.Why(k)),
         R32(kClsState), dropped);
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
    // its connection to the shared server has ended (cls.state 1 was stored at 0x2901F): its departure, confirmed
    MarkDeparture(TurnPlayer());
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
    if (r.origin == nsplit::Origin::Player && nsplit::DropsLines(g_departing, g_players, r.player)) {
        // a held player's line (its own CL_Disconnect's, or anything after its departure): never run
        g_held_lines += GuestString(text, nsplit::kQueueMax).size();
        return true;
    }
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
    if (nsplit::DropsLines(g_departing, g_players, k)) {  // its departure is pending (or player 1 departed)
        g_held_lines += g_cmds.Drop(k);
        return;
    }
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
    g_disconnect_cause = nsplit::Leave::Kicked;  // its CL_Disconnect's departure, recorded there, is a kick's
    kernel::CallGuest(ctx, 0x28F20, {});
    g_disconnect_cause = nsplit::Leave::Disconnect;
    LOGI("native split: player %d's server disconnected it (svc_disconnect: a kick or a drop): its client disconnects "
         "alone (cls.state %u), the others play on", k + 1, R32(kClsState));
    return true;
}

// ---- a departing player's client held until its removal (docs 15.15) ---------------------------------------------

// [[hook]] at 0x298F0, CL_CheckForResend's entry (no arguments, nothing returned; its only call at 0x5D487 in the
// client half, in each player's client turn): on_true "ret" for a held player's client - its state-1 branch
// (0x29904..0x2992C) would connect it to "localhost" again in place while the built-in server is active, its state-2
// branch would resend a connect. Every other client (solo, a player connecting or retrying, a new player's first
// connect, anything outside the turns) runs it as the title
bool NativeSplitCheckResend(X86Context&, uint8_t*) {
    int k = TurnPlayer();
    if (!nsplit::HoldsReconnect(NativeSplitOn(), csx::ClientContextInTurn(), k, g_departing, g_players)) return false;
    g_resends_held++;
    LOG_FIRST(4, "native split: player %d's CL_CheckForResend skipped (cls.state %u): its departure is pending, it does "
                 "not connect again", k + 1, R32(kClsState));
    return true;
}

// [[hook]] at 0x29310, the connect packet's entry (no arguments; called by CL_CheckForResend at 0x2996B / 0x299D6, the
// multicast reply's 0x27F1D, and 0xB70D0's tail jump at 0xB7130, reached at 0x5D4BC in the client half): on_true "ret"
// for a held player's client, as NativeSplitCheckResend
bool NativeSplitConnectPacket(X86Context&, uint8_t*) {
    int k = TurnPlayer();
    if (!nsplit::HoldsReconnect(NativeSplitOn(), csx::ClientContextInTurn(), k, g_departing, g_players)) return false;
    g_connects_held++;
    LOG_FIRST(4, "native split: player %d's connect packet not sent (cls.state %u): its departure is pending", k + 1,
              R32(kClsState));
    return true;
}
