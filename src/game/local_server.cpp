// The local server (local_server.h; N4.7, docs/splitscreen.md 17). The game starts csx-server for a match from its
// menus and joins it like any server; the listen server is the fallback (PC setting server_process 0).
//   start   the title's relaunch into the menus' match (net_session.cpp RelaunchNotify): csx-server is started in
//           local_server/ beside the player (its own logs, identity, emulated disk; a copy of the player's PC
//           settings, so it runs the match as the game's own server did), with the menus' match line, and the game
//           relaunches into "+connect" to it instead of "+map" once it reports ready. Offline on 127.0.0.1 and a free
//           port; Host a Match on the PC port (net_port, 27015) for the network, found on the LAN like any server.
//   owner   the game's identity, given to the server: the pause menu's host actions (Restart Match, Change Map, the
//           pause) and the server's console commands typed in the game go to it as pc_owner commands
//           (server_admin.cpp), which only that identity may send.
//   stop    the match ends (the game leaves it, starts another, returns to its menus, quits): SIGTERM (the players
//           told), SIGKILL after 5 s, reaped. A game that ended any other way leaves none: the server quits when its
//           parent is gone (server_main.cpp). A server that ended by itself sends the game back to its menus with why.
// What persists across the title's relaunches (the game's own process, its id kept): CSX_LOCAL_SERVER "PID ADDRESS
// KEY", in the environment the relaunch keeps.
#include "game/local_server.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/transport.h"

namespace fs = std::filesystem;

namespace {

struct Running {
    int pid = 0;
    std::string address, key;
};

// this game's local server, from the environment (it outlives the title's relaunches)
Running Current() {
    Running r;
    const char* e = std::getenv("CSX_LOCAL_SERVER");
    if (!e) return r;
    std::istringstream in(e);
    in >> r.pid >> r.address >> r.key;
    if (r.pid <= 1 || r.address.empty() || r.key.size() != 64) r = Running{};
    return r;
}

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) out += d[c >> 4], out += d[c & 15];
    return out;
}

#ifndef _WIN32
// csx-server: CSX_SERVER_EXE, else beside the game's program, else build/server/ beside build/window/ (a build tree)
std::string ServerExe() {
    if (const char* e = std::getenv("CSX_SERVER_EXE"); e && *e) return ::access(e, X_OK) == 0 ? e : "";
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    for (fs::path p : {self.parent_path() / "csx-server", self.parent_path().parent_path() / "server" / "csx-server"})
        if (::access(p.c_str(), X_OK) == 0) return p.string();
    return {};
}

// a UDP port on 127.0.0.1 for an offline match's server: the PC port as a listen server's was, unless something holds
// it (another server), then any free one
int FreeLoopbackPort(int preferred) {
    int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(uint16_t(preferred));
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        ::close(s);
        return preferred;
    }
    a.sin_port = 0;
    socklen_t n = sizeof(a);
    int port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &n) == 0)
        port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

std::string ExitText(int status) {
    if (WIFEXITED(status)) return "exit code " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::string("signal ") + (sigabbrev_np(WTERMSIG(status)) ? sigabbrev_np(WTERMSIG(status)) : "?");
    return "status " + std::to_string(status);
}
#endif

}  // namespace

