// The PC transport (src/net/transport.h) on its own, over a simulated network with a simulated clock
// (tests/net_transport_test.py builds and runs it): the handshake, messages of every size up to 64 KB, loss,
// duplication, reordering and delay (seeded, repeatable), fragment loss and reassembly limits, replay and
// tampering, spoofed and stale cookies, address binding, identity, rejects, timeouts, disconnects and the rate limit.
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <sodium.h>
#include <sys/stat.h>

#include "net/transport.h"

using namespace net;

namespace {
int g_failed = 0, g_checks = 0;

void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

double g_now = 0;
double Now() { return g_now; }

// the simulated network: datagrams queued per destination with a delivery time; loss, duplication, reordering
// (random extra delay) and a hook that can see and change each datagram
struct Network {
    struct Packet {
        double at;
        Address from, to;
        std::vector<uint8_t> data;
    };
    std::vector<Packet> flight;
    std::mt19937 rng{1234};
    double loss = 0, dup = 0, jitter = 0, delay = 0.001;
    std::function<bool(Packet&)> tap;  // false drops the datagram
    uint64_t sent = 0, largest = 0;

    void Send(const Address& from, const Address& to, const uint8_t* d, size_t n) {
        sent++;
        largest = std::max<uint64_t>(largest, n);
        std::uniform_real_distribution<double> u(0, 1);
        if (u(rng) < loss) return;
        int copies = u(rng) < dup ? 2 : 1;
        for (int i = 0; i < copies; i++) {
            Packet p{g_now + delay + u(rng) * jitter, from, to, std::vector<uint8_t>(d, d + n)};
            if (tap && !tap(p)) continue;
            flight.push_back(std::move(p));
        }
    }
    bool Take(const Address& to, Packet& out) {
        size_t best = flight.size();
        for (size_t i = 0; i < flight.size(); i++)
            if (flight[i].to == to && flight[i].at <= g_now && (best == flight.size() || flight[i].at < flight[best].at))
                best = i;
        if (best == flight.size()) return false;
        out = std::move(flight[best]);
        flight.erase(flight.begin() + long(best));
        return true;
    }
};

class SimSocket : public DatagramSocket {
public:
    SimSocket(Network& net, Address me) : net_(net), me_(me) {}
    bool Send(const Address& to, const uint8_t* data, size_t len) override {
        if (len > kMaxDatagram) oversize++;
        net_.Send(me_, to, data, len);
        return true;
    }
    int Receive(Address& from, uint8_t* buf, size_t max) override {
        Network::Packet p;
        if (!net_.Take(me_, p)) return -1;
        if (p.data.size() > max) return -1;
        if (!p.data.empty()) std::memcpy(buf, p.data.data(), p.data.size());
        from = p.from;
        return int(p.data.size());
    }
    Address Local() const override { return me_; }
    int oversize = 0;

private:
    Network& net_;
    Address me_;
};

Address Addr(int last, uint16_t port) {
    Address a;
    a.ip = {10, 0, 0, uint8_t(last)};
    a.port = port;
    return a;
}

struct Pair {
    Network net;
    Address server_addr = Addr(1, 27015), client_addr = Addr(2, 40000);
    SimSocket* server_sock;
    SimSocket* client_sock;
    std::unique_ptr<Endpoint> server, client;
    Pair(Config sc = {}, Config cc = {}) {
        g_now = 100;
        auto ss = std::make_unique<SimSocket>(net, server_addr);
        auto cs = std::make_unique<SimSocket>(net, client_addr);
        server_sock = ss.get();
        client_sock = cs.get();
        server = std::make_unique<Endpoint>(Endpoint::Role::Server, std::move(ss), sc, Now);
        client = std::make_unique<Endpoint>(Endpoint::Role::Client, std::move(cs), cc, Now);
    }
    // advances the clock in steps, pumping both
    void Run(double seconds, double step = 0.005) {
        for (double t = 0; t < seconds; t += step) {
            g_now += step;
            server->Pump();
            client->Pump();
        }
    }
};

std::vector<uint8_t> Pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 r(seed);
    for (auto& b : v) b = uint8_t(r());
    return v;
}

