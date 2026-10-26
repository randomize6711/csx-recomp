// csx-master: the reference master server for Counter-Strike Xbox PC servers (src/net/master.h; docs/pc_multiplayer.md
// "N3"). A small, self-hostable UDP program: servers that name it (server.cfg "master", PC setting net_master_register
// for a listen server) register and heartbeat; browsers (PC setting net_master) fetch the list. No accounts, no
// database, nothing kept across restarts (servers register again within a heartbeat). Built by tools/build_master.sh.
//
//   csx-master [--port 27010] [--bind ADDRESS] [--expiry SECONDS] [--heartbeat SECONDS] [--max-servers N]
//              [--max-per-ip N] [--public] [--quiet]
//
// --public refuses servers at private, loopback and link-local addresses (a master on the internet); without it a
// LAN party's master lists them. Logs to standard output; SIGINT or SIGTERM ends it.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "net/master.h"
#include "net/transport.h"

namespace {

std::atomic<bool> g_stop{false};

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

void Log(const std::string& s) {
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::printf("%s %s\n", stamp, s.c_str());
    std::fflush(stdout);
}

int Usage(const char* why) {
    std::fprintf(stderr, "csx-master: %s\nusage: csx-master [--port 27010] [--bind ADDRESS] [--expiry SECONDS] "
                         "[--heartbeat SECONDS] [--max-servers N] [--max-per-ip N] [--public] [--quiet]\n", why);
    return 2;
}

bool Number(const char* s, long lo, long hi, long& out) {
    char* end = nullptr;
    out = std::strtol(s, &end, 10);
    return end && *end == 0 && out >= lo && out <= hi;
}

}  // namespace

int main(int argc, char** argv) {
    net::MasterServer::Config config;
    long port = net::kMasterPort;
    std::string bind;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        long v = 0;
        auto value = [&](long lo, long hi) {
            if (i + 1 >= argc || !Number(argv[i + 1], lo, hi, v)) return false;
            i++;
            return true;
        };
        if (a == "--port" && value(1, 65535)) port = v;
        else if (a == "--bind" && i + 1 < argc) bind = argv[++i];
        else if (a == "--expiry" && value(20, 3600)) config.expiry = double(v);
        else if (a == "--heartbeat" && value(10, 600)) config.heartbeat = uint16_t(v);
        else if (a == "--max-servers" && value(1, 1000000)) config.max_servers = size_t(v);
        else if (a == "--max-per-ip" && value(1, 4096)) config.max_per_ip = size_t(v);
        else if (a == "--public") config.allow_private = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h") return Usage("the Counter-Strike Xbox PC master server");
        else return Usage(("unknown or invalid argument " + a).c_str());
    }
    if (config.heartbeat * 2 > config.expiry) return Usage("--expiry must be at least twice --heartbeat");
    std::string error;
    std::optional<net::Address> at;
    if (!bind.empty() && !(at = net::Address::Parse(bind, 0))) return Usage(("cannot use the address " + bind).c_str());
    auto socket = net::OpenUdp(uint16_t(port), error, at ? &*at : nullptr);
    if (!socket) {
        std::fprintf(stderr, "csx-master: %s\n", error.c_str());
        return 1;
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    net::MasterServer master(config, Now);
    if (!quiet) master.log = Log;
    Log("csx-master on UDP " + socket->Local().ToString() + ": expiry " + std::to_string(int(config.expiry)) +
        " s, heartbeat " + std::to_string(config.heartbeat) + " s, at most " + std::to_string(config.max_servers) +
        " servers, " + std::to_string(config.max_per_ip) + " an address, " +
        (config.allow_private ? "private addresses listed" : "public addresses only"));
    double status = Now() + 60;
    uint8_t buf[2048];
    net::Address from;
    while (!g_stop) {
        socket->Wait(1.0);
        for (int i = 0; i < 4096; i++) {
            int n = socket->Receive(from, buf, sizeof(buf));
            if (n < 0) break;
            master.Handle(from, buf, size_t(n), [&](const net::Address& to, const std::vector<uint8_t>& d) {
                socket->Send(to, d.data(), d.size());
            });
        }
        master.Expire();
        if (Now() >= status) {
            status = Now() + 60;
            const auto& s = master.GetStats();
            Log("status: " + std::to_string(master.Count()) + " servers listed; " + std::to_string(s.registered) +
                " registered, " + std::to_string(s.expired) + " expired, " + std::to_string(s.left) + " left, " +
                std::to_string(s.lists) + " list pages sent; refused " + std::to_string(s.refused) + ", bad cookies " +
                std::to_string(s.bad_cookie) + ", bad signatures " + std::to_string(s.bad_signature) +
                ", rate limited " + std::to_string(s.rate_limited) + ", malformed " + std::to_string(s.malformed));
        }
    }
    Log("csx-master stopped with " + std::to_string(master.Count()) + " servers listed");
    return 0;
}
