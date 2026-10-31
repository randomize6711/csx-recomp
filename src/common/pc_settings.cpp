#include "common/pc_settings.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "app/app.h"
#include "common/profiles.h"
#include "core/log.h"

namespace pc {

std::string ProfileFolder();

namespace {
std::mutex g_lock;
std::map<std::string, std::string> g_values;
std::map<std::string, std::string> g_profile;  // the scoped settings of the profile in use (g_profile_id)
std::string g_profile_id;
bool g_loaded = false;
std::atomic<unsigned> g_version{0};

std::string Path() { return (app::ExeDir() / "pc_settings.ini").string(); }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(Path());
    for (std::string line; std::getline(f, line);) {
        line = Trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        g_values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    if (!g_values.empty()) LOGI("pc: %zu settings from %s", g_values.size(), Path().c_str());
    if (const char* id = std::getenv("CSX_PC_PROFILE"); id && *id) {
        profiles::Store store(ProfileFolder());
        if (profiles::IdValid(id) && store.Exists(id)) {
            g_profile_id = id;
            g_profile = store.Values(id);
            LOGI("pc: playing as profile %s (%s), %zu of its own settings", id, store.Name(id).c_str(), g_profile.size());
        } else {
            LOGW("pc: profile %s (CSX_PC_PROFILE) not found: the PC's settings are used", id);
        }
    }
}

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}
}  // namespace

std::string Get(const std::string& name, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked();
    if (!g_profile_id.empty() && profiles::Scoped(name))
        if (auto p = g_profile.find(name); p != g_profile.end()) return p->second;
    auto it = g_values.find(name);
    return it == g_values.end() ? fallback : it->second;
}

std::string GetGlobal(const std::string& name, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked();
    auto it = g_values.find(name);
    return it == g_values.end() ? fallback : it->second;
}

float GetFloat(const std::string& name, float fallback) {
    std::string v = Get(name, "");
    return v.empty() ? fallback : float(std::atof(v.c_str()));
}

void Set(const std::string& name, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        LoadLocked();
        if (!g_profile_id.empty() && profiles::Scoped(name)) g_profile[name] = value;
        else g_values[name] = value;
    }
    g_version++;
    LOGI("pc: %s = %s", name.c_str(), value.c_str());
}

void Erase(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        LoadLocked();
        if (!g_profile_id.empty() && profiles::Scoped(name)) g_profile.erase(name);
        else g_values.erase(name);
    }
    g_version++;
    LOGI("pc: %s removed", name.c_str());
}

void Save() {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked();
    // whole or not at all: written beside the file, then renamed over it (a crash or a full disk mid-write leaves the
    // last saved settings as they were)
    const std::string path = Path(), tmp = path + ".tmp";
    bool written = false;
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# PC settings (Counter-Strike Xbox recompilation), edited in the game's Options\n";
        for (auto& [k, v] : g_values) f << k << " = " << v << "\n";
        f.flush();
        written = bool(f);
    }
    std::error_code ec;
    if (written) std::filesystem::rename(tmp, path, ec);
    if (!written || ec) {
        std::filesystem::remove(tmp, ec);
        LOGW("pc: settings NOT saved to %s (%s): the file keeps its last saved settings", path.c_str(),
             written ? "the rename failed" : "the write failed");
    } else {
        LOGI("pc: settings saved to %s", path.c_str());
    }
    if (!g_profile_id.empty()) {
        std::string error;
        if (profiles::Store(ProfileFolder()).Save(g_profile_id, g_profile, error))
            LOGI("pc: profile %s's settings saved", g_profile_id.c_str());
        else
            LOGW("pc: profile %s's settings not saved: %s", g_profile_id.c_str(), error.c_str());
    }
}

bool UseProfile(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        LoadLocked();
        profiles::Store store(ProfileFolder());
        if (!id.empty() && !(profiles::IdValid(id) && store.Exists(id))) return false;
        g_profile_id = id;
        g_profile = id.empty() ? std::map<std::string, std::string>() : store.Values(id);
        SetEnv("CSX_PC_PROFILE", id);
        if (id.empty()) LOGI("pc: playing as no profile");
        else LOGI("pc: playing as profile %s (%s)", id.c_str(), store.Name(id).c_str());
    }
    g_version++;
    return true;
}

std::string ActiveProfile() {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked();
    return g_profile_id;
}

// the profiles are the party's, whichever player's folder a process runs in: CSX_PROFILE_DIR (the compositor's run
// folder, given to every player), else this process's own folder
std::string ProfileFolder() {
    const char* d = std::getenv("CSX_PROFILE_DIR");
    return d && *d ? std::string(d) : app::ExeDir().string();
}

unsigned Version() { return g_version; }

bool XboxEffectsMaster() { return Get("xbox_effects", "1") != "0"; }
bool XboxEffect(const std::string& name) { return XboxEffectsMaster() && Get(name, "1") != "0"; }

}  // namespace pc