// sends `count` messages of the given sizes client -> server and back, returns how many arrived intact each way
std::pair<int, int> Exchange(Pair& p, const std::vector<size_t>& sizes, double settle = 2.0) {
    std::map<std::vector<uint8_t>, int> want_server, want_client;
    uint32_t seed = 1;
    for (size_t n : sizes) {
        auto a = Pattern(n, seed++), b = Pattern(n, seed++);
        p.client->Send(p.server_addr, a.data(), a.size());
        p.server->Send(p.client_addr, b.data(), b.size());
        want_server[a]++;
        want_client[b]++;
        p.Run(0.01);
    }
    p.Run(settle);
    int got_s = 0, got_c = 0;
    Address from;
    std::vector<uint8_t> m;
    while (p.server->Receive(from, m))
        if (want_server[m]-- > 0) got_s++;
    while (p.client->Receive(from, m))
        if (want_client[m]-- > 0) got_c++;
    return {got_s, got_c};
}

void Connect(Pair& p) {
    uint8_t hi[] = {1, 2, 3};
    p.client->Send(p.server_addr, hi, sizeof(hi));
    p.Run(0.5);
    Address from;
    std::vector<uint8_t> m;
    p.server->Receive(from, m);
}

void TestHandshake() {
    Pair p;
    uint8_t hi[] = {0xFF, 0xFF, 0xFF, 0xFF, 'g', 'e', 't'};
    Check(p.client->Send(p.server_addr, hi, sizeof(hi)), "handshake: the first packet is queued");
    p.Run(0.2);
    Check(p.client->Connected(p.server_addr) && p.server->Connected(p.client_addr), "handshake: both connected");
    Address from;
    std::vector<uint8_t> m;
    Check(p.server->Receive(from, m) && m == std::vector<uint8_t>(hi, hi + sizeof(hi)) && from == p.client_addr,
          "handshake: the queued packet arrives after it");
    Endpoint::Event e;
    bool connected = false;
    while (p.server->PollEvent(e)) connected |= e.kind == Endpoint::Event::Connected;
    Check(connected, "handshake: the server reports the connection");
    Check(p.net.largest <= kMaxDatagram, "handshake: no datagram over 1200 bytes");
    Check(p.client_sock->oversize == 0 && p.server_sock->oversize == 0, "handshake: nothing oversized sent");
    Check(p.server->GetStats().handshakes_ok == 1, "handshake: counted once");
}

void TestSizes() {
    Pair p;
    Connect(p);
    std::vector<size_t> sizes = {1, 16, 100, kMaxFragment - 1, kMaxFragment, kMaxFragment + 1, 2 * kMaxFragment,
                                 4000, 8643, 20000, 60000, kMaxMessage - 1, kMaxMessage};
    auto [s, c] = Exchange(p, sizes);
    Check(s == int(sizes.size()) && c == int(sizes.size()),
          "sizes: every size up to 64 KB arrives intact (" + std::to_string(s) + "/" + std::to_string(c) + ")");
    Check(p.net.largest <= kMaxDatagram, "sizes: no datagram over 1200 bytes (largest " + std::to_string(p.net.largest) + ")");
    std::vector<uint8_t> big(kMaxMessage + 1, 7);
    Check(!p.client->Send(p.server_addr, big.data(), big.size()), "sizes: a message over 64 KB is refused");
    Check(!p.client->Send(p.server_addr, big.data(), 0), "sizes: an empty message is refused");
}

void TestImpairment(double loss, double dup, double jitter, const std::string& name) {
    Pair p;
    Connect(p);
    p.net.loss = loss;
    p.net.dup = dup;
    p.net.jitter = jitter;
    std::vector<size_t> sizes;
    for (int i = 0; i < 300; i++) sizes.push_back(i % 10 == 0 ? 6000 : 40 + (i * 37) % 1000);
    auto [s, c] = Exchange(p, sizes, 3.0);
    // nothing arrives twice or corrupted (Exchange counts only intact first copies); the expected share arrives
    int n = int(sizes.size());
    double expect = 1.0;
    if (loss > 0) expect = 0.5;  // multi-fragment messages lose more: a loose floor, the exact rate is in the log
    Check(s >= int(n * expect) && c >= int(n * expect),
          name + ": delivered " + std::to_string(s) + " and " + std::to_string(c) + " of " + std::to_string(n));
    Address from;
    std::vector<uint8_t> m;
    int extra = 0;
    while (p.server->Receive(from, m)) extra++;
    Check(extra == 0, name + ": no duplicate delivered");
    std::printf("  %s: %d/%d and %d/%d delivered, replays dropped %llu, lost seen %llu\n", name.c_str(), s, n, c, n,
                (unsigned long long)p.server->GetStats().dropped_replay,
                (unsigned long long)p.server->GetStats().lost_in);
    Check(p.client->Connected(p.server_addr), name + ": still connected");
}

