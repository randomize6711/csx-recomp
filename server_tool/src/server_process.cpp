#include "server_process.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace tool {

const char* StateName(ServerState s) {
    switch (s) {
    case ServerState::Offline: return "Offline";
    case ServerState::Starting: return "Starting";
    case ServerState::Online: return "Online";
    case ServerState::Stopping: return "Stopping";
    case ServerState::Error: return "Error";
    }
    return "?";
}

ServerProcess::~ServerProcess() {
    if (proc_) {
        Stop(5);
        // the tool is closing: wait for the server's own quit, then make sure it is gone
        for (int i = 0; i < 400 && proc_; i++) {
            Poll();
            SDL_Delay(25);
        }
        if (proc_) {
            SDL_KillProcess(proc_, true);
            SDL_WaitProcess(proc_, true, nullptr);
        }
    }
    if (reader_.joinable()) reader_.join();
    if (proc_) SDL_DestroyProcess(proc_);
}

bool ServerProcess::Start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
                          const std::vector<std::pair<std::string, std::string>>& env, std::string& error) {
    if (proc_) return error = "the server is already running", false;
    if (reader_.joinable()) reader_.join();
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Environment* e = SDL_CreateEnvironment(true);
    for (const auto& [k, v] : env) SDL_SetEnvironmentVariable(e, k.c_str(), v.c_str(), true);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, e);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    proc_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(e);
    if (!proc_) return error = std::string("cannot start ") + exe + ": " + SDL_GetError(), false;
    pid_ = (long long)SDL_GetNumberProperty(SDL_GetProcessProperties(proc_), SDL_PROP_PROCESS_PID_NUMBER, 0);
    stopping_ = false;
    stop_stage_ = 0;
    reader_done_ = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        partial_.clear();
    }
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
}

void ServerProcess::ReadLoop() {
    SDL_IOStream* out = SDL_GetProcessOutput(proc_);
    char buf[16384];
    for (;;) {
        size_t n = out ? SDL_ReadIO(out, buf, sizeof(buf)) : 0;
        if (n > 0) {
            std::lock_guard<std::mutex> l(lock_);
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
                    lines_.push_back(std::move(partial_));
                    partial_.clear();
                } else if (partial_.size() < 64 * 1024) {
                    partial_ += buf[i];
                }
            }
            // a UI that does not take its lines for a long time (it cannot happen while it runs) keeps the newest
            while (lines_.size() > 200000) lines_.pop_front();
            continue;
        }
        if (!out || SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;  // end of output: the process ended
        SDL_Delay(10);
    }
    std::lock_guard<std::mutex> l(lock_);
    if (!partial_.empty()) lines_.push_back(std::move(partial_)), partial_.clear();
    reader_done_ = true;
}

bool ServerProcess::Send(const std::string& line) {
    if (!proc_) return false;
    SDL_IOStream* in = SDL_GetProcessInput(proc_);
    if (!in) return false;
    std::string l = line;
    l.erase(std::remove_if(l.begin(), l.end(), [](char c) { return c == '\n' || c == '\r' || c == '\0'; }), l.end());
    l += "\n";
    return SDL_WriteIO(in, l.data(), l.size()) == l.size() && SDL_FlushIO(in);
}

void ServerProcess::Stop(double grace_s) {
    if (!proc_ || stopping_) return;
    stopping_ = true;
    stop_stage_ = 0;
    grace_ = grace_s;
    stop_at_ = std::chrono::steady_clock::now();
    Send("quit");  // the server's own quit: the players are told, the logs closed
}

ProcessExit ServerProcess::Poll() {
    ProcessExit ex;
    if (!proc_) return ex;
    if (stopping_) {
        double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - stop_at_).count();
        if (stop_stage_ == 0 && waited > grace_) {
            stop_stage_ = 1;
            SDL_KillProcess(proc_, false);  // SIGTERM: the server quits as it does on its console
        } else if (stop_stage_ == 1 && waited > 2 * grace_) {
            stop_stage_ = 2;
            SDL_KillProcess(proc_, true);
        }
    }
    int code = 0;
    if (!SDL_WaitProcess(proc_, false, &code)) return ex;
    // the output is read to its end before the exit is reported, so the last lines are not lost
    if (reader_.joinable()) reader_.join();
    SDL_DestroyProcess(proc_);
    proc_ = nullptr;
    ex.happened = true;
    ex.code = code;
    ex.expected = stopping_;
    stopping_ = false;
    pid_ = 0;
    return ex;
}

std::vector<std::string> ServerProcess::TakeLines() {
    std::lock_guard<std::mutex> l(lock_);
    std::vector<std::string> out(std::make_move_iterator(lines_.begin()), std::make_move_iterator(lines_.end()));
    lines_.clear();
    return out;
}

double RestartTracker::OnCrash(const RestartPolicy& p, double now) {
    crashes_.erase(std::remove_if(crashes_.begin(), crashes_.end(), [&](double t) { return now - t > p.window_s; }),
                   crashes_.end());
    crashes_.push_back(now);
    if (!p.enabled || int(crashes_.size()) > p.max_attempts) return -1;
    return p.base_delay_s * std::pow(2.0, double(crashes_.size() - 1));
}

}  // namespace tool
