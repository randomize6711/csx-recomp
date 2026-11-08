#include "storage.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tool {

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& text, bool owner_only) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
#ifndef _WIN32
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
    ok = ::fsync(fd) == 0 && ok;
    ok = ::close(fd) == 0 && ok;
    if (owner_only) ::chmod(tmp.c_str(), 0600);
#else
    (void)owner_only;
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), std::streamsize(text.size()));
        ok = bool(f);
    }
#endif
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

bool Ini::Load(const std::string& path) {
    std::string text;
    if (!ReadFileText(path, text)) return false;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

bool Ini::Save(const std::string& path) const {
    std::string text = "# csx-server-tool settings\n";
    for (const auto& [k, v] : values_) text += k + "=" + v + "\n";
    return WriteFileAtomic(path, text);
}

std::string Ini::Get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}
int Ini::GetInt(const std::string& key, int fallback) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    return end && *end == 0 ? int(v) : fallback;
}
double Ini::GetDouble(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    return end && *end == 0 ? v : fallback;
}
bool Ini::GetBool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    return it->second == "1" || it->second == "true" || it->second == "on";
}
void Ini::SetDouble(const std::string& key, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.6g", v);
    values_[key] = b;
}

Paths ResolvePaths(const std::string& base_dir, const std::string& pref_dir, const std::string& forced) {
    Paths p;
    p.tool_dir = base_dir;
    std::error_code ec;
    if (!forced.empty()) {
        fs::create_directories(forced, ec);
        p.data = fs::absolute(forced, ec).string();
        p.portable = true;
        return p;
    }
    fs::path local = fs::path(base_dir) / "csx-server-tool-data";
    if (!fs::is_directory(local, ec)) fs::create_directories(local, ec);
    // writable? (a read-only install folder falls back to the per-user one)
    if (fs::is_directory(local, ec)) {
        fs::path probe = local / ".write-test";
        std::ofstream f(probe);
        bool ok = bool(f << "x");
        f.close();
        fs::remove(probe, ec);
        if (ok) {
            p.data = local.string();
            p.portable = true;
            return p;
        }
    }
    p.data = pref_dir;
    fs::create_directories(p.data, ec);
    return p;
}

bool ValidProfileName(const std::string& name) {
    if (name.empty() || name.size() > 40 || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_';
    });
}

std::string ProfileDir(const Paths& p, const std::string& name) { return (fs::path(p.data) / "profiles" / name).string(); }
std::string ProfileServerDir(const Paths& p, const std::string& name) {
    return (fs::path(ProfileDir(p, name)) / "server").string();
}

std::vector<std::string> ListProfiles(const Paths& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(p.data) / "profiles", ec))
        if (e.is_directory() && ValidProfileName(e.path().filename().string()) && fs::exists(e.path() / "server.cfg"))
            out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool LoadProfileConfig(const Paths& p, const std::string& name, server::Config& cfg, std::vector<server::Error>& errors) {
    std::string text;
    std::string path = (fs::path(ProfileDir(p, name)) / "server.cfg").string();
    if (!ReadFileText(path, text)) {
        errors.push_back({path, "cannot be read"});
        return false;
    }
    server::Config c;
    server::ParseFile(c, text, "server.cfg", errors);
    server::CheckTogether(c, errors);
    cfg = c;
    return errors.empty();
}

bool SaveProfileConfig(const Paths& p, const std::string& name, const server::Config& cfg) {
    fs::path dir = ProfileDir(p, name), path = dir / "server.cfg";
    std::error_code ec;
    fs::create_directories(dir / "server", ec);
    if (fs::exists(path, ec)) fs::copy_file(path, dir / "server.cfg.bak", fs::copy_options::overwrite_existing, ec);
    std::string text = "# csx-server settings (csx-server --help lists the keys); written by csx-server-tool\n" +
                       server::Describe(cfg, true);
    return WriteFileAtomic(path.string(), text, true);  // owner-only: the password is in it
}

bool DeleteProfile(const Paths& p, const std::string& name) {
    if (!ValidProfileName(name)) return false;
    std::error_code ec;
    fs::remove_all(ProfileDir(p, name), ec);
    return !ec;
}

std::vector<std::string> ListRotations(const Paths& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(p.data) / "rotations", ec))
        if (e.path().extension() == ".txt" && ValidProfileName(e.path().stem().string())) out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool SaveRotation(const Paths& p, const std::string& name, const std::vector<std::string>& maps) {
    if (!ValidProfileName(name)) return false;
    std::string text;
    for (const auto& m : maps) text += m + "\n";
    return WriteFileAtomic((fs::path(p.data) / "rotations" / (name + ".txt")).string(), text);
}

bool LoadRotation(const Paths& p, const std::string& name, std::vector<std::string>& maps) {
    std::string text;
    if (!ValidProfileName(name) || !ReadFileText((fs::path(p.data) / "rotations" / (name + ".txt")).string(), text)) return false;
    maps.clear();
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') maps.push_back(l);
    }
    return true;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string JoinList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

}  // namespace tool
