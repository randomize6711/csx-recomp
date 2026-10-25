// The PC network transport (transport.h; docs/pc_multiplayer.md "N1 transport").
#include "net/transport.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace net {
namespace {

// ---- serialization: bounds-checked, little-endian ------------------------------------------------------------------
struct Writer {
    uint8_t* p;
    size_t cap, n = 0;
    bool ok = true;
    void Bytes(const void* d, size_t len) {
        if (!ok || cap - n < len) {
            ok = false;
            return;
        }
        std::memcpy(p + n, d, len);
        n += len;
    }
    void U8(uint8_t v) { Bytes(&v, 1); }
    void U16(uint16_t v) {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        Bytes(b, 2);
    }
    void U32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        Bytes(b, 4);
    }
    void U64(uint64_t v) {
        U32(uint32_t(v));
        U32(uint32_t(v >> 32));
    }
    void Zero(size_t len) {
        if (!ok || cap - n < len) {
            ok = false;
            return;
        }
        std::memset(p + n, 0, len);
        n += len;
    }
};

struct Reader {
    const uint8_t* p;
    size_t len, n = 0;
    bool ok = true;
    const uint8_t* Bytes(size_t k) {
        if (!ok || len - n < k) {
            ok = false;
            return nullptr;
        }
        const uint8_t* r = p + n;
        n += k;
        return r;
    }
    uint8_t U8() {
        const uint8_t* b = Bytes(1);
        return b ? b[0] : 0;
    }
    uint16_t U16() {
        const uint8_t* b = Bytes(2);
        return b ? uint16_t(b[0] | b[1] << 8) : 0;
    }
    uint32_t U32() {
        const uint8_t* b = Bytes(4);
        return b ? uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24 : 0;
    }
    uint64_t U64() {
        uint64_t lo = U32();
        return lo | uint64_t(U32()) << 32;
    }
    size_t Left() const { return ok ? len - n : 0; }
};

void Prefix(Writer& w, Type t) {
    w.U32(kMagic);
    w.U8(kVersion);
    w.U8(uint8_t(t));
}

constexpr size_t kNonce = crypto_aead_chacha20poly1305_ietf_NPUBBYTES;  // 12: 4 zero bytes and the packet number
static_assert(crypto_aead_chacha20poly1305_ietf_ABYTES == kTag);
static_assert(crypto_kx_PUBLICKEYBYTES == 32 && crypto_kx_SESSIONKEYBYTES == 32);
static_assert(crypto_sign_PUBLICKEYBYTES == 32 && crypto_sign_SECRETKEYBYTES == 64 && crypto_sign_BYTES == 64);

void NonceOf(uint64_t number, uint8_t (&nonce)[kNonce]) {
    std::memset(nonce, 0, kNonce);
    for (int i = 0; i < 8; i++) nonce[4 + i] = uint8_t(number >> (8 * i));
}

constexpr char kIdentityContext[] = "CSXN identity v1";
constexpr size_t kConnectBase = kPrefix + 2 + 8 + 4 + 32 + 32;
constexpr size_t kConnectIdentity = 32 + 64;
constexpr int kDisconnectRepeats = 3;

bool SodiumReady() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

}  // namespace

const char* ReasonText(Reason r) {
    switch (r) {
    case Reason::None: return "none";
    case Reason::Closed: return "closed by the peer";
    case Reason::Timeout: return "timed out";
    case Reason::Version: return "different version";
    case Reason::Full: return "server full";
    case Reason::IdentityRequired: return "the server requires a player identity";
    case Reason::IdentityInvalid: return "player identity not accepted";
    case Reason::ServerShutdown: return "server shut down";
    case Reason::MapChange: return "map change";
    case Reason::Kicked: return "kicked";
    case Reason::RateLimited: return "too many connection attempts";
    case Reason::HandshakeFailed: return "no answer from the server";
    }
    return "unknown";
}

std::string Address::ToString() const {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
    return s;
}

// ---- replay window ---------------------------------------------------------------------------------------------------
bool ReplayWindow::Fresh(uint64_t n) const {
    if (n == 0) return false;
    if (n > highest) return true;
    uint64_t age = highest - n;
    return age < 64 && !(bits & (uint64_t(1) << age));
}

