// The PC network transport (docs/pc_multiplayer.md "N1 transport"): what the engine's UDP packets travel in between
// separate processes. The engine's netchan above it keeps sequencing, acknowledgement and the reliable stream; this
// layer gives it connections that are established by a handshake (a stateless cookie against spoofed sources, an
// X25519 key exchange, the server's Ed25519 identity signing it, optionally an Ed25519 player identity), datagrams of at most 1200 bytes encrypted and
// authenticated with ChaCha20-Poly1305, engine packets up to 64 KB fragmented and reassembled within bounded memory
// and time, replay and duplicate rejection, timeouts and explicit disconnects with a reason. Host code only; the
// engine is wired to it in src/game/net_engine.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace net {

// ---- wire format (all integers little-endian) ----------------------------------------------------------------------
constexpr uint32_t kMagic = 0x4E585343;  // "CSXN"
constexpr uint8_t kVersion = 3;  // 2: rejects carry the handshake nonce; 3: the accept is signed by the server
constexpr size_t kMaxDatagram = 1200;    // never more on the wire: no IP fragmentation on any common path
constexpr size_t kMaxMessage = 0x10000;  // an engine packet: the size of the engine's net_message
constexpr size_t kPrefix = 6;            // magic, version, type
constexpr size_t kDataHeader = kPrefix + 4 + 8;  // + connection id, packet number
constexpr size_t kTag = 16;              // Poly1305
constexpr size_t kFragHeader = 1 + 2 + 1 + 1;    // frame kind, message id, index, count
constexpr size_t kMaxFragment = kMaxDatagram - kDataHeader - kTag - kFragHeader;  // 1161
constexpr size_t kMaxFragments = (kMaxMessage + kMaxFragment - 1) / kMaxFragment;   // 57

enum class Type : uint8_t { Hello = 1, Challenge = 2, Connect = 3, Reject = 4, Data = 5, Accept = 6 };
enum class Frame : uint8_t { Fragment = 1, Ping = 2, Pong = 3, Disconnect = 4 };

// why a connection ended or was refused (Reject and Disconnect frames carry one, with a short text)
enum class Reason : uint8_t {
    None = 0,
    Closed = 1,          // the peer closed it (the engine disconnected)
    Timeout = 2,         // nothing received for the idle timeout
    Version = 3,         // transport version or engine protocol differs
    Full = 4,            // too many connections
    IdentityRequired = 5,
    IdentityInvalid = 6,
    ServerShutdown = 7,
    MapChange = 8,       // the server relaunches into another map: reconnect to the same address
    Kicked = 9,
    RateLimited = 10,
    HandshakeFailed = 11,  // no answer to the handshake
    ServerIdentityInvalid = 12,  // client: the accept's server signature did not verify
    ServerIdentityChanged = 13,  // client: the server's key is not the one pinned or remembered for its address
    LanOnly = 14,                // the server takes players from private networks only
};
const char* ReasonText(Reason r);

// an IPv4 endpoint, network byte order kept as the engine's netadr_t has it (ip as 4 bytes, port in host order here)
struct Address {
    std::array<uint8_t, 4> ip{};
    uint16_t port = 0;
    bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
    // loopback, private (RFC 1918) or link-local: an address a LAN-only server takes
    bool Private() const {
        return ip[0] == 127 || ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 192 && ip[1] == 168) ||
               (ip[0] == 169 && ip[1] == 254);
    }
    uint64_t Key() const {
        return uint64_t(ip[0]) << 40 | uint64_t(ip[1]) << 32 | uint64_t(ip[2]) << 24 | uint64_t(ip[3]) << 16 | port;
    }
    std::string ToString() const;
    static std::optional<Address> Parse(const std::string& text, uint16_t default_port);  // "a.b.c.d[:port]" or a name
};

// ---- the datagram socket the transport runs on (a UDP socket, or the tests' simulated network) ---------------------
class DatagramSocket {
public:
    virtual ~DatagramSocket() = default;
    virtual bool Send(const Address& to, const uint8_t* data, size_t len) = 0;
    // one datagram, or -1 when none is waiting
    virtual int Receive(Address& from, uint8_t* buf, size_t max) = 0;
    virtual Address Local() const = 0;
};
// a non-blocking UDP socket bound to port (0: any) on all IPv4 interfaces (or the one address given), close-on-exec;
// null with an error text
std::unique_ptr<DatagramSocket> OpenUdp(uint16_t port, std::string& error, const Address* bind_to = nullptr);

