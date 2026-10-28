#pragma once
// Saved player profiles (N4.8, docs/multiplayer_frontend.md): one file a profile, <folder>/profiles/<id>.ini, in the
// writable folder the PC settings are in (never the game's files). A profile has a stable id (16 hex digits, made at
// random when it is created: the only thing anything refers to it by), a name (shown; two profiles may have the same
// one) and its own values of the profile-scoped settings (ProfileScoped: mouse, FOV, bindings, the title's Controller
// Options). No logging and no game state here: src/common/pc_settings.cpp puts a process's profile in front of the
// PC settings, the menus (src/game/pc_party.cpp) list, create, rename, edit and delete them. Thread safe.
#include <map>
#include <string>
#include <vector>

namespace profiles {

struct Info {
    std::string id, name;
};

// a name a profile may have: 1-20 characters of letters, digits, spaces and - _ . ! ? ( ) [ ] * + = : ' @ # &, not only
// spaces (it goes into a console command and a server's userinfo)
bool NameValid(const std::string& name);
// an id as made by Create: 16 lower-case hex digits (anything else is refused, so no id names a path)
bool IdValid(const std::string& id);
// the settings a profile holds its own value of (the others are the PC's: display, audio, the browser...)
bool Scoped(const std::string& key);

class Store {
public:
    // the folder holding profiles/ (or another kind of named settings file in its own subfolder, with its own keys:
    // the match rulesets, N4.8: Store(folder, "rulesets", RuleKey))
    explicit Store(std::string folder, std::string sub = "profiles", bool (*keys)(const std::string&) = Scoped);
    std::vector<Info> List() const;      // every readable profile, by name then id
    // a new profile with that name and those first values (the PC's current ones); "" when the name is not valid or
    // the file could not be written (error says which)
    std::string Create(const std::string& name, const std::map<std::string, std::string>& values, std::string& error);
    bool Exists(const std::string& id) const;
    std::string Name(const std::string& id) const;  // "" for no such profile
    bool Rename(const std::string& id, const std::string& name, std::string& error);
    bool Remove(const std::string& id, std::string& error);  // the file deleted (the menus ask first)
    std::map<std::string, std::string> Values(const std::string& id) const;  // the scoped settings it holds
    bool Save(const std::string& id, const std::map<std::string, std::string>& values, std::string& error);
    std::string Folder() const { return folder_; }

private:
    std::string Path(const std::string& id) const;
    bool Write(const std::string& id, const std::string& name, const std::map<std::string, std::string>& values,
               std::string& error) const;
    bool Read(const std::string& id, std::string& name, std::map<std::string, std::string>& values) const;
    std::string folder_, sub_;
    bool (*keys_)(const std::string&);
};

}  // namespace profiles