bool ReplayWindow::Accept(uint64_t n) {
    if (n == 0) return false;  // packet numbers start at 1
    if (n > highest) {
        uint64_t shift = n - highest;
        // numbers leaving the window unseen are lost (a late one inside it is reordering, not loss)
        // (window positions are ages 0..63: number highest - age; numbers below 1 never existed)
        if (highest != 0) {
            uint64_t oldest = std::min<uint64_t>(63, highest - 1);  // the oldest age that holds a real number
            if (shift >= 64) {
                lost += (oldest + 1) - uint64_t(__builtin_popcountll(bits)) + (shift - 64);
            } else if (oldest >= 64 - shift) {
                uint64_t valid = oldest - (64 - shift) + 1;
                lost += valid - uint64_t(__builtin_popcountll(bits >> (64 - shift)));
            }
        }
        bits = shift >= 64 ? 0 : bits << shift;
        bits |= 1;
        highest = n;
        return true;
    }
    uint64_t age = highest - n;
    if (age >= 64) return false;
    uint64_t bit = uint64_t(1) << age;
    if (bits & bit) return false;
    bits |= bit;
    return true;
}

// ---- reassembly ------------------------------------------------------------------------------------------------------
Reassembler::Result Reassembler::Add(uint16_t id, uint8_t index, uint8_t count, const uint8_t* data, size_t len,
                                     double now, std::vector<uint8_t>& out) {
    if (count == 0 || count > kMaxFragments || index >= count || len == 0 || len > kMaxFragment) return Result::Invalid;
    if (count == 1) {
        out.assign(data, data + len);
        return Result::Complete;
    }
    // every fragment but the last is full; the message's size is then known within one fragment
    if (index + 1 < count && len != kMaxFragment) return Result::Invalid;
    auto it = parts_.find(id);
    if (it == parts_.end()) {
        if (parts_.size() >= max_messages_) {
            // the oldest partial message makes room (a newer message supersedes an older lost one)
            auto oldest = std::min_element(parts_.begin(), parts_.end(),
                                           [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
            bytes_ -= oldest->second.bytes;
            parts_.erase(oldest);
        }
        Part p;
        p.count = count;
        p.first = now;
        p.frags.resize(count);
        p.have.assign(count, false);
        it = parts_.emplace(id, std::move(p)).first;
    }
    Part& p = it->second;
    if (p.count != count) {
        bytes_ -= p.bytes;
        parts_.erase(it);
        return Result::Invalid;
    }
    if (p.have[index]) return Result::Duplicate;
    if (bytes_ + len > max_bytes_) return Result::Overflow;
    p.frags[index].assign(data, data + len);
    p.have[index] = true;
    p.bytes += len;
    bytes_ += len;
    if (++p.received < count) return Result::Partial;
    size_t total = p.bytes;
    if (total > kMaxMessage) {
        bytes_ -= p.bytes;
        parts_.erase(it);
        return Result::Invalid;
    }
    out.clear();
    out.reserve(total);
    for (auto& f : p.frags) out.insert(out.end(), f.begin(), f.end());
    bytes_ -= p.bytes;
    parts_.erase(it);
    return Result::Complete;
}

size_t Reassembler::Expire(double now) {
    size_t n = 0;
    for (auto it = parts_.begin(); it != parts_.end();) {
        if (now - it->second.first > timeout_) {
            bytes_ -= it->second.bytes;
            it = parts_.erase(it);
            n++;
        } else {
            ++it;
        }
    }
    return n;
}

// ---- identity --------------------------------------------------------------------------------------------------------
std::optional<Identity> Identity::LoadOrCreate(const std::string& path, std::string& error) {
    if (!SodiumReady()) {
        error = "libsodium did not initialise";
        return std::nullopt;
    }
    Identity id;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        char magic[8] = {};
        in.read(magic, 8);
        in.read(reinterpret_cast<char*>(id.secret_key.data()), 64);
        if (in.gcount() == 64 && std::memcmp(magic, "CSXNID1\n", 8) == 0) {
            crypto_sign_ed25519_sk_to_pk(id.public_key.data(), id.secret_key.data());
            return id;
        }
        error = path + " is not an identity file";
        return std::nullopt;
    }
    crypto_sign_keypair(id.public_key.data(), id.secret_key.data());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("CSXNID1\n", 8);
    out.write(reinterpret_cast<const char*>(id.secret_key.data()), 64);
    if (!out) {
        error = "cannot write " + path;
        return std::nullopt;
    }
    return id;
}

std::string Identity::Fingerprint(const std::array<uint8_t, 32>& public_key) {
    uint8_t h[8];
    crypto_generichash(h, sizeof(h), public_key.data(), public_key.size(), nullptr, 0);
    char s[17];
    for (int i = 0; i < 8; i++) std::snprintf(s + 2 * i, 3, "%02x", h[i]);
    return s;
}

// ---- connections -----------------------------------------------------------------------------------------------------
struct Endpoint::Conn {
    Address peer;
    uint32_t id = 0;           // our id for it (the server picks it; both sides send it in every data packet)
    std::array<uint8_t, 32> rx{}, tx{};
    uint64_t next_out = 1;     // packet numbers, also the AEAD nonce
    ReplayWindow window;
    Reassembler reasm;
    uint16_t next_message = 0;
    double last_in = 0, last_out = 0;
    double rtt = -1;
    uint32_t ping_id = 0;
    double ping_sent = 0, last_ping = 0;
    std::string identity;
    bool closing = false;
    Conn(const Config& c) : reasm(c.reassembly_messages, c.reassembly_bytes, c.reassembly_timeout) {}
};

struct Endpoint::Pending {
    Address peer;
    uint64_t nonce = 0;
    std::array<uint8_t, 32> pk{}, sk{};  // this connection's key exchange pair
    bool challenged = false;
    uint32_t cookie_time = 0;
    std::array<uint8_t, 32> cookie{}, server_pk{};
    int tries = 0;
    double next = 0;
    std::deque<std::vector<uint8_t>> queue;
};

Endpoint::Endpoint(Role role, std::unique_ptr<DatagramSocket> socket, Config config, Clock clock) :
    role_(role), socket_(std::move(socket)), config_(config), clock_(std::move(clock)) {
    SodiumReady();
    crypto_kx_keypair(kx_public_.data(), kx_secret_.data());
    randombytes_buf(cookie_key_.data(), cookie_key_.size());
    start_ = clock_();
}

Endpoint::~Endpoint() {
    sodium_memzero(kx_secret_.data(), kx_secret_.size());
    sodium_memzero(cookie_key_.data(), cookie_key_.size());
}

uint32_t Endpoint::CookieTime() const {
    return uint32_t(clock_() - start_);
}

std::array<uint8_t, 32> Endpoint::Cookie(const Address& from, uint64_t nonce, uint32_t time) const {
    uint8_t msg[4 + 2 + 8 + 4 + 32];
    Writer w{msg, sizeof(msg)};
    w.Bytes(from.ip.data(), 4);
    w.U16(from.port);
    w.U64(nonce);
    w.U32(time);
    w.Bytes(kx_public_.data(), 32);
    std::array<uint8_t, 32> mac{};
    crypto_auth_hmacsha256(mac.data(), msg, sizeof(msg), cookie_key_.data());
    return mac;
}

bool Endpoint::RateOk(const Address& from) {
    double now = clock_();
    uint64_t ip = from.Key() >> 16;
    Bucket& b = buckets_[ip];
    double burst = 2 * config_.hello_rate;
    if (b.last == 0) b.tokens = burst;
    b.tokens = std::min(burst, b.tokens + (now - b.last) * config_.hello_rate);
    b.last = now;
    if (b.tokens < 1) {
        stats_.dropped_rate++;
        return false;
    }
    b.tokens -= 1;
    if (buckets_.size() > 4096) {  // forget quiet sources
        for (auto it = buckets_.begin(); it != buckets_.end();) it = now - it->second.last > 10 ? buckets_.erase(it) : ++it;
    }
    return true;
}

bool Endpoint::Send(const Address& to, const uint8_t* data, size_t len) {
    Pump();
    if (len == 0 || len > kMaxMessage) return false;
    auto it = conns_.find(to.Key());
    if (it != conns_.end() && !it->second->closing) {
        SendMessage(*it->second, data, len);
        return true;
    }
    if (role_ == Role::Server) {
        stats_.dropped_unknown++;
        return false;
    }
    // a server that just closed this connection is not connected to again at once by the engine's leftover packets
    // (a shutting down server would take them); a new connect after the hold, or from a new process, is
    if (auto b = hold_.find(to.Key()); b != hold_.end()) {
        if (clock_() < b->second) return false;
        hold_.erase(b);
    }
    // a client endpoint connects first
    auto& h = pending_[to.Key()];
    if (!h) {
        h = std::make_unique<Pending>();
        h->peer = to;
        randombytes_buf(&h->nonce, sizeof(h->nonce));
        crypto_kx_keypair(h->pk.data(), h->sk.data());
        h->next = 0;
        SendHandshake(*h);
    }
    if (h->queue.size() >= config_.pending_packets) return false;
    h->queue.emplace_back(data, data + len);
    return true;
}

bool Endpoint::Receive(Address& from, std::vector<uint8_t>& data) {
    if (inbox_.empty()) Pump();
    if (inbox_.empty()) return false;
    from = inbox_.front().first;
    data = std::move(inbox_.front().second);
    inbox_.pop_front();
    return true;
}

void Endpoint::Pump() {
    uint8_t buf[2048];
    Address from;
    for (int i = 0; i < 4096; i++) {
        int n = socket_->Receive(from, buf, sizeof(buf));
        if (n < 0) break;
        stats_.datagrams_in++;
        stats_.bytes_in += uint64_t(n);
        HandleDatagram(from, buf, size_t(n));
    }
    Timers();
}

bool Endpoint::PollEvent(Event& e) {
    if (events_.empty()) return false;
    e = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool Endpoint::Connected(const Address& peer) const {
    auto it = conns_.find(peer.Key());
    return it != conns_.end() && !it->second->closing;
}

std::optional<std::string> Endpoint::PeerIdentity(const Address& peer) const {
    auto it = conns_.find(peer.Key());
    if (it == conns_.end() || it->second->identity.empty()) return std::nullopt;
    return it->second->identity;
}

double Endpoint::Rtt(const Address& peer) const {
    auto it = conns_.find(peer.Key());
    return it == conns_.end() ? -1 : it->second->rtt;
}

size_t Endpoint::ConnectionCount() const {
    return conns_.size();
}

void Endpoint::HandleDatagram(const Address& from, const uint8_t* p, size_t n) {
    if (n < kPrefix || n > kMaxDatagram) {
        stats_.dropped_malformed++;
        return;
    }
    Reader r{p, n};
    if (r.U32() != kMagic) {
        stats_.dropped_malformed++;
        return;
    }
    uint8_t version = r.U8();
    Type type = Type(r.U8());
    if (version != kVersion) {
        // only a full-size hello earns an answer (it cannot amplify)
        if (role_ == Role::Server && type == Type::Hello && n == kMaxDatagram && RateOk(from))
            SendReject(from, Reason::Version, "transport version " + std::to_string(kVersion));
        stats_.dropped_malformed++;
        return;
    }
    switch (type) {
    case Type::Hello: HandleHello(from, p, n); break;
    case Type::Challenge: HandleChallenge(from, p, n); break;
    case Type::Connect: HandleConnect(from, p, n); break;
    case Type::Accept: HandleAccept(from, p, n); break;
    case Type::Reject: HandleReject(from, p, n); break;
    case Type::Data: HandleData(from, p, n); break;
    default: stats_.dropped_malformed++; break;
    }
}

// hello: prefix, flags, reserved, client nonce, padding to the full datagram (a reply is never larger than this)
void Endpoint::HandleHello(const Address& from, const uint8_t* p, size_t n) {
    if (role_ != Role::Server || n != kMaxDatagram) {
        stats_.dropped_malformed++;
        return;
    }
    if (!RateOk(from)) return;
    Reader r{p + kPrefix, n - kPrefix};
    r.U8();
    r.U8();
    uint64_t nonce = r.U64();
    uint32_t time = CookieTime();
    auto cookie = Cookie(from, nonce, time);
    uint8_t out[kPrefix + 2 + 8 + 4 + 32 + 32];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Challenge);
    w.U8(config_.require_identity ? 1 : 0);
    w.U8(0);
    w.U64(nonce);
    w.U32(time);
    w.Bytes(cookie.data(), 32);
    w.Bytes(kx_public_.data(), 32);
    if (socket_->Send(from, out, w.n)) {
        stats_.datagrams_out++;
        stats_.bytes_out += w.n;
    }
}

void Endpoint::SendHandshake(Pending& h) {
    uint8_t out[kMaxDatagram];
    Writer w{out, sizeof(out)};
    if (!h.challenged) {
        Prefix(w, Type::Hello);
        w.U8(0);
        w.U8(0);
        w.U64(h.nonce);
        w.Zero(kMaxDatagram - w.n);
    } else {
        Prefix(w, Type::Connect);
        bool with_id = identity_.has_value();
        w.U8(with_id ? 1 : 0);
        w.U8(0);
        w.U64(h.nonce);
        w.U32(h.cookie_time);
        w.Bytes(h.cookie.data(), 32);
        w.Bytes(h.pk.data(), 32);
        if (with_id) {
            // the signature binds the identity to this handshake: the server's key, ours, the cookie and the nonce
            uint8_t msg[sizeof(kIdentityContext) + 32 + 32 + 32 + 8];
            Writer m{msg, sizeof(msg)};
            m.Bytes(kIdentityContext, sizeof(kIdentityContext));
            m.Bytes(h.server_pk.data(), 32);
            m.Bytes(h.pk.data(), 32);
            m.Bytes(h.cookie.data(), 32);
            m.U64(h.nonce);
            uint8_t sig[64];
            crypto_sign_detached(sig, nullptr, msg, m.n, identity_->secret_key.data());
            w.Bytes(identity_->public_key.data(), 32);
            w.Bytes(sig, 64);
        }
    }
    if (socket_->Send(h.peer, out, w.n)) {
        stats_.datagrams_out++;
        stats_.bytes_out += w.n;
    }
    h.tries++;
    h.next = clock_() + config_.handshake_retry;
}

void Endpoint::HandleChallenge(const Address& from, const uint8_t* p, size_t n) {
    auto it = pending_.find(from.Key());
    if (role_ != Role::Client || it == pending_.end() || n != kPrefix + 2 + 8 + 4 + 32 + 32) {
        stats_.dropped_unknown++;
        return;
    }
    Pending& h = *it->second;
    Reader r{p + kPrefix, n - kPrefix};
    uint8_t flags = r.U8();
    r.U8();
    if (r.U64() != h.nonce) {
        stats_.dropped_auth++;
        return;
    }
    if ((flags & 1) && !identity_) {
        events_.push_back({Event::Closed, from, Reason::IdentityRequired, ReasonText(Reason::IdentityRequired), {}, true});
        pending_.erase(it);
        stats_.handshakes_failed++;
        return;
    }
    h.cookie_time = r.U32();
    std::memcpy(h.cookie.data(), r.Bytes(32), 32);
    std::memcpy(h.server_pk.data(), r.Bytes(32), 32);
    if (!h.challenged) {
        h.challenged = true;
        h.tries = 0;
    }
    SendHandshake(h);
}

void Endpoint::HandleConnect(const Address& from, const uint8_t* p, size_t n) {
    if (role_ != Role::Server || (n != kConnectBase && n != kConnectBase + kConnectIdentity)) {
        stats_.dropped_malformed++;
        return;
    }
    if (!RateOk(from)) return;
    Reader r{p + kPrefix, n - kPrefix};
    uint8_t flags = r.U8();
    r.U8();
    uint64_t nonce = r.U64();
    uint32_t time = r.U32();
    const uint8_t* cookie = r.Bytes(32);
    const uint8_t* client_pk = r.Bytes(32);
    uint32_t now = CookieTime();
    auto expect = Cookie(from, nonce, time);
    if (!cookie || time > now || now - time > uint32_t(config_.cookie_lifetime) ||
        sodium_memcmp(expect.data(), cookie, 32) != 0) {
        stats_.dropped_auth++;  // forged or stale: no answer
        return;
    }
    // a connect for a connection already made (our accept was lost): answer it again
    if (auto it = conns_.find(from.Key()); it != conns_.end()) {
        Conn& c = *it->second;
        uint8_t out[kPrefix + 4 + 8 + kTag];
        Writer w{out, sizeof(out)};
        Prefix(w, Type::Accept);
        w.U32(c.id);
        w.U64(nonce);
        uint8_t nonce_bytes[kNonce];
        NonceOf(0, nonce_bytes);
        unsigned long long clen = 0;
        crypto_aead_chacha20poly1305_ietf_encrypt(out + w.n, &clen, nullptr, 0, out, w.n, nullptr, nonce_bytes,
                                                  c.tx.data());
        socket_->Send(from, out, w.n + size_t(clen));
        return;
    }
    std::string identity;
    if (flags & 1) {
        if (n != kConnectBase + kConnectIdentity) {
            stats_.dropped_malformed++;
            return;
        }
        const uint8_t* id_pk = r.Bytes(32);
        const uint8_t* sig = r.Bytes(64);
        uint8_t msg[sizeof(kIdentityContext) + 32 + 32 + 32 + 8];
        Writer m{msg, sizeof(msg)};
        m.Bytes(kIdentityContext, sizeof(kIdentityContext));
        m.Bytes(kx_public_.data(), 32);
        m.Bytes(client_pk, 32);
        m.Bytes(cookie, 32);
        m.U64(nonce);
        if (crypto_sign_verify_detached(sig, msg, m.n, id_pk) != 0) {
            SendReject(from, Reason::IdentityInvalid, ReasonText(Reason::IdentityInvalid));
            stats_.handshakes_failed++;
            return;
        }
        std::array<uint8_t, 32> pk{};
        std::memcpy(pk.data(), id_pk, 32);
        identity = Identity::Fingerprint(pk);
    } else if (config_.require_identity) {
        SendReject(from, Reason::IdentityRequired, ReasonText(Reason::IdentityRequired));
        stats_.handshakes_failed++;
        return;
    }
    size_t same_ip = 0;
    for (auto& [k, c] : conns_) same_ip += (k >> 16) == (from.Key() >> 16);
    if (conns_.size() >= config_.max_connections || same_ip >= config_.max_connections_per_ip) {
        SendReject(from, Reason::Full, ReasonText(Reason::Full));
        stats_.handshakes_failed++;
        return;
    }
    auto c = std::make_unique<Conn>(config_);
    c->peer = from;
    do {
        randombytes_buf(&c->id, sizeof(c->id));
    } while (c->id == 0 || by_id_.count(c->id));
    if (crypto_kx_server_session_keys(c->rx.data(), c->tx.data(), kx_public_.data(), kx_secret_.data(), client_pk) != 0) {
        stats_.dropped_auth++;
        return;
    }
    c->identity = identity;
    c->last_in = c->last_out = clock_();
    Conn& ref = *c;
    by_id_[c->id] = c.get();
    conns_[from.Key()] = std::move(c);
    // accept: the connection id, the client's nonce, and an empty AEAD message proving the keys match
    uint8_t out[kPrefix + 4 + 8 + kTag];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Accept);
    w.U32(ref.id);
    w.U64(nonce);
    uint8_t nonce_bytes[kNonce];
    NonceOf(0, nonce_bytes);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + w.n, &clen, nullptr, 0, out, w.n, nullptr, nonce_bytes,
                                              ref.tx.data());
    if (socket_->Send(from, out, w.n + size_t(clen))) {
        stats_.datagrams_out++;
        stats_.bytes_out += w.n + clen;
    }
    stats_.handshakes_ok++;
    events_.push_back({Event::Connected, from, Reason::None, {}, identity, false});
}