void TestHandshakeUnderLoss() {
    int ok = 0;
    for (int seed = 0; seed < 20; seed++) {
        Pair p;
        p.net.rng.seed(seed);
        p.net.loss = 0.3;
        uint8_t hi[] = {1};
        p.client->Send(p.server_addr, hi, 1);
        p.Run(6.0, 0.01);
        ok += p.client->Connected(p.server_addr);
    }
    Check(ok >= 18, "handshake under 30% loss connects (" + std::to_string(ok) + "/20)");
}

void TestFragmentLoss() {
    Pair p;
    Connect(p);
    // drop exactly one fragment (index 2) of the first large message: it never arrives, the next one does
    int dropped = 0;
    p.net.tap = [&](Network::Packet& pk) {
        if (pk.data.size() == kMaxDatagram && pk.from == p.client_addr && dropped == 0) {
            static int seen = 0;
            if (++seen == 3) {
                dropped++;
                return false;
            }
        }
        return true;
    };
    auto a = Pattern(10000, 9), b = Pattern(10000, 10);
    p.client->Send(p.server_addr, a.data(), a.size());
    p.Run(0.1);
    p.client->Send(p.server_addr, b.data(), b.size());
    p.Run(2.0);
    Address from;
    std::vector<uint8_t> m;
    int got_a = 0, got_b = 0;
    while (p.server->Receive(from, m)) got_a += m == a, got_b += m == b;
    Check(dropped == 1 && got_a == 0 && got_b == 1, "fragment loss: the message without one fragment is dropped, the next arrives");
    Check(p.server->GetStats().reassembly_timeouts >= 1, "fragment loss: the partial message times out");
}

void TestReassembler() {
    Reassembler r(2, 4 * kMaxFragment, 1.0);
    std::vector<uint8_t> full(kMaxFragment, 1), out;
    using R = Reassembler::Result;
    Check(r.Add(1, 0, 0, full.data(), full.size(), 0, out) == R::Invalid, "reassembler: count 0 invalid");
    Check(r.Add(1, 3, 3, full.data(), full.size(), 0, out) == R::Invalid, "reassembler: index past count invalid");
    Check(r.Add(1, 0, uint8_t(kMaxFragments + 1), full.data(), full.size(), 0, out) == R::Invalid, "reassembler: too many fragments invalid");
    Check(r.Add(1, 0, 2, full.data(), 10, 0, out) == R::Invalid, "reassembler: a short fragment before the last invalid");
    Check(r.Add(1, 0, 2, full.data(), kMaxFragment + 1, 0, out) == R::Invalid, "reassembler: an oversized fragment invalid");
    Check(r.Add(1, 0, 2, full.data(), full.size(), 0, out) == R::Partial, "reassembler: first of two partial");
    Check(r.Add(1, 0, 2, full.data(), full.size(), 0, out) == R::Duplicate, "reassembler: the same fragment again is a duplicate");
    Check(r.Add(1, 1, 3, full.data(), full.size(), 0, out) == R::Invalid, "reassembler: another count for the same id invalid");
    Check(r.Messages() == 0 && r.Buffered() == 0, "reassembler: the inconsistent message is dropped whole");
    Check(r.Add(2, 0, 2, full.data(), full.size(), 0, out) == R::Partial, "reassembler: message 2 partial");
    Check(r.Add(3, 0, 2, full.data(), full.size(), 0.1, out) == R::Partial, "reassembler: message 3 partial");
    Check(r.Add(4, 0, 2, full.data(), full.size(), 0.2, out) == R::Partial && r.Messages() == 2,
          "reassembler: a third message evicts the oldest (2 kept)");
    Check(r.Add(2, 1, 2, full.data(), 5, 0.3, out) == R::Partial, "reassembler: the evicted message starts again");
    Check(r.Expire(5.0) == 2 && r.Messages() == 0 && r.Buffered() == 0, "reassembler: expired messages free their bytes");
    Reassembler small(8, 2 * kMaxFragment, 1.0);
    Check(small.Add(1, 0, 3, full.data(), full.size(), 0, out) == R::Partial &&
              small.Add(1, 1, 3, full.data(), full.size(), 0, out) == R::Partial &&
              small.Add(2, 0, 3, full.data(), full.size(), 0, out) == R::Overflow,
          "reassembler: the byte cap refuses more");
    Reassembler one(8, 1 << 20, 1.0);
    std::vector<uint8_t> tail(100, 2);
    Check(one.Add(9, 0, 2, full.data(), full.size(), 0, out) == R::Partial &&
              one.Add(9, 1, 2, tail.data(), tail.size(), 0, out) == R::Complete && out.size() == kMaxFragment + 100,
          "reassembler: two fragments make the message");
}

