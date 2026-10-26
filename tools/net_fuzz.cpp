// Malformed network traffic against a running server (tests/net/net_malformed.txt; built by tools/net_fuzz.py): what a
// hostile or broken peer can send to the PC transport's port and, once connected, to the engine behind it.
//   1. raw datagrams: random bytes, every transport type with a valid prefix and random contents, truncated and
//      oversized datagrams, hellos too short to answer, connects with forged cookies, data with unknown connection ids
//   2. a hello flood from one address (the rate limit)
//   3. a proper transport connection (src/net/transport.h), then the engine's connectionless commands with bad
//      arguments (getchallenge, connect with wrong challenges, oversized and "*"-keyed userinfo, rcon without a
//      challenge, info, players, rules, random text)
//   4. on that connection, after a real engine connect (challenge, connect, accept): netchan packets with random
//      sequence numbers, flags and payloads (random client commands, oversized lengths), and random fragment headers
// It exits 0 after its time; whether the server survived is the test's own check (its log, a later client).
//   net_fuzz HOST PORT SECONDS [SEED]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/query.h"
#include "net/transport.h"

using namespace net;

namespace {
double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> Oob(const std::string& text) {
    std::vector<uint8_t> v = {0xFF, 0xFF, 0xFF, 0xFF};
    v.insert(v.end(), text.begin(), text.end());
    v.push_back(0);
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: net_fuzz HOST PORT SECONDS [SEED]\n");
        return 2;
    }
    auto server = Address::Parse(std::string(argv[1]) + ":" + argv[2], 27015);
    if (!server) {
        std::fprintf(stderr, "bad address\n");
        return 2;
    }
    double seconds = std::atof(argv[3]);
    std::mt19937 rng(argc > 4 ? uint32_t(std::atoi(argv[4])) : 1);
    std::string error;
    auto raw = OpenUdp(0, error);
    if (!raw) {
        std::fprintf(stderr, "socket: %s\n", error.c_str());
        return 1;
    }
    uint64_t sent = 0;
    auto rnd = [&](size_t n) {
        std::vector<uint8_t> v(n);
        for (auto& b : v) b = uint8_t(rng());
        return v;
    };
    // 1. raw datagrams
    for (int i = 0; i < 20000; i++) {
        size_t n = rng() % 1500;
        auto d = rnd(n);
        if (n >= 6 && i % 2) {
            uint32_t m = kMagic;
            std::memcpy(d.data(), &m, 4);
            d[4] = i % 7 ? kVersion : uint8_t(rng());
            d[5] = uint8_t(rng() % 16);  // the connection types and the connectionless ones (queries, master)
        }
        raw->Send(*server, d.data(), d.size());
        sent++;
        if (i % 500 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("fuzz: %llu raw datagrams sent\n", (unsigned long long)sent);
    // 2. a hello flood (full size, so each could be answered)
    std::vector<uint8_t> hello(kMaxDatagram, 0);
    uint32_t m = kMagic;
    std::memcpy(hello.data(), &m, 4);
    hello[4] = kVersion;
    hello[5] = uint8_t(Type::Hello);
    int answered = 0;
    for (int i = 0; i < 2000; i++) raw->Send(*server, hello.data(), hello.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    uint8_t buf[2048];
    Address from;
    while (raw->Receive(from, buf, sizeof(buf)) >= 0) answered++;
    std::printf("fuzz: hello flood of 2000, %d answered (the rate limit's burst)\n", answered);
    // 2b. the server query (src/net/query.h): malformed ones of every size and kind and master packets sent to a game
    // server (none answered), then a flood of valid queries from one address (only the rate limit's burst answered)
    auto raw2 = OpenUdp(0, error);
    if (!raw2) return 1;
    for (int i = 0; i < 6000; i++) {
        size_t n = i % 3 == 0 ? kInfoQuerySize : i % 3 == 1 ? kPlayersQuerySize : 6 + rng() % 1300;
        auto d = rnd(n);
        std::memcpy(d.data(), &m, 4);
        d[4] = kVersion;
        d[5] = uint8_t(i % 4 ? Type::Query : Type(9 + rng() % 6));
        if (i % 4 && n >= 7) d[6] = uint8_t(rng() % 4);  // the kind: none, info, players, none
        raw2->Send(*server, d.data(), d.size());
        if (i % 500 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int stray = 0;
    while (raw2->Receive(from, buf, sizeof(buf)) >= 0) stray++;
    int answers = 0;
    for (int i = 0; i < 1000; i++) {
        auto q = EncodeQuery(i % 2 ? QueryKind::Info : QueryKind::Players, 0, uint64_t(i + 1));
        raw2->Send(*server, q.data(), q.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    while (raw2->Receive(from, buf, sizeof(buf)) >= 0) answers++;
    std::printf("fuzz: 6000 malformed queries (%d answers to random ones that happened to be valid), then 1000 valid "
                "ones: %d answered (the query rate limit's burst)\n", stray, answers);
    raw.reset();
    raw2.reset();

    // 3 and 4: a real transport connection
    auto sock = OpenUdp(0, error);
    Endpoint ep(Endpoint::Role::Client, std::move(sock), Config{}, Now);
    auto send = [&](const std::vector<uint8_t>& v) {
        ep.Send(*server, v.data(), v.size());
        ep.Pump();
    };
    auto drain = [&](double wait) {
        std::vector<std::vector<uint8_t>> got;
        double until = Now() + wait;
        Address f;
        std::vector<uint8_t> msg;
        while (Now() < until) {
            ep.Pump();
            while (ep.Receive(f, msg)) got.push_back(msg);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return got;
    };
    send(Oob("info"));
    drain(1.0);
    if (!ep.Connected(*server)) {
        std::printf("fuzz: no transport connection to the server\n");
        return 1;
    }
    std::printf("fuzz: transport connected\n");
    const char* cmds[] = {"getchallenge", "getchallenge steam", "connect", "connect 47", "connect 46 1 \"\" \"\"",
                          "connect 47 12345 \"\\prot\\2\\unique\\-1\\raw\\00\" \"\\name\\x\"", "rcon", "rcon 1 \"\" status",
                          "rcon 99 secret quit", "challenge rcon", "info", "details", "infostring", "players", "rules",
                          "ping", "log hello", "\x01\x02\x03", "pstat", ""};
    for (int round = 0; round < 20; round++) {
        for (const char* c : cmds) send(Oob(c));
        std::string huge = "connect 47 1 \"\\prot\\2\\unique\\-1\\raw\\" + std::string(32, 'a') + "\" \"";
        for (int i = 0; i < 300; i++) huge += "\\*k" + std::to_string(i) + "\\" + std::string(20, 'v');
        huge += "\"";
        send(Oob(huge));
        auto r = rnd(rng() % 3000);
        r.insert(r.begin(), {0xFF, 0xFF, 0xFF, 0xFF});
        send(r);
        drain(0.05);
    }
    // a real engine connect: the challenge, then connect with it
    send(Oob("getchallenge"));
    uint32_t challenge = 0;
    for (auto& g : drain(1.0))
        if (g.size() > 5 && g[4] == 'A') challenge = uint32_t(std::strtoul(reinterpret_cast<const char*>(g.data()) + 14, nullptr, 10));
    std::printf("fuzz: challenge %u\n", challenge);
    std::string raw32(32, 'f');
    send(Oob("connect 47 " + std::to_string(challenge) + " \"\\prot\\2\\unique\\-1\\raw\\" + raw32 +
             "\" \"\\name\\fuzzer\\*bot\\1\\*pid\\fake\\rate\\100000\\cl_updaterate\\30\""));
    bool accepted = false;
    for (auto& g : drain(1.5))
        if (g.size() > 5 && g[4] == 'B') accepted = true;
    std::printf("fuzz: engine connect %s\n", accepted ? "accepted" : "not accepted");
    // netchan packets with random headers and payloads, and random fragment headers
    double end = Now() + std::max(5.0, seconds - Now());
    uint64_t packets = 0;
    uint32_t seq = 1;
    while (Now() < end) {
        std::vector<uint8_t> p;
        uint32_t s = (rng() % 4 == 0) ? uint32_t(rng()) : (seq++ | (rng() % 2 ? 0x80000000u : 0) | (rng() % 3 == 0 ? 0x40000000u : 0));
        uint32_t a = rng() % 4 == 0 ? uint32_t(rng()) : 0;
        for (int i = 0; i < 4; i++) p.push_back(uint8_t(s >> (8 * i)));
        for (int i = 0; i < 4; i++) p.push_back(uint8_t(a >> (8 * i)));
        auto body = rnd(rng() % (rng() % 10 == 0 ? 60000 : 400));
        if (!body.empty() && rng() % 2) body[0] = uint8_t(rng() % 12);  // a client command id (clc_*)
        p.insert(p.end(), body.begin(), body.end());
        send(p);
        packets++;
        if (packets % 50 == 0) drain(0.02);
    }
    std::printf("fuzz: %llu netchan packets sent\n", (unsigned long long)packets);
    const Stats& st = ep.GetStats();
    std::printf("fuzz: done; still connected %s; datagrams out %llu in %llu\n", ep.Connected(*server) ? "yes" : "no",
                (unsigned long long)st.datagrams_out, (unsigned long long)st.datagrams_in);
    return 0;
}