void Endpoint::HandleAccept(const Address& from, const uint8_t* p, size_t n) {
    auto it = pending_.find(from.Key());
    if (role_ != Role::Client || it == pending_.end() || !it->second->challenged || n != kPrefix + 4 + 8 + kTag) {
        stats_.dropped_unknown++;
        return;
    }
    Pending& h = *it->second;
    Reader r{p + kPrefix, n - kPrefix};
    uint32_t id = r.U32();
    if (r.U64() != h.nonce || id == 0) {
        stats_.dropped_auth++;
        return;
    }
    auto c = std::make_unique<Conn>(config_);
    c->peer = from;
    c->id = id;
    if (crypto_kx_client_session_keys(c->rx.data(), c->tx.data(), h.pk.data(), h.sk.data(), h.server_pk.data()) != 0) {
        stats_.dropped_auth++;
        return;
    }
    uint8_t nonce_bytes[kNonce];
    NonceOf(0, nonce_bytes);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(nullptr, nullptr, nullptr, p + kPrefix + 12, kTag, p, kPrefix + 12,
                                                  nonce_bytes, c->rx.data()) != 0) {
        stats_.dropped_auth++;
        return;
    }
    sodium_memzero(h.sk.data(), h.sk.size());
    c->last_in = c->last_out = clock_();
    auto queue = std::move(h.queue);
    pending_.erase(it);
    Conn& ref = *c;
    by_id_[id] = c.get();
    conns_[from.Key()] = std::move(c);
    stats_.handshakes_ok++;
    events_.push_back({Event::Connected, from, Reason::None, {}, {}, false});
    for (auto& m : queue) SendMessage(ref, m.data(), m.size());
}

