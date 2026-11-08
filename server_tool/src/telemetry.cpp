#include "telemetry.h"

#include "json.h"

namespace tool {

bool ParseStatusLine(const std::string& line, Status& out) {
    static const std::string kPrefix = "@csx status ";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    json::Value v;
    if (!json::Parse(line.substr(kPrefix.size()), v) || v["v"].Num() != 1) return false;
    Status s;
    s.valid = true;
    s.state = v["state"].Str();
    s.map = v["map"].Str();
    s.next_map = v["next_map"].Str();
    s.key = v["key"].Str();
    s.game_mode = v["game_mode"].Str();
    s.uptime = v["uptime"].Num();
    s.tick_hz = v["tick_hz"].Num();
    s.frame_ms = v["frame_ms"].Num();
    s.frame_ms_max = v["frame_ms_max"].Num();
    s.frames = (long long)v["frames"].Num();
    s.time = (long long)v["time"].Num();
    s.max_players = int(v["max_players"].Num());
    s.humans = int(v["humans"].Num());
    s.bots = int(v["bots"].Num());
    s.port = int(v["port"].Num());
    s.password = v["password"].Bool();
    s.lan_only = v["lan_only"].Bool();
    const json::Value& net = v["net"];
    if (net.type == json::Value::Type::Object) {
        s.net_valid = true;
        s.connections = int(net["connections"].Num());
        if (!net["in_bps"].IsNull()) s.in_bps = net["in_bps"].Num();
        if (!net["out_bps"].IsNull()) s.out_bps = net["out_bps"].Num();
        s.in_total = net["in_total"].Num();
        s.out_total = net["out_total"].Num();
    }
    for (const json::Value& p : v["players"].a) {
        PlayerRow r;
        r.slot = int(p["slot"].Num());
        r.name = p["name"].Str();
        r.team = p["team"].Str("-");
        r.address = p["address"].Str();
        r.key = p["key"].Str();
        r.score = int(p["score"].Num());
        r.deaths = int(p["deaths"].Num());
        if (!p["ping"].IsNull()) r.ping = int(p["ping"].Num());
        r.bot = p["bot"].Bool();
        r.seconds = p["seconds"].Num();
        s.players.push_back(std::move(r));
    }
    out = std::move(s);
    return true;
}

}  // namespace tool
