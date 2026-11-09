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
//     process) is tracked (heap_owner.h), so a new player's copy can be scrubbed of player 1's heap pointers, and
//     with CSX_CTX_GUARD=1 a free of another player's block is reported;
//   - the loopback: NET_SendPacket (0x6CC70) and NET_GetLoopPacket (0x6BAF0) use the host's channels: the active
//     player's client to the server, the server to the player its address's port names (the port is the player's
//     slot; player 1's is 0, the address it always had); NET_CompareAdr (0x6B3C0) and SV_ReadPackets (0x9C722) compare
//     a loopback address's port, so each local player is its own server client;
//   - a player added (between frames, once player 1 is in its match): a context from the state taken at the end of
//     Host_Init, scrubbed; the client game's Initialize (0x18B7A0) and HUD_Init (0x18B800) run in it, the
//     registrations they repeat answered by the first player's (a cvar: the registered one; the engine's
//     Cmd_AddCommand, fatal after host_initialized: skipped; commands and message hooks are already idempotent); its
//     own userinfo name; then its client connects to the built-in server on its own turn (CL_CheckForResend:
//     disconnected while the server is active);
//   - a player removed: its server slot dropped (SV_DropClient, not CL_Disconnect, which would shut the server down),
//     its client entities freed, its packets forgotten, its context removed;
//   - until M3 / M4: an added player draws nothing (SCR_UpdateScreen skipped in its turn), reads no pad or mouse (the
//     look routine 0x19E160 and the pad keys 0x19DFD0 skipped in its turn) and starts no sound (0x100F10 skipped):
//     the window, the controls and the speakers stay player 1's.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/client_context.h"
#include "game/heap_owner.h"
#include "game/local_loopback.h"
#include "game/native_split_core.h"
#include "game/net_buffers.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"

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

// the allocations that are shared by nature: a pointer to one of these, cleared from the initial state, is given back
// to a new player its initialisation did not replace (heap_owner.h Restorable). Return addresses of their malloc calls.
constexpr uint32_t kSharedSites[] = {
    0x1BF2A,  // the client's pfnRegisterVariable (0x1BEF0): a registered cvar_t
    0x2C716,  // HookUserMsg (0x2C690): the client's user message list entry
    0x3C92A,  // the client's command registration (0x3C8C0): the command list entry
    0x1CB0D,  // ClientDLL_HudVidInit (0x1CAC0): the HUD sprite registry (allocated once, never freed)
};
bool SharedSite(uint32_t site) {
    for (uint32_t x : kSharedSites)
        if (x == site) return true;
    return false;
}

// a player's own objects, which another player must never share: their pointers in the per-player regions (the HUD's
// element list at gHUD + 0, the particle manager, the client game's objects of docs 10.5)
struct Root {
    uint32_t va;
    const char* what;
};
constexpr Root kOwnRoots[] = {
    {0x5623D0, "HUD element list (gHUD + 0)"},
    {0x552B8C, "particle manager"},
    {0x552C70, "client object 0x552C70 (vtable 0x35EB78)"},
    {0x552CA8, "client object 0x552CA8"},
    {0x552BB4, "client object 0x552BB4"},
    {0x552C9C, "client object 0x552C9C"},
};

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

loopnet::Channels g_ch;
heapown::Blocks g_blocks;
nsplit::Players g_players;
bool g_tracking = false;   // heap ownership tracked (native split-screen on, from the process's start)
int g_init_owner = -1;     // the player whose initialisation is running (its allocations are its own), -1 none
uint64_t g_foreign_frees = 0, g_skipped_frees = 0;
std::vector<heapown::Cleared> g_cleared;  // the heap pointers cleared from the initial state when it was taken
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

