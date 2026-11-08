// The tool's files (docs/server_tool.md "Files"). Portable first: DATA is csx-server-tool-data/ beside the tool when
// that folder exists or can be made there, else the user's per-user folder (SDL_GetPrefPath).
//   DATA/tool.ini                       window, page, tab, the game folder, the current profile, preferences
//   DATA/profiles/NAME/server.cfg       the profile's server settings (csx-server's own format, owner-only)
//   DATA/profiles/NAME/tool.ini         the profile's tool settings (crash restart)
//   DATA/profiles/NAME/server/          the server's folder: logs/, server_identity.key, admins.cfg, bans.txt, hdd/
//   DATA/rotations/NAME.txt             saved map rotations, one map a line
// Writes are whole (a new file renamed over the old), so a crash never leaves half a file; a profile's server.cfg is
// backed up as server.cfg.bak before it is replaced.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "server/server_config.h"

namespace tool {

class Ini {
public:
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    std::string Get(const std::string& key, const std::string& fallback = {}) const;
    int GetInt(const std::string& key, int fallback) const;
    double GetDouble(const std::string& key, double fallback) const;
    bool GetBool(const std::string& key, bool fallback) const;
    void Set(const std::string& key, const std::string& value) { values_[key] = value; }
    void SetInt(const std::string& key, long long v) { values_[key] = std::to_string(v); }
    void SetDouble(const std::string& key, double v);
    void SetBool(const std::string& key, bool v) { values_[key] = v ? "1" : "0"; }
    const std::map<std::string, std::string>& All() const { return values_; }

private:
    std::map<std::string, std::string> values_;
};

// whole-file writes (a .tmp renamed over the target); owner_only: 0600 on POSIX
bool WriteFileAtomic(const std::string& path, const std::string& text, bool owner_only = false);
bool ReadFileText(const std::string& path, std::string& out);

struct Paths {
    std::string data;        // DATA
    bool portable = false;   // DATA is beside the tool
    std::string tool_dir;    // where the tool's executable is
};
// finds (and makes) DATA; base_dir is the tool's folder, pref_dir the per-user fallback
Paths ResolvePaths(const std::string& base_dir, const std::string& pref_dir, const std::string& forced = {});

std::string ProfileDir(const Paths& p, const std::string& name);
std::string ProfileServerDir(const Paths& p, const std::string& name);
std::vector<std::string> ListProfiles(const Paths& p);
bool ValidProfileName(const std::string& name);  // letters, digits, space, - and _, 1..40

// a profile's settings: false with the problems (file:line) when server.cfg does not read cleanly
bool LoadProfileConfig(const Paths& p, const std::string& name, server::Config& cfg, std::vector<server::Error>& errors);
bool SaveProfileConfig(const Paths& p, const std::string& name, const server::Config& cfg);
bool DeleteProfile(const Paths& p, const std::string& name);

std::vector<std::string> ListRotations(const Paths& p);
bool SaveRotation(const Paths& p, const std::string& name, const std::vector<std::string>& maps);
bool LoadRotation(const Paths& p, const std::string& name, std::vector<std::string>& maps);

// "a,b,c" <-> {a, b, c}
std::vector<std::string> SplitList(const std::string& s);
std::string JoinList(const std::vector<std::string>& v);

}  // namespace tool
