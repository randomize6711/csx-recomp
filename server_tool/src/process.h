// csx-server as the tool's child process (docs/server_tool.md "IPC"): started with pipes on stdin and stdout (stderr
// merged into it), CSX_SERVER_TELEMETRY=1, in its profile's server folder. Its output is read on a thread of its own
// (never stalled by the UI); lines are handed over through a queue. Commands are lines written to its stdin. A map
// change re-executes the server in place: same process, same pipes.
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_Process;

namespace tool {

enum class ServerState { Offline, Starting, Online, Stopping, Error };
const char* StateName(ServerState s);

struct ProcessExit {
    bool happened = false;
    int code = 0;
    bool expected = false;  // the tool asked it to stop
};

class ServerProcess {
public:
    ~ServerProcess();
    // false with the reason when it could not be started
    bool Start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
               const std::vector<std::pair<std::string, std::string>>& env, std::string& error);
    // a line on its console (stdin); false when it is not running
    bool Send(const std::string& line);
    // graceful: "quit" on its console, then SIGTERM after grace_s, then a forced kill after another grace_s
    void Stop(double grace_s = 15);
    // call often (the UI's frame): reaps the process, escalates a stop; the exit once it happened
    ProcessExit Poll();
    // output lines since the last call
    std::vector<std::string> TakeLines();
    bool Running() const { return proc_ != nullptr; }
    long long Pid() const { return pid_; }
    bool Stopping() const { return stopping_; }

private:
    void ReadLoop();

    SDL_Process* proc_ = nullptr;
    long long pid_ = 0;
    std::thread reader_;
    std::atomic<bool> reader_done_{false};
    std::mutex lock_;
    std::deque<std::string> lines_;
    std::string partial_;
    bool stopping_ = false;
    int stop_stage_ = 0;
    double grace_ = 15;
    std::chrono::steady_clock::time_point stop_at_;
};

// the crash policy (Configuration > Advanced): restart an unexpected exit after a delay that doubles each time
// (base, 2x base, 4x base ...), at most max_attempts within window_s; then give up and say so
struct RestartPolicy {
    bool enabled = false;
    int max_attempts = 3;
    double window_s = 600;
    double base_delay_s = 5;
};

class RestartTracker {
public:
    // an unexpected exit at time now: the delay before restarting, or a negative value to give up
    double OnCrash(const RestartPolicy& p, double now);
    void Reset() { crashes_.clear(); }
    int RecentCrashes() const { return int(crashes_.size()); }

private:
    std::vector<double> crashes_;
};

}  // namespace tool
