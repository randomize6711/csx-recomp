#include "common/pc_bindings.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "common/pc_settings.h"

namespace pc {

namespace {
// UTF-8 to Latin-1 (the menus' text: the game widens its bytes), empty when a character is not in Latin-1
std::string Latin1(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            out += char(c);
            i++;
        } else if (c >= 0xC2 && c <= 0xC3 && i + 1 < utf8.size()) {
            out += char(((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F));
            i += 2;
        } else {
            return {};
        }
    }
    return out;
}

// a punctuation key's name as the keyboard layout prints it (CSX_KEY_LABELS from the window: "C=LABEL" lines in
// UTF-8), else its US character; the names live as long as the program
const char* Label(char key, const char* us, const char* suffix = "") {
    static std::deque<std::string> names;
    std::string label = us;
    const char* env = std::getenv("CSX_KEY_LABELS");
    for (const char* p = env; p && *p;) {
        const char* end = std::strchr(p, '\n');
        std::string line = end ? std::string(p, end) : std::string(p);
        if (std::string l = line.size() > 2 && line[0] == key && line[1] == '=' ? Latin1(line.substr(2)) : "";
            !l.empty())
            label = l + suffix;
        p = end ? end + 1 : nullptr;
    }
    if (label == us) return us;
    names.push_back(label);
    return names.back().c_str();
}
}  // namespace

