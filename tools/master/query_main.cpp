// csx-query: asks Counter-Strike Xbox PC servers what the server browser shows (src/net/browser.h; docs/
// pc_multiplayer.md "N3"), from the command line: LAN servers by broadcast, a master server's list, or addresses.
// For server owners and the tests (tests/net/net_browse_*.txt). Built by tools/build_master.sh.
//
//   csx-query [--lan] [--ports 27015-27020] [--master HOST[:PORT]]... [--players] [--wait SECONDS] [--bind ADDRESS]
//             [ADDRESS[:PORT]]...
//
// One line a server: "server ADDRESS name=... map=... players=HUMANS+BOTS/MAX ping=MS password=yes|no dedicated=..
// collision=on|off lan_only=.. key=KEY sources=lan,internet,direct", then its players with --players; a closing
// "summary" line (servers, queries, bytes, time to the first and last answer). Exit 0 when at least one answered.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "net/browser.h"

namespace {

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

int Usage(const char* why) {
    std::fprintf(stderr, "csx-query: %s\nusage: csx-query [--lan] [--ports 27015-27020] [--master HOST[:PORT]]... "
                         "[--players] [--wait SECONDS] [--bind ADDRESS] [ADDRESS[:PORT]]...\n", why);
    return 2;
}

std::string Sources(uint8_t s) {
    std::string out;
    for (auto [bit, name] : {std::pair{net::kLan, "lan"}, {net::kInternet, "internet"}, {net::kDirect, "direct"}})
        if (s & bit) out += std::string(out.empty() ? "" : ",") + name;
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    net::BrowserConfig config;
    bool lan = false, players = false;
    double wait = 3;
    std::string bind;
    std::vector<net::Address> direct;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--lan") {
            lan = true;
        } else if (a == "--players") {
            players = true;
        } else if (a == "--ports" && i + 1 < argc) {
            unsigned lo = 0, hi = 0;
            if (std::sscanf(argv[++i], "%u-%u", &lo, &hi) != 2 || lo == 0 || hi > 65535 || hi < lo || hi - lo > 32)
                return Usage("--ports wants FIRST-LAST, at most 33 ports");
            config.lan_ports.clear();
            for (unsigned p = lo; p <= hi; p++) config.lan_ports.push_back(uint16_t(p));
        } else if (a == "--master" && i + 1 < argc) {
            auto m = net::Address::Parse(argv[++i], net::kMasterPort);
            if (!m) return Usage("cannot resolve the master server");
            config.masters.push_back(*m);
        } else if (a == "--wait" && i + 1 < argc) {
            wait = std::atof(argv[++i]);
            if (wait <= 0 || wait > 120) return Usage("--wait is 0 to 120 seconds");
        } else if (a == "--bind" && i + 1 < argc) {
            bind = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            auto d = net::Address::Parse(a, 27015);
            if (!d) return Usage(("cannot resolve " + a).c_str());
            direct.push_back(*d);
        } else {
            return Usage(("unknown argument " + a).c_str());
        }
    }
    if (!lan && config.masters.empty() && direct.empty()) return Usage("nothing to ask: --lan, --master or an address");
    std::string error;
    std::optional<net::Address> at;
    if (!bind.empty() && !(at = net::Address::Parse(bind, 0))) return Usage("cannot use the bind address");
    auto socket = net::OpenUdp(0, error, at ? &*at : nullptr, true);
    if (!socket) {
        std::fprintf(stderr, "csx-query: %s\n", error.c_str());
        return 1;
    }
    config.broadcast = net::LanBroadcastAddresses();
    net::Browser browser(std::move(socket), config, Now);
    double start = Now(), first = -1, last = -1;
    if (lan) browser.RefreshLan();
    if (!config.masters.empty()) browser.RefreshInternet();
    for (const auto& d : direct) browser.Add(d, net::kDirect);
    uint64_t seen = 0;
    bool asked_players = false;
    while (Now() - start < wait) {
        browser.Wait(0.005);
        browser.Pump();
        if (browser.Version() != seen) {
            seen = browser.Version();
            for (const auto& e : browser.Entries())
                if (e.have_info) {
                    if (first < 0) first = Now() - start;
                    last = std::max(last, e.last_answer - start);
                }
        }
        // with --players, once the answers are in (or half the time passed), every answered server's players
        if (players && !asked_players && (Now() - start > wait / 2 || (!browser.Busy() && first >= 0 && !lan))) {
            asked_players = true;
            for (const auto& e : browser.Entries())
                if (e.have_info) browser.QueryPlayers(e.address);
        }
        if (!players && !lan && !browser.Busy() && Now() - start > 0.2) break;
    }
    int answered = 0;
    net::ViewOptions all;
    all.sources = 0xFF;
    all.answered_only = true;
    for (size_t i : net::View(browser.Entries(), all)) {
        const auto& e = browser.Entries()[i];
        const auto& s = e.info;
        answered++;
        std::printf("server %s name=%s map=%s mode=%s players=%u+%u/%u ping=%d password=%s dedicated=%s collision=%s "
                    "friendly_fire=%s lan_only=%s key=%s sources=%s%s%s\n",
                    e.address.ToString().c_str(), s.name.c_str(), s.map.c_str(), net::GameMode(s.map).c_str(),
                    s.humans, s.bots, s.max_players, e.ping, s.password ? "yes" : "no", s.dedicated ? "yes" : "no",
                    s.collision ? "on" : "off", s.friendly_fire ? "yes" : "no", s.lan_only ? "yes" : "no", e.key.c_str(),
                    Sources(e.sources).c_str(), e.problem.empty() ? "" : " problem=", e.problem.c_str());
        if (players)
            for (const auto& p : e.players)
                std::printf("  player %s score=%d ping=%u time=%us%s\n", p.name.c_str(), p.score, p.ping, p.seconds,
                            p.bot ? " bot" : "");
    }
    for (const auto& e : browser.Entries())
        if (!e.have_info && (e.sources & net::kDirect))
            std::printf("noanswer %s\n", e.address.ToString().c_str());
    const auto& st = browser.GetStats();
    std::printf("summary servers=%d queries=%llu broadcasts=%llu list_pages=%llu sent_bytes=%llu received_bytes=%llu "
                "first_answer_ms=%d last_answer_ms=%d malformed=%llu wrong_key=%llu\n",
                answered, (unsigned long long)st.queries, (unsigned long long)st.broadcasts,
                (unsigned long long)st.list_pages, (unsigned long long)st.query_bytes,
                (unsigned long long)st.answer_bytes, first < 0 ? -1 : int(first * 1000),
                last < 0 ? -1 : int(last * 1000), (unsigned long long)st.malformed, (unsigned long long)st.wrong_key);
    return answered ? 0 : 1;
}
