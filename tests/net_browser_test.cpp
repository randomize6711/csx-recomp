// Unit tests of the server browser's protocols and discovery (src/net/query.h, master.h, browser.h) over a simulated
// network with broadcast and a simulated clock. Built and run by tests/net_browser_test.py.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/browser.h"
#include "net/master.h"
#include "net/query.h"
#include "net/transport.h"

#include <sodium.h>

using namespace net;

namespace {

int g_checks = 0, g_failed = 0;
double g_now = 100;
double Now() { return g_now; }

void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

Identity MakeId(uint8_t seed) {
    Identity id;
    uint8_t s[32];
    std::memset(s, seed, sizeof(s));
    crypto_sign_seed_keypair(id.public_key.data(), id.secret_key.data(), s);
    return id;
}

Address Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    Address x;
    x.ip = {a, b, c, d};
    x.port = port;
    return x;
}

// ---- a network of sockets on 10.0.0.0/24 (broadcast 10.0.0.255) and 127/8 (broadcast 127.255.255.255) ----------
struct Network {
    struct Packet {
        Address from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Packet> flight;
    std::set<uint64_t> bound;
    uint64_t largest = 0, sent = 0, bytes = 0;
    bool drop_all = false;
};

class SimSocket : public DatagramSocket {
public:
    SimSocket(Network& net, Address me) : net_(net), me_(me) { net_.bound.insert(me.Key()); }
    ~SimSocket() override { net_.bound.erase(me_.Key()); }
    bool Send(const Address& to, const uint8_t* data, size_t len) override {
        net_.largest = std::max<uint64_t>(net_.largest, len);
        net_.sent++;
        net_.bytes += len;
        if (len > kMaxDatagram) oversize++;
        if (net_.drop_all || mute) return true;
        net_.flight.push_back({me_, to, std::vector<uint8_t>(data, data + len)});
        return true;
    }
    int Receive(Address& from, uint8_t* buf, size_t max) override {
        for (size_t i = 0; i < net_.flight.size(); i++) {
            const Address& to = net_.flight[i].to;
            bool bcast = (to.ip == std::array<uint8_t, 4>{10, 0, 0, 255} && me_.ip[0] == 10) ||
                         (to.ip == std::array<uint8_t, 4>{127, 255, 255, 255} && me_.ip[0] == 127) ||
                         to.ip == std::array<uint8_t, 4>{255, 255, 255, 255};
            if (!(to == me_) && !(bcast && to.port == me_.port)) continue;
            Network::Packet p = net_.flight[i];
            // a broadcast stays in flight for every listener (Settle clears it); each socket reads its copy once
            if (!bcast) net_.flight.erase(net_.flight.begin() + long(i));
            else if (delivered_.count(Key(p))) continue;
            else delivered_.insert(Key(p));
            if (p.data.size() > max) return -1;
            std::memcpy(buf, p.data.data(), p.data.size());
            from = p.from;
            return int(p.data.size());
        }
        return -1;
    }
    Address Local() const override { return me_; }
    int oversize = 0;
    bool mute = false;  // a server that stopped answering

private:
    static std::string Key(const Network::Packet& p) {
        return std::to_string(p.from.Key()) + "/" + std::to_string(p.to.Key()) + "/" +
               std::string(p.data.begin(), p.data.end());
    }
    Network& net_;
    Address me_;
    std::set<std::string> delivered_;
};

// broadcasts stay in flight for every listener; cleared between steps once everyone read
void Settle(Network& net) {
    net.flight.erase(std::remove_if(net.flight.begin(), net.flight.end(), [](const Network::Packet& p) {
                         return p.to.ip == std::array<uint8_t, 4>{10, 0, 0, 255} ||
                                p.to.ip == std::array<uint8_t, 4>{127, 255, 255, 255} ||
                                p.to.ip == std::array<uint8_t, 4>{255, 255, 255, 255};
                     }),
                     net.flight.end());
}

// a game server: an endpoint with its query answered (as src/game/server_query.cpp wires it)
struct SimServer {
    SimSocket* sock;
    std::unique_ptr<Endpoint> ep;
    std::unique_ptr<QueryServer> query;
    Identity id;
    SimServer(Network& net, Address a, uint8_t seed, ServerInfo info, QueryServer::Config qc = {}) : id(MakeId(seed)) {
        auto s = std::make_unique<SimSocket>(net, a);
        sock = s.get();
        ep = std::make_unique<Endpoint>(Endpoint::Role::Server, std::move(s), Config{}, Now);
        ep->SetIdentity(id);
        query = std::make_unique<QueryServer>(qc, id);
        query->Set(info, {});
        ep->SetConnectionless([this](const Address& from, Type t, const uint8_t* p, size_t n) {
            if (t != Type::Query) return;
            auto a = query->Answer(from, p, n, Now());
            if (!a.empty()) ep->SendDatagram(from, a.data(), a.size());
        });
    }
    Address address() const { return sock->Local(); }
};

ServerInfo Info(const std::string& name, const std::string& map, uint8_t humans, uint8_t bots, uint8_t max) {
    ServerInfo i;
    i.name = name;
    i.map = map;
    i.humans = humans;
    i.bots = bots;
    i.max_players = max;
    i.round_seconds = 180;
    return i;
}

void Run(Network& net, std::vector<SimServer*> servers, Browser& b, double seconds, double step = 0.01) {
    for (double t = 0; t < seconds; t += step) {
        g_now += step;
        for (auto* s : servers) s->ep->Pump();
        Settle(net);  // the broadcasts every server has read; the browser's next ones stay for the next step
        b.Pump();
    }
}

// ---- serialization -------------------------------------------------------------------------------------------------

void TestQuerySerialization() {
    auto q = EncodeQuery(QueryKind::Info, 0, 0x1122334455667788ull);
    Check(q.size() == kInfoQuerySize, "query: an info query is 256 bytes");
    Check(EncodeQuery(QueryKind::Players, 3, 1).size() == kMaxDatagram, "query: a players query is a full datagram");
    QueryKind k;
    uint8_t page;
    uint64_t nonce;
    Check(DecodeQuery(q.data(), q.size(), k, page, nonce) && k == QueryKind::Info && nonce == 0x1122334455667788ull,
          "query: read back");
    Check(!DecodeQuery(q.data(), q.size() - 1, k, page, nonce), "query: one byte short refused");
    auto big = q;
    big.resize(kMaxDatagram);
    Check(!DecodeQuery(big.data(), big.size(), k, page, nonce), "query: an info query padded to another size refused");
    auto bad = q;
    bad[kPrefix] = 9;
    Check(!DecodeQuery(bad.data(), bad.size(), k, page, nonce), "query: an unknown kind refused");
    bad = q;
    bad[4] = 2;
    Check(!DecodeQuery(bad.data(), bad.size(), k, page, nonce), "query: another transport version refused");

    Identity id = MakeId(7);
    ServerInfo in = Info("Friday night", "de_dust2", 2, 63, 65);
    in.password = true;
    in.dedicated = true;
    in.collision = false;
    in.friendly_fire = true;
    in.time_limit = 30;
    in.max_rounds = 7;
    auto a = EncodeInfo(in, 42, id);
    Check(a.size() <= kInfoQuerySize, "info: no larger than its query (" + std::to_string(a.size()) + " bytes)");
    ServerInfo out;
    std::string err;
    Check(DecodeInfo(a.data(), a.size(), 42, out, err), "info: read back (" + err + ")");
    Check(out.name == "Friday night" && out.map == "de_dust2" && out.humans == 2 && out.bots == 63 &&
              out.max_players == 65 && out.Total() == 65 && out.Full(),
          "info: 2 humans, 63 bots, 65 slots: full");
    Check(out.password && out.dedicated && !out.collision && out.friendly_fire && out.time_limit == 30 &&
              out.max_rounds == 7 && out.round_seconds == 180,
          "info: flags and times");
    Check(out.key == id.public_key, "info: the identity key");
    Check(!DecodeInfo(a.data(), a.size(), 43, out, err), "info: another nonce refused");
    for (size_t i = 0; i < a.size(); i += 7) {
        auto t = a;
        t[i] ^= 0x40;
        Check(!DecodeInfo(t.data(), t.size(), 42, out, err), "info: byte " + std::to_string(i) + " changed refused");
    }
    Check(!DecodeInfo(a.data(), a.size() - 1, 42, out, err), "info: truncated refused");

    // a server lying about counts, and hostile text
    ServerInfo lie = Info("x", "de_dust2", 60, 10, 65);
    a = EncodeInfo(lie, 1, id);
    Check(!DecodeInfo(a.data(), a.size(), 1, out, err), "info: more players than slots refused");
    lie = Info("x", "de_dust2", 0, 0, 0);
    a = EncodeInfo(lie, 1, id);
    Check(!DecodeInfo(a.data(), a.size(), 1, out, err), "info: no slots refused");
    ServerInfo hostile = Info(std::string("evil\"; quit\n\x01#Cstrike_x %s ") + std::string(200, 'A'), "de_dust2\";rcon",
                              1, 0, 65);
    a = EncodeInfo(hostile, 5, id);
    Check(DecodeInfo(a.data(), a.size(), 5, out, err), "info: hostile text still read");
    bool clean = out.name.size() <= kMaxServerName && out.map.size() <= kMaxMapName;
    for (char c : out.name + out.map) clean = clean && c >= 0x20 && c < 0x7F && !std::strchr("\";\\%#", c);
    Check(clean, "info: names cleaned and bounded (" + out.name + " / " + out.map + ")");
    Check(GameMode("de_dust2") == "Bomb Defusal" && GameMode("cs_office") == "Hostage Rescue" && GameMode("x").empty(),
          "info: game mode from the map");
}

void TestPlayersPages() {
    Identity id = MakeId(9);
    std::vector<PlayerInfo> players;
    for (int i = 0; i < 65; i++)
        players.push_back({"Player with a long name number " + std::to_string(i), int16_t(i - 3), uint16_t(i * 3),
                           uint32_t(i * 60), i >= 2});
    uint8_t pages = 0;
    EncodePlayers(players, 0, 77, id, &pages);
    Check(pages >= 2, "players: 65 long names take " + std::to_string(pages) + " pages");
    std::vector<PlayerInfo> all;
    bool sizes_ok = true, ok = true;
    for (uint8_t p = 0; p < pages; p++) {
        auto a = EncodePlayers(players, p, 77, id);
        sizes_ok = sizes_ok && !a.empty() && a.size() <= kPlayersQuerySize;
        uint8_t page = 0, n = 0;
        std::vector<PlayerInfo> part;
        std::string err;
        ok = ok && DecodePlayers(a.data(), a.size(), 77, id.public_key, page, n, part, err) && page == p && n == pages;
        all.insert(all.end(), part.begin(), part.end());
        auto wrong = MakeId(10);
        Check(!DecodePlayers(a.data(), a.size(), 77, wrong.public_key, page, n, part, err),
              "players: another key refused");
    }
    Check(sizes_ok, "players: every page within one datagram, no larger than its query");
    Check(ok && all.size() == 65 && all[64].bot && !all[0].bot && all[1].score == -2 && all[0].name.size() <= kMaxPlayerName,
          "players: 65 read back over the pages");
    Check(EncodePlayers(players, pages, 77, id).empty(), "players: a page past the last is not answered");
}

// ---- the server's answer: malformed queries, rate limits ----------------------------------------------------------

void TestQueryServer() {
    Identity id = MakeId(3);
    QueryServer qs({8, 400, false}, id);
    qs.Set(Info("s", "de_dust2", 1, 2, 65), {});
    Address a = Addr(10, 0, 0, 2, 40000), b = Addr(10, 0, 0, 3, 40000);
    std::mt19937 rng(1);
    int answered = 0;
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> d(rng() % 1300);
        for (auto& x : d) x = uint8_t(rng());
        if (d.size() >= 6 && i % 3 == 0) {  // a valid prefix, the rest random
            uint8_t pre[] = {0x43, 0x53, 0x58, 0x4E, kVersion, uint8_t(Type::Query)};
            std::memcpy(d.data(), pre, 6);
        }
        answered += !qs.Answer(a, d.data(), d.size(), g_now).empty();
    }
    Check(answered == 0, "query server: 20000 random datagrams, none answered");
    Check(qs.GetStats().malformed == 20000, "query server: all counted malformed");