const std::vector<KeyInfo>& BindableKeys() {
    static const std::vector<KeyInfo> k = [] {
        std::vector<KeyInfo> v = {{"", -1, "None"}};
        static const char* letters[26] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                          "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
        static const char* upper[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        for (int i = 0; i < 26; i++) v.push_back({letters[i], 'a' + i, upper[i]});
        static const char* digits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        for (int i = 1; i <= 10; i++) v.push_back({digits[i % 10], '0' + i % 10, digits[i % 10]});
        const KeyInfo rest[] = {
            {"SPACE", 32, "Space"}, {"TAB", 9, "Tab"}, {"ENTER", 13, "Enter"}, {"BACKSPACE", 127, "Backspace"},
            {"SHIFT", 134, "Shift"}, {"CTRL", 133, "Ctrl"}, {"ALT", 132, "Alt"}, {"CAPSLOCK", 175, "Caps Lock"},
            {"MOUSE1", 241, "Mouse 1"}, {"MOUSE2", 242, "Mouse 2"}, {"MOUSE3", 243, "Mouse 3"},
            {"MOUSE4", 244, "Mouse 4"}, {"MOUSE5", 245, "Mouse 5"}, {"MWHEELUP", 240, "Wheel Up"},
            {"MWHEELDOWN", 239, "Wheel Down"}, {"UPARROW", 128, "Up Arrow"}, {"DOWNARROW", 129, "Down Arrow"},
            {"LEFTARROW", 130, "Left Arrow"}, {"RIGHTARROW", 131, "Right Arrow"}, {"INS", 147, "Insert"},
            {"DEL", 148, "Delete"}, {"HOME", 151, "Home"}, {"END", 152, "End"}, {"PGUP", 150, "Page Up"},
            {"PGDN", 149, "Page Down"}, {"F2", 136, "F2"}, {"F3", 137, "F3"}, {"F4", 138, "F4"}, {"F5", 139, "F5"},
            {"F6", 140, "F6"}, {"F7", 141, "F7"}, {"F8", 142, "F8"}, {"F9", 143, "F9"}, {"F10", 144, "F10"},
            {"F12", 146, "F12"}, {"KP_INS", 170, "Keypad 0"}, {"KP_END", 166, "Keypad 1"},
            {"KP_DOWNARROW", 167, "Keypad 2"}, {"KP_PGDN", 168, "Keypad 3"}, {"KP_LEFTARROW", 163, "Keypad 4"},
            {"KP_5", 164, "Keypad 5"}, {"KP_RIGHTARROW", 165, "Keypad 6"}, {"KP_HOME", 160, "Keypad 7"},
            {"KP_UPARROW", 161, "Keypad 8"}, {"KP_PGUP", 162, "Keypad 9"}, {"KP_ENTER", 169, "Keypad Enter"},
            {"KP_PLUS", 174, "Keypad +"}, {"KP_MINUS", 173, "Keypad -"}, {"KP_DEL", 171, "Keypad ."},
            {"KP_SLASH", 172, "Keypad /"}, {"`", '`', Label('`', "` (left of 1)", " (left of 1)")},
            {"-", '-', Label('-', "-")}, {"=", '=', Label('=', "=")}, {"[", '[', Label('[', "[")},
            {"]", ']', Label(']', "]")}, {"SEMICOLON", ';', Label(';', ";")}, {"'", '\'', Label('\'', "'")},
            {"\\", '\\', Label('\\', "\\")}, {"<", '<', Label('<', "<")}, {",", ',', Label(',', ",")},
            {".", '.', Label('.', ".")}, {"/", '/', Label('/', "/")}};
        for (const KeyInfo& r : rest) v.push_back(r);
        return v;
    }();
    return k;
}

const KeyInfo* FindKey(const std::string& name) {
    for (const KeyInfo& k : BindableKeys())
        if (name == k.name) return &k;
    return nullptr;
}

const std::vector<Action>& Actions() {
    static const std::vector<Action> a = {
        {"forward", "Move Forward", "+forward", "w"},
        {"back", "Move Back", "+back", "s"},
        {"moveleft", "Move Left", "+moveleft", "a"},
        {"moveright", "Move Right", "+moveright", "d"},
        {"jump", "Jump", "+jump", "SPACE"},
        {"duck", "Crouch", "+duck", "CTRL"},
        {"walk", "Walk", "+speed", "SHIFT"},
        {"attack", "Fire", "+attack", "MOUSE1"},
        {"attack2", "Secondary Fire", "+attack2", "MOUSE2"},
        {"reload", "Reload", "+reload", "r"},
        {"use", "Use", "+use", "e"},
        {"weapon_next", "Next Weapon", "invnext", "MWHEELDOWN"},
        {"weapon_prev", "Previous Weapon", "invprev", "MWHEELUP"},
        {"weapon_last", "Last Weapon", "lastinv", "q"},
        {"slot1", "Primary Weapon", "slot1", "1"},
        {"slot2", "Pistol", "slot2", "2"},
        {"slot3", "Knife", "slot3", "3"},
        {"slot4", "Grenades", "slot4", "4"},
        {"slot5", "Bomb", "slot5", "5"},
        {"drop", "Drop Weapon", "drop", "g"},
        {"buymenu", "Buy Menu", "+buymenu", "b"},
        {"scores", "Scoreboard", "", "TAB", 0x20},  // BACK: +showscores, and the screen closes on its release
        {"nightvision", "Night Vision", "nightvision", "n"},
        {"followme", "Radio: Follow Me", "followme", "z"},
        {"holdpos", "Radio: Hold Position", "holdpos", "x"},
        {"report", "Radio: Report In", "report", "c"},
        {"needhelp", "Radio: Need Backup", "needhelp", "v"},
        {"console", "Toggle Developer Console", "toggleconsole", "`"},
        // N5C (appended: the rows above keep their places): CS 1.6 / Condition Zero style buying, grenades, radio
        {"buyammo1", "Buy Primary Ammo", "buyammo1", ","},
        {"buyammo2", "Buy Secondary Ammo", "buyammo2", "."},
        {"autobuy", "Autobuy", "autobuy", "F3"},
        {"rebuy", "Rebuy", "rebuy", "F4"},
        // N7 (docs/n7_gameplay.md): the game's own impulses (the flashlight where the server allows it, mp_flashlight;
        // the spray logo)
        {"flashlight", "Flashlight", "impulse 100", "f"},
        {"spray", "Spray Logo", "impulse 201", "t"},
        {"invite", "Copy Invite Code", "pc_invite", "F6"},  // N9 (src/game/pc_invite.cpp)
        {"hud_edit", "Edit HUD Layout", "pc_hud_edit", "F7"},  // N10 (src/game/pc_hud_layout.cpp)
        {"hegrenade", "HE Grenade", "weapon_hegrenade", ""},
        {"flashbang", "Flashbang", "weapon_flashbang", ""},
        {"smokegrenade", "Smoke Grenade", "weapon_smokegrenade", ""},
        {"radio1", "Radio Menu: Commands", "radio1", ""},
        {"radio2", "Radio Menu: Group", "radio2", ""},
        {"radio3", "Radio Menu: Responses", "radio3", ""},
        {"coverme", "Radio: Cover Me", "coverme", ""},
        {"takepoint", "Radio: You Take the Point", "takepoint", ""},
        {"regroup", "Radio: Regroup Team", "regroup", ""},
        {"takingfire", "Radio: Taking Fire", "takingfire", ""},
        {"go", "Radio: Go Go Go", "go", ""},
        {"fallback", "Radio: Team, Fall Back", "fallback", ""},
        {"sticktog", "Radio: Stick Together", "sticktog", ""},
        {"getinpos", "Radio: Get in Position", "getinpos", ""},
        {"stormfront", "Radio: Storm the Front", "stormfront", ""},
        {"roger", "Radio: Affirmative", "roger", ""},
        {"enemyspot", "Radio: Enemy Spotted", "enemyspot", ""},
        {"sectorclear", "Radio: Sector Clear", "sectorclear", ""},
        {"inposition", "Radio: In Position", "inposition", ""},
        {"reportingin", "Radio: Reporting In", "reportingin", ""},
        {"getout", "Radio: Get Out of There", "getout", ""},
        {"negative", "Radio: Negative", "negative", ""},
        {"enemydown", "Radio: Enemy Down", "enemydown", ""},
        // Condition Zero's extra lines (pc_radio.cpp): only on servers that serve them (this game's)
        {"cz_circleback", "CZ Radio: Circle Around Back", "csx_radio circleback", ""},
        {"cz_flankthem", "CZ Radio: Flank Them", "csx_radio flankthem", ""},
        {"cz_meetme", "CZ Radio: Meet at My Location", "csx_radio meetme", ""},
        {"cz_matedown", "CZ Radio: Teammate Down", "csx_radio matedown", ""},
        {"cz_imhit", "CZ Radio: I'm Hit", "csx_radio imhit", ""},
        {"cz_fireassis", "CZ Radio: Need Fire Assistance", "csx_radio fireassis", ""},
        {"cz_hitassist", "CZ Radio: Hit, Need Assistance", "csx_radio hitassist", ""},
        {"cz_bombpl", "CZ Radio: Bomb Planted", "csx_radio bombpl", ""},
        {"cz_bombdef", "CZ Radio: Bomb Defused", "csx_radio bombdef", ""},
        {"cz_hosdown", "CZ Radio: Hostage Down", "csx_radio hosdown", ""},
    };
    return a;
}

std::string BoundKey(const Action& a) { return Get(std::string("bind.") + a.id, a.key); }

}  // namespace pc
