// tests/pc_modes_rules_test.py builds and runs this: src/game/pc_modes_rules.h on its own
#include <cstdio>

#include "game/pc_modes_rules.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    using namespace pcmodes;
    // Gun Game: the level's weapon only
    Check(LevelKill("weapon_ak47", "ak47", true), "AK-47 level, AK-47 kill counts");
    Check(!LevelKill("weapon_ak47", "m4a1", true), "AK-47 level, picked-up M4A1 kill does not count");
    Check(!LevelKill("weapon_knife", "deagle", true), "knife level, a gun kill never wins");
    Check(LevelKill("weapon_knife", "knife", true), "knife level, knife kill wins");
    Check(!LevelKill("weapon_glock18", "grenade", true), "pistol level, grenade kill does not count");
    Check(!LevelKill("weapon_glock18", "glock18", false), "a team kill counts for nothing");
    Check(!LevelKill("weapon_glock18", "", true), "a death by the world counts for nothing");
    Check(LevelKill("glock18", "weapon_glock18", true), "names with or without weapon_");
    // slots
    Check(FindWeapon("ak47")->slot == WeaponInfo::Primary, "AK-47 primary");
    Check(FindWeapon("weapon_deagle")->slot == WeaponInfo::Secondary, "Deagle secondary");
    Check(FindWeapon(17) && FindWeapon(17)->name == std::string_view("glock18"), "id 17 the Glock");
    Check(FindWeapon(29)->slot == WeaponInfo::Knife, "id 29 the knife");
    Check(FindWeapon("hegrenade")->slot == WeaponInfo::Grenade, "HE grenade");
    Check(!FindWeapon("crowbar"), "unknown weapon");
    // unlimited reserve: firearms only
    Check(UnlimitedReserve("awp") && UnlimitedReserve("usp") && UnlimitedReserve("m249"), "firearms unlimited");
    Check(!UnlimitedReserve("hegrenade") && !UnlimitedReserve("flashbang") && !UnlimitedReserve("smokegrenade"),
          "grenades keep their rules");
    Check(!UnlimitedReserve("c4") && !UnlimitedReserve("knife"), "bomb and knife none");
    // every id once
    for (const WeaponInfo& a : kWeaponInfo)
        for (const WeaponInfo& b : kWeaponInfo)
            if (&a != &b && a.id == b.id) Check(false, "a duplicate id");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
