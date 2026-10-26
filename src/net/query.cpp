// The server information query (query.h; docs/pc_multiplayer.md "N3").
#include "net/query.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace net {
namespace {

using wire::Reader;
using wire::Writer;

constexpr char kInfoContext[] = "CSXN info v1";
// answer header: prefix, kind, page, pages, flags, nonce
constexpr size_t kAnswerHeader = kPrefix + 4 + 8;

enum InfoFlags : uint8_t { kPassword = 1, kDedicated = 2, kLanOnly = 4, kCollisionOff = 8, kFriendlyFire = 16 };

bool SodiumReady() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

// the signature over the context and the signed bytes, appended
void Sign(std::vector<uint8_t>& packet, const Identity& id) {
    std::vector<uint8_t> msg(sizeof(kInfoContext) - 1 + packet.size());
    std::memcpy(msg.data(), kInfoContext, sizeof(kInfoContext) - 1);
    std::memcpy(msg.data() + sizeof(kInfoContext) - 1, packet.data(), packet.size());
    uint8_t sig[kSignature];
    crypto_sign_detached(sig, nullptr, msg.data(), msg.size(), id.secret_key.data());
    packet.insert(packet.end(), sig, sig + kSignature);
}

bool Verify(const uint8_t* p, size_t n, const std::array<uint8_t, 32>& key) {
    if (n < kSignature) return false;
    std::vector<uint8_t> msg(sizeof(kInfoContext) - 1 + n - kSignature);
    std::memcpy(msg.data(), kInfoContext, sizeof(kInfoContext) - 1);
    std::memcpy(msg.data() + sizeof(kInfoContext) - 1, p, n - kSignature);
    return SodiumReady() && crypto_sign_verify_detached(p + n - kSignature, msg.data(), msg.size(), key.data()) == 0;
}

void Header(Writer& w, QueryKind kind, uint8_t page, uint8_t pages, uint64_t nonce) {
    wire::Prefix(w, Type::Info);
    w.U8(uint8_t(kind));
    w.U8(page);
    w.U8(pages);
    w.U8(0);
    w.U64(nonce);
}

// the header of an answer checked: Type::Info, the kind, the nonce; the reader left at the body
bool ReadHeader(Reader& r, QueryKind kind, uint64_t nonce, uint8_t& page, uint8_t& pages, std::string& error) {
    if (r.U32() != kMagic || r.U8() != kVersion || Type(r.U8()) != Type::Info) {
        error = "not an answer";
        return false;
    }
    if (QueryKind(r.U8()) != kind) {
        error = "another kind of answer";
        return false;
    }
    page = r.U8();
    pages = r.U8();
    r.U8();
    if (r.U64() != nonce || !r.ok) {
        error = "not the answer to this query";
        return false;
    }
    return true;
}

// players split into pages of at most kMaxDatagram bytes: the first player of each page
std::vector<size_t> PageStarts(const std::vector<PlayerInfo>& players) {
    std::vector<size_t> starts = {0};
    size_t used = kAnswerHeader + 1 + kSignature;
    for (size_t i = 0; i < players.size(); i++) {
        size_t size = 1 + std::min(players[i].name.size(), kMaxPlayerName) + 2 + 2 + 4 + 1;
        if (used + size > kMaxDatagram) {
            starts.push_back(i);
            used = kAnswerHeader + 1 + kSignature;
        }
        used += size;
    }
    return starts;
}

}  // namespace

static_assert(kAnswerHeader + 3 + (1 + kMaxServerName) + (1 + kMaxMapName) + (1 + kMaxGameName) + 4 + 6 + 32 +
              kSignature <= kInfoQuerySize, "an info answer is never larger than its query");

std::vector<uint8_t> EncodeQuery(QueryKind kind, uint8_t page, uint64_t nonce) {
    std::vector<uint8_t> p(QuerySize(uint8_t(kind)), 0);
    Writer w{p.data(), p.size()};
    wire::Prefix(w, Type::Query);
    w.U8(uint8_t(kind));
    w.U8(page);
    w.U16(0);
    w.U64(nonce);
    return p;
}

bool DecodeQuery(const uint8_t* p, size_t n, QueryKind& kind, uint8_t& page, uint64_t& nonce) {
    if (n != kInfoQuerySize && n != kPlayersQuerySize) return false;
    Reader r{p, n};
    if (r.U32() != kMagic || r.U8() != kVersion || Type(r.U8()) != Type::Query) return false;
    kind = QueryKind(r.U8());
    page = r.U8();
    r.U16();
    nonce = r.U64();
    return r.ok && (kind == QueryKind::Info || kind == QueryKind::Players) && n == QuerySize(uint8_t(kind));
}

std::vector<uint8_t> EncodeInfo(const ServerInfo& info, uint64_t nonce, const Identity& id) {
    std::vector<uint8_t> p(kInfoQuerySize);
    Writer w{p.data(), p.size()};
    Header(w, QueryKind::Info, 0, 1, nonce);
    w.U8(kInfoVersion);
    w.U8(info.protocol);
    w.U8(kVersion);
    w.Str(info.name, kMaxServerName);
    w.Str(info.map, kMaxMapName);
    w.Str(info.game, kMaxGameName);
    w.U8(info.humans);
    w.U8(info.bots);
    w.U8(info.max_players);
    w.U8(uint8_t((info.password ? kPassword : 0) | (info.dedicated ? kDedicated : 0) | (info.lan_only ? kLanOnly : 0) |
                 (info.collision ? 0 : kCollisionOff) | (info.friendly_fire ? kFriendlyFire : 0)));
    w.U16(info.round_seconds);
    w.U16(info.time_limit);
    w.U16(info.max_rounds);
    w.Bytes(id.public_key.data(), 32);
    p.resize(w.n);
    Sign(p, id);
    return p;
}

