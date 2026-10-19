#pragma once
// PC key bindings (FINDINGS.md "PC key bindings"): the keys a binding may use (the engine's key names and numbers,
// keys.c's table in the XBE at 0x378250) and the gameplay actions with their engine commands and PC defaults. The
// chosen key of an action is kept in pc_settings.ini as "bind.<action> = <key name>" (empty: none).
#include <string>
#include <vector>

namespace pc {

struct KeyInfo {
    const char* name;  // engine key name (bind / unbind), lower case for letters as the engine reads them
    int number;        // engine key number (Key_Event)
    const char* label;
};
// every key a binding may use, in menu order ("None" first); Escape (the pause menu), F1 and F11 (the window's) are
// left out so the menus and the window stay reachable
const std::vector<KeyInfo>& BindableKeys();
const KeyInfo* FindKey(const std::string& name);

struct Action {
    const char* id;       // pc_settings.ini: bind.<id>
    const char* label;    // menu text
    const char* command;  // engine command bound to the key
    const char* key;      // PC default (a BindableKeys name, "" for none)
    unsigned pad = 0;     // instead of a command: the pad button (XINPUT wButtons bit) held while the key is, for
                          // what the game ties to a pad button's release (the Scoreboard screen closes on BACK up)
};
const std::vector<Action>& Actions();
// the key name an action is bound to now (pc_settings.ini, else its default); "" when none
std::string BoundKey(const Action& a);

}  // namespace pc
