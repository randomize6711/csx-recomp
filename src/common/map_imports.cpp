// Imported maps (N6B, tools/import_map.py, docs/n6_maps.md) and mods (N8, common/mods.h). Each map is an archive
// (NAME.SXWad) and its report (NAME.json) in a folder of their own: CSX_MAPS (empty: none), else maps/ beside the
// build (build/maps), never the game folder.
// When some are playable (their report says so) or mods are enabled, the game runs from an overlay of the game
// folder: RUN/gamedir, a link to each of the game folder's files and to each playable map's archive and report, made
// afresh at each start, so the game's paths (D:\NAME.sxwad) and the menus' map lists (pc_lobby.cpp, pc_dlc.cpp read
// GAME_DIR) find them; the mods' files are linked under it (mods.cpp). The game folder itself is never written.
#include "common/map_imports.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "common/mods.h"
#include "common/pc_settings.h"
#include "core/log.h"

namespace csx {

namespace {
namespace fs = std::filesystem;
constexpr char kMarker[] = ".csx_overlay";

fs::path MapsFolder() {
    if (const char* d = std::getenv("CSX_MAPS")) return d;  // empty: none (the tests' default, tools/drive.py)
    std::error_code ec;
    fs::path exe = fs::canonical("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path().parent_path() / "maps";
}

bool Playable(const fs::path& report) {
    std::ifstream f(report);
    std::string text((std::istreambuf_iterator<char>(f)), {});
    return text.find("\"playable\": true") != std::string::npos;
}
}  // namespace

std::string GameDirWithImports(const std::string& game, const std::string& run, bool apply_mods) {
    std::error_code ec;
    if (fs::exists(fs::path(game) / kMarker, ec)) {  // already the overlay (a relaunch): the mods applied again (the
                                                     // profile playing may have changed them)
        if (!apply_mods) {  // resolved only: the overlay's links are its maker's
            mods::Apply(game, false);
            return game;
        }
        fs::remove_all(fs::path(game) / "cstrike", ec);
        mods::Apply(game);
        return game;
    }
    bool want_mods = pc::Get("mods_original", "0") != "1" && !mods::Enabled(mods::Scan()).empty();
    fs::path maps = MapsFolder();
    std::vector<fs::path> playable;
    if (!maps.empty() && fs::is_directory(maps, ec))
        for (const auto& e : fs::directory_iterator(maps, ec)) {
            fs::path p = e.path();
            if (p.extension() != ".json") continue;
            fs::path archive = p;
            archive.replace_extension(".SXWad");
            if (fs::is_regular_file(archive, ec) && Playable(p)) playable.push_back(p);
        }
    if (playable.empty() && !want_mods) return game;
    fs::path overlay = fs::path(run) / "gamedir";
    fs::remove_all(overlay, ec);
    fs::create_directories(overlay, ec);
    int linked = 0;
    for (const auto& e : fs::directory_iterator(game, ec)) {
        fs::create_symlink(fs::absolute(e.path()), overlay / e.path().filename(), ec);
        if (!ec) linked++;
    }
    std::string names;
    for (const fs::path& report : playable) {
        fs::path archive = report;
        archive.replace_extension(".SXWad");
        if (fs::exists(overlay / archive.filename(), ec)) continue;  // a map of the game's own name: the game's wins
        fs::create_symlink(fs::absolute(archive), overlay / archive.filename(), ec);
        fs::create_symlink(fs::absolute(report), overlay / report.filename(), ec);
        names += (names.empty() ? "" : ", ") + report.stem().string();
    }
    std::ofstream(overlay / kMarker) << game << "\n";
    std::string line = "maps: imported " + (names.empty() ? std::string("none") : names) + " (" + maps.string() +
                       "): the game runs from " + overlay.string() + " (" + std::to_string(linked) +
                       " of the game folder's files linked)";
    LOGI("%s", line.c_str());
    mods::Note(line);  // logged again once the game's log is open
    mods::Apply(overlay.string());
    return overlay.string();
}

}  // namespace csx
