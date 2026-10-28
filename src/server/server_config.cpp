// The dedicated server's configuration (server_config.h).
#include "server/server_config.h"

#include <algorithm>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace server {
namespace {

bool Int(const std::string& v, int lo, int hi, int& out, std::string& error) {
    if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) {
        error = "not a whole number: \"" + v + "\"";
        return false;
    }
    int n = std::atoi(v.c_str());
    if (n < lo || n > hi) {
        error = std::to_string(n) + " is outside " + std::to_string(lo) + ".." + std::to_string(hi);
        return false;
    }
    out = n;
    return true;
}

bool Number(const std::string& v, double lo, double hi, double& out, std::string& error) {
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (v.empty() || *end || !std::isfinite(d) || v.find_first_not_of("0123456789.") != std::string::npos) {
        error = "not a number: \"" + v + "\"";
        return false;
    }
    if (d < lo || d > hi) {
        char b[96];
        std::snprintf(b, sizeof(b), "%g is outside %g..%g", d, lo, hi);
        error = b;
        return false;
    }
    out = d;
    return true;
}

bool Bool(const std::string& v, bool& out, std::string& error) {
    if (v == "1" || v == "on" || v == "yes" || v == "true") return out = true, true;
    if (v == "0" || v == "off" || v == "no" || v == "false") return out = false, true;
    error = "not on or off: \"" + v + "\"";
    return false;
}

// text that goes into the engine's command line and cvars: printable, none of the characters its parser splits or
// quotes at (a quote, a semicolon, a backslash: the info strings' separator), at most max bytes
bool Text(const std::string& v, size_t max, bool spaces, std::string& out, std::string& error) {
    if (v.size() > max) {
        error = "longer than " + std::to_string(max) + " characters";
        return false;
    }
    for (unsigned char ch : v)
        if (ch < 0x20 || ch > 0x7E || ch == '"' || ch == ';' || ch == '\\' || (!spaces && ch == ' ')) {
            error = std::string("may not contain ") + (ch == ' ' ? "spaces" : ch < 0x20 || ch > 0x7E ? "control or non-ASCII characters"
                                                                                                    : std::string("'") + char(ch) + "'");
            return false;
        }
    out = v;
    return true;
}

std::string Num(double d) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", d);
    return b;
}

}  // namespace

const std::vector<Key>& Keys() {
    static const std::vector<Key> k = {
        {"name", "the server's name (hostname), up to 63 characters"},
        {"port", "UDP port, 1..65535 (27015)"},
        {"map", "the map to start on (de_dust2): one of the game folder's maps"},
        {"max_players", "player slots, 1..65 (65): remote players and bots together; the server takes none"},
        {"bots", "bots kept in the match, 0..max_players (0)"},
        {"bot_difficulty", "0 easy, 1 normal, 2 hard, 3 expert (1)"},
        {"player_collision", "on or off (on): off lets players pass through each other, for crowded matches"},
        {"friendly_fire", "on or off (off)"},
        {"round_time", "minutes a round lasts, 1..9 (3)"},
        {"freeze_time", "seconds of freeze at a round's start, 0..60 (3)"},
        {"buy_time", "minutes the buy zone is open, 0.25..9 (1.5)"},
        {"start_money", "800..16000 (800)"},
        {"max_rounds", "rounds before the map ends, 0 for no limit (0)"},
        {"time_limit", "minutes before the map ends, 0 for no limit (30)"},
        {"password", "players must give it to join (setinfo password, run_game.sh --password); empty for none"},
        {"lan_only", "on or off (off): on refuses addresses outside private networks (10/8, 172.16/12, 192.168/16, "
                     "127/8, 169.254/16)"},
        {"snapshot_rate", "the most snapshots a second each client gets, 10..100 (30); the server ticks 30 times a "
                          "second, so more than 30 sends no more"},
        {"master", "master servers to list the server on, HOST[:PORT] separated by commas (none): players find it "
                   "under Internet; LAN players find it without one; a lan_only server registers nowhere"},
    };
    return k;
}

