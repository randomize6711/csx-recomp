// The PC network transport (transport.h; docs/pc_multiplayer.md "N1 transport").
#include "net/transport.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
constexpr char kServerContext[] = "CSXN server v1";
// accept: prefix, connection id, the client's nonce, the server's identity key, its signature, an empty AEAD message
constexpr size_t kAcceptSize = kPrefix + 4 + 8 + 32 + 64 + kTag;
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
    case Reason::ServerIdentityInvalid: return "the server's identity did not verify";
    case Reason::ServerIdentityChanged: return "the server's identity key changed";
    case Reason::LanOnly: return "the server takes players from its local network only";
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
#ifndef _WIN32
            ::chmod(path.c_str(), 0600);  // a key file made before it was written owner-only
#endif
            crypto_sign_ed25519_sk_to_pk(id.public_key.data(), id.secret_key.data());
            return id;
        }
        error = path + " is not an identity file";
        return std::nullopt;
    }
    crypto_sign_keypair(id.public_key.data(), id.secret_key.data());
    // the secret key readable by its owner only, written whole or not at all (a temporary file renamed over)
    std::string tmp = path + ".tmp";
    uint8_t file[8 + 64];
    std::memcpy(file, "CSXNID1\n", 8);
    std::memcpy(file + 8, id.secret_key.data(), 64);
    bool ok = false;
#ifdef _WIN32
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(file, 1, sizeof(file), f) == sizeof(file);
        ok = std::fclose(f) == 0 && ok;
    }
    ok = ok && (std::remove(path.c_str()), std::rename(tmp.c_str(), path.c_str()) == 0);
#else
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ok = ::write(fd, file, sizeof(file)) == ssize_t(sizeof(file)) && ::fsync(fd) == 0;
        ok = ::close(fd) == 0 && ok;
    }
    ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    sodium_memzero(file, sizeof(file));
    if (!ok) {
        std::remove(tmp.c_str());
        error = "cannot write " + path;
        return std::nullopt;
    }
    return id;
}

std::string Identity::Fingerprint(const std::array<uint8_t, 32>& public_key) {
    char s[65];
    for (int i = 0; i < 32; i++) std::snprintf(s + 2 * i, 3, "%02x", public_key[i]);
    return s;
}

std::optional<std::array<uint8_t, 32>> Identity::ParseFingerprint(const std::string& text) {
    if (text.size() != 64) return std::nullopt;
    std::array<uint8_t, 32> k{};
    for (size_t i = 0; i < 64; i++) {
        char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return std::nullopt;
        k[i / 2] = uint8_t(k[i / 2] << 4 | v);
    }
    return k;
}

// ---- known servers ---------------------------------------------------------------------------------------------------
std::map<std::string, std::string> KnownServers::Read() const {
    std::map<std::string, std::string> m;
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string key = line.substr(sp + 1);
        while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
        if (Identity::ParseFingerprint(key)) m[line.substr(0, sp)] = key;
    }
    return m;
}

KnownServers::Result KnownServers::Check(const std::string& server, const std::string& key, std::string* old) const {
    auto m = Read();
    auto it = m.find(server);
    if (it == m.end()) return Result::New;
    if (it->second == key) return Result::Match;
    if (old) *old = it->second;
    return Result::Changed;
}

bool KnownServers::Remember(const std::string& server, const std::string& key, std::string& error) {
    if (server.find_first_of(" \n") != std::string::npos || !Identity::ParseFingerprint(key)) {
        error = "not an address and key";
        return false;
    }
    auto m = Read();
    m[server] = key;
    std::string text = "# servers this install connected to and their identity keys (trust on first use):\n"
                       "# a server whose key changes is refused until its line is removed\n";
    for (auto& [a, k] : m) text += a + " " + k + "\n";
    std::string tmp = path_ + ".tmp";
    bool ok = false;
#ifdef _WIN32
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fclose(f) == 0 && ok;
    }
    ok = ok && (std::remove(path_.c_str()), std::rename(tmp.c_str(), path_.c_str()) == 0);
#else
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size()) && ::fsync(fd) == 0;
        ok = ::close(fd) == 0 && ok;
    }
    ok = ok && std::rename(tmp.c_str(), path_.c_str()) == 0;
