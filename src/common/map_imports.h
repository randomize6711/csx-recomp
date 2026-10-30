// Imported maps (N6B): the game folder to run from (src/common/map_imports.cpp)
#pragma once

#include <string>

namespace csx {

// game itself when no imported map is playable; else an overlay folder in run (links to the game folder's files and
// the imported maps' archives), made afresh; an overlay given again (a relaunch) is returned as it is
// mods: false leaves an overlay given again as it is (the local server, run from its game's overlay, never changes
// the game's mods); a new overlay always takes this process's enabled mods
std::string GameDirWithImports(const std::string& game, const std::string& run, bool mods = true);

}  // namespace csx
