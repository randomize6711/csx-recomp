// The master server protocol, its state, and a server's registration (master.h; docs/pc_multiplayer.md "N3").
#include "net/master.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace net {
namespace {

using wire::Reader;
using wire::Writer;

constexpr char kMasterContext[] = "CSXN master v1";
constexpr size_t kChallengeSize = kPrefix + 8 + 4 + 32;
constexpr size_t kRegisterSigned = kPrefix + 8 + 4 + 32 + 1 + 32;
constexpr size_t kRegisterSize = kRegisterSigned + 64;
constexpr size_t kRegisteredSize = kPrefix + 8 + 2;
constexpr uint8_t kLeaving = 1;

bool SodiumReady() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

uint64_t RandomNonce() {
    uint64_t n = 0;
    if (SodiumReady()) randombytes_buf(&n, sizeof(n));
    return n;
}

// the bytes a register's signature covers: the context, then the packet up to the signature
std::vector<uint8_t> SignedPart(const uint8_t* p) {
    std::vector<uint8_t> msg(sizeof(kMasterContext) - 1 + kRegisterSigned);
    std::memcpy(msg.data(), kMasterContext, sizeof(kMasterContext) - 1);
    std::memcpy(msg.data() + sizeof(kMasterContext) - 1, p, kRegisterSigned);
    return msg;
}

bool Header(Reader& r, Type type) {
    return r.U32() == kMagic && r.U8() == kVersion && Type(r.U8()) == type;
}

// an address a master may list: a unicast IPv4 address with a port (private ones only when allowed)
bool Listable(const Address& a, bool allow_private) {
    if (a.port == 0 || a.ip[0] == 0 || a.ip[0] >= 224) return false;
    return allow_private || !a.Private();
}

}  // namespace

// ---- the master server -----------------------------------------------------------------------------------------------

MasterServer::MasterServer(Config config, Clock clock) :
    config_(config), clock_(std::move(clock)), limit_(config.per_source, config.total) {
    if (SodiumReady()) randombytes_buf(cookie_key_.data(), cookie_key_.size());
    start_ = clock_();
}

MasterServer::~MasterServer() {
    sodium_memzero(cookie_key_.data(), cookie_key_.size());
}

std::array<uint8_t, 32> MasterServer::Cookie(const Address& from, uint64_t nonce, uint32_t time) const {
    uint8_t msg[4 + 2 + 8 + 4];
    Writer w{msg, sizeof(msg)};
    w.Bytes(from.ip.data(), 4);
    w.U16(from.port);
    w.U64(nonce);
    w.U32(time);
    std::array<uint8_t, 32> mac{};
    crypto_auth_hmacsha256(mac.data(), msg, sizeof(msg), cookie_key_.data());
    return mac;
}

std::vector<MasterEntry> MasterServer::Entries() const {
    std::vector<MasterEntry> v;
    for (auto& [order, key] : order_) {
        auto it = servers_.find(key);
        if (it == servers_.end()) continue;
        MasterEntry e;
        e.address.ip = {uint8_t(key >> 40), uint8_t(key >> 32), uint8_t(key >> 24), uint8_t(key >> 16)};
        e.address.port = uint16_t(key);
        e.key = it->second.key;
        v.push_back(e);
    }
    return v;
}

void MasterServer::Expire() {
    double now = clock_();
    for (auto it = servers_.begin(); it != servers_.end();) {
        if (now - it->second.seen <= config_.expiry) {
            ++it;
            continue;
        }
        Address a;
        a.ip = {uint8_t(it->first >> 40), uint8_t(it->first >> 32), uint8_t(it->first >> 24), uint8_t(it->first >> 16)};
        a.port = uint16_t(it->first);
        Log("expired " + a.ToString());
        order_.erase(it->second.order);
        if (--per_ip_[it->first >> 16] == 0) per_ip_.erase(it->first >> 16);
        it = servers_.erase(it);
        stats_.expired++;
    }
}

