#include "common/profiles.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>

namespace fs = std::filesystem;

namespace profiles {

namespace {
std::mutex g_lock;  // one writer at a time (the files are small; a rename makes each write whole)

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// values are one line each: no line breaks (a binding or a name never has one)
bool ValueValid(const std::string& v) { return v.find_first_of("\r\n") == std::string::npos; }
}  // namespace

bool NameValid(const std::string& name) {
    if (name.empty() || name.size() > 20 || name.find_first_not_of(' ') == std::string::npos) return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && !std::strchr(" -_.!?()[]*+=:'@#&", c)) return false;
    return true;
}

bool IdValid(const std::string& id) {
    return id.size() == 16 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool Scoped(const std::string& key) {
    static const char* const kKeys[] = {"mouse_sensitivity", "mouse_zoom_sensitivity", "mouse_invert", "mouse_raw", "fov", "prompt_style"};
    for (const char* k : kKeys)
        if (key == k) return true;
    return key.rfind("bind.", 0) == 0 || key.rfind("pad.", 0) == 0 || key.rfind("mod.", 0) == 0 ||
           key.rfind("mod_order.", 0) == 0 || key == "mods_original" ||
           key.rfind("hud.", 0) == 0 || key.rfind("hud_split.", 0) == 0 || key == "hud_scale" || key == "hud_scale_split";
}

Store::Store(std::string folder, std::string sub, bool (*keys)(const std::string&))
    : folder_(std::move(folder)), sub_(std::move(sub)), keys_(keys) {}

std::string Store::Path(const std::string& id) const { return (fs::path(folder_) / sub_ / (id + ".ini")).string(); }

bool Store::Read(const std::string& id, std::string& name, std::map<std::string, std::string>& values) const {
    if (!IdValid(id)) return false;
    std::ifstream f(Path(id));
    if (!f) return false;
    name.clear();
    for (std::string line; std::getline(f, line);) {
        line = Trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == "name") name = v;
        else if (keys_(k)) values[k] = v;
    }
    return NameValid(name);
}

bool Store::Write(const std::string& id, const std::string& name, const std::map<std::string, std::string>& values,
                  std::string& error) const {
    std::error_code ec;
    fs::create_directories(fs::path(folder_) / sub_, ec);
    std::string tmp = Path(id) + ".new";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# " << (sub_ == "profiles" ? "a player profile" : "saved settings") << " (Counter-Strike Xbox recompilation): Multiplayer\n";
        f << "name = " << name << "\n";
        for (auto& [k, v] : values)
            if (keys_(k) && ValueValid(v)) f << k << " = " << v << "\n";
        if (!f) {
            error = "the profile could not be written (" + tmp + ")";
            return false;
        }
    }
    fs::rename(tmp, Path(id), ec);
    if (ec) {
        error = "the profile could not be saved (" + ec.message() + ")";
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<Info> Store::List() const {
    std::lock_guard<std::mutex> lock(g_lock);
    std::vector<Info> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(folder_) / sub_, ec)) {
        if (e.path().extension() != ".ini") continue;
        std::string id = e.path().stem().string(), name;
        std::map<std::string, std::string> values;
        if (Read(id, name, values)) out.push_back({id, name});
    }
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    return out;
}

std::string Store::Create(const std::string& name, const std::map<std::string, std::string>& values, std::string& error) {
    if (!NameValid(name)) {
        error = "A character in that name cannot be used.";
        return {};
    }
    std::lock_guard<std::mutex> lock(g_lock);
    std::random_device rd;
    for (int attempt = 0; attempt < 8; attempt++) {
        char id[17];
        std::snprintf(id, sizeof id, "%08x%08x", rd(), rd());
        std::error_code ec;
        if (fs::exists(Path(id), ec)) continue;  // never another profile's file
        return Write(id, name, values, error) ? std::string(id) : std::string();
    }
    error = "no free profile id";
    return {};
}

bool Store::Exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string name;
    std::map<std::string, std::string> v;
    return Read(id, name, v);
}

std::string Store::Name(const std::string& id) const {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string name;
    std::map<std::string, std::string> v;
    return Read(id, name, v) ? name : std::string();
}

bool Store::Rename(const std::string& id, const std::string& name, std::string& error) {
    if (!NameValid(name)) {
        error = "A character in that name cannot be used.";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    std::string old;
    std::map<std::string, std::string> v;
    if (!Read(id, old, v)) {
        error = "no such profile";
        return false;
    }
    return Write(id, name, v, error);
}

bool Store::Remove(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string name;
    std::map<std::string, std::string> v;
    if (!Read(id, name, v)) {
        error = "no such profile";
        return false;
    }
    std::error_code ec;
    if (!fs::remove(Path(id), ec)) {
        error = "the profile could not be deleted (" + ec.message() + ")";
        return false;
    }
    return true;
}

std::map<std::string, std::string> Store::Values(const std::string& id) const {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string name;
    std::map<std::string, std::string> v;
    Read(id, name, v);
    return v;
}

bool Store::Save(const std::string& id, const std::map<std::string, std::string>& values, std::string& error) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string name;
    std::map<std::string, std::string> old;
    if (!Read(id, name, old)) {
        error = "no such profile";
        return false;
    }
    return Write(id, name, values, error);
}

}  // namespace profiles