// ---- configuration and statistics ----------------------------------------------------------------------------------
struct Config {
    double handshake_retry = 0.5;      // seconds between handshake resends
    int handshake_tries = 10;          // then the connect fails (Reason::HandshakeFailed)
    double idle_timeout = 30.0;        // nothing received for this long: the connection is dropped
    double keepalive = 1.0;            // a ping when nothing was sent for this long
    double cookie_lifetime = 15.0;     // a challenge's cookie is accepted this long
    double reassembly_timeout = 1.0;   // a message whose fragments did not all arrive is dropped after this
    size_t reassembly_messages = 8;    // partly received messages kept per connection
    size_t reassembly_bytes = 256 * 1024;  // their bytes, per connection
    size_t max_connections = 128;
    size_t max_connections_per_ip = 16;
    double hello_rate = 20.0;          // handshake packets a second per source address (burst: twice that)
    size_t pending_packets = 32;       // engine packets queued while a client connection is being established
    bool require_identity = false;     // server: refuse clients without a player identity
    double reconnect_hold = 5.0;       // client: no new connection to a server that closed ours for this long
    double hello_rate_total = 500.0;   // handshake packets a second from all sources together (burst: twice that)
    size_t rate_sources = 4096;        // source addresses the per-source rate remembers (a full table takes no new)
    bool lan_only = false;             // server: handshakes from private addresses only (Address::Private)
};

struct Stats {
    uint64_t datagrams_out = 0, datagrams_in = 0, bytes_out = 0, bytes_in = 0;
    uint64_t messages_out = 0, messages_in = 0, fragmented_out = 0, fragments_out = 0, fragments_in = 0;
    uint64_t largest_message_out = 0;
    uint64_t dropped_malformed = 0, dropped_unknown = 0, dropped_replay = 0, dropped_auth = 0, dropped_address = 0;
    uint64_t dropped_rate = 0, reassembly_timeouts = 0, reassembly_overflow = 0, handshakes_ok = 0, handshakes_failed = 0;
    uint64_t lost_in = 0;  // gaps in the peers' packet numbers
};

// an identity: an Ed25519 key pair made once per install (Identity::LoadOrCreate), a player's (identity.key: proves
// only that the same install connects again, not who the player is) or a server's (server_identity.key: it signs
// every handshake it accepts, so a client knows it reached the server it trusts)
struct Identity {
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 64> secret_key{};
    static std::optional<Identity> LoadOrCreate(const std::string& path, std::string& error);
    // the whole public key, 64 hex characters (no shortened hash: a fingerprint is compared in full)
    static std::string Fingerprint(const std::array<uint8_t, 32>& public_key);
    static std::optional<std::array<uint8_t, 32>> ParseFingerprint(const std::string& text);
};

// what a client knows of servers' keys (trust on first use): a text file of "address key" lines, the key a
// Fingerprint. The first connect to an address remembers its key; a later one with another key is refused until the
// line is removed (or the new key pinned). Written whole (a temporary file renamed over), owner-only.
class KnownServers {
public:
    explicit KnownServers(std::string path) : path_(std::move(path)) {}
    enum class Result { New, Match, Changed };
    // the stored key of server (an Address string) compared with key; on Changed, old receives the stored one
    Result Check(const std::string& server, const std::string& key, std::string* old = nullptr) const;
    bool Remember(const std::string& server, const std::string& key, std::string& error);
    const std::string& Path() const { return path_; }

private:
    std::map<std::string, std::string> Read() const;
    std::string path_;
};

// a client's decision about the server key an accept proved: Trusted continues, Changed refuses the connection
// (Reason::ServerIdentityChanged, the text saying why)
enum class ServerTrust { Trusted, Changed };
using ServerKeyCheck = std::function<ServerTrust(const Address& server, const std::string& fingerprint, std::string& why)>;

using Clock = std::function<double()>;  // seconds, monotonic

// ---- an endpoint: one socket, any number of connections -----------------------------------------------------------
class Endpoint {
public:
    // server: accepts handshakes; client: starts one when the engine sends to an address it has no connection to
    enum class Role { Server, Client };
    Endpoint(Role role, std::unique_ptr<DatagramSocket> socket, Config config, Clock clock);
    ~Endpoint();

    // a client's player identity (optional), a server's own (required: it signs each accept)
    void SetIdentity(std::optional<Identity> identity) { identity_ = std::move(identity); }
    // client: decides about each server key (unset: every correctly signed server is trusted, for the unit tests)
    void SetServerKeyCheck(ServerKeyCheck check) { server_check_ = std::move(check); }

    // an engine packet to a peer (a client endpoint connects first and keeps the packet meanwhile); false when it was
    // dropped (no connection on a server endpoint, too large, queue full)
    bool Send(const Address& to, const uint8_t* data, size_t len);
    // the next engine packet that arrived whole, or false
    bool Receive(Address& from, std::vector<uint8_t>& data);
    // reads the socket and runs the timers; Send and Receive call it too
    void Pump();

