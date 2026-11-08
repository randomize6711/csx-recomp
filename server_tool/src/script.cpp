#include "script.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

#include "controller.h"

namespace tool {

namespace {

bool Compare(double a, const std::string& op, double b) {
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    return false;
}

bool Field(const Controller& c, const std::string& f, std::string& s, double& n) {
    const Status& st = c.status;
    n = 0;
    s.clear();
    if (f == "state") return s = StateName(c.state), true;
    if (!st.valid) return false;
    if (f == "map") return s = st.map, true;
    if (f == "next_map") return s = st.next_map, true;
    if (f == "bots") return n = st.bots, s = std::to_string(st.bots), true;
    if (f == "humans") return n = st.humans, s = std::to_string(st.humans), true;
    if (f == "players") return n = st.humans + st.bots, s = std::to_string(st.humans + st.bots), true;
    if (f == "max_players") return n = st.max_players, s = std::to_string(st.max_players), true;
    if (f == "tick_hz") return n = st.tick_hz, s = std::to_string(st.tick_hz), true;
    if (f == "port") return n = st.port, s = std::to_string(st.port), true;
    if (f == "uptime") return n = st.uptime, s = std::to_string(st.uptime), true;
    return false;
}

bool Holds(const Controller& c, const std::string& f, const std::string& op, const std::string& v) {
    std::string s;
    double n;
    if (!Field(c, f, s, n)) return false;
    char* end = nullptr;
    double want = std::strtod(v.c_str(), &end);
    if (end && *end == 0 && !v.empty() && f != "map" && f != "next_map" && f != "state") return Compare(n, op, want);
    return op == "==" ? s == v : op == "!=" ? s != v : false;
}

}  // namespace

int RunScript(const std::string& path, const Paths& paths, const std::string& tool_dir) {
    std::ifstream in(path);
    if (!in) return std::fprintf(stderr, "cannot read %s\n", path.c_str()), 2;
    Controller c;
    c.paths = paths;
    c.server_exe = FindServerExe(tool_dir);
    c.profile = "Default";
    int failures = 0, number = 0;
    auto pump = [&](double seconds, auto until) {
        double end = c.clock() + seconds;
        while (c.clock() < end) {
            c.Update();
            if (until()) return true;
            SDL_Delay(20);
        }
        c.Update();
        return until();
    };
    auto seen = [&](const std::string& text) {
        for (const auto& l : c.console.Lines())
            if (l.text.find(text) != std::string::npos) return true;
        return false;
    };
    auto report = [&](bool ok, const std::string& what) {
        std::printf("%s line %d: %s\n", ok ? "ok  " : "FAIL", number, what.c_str());
        std::fflush(stdout);
        if (!ok) failures++;
    };
    for (std::string line; std::getline(in, line);) {
        number++;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream w(line);
        std::string cmd, rest;
        w >> cmd;
        std::getline(w, rest);
        rest.erase(0, rest.find_first_not_of(' '));
        std::vector<std::string> a;
        for (std::istringstream r(rest); r >> line;) a.push_back(line);
        auto arg = [&](size_t i, const std::string& d = {}) { return i < a.size() ? a[i] : d; };
        double timeout = 60;

        if (cmd == "game") {
            // GAME: GAME_DIR, else the repository build's game folder beside the tool's (build/gamedata)
            c.game_dir = rest;
            if (rest == "GAME") c.game_dir = std::getenv("GAME_DIR") ? std::getenv("GAME_DIR") : tool_dir + "/../gamedata";
        }
        else if (cmd == "exe") c.server_exe = rest;
        else if (cmd == "profile") {
            c.profile = rest;
            if (ListProfiles(paths).empty() || !c.LoadProfile(rest)) {
                c.config = server::Config();
                c.start_problems.clear();
            }
            report(c.SaveProfile(), "profile " + rest);
        } else if (cmd == "set") {
            std::string error;
            std::string value = rest.substr(std::min(rest.size(), arg(0).size() + 1));
            report(server::Set(c.config, arg(0), value, error), "set " + arg(0) + " " + value + (error.empty() ? "" : ": " + error));
        } else if (cmd == "save") report(c.SaveProfile(), "save");
        else if (cmd == "start") {
            bool ok = c.Start();
            std::string why;
            for (auto& p : c.start_problems) why += " | " + p;
            report(ok, "start" + why);
        } else if (cmd == "expect_start_fails") {
            bool ok = !c.Start();
            std::string why;
            for (auto& p : c.start_problems) why += " | " + p;
            report(ok && !c.start_problems.empty(), "start refused" + why);
        } else if (cmd == "stop") c.Stop(), report(true, "stop");
        else if (cmd == "restart") c.Restart(), report(true, "restart");
        else if (cmd == "kill") {
#ifndef _WIN32
            report(c.Pid() > 0 && ::kill(pid_t(c.Pid()), SIGKILL) == 0, "kill -9 " + std::to_string(c.Pid()));
#else
            report(false, "kill is POSIX only");
#endif
        } else if (cmd == "restart_policy") {
            c.restart.enabled = arg(0) == "on";
            c.restart.max_attempts = std::atoi(arg(1, "3").c_str());
            c.restart.base_delay_s = std::atof(arg(2, "5").c_str());
            report(c.SaveProfile(), "restart_policy " + rest);
        } else if (cmd == "wait_online") {
            timeout = std::atof(arg(0, "120").c_str());
            report(pump(timeout, [&] { return c.state == ServerState::Online && c.status.valid && c.status.state == "running"; }),
                   "online within " + arg(0, "120") + " s (" + StateName(c.state) + " " + c.state_detail + ")");
        } else if (cmd == "wait_offline") {
            timeout = std::atof(arg(0, "60").c_str());
            report(pump(timeout, [&] { return !c.Running(); }), std::string("ended within ") + arg(0, "60") + " s (" + StateName(c.state) + ")");
        } else if (cmd == "wait_state") {
            timeout = std::atof(arg(1, "60").c_str());
            report(pump(timeout, [&] { return arg(0) == StateName(c.state); }), "state " + arg(0) + " (is " + StateName(c.state) + ")");
        } else if (cmd == "wait_line") {
            report(pump(timeout, [&] { return seen(rest); }), "line: " + rest);
        } else if (cmd == "wait_status" || cmd == "expect_status") {
            timeout = cmd == "wait_status" ? std::atof(arg(3, "60").c_str()) : 0;
            bool ok = pump(timeout, [&] { return Holds(c, arg(0), arg(1), arg(2)); });
            std::string s;
            double n;
            Field(c, arg(0), s, n);
            report(ok, arg(0) + " " + arg(1) + " " + arg(2) + " (is " + s + ")");
        } else if (cmd == "expect_line") report(seen(rest), "seen: " + rest);
        else if (cmd == "expect_no_line") report(!seen(rest), "not seen: " + rest);
        else if (cmd == "expect_meter") {
            pump(2.5, [] { return false; });
            report(c.meter.cpu_percent.has_value() && c.meter.memory_mb.has_value(),
                   "measured: cpu " + (c.meter.cpu_percent ? std::to_string(*c.meter.cpu_percent) : "n/a") + " %, memory " +
                       (c.meter.memory_mb ? std::to_string(*c.meter.memory_mb) : "n/a") + " MB");
        } else if (cmd == "expect_exit_code") report(c.last_exit_code == std::atoi(arg(0).c_str()), "exit code " + std::to_string(c.last_exit_code));
        else if (cmd == "expect_human" || cmd == "kick_human") {
            // a network player as the telemetry shows it: a ping, an address and an identity key
            const PlayerRow* h = nullptr;
            for (const auto& p : c.status.players)
                if (!p.bot && p.ping && !p.address.empty() && p.key.size() == 64) h = &p;
            std::string who = h ? "\"" + h->name + "\" slot " + std::to_string(h->slot) + " " + h->team + " ping " +
                                      std::to_string(*h->ping) + " " + h->address : std::string("none");
            if (cmd == "expect_human") report(h != nullptr, "a human: " + who);
            else report(h && c.Kick(h->slot, "kicked by the tool test"), "kick " + who);
        } else if (cmd == "command") report(c.Command(rest), "command " + rest);
        else if (cmd == "say") report(c.Say(rest), "say " + rest);
        else if (cmd == "kick") report(c.Kick(std::atoi(arg(0).c_str()), "kicked by test"), "kick " + arg(0));
        else if (cmd == "ban") report(c.Ban(std::atoi(arg(0).c_str()), std::atoi(arg(1, "0").c_str())), "ban " + rest);
        else if (cmd == "map") report(c.ChangeMap(arg(0)), "map " + arg(0));
        else if (cmd == "rotation") report(c.SetRotationLive(arg(0) == "-" ? std::vector<std::string>{} : SplitList(arg(0))), "rotation " + arg(0));
        else if (cmd == "bots") report(c.SetBotsNextMap(std::atoi(arg(0).c_str())), "bots " + arg(0));
        else if (cmd == "difficulty") report(c.SetBotDifficultyNextMap(std::atoi(arg(0).c_str())), "difficulty " + arg(0));
        else if (cmd == "sleep") pump(std::atof(arg(0, "1").c_str()), [] { return false; });
        else if (cmd == "print") {
            std::printf("     state %s (%s), map %s, next %s, %d humans %d bots / %d, tick %.1f, pid %lld\n", StateName(c.state),
                        c.state_detail.c_str(), c.status.map.c_str(), c.status.next_map.c_str(), c.status.humans, c.status.bots,
                        c.status.max_players, c.status.tick_hz, c.Pid());
        } else report(false, "unknown command " + cmd);
    }
    if (c.Running()) {
        c.Stop();
        pump(30, [&] { return !c.Running(); });
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    // the run's output for the test's log
    if (const char* dump = std::getenv("CSX_TOOL_DUMP"); dump && *dump) {
        std::ofstream f(dump);
        for (const auto& l : c.console.Lines()) f << l.text << "\n";
    }
    return failures ? 1 : 0;
}

}  // namespace tool