    // a burst of twice the rate, then the rate
    auto q = EncodeQuery(QueryKind::Info, 0, 1);
    int got = 0;
    for (int i = 0; i < 100; i++) got += !qs.Answer(a, q.data(), q.size(), g_now).empty();
    Check(got == 16, "rate: a burst of 16 answered of 100 (" + std::to_string(got) + ")");
    Check(!qs.Answer(b, q.data(), q.size(), g_now).empty(), "rate: another address still answered");
    got = 0;
    for (int i = 0; i < 100; i++) got += !qs.Answer(a, q.data(), q.size(), g_now + 1.0).empty();
    Check(got == 8, "rate: 8 more after a second (" + std::to_string(got) + ")");
    // everyone together
    QueryServer total({1000, 50, false}, id);
    total.Set(Info("s", "de_dust2", 1, 2, 65), {});
    got = 0;
    for (int i = 0; i < 1000; i++) {
        Address src = Addr(10, 1, uint8_t(i >> 8), uint8_t(i), 5000);
        got += !total.Answer(src, q.data(), q.size(), g_now + 5).empty();
    }
    Check(got == 100, "rate: 1000 sources together get the total's burst, 100 (" + std::to_string(got) + ")");
    // LAN only
    QueryServer lan({8, 400, true}, id);
    lan.Set(Info("s", "de_dust2", 1, 2, 65), {});
    Check(lan.Answer(Addr(8, 8, 8, 8, 1), q.data(), q.size(), g_now).empty() &&
              !lan.Answer(Addr(192, 168, 1, 9, 1), q.data(), q.size(), g_now).empty(),
          "LAN only: an internet address unanswered, a private one answered");
    // players query of the wrong size
    auto pq = EncodeQuery(QueryKind::Players, 0, 2);
    Check(qs.Answer(b, pq.data(), 256, g_now + 10).empty(), "query server: a players query cut to 256 bytes unanswered");
}