void MasterServer::Handle(const Address& from, const uint8_t* p, size_t n, const Send& send) {
    double now = clock_();
    if (now - expired_at_ >= 1.0) {
        expired_at_ = now;
        Expire();
    }
    if (n < kPrefix || n > kMaxDatagram) {
        stats_.malformed++;
        return;
    }
    Reader head{p, n};
    if (head.U32() != kMagic || head.U8() != kVersion) {
        stats_.malformed++;
        return;
    }
    Type type = Type(head.U8());
    if (type != Type::Heartbeat && type != Type::Register && type != Type::ListRequest) {
        stats_.malformed++;
        return;
    }
    if (!limit_.Allow(from, now)) {
        stats_.rate_limited++;
        return;
    }
    if (type == Type::Heartbeat) {
        if (n != kHeartbeatSize) {
            stats_.malformed++;
            return;
        }
        Reader r{p + kPrefix, n - kPrefix};
        uint64_t nonce = r.U64();
        uint32_t time = uint32_t(now - start_);
        std::vector<uint8_t> out(kChallengeSize);
        Writer w{out.data(), out.size()};
        wire::Prefix(w, Type::MasterChallenge);
        w.U64(nonce);
        w.U32(time);
        auto cookie = Cookie(from, nonce, time);
        w.Bytes(cookie.data(), cookie.size());
        stats_.heartbeats++;
        send(from, out);
        return;
    }
    if (type == Type::Register) {
        if (n != kRegisterSize) {
            stats_.malformed++;
            return;
        }
        Reader r{p + kPrefix, n - kPrefix};
        uint64_t nonce = r.U64();
        uint32_t time = r.U32();
        const uint8_t* cookie = r.Bytes(32);
        uint8_t flags = r.U8();
        const uint8_t* key = r.Bytes(32);
        const uint8_t* sig = r.Bytes(64);
        if (!r.ok) {
            stats_.malformed++;
            return;
        }
        auto expect = Cookie(from, nonce, time);
        double age = (now - start_) - double(time);
        if (sodium_memcmp(expect.data(), cookie, 32) != 0 || age < -1 || age > config_.cookie_lifetime) {
            stats_.bad_cookie++;
            return;
        }
        std::vector<uint8_t> msg = SignedPart(p);
        if (crypto_sign_verify_detached(sig, msg.data(), msg.size(), key) != 0) {
            stats_.bad_signature++;
            return;
        }
        uint64_t k = from.Key();
        auto it = servers_.find(k);
        std::string who = from.ToString() + " " + Identity::Fingerprint(*reinterpret_cast<const std::array<uint8_t, 32>*>(key));
        if (flags & kLeaving) {
            if (it != servers_.end() && std::memcmp(it->second.key.data(), key, 32) == 0) {
                order_.erase(it->second.order);
                if (--per_ip_[k >> 16] == 0) per_ip_.erase(k >> 16);
                servers_.erase(it);
                stats_.left++;
                Log("left " + who);
            }
            return;
        }
        if (!Listable(from, config_.allow_private)) {
            stats_.refused++;
            Log("refused " + from.ToString() + " (an address this master does not list)");
            return;
        }
        if (it == servers_.end()) {
            if (servers_.size() >= config_.max_servers || per_ip_[k >> 16] >= config_.max_per_ip) {
                if (per_ip_[k >> 16] == 0) per_ip_.erase(k >> 16);
                stats_.refused++;
                Log("refused " + from.ToString() + " (the list or its address's share is full)");
                return;
            }
            Server s;
            std::memcpy(s.key.data(), key, 32);
            s.seen = now;
            s.order = next_order_++;
            order_[s.order] = k;
            per_ip_[k >> 16]++;
            servers_[k] = s;
            stats_.registered++;
            Log("listed " + who);
        } else {
            if (std::memcmp(it->second.key.data(), key, 32) != 0) {
                std::memcpy(it->second.key.data(), key, 32);
                Log("new identity for " + who);
            }
            it->second.seen = now;
            stats_.refreshed++;
        }
        std::vector<uint8_t> out(kRegisteredSize);
        Writer w{out.data(), out.size()};
        wire::Prefix(w, Type::Registered);
        w.U64(nonce);
        w.U16(config_.heartbeat);
        send(from, out);
        return;
    }
    // a list request: a page of the servers in registration order
    if (n != kMaxDatagram) {
        stats_.malformed++;
        return;
    }
    Reader r{p + kPrefix, n - kPrefix};
    uint64_t nonce = r.U64();
    uint32_t page = r.U32();
    std::vector<MasterEntry> all = Entries();
    uint32_t pages = uint32_t(std::max<size_t>(1, (all.size() + kListPerPage - 1) / kListPerPage));
    size_t first = size_t(page) * kListPerPage, count = first < all.size() ? std::min(kListPerPage, all.size() - first) : 0;
    std::vector<uint8_t> out(kListHeader + count * kListEntry);
    Writer w{out.data(), out.size()};
    wire::Prefix(w, Type::List);
    w.U64(nonce);
    w.U32(page);
    w.U32(pages);
    w.U32(uint32_t(all.size()));
    w.U16(uint16_t(count));
    for (size_t i = first; i < first + count; i++) {
        w.Bytes(all[i].address.ip.data(), 4);
        w.U16(all[i].address.port);
        w.Bytes(all[i].key.data(), 32);
    }
    stats_.lists++;
    send(from, out);
}

