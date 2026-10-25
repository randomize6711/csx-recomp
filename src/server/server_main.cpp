// The dedicated server (docs/pc_multiplayer.md "N2"): the game's engine and game DLL with no client, no window, no
// GPU, no audio device and no input, serving every player slot to remote players and bots over the PC transport.
//   csx-server --game DIR [--config FILE] [--KEY VALUE]...      (keys: csx-server --help; tools/build_server.sh)
// Run from the folder that keeps its state: logs/, hdd/ (the emulated disk, fresh at each start),
// server_identity.key (made on the first start, owner-only), admins.cfg and bans.txt (remote administration, off
// without admins.cfg), known to clients by its identity key.
// Its console is stdin: engine commands (status, kick, say ...); quit ends it (the end of input is ignored); SIGINT and SIGTERM
// end it cleanly (the players are told). A map change is a relaunch of this process (the title's), which comes back
// without arguments: the settings travel in CSX_SERVER_SETTINGS, the new map from the title's command line.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
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
#include <fcntl.h>
#include <execinfo.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "app/app.h"
#include "common/csx.h"
#include "core/log.h"
#include "game/net_engine.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "server/server_config.h"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

void Usage(const char* exe) {
    std::printf("usage: %s --game DIR [--config FILE] [--KEY VALUE]...\n"
                "  --game DIR     the game folder (default.xbe, the maps); or GAME_DIR\n"
                "  --config FILE  settings, one \"key value\" per line (# comments); options given after it override\n"
                "  --KEY VALUE    one setting (also --key-name), from:\n",
                exe);
    for (const auto& k : server::Keys()) std::printf("    %-17s %s\n", k.name, k.help);
    std::printf("Run it in the folder that keeps its state (logs, identity key, admins.cfg). Console: stdin.\n");
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

// what the front end leaves for its relaunch into a match: the launch data page naming the image
// (tests/launch/README.md; the same 4 KB page every fixture has)
std::vector<uint8_t> LaunchPage() {
    std::vector<uint8_t> p(4096, 0);
    const uint8_t head[] = {0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x53, 0x4D};
    std::memcpy(p.data(), head, sizeof(head));
    std::memcpy(p.data() + 8, "\\Device\\CdRom0;default.xbe", 26);
    const uint8_t mid[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(p.data() + 0x210, mid, sizeof(mid));
    std::memcpy(p.data() + 0x21C, "\\Device\\CdRom0\0default.xbe", 26);
    p[0x400] = 3;
    return p;
}

bool WriteFile(const fs::path& path, const void* data, size_t n, bool owner_only) {
    fs::create_directories(path.parent_path());
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, data, n) == ssize_t(n);
    return ::close(fd) == 0 && ok;
#else
    (void)owner_only;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), std::streamsize(n));
    return bool(f);
#endif
}

// the map the title's own relaunch names ("+map NAME ..." in Z:\releasemap.txt: a map change, the end of a match)
std::string RelaunchMap(const fs::path& hdd) {
    std::string line;
    if (!ReadFile((hdd / "Z" / "releasemap.txt").string(), line) || line.rfind("+map ", 0) != 0) return {};
    size_t end = line.find_first_of(" \n\r", 5);
    return line.substr(5, end == std::string::npos ? std::string::npos : end - 5);
}

// stdin, the server's console: each line an engine command; SIGINT / SIGTERM queue "quit"
void Console() {
    std::string line;
#ifndef _WIN32
    bool eof = false;
    for (;;) {
        if (g_stop.exchange(false)) {
            LOGI("server: stop requested, quitting");
            csx::QueueEngineCommand("quit\n");
        }
        if (eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        pollfd p{0, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        char buf[512];
        ssize_t n = ::read(0, buf, sizeof(buf));
        if (n <= 0) {
            eof = true;  // no console (a service, a closed terminal): the server runs on
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    LOGI("server console: %s", line.c_str());
                    csx::QueueEngineCommand(line + "\n");
                }
                line.clear();
            } else if (line.size() < 1000 && buf[i] != '\0') {
                line += buf[i];
            }
        }
    }
#else
    while (std::getline(std::cin, line))
        if (!line.empty()) csx::QueueEngineCommand(line + "\n");
#endif
}

#ifndef _WIN32
// a server whose frames stopped (a guest loop that never returns: nothing else would show where): every thread's
// stack once, on stderr (run.out); the recompiled functions are named by their guest address (sub_0005D5D0)
void DumpStack(int) {
    void* frames[48];
    int n = backtrace(frames, 48);
    char head[64];
    int len = std::snprintf(head, sizeof(head), "--- thread %ld\n", long(syscall(SYS_gettid)));
    (void)!::write(2, head, size_t(len));
    backtrace_symbols_fd(frames, n, 2);
}