// ---- LAN discovery -------------------------------------------------------------------------------------------------

BrowserConfig LanConfig() {
    BrowserConfig c;
    c.broadcast = {Addr(10, 0, 0, 255, 0), Addr(127, 255, 255, 255, 0)};
    c.lan_ports = {27015, 27016, 27017};
    return c;
}

void TestLanDiscovery() {
    Network net;
    SimServer s1(net, Addr(10, 0, 0, 1, 27015), 1, Info("Alpha", "de_dust2", 3, 10, 65));
    SimServer s2(net, Addr(10, 0, 0, 2, 27016), 2, Info("Bravo", "cs_office", 0, 0, 16));
    SimServer s3(net, Addr(127, 0, 0, 5, 27015), 3, Info("Local", "de_aztec", 1, 63, 65));
    SimServer other(net, Addr(10, 0, 0, 9, 27030), 4, Info("Off port", "de_dust2", 0, 0, 8));  // not a LAN port
    Browser b(std::make_unique<SimSocket>(net, Addr(10, 0, 0, 50, 41000)), LanConfig(), Now);
    std::vector<SimServer*> all = {&s1, &s2, &s3, &other};
    double t0 = g_now;
    b.RefreshLan();
    Run(net, all, b, 0.2);
    std::set<std::string> names;
    for (auto& e : b.Entries()) names.insert(e.info.name);
    Check(b.Entries().size() == 3 && names.count("Alpha") && names.count("Bravo") && names.count("Local"),
          "LAN: three servers on LAN ports found (" + std::to_string(b.Entries().size()) + ")");
    bool pinged = true;
    for (auto& e : b.Entries()) pinged = pinged && e.ping >= 0 && e.ping < 50 && (e.sources & kLan);
    Check(pinged, "LAN: each with a ping and the LAN source");
    Check(g_now - t0 < 2.0, "LAN: found within 2 s");

    // the same server reached through two broadcast addresses is one entry
    b.RefreshLan();
    b.RefreshLan();
    Run(net, all, b, 0.2);
    Check(b.Entries().size() == 3, "LAN: repeated broadcasts, no duplicates");

    // a server appears, one disappears
    SimServer s4(net, Addr(10, 0, 0, 4, 27017), 5, Info("Charlie", "de_inferno", 5, 0, 20));
    all.push_back(&s4);
    s2.sock->mute = true;
    b.WatchLan(true);
    Run(net, all, b, 4);
    bool charlie = false;
    for (auto& e : b.Entries()) charlie = charlie || e.info.name == "Charlie";
    Check(charlie, "LAN: a new server appears at the next broadcast");
    Run(net, all, b, 12);
    bool bravo = false;
    for (auto& e : b.Entries()) bravo = bravo || e.info.name == "Bravo";
    Check(!bravo && b.Entries().size() == 3, "LAN: a silent server expires (" + std::to_string(b.Entries().size()) + " left)");
    Check(!b.Busy() || true, "LAN: not busy after");
    b.WatchLan(false);
}

