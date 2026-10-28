// The dedicated server's configuration (src/server/server_config.h) on its own (tests/server_config_test.py): every
// key's range, a file's lines and errors, the command line it makes, the password kept out of what is logged.
#include <cstdio>
#include <string>
#include <vector>

#include "server/server_config.h"

using namespace server;

namespace {
int g_failed = 0, g_checks = 0;
void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}
bool Ok(const char* key, const char* value) {
    Config c;
    std::string e;
    return Set(c, key, value, e);
}
}  // namespace

int main() {
    Config d;
    Check(d.port == 27015 && d.max_players == 65 && d.bots == 0 && d.player_collision && !d.friendly_fire && !d.lan_only &&
              d.snapshot_rate == 30 && d.password.empty(),
          "defaults: port 27015, 65 slots, no bots, collision on, no friendly fire, internet, 30 snapshots, no password");
    Check(Ok("port", "1") && Ok("port", "65535") && !Ok("port", "0") && !Ok("port", "65536") && !Ok("port", "27015x") &&
              !Ok("port", "-1") && !Ok("port", ""),
          "port: 1..65535, whole numbers only");
    Check(Ok("max_players", "65") && Ok("max_players", "1") && !Ok("max_players", "66") && !Ok("max_players", "0"),
          "max_players: 1..65");
    Check(Ok("bots", "0") && Ok("bots", "65") && !Ok("bots", "66"), "bots: 0..65");
    Check(Ok("bot_difficulty", "3") && !Ok("bot_difficulty", "4"), "bot_difficulty: 0..3");
    Check(Ok("master", "master.example.org") && Ok("master", "10.0.0.5:27010,master.example.org:27011") &&
              !Ok("master", "a;quit") && !Ok("master", "a b") && !Ok("master", "a,b,c,d,e"),
          "master: a comma list of up to 4 HOST[:PORT], nothing else");
    Check(Ok("player_collision", "off") && Ok("player_collision", "1") && !Ok("player_collision", "maybe"), "booleans");
    Check(Ok("round_time", "1") && Ok("round_time", "2.5") && Ok("round_time", "9") && !Ok("round_time", "0.5") &&
              !Ok("round_time", "10") && !Ok("round_time", "nan") && !Ok("round_time", "1e1"),
          "round_time: 1..9 minutes, plain numbers");
    Check(Ok("snapshot_rate", "10") && Ok("snapshot_rate", "100") && !Ok("snapshot_rate", "9") && !Ok("snapshot_rate", "101"),
          "snapshot_rate: 10..100");
    Check(Ok("map", "de_dust2") && !Ok("map", "../etc") && !Ok("map", "de dust") && !Ok("map", "DE_DUST2") && !Ok("map", ""),
          "map: lower case letters, digits and _ only (no paths)");
    Check(Ok("name", "My server 1") && !Ok("name", "a\"b") && !Ok("name", "a;quit") && !Ok("name", "a\\b") &&
              !Ok("name", "a\nb") && !Ok("name", std::string(64, 'x').c_str()) && !Ok("name", ""),
          "name: printable, no quote, semicolon, backslash or line break, 1..63");
    Check(Ok("password", "s3cret") && Ok("password", "") && !Ok("password", "two words") && !Ok("password", "a;b") &&
              !Ok("password", std::string(33, 'x').c_str()),
          "password: no spaces or separators, up to 32");
    Check(!Ok("rcon_password", "x") && !Ok("unknown", "1"), "unknown keys are refused (no rcon setting)");

    Config c;
    std::vector<Error> errors;
    ParseFile(c,
              "# a server\n"
              "name  Friday night \n"
              "\n"
              "port 27016\r\n"
              "bots 64\n"
              "max_players 40\n"
              "player_collision off\n"
              "password hunter2\n"
              "bogus 1\n"
              "port 99999\n",
              "test.cfg", errors);
    Check(c.name == "Friday night" && c.port == 27016 && c.bots == 64 && c.max_players == 40 && !c.player_collision,
          "file: values set, spaces and CR trimmed, comments and blanks skipped");
    Check(errors.size() == 2 && errors[0].where == "test.cfg:9" && errors[1].where == "test.cfg:10" && c.port == 27016,
          "file: an unknown key and a bad value reported with their lines, the bad value not applied");
    std::vector<Error> together;
    CheckTogether(c, together);
    Check(together.size() == 1, "bots over max_players is refused");
    c.max_players = 65;
    together.clear();
    CheckTogether(c, together);
    Check(together.empty(), "bots within max_players is fine");

    std::string line = CommandLine(c);
    Check(!line.empty() && line.back() == '\0', "command line: ends with its NUL");
    Check(line.find("+map de_dust2 ") == 0 && line.find(" +maxplayers 65 ") != std::string::npos &&
              line.find(" +bot_quota 64 ") != std::string::npos && line.find(" +mp_dedicated_server 1 ") != std::string::npos &&
              line.find(" +sv_password hunter2") != std::string::npos && line.find(" +hostname \"Friday night\"") != std::string::npos &&
              line.find(" +pc_player_collision 0") != std::string::npos && line.find(" +sv_maxupdaterate 30 ") != std::string::npos,
          "command line: the settings as the engine's commands");
    Config np;
    Check(CommandLine(np).find("sv_password") == std::string::npos, "command line: no password, no sv_password");
    std::string shown = Describe(c), kept = Describe(c, true);
    Check(shown.find("hunter2") == std::string::npos && shown.find("password (set)") != std::string::npos,
          "describe: the password is not shown in what is logged");
    Config back;
    std::vector<Error> e2;
    ParseFile(back, kept, "kept", e2);
    Check(e2.empty() && Describe(back, true) == kept, "describe: the kept settings read back the same");

    // a local server's match (N4.7): the menus' line (vk_split_4's, its spacing kept) with the server's map and settings
    const std::string menu = "+map de_airstrip +wait +bot_quota 61 +bot_difficulty 1 +bot_join_team any    "
                             "+mp_match_option_preset 2 +mp_player_voting 0 +mp_dedicated_server 0 +mp_timelimit 60 "
                             "+mp_maxrounds 7 +mp_buytime 1.5 +bot_allow_rogues 1  +bot_auto_vacate 0";
    Config lc;
    lc.map = "de_aztec";
    std::string err;
    std::string ml = MatchLine(lc, menu, err);
    Check(err.empty() && !ml.empty() && ml.back() == '\0', "match line: the menus' line taken");
    Check(ml.find("+map de_aztec +wait +bot_quota 61 +bot_difficulty 1 +bot_join_team any +mp_match_option_preset 2") == 0 &&
              ml.find(" +mp_buytime 1.5 ") != std::string::npos && ml.find("de_airstrip") == std::string::npos,
          "match line: the server's map, the menus' settings in their order");
    Check(ml.find("+mp_dedicated_server 0") == std::string::npos && ml.find(" +mp_dedicated_server 1 ") != std::string::npos &&
              ml.find("bot_join_after_player") == std::string::npos && ml.find(" +maxplayers 65 ") != std::string::npos &&
              ml.find(" +pc_player_collision 1") != std::string::npos,
          "match line: the server's own settings after the menus' (dedicated, 65 slots; bots wait for a human as in the game)");
    // Host a Match's line (net_split_listen's)
    const std::string host = "+map de_airstrip   +maxhumanplayers 10   +mp_dedicated_server 0  +mp_match_option_preset 2 "
                             "+mp_timelimit 60 +mp_maxrounds 7 +mp_winlimit 4 +mp_roundtime 5 +mp_freezetime 6 +mp_buytime 1.5 "
                             "+mp_startmoney 800 +mp_friendlyfire 0 +mp_c4timer 45";
    std::string hl = MatchLine(lc, host, err);
    Check(!hl.empty() && hl.find(" +maxhumanplayers 10 ") != std::string::npos, "match line: Host a Match's line taken (" + err + ")");
    // Custom Options' one-team row (the bots one team, the players the other)
    std::string one = MatchLine(lc, menu + " +wait +bot_join_team t +mp_autoteambalance 0 +mp_limitteams 0 +teampreference 2", err);
    Check(one.find(" +teampreference 2") != std::string::npos && one.find(" +bot_join_team t ") != std::string::npos,
          "match line: the one-team row's settings taken (" + err + ")");
    std::string off = MatchLine(lc, menu + " +pc_player_collision 0", err);
    Check(off.find("+pc_player_collision 0") != std::string::npos && off.find("+pc_player_collision 1") == std::string::npos,
          "match line: the menus' Player Collision kept, not the server's default");
    for (const char* bad : {"+map de_dust2 +exec evil.cfg", "+map de_dust2 +mp_timelimit 5;quit", "+map de_dust2 +rcon_password x",
                            "+map de_dust2 +mp_timelimit \"5\"", "+map de_dust2 +connect 1.2.3.4", "de_dust2 +mp_timelimit 5",
                            "+mp_timelimit 5", "+map de_dust2 +mp_timelimit 5\n+quit", "+map de_dust2 +hostname x"}) {
        err.clear();
        Check(MatchLine(lc, bad, err).empty() && !err.empty(), std::string("match line: refused: ") + bad);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