void Endpoint::SendReject(const Address& to, Reason reason, const std::string& text) {
    uint8_t out[kPrefix + 2 + 255];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Reject);
    w.U8(uint8_t(reason));
    size_t len = std::min<size_t>(text.size(), 255);
    w.U8(uint8_t(len));
    w.Bytes(text.data(), len);
    if (socket_->Send(to, out, w.n)) {
        stats_.datagrams_out++;
        stats_.bytes_out += w.n;
    }
}

void Endpoint::HandleReject(const Address& from, const uint8_t* p, size_t n) {
    auto it = pending_.find(from.Key());
    if (role_ != Role::Client || it == pending_.end()) {
        stats_.dropped_unknown++;
        return;
    }
    Reader r{p + kPrefix, n - kPrefix};
    Reason reason = Reason(r.U8());
    uint8_t len = r.U8();
    const uint8_t* t = r.Bytes(len);
    std::string text = t ? std::string(reinterpret_cast<const char*>(t), len) : ReasonText(reason);
    // a reject is not authenticated: only believed before the handshake got further than the hello
    events_.push_back({Event::Closed, from, reason, text, {}, true});
    pending_.erase(it);
    stats_.handshakes_failed++;
}

bool Endpoint::SendFrame(Conn& c, const uint8_t* frame, size_t len) {
    if (len + kDataHeader + kTag > kMaxDatagram) return false;
    uint8_t out[kMaxDatagram];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Data);
    w.U32(c.id);
    uint64_t number = c.next_out++;
    w.U64(number);
    uint8_t nonce_bytes[kNonce];
    NonceOf(number, nonce_bytes);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + w.n, &clen, frame, len, out, w.n, nullptr, nonce_bytes, c.tx.data());
    size_t total = w.n + size_t(clen);
    c.last_out = clock_();
    if (!socket_->Send(c.peer, out, total)) return false;
    stats_.datagrams_out++;
    stats_.bytes_out += total;
    return true;
}

