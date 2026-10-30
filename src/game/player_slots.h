#pragma once
// 65 player slots (G8.2B): the engine's player count, as config/player_slots.toml [slots] max_players gives it to the
// operand patches (tools/slot_patches.py checks that the two agree). An experimental build with more slots (N11,
// tools/slots_variant.sh: 96 or 128) defines CSX_MAX_PLAYERS and uses its own generated patches, its own protocol
// version (a 65-slot build and it refuse each other at connect) and its own output folders.
#include <cstdint>

#ifndef CSX_MAX_PLAYERS
#define CSX_MAX_PLAYERS 65
#endif

namespace slots {

constexpr uint32_t kMaxPlayers = CSX_MAX_PLAYERS;

}  // namespace slots
