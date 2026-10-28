#pragma once
// PC settings the original game has no place for (mouse, display, audio output, bindings), kept apart from the Xbox
// profile in pc_settings.ini in the run folder: "name = value" lines. Edited in the game's own menus (src/game/
// pc_menu.cpp) and read by the host (window, input, audio). Thread safe.
#include <string>

namespace pc {

std::string Get(const std::string& name, const std::string& fallback);
float GetFloat(const std::string& name, float fallback);
void Set(const std::string& name, const std::string& value);
void Erase(const std::string& name);  // the setting removed (Save writes the file without it)
void Save();         // writes pc_settings.ini
unsigned Version();  // bumped by every Set, for readers that cache

// an optional Xbox graphical effect's setting ("bloom", "auto_exposure"; On by default) as it takes effect: On only
// while PC Options > Xbox Graphical Effects ("xbox_effects", On by default) is On too. The effect's own saved value
// is never changed by the master; Off of the master turns the effect off, On gives back what its setting holds.
bool XboxEffect(const std::string& name);
bool XboxEffectsMaster();

// The profile this process plays as (N4.8, src/common/profiles.h): its id from CSX_PC_PROFILE when the settings are
// first read, or UseProfile. While one is in use, the profile-scoped settings (profiles::Scoped: mouse, FOV,
// bindings, the title's controller options) are read from it (the PC's value when it has none) and Set / Save write
// them to it; the other settings stay the PC's. UseProfile("") plays as no profile. The id is kept in the environment
// (CSX_PC_PROFILE) for the title's relaunches.
bool UseProfile(const std::string& id);
std::string ActiveProfile();
// the PC's own value of a setting, past the profile (a new profile starts from these)
std::string GetGlobal(const std::string& name, const std::string& fallback);
// the folder holding profiles/: CSX_PROFILE_DIR (the compositor's run folder, for every local player), else this
// process's own
std::string ProfileFolder();

}  // namespace pc