void TestReplayWindow() {
    ReplayWindow w;
    Check(!w.Accept(0), "replay: number 0 refused");
    Check(w.Accept(1) && !w.Accept(1), "replay: a repeat refused");
    Check(w.Accept(5) && w.Accept(3) && !w.Accept(3), "replay: out of order once");
    Check(w.Accept(100) && !w.Accept(36) && w.Accept(37), "replay: older than 64 refused, within accepted");
    Check(w.Accept(1000000) && !w.Accept(100) && w.Fresh(999999) && !w.Fresh(1000000), "replay: a jump clears the window");
    ReplayWindow l;
    for (uint64_t n = 1; n <= 200; n++)
        if (n % 10) l.Accept(n);
    Check(l.lost == 13, "replay: 13 numbers left the window unseen (" + std::to_string(l.lost) + ")");
    ReplayWindow o;
    for (uint64_t n = 1; n <= 200; n += 2) o.Accept(n + 1), o.Accept(n);
    Check(o.lost == 0, "replay: swapped pairs are reordering, not loss (" + std::to_string(o.lost) + ")");
}

void TestTamper() {
    Pair p;
    Connect(p);
    p.net.tap = [&](Network::Packet& pk) {
        if (pk.from == p.client_addr && pk.data.size() > 40) pk.data[30] ^= 1;  // one bit in the ciphertext
        return true;
    };
    auto a = Pattern(500, 3);
    p.client->Send(p.server_addr, a.data(), a.size());
    p.Run(0.2);
    Address from;
    std::vector<uint8_t> m;
    Check(!p.server->Receive(from, m), "tamper: a changed datagram is not delivered");
    Check(p.server->GetStats().dropped_auth >= 1, "tamper: counted as failed authentication");
}

void TestReplayAttack() {
    Pair p;
    Connect(p);
    std::vector<Network::Packet> captured;
    p.net.tap = [&](Network::Packet& pk) {
        if (pk.from == p.client_addr) captured.push_back(pk);
        return true;
    };
    auto a = Pattern(300, 4);
    p.client->Send(p.server_addr, a.data(), a.size());
    p.Run(0.2);
    p.net.tap = nullptr;
    Address from;
    std::vector<uint8_t> m;
    int got = 0;
    while (p.server->Receive(from, m)) got++;
    for (auto& c : captured) p.net.flight.push_back(c);  // replayed as they were
    p.Run(0.2);
    int again = 0;
    while (p.server->Receive(from, m)) again++;
    Check(got == 1 && again == 0, "replay attack: a replayed datagram is not delivered again");
    // the same datagrams from another address (a spoofer's): dropped by the address binding
    for (auto& c : captured) {
        c.from = Addr(66, 5555);
        c.at = g_now;
        p.net.flight.push_back(c);
    }
    p.Run(0.2);
    Check(!p.server->Receive(from, m) && p.server->GetStats().dropped_address >= 1, "replay attack: another address is not the connection");
}