#endif
    if (!ok) {
        std::remove(tmp.c_str());
        error = "cannot write " + path_;
    }
    return ok;
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
    std::array<uint8_t, 32> client_pk{};  // server: the key the client connected with (a new one is a new client)
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
    if (!SodiumReady()) {  // no keys can be made: nothing may be sent under them
        std::fprintf(stderr, "net: libsodium did not initialise\n");
        std::abort();
    }
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
    // every source together: a flood from many (spoofed) addresses costs a bounded amount of work a second
    double global_burst = 2 * config_.hello_rate_total;
    if (global_.last == 0) global_.tokens = global_burst;
    global_.tokens = std::min(global_burst, global_.tokens + (now - global_.last) * config_.hello_rate_total);
    global_.last = now;
    if (global_.tokens < 1) {
        stats_.dropped_rate++;
        return false;
    }
    global_.tokens -= 1;
    // quiet sources forgotten at most once a second (not a scan per packet); a full table takes no new source
    if (now - buckets_cleaned_ >= 1.0) {
        buckets_cleaned_ = now;
        for (auto it = buckets_.begin(); it != buckets_.end();) it = now - it->second.last > 10 ? buckets_.erase(it) : ++it;
    }
    uint64_t ip = from.Key() >> 16;
    if (buckets_.size() >= config_.rate_sources && !buckets_.count(ip)) {
        stats_.dropped_rate++;
        return false;
    }
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
            SendReject(from, Reason::Version, "transport version " + std::to_string(kVersion), 0);
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
    if (config_.lan_only && !from.Private()) {
        SendReject(from, Reason::LanOnly, ReasonText(Reason::LanOnly), nonce);
        stats_.handshakes_failed++;
        return;
    }
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
    if (config_.lan_only && !from.Private()) {  // its hello was refused; no cookie for it can exist, but not even then
        stats_.dropped_address++;
        return;
    }
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
    // a connect for a connection already made with this key (our accept was lost): answer it again. One that is
    // closing, or made with another key (the client restarted on the same port: the cookie proved the address), is
    // replaced by a new connection
    if (auto it = conns_.find(from.Key()); it != conns_.end()) {
        Conn& old = *it->second;
        if (old.closing || !client_pk || sodium_memcmp(old.client_pk.data(), client_pk, 32) != 0) {
            if (!old.closing) Close(old, Reason::Closed, "replaced by a new connection from the same address", false, false);
            by_id_.erase(old.id);
            conns_.erase(it);
        }
    }
    if (auto it = conns_.find(from.Key()); it != conns_.end()) {
        SendAccept(*it->second, nonce);
        return;
    }
    if (!identity_) {  // a server without its identity cannot prove itself: no connection (the engine glue loads one)
        stats_.handshakes_failed++;
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
            SendReject(from, Reason::IdentityInvalid, ReasonText(Reason::IdentityInvalid), nonce);
            stats_.handshakes_failed++;
            return;
        }
        std::array<uint8_t, 32> pk{};
        std::memcpy(pk.data(), id_pk, 32);
        identity = Identity::Fingerprint(pk);
    } else if (config_.require_identity) {
        SendReject(from, Reason::IdentityRequired, ReasonText(Reason::IdentityRequired), nonce);
        stats_.handshakes_failed++;
        return;
    }
    size_t same_ip = 0;
    size_t open = 0;
    for (auto& [k, c] : conns_) {
        open += !c->closing;
        same_ip += !c->closing && (k >> 16) == (from.Key() >> 16);
    }
    if (open >= config_.max_connections || same_ip >= config_.max_connections_per_ip) {
        SendReject(from, Reason::Full, ReasonText(Reason::Full), nonce);
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
    std::memcpy(c->client_pk.data(), client_pk, 32);
    c->last_in = c->last_out = clock_();
    Conn& ref = *c;
    by_id_[c->id] = c.get();
    conns_[from.Key()] = std::move(c);
    SendAccept(ref, nonce);
    stats_.handshakes_ok++;
    events_.push_back({Event::Connected, from, Reason::None, {}, identity, false});
}

// what the server's identity signs in an accept: both key exchange keys (so no one between can put in their own), the
// client's nonce and the connection id
static void ServerSigned(Writer& m, const uint8_t* server_kx, const uint8_t* client_kx, uint64_t nonce, uint32_t id) {
    m.Bytes(kServerContext, sizeof(kServerContext));
    m.Bytes(server_kx, 32);
    m.Bytes(client_kx, 32);
    m.U64(nonce);
    m.U32(id);
}

// accept: the connection id, the client's nonce, the server's identity key and its signature of the handshake, and an
// empty AEAD message (all of it as associated data) proving the session keys match. Sent again unchanged when the
// client repeats its connect (the signature is deterministic, the AEAD message empty under the same key and nonce)
void Endpoint::SendAccept(Conn& c, uint64_t nonce) {
    uint8_t sig[64];
    uint8_t msg[sizeof(kServerContext) + 32 + 32 + 8 + 4];
    Writer m{msg, sizeof(msg)};
    ServerSigned(m, kx_public_.data(), c.client_pk.data(), nonce, c.id);
    crypto_sign_detached(sig, nullptr, msg, m.n, identity_->secret_key.data());
    uint8_t out[kAcceptSize];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Accept);
    w.U32(c.id);
    w.U64(nonce);
    w.Bytes(identity_->public_key.data(), 32);
    w.Bytes(sig, 64);
    uint8_t nonce_bytes[kNonce];
    NonceOf(0, nonce_bytes);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + w.n, &clen, nullptr, 0, out, w.n, nullptr, nonce_bytes, c.tx.data());
    if (socket_->Send(c.peer, out, w.n + size_t(clen))) {
        stats_.datagrams_out++;
        stats_.bytes_out += w.n + clen;
    }
}

