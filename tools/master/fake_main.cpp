// csx-fakeservers: many servers that answer the browser's queries and register with a master server, for the server
// browser's load tests (tests/net/net_browse_many.txt; docs/pc_multiplayer.md "N3"). Each is a real UDP socket with
// its own identity, signed answers and registration (src/net/query.h, master.h), with made-up names, maps and player
// counts. No game: they cannot be joined. Built by tools/build_master.sh.
//
//   csx-fakeservers --count N [--bind ADDRESS] [--port FIRST] [--master HOST[:PORT]] [--seconds S]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sodium.h>

#include "net/master.h"
#include "net/query.h"

namespace {

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

struct Fake {
    std::unique_ptr<net::DatagramSocket> socket;
    std::unique_ptr<net::QueryServer> query;
    std::unique_ptr<net::Registrar> registrar;
};

}  // namespace

int main(int argc, char** argv) {
    int count = 100, port = 27100;
    double seconds = 120;
    std::string bind = "0.0.0.0";
    std::vector<net::Address> masters;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--count") count = std::atoi(v.c_str());
        else if (a == "--port") port = std::atoi(v.c_str());
        else if (a == "--bind") bind = v;
        else if (a == "--seconds") seconds = std::atof(v.c_str());
        else if (a == "--master") {
            auto m = net::Address::Parse(v, net::kMasterPort);
            if (!m) return std::fprintf(stderr, "csx-fakeservers: cannot resolve %s\n", v.c_str()), 2;
            masters.push_back(*m);
        } else {
            return std::fprintf(stderr, "csx-fakeservers: unknown %s\n", a.c_str()), 2;
        }
    }
    if (count < 1 || count > 4000 || port < 1 || port + count > 65535 || sodium_init() < 0) return 2;
    auto at = net::Address::Parse(bind, 0);
    if (!at) return 2;
    static const char* maps[] = {"de_dust2", "de_aztec", "cs_office", "de_inferno", "cs_italy", "de_dust", "de_cbble"};
    std::vector<Fake> fakes;
    for (int i = 0; i < count; i++) {
        std::string error;
        Fake f;
        f.socket = net::OpenUdp(uint16_t(port + i), error, &*at);
        if (!f.socket) return std::fprintf(stderr, "csx-fakeservers: port %d: %s\n", port + i, error.c_str()), 1;
        net::Identity id;
        crypto_sign_keypair(id.public_key.data(), id.secret_key.data());
        f.query = std::make_unique<net::QueryServer>(net::QueryServer::Config{1000, 100000, false}, id);
        net::ServerInfo info;
        info.name = "Fake server " + std::to_string(i + 1);
        info.map = maps[i % 7];
        info.max_players = uint8_t(i % 3 == 0 ? 65 : i % 3 == 1 ? 32 : 16);
        info.humans = uint8_t(i % 5);
        info.bots = uint8_t(std::min<int>(info.max_players - info.humans, (i * 7) % 40));
        info.password = i % 6 == 0;
        info.dedicated = true;
        info.round_seconds = 180;
        info.time_limit = 30;
        f.query->Set(info, {});
        if (!masters.empty()) f.registrar = std::make_unique<net::Registrar>(id, masters, Now);
        fakes.push_back(std::move(f));
    }
    std::printf("csx-fakeservers: %d servers on %s ports %d-%d\n", count, bind.c_str(), port, port + count - 1);
    std::fflush(stdout);
    uint64_t answered = 0;
    double end = Now() + seconds, report = Now() + 10;
    uint8_t buf[2048];
    while (Now() < end) {
        for (auto& f : fakes) {
            auto send = [&](const net::Address& to, const std::vector<uint8_t>& d) { f.socket->Send(to, d.data(), d.size()); };
            if (f.registrar) f.registrar->Pump(send);
            net::Address from;
            int n;
            while ((n = f.socket->Receive(from, buf, sizeof(buf))) >= 0) {
                if (n < 6) continue;
                net::Type t = net::Type(buf[5]);
                if (f.registrar && f.registrar->Handle(from, t, buf, size_t(n), send)) continue;
                if (t != net::Type::Query) continue;
                auto a = f.query->Answer(from, buf, size_t(n), Now());
                if (!a.empty()) {
                    send(from, a);
                    answered++;
                }
            }
        }
        if (Now() >= report) {
            report = Now() + 10;
            std::printf("csx-fakeservers: %llu queries answered\n", (unsigned long long)answered);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("csx-fakeservers: done, %llu queries answered\n", (unsigned long long)answered);
    return 0;
}
