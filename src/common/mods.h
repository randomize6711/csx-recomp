// Mods (N8, docs/n8_mods.md): asset packs in a folder of their own, never in the game's files. Each mod is a folder
// with a manifest (mod.ini) and files under files/ at the game's own paths (files/models/v_ak47.mdl replaces
// models\v_ak47.mdl). Only assets are taken (models, sprites, textures, WADs, menu artwork, maps); nothing that runs
// commands or code. Enabled mods (pc_settings.ini mod.ID = 1, in mod_order.ID order then by id, the last one's file
// wins; per profile) are checked (manifest, this game's version, dependencies and their versions, conflicts) and their
// files linked into the game's overlay folder (common/map_imports.cpp), where the game finds them in place of its archives'
// (game/pc_mods.cpp: the archive answers "not here" for an overridden name and the game reads the loose file).
#pragma once

#include <string>
#include <vector>

namespace csx::mods {

constexpr const char* kGameVersion = "0.8.0";  // this build's mod interface version (mod.ini's requires)

struct Requirement {
    std::string id, op, version;  // op: ">=", "=", "" (any version)
};

struct Mod {
    std::string id, name, version, author, description;
    std::string kind = "cosmetic";  // cosmetic: this PC's looks only; content: a match's players must all have it
    std::string needs;              // the game's mod interface version it needs (kGameVersion or older)
    std::vector<Requirement> depends, conflicts;
    std::string folder;
    std::vector<std::string> files;  // relative paths under files/ ('/' separated, lower case)
    std::vector<std::string> refused;  // files left out (not an asset this loader takes)
    std::vector<std::string> problems;  // the manifest's or the files' (the mod cannot be enabled)
    std::string hash;  // of its manifest and files (content mods' multiplayer check)
};

// the mods folder: CSX_MODS (empty: none), else mods/ beside the build
std::string Folder();
std::vector<Mod> Scan();

// the enabled mods resolved: those that load (in order), and why the others do not
struct Plan {
    std::vector<Mod> active;
    std::vector<std::string> messages;
};
Plan Resolve(const std::vector<Mod>& all, const std::vector<std::string>& enabled);

// "a,b,c" <-> ids
std::vector<std::string> Split(const std::string& list);
std::string Join(const std::vector<std::string>& ids);

// the enabled ids (mod.ID = 1) in load order (mod_order.ID, 1..99, 50 by default; then the id)
std::vector<std::string> Enabled(const std::vector<Mod>& all);

// the enabled mods ("mods_original" 1: none, the original's looks) resolved and their files linked
// into overlay (the game's folder as the game sees it); logged. Returns the active mods. link false: only resolved
// (Overridden, ContentSignature) for a process running from an overlay another one made (the local server)
std::vector<Mod> Apply(const std::string& overlay, bool link = true);

// an archive entry the active mods replace (the guest's name, any case, '\' or '/')
bool Overridden(const std::string& name);

// what the last Apply reported (each mod active or left out and why), taken once (logged by the game once its log
// is open: Apply runs before it)
std::vector<std::string> TakeReport();
void Note(const std::string& line);  // a line for that report (the map imports' overlay, map_imports.cpp)

// the active content mods, "id@version:hash ..." (empty: none), for a server's signon and a client's check
std::string ContentSignature();

// version compare: <0, 0, >0 ("1.2" < "1.10")
int CompareVersions(const std::string& a, const std::string& b);

}  // namespace csx::mods
