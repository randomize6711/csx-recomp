#pragma once
// The multiplayer party's members for the other front end screens (N4.8, src/game/pc_party.cpp: the party screen and
// profiles; src/game/pc_lobby.cpp: the destination, map, mode, rules and lobby screens)
#include <string>
#include <vector>

struct X86Context;

struct PcPartyMember {
    bool in = false, pad = false;  // a member; its controller connected
    std::string profile, name;     // its profile's id and name ("" none)
    int team = 0;                  // 0 Auto-Select, 1 Terrorist, 2 Counter-Terrorist
    bool ready = false;
};

std::vector<PcPartyMember> PcPartyMembers();  // four, player 1 first (the compositor's party)
int PcPartyCount();                            // the members
int PcPartyFocus();                            // a member editing its profile has the menus (0: none)
void PcPartySetMember(int n, int team, bool ready);
const char* PcPartyTeamName(int team);
std::string PcPartyPrompt(int n, const char* pad);  // a hint with the member's own device's buttons
std::string PcPartyProfileName();                   // the profile this process plays as ("" none)