void AddPlayer(X86Context& ctx, int k) {
    std::string name = "Player " + std::to_string(k + 1);
    int context = csx::ClientContextAdd();
    if (context <= 0) {
        LOGE("native split: no context for player %d (the region list failed its checks)", k + 1);
        return;
    }
    if (!g_players.Begin(k, context, name)) {
        LOGE("native split: player %d cannot start (slot %s)", k + 1, nsplit::Name(g_players.At(k).state));
        csx::ClientContextRemove(context);
        return;
    }
    // its copy is the initial state, scrubbed of player 1's heap pointers when it was taken (NativeSplitInitialTaken)
    const std::vector<heapown::Cleared>& cleared = g_cleared;
    auto t0 = std::chrono::steady_clock::now();

    csx::ClientContextSwitch(context);
    g_init_owner = k;
    kernel::CallGuest(ctx, kClientInitialize, {kEngineFunctions, 7});
    kernel::CallGuest(ctx, kHudInit, {});
    {
        GuestStrings gs(ctx);
        uint32_t key = gs.Put("name"), value = gs.Put(name);
        kernel::CallGuest(ctx, kInfoSetValue, {kClsUserinfo, key, value, 0x100});
    }
    g_init_owner = -1;
    // the cleared pointers the initialisation did not replace name shared objects: their values back, when the very
    // allocation they named is still alive (not freed, not reused since the snapshot)
    size_t restored = 0, replaced = 0, gone = 0, left = 0;
    for (const heapown::Cleared& c : cleared) {
        uint32_t va = csx::ClientContextAddressOf(c.offset);
        if (!va) continue;
        uint32_t now = R32(va);
        if (now) {
            replaced++;
        } else if (heapown::Restorable(c, now, g_blocks, SharedSite)) {
            W32(va, c.value);
            restored++;
        } else if (heapown::StillAlive(c, g_blocks)) {
            // alive but not of a shared kind: player 1's object; the new player has none there (yet)
            left++;
            LOG_FIRST(64, "native split: player %d: 0x%08X left empty (player 1's block 0x%08X, %u bytes, allocated at "
                      "0x%08X: not a shared kind)", k + 1, va, c.block, c.size, c.site);
        } else {
            gone++;
        }
    }
    // the player's own objects: never player 1's
    std::vector<uint8_t>& p1 = csx::ClientContextSaved(0);
    int shared_roots = 0;
    for (const Root& r : kOwnRoots) {
        long off = csx::ClientContextOffsetOf(r.va);
        uint32_t mine = R32(r.va), theirs = 0;
        if (off >= 0 && size_t(off) + 4 <= p1.size()) std::memcpy(&theirs, p1.data() + off, 4);
        if (mine && mine == theirs) {
            shared_roots++;
            LOGE("native split: player %d shares player 1's %s (0x%08X)", k + 1, r.what, mine);
        } else {
            LOGI("native split: player %d's %s: 0x%08X (player 1's 0x%08X)%s", k + 1, r.what, mine, theirs,
                 mine ? "" : ", none yet");
        }
    }
    uint32_t state = R32(kClsState);
    if (state != kClsDisconnected) {
        LOGW("native split: player %d's client state is %u after its initialisation, set to disconnected", k + 1, state);
        W32(kClsState, kClsDisconnected);
    }
    csx::ClientContextSwitch(0);
    g_players.Initialised(k);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOGI("native split: player %d (\"%s\") added in context %d: of the %zu heap pointers cleared from the initial state, "
         "%zu replaced by its own initialisation, %zu given back (shared kinds, alive), %zu left empty (player 1's, "
         "not shared kinds), %zu left empty (freed since); %d of its own objects shared with player 1; initialised in "
         "%.2f ms; %llu frees of others' blocks skipped so far; it joins on its turn",
         k + 1, name.c_str(), context, cleared.size(), replaced, restored, left, gone, shared_roots, ms,
         (unsigned long long)g_skipped_frees);
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
    // its objects: the client entities it allocated at its connect (the rest leak: their destructors, M4/M8)
    std::vector<uint8_t>& copy = csx::ClientContextSaved(p.context);
    long off = csx::ClientContextOffsetOf(kClientEntities);
    size_t freed = 0;
    if (off >= 0 && size_t(off) + 4 <= copy.size()) {
        uint32_t ents;
        std::memcpy(&ents, copy.data() + off, 4);
        if (ents && g_blocks.At(ents) && g_blocks.At(ents)->owner == k) {
            g_init_owner = k;  // its own free
            kernel::CallGuest(ctx, kFree, {ents});
            g_init_owner = -1;
            freed++;
        }
    }
    // what it still owns: not freed (their destructors are not run), handed to the process
    auto [leaked, leaked_bytes] = g_blocks.Orphan(k);
    csx::ClientContextRemove(p.context);
    g_players.ContextRemoved(p.context);
    g_players.Released(k);
    LOGI("native split: player %d left: its server slot dropped, %zu object freed (its client entities), %zu kept "
         "(%zu bytes), its context removed",
         k + 1, freed, leaked, leaked_bytes);
}

}  // namespace

bool NativeSplitOn() { return Wanted() >= 2; }

// client_context.cpp, at the end of Host_Init when the initial state was taken: what the heap holds now is the
// process's (player 1's state refers to it; another player must not free it)
void NativeSplitInitialTaken() {
    g_blocks.OwnAll(heapown::kProcess);
    // the heap pointers out of the initial state now, while they are current (heap_owner.h Scrub)
    g_cleared = heapown::Scrub(csx::ClientContextInitial(), g_blocks);
    LOGI("native split: %d local players asked; %zu heap blocks from the process's initialisation; %zu heap pointers "
         "cleared from the initial state",
         Wanted(), g_blocks.Count(), g_cleared.size());
}

// client_context.cpp, after the last player's client turn: players added or removed between frames
void NativeSplitFrameEnd(X86Context& ctx) {
    if (!NativeSplitOn() || !csx::ClientContextHasInitial()) return;
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
    if (R32(kSvActive) && int(R32(kClsState)) == kClsActive && g_players.Count() < Wanted() &&
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
    g_blocks.Allocated(ctx.eax, R32(ctx.esp + 4), OwnerNow(), up == 0x22D879 ? R32(ctx.esp + 0x14) : up);
}

// [[hook]] at 0x235F2D in calloc (0x235EFA), after its HeapAlloc: the block in eax, its size in esi
void NativeSplitHeapCalloc(X86Context& ctx, uint8_t*) {
    if (!NativeSplitOn()) return;
    g_blocks.Allocated(ctx.eax, ctx.esi, OwnerNow(), R32(ctx.esp + 4));  // calloc's caller (only esi pushed above it)
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
    if (heapown::FreeIsForeign(owner, freeing)) {
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

// [[hook]] at 0x5C9D0, Host_ShutdownServer's entry: the host's loopback channels emptied with the title's queues
void NativeSplitShutdownServer(X86Context&, uint8_t*) {
    if (NativeSplitOn()) g_ch.Clear();
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
