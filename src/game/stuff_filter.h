// What a network server may run on its clients through svc_stufftext (src/game/net_security.cpp StuffTextFilter), as
// a pure function for tests/stuff_filter_test.py.
#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace csx {

// what a network server may run on its clients through svc_stufftext: what Counter-Strike's server sends (found in
// play and in the game DLL's CLIENT_COMMAND strings) and the PC port's own; anything else is logged and dropped
inline const std::set<std::string> kStuffAllowed = {
    "reconnect", "retry", "disconnect", "fullserverinfo", "echo", "spk", "speak", "play", "playvol", "mp3", "cd",
    "lastinv", "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8", "slot9", "slot10",
    "-attack", "-attack2", "+attack", "-use", "-jump", "-duck", "-reload", "-speed", "-forward", "-back",
    "-moveleft", "-moveright", "-left", "-right", "-lookup", "-lookdown", "-strafe", "-showscores",
    "cl_minmodels", "spec_mode", "spec_pip", "spec_autodirector", "spec_drawnames", "spec_drawcone", "spec_drawstatus",
    "hud_takesshots", "hud_centerid", "cl_righthand", "cl_corpsestay", "voice_modenable", "voice_scale",
    "room_type", "say", "say_team", "fov", "default_fov", "hideradar", "drawradar", "unpause", "pc_player_collision",
    "firstperson", "thirdperson", "bot_menu", "menuselect", "showbriefing", "chooseteam", "buy", "buyequip",
    "cl_dlmax", "cl_lc", "cl_lw", "cl_updaterate", "rate", "cl_cmdrate", "ex_interp", "gl_spriteblend",
};

// the commands of a server's stufftext split as Cbuf_Execute (0x3BB50) splits them: a newline always ends a command,
// even inside quotes (0x3BB94), a ';' only outside them, the quotes counted afresh on each line. A command is kept when
// its name (the first word, compared as the engine's lookup does, case-insensitively) is on the list; a name the
// engine would read differently (quoted, with control characters) is not on it, so is dropped. Also dropped: a line
// with unbalanced quotes, a command over 1000 characters, and text after the last newline (Cbuf_AddText appends the
// next message to it: two stufftexts would make one command the filter never saw whole)
inline std::string FilterStuff(const std::string& text, std::vector<std::string>& dropped) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            dropped.push_back(text.substr(pos) + " (no newline)");
            break;
        }
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (std::count(line.begin(), line.end(), '"') % 2) {
            dropped.push_back(line + " (unbalanced quotes)");
            out += '\n';
            continue;
        }
        std::string cmd;
        bool quoted = false;
        auto flush = [&](char sep) {
            std::string c;
            c.swap(cmd);
            size_t a = c.find_first_not_of(" \t");
            if (a != std::string::npos) {
                size_t e = c.find_first_of(" \t", a);
                std::string name = c.substr(a, e == std::string::npos ? std::string::npos : e - a);
                for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (!kStuffAllowed.count(name) || c.size() > 1000) {
                    dropped.push_back(c.substr(a, 200));
                    c.clear();
                }
            }
            out += c;
            out += sep;
        };
        for (char ch : line) {
            if (ch == '"') quoted = !quoted;
            if (!quoted && ch == ';') {
                flush(';');
                continue;
            }
            cmd += ch;
        }
        flush('\n');
    }
    return out;
}

}  // namespace csx