void TestSpoofedConnect() {
    Pair p;
    // a connect with a made-up cookie from an address that never got one
    uint8_t pkt[6 + 2 + 8 + 4 + 32 + 32] = {};
    uint32_t magic = kMagic;
    std::memcpy(pkt, &magic, 4);
    pkt[4] = kVersion;
    pkt[5] = uint8_t(Type::Connect);
    p.server_sock->Send(p.server_addr, pkt, sizeof(pkt));  // (sent to itself: only to reach its queue)
    p.net.flight.back().from = Addr(77, 1000);
    p.Run(0.1);
    Check(p.server->ConnectionCount() == 0 && p.server->GetStats().dropped_auth == 1, "spoof: a connect without a valid cookie is ignored");
    // a hello shorter than a full datagram gets no answer (no amplification)
    uint8_t hello[6 + 2 + 8] = {};
    std::memcpy(hello, &magic, 4);
    hello[4] = kVersion;
    hello[5] = uint8_t(Type::Hello);
    size_t before = p.net.sent;
    p.server_sock->Send(p.server_addr, hello, sizeof(hello));
    p.net.flight.back().from = Addr(77, 1000);
    p.Run(0.1);
    Check(p.net.sent == before + 1, "spoof: a short hello gets no answer");
}

void TestStaleCookie() {
    Config sc;
    sc.cookie_lifetime = 2;
    Pair p(sc);
    // the challenge is held back until the cookie is stale: the connect is ignored, the client retries with a new hello
    std::vector<Network::Packet> held;
    p.net.tap = [&](Network::Packet& pk) {
        if (pk.data.size() > 5 && pk.data[5] == uint8_t(Type::Challenge) && held.empty()) {
            held.push_back(pk);
            return false;
        }
        return true;
    };
    uint8_t hi[] = {1};
    p.client->Send(p.server_addr, hi, 1);
    p.Run(5.0, 0.01);
    Check(p.client->Connected(p.server_addr), "stale cookie: the handshake still completes on a retry");
}

void TestIdentity() {
    std::string err;
    std::string path = "/tmp/csxn_test_identity.key";
    std::remove(path.c_str());
    auto id = Identity::LoadOrCreate(path, err);
    auto again = Identity::LoadOrCreate(path, err);
    struct stat st {};
    Check(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "identity: the key file is readable by its owner only");
    Check(id && again && id->public_key == again->public_key, "identity: created once, loaded again");
    std::remove(path.c_str());
    {
        Config sc;
        sc.require_identity = true;
        Pair p(sc);
        uint8_t hi[] = {1};
        p.client->Send(p.server_addr, hi, 1);
        p.Run(1.0);
        Endpoint::Event e;
        bool refused = false;
        while (p.client->PollEvent(e)) refused |= e.kind == Endpoint::Event::Closed && e.reason == Reason::IdentityRequired;
        Check(refused && !p.client->Connected(p.server_addr), "identity: a server that requires one refuses a client without");
    }
    {
        Config sc;
        sc.require_identity = true;
        Pair p(sc);
        p.client->SetIdentity(id);
        uint8_t hi[] = {1};
        p.client->Send(p.server_addr, hi, 1);
        p.Run(1.0);
        auto fp = p.server->PeerIdentity(p.client_addr);
        Check(fp && *fp == Identity::Fingerprint(id->public_key), "identity: the server sees the client's fingerprint");
    }
    {
        // a signature for another handshake: the connect's cookie is changed after signing
        Pair p;
        p.client->SetIdentity(id);
        p.net.tap = [&](Network::Packet& pk) {
            if (pk.data.size() > 5 && pk.data[5] == uint8_t(Type::Connect)) pk.data[6 + 2 + 8 + 4 + 32 + 5] ^= 1;  // client key
            return true;
        };
        uint8_t hi[] = {1};
        p.client->Send(p.server_addr, hi, 1);
        p.Run(1.0);
        Check(!p.server->Connected(p.client_addr), "identity: a signature over other keys is refused");
    }
}