void Endpoint::HandleAccept(const Address& from, const uint8_t* p, size_t n) {
    auto it = pending_.find(from.Key());
    if (role_ != Role::Client || it == pending_.end() || !it->second->challenged || n != kAcceptSize) {
        stats_.dropped_unknown++;
        return;
    }
    Pending& h = *it->second;
    Reader r{p + kPrefix, n - kPrefix};
    uint32_t id = r.U32();
    uint64_t nonce = r.U64();
    std::array<uint8_t, 32> server_id{};
    std::memcpy(server_id.data(), r.Bytes(32), 32);
    const uint8_t* sig = r.Bytes(64);
    if (nonce != h.nonce || id == 0) {
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
    size_t ad = kAcceptSize - kTag;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(nullptr, nullptr, nullptr, p + ad, kTag, p, ad, nonce_bytes,
                                                  c->rx.data()) != 0) {
        stats_.dropped_auth++;
        return;
    }
    // the keys match, so this came from whoever holds the key exchange secret the challenge named; the signature says
    // that is the holder of server_id. A wrong one ends the attempt (a third party cannot make one: it has no keys)
    uint8_t msg[sizeof(kServerContext) + 32 + 32 + 8 + 4];
    Writer m{msg, sizeof(msg)};
    ServerSigned(m, h.server_pk.data(), h.pk.data(), nonce, id);
    if (crypto_sign_verify_detached(sig, msg, m.n, server_id.data()) != 0) {
        events_.push_back({Event::Closed, from, Reason::ServerIdentityInvalid, ReasonText(Reason::ServerIdentityInvalid),
                           {}, false});
        pending_.erase(it);
        stats_.handshakes_failed++;
        hold_[from.Key()] = clock_() + config_.reconnect_hold;
        return;
    }
    sodium_memzero(h.sk.data(), h.sk.size());
    c->identity = Identity::Fingerprint(server_id);
    std::string why;
    if (server_check_ && server_check_(from, c->identity, why) != ServerTrust::Trusted) {
        // refused: the server is told (it would keep the connection until its timeout otherwise), no packet goes to it
        pending_.erase(it);
        Conn& ref = *c;
        if (auto old = conns_.find(from.Key()); old != conns_.end()) by_id_.erase(old->second->id);
        by_id_[id] = c.get();
        conns_[from.Key()] = std::move(c);
        stats_.handshakes_failed++;
        hold_[from.Key()] = clock_() + config_.reconnect_hold;
        Close(ref, Reason::ServerIdentityChanged, why.empty() ? ReasonText(Reason::ServerIdentityChanged) : why, true,
              false);
        return;
    }
    c->last_in = c->last_out = clock_();
    auto queue = std::move(h.queue);
    pending_.erase(it);
    Conn& ref = *c;
    if (auto old = conns_.find(from.Key()); old != conns_.end()) by_id_.erase(old->second->id);
    by_id_[id] = c.get();
    conns_[from.Key()] = std::move(c);
    stats_.handshakes_ok++;
    events_.push_back({Event::Connected, from, Reason::None, {}, ref.identity, false});
    for (auto& m : queue) SendMessage(ref, m.data(), m.size());
}

// reject: prefix, reason, the client's handshake nonce (0 for a version reject, sent before the hello is understood),
// text length, text. The nonce is what makes an off-path forgery fail: the client believes a reject only with its own
void Endpoint::SendReject(const Address& to, Reason reason, const std::string& text, uint64_t nonce) {
    uint8_t out[kPrefix + 2 + 8 + 255];
    Writer w{out, sizeof(out)};
    Prefix(w, Type::Reject);
    w.U8(uint8_t(reason));
    w.U64(nonce);
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
    uint64_t nonce = r.U64();
    uint8_t len = r.U8();
    const uint8_t* t = r.Bytes(len);
    std::string text = t ? std::string(reinterpret_cast<const char*>(t), len) : ReasonText(reason);
    // a reject is not encrypted: believed when it names this handshake's nonce (which only the server and the path
    // saw), or, before any challenge came, a version reject (the server could not read our hello)
    if (!r.ok || !(nonce == it->second->nonce || (reason == Reason::Version && !it->second->challenged))) {
        stats_.dropped_auth++;
        return;
    }
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
    // a closing connection's late packets are dropped unread: they neither keep it alive nor get an answer (its
    // send key is gone)
    if (c.closing) {
        stats_.dropped_unknown++;
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
    // a server's packets from that peer not yet taken are not delivered (a refused connection's first packets: its
    // engine connect must not reach the engine)
    if (role_ == Role::Server)
        inbox_.erase(std::remove_if(inbox_.begin(), inbox_.end(), [&](const auto& m) { return m.first == peer; }),
                     inbox_.end());
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
