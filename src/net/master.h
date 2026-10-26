// The master server (docs/pc_multiplayer.md "N3"): an optional, self-hostable list of internet servers. No accounts,
// no Steam, nothing a server or a player must use: a browser lists LAN servers by broadcast and favourites by address
// without it. Its address is a setting (net_master on players, master in server.cfg); none is set by default.
//
// Registration (server -> master, from the server's game socket, so the master lists the address and port players
// reach it at, through any NAT mapping the game traffic keeps open):
//   heartbeat   prefix, nonce, zeros to 64 bytes (no answer is larger)
//   challenge   (master) prefix, the nonce, a time, a cookie (HMAC under the master's key of the source address, the
//               nonce and the time): proves the server receives at the address it claims, so nobody registers
//               someone else's address
//   register    prefix, the nonce, the time, the cookie, flags (1: leaving), the server's identity key, an Ed25519
//               signature by it over "CSXN master v1" and everything before it
//   registered  (master) prefix, the nonce, the heartbeat interval it wants (seconds)
// The master lists the server (source address, key) until it misses heartbeats for the expiry time; a newer key for an
// address replaces the old one (a server that made a new identity), an address cannot be listed twice.
//
// The list (browser -> master):
//   list request  prefix, nonce, page, zeros to 1200 bytes
//   list          (master) prefix, the nonce, page, pages, total, count, count x (IPv4, port, identity key)
// A listing is not trust: the browser queries every server itself (query.h) and drops one whose signed answer carries
// another key than the master listed; joining still goes through the N2 checks (known servers, pinned keys).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/transport.h"
#include "net/wire.h"

namespace net {

constexpr uint16_t kMasterPort = 27010;  // GoldSrc's master port
constexpr size_t kHeartbeatSize = 64;
constexpr size_t kListEntry = 4 + 2 + 32;
constexpr size_t kListHeader = kPrefix + 8 + 4 + 4 + 4 + 2;
constexpr size_t kListPerPage = (kMaxDatagram - kListHeader) / kListEntry;  // 30

struct MasterEntry {
    Address address;
    std::array<uint8_t, 32> key{};
};

// ---- the master server's state, fed datagrams by its program (tools/master/master_main.cpp) or the unit tests ----
class MasterServer {
public:
    struct Config {
        double expiry = 180;           // a server not heard from for this long is dropped
        uint16_t heartbeat = 60;       // the interval servers are told to use
        double cookie_lifetime = 30;   // a challenge's cookie is accepted this long
        size_t max_servers = 20000;
        size_t max_per_ip = 32;        // servers listed from one IP address
        double per_source = 10;        // datagrams a second from one address that are answered (burst twice that)
        double total = 2000;           // and from everyone together
        bool allow_private = true;     // list servers at private addresses (a LAN party's master); off for a public one
    };
    using Send = std::function<void(const Address& to, const std::vector<uint8_t>& data)>;
    MasterServer(Config config, Clock clock);
    ~MasterServer();
    void Handle(const Address& from, const uint8_t* p, size_t n, const Send& send);
    void Expire();
    size_t Count() const { return servers_.size(); }
    std::vector<MasterEntry> Entries() const;
    struct Stats {
        uint64_t heartbeats = 0, registered = 0, refreshed = 0, left = 0, expired = 0, lists = 0, malformed = 0,
                 rate_limited = 0, refused = 0, bad_cookie = 0, bad_signature = 0;
    };
    const Stats& GetStats() const { return stats_; }
    std::function<void(const std::string&)> log;  // what it did, for its program's log

private:
    struct Server {
        std::array<uint8_t, 32> key{};
        double seen = 0;
        uint64_t order = 0;
    };
    std::array<uint8_t, 32> Cookie(const Address& from, uint64_t nonce, uint32_t time) const;
    void Log(const std::string& s) { if (log) log(s); }
    Config config_;
    Clock clock_;
    std::array<uint8_t, 32> cookie_key_{};
    wire::RateLimiter limit_;
    std::unordered_map<uint64_t, Server> servers_;  // by address key
    std::map<uint64_t, uint64_t> order_;            // registration order -> address key: pages in a stable order
    std::unordered_map<uint64_t, size_t> per_ip_;
    uint64_t next_order_ = 1;
    double start_ = 0, expired_at_ = -1e9;
    Stats stats_;
};

// ---- a server's registration with its masters (on the game thread, through the server endpoint's socket) ----------
class Registrar {
public:
    Registrar(Identity identity, std::vector<Address> masters, Clock clock) :
        identity_(identity), masters_(std::move(masters)), clock_(std::move(clock)) {}
    using Send = std::function<void(const Address& to, const std::vector<uint8_t>& data)>;
    // heartbeats when due (the first at once)
    void Pump(const Send& send);
    // a challenge or a registered from a master (others are ignored); true when it was one of ours
    bool Handle(const Address& from, Type type, const uint8_t* p, size_t n, const Send& send);
    // tells the masters the server is leaving (with the last cookie they gave, while still fresh)
    void Leave(const Send& send);
    void SetMasters(std::vector<Address> masters) { masters_ = std::move(masters); }
    std::function<void(const std::string&)> log;

private:
    struct State {
        uint64_t nonce = 0;
        double next = 0, interval = 60;
        bool listed = false;
        uint32_t time = 0;
        std::array<uint8_t, 32> cookie{};
        double cookie_at = -1;
    };
    void Log(const std::string& s) { if (log) log(s); }
    std::vector<uint8_t> RegisterPacket(const State& s, bool leaving) const;
    Identity identity_;
    std::vector<Address> masters_;
    Clock clock_;
    std::map<uint64_t, State> state_;  // by master address key
};

// ---- the browser's side: list requests and pages ------------------------------------------------------------------
std::vector<uint8_t> EncodeListRequest(uint64_t nonce, uint32_t page);
bool DecodeList(const uint8_t* p, size_t n, uint64_t nonce, uint32_t& page, uint32_t& pages, uint32_t& total,
                std::vector<MasterEntry>& out, std::string& error);

}  // namespace net