void TestTimeoutAndDisconnect() {
    Config c;
    c.idle_timeout = 3;
    {
        Pair p(c, c);
        Connect(p);
        p.net.loss = 1.0;  // the link goes dead
        p.Run(4.0, 0.01);
        Endpoint::Event e;
        bool timed = false;
        while (p.server->PollEvent(e)) timed |= e.kind == Endpoint::Event::Closed && e.reason == Reason::Timeout;
        Check(timed && !p.server->Connected(p.client_addr), "timeout: a dead link times out");
    }
    {
        Pair p(c, c);
        Connect(p);
        p.Run(5.0, 0.01);  // idle but alive: keepalives
        Check(p.server->Connected(p.client_addr) && p.client->Connected(p.server_addr), "keepalive: an idle link stays up");
        Check(p.client->Rtt(p.server_addr) > 0, "keepalive: the round trip is measured");
    }
    {
        Pair p;
        Connect(p);
        p.server->Disconnect(p.client_addr, Reason::MapChange, "de_dust2");
        p.Run(0.2);
        Endpoint::Event e;
        bool got = false;
        std::string text;
        while (p.client->PollEvent(e))
            if (e.kind == Endpoint::Event::Closed) got = e.reason == Reason::MapChange && e.remote_closed, text = e.text;
        Check(got && text == "de_dust2" && !p.client->Connected(p.server_addr), "disconnect: the reason and text reach the peer");
        // a server that closed us is not reconnected to at once (the engine's leftover packets), but after the hold
        uint8_t hi[] = {1};
        Check(!p.client->Send(p.server_addr, hi, 1), "reconnect: held right after the server closed the connection");
        p.Run(6.0, 0.01);
        p.client->Send(p.server_addr, hi, 1);
        p.Run(0.5);
        Check(p.client->Connected(p.server_addr), "reconnect: a new connection after a disconnect");
    }
    {
        Pair p;
        p.server.reset();  // nobody listens
        uint8_t hi[] = {1};
        p.client->Send(p.server_addr, hi, 1);
        g_now += 0.001;
        for (int i = 0; i < 800; i++) {
            g_now += 0.01;
            p.client->Pump();
        }
        Endpoint::Event e;
        bool failed = false;
        while (p.client->PollEvent(e)) failed |= e.kind == Endpoint::Event::Closed && e.reason == Reason::HandshakeFailed;
        Check(failed, "connect: no server answers -> handshake failed after the retries");
    }
}

void TestFull() {
    Config sc;
    sc.max_connections = 2;
    Network net;
    g_now = 100;
    auto ss = std::make_unique<SimSocket>(net, Addr(1, 27015));
    Endpoint server(Endpoint::Role::Server, std::move(ss), sc, Now);
    std::vector<std::unique_ptr<Endpoint>> clients;
    for (int i = 0; i < 3; i++)
        clients.push_back(std::make_unique<Endpoint>(Endpoint::Role::Client,
                                                     std::make_unique<SimSocket>(net, Addr(10 + i, 4000)), Config{}, Now));
    uint8_t hi[] = {1};
    for (auto& c : clients) c->Send(Addr(1, 27015), hi, 1);
    for (int i = 0; i < 100; i++) {
        g_now += 0.01;
        server.Pump();
        for (auto& c : clients) c->Pump();
    }
    int connected = 0, full = 0;
    for (auto& c : clients) {
        connected += c->Connected(Addr(1, 27015));
        Endpoint::Event e;
        while (c->PollEvent(e)) full += e.kind == Endpoint::Event::Closed && e.reason == Reason::Full;
    }
    Check(connected == 2 && full == 1, "full: the third client is refused with a reason");
}

void TestRateLimit() {
    Pair p;
    uint8_t hello[kMaxDatagram] = {};
    uint32_t magic = kMagic;
    std::memcpy(hello, &magic, 4);
    hello[4] = kVersion;
    hello[5] = uint8_t(Type::Hello);
    size_t before = p.net.sent;
    for (int i = 0; i < 1000; i++) {
        p.net.Send(Addr(88, 1), p.server_addr, hello, sizeof(hello));
    }
    p.Run(0.05);
    uint64_t answers = p.net.sent - before - 1000;
    Check(answers <= 45 && p.server->GetStats().dropped_rate >= 900, "rate: a flood of hellos from one address gets at most the burst answered (" + std::to_string(answers) + ")");
}

