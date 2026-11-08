#include "gamefiles.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include "sha256.h"

namespace fs = std::filesystem;

namespace tool {

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a file whose name matches regardless of case (the game's files come in either case from different dumps)
fs::path FindFile(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return dir / name;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (Lower(e.path().filename().string()) == Lower(name)) return e.path();
    return {};
}
}  // namespace

bool ValidMapName(const std::string& name) {
    return !name.empty() && name.size() <= 31 &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}

GameCheck CheckGameFolder(const std::string& dir, bool hash) {
    GameCheck c;
    std::error_code ec;
    if (dir.empty()) {
        c.problems.push_back("No game folder is set. Choose the folder with your Counter-Strike files (default.xbe, "
                             "the .SXWad maps and Media).");
        return c;
    }
    if (!fs::is_directory(dir, ec)) {
        c.problems.push_back("The game folder does not exist: " + dir);
        return c;
    }
    fs::path xbe = FindFile(dir, "default.xbe");
    if (xbe.empty()) {
        c.problems.push_back("default.xbe is missing from " + dir + ".");
    } else if (fs::file_size(xbe, ec) != 3973120) {
        c.problems.push_back("default.xbe has the wrong size (" + std::to_string(fs::file_size(xbe, ec)) +
                             " bytes; the supported version is 3 973 120 bytes, title ID MS-054, PAL).");
    } else if (xbe.filename() != "default.xbe") {
        c.problems.push_back("The server needs the file named exactly default.xbe (found " + xbe.filename().string() + ").");
    } else if (hash) {
        std::string h = Sha256File(xbe.string());
        if (h == kXbeSha256) c.xbe_verified = true;
        else c.problems.push_back("default.xbe is not the supported version (SHA-256 " + h.substr(0, 16) +
                                  "...; needed e7698b09...308f: title ID MS-054, PAL). See the README.");
    }
    if (FindFile(dir, "FrontEnd.SXWad").empty()) c.problems.push_back("FrontEnd.SXWad is missing (the disc's files are incomplete).");
    if (!fs::is_directory(fs::path(dir) / "Media", ec) && FindFile(dir, "Media").empty())
        c.problems.push_back("The Media folder is missing (the disc's files are incomplete).");
    auto maps = ScanMaps(dir, "");
    if (maps.empty()) c.problems.push_back("No maps (cs_*.SXWad, de_*.SXWad) were found in " + dir + ".");
    else c.notes.push_back(std::to_string(maps.size()) + " maps found.");
    if (!hash && !xbe.empty()) c.notes.push_back("default.xbe not hashed yet.");
    c.ok = c.problems.empty();
    return c;
}

std::vector<MapEntry> ScanMaps(const std::string& game_dir, const std::string& imports_dir) {
    std::vector<MapEntry> out;
    std::set<std::string> seen;
    std::error_code ec;
    if (!game_dir.empty())
        for (const auto& e : fs::directory_iterator(game_dir, ec)) {
            std::string file = e.path().filename().string();
            if (Lower(e.path().extension().string()) != ".sxwad") continue;
            std::string stem = e.path().stem().string();
            std::string low = Lower(stem);
            if (low == "frontend" || low.rfind("cachetree", 0) == 0) continue;
            // the server opens NAME.SXWad by its exact name: only names it accepts are offered
            if (!ValidMapName(stem) || e.path().extension() != ".SXWad") continue;
            if (seen.insert(stem).second) out.push_back({stem, false});
        }
    if (!imports_dir.empty())
        for (const auto& e : fs::directory_iterator(imports_dir, ec)) {
            if (e.path().extension() != ".SXWad") continue;
            std::string stem = e.path().stem().string();
            fs::path report = e.path().parent_path() / (stem + ".json");
            std::ifstream f(report);
            std::string text((std::istreambuf_iterator<char>(f)), {});
            if (text.find("\"playable\": true") == std::string::npos) continue;  // as map_imports.cpp decides
            if (ValidMapName(stem) && seen.insert(stem).second) out.push_back({stem, true});
        }
    std::sort(out.begin(), out.end(), [](const MapEntry& a, const MapEntry& b) { return a.name < b.name; });
    return out;
}

}  // namespace tool
