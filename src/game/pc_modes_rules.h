// Deathmatch's and Gun Game's rules that need no game memory (src/game/pc_modes.cpp; tested on their own by
// tests/pc_modes_rules_test.py): the weapons' ids (Counter-Strike's: the bits of entvars weapons, the state probe's
// numbers), which slot a bought weapon fills in a remembered loadout, which weapons have unlimited reserve ammunition,
// and whether a kill counts for a Gun Game level.
#pragma once
#include <cstring>
#include <string_view>

namespace pcmodes {

struct WeaponInfo {
    const char* name;  // without "weapon_"
    int id;
    enum Slot { Primary, Secondary, Knife, Grenade, Bomb } slot;
};

inline constexpr WeaponInfo kWeaponInfo[] = {
    {"p228", 1, WeaponInfo::Secondary},   {"scout", 3, WeaponInfo::Primary},    {"hegrenade", 4, WeaponInfo::Grenade},
    {"xm1014", 5, WeaponInfo::Primary},   {"c4", 6, WeaponInfo::Bomb},          {"mac10", 7, WeaponInfo::Primary},
    {"aug", 8, WeaponInfo::Primary},      {"smokegrenade", 9, WeaponInfo::Grenade}, {"elite", 10, WeaponInfo::Secondary},
    {"fiveseven", 11, WeaponInfo::Secondary}, {"ump45", 12, WeaponInfo::Primary}, {"sg550", 13, WeaponInfo::Primary},
    {"galil", 14, WeaponInfo::Primary},   {"famas", 15, WeaponInfo::Primary},   {"usp", 16, WeaponInfo::Secondary},
    {"glock18", 17, WeaponInfo::Secondary}, {"awp", 18, WeaponInfo::Primary},  {"mp5navy", 19, WeaponInfo::Primary},
    {"m249", 20, WeaponInfo::Primary},    {"m3", 21, WeaponInfo::Primary},      {"m4a1", 22, WeaponInfo::Primary},
    {"tmp", 23, WeaponInfo::Primary},     {"g3sg1", 24, WeaponInfo::Primary},   {"flashbang", 25, WeaponInfo::Grenade},
    {"deagle", 26, WeaponInfo::Secondary}, {"sg552", 27, WeaponInfo::Primary}, {"ak47", 28, WeaponInfo::Primary},
    {"knife", 29, WeaponInfo::Knife},     {"p90", 30, WeaponInfo::Primary},
};

// "ak47" or "weapon_ak47"
inline std::string_view Bare(std::string_view name) {
    return name.substr(0, 7) == "weapon_" ? name.substr(7) : name;
}

inline const WeaponInfo* FindWeapon(std::string_view name) {
    name = Bare(name);
    for (const WeaponInfo& w : kWeaponInfo)
        if (name == w.name) return &w;
    return nullptr;
}
inline const WeaponInfo* FindWeapon(int id) {
    for (const WeaponInfo& w : kWeaponInfo)
        if (id == w.id) return &w;
    return nullptr;
}

// a firearm: its reserve ammunition is unlimited in Deathmatch and Gun Game (grenades, the bomb and the knife keep
// their own rules)
inline bool UnlimitedReserve(std::string_view name) {
    const WeaponInfo* w = FindWeapon(name);
    return w && (w->slot == WeaponInfo::Primary || w->slot == WeaponInfo::Secondary);
}

// a kill for a Gun Game level: by the level's own weapon (the weapon the server names for the kill: the killer's
// weapon in hand at the kill, or the projectile's class for a grenade), of an enemy; a team kill, a suicide or a
// death by the world counts for nothing
inline bool LevelKill(std::string_view level_weapon, std::string_view kill_weapon, bool enemy) {
    return enemy && !kill_weapon.empty() && Bare(level_weapon) == Bare(kill_weapon);
}

}  // namespace pcmodes