namespace csx {

bool LocalServerWanted() {
#ifdef _WIN32
    return false;  // Linux first: the Windows port starts its process when it is approved
#else
    if (Dedicated() || SplitPlayer() > 1) return false;
    const char* e = std::getenv("CSX_SERVER_PROCESS");
    bool want = e && *e ? *e == '1' : pc::Get("server_process", "1") == "1";
    if (want && pc::Get("net_identity", "1") != "1") {
        // the owner's host actions are authenticated by its identity: without one, the listen server
        LOG_FIRST(1, "local server: not used: the player has no identity (PC setting net_identity 0); the match runs in the game");
        return false;
    }
    return want;
#endif
}

bool LocalServerStart(const std::string& match_line, bool host, std::string& connect_line, std::string& error) {
#ifdef _WIN32
    (void)match_line, (void)host, (void)connect_line;
    error = "not available on this system";
    return false;
#else
    auto t0 = std::chrono::steady_clock::now();
    std::string exe = ServerExe();
    if (exe.empty()) {
        error = "The match's server program (csx-server) was not found beside the game.";
        std::error_code ec;
        LOGE("local server: csx-server not found (beside %s, or CSX_SERVER_EXE)", fs::read_symlink("/proc/self/exe", ec).c_str());
        return false;
    }
    const char* game = std::getenv("GAME_DIR");
    if (!game || !*game) {
        error = "The game folder is not known (GAME_DIR).";
        return false;
    }
    std::string idmsg;
    auto id = net::Identity::LoadOrCreate((app::ExeDir() / "identity.key").string(), idmsg);
    if (!id) {
        error = "The player's identity could not be read: " + idmsg;
        return false;
    }
    std::string owner_key = net::Identity::Fingerprint(id->public_key);
    std::string map = match_line.substr(5, match_line.find_first_of(std::string(" \0", 2), 5) - 5);

    // where it listens: offline on 127.0.0.1 (the PC port if free, else any); hosted on the PC port, on every address (a test's
    // numbered instance or CSX_NET_BIND: that address, as the listen server bound)
    int port = host ? NetServerPort() : FreeLoopbackPort(NetServerPort());
    std::string ip = "127.0.0.1";
    if (host && app::Instance() > 0) ip = "127.0.0." + std::to_string(app::Instance());
    if (const char* b = std::getenv("CSX_NET_BIND"); host && b && *b) ip = b;
    if (port <= 0) {
        error = "No free network port was found for the match's server.";
        return false;
    }

    // its folder: logs, its identity (kept: its key is the same in every match), its disk; the player's PC settings
    fs::path dir = app::ExeDir() / "local_server";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::copy_file(app::ExeDir() / "pc_settings.ini", dir / "pc_settings.ini", fs::copy_options::overwrite_existing, ec);
    if (ec && fs::exists(app::ExeDir() / "pc_settings.ini")) LOGW("local server: the PC settings not copied: %s", ec.message().c_str());
    if (!fs::exists(app::ExeDir() / "pc_settings.ini")) fs::remove(dir / "pc_settings.ini", ec);
    fs::path status = dir / "status";
    fs::remove(status, ec);

    std::vector<std::string> args = {exe, "--game", game, "--port", std::to_string(port), "--map", map, "--max-players", "65"};
    if (std::string name = pc::Get("net_server_name", ""); !name.empty()) args.insert(args.end(), {"--name", name});
    if (host && pc::Get("net_lan_only", "0") == "1") args.insert(args.end(), {"--lan-only", "on"});
    if (host && pc::Get("net_master_register", "0") == "1" && !pc::Get("net_master", "").empty())
        args.insert(args.end(), {"--master", pc::Get("net_master", "")});

    // its environment: the game's, without what is the game's own (its compositor socket and split-screen role, a
    // join's server key and password, the menus' hosting flags), with the match and the owner
    std::vector<std::string> env;
    static const char* const kDropped[] = {"RELAUNCH_KEEP_FD=", "CSX_SPLIT_", "CSX_LOCAL_SERVER", "CSX_BROWSER_",
                                           "CSX_SERVER_", "CSX_NET_PASSWORD=", "CSX_PLAYER_NAME=", "CSX_MENU_HOST=",
                                           "CSX_DEDICATED=", "CSX_AUDIO_WAV="};
    for (char** e = environ; *e; e++) {
        std::string v = *e;
        bool drop = false;
        for (const char* d : kDropped) drop = drop || v.rfind(d, 0) == 0;
        // a profile (CSX_PROFILE=FILE) goes to the server's own folder, not over the game's
        if (v.rfind("CSX_PROFILE=", 0) == 0) v = "CSX_PROFILE=" + (dir / "threads.txt").string();
        if (!drop) env.push_back(v);
    }
    std::string line = match_line.substr(0, match_line.find('\0'));
    env.push_back("CSX_SERVER_MATCH=" + line);
    env.push_back("CSX_SERVER_STATUS=" + status.string());
    env.push_back("CSX_SERVER_OWNER_PID=" + std::to_string(getpid()));
    env.push_back("CSX_SERVER_OWNER_KEY=" + owner_key);
    if (!host) env.push_back("CSX_SERVER_LOOPBACK=1");
    std::vector<char*> argv, envp;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (auto& v : env) envp.push_back(v.data());
    envp.push_back(nullptr);
    std::string cwd = dir.string();

    LOGI("local server: starting %s for %s (%s, %s:%d)", exe.c_str(), map.c_str(), host ? "Host a Match: the network's"
                                                                                       : "offline: this machine's", ip.c_str(), port);
    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("The match's server could not be started: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // only what is safe after a fork in a threaded process: its folder, no console input, none of the game's
        // descriptors (its window, GPU, compositor socket), then the server
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) ::dup2(null, 0);
        ::syscall(SYS_close_range, 3u, ~0u, 0u);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    // ready when it says so (its map loaded, its port open): the title's relaunch waits here, as its own match load
    // would; an exit or an error in the status is the player's message
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::string text;
        {
            std::ifstream f(status);
            std::getline(f, text);
        }
        if (text.rfind("ready ", 0) == 0) {
            std::istringstream in(text.substr(6));
            int p = 0;
            std::string key;
            in >> p >> key;
            if (p != port || key.size() != 64) {
                error = "The match's server reported an unexpected state.";
                LOGE("local server: status \"%s\" (port %d expected)", text.c_str(), port);
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                return false;
            }
            std::string address = ip + ":" + std::to_string(port);
            SetEnv("CSX_LOCAL_SERVER", std::to_string(pid) + " " + address + " " + key);
            connect_line = "+connect " + address;
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            LOGI("local server: ready in %.1f s: process %d, %s, server identity %s; the game joins it", s, int(pid),
                 address.c_str(), key.c_str());
            return true;
        }
        int st = 0;
        bool ended = waitpid(pid, &st, WNOHANG) == pid;
        bool late = std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60);
        if (text.rfind("error ", 0) == 0 || ended || late) {
            std::string why = text.rfind("error ", 0) == 0 ? text.substr(6)
                              : ended                     ? "it ended (" + ExitText(st) + ")"
                                                          : "it did not start within 60 seconds";
            LOGE("local server: not started: %s (its log: %s)", why.c_str(), (dir / "logs").c_str());
            if (!ended) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
            error = "The match's server did not start: " + why;
            if (why.find("UDP port") != std::string::npos && host) error += " (another server on this port?)";
            return false;
        }
    }
