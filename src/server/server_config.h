// The dedicated server's configuration (docs/pc_multiplayer.md "N2 configuration"): a text file of "key value" lines
// and command-line overrides ("--key value"), every value checked against its range before anything starts, and the
// match command line the title starts from (the front end's Z:\releasemap.txt). Pure: no game code, no files beyond
// the one read (tests/server_config_test.py).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace server {

struct Config {
    std::string name = "Counter-Strike Xbox PC server";  // hostname: what a server list shows
    int port = 27015;                                    // UDP
    std::string map = "de_dust2";
    int max_players = 65;                                // 1..65: every slot for remote players and bots
    int bots = 0;                                        // bot_quota, at most max_players
    int bot_difficulty = 1;                              // 0 easy, 1 normal, 2 hard, 3 expert
    bool player_collision = true;                        // Off: players pass through each other (crowded matches)
    bool friendly_fire = false;
    double round_time = 3;                               // minutes, 1..9
    double freeze_time = 3;                              // seconds, 0..60
    double buy_time = 1.5;                               // minutes, 0.25..9
    int start_money = 800;                               // 800..16000
    int max_rounds = 0;                                  // 0: no limit
    int time_limit = 30;                                 // minutes a map runs, 0: no limit
    std::string password;                                // sv_password; empty: none
    bool lan_only = false;                               // refuse connections from outside private networks
    int snapshot_rate = 30;                              // the most snapshots a second a client gets (sv_maxupdaterate)
    std::string master;                                  // master servers to register with, "host[:port]" comma list
};

// one line or option's problem
struct Error {
    std::string where;  // "server.cfg:12" or "--port"
    std::string text;
};

// applies "key value" (a file line or an option): false with the error when the key is unknown or the value invalid
bool Set(Config& c, const std::string& key, const std::string& value, std::string& error);
// a whole file's lines ("#" comments, blank lines); errors appended
void ParseFile(Config& c, const std::string& text, const std::string& name, std::vector<Error>& errors);
// what holds after every value was set: bots within max_players
void CheckTogether(const Config& c, std::vector<Error>& errors);
// the match command line, ending with its NUL as the front end writes it
std::string CommandLine(const Config& c);
// a local server's match (N4.7: the game started it for a match from its menus, CSX_SERVER_MATCH): the front end's
// own line (its "+cvar value" tokens: the menus' bots, rules and Player Collision) with this server's map and the
// settings a server needs after them (its slots, bots with no human yet, its name, LAN and password). Empty, with the
// error, when a token is not a plain "+name value..." of letters, digits and _ . - (nothing that ends or adds a command)
std::string MatchLine(const Config& c, const std::string& menu_line, std::string& error);
// the config as "key value" lines again (the effective settings: kept across relaunches; logged with the password
// shown as "(set)")
std::string Describe(const Config& c, bool show_password = false);
// the keys, each with its default and a one-line help
struct Key {
    const char* name;
    const char* help;
};
const std::vector<Key>& Keys();

}  // namespace server