void Endpoint::SendMessage(Conn& c, const uint8_t* data, size_t len) {
    size_t count = (len + kMaxFragment - 1) / kMaxFragment;
    uint16_t id = c.next_message++;
    stats_.messages_out++;
    stats_.largest_message_out = std::max<uint64_t>(stats_.largest_message_out, len);
    if (count > 1) {
        stats_.fragmented_out++;
        stats_.fragments_out += count;
    }
    uint8_t frame[kFragHeader + kMaxFragment];
    for (size_t i = 0; i < count; i++) {
        size_t off = i * kMaxFragment, part = std::min(kMaxFragment, len - off);
        Writer w{frame, sizeof(frame)};
        w.U8(uint8_t(Frame::Fragment));
        w.U16(id);
        w.U8(uint8_t(i));
        w.U8(uint8_t(count));
        w.Bytes(data + off, part);
        SendFrame(c, frame, w.n);
    }
}

void Endpoint::HandleData(const Address& from, const uint8_t* p, size_t n) {
    if (n < kDataHeader + kTag + 1) {
        stats_.dropped_malformed++;
        return;
    }
    Reader r{p + kPrefix, n - kPrefix};
    uint32_t id = r.U32();
    uint64_t number = r.U64();
    auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        stats_.dropped_unknown++;
        return;
    }
    Conn& c = *it->second;
    // strict address binding: a known connection id from another address is not this connection (no migration
    // without a path check)
    if (!(c.peer == from)) {
        stats_.dropped_address++;
        return;
    }
    if (!c.window.Fresh(number)) {  // before the decryption: old and repeated numbers cost nothing
        stats_.dropped_replay++;
        return;
    }
    uint8_t plain[kMaxDatagram];
    unsigned long long plen = 0;
    uint8_t nonce_bytes[kNonce];
    NonceOf(number, nonce_bytes);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(plain, &plen, nullptr, p + kDataHeader, n - kDataHeader, p,
                                                  kDataHeader, nonce_bytes, c.rx.data()) != 0) {
        stats_.dropped_auth++;
        return;
    }
    uint64_t lost_before = c.window.lost;
    if (!c.window.Accept(number)) {
        stats_.dropped_replay++;
        return;
    }
    stats_.lost_in += c.window.lost - lost_before;
    c.last_in = clock_();
    HandleFrame(c, plain, size_t(plen));
}

