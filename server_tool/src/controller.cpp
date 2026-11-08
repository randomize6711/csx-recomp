#include "controller.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "gamefiles.h"

namespace fs = std::filesystem;

namespace tool {

namespace {
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

Apply HowApplied(const std::string& key) {
    // the commands the server has for a running match (server_admin.cpp): friendly fire, round time, the password and
    // the rotation take effect at once; bots and their difficulty from the next map (the title sets bots at a
    // match's start); everything else is read when the server starts
    if (key == "friendly_fire" || key == "round_time" || key == "password" || key == "map_rotation") return Apply::Live;
    if (key == "bots" || key == "bot_difficulty") return Apply::NextMap;
    return Apply::Restart;
}

Controller::Controller() {
    clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
}

Controller::~Controller() = default;

void Controller::ToolLine(const std::string& text, Severity sev) { console.Add(text, sev, NowMs()); }

bool Controller::LoadProfile(const std::string& name) {
    start_problems.clear();
    std::vector<server::Error> errors;
    bool ok = LoadProfileConfig(paths, name, config, errors);
    for (auto& e : errors) start_problems.push_back(e.where + ": " + e.text);
    Ini ini;
    ini.Load((fs::path(ProfileDir(paths, name)) / "tool.ini").string());
    restart.enabled = ini.GetBool("auto_restart", false);
    restart.max_attempts = std::clamp(ini.GetInt("restart_max_attempts", 3), 1, 20);
    restart.window_s = std::clamp(ini.GetDouble("restart_window_s", 600), 60.0, 86400.0);
    restart.base_delay_s = std::clamp(ini.GetDouble("restart_base_delay_s", 5), 1.0, 300.0);
    profile = name;
    return ok;
}

bool Controller::SaveProfile() {
    if (profile.empty()) return false;
    Ini ini;
    ini.SetBool("auto_restart", restart.enabled);
    ini.SetInt("restart_max_attempts", restart.max_attempts);
    ini.SetDouble("restart_window_s", restart.window_s);
    ini.SetDouble("restart_base_delay_s", restart.base_delay_s);
    bool ok = SaveProfileConfig(paths, profile, config);
    return ini.Save((fs::path(ProfileDir(paths, profile)) / "tool.ini").string()) && ok;
}

bool Controller::Start() {
    start_problems.clear();
    crash_prompt = false;
    restart_at = -1;
    if (proc_.Running()) {
        start_problems.push_back("The server is already running.");
        return false;
    }
    if (profile.empty()) start_problems.push_back("No profile is selected.");
    std::error_code ec;
    if (server_exe.empty() || !fs::exists(server_exe, ec))
        start_problems.push_back("The server program (csx-server) was not found" +
                                 (server_exe.empty() ? std::string(".") : ": " + server_exe) +
                                 " Build it (tools/build_server.sh) or choose it in Tools > Server Program.");
    GameCheck g = CheckGameFolder(game_dir, false);
    for (auto& p : g.problems) start_problems.push_back(p);
    if (g.ok) {
        auto maps = ScanMaps(game_dir, ImportsDirFor(server_exe));
        if (std::none_of(maps.begin(), maps.end(), [&](const MapEntry& m) { return m.name == config.map; }))
            start_problems.push_back("The starting map " + config.map + " is not in the game folder.");
        for (const auto& m : SplitList(config.map_rotation))
            if (std::none_of(maps.begin(), maps.end(), [&](const MapEntry& e) { return e.name == m; }))
                start_problems.push_back("The rotation's map " + m + " is not in the game folder.");
    }
    std::vector<server::Error> errors;
    server::CheckTogether(config, errors);
    for (auto& e : errors) start_problems.push_back(e.text);
    if (!start_problems.empty()) {
        for (auto& p : start_problems) ToolLine("Not started: " + p, Severity::Error);
        state = ServerState::Error;
        state_detail = start_problems.front();
        return false;
    }
    if (!SaveProfile()) {
        start_problems.push_back("The profile could not be saved to " + ProfileDir(paths, profile));
        state = ServerState::Error;
        return false;
    }
    std::string dir = ProfileServerDir(paths, profile);
    fs::create_directories(dir, ec);
    std::string cfg = (fs::path(ProfileDir(paths, profile)) / "server.cfg").string();
    std::vector<std::string> args = {"--game", fs::absolute(game_dir, ec).string(), "--config", cfg};
    std::vector<std::pair<std::string, std::string>> env = {{"CSX_SERVER_TELEMETRY", "1"}};
    std::string error;
    ToolLine("Starting " + server_exe + " (profile " + profile + ", map " + config.map + ", port " +
             std::to_string(config.port) + ")");
    if (!proc_.Start(server_exe, args, dir, env, error)) {
        start_problems.push_back(error);
        ToolLine(error, Severity::Error);
        state = ServerState::Error;
        state_detail = error;
        return false;
    }
    state = ServerState::Starting;
    state_detail = "Starting the engine";
    status = Status();
    status_age = 1e9;
    started_at = clock();
    pending_map_ = config.map;
    recent_errors_.clear();
    ToolLine("Server process " + std::to_string(proc_.Pid()) + " started");
    return true;
}

void Controller::Stop() {
    restart_at = -1;
    restart_after_stop_ = false;
    if (!proc_.Running()) return;
    state = ServerState::Stopping;
    state_detail = "Telling the players and shutting down";
    ToolLine("Stopping the server (quit on its console)");
    proc_.Stop();
}

void Controller::Restart() {
    if (!proc_.Running()) {
        Start();
        return;
    }
    Stop();
    restart_after_stop_ = true;
}

double Controller::Uptime() const {
    if (status.valid && status.uptime > 0) return status.uptime;
    return proc_.Running() ? clock() - started_at : 0;
}

void Controller::OnLine(const std::string& line) {
    Status s;
    if (ParseStatusLine(line, s)) {
        status = std::move(s);
        status_age = 0;
        if (status.state == "running" && state == ServerState::Starting) {
            state = ServerState::Online;
            state_detail.clear();
        } else if (status.state == "loading" && state == ServerState::Online) {
            state = ServerState::Starting;
            state_detail = "Loading " + (pending_map_.empty() ? std::string("the next map") : pending_map_);
        }
        return;
    }
    Severity sev = LineSeverity(line);
    console.Add(line, sev, NowMs());
    if (sev == Severity::Error) {
        recent_errors_.push_back(line);
        if (recent_errors_.size() > 20) recent_errors_.erase(recent_errors_.begin());
    }
    // the startup's stages, from the server's own log lines
    auto has = [&](const char* s) { return line.find(s) != std::string::npos; };
    if (has("dedicated: the engine loads")) state_detail = "Loading the engine";
    else if (has("net: server identity")) state_detail = "Network ready, loading " + pending_map_;
    else if (has("net: relaunching into ")) {
        size_t a = line.find("net: relaunching into ") + 22;
        pending_map_ = line.substr(a, line.find_first_of(": ", a) - a);
        state = ServerState::Starting;
        state_detail = "Changing map to " + pending_map_;
    } else if (has("server: ready: map ")) {
        size_t a = line.find("server: ready: map ") + 19;
        pending_map_ = line.substr(a, line.find(' ', a) - a);
        if (state == ServerState::Starting) state = ServerState::Online, state_detail.clear();
    }
}

void Controller::OnExit(const ProcessExit& ex) {
    last_exit_code = ex.code;
    meter.Sample(0);
    status.valid = false;
    if (ex.expected) {
        ToolLine("The server stopped (exit code " + std::to_string(ex.code) + ")");
        state = ServerState::Offline;
        state_detail.clear();
        if (restart_after_stop_) {
            restart_after_stop_ = false;
            Start();
        }
        return;
    }
    // the server ended on its own: a clean quit typed on its console is not a crash
    bool clean = ex.code == 0;
    std::string why = clean ? "The server quit (exit code 0)" : "The server ended unexpectedly (exit code " + std::to_string(ex.code) + ")";
    if (!recent_errors_.empty()) why += ": " + recent_errors_.back();
    ToolLine(why, clean ? Severity::Tool : Severity::Error);
    if (clean) {
        state = ServerState::Offline;
        state_detail.clear();
        return;
    }
    state = ServerState::Error;
    state_detail = why;
    // a server that never got going (bad settings, exit 2 before it ran) is not restarted: it would fail again
    bool ran = clock() - started_at > 10 && ex.code != 2;
    double delay = ran ? tracker_.OnCrash(restart, clock()) : -1;
    if (delay >= 0) {
        restart_at = clock() + delay;
        ToolLine("Restarting in " + std::to_string(int(delay)) + " s (attempt " + std::to_string(tracker_.RecentCrashes()) +
                 " of " + std::to_string(restart.max_attempts) + ")", Severity::Warn);
    } else {
        crash_prompt = true;
        if (restart.enabled && ran)
            ToolLine("Not restarting: " + std::to_string(tracker_.RecentCrashes()) + " crashes within " +
                     std::to_string(int(restart.window_s / 60)) + " minutes", Severity::Error);
    }
}

void Controller::Update() {
    for (const auto& l : proc_.TakeLines()) OnLine(l);
    ProcessExit ex = proc_.Poll();
    if (ex.happened) {
        for (const auto& l : proc_.TakeLines()) OnLine(l);
        OnExit(ex);
    }
    if (restart_at >= 0 && clock() >= restart_at && !proc_.Running()) {
        restart_at = -1;
        ToolLine("Restarting the server after the crash", Severity::Warn);
        Start();
    }
    if (restart_at >= 0) state_detail = "Restarting in " + std::to_string(int(restart_at - clock()) + 1) + " s";
    double now = clock();
    if (now - last_sample_ >= 1.0) {
        double dt = last_sample_ > 0 ? now - last_sample_ : 1.0;
        last_sample_ = now;
        status_age += dt;
        meter.Sample(proc_.Pid());
        Sample s;
        if (meter.cpu_percent) s.cpu = float(*meter.cpu_percent);
        if (meter.memory_mb) s.memory = float(*meter.memory_mb);
        if (status.valid && status_age < 5) {
            s.players = float(status.humans + status.bots);
            s.in_kbps = float(status.in_bps.value_or(0) / 1024.0);
            s.out_kbps = float(status.out_bps.value_or(0) / 1024.0);
            s.tick = float(status.tick_hz);
        }
        if (proc_.Running()) {
            history.push_back(s);
            while (history.size() > 600) history.pop_front();
        }
    }
}

bool Controller::Command(const std::string& line, bool echo) {
    if (line.empty()) return false;
    if (!proc_.Running()) {
        ToolLine("Not sent (the server is not running): " + line, Severity::Warn);
        return false;
    }
    if (echo) ToolLine("> " + line);
    return proc_.Send(line);
}

namespace {
std::string CleanText(std::string s, size_t max) {  // as the server cleans it: no quotes, separators or breaks
    for (char& c : s)
        if (c == '"' || c == ';' || c == '\n' || c == '\r' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = ' ';
    if (s.size() > max) s.resize(max);
    return s;
}
}  // namespace

bool Controller::Say(const std::string& text) { return !text.empty() && Command("admin say " + CleanText(text, 120)); }
bool Controller::Kick(int slot, const std::string& reason) {
    return Command("admin kick " + std::to_string(slot) + (reason.empty() ? "" : " " + CleanText(reason, 60)));
}
bool Controller::Ban(int slot, int minutes) {
    return Command("admin ban " + std::to_string(slot) + " " + std::to_string(std::clamp(minutes, 0, 525600)));
}
bool Controller::Unban(const std::string& who) { return Command("admin unban " + CleanText(who, 64)); }
bool Controller::ChangeMap(const std::string& map) { return ValidMapName(map) && Command("admin map " + map); }
bool Controller::RestartRound(int seconds) { return Command("admin restart " + std::to_string(std::clamp(seconds, 1, 15))); }
bool Controller::SetRotationLive(const std::vector<std::string>& maps) {
    for (const auto& m : maps)
        if (!ValidMapName(m)) return false;
    return Command("admin rotation " + (maps.empty() ? std::string("-") : JoinList(maps)));
}
bool Controller::SetBotsNextMap(int bots) { return Command("admin bots " + std::to_string(std::max(0, bots))); }
bool Controller::SetBotDifficultyNextMap(int level) { return Command("admin bot_difficulty " + std::to_string(std::clamp(level, 0, 3))); }
bool Controller::SetFriendlyFireLive(bool on) { return Command(std::string("admin friendly_fire ") + (on ? "on" : "off")); }
bool Controller::SetRoundTimeLive(int minutes) { return Command("admin round_time " + std::to_string(std::clamp(minutes, 1, 9))); }
bool Controller::SetPasswordLive(const std::string& pw) {
    if (!proc_.Running()) return false;
    ToolLine(std::string("> admin password ") + (pw.empty() ? "-" : "(hidden)"));
    return proc_.Send("admin password " + (pw.empty() ? std::string("-") : pw));
}

std::string FindServerExe(const std::string& tool_dir) {
#ifdef _WIN32
    const char* name = "csx-server.exe";
#else
    const char* name = "csx-server";
#endif
    std::error_code ec;
    fs::path d = tool_dir;
    for (fs::path c : {d / name, d / ".." / "server" / name, d / ".." / "build" / "server" / name,
                       d / ".." / ".." / "build" / "server" / name, d / ".." / ".." / ".." / "build" / "server" / name})
        if (fs::is_regular_file(c, ec)) return fs::weakly_canonical(c, ec).string();
    return {};
}

std::string ImportsDirFor(const std::string& server_exe) {
    if (server_exe.empty()) return {};
    std::error_code ec;
    fs::path d = fs::path(server_exe).parent_path().parent_path() / "maps";
    return fs::is_directory(d, ec) ? d.string() : std::string();
}

}  // namespace tool