void Watchdog() {
    void* warm[4];
    backtrace(warm, 4);  // loads the unwinder now, not inside the signal handler
    struct sigaction sa {};
    sa.sa_handler = DumpStack;
    sigaction(SIGUSR2, &sa, nullptr);
    uint64_t last = csx::ServerFrames();
    int still = 0;
    bool dumped = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t now = csx::ServerFrames();
        still = now == last ? still + 1 : 0;
        last = now;
        if (still == 15 && !dumped) {
            dumped = true;
            LOGE("server: no server frame for 15 s (%llu frames so far): every thread's stack follows on stderr",
                 static_cast<unsigned long long>(now));
            logx::Flush();
            for (const auto& t : fs::directory_iterator("/proc/self/task")) {
                long tid = std::atol(t.path().filename().c_str());
                if (tid != long(syscall(SYS_gettid))) syscall(SYS_tgkill, long(getpid()), tid, SIGUSR2);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
}
#endif

}  // namespace

int main(int argc, char** argv) {
    server::Config cfg;
    std::vector<server::Error> errors;
    std::string game = std::getenv("GAME_DIR") ? std::getenv("GAME_DIR") : "";
    const char* kept = std::getenv("CSX_SERVER_SETTINGS");
    bool relaunch = kept && argc == 1;
    if (relaunch) {
        server::ParseFile(cfg, kept, "CSX_SERVER_SETTINGS", errors);
    } else {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "-h" || a == "--help") {
                Usage(argv[0]);
                return 0;
            }
            if (a.rfind("--", 0) != 0 || i + 1 >= argc) {
                errors.push_back({a, i + 1 >= argc && a.rfind("--", 0) == 0 ? "needs a value" : "not an option"});
                continue;
            }
            std::string key = a.substr(2), value = argv[++i];
            for (char& ch : key) ch = ch == '-' ? '_' : ch;
            if (key == "game") {
                game = value;
            } else if (key == "config") {
                std::string text;
                if (!ReadFile(value, text)) errors.push_back({value, std::string("cannot read: ") + std::strerror(errno)});
                else server::ParseFile(cfg, text, value, errors);
            } else if (std::string error; !server::Set(cfg, key, value, error)) {
                errors.push_back({a, error});
            }
        }
    }
    fs::path hdd = fs::current_path() / (app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd");
    if (relaunch) {
        if (std::string m = RelaunchMap(hdd); !m.empty()) {
            std::string error;
            if (!server::Set(cfg, "map", m, error)) errors.push_back({"the title's next map", error});
        }
    }
    if (game.empty()) errors.push_back({"--game", "the game folder is needed (or GAME_DIR)"});
    else if (!fs::exists(fs::path(game) / IMAGE_XBE)) errors.push_back({"--game", game + " has no " + IMAGE_XBE});
    else if (!fs::exists(fs::path(game) / (cfg.map + ".SXWad"))) errors.push_back({"map", "no map " + cfg.map + " in " + game});
    server::CheckTogether(cfg, errors);
    if (!errors.empty()) {
        for (auto& e : errors) std::fprintf(stderr, "csx-server: %s: %s\n", e.where.c_str(), e.text.c_str());
        std::fprintf(stderr, "csx-server: not started (%zu problems; --help lists the settings)\n", errors.size());
        return 2;
    }
    game = fs::absolute(game).string();
    setenv("GAME_DIR", game.c_str(), 1);
    setenv("CSX_SERVER_SETTINGS", server::Describe(cfg, true).c_str(), 1);
    setenv("CSX_DEDICATED", "1", 1);
    setenv("CSX_NET_HOST", "1", 1);
    setenv("CSX_NET_PORT", std::to_string(cfg.port).c_str(), 1);
    setenv("CSX_NET_LAN_ONLY", cfg.lan_only ? "1" : "0", 1);

    // the match the title boots into: a fresh disk (as a console's quick reboot leaves it) with the launch page and
    // the command line; on a relaunch the title's own launch page is kept and the line rewritten with these settings
    if (!relaunch) {
        std::error_code ec;
        fs::remove_all(hdd, ec);
        auto page = LaunchPage();
        WriteFile(hdd / "launch.bin", page.data(), page.size(), false);
    }
    std::string line = server::CommandLine(cfg);
    if (!WriteFile(hdd / "Z" / "releasemap.txt", line.data(), line.size(), true)) {
        std::fprintf(stderr, "csx-server: cannot write %s\n", (hdd / "Z" / "releasemap.txt").c_str());
        return 1;
    }

    logx::Init((app::ExeDir() / "logs").string(), "server", relaunch);
    LOGI("server: %s on UDP port %d, map %s, %d player slots, %d bots", relaunch ? "relaunched" : "starting", cfg.port,
         cfg.map.c_str(), cfg.max_players, cfg.bots);
    {
        std::istringstream d(server::Describe(cfg));
        for (std::string l; std::getline(d, l);) LOGI("server setting: %s", l.c_str());
    }
#ifndef _WIN32
    struct sigaction sa {};
    sa.sa_handler = [](int) { g_stop = true; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
    std::thread(Console).detach();
#ifndef _WIN32
    std::thread(Watchdog).detach();
#endif

    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(nullptr);
    std::ifstream f(fs::path(game) / IMAGE_XBE, std::ios::binary);
    std::vector<uint8_t> xbe((std::istreambuf_iterator<char>(f)), {});
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game, hdd.string())) return 1;
    while (!csx::ServerFinished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        logx::Flush();
    }
    LOGI("server: shut down");
    logx::Flush();
    std::_Exit(0);  // the title's threads are still parked in its code: nothing of theirs to unwind
}
