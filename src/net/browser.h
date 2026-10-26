// The server browser's discovery (docs/pc_multiplayer.md "N3"): finds servers and keeps what they answered, for the
// menus (src/game/pc_browser.cpp) to show. Host code, no game state; single threaded (the game runs it on a thread of
// its own so a scan never waits for a frame and pings are measured when the answer arrives). Sources:
//   LAN        a query broadcast to the LAN's broadcast addresses on each LAN port (27015-27020 by default); every
//              server that answers is listed; repeated while the LAN list is watched, a server silent for
//              lan_expiry seconds disappears
//   internet   the master servers' lists (master.h), then a query to each listed server, paced
//   favourites, recent servers, an address typed: queried directly
// Every answer is checked (query.h: nonce, bounds, the signature by the key it carries); a server whose proven key
// differs from the one its listing or favourite expected is kept apart (problem set), never merged or shown as the
// expected server. Bounded: max_servers entries, queries paced at send_rate a second with at most max_outstanding
// unanswered.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/master.h"
#include "net/query.h"
#include "net/transport.h"

namespace net {

enum Source : uint8_t { kLan = 1, kInternet = 2, kFavorite = 4, kRecent = 8, kDirect = 16 };

struct BrowserConfig {
    std::vector<uint16_t> lan_ports = {27015, 27016, 27017, 27018, 27019, 27020};
    std::vector<Address> broadcast;  // where LAN queries go (LanBroadcastAddresses())
    std::vector<Address> masters;
    double timeout = 1.0;            // an unanswered query is sent again after this long
    int tries = 2;                   // then the server is not responding
    size_t max_outstanding = 64;
    double send_rate = 200;          // queries a second at most (an info query is 256 bytes: 51 KB/s)
    double lan_interval = 3;         // while the LAN list is watched, a broadcast this often
    double lan_expiry = 10;          // a LAN server silent this long is dropped
    size_t max_servers = 4096;
    uint32_t max_master_pages = 137; // 4110 listed servers
    double master_timeout = 2;       // a list page unanswered this long is asked again, three times
};

struct BrowserEntry {
    Address address;
    uint8_t sources = 0;
    enum class State : uint8_t { Waiting, Answered, NoAnswer, Refused } state = State::Waiting;
    ServerInfo info;
    bool have_info = false;
    std::string key;           // the identity key the answer proved (Fingerprint), "" until one did
    std::string expected_key;  // the key its listing or favourite named, "" none
    std::string problem;       // why it is refused (another key than expected, a malformed answer)
    int ping = -1;             // milliseconds, the last answer's round trip
    double last_answer = -1;   // when (the browser's clock)
    std::vector<PlayerInfo> players;
    bool players_complete = false;
};

// the LAN's broadcast addresses: each IPv4 interface's (its subnet's), the loopback network's and 255.255.255.255
std::vector<Address> LanBroadcastAddresses();

class Browser {
public:
    Browser(std::unique_ptr<DatagramSocket> socket, BrowserConfig config, Clock clock);

    void RefreshLan();                      // a broadcast now
    void WatchLan(bool on) { watch_lan_ = on; }
    void RefreshInternet();                 // the masters' lists again, then every listed server
    // a server known by address (favourite, recent, typed), queried now; expected_key from a favourite
    void Add(const Address& a, uint8_t source, const std::string& expected_key = "");
    void Remove(const Address& a, uint8_t source);  // that source no longer lists it
    void Refresh(const Address& a);                 // queried again
    void RefreshSource(uint8_t source);             // every server of that source queried again
    void QueryPlayers(const Address& a);
    void SetMasters(std::vector<Address> masters) { config_.masters = std::move(masters); }

    void Pump();                  // answers read, queries sent, timers run
    void Wait(double seconds);    // until a datagram arrives or the time passes (the browser thread's sleep)

    const std::vector<BrowserEntry>& Entries() const { return entries_; }
    const BrowserEntry* Find(const Address& a) const;
    uint64_t Version() const { return version_; }  // changes whenever an entry does
    bool Busy() const;                             // queries or list pages still unanswered
    bool Listing() const;                          // master lists still arriving
    size_t MastersAnswered() const { return masters_answered_; }

    struct Stats {
        uint64_t queries = 0, query_bytes = 0, answers = 0, answer_bytes = 0, malformed = 0, wrong_key = 0,
                 unexpected = 0, broadcasts = 0, list_requests = 0, list_pages = 0, dropped_full = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Query {
        uint64_t nonce = 0;
        double sent = -1;
        int tries = 0;
        bool queued = false, outstanding = false;
        uint8_t players_page = 0, players_pages = 0;
        std::vector<std::vector<PlayerInfo>> pages;
    };
    struct MasterFetch {
        uint64_t nonce = 0;
        uint32_t page = 0, pages = 1;
        double sent = -1;
        int tries = 0;
        bool done = true;
    };
    BrowserEntry* Entry(const Address& a, bool create);
    void Queue(BrowserEntry& e);
    void SendQuery(BrowserEntry& e, Query& q);
    void SendListPage(const Address& master, MasterFetch& f);
    void HandleInfo(const Address& from, const uint8_t* p, size_t n);
    void HandlePlayers(const Address& from, const uint8_t* p, size_t n);
    void HandleList(const Address& from, const uint8_t* p, size_t n);
    void Accept(BrowserEntry& e, const ServerInfo& info, double rtt);
    void Erase(const Address& a);
    void Changed() { version_++; }

    std::unique_ptr<DatagramSocket> socket_;
    BrowserConfig config_;
    Clock clock_;
    std::vector<BrowserEntry> entries_;
    std::unordered_map<uint64_t, size_t> index_;  // address key -> entries_
    std::unordered_map<uint64_t, Query> queries_;
    std::vector<uint64_t> queue_;                 // address keys to query, in order
    size_t queue_head_ = 0;
    std::map<uint64_t, MasterFetch> masters_;     // by master address key
    size_t masters_answered_ = 0;
    uint64_t lan_nonce_ = 0;
    double lan_sent_ = -1, next_lan_ = 0, tokens_ = 0, tokens_at_ = -1;
    bool watch_lan_ = false;
    uint64_t version_ = 1;
    Stats stats_;
};

// ---- what the menus show: a filtered, sorted view of the entries -------------------------------------------------
enum class SortKey : uint8_t { Ping, Players, Name, Map };
struct ViewOptions {
    uint8_t sources = kLan;        // the tab: entries with any of these sources
    SortKey sort = SortKey::Ping;  // ping and names ascending, players descending by default
    bool reverse = false;          // the other way
    bool hide_full = false;        // only servers with a free slot
    bool hide_empty = false;
    int password = 0;              // 0 any, 1 without a password, 2 only with one
    std::string map;               // "" any, else this map only
    int location = 0;              // 0 any, 1 LAN only, 2 internet only (the Favourites and Recent tabs)
    bool answered_only = false;    // hide servers that did not answer (the LAN and internet tabs)
};
// indices into entries, stable for equal keys (then by name, then by address); refused entries never shown
std::vector<size_t> View(const std::vector<BrowserEntry>& entries, const ViewOptions& options);

// ---- favourites and recent servers, as stored in the PC settings ------------------------------------------------
// "ADDRESS KEY NAME" (KEY "-" when unknown; the name cleaned, for the list before the server answers)
struct SavedServer {
    std::string address, key, name;
};
std::string FormatSaved(const SavedServer& s);
std::optional<SavedServer> ParseSaved(const std::string& line);

}  // namespace net
