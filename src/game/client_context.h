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
double ClientContextSwitchMs(uint64_t& switches);  // the time spent switching so far (ms) and how many switches
void ClientContextSetFields(std::vector<uint32_t> fields, std::vector<uint32_t> fresh);  // heap fields (docs 18.5)
void ClientContextBroadcast(uint32_t va, const void* data, uint32_t n);  // into every player (docs 18.5)
bool ClientContextRemove(int k);    // context k dropped (not 0, not active); later ones move down an index
bool ClientContextInTurn();         // inside a player's turn of _Host_Frame (its CL_Move, or its client half)
bool ClientContextInServerFrame();  // inside _Host_Frame's SV_Frame call (0x5D3CD..0x5D3D2): the built-in server's
bool ClientContextHasInitial();     // the state a new context starts from was taken (native split-screen only)
std::vector<uint8_t>& ClientContextInitial();     // that state (the boot image: native_split.cpp checks it)
std::vector<uint8_t>& ClientContextSaved(int k);  // context k's saved copy (not the active one's: stale)
uint32_t ClientContextAddressOf(size_t offset);   // a copy's byte's guest address
long ClientContextOffsetOf(uint32_t va);          // a guest address's offset in a copy, -1 outside the regions

}  // namespace csx