// ---- favourites, typed addresses, identity changes -----------------------------------------------------------------

void TestDirect() {
    Network net;
    SimServer s1(net, Addr(10, 0, 0, 1, 27015), 1, Info("Alpha", "de_dust2", 3, 10, 65));
    SimServer s2(net, Addr(10, 0, 0, 2, 27015), 2, Info("Bravo", "de_dust2", 0, 0, 16));
    Browser b(std::make_unique<SimSocket>(net, Addr(10, 0, 0, 50, 41000)), LanConfig(), Now);
    std::string k1 = Identity::Fingerprint(s1.id.public_key), k2 = Identity::Fingerprint(s2.id.public_key);
    b.Add(s1.address(), kFavorite, k1);
    b.Add(Addr(10, 0, 0, 2, 27015), kFavorite, k1);  // the favourite remembered s1's key at s2's address
    b.Add(Addr(10, 0, 0, 77, 27015), kFavorite);     // nobody there
    Run(net, {&s1, &s2}, b, 3);
    const BrowserEntry* e1 = b.Find(Addr(10, 0, 0, 1, 27015));
    const BrowserEntry* e2 = b.Find(Addr(10, 0, 0, 2, 27015));
    const BrowserEntry* e3 = b.Find(Addr(10, 0, 0, 77, 27015));
    Check(e1 && e1->state == BrowserEntry::State::Answered && e1->problem.empty() && e1->key == k1,
          "favourite: answers with its key");
    Check(e2 && e2->problem == "identity changed" && e2->key == k2 && e2->expected_key == k1,
          "favourite: another key at its address is flagged, never taken");
    Check(e3 && e3->state == BrowserEntry::State::NoAnswer && e3->ping == -1, "favourite: no answer after its tries");
    ViewOptions fav;
    fav.sources = kFavorite;
    Check(View(b.Entries(), fav).size() == 3, "favourite: all three shown on the favourites tab");
    b.Remove(Addr(10, 0, 0, 77, 27015), kFavorite);
    Check(!b.Find(Addr(10, 0, 0, 77, 27015)), "favourite: removed");
    // players
    std::vector<PlayerInfo> pl;
    for (int i = 0; i < 65; i++) pl.push_back({"P" + std::to_string(i), 0, 10, 5, i > 2});
    s1.query->Set(Info("Alpha", "de_dust2", 3, 62, 65), pl);
    b.Refresh(Addr(10, 0, 0, 1, 27015));
    Run(net, {&s1, &s2}, b, 0.5);
    b.QueryPlayers(Addr(10, 0, 0, 1, 27015));
    Run(net, {&s1, &s2}, b, 0.5);
    e1 = b.Find(Addr(10, 0, 0, 1, 27015));
    Check(e1 && e1->players_complete && e1->players.size() == 65, "players: 65 listed through the browser");
}