std::vector<uint8_t> EncodePlayers(const std::vector<PlayerInfo>& players, uint8_t page, uint64_t nonce,
                                   const Identity& id, uint8_t* pages_out) {
    std::vector<size_t> starts = PageStarts(players);
    uint8_t pages = uint8_t(std::min<size_t>(starts.size(), 255));
    if (pages_out) *pages_out = pages;
    if (page >= pages) return {};
    size_t first = starts[page], last = page + 1u < starts.size() ? starts[page + 1] : players.size();
    std::vector<uint8_t> p(kMaxDatagram);
    Writer w{p.data(), p.size()};
    Header(w, QueryKind::Players, page, pages, nonce);
    w.U8(uint8_t(last - first));
    for (size_t i = first; i < last; i++) {
        const PlayerInfo& pl = players[i];
        w.Str(pl.name, kMaxPlayerName);
        w.U16(uint16_t(pl.score));
        w.U16(pl.ping);
        w.U32(pl.seconds);
        w.U8(pl.bot ? 1 : 0);
    }
    if (!w.ok || w.n + kSignature > kMaxDatagram) return {};
    p.resize(w.n);
    Sign(p, id);
    return p;
}

bool DecodeInfo(const uint8_t* p, size_t n, uint64_t nonce, ServerInfo& out, std::string& error) {
    if (n < kAnswerHeader + kSignature || n > kMaxDatagram) {
        error = "wrong size";
        return false;
    }
    Reader r{p, n - kSignature};
    uint8_t page = 0, pages = 0;
    if (!ReadHeader(r, QueryKind::Info, nonce, page, pages, error)) return false;
    ServerInfo s;
    if (r.U8() != kInfoVersion) {
        error = "another information version";
        return false;
    }
    s.protocol = r.U8();
    uint8_t transport = r.U8();
    std::string name = r.Str(kMaxServerName), map = r.Str(kMaxMapName), game = r.Str(kMaxGameName);
    s.humans = r.U8();
    s.bots = r.U8();
    s.max_players = r.U8();
    uint8_t flags = r.U8();
    s.round_seconds = r.U16();
    s.time_limit = r.U16();
    s.max_rounds = r.U16();
    const uint8_t* key = r.Bytes(32);
    if (!r.ok || r.Left() != 0 || !key) {
        error = "malformed";
        return false;
    }
    if (transport != kVersion) {
        error = "another transport version (" + std::to_string(transport) + ")";
        return false;
    }
    std::memcpy(s.key.data(), key, 32);
    if (!Verify(p, n, s.key)) {
        error = "the signature does not verify";
        return false;
    }
    s.name = wire::CleanText(name, kMaxServerName);
    s.map = wire::CleanText(map, kMaxMapName);
    s.game = wire::CleanText(game, kMaxGameName);
    if (s.name.empty()) s.name = "(unnamed)";
    s.password = flags & kPassword;
    s.dedicated = flags & kDedicated;
    s.lan_only = flags & kLanOnly;
    s.collision = !(flags & kCollisionOff);
    s.friendly_fire = flags & kFriendlyFire;
    // counts that cannot be: more players than slots, more than a byte's worth of slots
    if (s.max_players == 0 || s.Total() > s.max_players) {
        error = "impossible player counts";
        return false;
    }
    out = std::move(s);
    return true;
}

bool DecodePlayers(const uint8_t* p, size_t n, uint64_t nonce, const std::array<uint8_t, 32>& key, uint8_t& page,
                   uint8_t& pages, std::vector<PlayerInfo>& out, std::string& error) {
    if (n < kAnswerHeader + 1 + kSignature || n > kMaxDatagram) {
        error = "wrong size";
        return false;
    }
    Reader r{p, n - kSignature};
    if (!ReadHeader(r, QueryKind::Players, nonce, page, pages, error)) return false;
    if (pages == 0 || page >= pages) {
        error = "malformed page";
        return false;
    }
    uint8_t count = r.U8();
    std::vector<PlayerInfo> list;
    for (int i = 0; i < count && r.ok; i++) {
        PlayerInfo pl;
        pl.name = wire::CleanText(r.Str(kMaxPlayerName), kMaxPlayerName);
        pl.score = int16_t(r.U16());
        pl.ping = r.U16();
        pl.seconds = r.U32();
        pl.bot = r.U8() & 1;
        list.push_back(std::move(pl));
    }
    if (!r.ok || r.Left() != 0) {
        error = "malformed";
        return false;
    }
    if (!Verify(p, n, key)) {
        error = "the signature does not verify";
        return false;
    }
    out = std::move(list);
    return true;
}

std::vector<uint8_t> QueryServer::Answer(const Address& from, const uint8_t* p, size_t n, double now) {
    QueryKind kind;
    uint8_t page;
    uint64_t nonce;
    if (!DecodeQuery(p, n, kind, page, nonce)) {
        stats_.malformed++;
        return {};
    }
    if (config_.lan_only && !from.Private()) {
        stats_.refused++;
        return {};
    }
    if (!limit_.Allow(from, now)) {
        stats_.rate_limited++;
        return {};
    }
    std::vector<uint8_t> a = kind == QueryKind::Info ? EncodeInfo(info_, nonce, identity_)
                                                     : EncodePlayers(players_, page, nonce, identity_);
    if (!a.empty()) stats_.answered++;
    return a;
}

std::string GameMode(const std::string& map) {
    if (map.rfind("de_", 0) == 0) return "Bomb Defusal";
    if (map.rfind("cs_", 0) == 0) return "Hostage Rescue";
    if (map.rfind("as_", 0) == 0) return "Assassination";
    return "";
}

}  // namespace net
