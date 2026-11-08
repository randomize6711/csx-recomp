// The server tool's core, without any UI: one profile's csx-server process, its state, its telemetry, its console,
// the machine's measurements of it, crash handling and the administration commands. The GUI (ui.cpp) and the
// headless script mode (--script, tests/server_tool_test.py) drive the same object.
#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "logs.h"
#include "process.h"
#include "server/server_config.h"
#include "storage.h"
#include "sysstats.h"
#include "telemetry.h"

namespace tool {

// how a setting takes effect on a running server
enum class Apply { Live, NextMap, Restart };
Apply HowApplied(const std::string& key);

struct Sample {  // one second of history for the graphs
    float cpu = -1, memory = -1, players = 0, in_kbps = 0, out_kbps = 0, tick = 0;
};

class Controller {
public:
    Paths paths;
    std::string profile;     // its name
    server::Config config;   // as saved in the profile (what the next start uses)
    std::string game_dir;    // the user's game folder
    std::string server_exe;  // csx-server
    RestartPolicy restart;   // the profile's crash policy

    ServerState state = ServerState::Offline;
    std::string state_detail;  // "Loading de_dust2", "Exited with code 2: ...", "Restarting in 5 s"
    Status status;             // the last telemetry line (status.valid false until one arrived)
    double status_age = 1e9;   // seconds since it arrived
    ProcessMeter meter;
    ConsoleBuffer console;
    std::deque<Sample> history;  // newest last, at most 600
    std::vector<std::string> start_problems;  // why the last start was refused or failed
    double started_at = 0;     // the tool's clock when this run started
    double restart_at = -1;    // a crash restart pending at this time (the tool's clock)
    bool crash_prompt = false; // the server ended unexpectedly and no automatic restart follows: ask the user
    int last_exit_code = 0;

    // the tool's clock (seconds); a test replaces it
    std::function<double()> clock;

    Controller();
    ~Controller();
    // the profile's files read into config / restart (false: problems in start_problems)
    bool LoadProfile(const std::string& name);
    bool SaveProfile();
    // checks, then starts; false with start_problems filled
    bool Start();
    void Stop();
    void Restart();  // stop, then start again once it ended
    // call every UI frame: output, telemetry, exits, crash restarts, measurements
    void Update();

    // commands, all through the server's console (its owner's admin commands where one exists)
    bool Command(const std::string& line, bool echo = true);
    bool Say(const std::string& text);
    bool Kick(int slot, const std::string& reason);
    bool Ban(int slot, int minutes);
    bool Unban(const std::string& key_or_ip);
    bool ChangeMap(const std::string& map);
    bool RestartRound(int seconds);
    bool SetRotationLive(const std::vector<std::string>& maps);
    bool SetBotsNextMap(int bots);
    bool SetBotDifficultyNextMap(int level);
    bool SetFriendlyFireLive(bool on);
    bool SetRoundTimeLive(int minutes);
    bool SetPasswordLive(const std::string& password);  // empty: none

    bool Running() const { return proc_.Running(); }
    long long Pid() const { return proc_.Pid(); }
    double Uptime() const;
    void ToolLine(const std::string& text, Severity sev = Severity::Tool);

private:
    void OnLine(const std::string& line);
    void OnExit(const ProcessExit& ex);
    ServerProcess proc_;
    RestartTracker tracker_;
    bool restart_after_stop_ = false;
    double last_sample_ = 0;
    std::string pending_map_;  // the map being loaded (from the log), for the progress text
    std::vector<std::string> recent_errors_;
};

// the server executable beside the tool (a package) or the repository's build (development); empty when none
std::string FindServerExe(const std::string& tool_dir);
// the imported maps folder of a server executable: maps/ beside its bin folder
std::string ImportsDirFor(const std::string& server_exe);

}  // namespace tool
