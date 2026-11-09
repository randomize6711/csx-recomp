// Native split-screen client contexts (docs/native_splitscreen.md, M1; client_context.cpp). One per local player;
// a game has one unless native split-screen adds more.
#pragma once

#include <vector>

#include "game/client_context_core.h"

namespace csx {

std::vector<ctx::Region> ClientContextRegions();  // one player's client state in guest memory
int ClientContextCount();
int ClientContextActive();
int ClientContextAdd();             // a new player's context (a copy of the active one's state); its index
bool ClientContextSwitch(int k);    // player k's state made the live memory

}  // namespace csx
