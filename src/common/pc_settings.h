#pragma once
// PC settings the original game has no place for (mouse, display, audio output, bindings), kept apart from the Xbox
// profile in pc_settings.ini in the run folder: "name = value" lines. Edited in the game's own menus (src/game/
// pc_menu.cpp) and read by the host (window, input, audio). Thread safe.
#include <string>

namespace pc {

std::string Get(const std::string& name, const std::string& fallback);
float GetFloat(const std::string& name, float fallback);
void Set(const std::string& name, const std::string& value);
void Save();         // writes pc_settings.ini
unsigned Version();  // bumped by every Set, for readers that cache

// an optional Xbox graphical effect's setting ("bloom", "auto_exposure"; On by default) as it takes effect: On only
// while PC Options > Xbox Graphical Effects ("xbox_effects", On by default) is On too. The effect's own saved value
// is never changed by the master; Off of the master turns the effect off, On gives back what its setting holds.
bool XboxEffect(const std::string& name);
bool XboxEffectsMaster();

}  // namespace pc