void Endpoint::HandleFrame(Conn& c, const uint8_t* p, size_t n) {
    Reader r{p, n};
    Frame kind = Frame(r.U8());
    switch (kind) {
    case Frame::Fragment: {
        uint16_t id = r.U16();
        uint8_t index = r.U8(), count = r.U8();
        size_t len = r.Left();
        const uint8_t* data = r.Bytes(len);
        if (!r.ok) {
            stats_.dropped_malformed++;
            return;
        }
        stats_.fragments_in++;
        std::vector<uint8_t> msg;
        switch (c.reasm.Add(id, index, count, data, len, clock_(), msg)) {
        case Reassembler::Result::Complete:
            if (c.closing) return;
            stats_.messages_in++;
            inbox_.emplace_back(c.peer, std::move(msg));
            break;
        case Reassembler::Result::Invalid: stats_.dropped_malformed++; break;
        case Reassembler::Result::Overflow: stats_.reassembly_overflow++; break;
        default: break;
        }
        break;
    }
    case Frame::Ping: {
        uint32_t ping = r.U32();
        if (!r.ok) return;
        uint8_t f[5];
        Writer w{f, sizeof(f)};
        w.U8(uint8_t(Frame::Pong));
        w.U32(ping);
        SendFrame(c, f, w.n);
        break;
    }
    case Frame::Pong: {
        uint32_t ping = r.U32();
        if (!r.ok || ping != c.ping_id || c.ping_sent == 0) return;
        double rtt = clock_() - c.ping_sent;
        c.rtt = c.rtt < 0 ? rtt : c.rtt * 0.875 + rtt * 0.125;
        c.ping_sent = 0;
        break;
    }
    case Frame::Disconnect: {
        Reason reason = Reason(r.U8());
        uint8_t len = r.U8();
        const uint8_t* t = r.Bytes(len);
        std::string text = t ? std::string(reinterpret_cast<const char*>(t), len) : ReasonText(reason);
        if (role_ == Role::Client) hold_[c.peer.Key()] = clock_() + config_.reconnect_hold;
        if (!c.closing) Close(c, reason, text, false, true);
        break;
    }
    default: stats_.dropped_malformed++; break;
    }
}