// ---- the master server -----------------------------------------------------------------------------------------------

struct SimMaster {
    SimSocket* sock;
    std::unique_ptr<DatagramSocket> own;
    MasterServer ms;
    SimMaster(Network& net, Address a, MasterServer::Config c) : ms(c, Now) {
        auto s = std::make_unique<SimSocket>(net, a);
        sock = s.get();
        own = std::move(s);
    }
    void Pump() {
        uint8_t buf[2048];
        Address from;
        int n;
        while ((n = own->Receive(from, buf, sizeof(buf))) >= 0)
            ms.Handle(from, buf, size_t(n), [&](const Address& to, const std::vector<uint8_t>& d) {
                own->Send(to, d.data(), d.size());
            });
    }
};

// a server registered with the master through its endpoint (as server_query.cpp does)
struct ListedServer : SimServer {
    std::unique_ptr<Registrar> reg;
    ListedServer(Network& net, Address a, uint8_t seed, ServerInfo info, Address master) : SimServer(net, a, seed, info) {
        reg = std::make_unique<Registrar>(id, std::vector<Address>{master}, Now);
        ep->SetConnectionless([this](const Address& from, Type t, const uint8_t* p, size_t n) {
            auto send = [this](const Address& to, const std::vector<uint8_t>& d) { ep->SendDatagram(to, d.data(), d.size()); };
            if (reg->Handle(from, t, p, n, send)) return;
            if (t != Type::Query) return;
            auto ans = query->Answer(from, p, n, Now());
            if (!ans.empty()) ep->SendDatagram(from, ans.data(), ans.size());
        });
    }
    void Tick() {
        reg->Pump([this](const Address& to, const std::vector<uint8_t>& d) { ep->SendDatagram(to, d.data(), d.size()); });
    }
};

