// The user's game files (README "Required game version"): checked before a server starts, and the maps they hold.
#pragma once

#include <string>
#include <vector>

namespace tool {

// the default.xbe the port is built for (config/game.toml sha256)
inline constexpr const char* kXbeSha256 = "e7698b09ed7322b69cf4c9c6d3778bb0864a15594b30258b5637ddcf8ee8308f";

struct GameCheck {
    bool ok = false;           // a server can start from it
    bool xbe_verified = false; // the hash was computed and matches (false while not checked yet)
    std::vector<std::string> problems;  // what is missing or wrong, for the user
    std::vector<std::string> notes;     // fine, but worth knowing
};

// files and folders only (fast); hash: also the default.xbe's SHA-256 (a few hundred ms)
GameCheck CheckGameFolder(const std::string& dir, bool hash);

struct MapEntry {
    std::string name;      // de_dust2
    bool imported = false; // an imported map (maps/ beside the server's folder), not the game folder's
};

// the playable maps: the game folder's NAME.SXWad except the front end and language files, plus imported maps whose
// report says they are playable (imports_dir: maps/ beside the server's bin folder; empty for none). Sorted by name.
std::vector<MapEntry> ScanMaps(const std::string& game_dir, const std::string& imports_dir);

// a map name a server accepts (lower case letters, digits and _, at most 31)
bool ValidMapName(const std::string& name);

}  // namespace tool