void Endpoint::Close(Conn& c, Reason reason, const std::string& text, bool notify_peer, bool remote) {
    if (notify_peer) {
        uint8_t f[3 + 255];
        Writer w{f, sizeof(f)};
        w.U8(uint8_t(Frame::Disconnect));
        w.U8(uint8_t(reason));
        size_t len = std::min<size_t>(text.size(), 200);
        w.U8(uint8_t(len));
        w.Bytes(text.data(), len);
        for (int i = 0; i < kDisconnectRepeats; i++) SendFrame(c, f, w.n);
    }
    events_.push_back({Event::Closed, c.peer, reason, text.empty() ? ReasonText(reason) : text, c.identity, remote});
    // kept a short while as closing so late packets of it are recognised and dropped, then removed by Timers
    c.closing = true;
    c.last_in = clock_();
    sodium_memzero(c.tx.data(), c.tx.size());
}

void Endpoint::Disconnect(const Address& peer, Reason reason, const std::string& text) {
    if (auto it = conns_.find(peer.Key()); it != conns_.end() && !it->second->closing)
        Close(*it->second, reason, text, true, false);
    if (auto it = pending_.find(peer.Key()); it != pending_.end()) pending_.erase(it);
}

void Endpoint::DisconnectAll(Reason reason, const std::string& text) {
    for (auto& [k, c] : conns_)
        if (!c->closing) Close(*c, reason, text, true, false);
    pending_.clear();
}

