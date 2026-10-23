#include "common/pc_settings.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

#include "app/app.h"
#include "core/log.h"

namespace pc {

namespace {
std::mutex g_lock;
std::map<std::string, std::string> g_values;
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
}
}  // namespace

std::string Get(const std::string& name, const std::string& fallback) {
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
        g_values[name] = value;
    }
    g_version++;
    LOGI("pc: %s = %s", name.c_str(), value.c_str());
}

void Save() {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked();
    std::ofstream f(Path(), std::ios::trunc);
    f << "# PC settings (Counter-Strike Xbox recompilation), edited in Options > PC Options\n";
    for (auto& [k, v] : g_values) f << k << " = " << v << "\n";
    LOGI("pc: settings saved to %s", Path().c_str());
}

unsigned Version() { return g_version; }

bool XboxEffectsMaster() { return Get("xbox_effects", "1") != "0"; }
bool XboxEffect(const std::string& name) { return XboxEffectsMaster() && Get(name, "1") != "0"; }

}  // namespace pc