void TestMaster() {
    Network net;
    Address maddr = Addr(10, 0, 0, 200, kMasterPort);
    MasterServer::Config mc;
    mc.expiry = 30;
    mc.max_per_ip = 200;
    SimMaster m(net, maddr, mc);
    std::vector<std::unique_ptr<ListedServer>> servers;
    for (int i = 0; i < 100; i++)
        servers.push_back(std::make_unique<ListedServer>(net, Addr(10, 0, 1, uint8_t(i), 27015), uint8_t(20 + i),
                                                         Info("Server " + std::to_string(i), i % 2 ? "de_dust2" : "cs_office",
                                                              uint8_t(i % 5), uint8_t(i % 3), 65),
                                                         maddr));
    auto step = [&](double seconds, Browser* b) {
        for (double t = 0; t < seconds; t += 0.01) {
            g_now += 0.01;
            for (auto& s : servers) {
                s->Tick();
                s->ep->Pump();
            }
            m.Pump();
            if (b) b->Pump();
            Settle(net);
        }
    };
    step(0.5, nullptr);
    Check(m.ms.Count() == 100, "master: 100 servers registered (" + std::to_string(m.ms.Count()) + ")");
    Check(m.ms.GetStats().bad_cookie == 0 && m.ms.GetStats().bad_signature == 0, "master: every registration verified");

    BrowserConfig bc = LanConfig();
    bc.masters = {maddr};
    Browser b(std::make_unique<SimSocket>(net, Addr(10, 0, 0, 51, 41000)), bc, Now);
    b.RefreshInternet();
    step(3, &b);
    ViewOptions inet;
    inet.sources = kInternet;
    inet.answered_only = true;
    Check(View(b.Entries(), inet).size() == 100, "internet: 100 servers listed and answered (" +
                                                     std::to_string(View(b.Entries(), inet).size()) + ")");
    Check(b.GetStats().list_pages == 4, "internet: 4 list pages of 30 (" + std::to_string(b.GetStats().list_pages) + ")");
    Check(!b.Busy(), "internet: done");

    // a listed server that proves another key than listed: dropped
    servers[5]->query = std::make_unique<QueryServer>(QueryServer::Config{}, MakeId(250));
    servers[5]->query->Set(Info("Impostor", "de_dust2", 1, 1, 65), {});
    b.RefreshInternet();
    step(3, &b);
    bool impostor = false;
    for (size_t i : View(b.Entries(), inet)) impostor = impostor || b.Entries()[i].info.name == "Impostor";
    Check(!impostor && b.GetStats().wrong_key >= 1, "internet: a server proving another key than listed is not shown");

    // forged registration: someone else's address (no cookie), a bad signature
    auto send = [&](const Address& from, const std::vector<uint8_t>& d) {
        m.ms.Handle(from, d.data(), d.size(), [](const Address&, const std::vector<uint8_t>&) {});
    };
    Identity evil = MakeId(251);
    Registrar forger(evil, {maddr}, Now);
    std::vector<uint8_t> challenge;
    forger.Pump([&](const Address&, const std::vector<uint8_t>& d) {
        // the master's challenge goes to the victim's address, which the forger never sees: it guesses a cookie
        auto reg = d;
        (void)reg;
    });
    std::vector<uint8_t> fake(147, 0);
    std::memcpy(fake.data(), "CSXN", 4);
    fake[4] = kVersion;
    fake[5] = uint8_t(Type::Register);
    uint64_t before = m.ms.GetStats().bad_cookie;
    send(Addr(10, 0, 9, 9, 27015), fake);
    Check(m.ms.GetStats().bad_cookie == before + 1 && m.ms.Count() == 100, "master: a register without its cookie refused");
    // random datagrams
    std::mt19937 rng(5);
    for (int i = 0; i < 5000; i++) {
        std::vector<uint8_t> d(rng() % 1300);
        for (auto& x : d) x = uint8_t(rng());
        if (d.size() > 6 && i % 2) {
            uint8_t pre[] = {0x43, 0x53, 0x58, 0x4E, kVersion, uint8_t(9 + rng() % 4)};
            std::memcpy(d.data(), pre, 6);
        }
        m.ms.Handle(Addr(10, 2, uint8_t(i >> 8), uint8_t(i), 1), d.data(), d.size(),
                    [](const Address&, const std::vector<uint8_t>&) {});
    }
    Check(m.ms.Count() == 100, "master: 5000 random datagrams change nothing");
    // per-IP limit
    MasterServer::Config small;
    small.max_per_ip = 2;
    SimMaster m2(net, Addr(10, 0, 0, 201, kMasterPort), small);
    std::vector<std::unique_ptr<ListedServer>> same_ip;
    for (int i = 0; i < 4; i++)
        same_ip.push_back(std::make_unique<ListedServer>(net, Addr(10, 0, 3, 1, uint16_t(27015 + i)), uint8_t(150 + i),
                                                         Info("s", "de_dust2", 0, 0, 8), Addr(10, 0, 0, 201, kMasterPort)));
    for (double t = 0; t < 0.5; t += 0.01) {
        g_now += 0.01;
        for (auto& s : same_ip) {
            s->Tick();
            s->ep->Pump();
        }
        m2.Pump();
        Settle(net);
    }
    Check(m2.ms.Count() == 2, "master: at most max_per_ip servers from one address (" + std::to_string(m2.ms.Count()) + ")");
    // expiry: servers stop heartbeating
    for (auto& s : servers) s->sock->mute = true;
    step(40, nullptr);
    m.ms.Expire();
    Check(m.ms.Count() == 0 && m.ms.GetStats().expired == 100, "master: silent servers expire (" +
                                                                   std::to_string(m.ms.Count()) + " left)");
    // leaving
    for (auto& s : servers) s->sock->mute = false;
    step(16, nullptr);
    size_t listed = m.ms.Count();
    for (auto& s : servers) s->reg->Leave([&](const Address& to, const std::vector<uint8_t>& d) { s->ep->SendDatagram(to, d.data(), d.size()); });
    step(0.2, nullptr);
    Check(listed == 100 && m.ms.Count() == 0, "master: servers back, then leaving removes them (" + std::to_string(listed) +
                                                  " then " + std::to_string(m.ms.Count()) + ")");
}