#endif
}

void LocalServerStop(const char* why) {
#ifndef _WIN32
    Running r = Current();
    SetEnv("CSX_LOCAL_SERVER", "");
    std::error_code ec;
    fs::remove(app::ExeDir() / "local_server" / "status", ec);  // not ready any more (tools/drive.py reads it)
    if (!r.pid) return;
    int st = 0;
    if (waitpid(r.pid, &st, WNOHANG) == r.pid) {
        LOGI("local server: %s; it had already ended (%s)", why, ExitText(st).c_str());
        return;
    }
    // SIGTERM: its quit tells its players (csx-server's handler); after 5 s SIGKILL; reaped either way
    kill(r.pid, SIGTERM);
    auto t0 = std::chrono::steady_clock::now();
    bool gone = false;
    while (!gone && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gone = waitpid(r.pid, &st, WNOHANG) == r.pid;
    }
    if (!gone) {
        LOGW("local server: process %d still running 5 s after SIGTERM: SIGKILL", r.pid);
        kill(r.pid, SIGKILL);
        waitpid(r.pid, &st, 0);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("local server: stopped (%s): process %d ended in %.2f s (%s)", why, r.pid, s, ExitText(st).c_str());
#else
    (void)why;
#endif
}

bool LocalServerRunning() { return Current().pid != 0; }

std::string LocalServerAddress() { return Current().address; }

bool LocalServerTrusted(const std::string& address, const std::string& key) {
    Running r = Current();
    return r.pid && r.address == address && r.key == key;
}

bool LocalServerAlive(std::string& why) {
#ifndef _WIN32
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next) return true;
    next = now + std::chrono::milliseconds(250);
    Running r = Current();
    if (!r.pid) return true;
    int st = 0;
    if (waitpid(r.pid, &st, WNOHANG) != r.pid) return true;
    why = ExitText(st);
    SetEnv("CSX_LOCAL_SERVER", "");
    std::error_code ec;
    fs::remove(app::ExeDir() / "local_server" / "status", ec);
    LOGE("local server: process %d ended by itself (%s); its log: %s", r.pid, why.c_str(),
         (app::ExeDir() / "local_server" / "logs").c_str());
    return false;
#else
    (void)why;
    return true;
#endif
}

bool LocalServerRoute(const std::string& line) {
    Running r = Current();
    if (!r.pid || !NetConnected(r.address)) return false;
    std::string word = line.substr(0, line.find_first_of(" \t\n"));
    for (char& c : word) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    // the pause menu's host actions: Restart Match (HostRestart) and Change Map (SetNextMap chose the map in this
    // front end's 0x526EC0, then changemap)
    if (word == "hostrestart") {
        LOGI("local server: Restart Match sent to the server");
        QueueEngineCommand("cmd pc_owner restart\n");
        return true;
    }
    if (word == "changemap") {
        std::string map = kernel::ReadCString(0x526EC0, 32);
        LOGI("local server: Change Map to %s sent to the server", map.c_str());
        QueueEngineCommand("cmd pc_owner changemap " + map + "\n");
        return true;
    }
    // the server's own settings and commands, typed in the game's console (or a test's): run there
    static const char* const kPrefixes[] = {"sv_", "mp_", "bot_"};
    static const char* const kNames[] = {"allow_spectators", "pausable", "pc_player_collision", "kick", "banid", "removeid",
                                         "addip", "removeip", "writeid", "writeip", "changelevel", "restart"};
    bool server = false;
    for (const char* p : kPrefixes) server = server || word.rfind(p, 0) == 0;
    for (const char* n : kNames) server = server || word == n;
    if (!server || text.empty() || text.size() > 200 || text.find(';') != std::string::npos) return false;
    LOGI("local server: console \"%s\" sent to the server", text.c_str());
    QueueEngineCommand("cmd pc_owner console " + Hex(text) + "\n");
    return true;
}

namespace {
bool g_paused = false;     // the owner's pause sent
bool g_menu_seen = false;  // a menu was open since (the pause menu's own frames)
}  // namespace

void LocalServerPause(bool on) {
    if (!LocalServerRunning() || g_paused == on) return;
    g_paused = on;
    g_menu_seen = false;
    QueueEngineCommand(std::string("cmd pc_owner pause ") + (on ? "on" : "off") + "\n");
}

void LocalServerPauseFrame(bool menu_open) {
    // the pause menu closed another way than its own unpause path (back to the Spectator screen after a death):
    // the match goes on once no menu is open
    if (!g_paused) return;
    if (menu_open) g_menu_seen = true;
    else if (g_menu_seen) LocalServerPause(false);
}

}  // namespace csx