    // ends a connection with a reason sent to the peer (a few times, without waiting)
    void Disconnect(const Address& peer, Reason reason, const std::string& text = {});
    void DisconnectAll(Reason reason, const std::string& text = {});

    // what happened to connections since the last call: established, closed (with the reason), for the engine glue
    struct Event {
        enum Kind { Connected, Closed } kind;
        Address peer;
        Reason reason = Reason::None;
        std::string text;
        std::string identity;  // fingerprint when the peer proved one (on a client: always the server's)
        bool remote_closed = false;  // the peer told us (else a timeout or a local close)
    };
    bool PollEvent(Event& e);

    bool Connected(const Address& peer) const;
    std::optional<std::string> PeerIdentity(const Address& peer) const;
    double Rtt(const Address& peer) const;  // seconds, smoothed; -1 when unknown
    size_t ConnectionCount() const;
    const Stats& GetStats() const { return stats_; }
    Address Local() const { return socket_->Local(); }
    Role GetRole() const { return role_; }

private:
    struct Conn;
    struct Pending;
    void HandleDatagram(const Address& from, const uint8_t* p, size_t n);
    void HandleHello(const Address& from, const uint8_t* p, size_t n);
    void HandleChallenge(const Address& from, const uint8_t* p, size_t n);
    void HandleConnect(const Address& from, const uint8_t* p, size_t n);
    void HandleAccept(const Address& from, const uint8_t* p, size_t n);
    void HandleReject(const Address& from, const uint8_t* p, size_t n);
    void HandleData(const Address& from, const uint8_t* p, size_t n);
    void HandleFrame(Conn& c, const uint8_t* p, size_t n);
    bool SendFrame(Conn& c, const uint8_t* frame, size_t len);
    void SendMessage(Conn& c, const uint8_t* data, size_t len);
    void SendHandshake(Pending& h);
    void SendReject(const Address& to, Reason reason, const std::string& text, uint64_t nonce);
    void SendAccept(Conn& c, uint64_t nonce);
    void Close(Conn& c, Reason reason, const std::string& text, bool notify_peer, bool remote);
    void Timers();
    bool RateOk(const Address& from);
    std::array<uint8_t, 32> Cookie(const Address& from, uint64_t nonce, uint32_t time) const;
    uint32_t CookieTime() const;

    Role role_;
    std::unique_ptr<DatagramSocket> socket_;
    Config config_;
    Clock clock_;
    std::optional<Identity> identity_;
    ServerKeyCheck server_check_;
    Stats stats_;
    std::array<uint8_t, 32> kx_public_{}, kx_secret_{}, cookie_key_{};
    double start_ = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns_;  // by address key
    std::unordered_map<uint32_t, Conn*> by_id_;
    std::unordered_map<uint64_t, std::unique_ptr<Pending>> pending_;  // client handshakes, by address key
    std::unordered_map<uint64_t, double> hold_;  // client: servers that closed us, not reconnected to before then
    struct Bucket {
        double tokens = 0, last = 0;
    };
    std::unordered_map<uint64_t, Bucket> buckets_;  // handshake rate per source ip
    Bucket global_;                                  // and of every source together
    double buckets_cleaned_ = 0;
    std::deque<std::pair<Address, std::vector<uint8_t>>> inbox_;
    std::deque<Event> events_;
};

// ---- pure helpers, exposed for the unit tests ------------------------------------------------------------------------
// the replay window of one direction: the highest packet number seen and the 64 before it
struct ReplayWindow {
    uint64_t highest = 0;
    uint64_t bits = 0;  // bit i: highest - i was seen
    uint64_t lost = 0;  // numbers that left the window without being seen
    // true (and recorded) for a number not seen before and not older than the window
    bool Accept(uint64_t n);
    // whether Accept would take it (nothing recorded)
    bool Fresh(uint64_t n) const;
};

// one connection's reassembly of fragmented messages
class Reassembler {
public:
    Reassembler(size_t max_messages, size_t max_bytes, double timeout) :
        max_messages_(max_messages), max_bytes_(max_bytes), timeout_(timeout) {}
    enum class Result { Partial, Complete, Duplicate, Invalid, Overflow };
    Result Add(uint16_t id, uint8_t index, uint8_t count, const uint8_t* data, size_t len, double now,
               std::vector<uint8_t>& out);
    size_t Expire(double now);  // drops messages older than the timeout; how many
    size_t Buffered() const { return bytes_; }
    size_t Messages() const { return parts_.size(); }

private:
    struct Part {
        uint8_t count = 0;
        double first = 0;
        size_t bytes = 0, received = 0;
        std::vector<std::vector<uint8_t>> frags;
        std::vector<bool> have;
    };
    size_t max_messages_, max_bytes_;
    double timeout_;
    size_t bytes_ = 0;
    std::map<uint16_t, Part> parts_;
};

}  // namespace net