bool Set(Config& c, const std::string& key, const std::string& value, std::string& error) {
    if (key == "name") {
        if (value.empty()) return error = "may not be empty", false;
        return Text(value, 63, true, c.name, error);
    }
    if (key == "port") return Int(value, 1, 65535, c.port, error);
    if (key == "map") {
        if (value.empty() || value.size() > 31 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            error = "a map name is lower case letters, digits and '_': \"" + value + "\"";
            return false;
        }
        c.map = value;
        return true;
    }
    if (key == "max_players") return Int(value, 1, 65, c.max_players, error);
    if (key == "bots") return Int(value, 0, 65, c.bots, error);
    if (key == "bot_difficulty") return Int(value, 0, 3, c.bot_difficulty, error);
    if (key == "player_collision") return Bool(value, c.player_collision, error);
    if (key == "friendly_fire") return Bool(value, c.friendly_fire, error);
    if (key == "round_time") return Number(value, 1, 9, c.round_time, error);
    if (key == "freeze_time") return Number(value, 0, 60, c.freeze_time, error);
    if (key == "buy_time") return Number(value, 0.25, 9, c.buy_time, error);
    if (key == "start_money") return Int(value, 800, 16000, c.start_money, error);
    if (key == "max_rounds") return Int(value, 0, 999, c.max_rounds, error);
    if (key == "time_limit") return Int(value, 0, 9999, c.time_limit, error);
    if (key == "password") return Text(value, 32, false, c.password, error);
    if (key == "lan_only") return Bool(value, c.lan_only, error);
    if (key == "snapshot_rate") return Int(value, 10, 100, c.snapshot_rate, error);
    if (key == "master") {
        // host names, addresses and ports, at most 4 of them
        if (value.size() > 255 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:,") !=
                                      std::string::npos) {
            error = "a comma list of HOST[:PORT] (letters, digits, '.', '-', ':')";
            return false;
        }
        if (std::count(value.begin(), value.end(), ',') > 3) return error = "at most 4 master servers", false;
        c.master = value;
        return true;
    }
    error = "unknown setting \"" + key + "\"";
    return false;
}

void ParseFile(Config& c, const std::string& text, const std::string& name, std::vector<Error>& errors) {
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        number++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t b = line.find_first_of(" \t", a);
        std::string key = line.substr(a, b == std::string::npos ? std::string::npos : b - a), value;
        if (b != std::string::npos) {
            size_t v = line.find_first_not_of(" \t", b);
            if (v != std::string::npos) value = line.substr(v);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        }
        std::string error;
        if (!Set(c, key, value, error)) errors.push_back({name + ":" + std::to_string(number), key + ": " + error});
    }
}

void CheckTogether(const Config& c, std::vector<Error>& errors) {
    if (c.bots > c.max_players)
        errors.push_back({"bots", std::to_string(c.bots) + " bots do not fit in " + std::to_string(c.max_players) +
                                      " player slots (max_players)"});
}

std::string CommandLine(const Config& c) {
    // the front end's match line (tests/launch/README.md) with this server's values. The bot manager fills the quota
    // without waiting for a human: mp_dedicated_server 1 (0x162593, as the console's Dedicated Server option did) and
    // bot_join_after_player 0 (0x1625EA: 1 keeps the quota at 0 while no human is in the match)
    std::string s = "+map " + c.map + " +mp_match_option_preset 1 +wait +maxplayers " + std::to_string(c.max_players) +
                    " +bot_quota " + std::to_string(c.bots) + " +bot_difficulty " + std::to_string(c.bot_difficulty) +
                    " +bot_allow_rogues 0 +bot_join_after_player 0 +mp_player_voting 0 +mp_dedicated_server 1 +mp_timelimit " +
                    std::to_string(c.time_limit) + " +mp_maxrounds " + std::to_string(c.max_rounds) +
                    " +mp_winlimit 0 +mp_roundtime " + Num(c.round_time) + " +mp_freezetime " + Num(c.freeze_time) +
                    " +mp_buytime " + Num(c.buy_time) + " +mp_startmoney " + std::to_string(c.start_money) +
                    " +mp_friendlyfire " + (c.friendly_fire ? "1" : "0") + " +mp_c4timer 45 +bot_auto_vacate 0" +
                    " +sv_maxupdaterate " + std::to_string(c.snapshot_rate) + " +sv_lan " + (c.lan_only ? "1" : "0") +
                    " +hostname \"" + c.name + "\"";
    if (!c.password.empty()) s += " +sv_password " + c.password;
    s += std::string(" +pc_player_collision ") + (c.player_collision ? "1" : "0");
    s += '\0';
    return s;
}