void TestMalformed() {
    Pair p;
    Connect(p);
    std::mt19937 r(77);
    for (int i = 0; i < 20000; i++) {
        size_t n = r() % (kMaxDatagram + 200);
        std::vector<uint8_t> d(n);
        for (auto& b : d) b = uint8_t(r());
        if (n >= 6 && i % 2) {  // half of them with a valid prefix, so they reach the type handlers
            uint32_t magic = kMagic;
            std::memcpy(d.data(), &magic, 4);
            d[4] = kVersion;
            d[5] = uint8_t(1 + r() % 6);
        }
        p.net.Send(i % 3 ? p.client_addr : Addr(99, 99), p.server_addr, d.data(), d.size());
        p.net.Send(p.server_addr, p.client_addr, d.data(), d.size());
    }
    p.Run(0.5);
    Address from;
    std::vector<uint8_t> m;
    int delivered = 0;
    while (p.server->Receive(from, m)) delivered++;
    while (p.client->Receive(from, m)) delivered++;
    Check(delivered == 0, "malformed: 40000 random datagrams deliver nothing");
    Check(p.client->Connected(p.server_addr) && p.server->ConnectionCount() == 1, "malformed: the connection survives them");
    auto [s, c] = Exchange(p, {100, 5000});
    Check(s == 2 && c == 2, "malformed: traffic still flows after them");
}

// security review 2026-10-25: a reconnect to the same server while the old connection is still closing replaces it
// (the client's id table kept the freed connection: a late packet of the old one was handled through it), and the
// server answers the new connect with new keys instead of the closing connection's erased ones
void TestReconnectWhileClosing() {
    Pair p;
    Connect(p);
    // a datagram of the old connection, held back and delivered after the reconnect
    std::vector<Network::Packet> held;
    p.net.tap = [&](Network::Packet& pk) {
        if (pk.from == p.server_addr && pk.data.size() > 5 && pk.data[5] == uint8_t(Type::Data) && held.empty()) {
            held.push_back(pk);
            return false;
        }
        return true;
    };
    uint8_t x[] = {9};
    p.server->Send(p.client_addr, x, 1);
    p.Run(0.05);
    p.net.tap = nullptr;
    p.client->DisconnectAll(Reason::Closed);  // the engine's disconnect; the connection stays a moment as closing
    p.Run(0.05);
    uint8_t hi[] = {1, 2};
    p.client->Send(p.server_addr, hi, sizeof(hi));  // connect again at once (retry)
    p.Run(0.5);
    Check(p.client->Connected(p.server_addr) && p.server->Connected(p.client_addr),
          "reconnect while closing: connected again at once");
    Check(held.size() == 1, "reconnect while closing: an old data packet was held");
    uint64_t unknown = p.client->GetStats().dropped_unknown;
    for (auto& pk : held) {
        pk.at = g_now;
        p.net.flight.push_back(pk);
    }
    p.Run(0.05);
    Check(p.client->GetStats().dropped_unknown == unknown + 1, "reconnect while closing: the old connection's packet is unknown");
    auto [s, c] = Exchange(p, {10, 3000});
    Check(s == 2 && c == 2, "reconnect while closing: the new connection carries messages both ways");
}

// a client process restarted on the same address and port (new keys) is a new connection at once, not the old one
// re-accepted with keys it no longer has
void TestClientRestartSamePort() {
    Pair p;
    Connect(p);
    p.client = std::make_unique<Endpoint>(Endpoint::Role::Client, std::make_unique<SimSocket>(p.net, p.client_addr),
                                          Config{}, Now);
    uint8_t hi[] = {3};
    p.client->Send(p.server_addr, hi, 1);
    p.Run(0.5);
    Check(p.client->Connected(p.server_addr), "restart on the same port: connects at once");
    auto [s, c] = Exchange(p, {100});
    Check(s >= 1 && c == 1, "restart on the same port: messages flow");
}

// a peer told to go (kicked) that keeps sending is removed anyway: its packets do not keep the closing connection
void TestClosingPeerKeepsSending() {
    Pair p;
    Connect(p);
    p.net.tap = [&](Network::Packet& pk) { return !(pk.from == p.server_addr); };  // it never hears the server
    p.server->Disconnect(p.client_addr, Reason::Kicked, "kicked");
    uint8_t x[] = {5};
    for (int i = 0; i < 300; i++) {
        p.client->Send(p.server_addr, x, 1);
        p.Run(0.01, 0.01);
    }
    Check(p.server->ConnectionCount() == 0, "kicked peer that keeps sending: removed after the closing time");
}

