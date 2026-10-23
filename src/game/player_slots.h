#pragma once
// 65 player slots (G8.2B): the engine's player count, as config/player_slots.toml [slots] max_players gives it to the
// operand patches (tools/slot_patches.py checks that the two agree)
#include <cstdint>

namespace slots {

constexpr uint32_t kMaxPlayers = 65;

}  // namespace slots