std::string MatchLine(const Config& c, const std::string& menu_line, std::string& error) {
    // "+name value..." tokens: the menus' cvars (mp_, bot_, pc_) and their +wait; the map is this server's (a map
    // change keeps the menus' rules with the next map); the settings a server sets itself are dropped from it and set
    // after it
    auto plain = [](const std::string& t, size_t max) {
        return !t.empty() && t.size() <= max &&
               t.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos;
    };
    std::istringstream in(menu_line);
    std::vector<std::vector<std::string>> cvars;
    std::string t;
    while (in >> t) {
        if (t[0] == '+') {
            std::string name = t.substr(1);
            for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            // (Host a Match's line also has +maxhumanplayers, its Max Players)
            bool known = name == "wait" || name == "map" || name == "maxhumanplayers" || name.rfind("mp_", 0) == 0 ||
                         name.rfind("bot_", 0) == 0 || name.rfind("pc_", 0) == 0;
            if (!plain(name, 40) || !known) {
                error = "the match setting \"" + (plain(t.substr(1), 40) ? t : std::string("(not plain text)")) +
                        "\" is not one a match from the menus sets";
                return {};
            }
            cvars.push_back({name});
        } else if (!cvars.empty() && plain(t, 32) && cvars.back().size() < 4) {
            cvars.back().push_back(t);
        } else {
            error = cvars.empty() ? "the match line does not start with a setting" : "a match setting's value is not plain text";
            return {};
        }
    }
    if (cvars.empty() || cvars[0][0] != "map") {
        error = "the match line does not start with its map";
        return {};
    }
    static const char* const kOwn[] = {"map", "mp_dedicated_server"};
    std::string s = "+map " + c.map;
    bool collision = false;
    for (auto& v : cvars) {
        if (std::find_if(std::begin(kOwn), std::end(kOwn), [&](const char* k) { return v[0] == k; }) != std::end(kOwn)) continue;
        collision = collision || v[0] == "pc_player_collision";
        s += " +" + v[0];
        for (size_t i = 1; i < v.size(); i++) s += " " + v[i];
    }
    // after the menus' tokens (the last one set wins): every slot for players and bots, the snapshot rate, LAN, the
    // name and the password. bot_join_after_player stays the menus' (the engine's 1: the bots join once a human has,
    // the round starting with the owner in it, as in the game's own listen server)
    s += " +maxplayers " + std::to_string(c.max_players) + " +mp_dedicated_server 1 +sv_maxupdaterate " +
         std::to_string(c.snapshot_rate) + " +sv_lan " + (c.lan_only ? "1" : "0") + " +hostname \"" + c.name + "\"";
    if (!c.password.empty()) s += " +sv_password " + c.password;
    if (!collision) s += std::string(" +pc_player_collision ") + (c.player_collision ? "1" : "0");
    s += '\0';
    return s;
}

std::string Describe(const Config& c, bool show_password) {
    std::string s;
    auto add = [&](const char* k, const std::string& v) { s += std::string(k) + " " + v + "\n"; };
    add("name", c.name);
    add("port", std::to_string(c.port));
    add("map", c.map);
    add("max_players", std::to_string(c.max_players));
    add("bots", std::to_string(c.bots));
    add("bot_difficulty", std::to_string(c.bot_difficulty));
    add("player_collision", c.player_collision ? "on" : "off");
    add("friendly_fire", c.friendly_fire ? "on" : "off");
    add("round_time", Num(c.round_time));
    add("freeze_time", Num(c.freeze_time));
    add("buy_time", Num(c.buy_time));
    add("start_money", std::to_string(c.start_money));
    add("max_rounds", std::to_string(c.max_rounds));
    add("time_limit", std::to_string(c.time_limit));
    add("password", show_password || c.password.empty() ? c.password : "(set)");
    add("lan_only", c.lan_only ? "on" : "off");
    add("snapshot_rate", std::to_string(c.snapshot_rate));
    if (!c.master.empty()) add("master", c.master);
    return s;
}

}  // namespace server
