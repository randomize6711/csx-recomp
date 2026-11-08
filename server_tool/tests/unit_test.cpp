// Unit tests of the server tool's parts without a window or a server: the JSON reader, the telemetry, settings and
// profiles, rotations, the game folder check and map list, log parsing, the crash policy, SHA-256.
//   build/server_tool/csx-server-tool-tests      (tests/server_tool_test.py runs it)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

#include "controller.h"
#include "gamefiles.h"
#include "json.h"
#include "logs.h"
#include "server_process.h"
#include "sha256.h"
#include "storage.h"
#include "telemetry.h"

namespace fs = std::filesystem;
using namespace tool;

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static void Touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

int main() {
    // --- json
    {
        json::Value v;
        Check(json::Parse(R"({"a":1,"b":[true,false,null],"c":"x\"yé","d":{"e":-2.5e1}})", v), "json: parses");
        Check(v["a"].Num() == 1 && v["b"].a.size() == 3 && v["b"].a[0].Bool() && v["b"].a[2].IsNull(), "json: values");
        Check(v["c"].Str() == "x\"y\xc3\xa9", "json: escapes and \\u00e9 as UTF-8");
        Check(v["d"]["e"].Num() == -25, "json: nested number");
        Check(v["missing"]["deeper"].IsNull(), "json: missing keys are null");
        for (const char* bad : {"{", "[1,", "{\"a\" 1}", "tru", "{\"a\":1} x", "\"open", ""}) Check(!json::Parse(bad, v), std::string("json: refused ") + bad);
        std::string deep(100, '[');
        Check(!json::Parse(deep, v), "json: depth limited");
    }
    // --- telemetry (a line as the server writes it)
    {
        std::string line =
            R"(@csx status {"v":1,"state":"running","time":1794111048,"uptime":35,"map":"de_aztec","next_map":null,"frames":1040,)"
            R"("tick_hz":30.0,"frame_ms":0.22,"frame_ms_max":0.28,"max_players":65,"humans":1,"bots":1,"port":27015,"key":"9c12","password":false,)"
            R"("lan_only":true,"game_mode":"classic","net":{"connections":1,"in_bps":1935,"out_bps":null,"in_total":58627,"out_total":99033},)"
            R"("players":[{"slot":1,"name":"empty","team":"CT","score":3,"deaths":1,"ping":33,"bot":false,"address":"127.0.0.2:52670","key":"32fc","seconds":30},)"
            R"({"slot":2,"name":"Nate","team":"T","score":0,"deaths":2,"ping":null,"bot":true,"seconds":12}]})";
        Status s;
        Check(ParseStatusLine(line, s) && s.valid, "telemetry: a status line");
        Check(s.map == "de_aztec" && s.next_map.empty() && s.tick_hz == 30 && s.max_players == 65 && s.lan_only, "telemetry: fields");
        Check(s.net_valid && s.in_bps && *s.in_bps == 1935 && !s.out_bps, "telemetry: null traffic stays unset");
        Check(s.players.size() == 2 && s.players[0].ping && *s.players[0].ping == 33 && s.players[0].team == "CT" &&
                  s.players[0].score == 3 && s.players[0].deaths == 1 && s.players[0].address == "127.0.0.2:52670",
              "telemetry: a human");
        Check(s.players[1].bot && !s.players[1].ping && s.players[1].deaths == 2, "telemetry: a bot has no ping");
        Check(!ParseStatusLine("[   1.0] [info] [t1] server: ready", s), "telemetry: a log line is not status");
        Check(!ParseStatusLine("@csx status {\"v\":2}", s), "telemetry: another version is ignored");
        Check(!ParseStatusLine("@csx status {broken", s), "telemetry: broken JSON is ignored");
    }
    // --- logs
    {
        Check(LineSeverity("[   25.009] [warn] [tf41a] x") == Severity::Warn, "logs: warn");
        Check(LineSeverity("[   25.009] [error] [tf41a] x") == Severity::Error, "logs: error");
        Check(LineSeverity("[   25.009] [info] [tf41a] x") == Severity::Info, "logs: info");
        Check(LineSeverity("csx-server: --bots: 99 is outside 0..65") == Severity::Error, "logs: the server's refusal");
        ConsoleBuffer b(3);
        for (int i = 0; i < 5; i++) b.Add(std::to_string(i), Severity::Info, 0);
        Check(b.Lines().size() == 3 && b.Lines().front().text == "2" && b.Dropped() == 2 && b.LastId() == 5, "logs: bounded console");
        Check(ContainsNoCase("Server: READY", "ready") && !ContainsNoCase("abc", "x"), "logs: search ignores case");
        Check(FormatDuration(3725) == "01:02:05" && FormatDuration(90061) == "1d 01:01:01", "logs: durations");
    }
    // --- the crash policy
    {
        RestartPolicy p;
        p.enabled = true, p.max_attempts = 3, p.window_s = 600, p.base_delay_s = 5;
        RestartTracker t;
        Check(t.OnCrash(p, 100) == 5 && t.OnCrash(p, 110) == 10 && t.OnCrash(p, 130) == 20, "restart: delays double");
        Check(t.OnCrash(p, 140) < 0, "restart: gives up after max_attempts in the window");
        RestartTracker t2;
        t2.OnCrash(p, 0);
        Check(t2.OnCrash(p, 1000) == 5, "restart: crashes outside the window are forgotten");
        p.enabled = false;
        RestartTracker t3;
        Check(t3.OnCrash(p, 0) < 0, "restart: off never restarts");
    }
    // --- sha256 (FIPS 180-4 examples)
    {
        Sha256 a;
        a.Update("abc", 3);
        Check(a.HexDigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: abc");
        Sha256 b;
        std::string m(1000000, 'a');
        b.Update(m.data(), m.size());
        Check(b.HexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256: a million a");
    }
    // --- storage, profiles, rotations
    fs::path tmp = fs::temp_directory_path() / ("csx_tool_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    {
        Paths p = ResolvePaths(tmp.string(), (tmp / "pref").string());
        Check(p.portable && p.data == (tmp / "csx-server-tool-data").string(), "storage: portable data beside the tool");
        Ini ini;
        ini.Set("a", "x=y");
        ini.SetInt("n", 42);
        ini.SetBool("b", true);
        ini.SetDouble("d", 2.5);
        Check(ini.Save((tmp / "t.ini").string()), "storage: ini saved");
        Ini back;
        Check(back.Load((tmp / "t.ini").string()) && back.Get("a") == "x=y" && back.GetInt("n", 0) == 42 && back.GetBool("b", false) &&
                  back.GetDouble("d", 0) == 2.5 && back.GetInt("missing", 7) == 7,
              "storage: ini read back");
        server::Config c;
        c.name = "Friday night";
        c.password = "secret";
        c.bots = 20;
        c.map_rotation = "de_dust2,de_aztec";
        Check(SaveProfileConfig(p, "Friday", c), "profile: saved");
        server::Config r;
        std::vector<server::Error> errors;
        Check(LoadProfileConfig(p, "Friday", r, errors) && errors.empty(), "profile: loads without errors");
        Check(server::Describe(r, true) == server::Describe(c, true), "profile: the same settings back (password too)");
#ifndef _WIN32
        auto perms = fs::status(fs::path(ProfileDir(p, "Friday")) / "server.cfg").permissions();
        Check((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none, "profile: server.cfg is owner-only");
#endif
        c.bots = 3;
        SaveProfileConfig(p, "Friday", c);
        Check(fs::exists(fs::path(ProfileDir(p, "Friday")) / "server.cfg.bak"), "profile: the previous file backed up");
        Check(ListProfiles(p) == std::vector<std::string>{"Friday"}, "profile: listed");
        Check(!ValidProfileName("../x") && !ValidProfileName("") && ValidProfileName("LAN party 2"), "profile: names");
        server::Config defaults;
        SaveProfileConfig(p, "Plain", defaults);
        Check(LoadProfileConfig(p, "Plain", r, errors) && r.map_rotation.empty() && r.gungame_order.empty(),
              "profile: defaults read back (no rotation called none)");
        Touch(fs::path(ProfileDir(p, "Broken")) / "server.cfg", "port 99999\nbots many\n");
        errors.clear();
        Check(!LoadProfileConfig(p, "Broken", r, errors) && errors.size() == 2, "profile: problems reported with their lines");
        Check(SaveRotation(p, "Classic", {"de_dust2", "cs_italy"}), "rotation: saved");
        std::vector<std::string> maps;
        Check(LoadRotation(p, "Classic", maps) && maps == std::vector<std::string>{"de_dust2", "cs_italy"}, "rotation: loaded");
        Check(ListRotations(p) == std::vector<std::string>{"Classic"}, "rotation: listed");
        Check(SplitList("a, b,,c") == std::vector<std::string>{"a", "b", "c"} && JoinList({"a", "b"}) == "a,b", "lists");
        Check(DeleteProfile(p, "Broken") && !fs::exists(ProfileDir(p, "Broken")), "profile: deleted");
    }
    // --- the game folder and its maps (a fake folder: names only, no game data)
    {
        fs::path g = tmp / "game";
        for (const char* f : {"FrontEnd.SXWad", "CacheTree-English.SXWad", "de_dust2.SXWad", "cs_italy.SXWad", "Readme.txt"}) Touch(g / f);
        fs::create_directories(g / "Media");
        auto maps = ScanMaps(g.string(), "");
        Check(maps.size() == 2 && maps[0].name == "cs_italy" && maps[1].name == "de_dust2", "maps: the game folder's, sorted, front end and languages left out");
        fs::path imp = tmp / "maps";
        Touch(imp / "de_new.SXWad");
        Touch(imp / "de_new.json", "{\"playable\": true}");
        Touch(imp / "de_bad.SXWad");
        Touch(imp / "de_bad.json", "{\"playable\": false}");
        maps = ScanMaps(g.string(), imp.string());
        Check(maps.size() == 3 && maps[2].name == "de_new" && maps[2].imported && !maps[1].imported, "maps: playable imports added, others not");
        GameCheck c = CheckGameFolder(g.string(), false);
        Check(!c.ok && !c.problems.empty() && c.problems[0].find("default.xbe") != std::string::npos, "game check: no default.xbe");
        Touch(g / "default.xbe", std::string(3973120, '\0'));
        c = CheckGameFolder(g.string(), true);
        Check(!c.ok && c.problems.size() == 1 && c.problems[0].find("not the supported version") != std::string::npos,
              "game check: a wrong default.xbe is refused by its hash");
        Check(!CheckGameFolder((tmp / "nope").string(), false).ok && !CheckGameFolder("", false).ok, "game check: missing folders");
        Check(ValidMapName("de_dust2") && !ValidMapName("De_Dust2") && !ValidMapName("de dust") && !ValidMapName(""), "maps: names");
    }
    // --- how settings apply
    Check(HowApplied("friendly_fire") == Apply::Live && HowApplied("map_rotation") == Apply::Live && HowApplied("bots") == Apply::NextMap &&
              HowApplied("port") == Apply::Restart && HowApplied("max_players") == Apply::Restart,
          "settings: live, next map, restart");
    fs::remove_all(tmp);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
