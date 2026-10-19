#include "common/pc_bindings.h"

#include "common/pc_settings.h"

namespace pc {

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
            {"KP_PLUS", 174, "Keypad +"}, {"KP_MINUS", 173, "Keypad -"}, {"`", '`', "`"}, {"-", '-', "-"},
            {"=", '=', "="}, {"[", '[', "["}, {"]", ']', "]"}, {"SEMICOLON", ';', ";"}, {",", ',', ","},
            {".", '.', "."}, {"/", '/', "/"}};
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
    };
    return a;
}

std::string BoundKey(const Action& a) { return Get(std::string("bind.") + a.id, a.key); }

}  // namespace pc