// ---- a server's registration ---------------------------------------------------------------------------------------

std::vector<uint8_t> Registrar::RegisterPacket(const State& s, bool leaving) const {
    std::vector<uint8_t> out(kRegisterSize);
    Writer w{out.data(), out.size()};
    wire::Prefix(w, Type::Register);
    w.U64(s.nonce);
    w.U32(s.time);
    w.Bytes(s.cookie.data(), 32);
    w.U8(leaving ? kLeaving : 0);
    w.Bytes(identity_.public_key.data(), 32);
    std::vector<uint8_t> msg = SignedPart(out.data());
    crypto_sign_detached(out.data() + kRegisterSigned, nullptr, msg.data(), msg.size(), identity_.secret_key.data());
    return out;
}

void Registrar::Pump(const Send& send) {
    double now = clock_();
    for (const Address& m : masters_) {
        State& s = state_[m.Key()];
        if (now < s.next) continue;
        if (s.listed && s.nonce && s.cookie_at < 0) {  // the last heartbeat got no answer
            s.listed = false;
            Log("master " + m.ToString() + " did not answer: not listed there until it does");
        }
        s.nonce = RandomNonce();
        s.cookie_at = -1;
        s.next = now + (s.listed ? s.interval : 15);
        std::vector<uint8_t> hb(kHeartbeatSize, 0);
        Writer w{hb.data(), hb.size()};
        wire::Prefix(w, Type::Heartbeat);
        w.U64(s.nonce);
        send(m, hb);
    }
}

bool Registrar::Handle(const Address& from, Type type, const uint8_t* p, size_t n, const Send& send) {
    auto it = state_.find(from.Key());
    if (it == state_.end() || (type != Type::MasterChallenge && type != Type::Registered)) return false;
    State& s = it->second;
    Reader r{p, n};
    if (!Header(r, type)) return true;
    if (type == Type::MasterChallenge) {
        if (n != kChallengeSize || r.U64() != s.nonce) return true;
        s.time = r.U32();
        std::memcpy(s.cookie.data(), r.Bytes(32), 32);
        s.cookie_at = clock_();
        send(from, RegisterPacket(s, false));
        return true;
    }
    if (n != kRegisteredSize || r.U64() != s.nonce) return true;
    s.interval = std::clamp<double>(r.U16(), 10, 600);
    if (!s.listed) Log("listed on the master server " + from.ToString() + " (heartbeat every " +
                       std::to_string(int(s.interval)) + " s)");
    s.listed = true;
    s.next = clock_() + s.interval;
    return true;
}

void Registrar::Leave(const Send& send) {
    double now = clock_();
    for (const Address& m : masters_) {
        auto it = state_.find(m.Key());
        if (it == state_.end() || !it->second.listed || it->second.cookie_at < 0 || now - it->second.cookie_at > 25) continue;
        send(m, RegisterPacket(it->second, true));
        Log("told the master server " + m.ToString() + " the server is leaving");
    }
}

// ---- the browser's side -----------------------------------------------------------------------------------------------

std::vector<uint8_t> EncodeListRequest(uint64_t nonce, uint32_t page) {
    std::vector<uint8_t> p(kMaxDatagram, 0);
    Writer w{p.data(), p.size()};
    wire::Prefix(w, Type::ListRequest);
    w.U64(nonce);
    w.U32(page);
    return p;
}

bool DecodeList(const uint8_t* p, size_t n, uint64_t nonce, uint32_t& page, uint32_t& pages, uint32_t& total,
                std::vector<MasterEntry>& out, std::string& error) {
    Reader r{p, n};
    if (n < kListHeader || n > kMaxDatagram || !Header(r, Type::List)) {
        error = "not a list";
        return false;
    }
    if (r.U64() != nonce) {
        error = "not the answer to this request";
        return false;
    }
    page = r.U32();
    pages = r.U32();
    total = r.U32();
    uint16_t count = r.U16();
    if (!r.ok || count > kListPerPage || r.Left() != size_t(count) * kListEntry || pages == 0 || page >= pages) {
        error = "malformed";
        return false;
    }
    out.clear();
    for (int i = 0; i < count; i++) {
        MasterEntry e;
        std::memcpy(e.address.ip.data(), r.Bytes(4), 4);
        e.address.port = r.U16();
        std::memcpy(e.key.data(), r.Bytes(32), 32);
        if (Listable(e.address, true)) out.push_back(e);
    }
    return true;
}

}  // namespace net
