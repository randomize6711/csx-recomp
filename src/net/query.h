// The server information query (docs/pc_multiplayer.md "N3"): what a browser asks a server outside any connection,
// on the server's own game port (the transport's socket, transport.h Type::Query / Type::Info), and what it answers.
//
//   query  (client -> server): prefix, kind (1 info, 2 players), page, 2 reserved bytes, the client's nonce, zeros to
//          the size of the largest answer of its kind (info: 256 bytes, players: a full 1200-byte datagram). Only a
//          query of exactly that size is answered: no answer is larger than the question, so a server cannot be used
//          to amplify a spoofed flood.
//   info   (server -> client): prefix, kind, page, pages, flags reserved, the nonce, the body, then an Ed25519
//          signature by the server's identity (N2) over "CSXN info v1" and everything before it. The body of an info
//          answer carries the identity key; the signature proves the answer comes from whoever holds that key and was
//          made for this query (the nonce), so a browser can tie a listing to the key the game connection will later
//          prove again (N2 trust: known servers, pinned keys). It proves nothing about the server's honesty.
//
// Answers are rate limited per source and in total; malformed or short queries are dropped without an answer. Every
// field a browser reads is bounded and its text cleaned (wire.h CleanText) before it reaches the menus.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/transport.h"
#include "net/wire.h"

namespace net {

enum class QueryKind : uint8_t { Info = 1, Players = 2 };

constexpr size_t kMaxServerName = 63;  // csx-server's name (server_config.cpp) and the engine's hostname
constexpr size_t kMaxMapName = 31;
constexpr size_t kMaxGameName = 15;
constexpr size_t kMaxPlayerName = 31;
constexpr size_t kSignature = 64;
constexpr uint8_t kInfoVersion = 1;
constexpr size_t kInfoQuerySize = 256;  // the largest info answer (239 bytes) fits
constexpr size_t kPlayersQuerySize = kMaxDatagram;
constexpr size_t QuerySize(uint8_t kind) { return kind == 1 ? kInfoQuerySize : kPlayersQuerySize; }

struct ServerInfo {
    std::string name, map, game = "cstrike";
    uint8_t protocol = 47;              // the engine's network protocol (G8.2B)
    uint8_t humans = 0, bots = 0;       // players in the match: network and local players, and bots
    uint8_t max_players = 0;            // player slots (a dedicated server's are all for players and bots)
    bool password = false, dedicated = false, lan_only = false, collision = true, friendly_fire = false;
    uint16_t round_seconds = 0;         // mp_roundtime
    uint16_t time_limit = 0;            // minutes a map lasts (mp_timelimit), 0 none
    uint16_t max_rounds = 0;            // rounds a map lasts (mp_maxrounds), 0 none
    std::array<uint8_t, 32> key{};      // the server's identity key (signed for)
    uint16_t Total() const { return uint16_t(humans + bots); }
    bool Full() const { return max_players && Total() >= max_players; }
};

struct PlayerInfo {
    std::string name;
    int16_t score = 0;
    uint16_t ping = 0;      // milliseconds (0 for bots and the local player)
    uint32_t seconds = 0;   // connected for
    bool bot = false;
};

// a query datagram (QuerySize bytes)
std::vector<uint8_t> EncodeQuery(QueryKind kind, uint8_t page, uint64_t nonce);
// a query read back; false for anything but a well-formed query of its kind's size
bool DecodeQuery(const uint8_t* p, size_t n, QueryKind& kind, uint8_t& page, uint64_t& nonce);

// answers, signed with the server's identity (the info's key is taken from it)
std::vector<uint8_t> EncodeInfo(const ServerInfo& info, uint64_t nonce, const Identity& id);
// the players answer for one page (pages of at most kMaxDatagram bytes); `pages` receives how many there are
std::vector<uint8_t> EncodePlayers(const std::vector<PlayerInfo>& players, uint8_t page, uint64_t nonce,
                                   const Identity& id, uint8_t* pages = nullptr);

// an answer read back and checked: the type and kind, the nonce the browser sent, every bound, the strings cleaned,
// the signature by the key it carries (players: by `key`, the one the server's info proved). False with an error text.
bool DecodeInfo(const uint8_t* p, size_t n, uint64_t nonce, ServerInfo& out, std::string& error);
bool DecodePlayers(const uint8_t* p, size_t n, uint64_t nonce, const std::array<uint8_t, 32>& key, uint8_t& page,
                   uint8_t& pages, std::vector<PlayerInfo>& out, std::string& error);

// the server side: answers queries from the information it is given (on the game thread, once a second or so)
class QueryServer {
public:
    struct Config {
        double per_source = 8;   // answers a second to one address (a burst of twice that)
        double total = 400;      // answers a second to everyone together
        bool lan_only = false;   // a LAN-only server answers private addresses only (as it takes only them)
    };
    QueryServer(Config config, Identity identity) : config_(config), identity_(identity),
        limit_(config.per_source, config.total) {}
    void Set(ServerInfo info, std::vector<PlayerInfo> players) {
        info_ = std::move(info);
        players_ = std::move(players);
    }
    // the answer to a datagram that arrived as Type::Query, empty when none is due (malformed, rate limited)
    std::vector<uint8_t> Answer(const Address& from, const uint8_t* p, size_t n, double now);
    struct Stats {
        uint64_t answered = 0, malformed = 0, rate_limited = 0, refused = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    Config config_;
    Identity identity_;
    wire::RateLimiter limit_;
    ServerInfo info_;
    std::vector<PlayerInfo> players_;
    Stats stats_;
};

// the game mode a map's name gives (de_ bomb defusal, cs_ hostage rescue, as_ assassination), "" when none
std::string GameMode(const std::string& map);

}  // namespace net