void Endpoint::Timers() {
    double now = clock_();
    for (auto it = pending_.begin(); it != pending_.end();) {
        Pending& h = *it->second;
        if (now >= h.next) {
            if (h.tries >= config_.handshake_tries) {
                events_.push_back({Event::Closed, h.peer, Reason::HandshakeFailed, ReasonText(Reason::HandshakeFailed),
                                   {}, false});
                stats_.handshakes_failed++;
                it = pending_.erase(it);
                continue;
            }
            SendHandshake(h);
        }
        ++it;
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
        Conn& c = *it->second;
        if (c.closing) {
            if (now - c.last_in > 2.0) {
                by_id_.erase(c.id);
                it = conns_.erase(it);
                continue;
            }
        } else if (now - c.last_in > config_.idle_timeout) {
            Close(c, Reason::Timeout, ReasonText(Reason::Timeout), true, false);
        } else {
            stats_.reassembly_timeouts += c.reasm.Expire(now);
            // a ping every keepalive interval: keeps an idle link up (and NAT mappings open) and measures the round trip
            if (now - c.last_ping >= config_.keepalive) {
                uint8_t f[5];
                Writer w{f, sizeof(f)};
                w.U8(uint8_t(Frame::Ping));
                w.U32(++c.ping_id);
                c.ping_sent = c.last_ping = now;
                SendFrame(c, f, w.n);
            }
        }
        ++it;
    }
}

}  // namespace net
