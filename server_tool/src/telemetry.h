// The server's "@csx status" line as the tool uses it (docs/server_tool.md "Telemetry format").
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace tool {

struct PlayerRow {
    int slot = 0;
    std::string name, team, address, key;
    int score = 0, deaths = 0;
    std::optional<int> ping;  // none for bots
    bool bot = false;
    double seconds = 0;
};

struct Status {
    bool valid = false;
    std::string state, map, next_map, key, game_mode;
    double uptime = 0, tick_hz = 0, frame_ms = 0, frame_ms_max = 0;
    long long frames = 0, time = 0;
    int max_players = 0, humans = 0, bots = 0, port = 0;
    bool password = false, lan_only = false;
    bool net_valid = false;
    int connections = 0;
    std::optional<double> in_bps, out_bps;
    double in_total = 0, out_total = 0;
    std::vector<PlayerRow> players;
};

// a line of the server's output: true (with the status) when it is a status line of version 1
bool ParseStatusLine(const std::string& line, Status& out);

}  // namespace tool
