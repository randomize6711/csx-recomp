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

}  // namespace pc