// a reject is believed only with the handshake's nonce (an off-path forger does not have it)
void TestForgedReject() {
    Pair p;
    uint8_t forged[kPrefix + 1 + 8 + 1] = {};
    uint32_t magic = kMagic;
    std::memcpy(forged, &magic, 4);
    forged[4] = kVersion;
    forged[5] = uint8_t(Type::Reject);
    forged[6] = uint8_t(Reason::Full);
    uint8_t hi[] = {1};
    p.client->Send(p.server_addr, hi, 1);
    for (int i = 0; i < 20; i++) {  // throughout the handshake, with a guessed nonce of 0
        p.net.Send(p.server_addr, p.client_addr, forged, sizeof(forged));
        p.Run(0.005);
    }
    p.Run(0.3);
    Check(p.client->Connected(p.server_addr), "forged reject: ignored, the handshake completes");
    Check(p.client->GetStats().dropped_auth >= 1, "forged reject: counted as failed authentication");
}

// hellos from very many (spoofed) sources: bounded work and memory, at most the global budget answered
void TestFloodManySources() {
    Pair p;
    uint8_t hello[kMaxDatagram] = {};
    uint32_t magic = kMagic;
    std::memcpy(hello, &magic, 4);
    hello[4] = kVersion;
    hello[5] = uint8_t(Type::Hello);
    size_t before = p.net.sent;
    for (int i = 0; i < 20000; i++) {
        Address a;
        a.ip = {uint8_t(20 + i / 65536), uint8_t(i / 256), uint8_t(i), 7};
        a.port = 5000;
        p.net.Send(a, p.server_addr, hello, sizeof(hello));
    }
    p.Run(0.05);
    uint64_t answers = p.net.sent - before - 20000;
    Check(answers <= 1100, "flood from many sources: at most the global burst answered (" + std::to_string(answers) + ")");
    Connect(p);
    p.Run(3.0, 0.01);  // the budget refills
    Check(p.client->Connected(p.server_addr), "flood from many sources: a real client connects once it passes");
}

void TestManyClients() {
    Network net;
    g_now = 100;
    Endpoint server(Endpoint::Role::Server, std::make_unique<SimSocket>(net, Addr(1, 27015)), Config{}, Now);
    std::vector<std::unique_ptr<Endpoint>> clients;
    for (int i = 0; i < 64; i++)
        clients.push_back(std::make_unique<Endpoint>(
            Endpoint::Role::Client, std::make_unique<SimSocket>(net, Address{{10, 1, uint8_t(i / 8), uint8_t(i % 8 + 1)}, 5000}),
            Config{}, Now));
    uint8_t hi[] = {1};
    for (auto& c : clients) c->Send(Addr(1, 27015), hi, 1);
    for (int i = 0; i < 100; i++) {
        g_now += 0.01;
        server.Pump();
        for (auto& c : clients) c->Pump();
    }
    Check(server.ConnectionCount() == 64, "64 clients connect (" + std::to_string(server.ConnectionCount()) + ")");
    // a snapshot of 8 KB to each, then each answers
    auto snap = Pattern(8643, 5);
    for (auto& c : clients) server.Send(c->Local(), snap.data(), snap.size());
    for (int i = 0; i < 50; i++) {
        g_now += 0.01;
        server.Pump();
        for (auto& c : clients) c->Pump();
    }
    int got = 0;
    for (auto& c : clients) {
        Address from;
        std::vector<uint8_t> m;
        while (c->Receive(from, m)) got += m == snap;
    }
    Check(got == 64, "64 clients each receive an 8.6 KB snapshot (" + std::to_string(got) + ")");
}

}  // namespace

int main() {
    if (sodium_init() < 0) {
        std::printf("libsodium failed\n");
        return 1;
    }
    TestReplayWindow();
    TestReassembler();
    TestHandshake();
    TestSizes();
    TestImpairment(0, 0, 0, "clean");
    TestImpairment(0, 0.2, 0, "20% duplicated");
    TestImpairment(0, 0, 0.05, "reordered (50 ms jitter)");
    TestImpairment(0.05, 0.05, 0.03, "5% loss, 5% duplicated, reordered");
    TestHandshakeUnderLoss();
    TestFragmentLoss();
    TestTamper();
    TestReplayAttack();
    TestSpoofedConnect();
    TestStaleCookie();
    TestIdentity();
    TestTimeoutAndDisconnect();
    TestFull();
    TestRateLimit();
    TestMalformed();
    TestManyClients();
    TestReconnectWhileClosing();
    TestClientRestartSamePort();
    TestClosingPeerKeepsSending();
    TestForgedReject();
    TestFloodManySources();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