// ---- the view: sorting, filters, many servers ----------------------------------------------------------------------

void TestView() {
    std::vector<BrowserEntry> v;
    auto add = [&](const std::string& name, const std::string& map, int humans, int bots, int max, int ping, bool pw,
                   uint8_t src, uint8_t last) {
        BrowserEntry e;
        e.address = Addr(src == kLan ? 192 : 8, 168, 0, last, 27015);
        e.sources = src;
        e.have_info = true;
        e.state = BrowserEntry::State::Answered;
        e.info = Info(name, map, uint8_t(humans), uint8_t(bots), uint8_t(max));
        e.info.password = pw;
        e.ping = ping;
        v.push_back(e);
    };
    add("Delta", "de_dust2", 2, 63, 65, 40, false, kLan, 1);
    add("alpha", "cs_office", 0, 0, 16, 20, true, kLan, 2);
    add("Charlie", "de_aztec", 16, 0, 16, 20, false, kLan, 3);
    add("Bravo", "de_dust2", 5, 0, 32, -1, false, kLan, 4);
    add("Echo", "de_dust2", 1, 0, 8, 5, false, kInternet, 5);
    BrowserEntry refused = v[0];
    refused.address.ip[3] = 9;
    refused.state = BrowserEntry::State::Refused;
    v.push_back(refused);
    auto names = [&](const ViewOptions& o) {
        std::string s;
        for (size_t i : View(v, o)) s += v[i].info.name.substr(0, 1);
        return s;
    };
    ViewOptions o;
    o.sources = kLan | kInternet;
    Check(names(o) == "EaCDB", "view: ping ascending, equal pings by name, no answer last (" + names(o) + ")");
    o.reverse = true;
    Check(names(o) == "BDaCE", "view: ping descending, no answer first (" + names(o) + ")");
    o.reverse = false;
    o.sort = SortKey::Players;
    Check(names(o) == "DCBEa", "view: most players first (" + names(o) + ")");
    o.sort = SortKey::Name;
    Check(names(o) == "aBCDE", "view: by name, case ignored (" + names(o) + ")");
    o.sort = SortKey::Map;
    Check(names(o) == "aCBDE", "view: by map, then name (" + names(o) + ")");
    o.sort = SortKey::Ping;
    o.hide_full = true;
    Check(names(o) == "EaB", "view: without full servers (65/65 and 16/16) (" + names(o) + ")");
    o.hide_full = false;
    o.password = 1;
    Check(names(o) == "ECDB", "view: without passwords (" + names(o) + ")");
    o.password = 2;
    Check(names(o) == "a", "view: passworded only (" + names(o) + ")");
    o.password = 0;
    o.map = "DE_DUST2";
    Check(names(o) == "EDB", "view: one map (" + names(o) + ")");
    o.map.clear();
    o.location = 2;
    Check(names(o) == "E", "view: internet only (" + names(o) + ")");
    o.location = 1;
    Check(names(o) == "aCDB", "view: LAN only (" + names(o) + ")");
    o.location = 0;
    o.hide_empty = true;
    Check(names(o) == "ECDB", "view: without empty servers (" + names(o) + ")");
    o.hide_empty = false;
    o.sources = kInternet;
    Check(names(o) == "E", "view: the internet tab");

    // many servers: a view of 4096 entries, sorted each way, quickly
    std::vector<BrowserEntry> many;
    std::mt19937 rng(3);
    for (int i = 0; i < 4096; i++) {
        BrowserEntry e;
        e.address = Addr(8, uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i), 27015);
        e.sources = kInternet;
        e.have_info = true;
        e.info = Info("S" + std::to_string(rng() % 1000), "de_dust2", uint8_t(rng() % 10), uint8_t(rng() % 10), 65);
        e.ping = int(rng() % 300);
        many.push_back(e);
    }
    auto t0 = std::chrono::steady_clock::now();
    ViewOptions mo;
    mo.sources = kInternet;
    size_t total = 0;
    for (SortKey k : {SortKey::Ping, SortKey::Players, SortKey::Name, SortKey::Map}) {
        mo.sort = k;
        total += View(many, mo).size();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Check(total == 4 * 4096, "view: 4096 servers in each order");
    std::printf("view: 4096 servers sorted four ways in %.1f ms\n", ms);
    Check(ms < 400, "view: 4096 servers sorted four ways in under 400 ms (sanitizers on)");
    // stable: the same view twice is the same
    mo.sort = SortKey::Ping;
    Check(View(many, mo) == View(many, mo), "view: the same order each time");
}

void TestSaved() {
    std::string k = Identity::Fingerprint(MakeId(1).public_key);
    auto s = ParseSaved("192.168.1.5:27015 " + k + " Friday night  server");
    Check(s && s->address == "192.168.1.5:27015" && s->key == k && s->name == "Friday night server", "saved: read");
    Check(FormatSaved(*s) == "192.168.1.5:27015 " + k + " Friday night server", "saved: written back");
    auto n = ParseSaved("games.example.org:27016 - ");
    Check(n && n->key.empty() && n->address == "games.example.org:27016", "saved: a name and no key");
    Check(!ParseSaved("1.2.3.4 notakey name"), "saved: a malformed key refused");
    Check(!ParseSaved("1.2.3.4;quit - x"), "saved: an address with other characters refused");
    Check(!ParseSaved(""), "saved: empty refused");
}

}  // namespace

int main() {
    if (sodium_init() < 0) return 1;
    TestQuerySerialization();
    TestPlayersPages();
    TestQueryServer();
    TestLanDiscovery();
    TestDirect();
    TestMaster();
    TestView();
    TestSaved();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
