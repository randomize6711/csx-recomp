// 65 player slots (G8.2B, docs/player_slots_plan.md, FINDINGS.md "Player slots"): the hooks that complete what the
// operand patches (config/player_slots_patches.toml, tools/slot_patches.py) cannot express on their own. The patches
// move the player tables to guest tables and raise the limits; these hooks cover the instructions whose address
// arithmetic shares a register with data that did not move, and the clears of tables that moved out of a structure
// the original clears as a whole.
#include <cstdint>
#include <cstring>

#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/player_slots.h"
#include "kernel/kernel.h"

using kernel::R32;
using kernel::W32;

namespace {

// --- the engine server ---

// svs.clients (0x636CE4): N client_t of 0x5010 bytes, then the voice listen bits (3 dwords per sending client: the
// block is sized for them by the patches at 0xA1A0B..0xA1A4F); client_t's own 64 bits at +0x5008 stay unused
constexpr uint32_t kSvsClients = 0x636CE4, kClientSize = 0x5010;
constexpr uint32_t kListenDwords = 3;
static_assert(kListenDwords * 32 >= slots::kMaxPlayers, "the listen bits hold every player");

uint32_t ListenRow(uint32_t client) {
    uint32_t clients = R32(kSvsClients), k = (client - clients) / kClientSize;
    if (client < clients || k >= slots::kMaxPlayers || (client - clients) % kClientSize) {
        LOGE("player slots: %08X is not a client_t of svs.clients %08X", client, clients);
        return 0;
    }
    return clients + slots::kMaxPlayers * kClientSize + k * kListenDwords * 4;
}

}  // namespace

// [[hook]] at 0xABDF8 (config/game.toml), SV_ParseVoiceData's relay loop: whether the speaking client (esi) lets the
// receiver (edi, a client index) hear it, from the moved listen bits; true continues at 0xABE09 (relay), false at
// 0xABE01 (only the speaker's own loopback)
bool SlotsVoiceRelayListening(X86Context& ctx, uint8_t*) {
    uint32_t row = ListenRow(ctx.esi), receiver = ctx.edi;
    if (!row || receiver >= kListenDwords * 32) return false;
    return R32(row + (receiver >> 5) * 4) >> (receiver & 31) & 1;
}

// [[hook]] at 0x79E03 (SV_CreateFakeClient) and 0xA271F (SV_ConnectClient), after each clears its client_t (ebx):
// its listen bits, which no longer live in the client_t, are cleared with it
void SlotsClientCleared(X86Context& ctx, uint8_t*) {
    if (uint32_t row = ListenRow(ctx.ebx))
        for (uint32_t i = 0; i < kListenDwords; i++) W32(row + i * 4, 0);
}

// --- the engine client ---

namespace {

constexpr uint32_t kParseCountMod = 0x74A064, kPlayerNum = 0x771224, kClMaxClients = 0x778A5C;
constexpr uint32_t kStateSize = 0xFB;  // entity_state_t
constexpr uint32_t kPmove = 0x552C34, kNumPhysent = 0x240, kMaxPhysents = 600;

// the player state of frame `frame` for player `player` (0-based) in cl_frame_playerstates, 0 when out of range
uint32_t FrameState(uint32_t frame, uint32_t player, const char* caller) {
    const guest_tables::Table& t = guest_tables::Production(guest_tables::k_cl_frame_playerstates);
    uint32_t f = guest_tables::Element(t, frame, caller);
    if (!f || player >= slots::kMaxPlayers) {
        if (f) LOGE("player slots: %s: player %u past the %u slots", caller, player, slots::kMaxPlayers);
        return 0;
    }
    return f + player * kStateSize;
}

void Clear(size_t k) {
    const guest_tables::Table& t = guest_tables::Production(k);
    if (t.count) std::memset(kernel::HostPtr(t.va), 0, size_t(t.elem) * t.count);
}

}  // namespace

// [[hook]] at 0x23FAE (CL_ParseClientdata, config/game.toml): the local player's state in the current frame, ecx
// (the original computes it from the frame pointer esi, which the frame's other fields still use), for the client's
// prediction data transfer
void SlotsFrameStateEcx(X86Context& ctx, uint8_t*) {
    if (uint32_t s = FrameState(R32(kParseCountMod), R32(kPlayerNum), "CL_ParseClientdata")) ctx.ecx = s;
}

// [[hook]] at 0x32F81 (CL_PredictMove): the local player's state in the current frame, esi, copied into the
// predicted frame (the original adds it to the frame pointer eax, which the copies after it still use)
void SlotsFrameStateEsi(X86Context& ctx, uint8_t*) {
    if (uint32_t s = FrameState(R32(kParseCountMod), R32(kPlayerNum), "CL_PredictMove")) ctx.esi = s;
}

// [[hook]] at 0x29128 and 0x2E358: the frames are about to be cleared; their player states are cleared with them
void SlotsClearFrameStates(X86Context&, uint8_t*) { Clear(guest_tables::k_cl_frame_playerstates); }

// [[hook]] at 0x28E37 (CL_ClearState, before it clears cl): the tables that moved out of cl, cleared with it
void SlotsClearClientState(X86Context&, uint8_t*) {
    Clear(guest_tables::k_cl_frame_playerstates);
    Clear(guest_tables::k_cl_players);
}

// [[hook]] at 0x31A22 (svc_pings' reader, the slot in esi): a slot past the server's player count is refused (the
// original indexes cl.players with it unchecked); false skips the entry's two stores to the next entry's flag bit
// (0x31A3B, which reads eax = 1 bit)
bool SlotsPingSlot(X86Context& ctx, uint8_t*) {
    uint32_t max = R32(kClMaxClients);
    if (ctx.esi < max && ctx.esi < slots::kMaxPlayers) return true;
    ctx.eax = 1;
    static bool logged = false;
    if (!logged) LOGW("player slots: svc_pings names slot %u, the server has %u: ignored", ctx.esi, max);
    logged = true;
    return false;
}

// [[hook]] at 0x26094 (CL_SetSolidPlayers' loop, about to add a player's physent): with the physents full (600, the
// limit the other adders check) the player is left out instead of written past the array; false skips (0x2626C)
bool SlotsPhysentRoom(X86Context&, uint8_t*) {
    uint32_t pm = R32(kPmove);
    if (R32(pm + kNumPhysent) < kMaxPhysents) return true;
    static bool logged = false;
    if (!logged) LOGW("player slots: the prediction's physents are full (%u): a player left out", kMaxPhysents);
    logged = true;
    return false;
}
