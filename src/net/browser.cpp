// The server browser's discovery (browser.h; docs/pc_multiplayer.md "N3").
#include "net/browser.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace net {
namespace {

uint64_t RandomNonce() {
    static const bool ok = sodium_init() >= 0;
    uint64_t n = 0;
    if (ok) randombytes_buf(&n, sizeof(n));
    return n | 1;  // never 0 (0 stands for "none")
}

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Browser::Browser(std::unique_ptr<DatagramSocket> socket, BrowserConfig config, Clock clock) :
    socket_(std::move(socket)), config_(std::move(config)), clock_(std::move(clock)) {}

const BrowserEntry* Browser::Find(const Address& a) const {
    auto it = index_.find(a.Key());
    return it == index_.end() ? nullptr : &entries_[it->second];
}

BrowserEntry* Browser::Entry(const Address& a, bool create) {
    auto it = index_.find(a.Key());
    if (it != index_.end()) return &entries_[it->second];
    if (!create) return nullptr;
    if (entries_.size() >= config_.max_servers) {
        stats_.dropped_full++;
        return nullptr;
    }
    index_[a.Key()] = entries_.size();
    entries_.emplace_back();
    entries_.back().address = a;
    Changed();
    return &entries_.back();
}

void Browser::Erase(const Address& a) {
    auto it = index_.find(a.Key());
    if (it == index_.end()) return;
    size_t i = it->second, last = entries_.size() - 1;
    index_.erase(it);
    queries_.erase(a.Key());
    if (i != last) {
        entries_[i] = std::move(entries_[last]);
        index_[entries_[i].address.Key()] = i;
    }
    entries_.pop_back();
    Changed();
}

void Browser::Queue(BrowserEntry& e) {
    Query& q = queries_[e.address.Key()];
    q.tries = 0;
    if (q.queued || q.outstanding) return;
    q.queued = true;
    queue_.push_back(e.address.Key());
    if (e.state != BrowserEntry::State::Answered) e.state = BrowserEntry::State::Waiting;
}

void Browser::Add(const Address& a, uint8_t source, const std::string& expected_key) {
    BrowserEntry* e = Entry(a, true);
    if (!e) return;
    e->sources |= source;
    if (!expected_key.empty() && expected_key != "-") e->expected_key = Lower(expected_key);
    Changed();
    Queue(*e);
}

void Browser::Remove(const Address& a, uint8_t source) {
    BrowserEntry* e = Entry(a, false);
    if (!e) return;
    e->sources &= uint8_t(~source);
    if (source == kFavorite) e->expected_key.clear();
    if (!e->sources) Erase(a);
    else Changed();
}

void Browser::Refresh(const Address& a) {
    if (BrowserEntry* e = Entry(a, false)) Queue(*e);
}

void Browser::RefreshSource(uint8_t source) {
    for (auto& e : entries_)
        if (e.sources & source) Queue(e);
}

void Browser::RefreshLan() {
    double now = clock_();
    lan_nonce_ = RandomNonce();
    lan_sent_ = now;
    next_lan_ = now + config_.lan_interval;
    std::vector<uint8_t> q = EncodeQuery(QueryKind::Info, 0, lan_nonce_);
    for (const Address& b : config_.broadcast)
        for (uint16_t port : config_.lan_ports) {
            Address to = b;
            to.port = port;
            socket_->Send(to, q.data(), q.size());
            stats_.broadcasts++;
            stats_.query_bytes += q.size();
        }
}

void Browser::RefreshInternet() {
    // the list starts again from the masters: servers only they listed go until they list them again
    for (size_t i = entries_.size(); i-- > 0;)
        if (entries_[i].sources & kInternet) Remove(entries_[i].address, kInternet);
    masters_.clear();
    masters_answered_ = 0;
    for (const Address& m : config_.masters) {
        MasterFetch& f = masters_[m.Key()];
        f.done = false;
        f.page = 0;
        SendListPage(m, f);
    }
    Changed();
}

void Browser::SendListPage(const Address& master, MasterFetch& f) {
    f.nonce = RandomNonce();
    f.sent = clock_();
    f.tries++;
    std::vector<uint8_t> p = EncodeListRequest(f.nonce, f.page);
    socket_->Send(master, p.data(), p.size());
    stats_.list_requests++;
    stats_.query_bytes += p.size();
}

void Browser::QueryPlayers(const Address& a) {
    BrowserEntry* e = Entry(a, false);
    if (!e || !e->have_info) return;
    Query& q = queries_[a.Key()];
    q.players_page = 0;
    q.players_pages = 0;
    q.pages.clear();
    q.nonce = q.nonce ? q.nonce : RandomNonce();
    std::vector<uint8_t> p = EncodeQuery(QueryKind::Players, 0, q.nonce);
    socket_->Send(a, p.data(), p.size());
    stats_.queries++;
    stats_.query_bytes += p.size();
}

void Browser::SendQuery(BrowserEntry& e, Query& q) {
    q.nonce = RandomNonce();
    q.sent = clock_();
    q.tries++;
    q.outstanding = true;
    std::vector<uint8_t> p = EncodeQuery(QueryKind::Info, 0, q.nonce);
    socket_->Send(e.address, p.data(), p.size());
    stats_.queries++;
    stats_.query_bytes += p.size();
}

void Browser::Accept(BrowserEntry& e, const ServerInfo& info, double rtt) {
    std::string key = Identity::Fingerprint(info.key);
    if (!e.expected_key.empty() && e.expected_key != key) {
        stats_.wrong_key++;
        if (e.sources & (kFavorite | kRecent | kDirect)) {
            // a server known to the player answers with another identity: shown, with the warning, never as trusted
            e.problem = "identity changed";
        } else {
            // a listing whose server proves another key than the master named: not that server
            e.state = BrowserEntry::State::Refused;
            e.problem = "its identity is not the one listed";
            Changed();
            return;
        }
    } else {
        e.problem.clear();
    }
    e.info = info;
    e.have_info = true;
    e.key = key;
    e.ping = int(std::max(0.0, rtt) * 1000 + 0.5);
    e.last_answer = clock_();
    e.state = BrowserEntry::State::Answered;
    Changed();
}

void Browser::HandleInfo(const Address& from, const uint8_t* p, size_t n) {
    if (n > kPrefix && QueryKind(p[kPrefix]) == QueryKind::Players) return HandlePlayers(from, p, n);
    stats_.answer_bytes += n;
    ServerInfo info;
    std::string error;
    auto it = queries_.find(from.Key());
    if (it != queries_.end() && it->second.outstanding && DecodeInfo(p, n, it->second.nonce, info, error)) {
        Query& q = it->second;
        q.outstanding = false;
        stats_.answers++;
        if (BrowserEntry* e = Entry(from, false)) Accept(*e, info, clock_() - q.sent);
        return;
    }
    // else the answer to the LAN broadcast (from any server, a known one too)
    if (!lan_nonce_ || !DecodeInfo(p, n, lan_nonce_, info, error)) {
        if (it != queries_.end() && it->second.outstanding) stats_.malformed++;
        else stats_.unexpected++;
        return;
    }
    stats_.answers++;
    // the same server answering at another address (two broadcast routes to one machine) is listed once
    std::string key = Identity::Fingerprint(info.key);
    for (const auto& e : entries_)
        if ((e.sources & kLan) && !(e.address == from) && e.key == key && e.last_answer >= lan_sent_) return;
    BrowserEntry* e = Entry(from, true);
    if (!e) return;
    e->sources |= kLan;
    Accept(*e, info, clock_() - lan_sent_);
}

void Browser::HandlePlayers(const Address& from, const uint8_t* p, size_t n) {
    stats_.answer_bytes += n;
    auto it = queries_.find(from.Key());
    BrowserEntry* e = Entry(from, false);
    if (it == queries_.end() || !e || !e->have_info) {
        stats_.unexpected++;
        return;
    }
    Query& q = it->second;
    uint8_t page = 0, pages = 0;
    std::vector<PlayerInfo> list;
    std::string error;
    if (!DecodePlayers(p, n, q.nonce, e->info.key, page, pages, list, error) || page != q.players_page) {
        stats_.malformed++;
        return;
    }
    if (page == 0) {
        q.players_pages = pages;
        q.pages.assign(pages, {});
    }
    if (pages != q.players_pages || page >= q.pages.size()) return;
    q.pages[page] = std::move(list);
    if (page + 1 < pages) {
        q.players_page = uint8_t(page + 1);
        std::vector<uint8_t> next = EncodeQuery(QueryKind::Players, q.players_page, q.nonce);
        socket_->Send(from, next.data(), next.size());
        stats_.queries++;
        stats_.query_bytes += next.size();
        return;
    }
    e->players.clear();
    for (auto& pg : q.pages) e->players.insert(e->players.end(), pg.begin(), pg.end());
    e->players_complete = true;
    q.pages.clear();
    Changed();
}

void Browser::HandleList(const Address& from, const uint8_t* p, size_t n) {
    auto it = masters_.find(from.Key());
    if (it == masters_.end() || it->second.done) {
        stats_.unexpected++;
        return;
    }
    MasterFetch& f = it->second;
    uint32_t page = 0, pages = 0, total = 0;
    std::vector<MasterEntry> list;
    std::string error;
    if (!DecodeList(p, n, f.nonce, page, pages, total, list, error) || page != f.page) {
        stats_.malformed++;
        return;
    }
    stats_.list_pages++;
    for (const MasterEntry& m : list) {
        BrowserEntry* e = Entry(m.address, true);
        if (!e) break;
        e->sources |= kInternet;
        if (e->expected_key.empty()) e->expected_key = Identity::Fingerprint(m.key);
        Queue(*e);
    }
    f.page++;
    f.tries = 0;
    if (f.page < std::min(pages, config_.max_master_pages)) {
        SendListPage(from, f);
    } else {
        f.done = true;
        masters_answered_++;
        Changed();
    }
}

void Browser::Pump() {
    uint8_t buf[2048];
    Address from;
    for (int i = 0; i < 4096; i++) {
        int n = socket_->Receive(from, buf, sizeof(buf));
        if (n < 0) break;
        if (size_t(n) < kPrefix || size_t(n) > kMaxDatagram) {
            stats_.malformed++;
            continue;
        }
        wire::Reader r{buf, size_t(n)};
        if (r.U32() != kMagic || r.U8() != kVersion) {
            stats_.malformed++;
            continue;
        }
        Type t = Type(r.U8());
        if (t == Type::Info) HandleInfo(from, buf, size_t(n));
        else if (t == Type::List) HandleList(from, buf, size_t(n));
        else stats_.unexpected++;
    }
    double now = clock_();
    if (watch_lan_ && now >= next_lan_) RefreshLan();
    // LAN servers that stopped answering the broadcasts
    for (size_t i = entries_.size(); i-- > 0;) {
        BrowserEntry& e = entries_[i];
        if ((e.sources & kLan) && lan_sent_ >= 0 && now - e.last_answer > config_.lan_expiry &&
            now - lan_sent_ < config_.lan_expiry)
            Remove(e.address, kLan);
    }
    // unanswered queries: again, then given up
    for (auto& [key, q] : queries_) {
        if (!q.outstanding || now - q.sent < config_.timeout) continue;
        q.outstanding = false;
        BrowserEntry* e = nullptr;
        if (auto it = index_.find(key); it != index_.end()) e = &entries_[it->second];
        if (!e) continue;
        if (q.tries < config_.tries) {
            q.queued = true;
            queue_.push_back(key);
        } else if (e->state != BrowserEntry::State::NoAnswer) {
            e->state = BrowserEntry::State::NoAnswer;
            e->ping = -1;
            Changed();
        }
    }
    std::vector<Address> gone;
    for (auto& e : entries_)  // internet servers that never answered are not kept
        if (e.sources == kInternet && e.state == BrowserEntry::State::NoAnswer) gone.push_back(e.address);
    for (const Address& a : gone) Erase(a);
    // queued queries, paced
    double burst = std::max(1.0, config_.send_rate / 20);
    tokens_ = tokens_at_ < 0 ? burst : std::min(burst, tokens_ + (now - tokens_at_) * config_.send_rate);
    tokens_at_ = now;
    size_t outstanding = 0;
    for (auto& [key, q] : queries_) outstanding += q.outstanding;
    while (queue_head_ < queue_.size() && tokens_ >= 1 && outstanding < config_.max_outstanding) {
        uint64_t key = queue_[queue_head_++];
        auto qi = queries_.find(key);
        auto ei = index_.find(key);
        if (qi == queries_.end() || ei == index_.end() || !qi->second.queued) continue;
        qi->second.queued = false;
        SendQuery(entries_[ei->second], qi->second);
        tokens_ -= 1;
        outstanding++;
    }
    if (queue_head_ >= queue_.size()) {
        queue_.clear();
        queue_head_ = 0;
    }
    // master list pages unanswered
    for (auto& [key, f] : masters_) {
        if (f.done || now - f.sent < config_.master_timeout) continue;
        Address m;
        m.ip = {uint8_t(key >> 40), uint8_t(key >> 32), uint8_t(key >> 24), uint8_t(key >> 16)};
        m.port = uint16_t(key);
        if (f.tries >= 3) {
            f.done = true;
            masters_answered_++;  // gave up: the list is what arrived
            Changed();
        } else {
            SendListPage(m, f);
        }
    }
}

void Browser::Wait(double seconds) {
    socket_->Wait(seconds);
}

bool Browser::Busy() const {
    if (queue_head_ < queue_.size()) return true;
    for (auto& [key, q] : queries_)
        if (q.outstanding) return true;
    return Listing();
}

bool Browser::Listing() const {
    for (auto& [key, f] : masters_)
        if (!f.done) return true;
    return false;
}

// ---- the view ----------------------------------------------------------------------------------------------------

std::vector<size_t> View(const std::vector<BrowserEntry>& entries, const ViewOptions& o) {
    std::vector<size_t> v;
    std::string map = Lower(o.map);
    for (size_t i = 0; i < entries.size(); i++) {
        const BrowserEntry& e = entries[i];
        if (!(e.sources & o.sources) || e.state == BrowserEntry::State::Refused) continue;
        if (o.answered_only && !e.have_info) continue;
        if (o.location == 1 && !(e.sources & kLan) && !e.address.Private()) continue;
        if (o.location == 2 && ((e.sources & kLan) || e.address.Private())) continue;
        if (e.have_info) {
            if (o.hide_full && e.info.Full()) continue;
            if (o.hide_empty && e.info.Total() == 0) continue;
            if (o.password == 1 && e.info.password) continue;
            if (o.password == 2 && !e.info.password) continue;
            if (!map.empty() && Lower(e.info.map) != map) continue;
        } else if (o.hide_full || o.hide_empty || o.password == 2 || !map.empty()) {
            continue;  // nothing known to match the filter by
        }
        v.push_back(i);
    }
    auto name = [&](size_t i) { return Lower(entries[i].have_info ? entries[i].info.name : ""); };
    auto less = [&](size_t a, size_t b) {
        const BrowserEntry &x = entries[a], &y = entries[b];
        int c = 0;  // < 0: a first in the key's natural order
        switch (o.sort) {
        case SortKey::Ping: {
            // no answer last whichever way
            int px = x.ping < 0 ? 1 << 30 : x.ping, py = y.ping < 0 ? 1 << 30 : y.ping;
            c = (px > py) - (px < py);
            break;
        }
        case SortKey::Players: {
            int tx = x.have_info ? x.info.Total() : -1, ty = y.have_info ? y.info.Total() : -1;
            c = (ty > tx) - (ty < tx);  // most first
            break;
        }
        case SortKey::Name: c = name(a).compare(name(b)); break;
        case SortKey::Map:
            c = Lower(x.have_info ? x.info.map : "~").compare(Lower(y.have_info ? y.info.map : "~"));
            break;
        }
        if (o.reverse) c = -c;
        if (c) return c < 0;
        int n = name(a).compare(name(b));
        if (n) return n < 0;
        return x.address.Key() < y.address.Key();
    };
    std::stable_sort(v.begin(), v.end(), less);
    return v;
}

// ---- saved servers -----------------------------------------------------------------------------------------------

std::string FormatSaved(const SavedServer& s) {
    std::string name = wire::CleanText(s.name, kMaxServerName);
    return s.address + " " + (s.key.empty() ? "-" : s.key) + (name.empty() ? "" : " " + name);
}

std::optional<SavedServer> ParseSaved(const std::string& line) {
    SavedServer s;
    size_t a = line.find(' ');
    s.address = line.substr(0, a);
    if (s.address.empty() || s.address.size() > 260) return std::nullopt;
    for (char c : s.address)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != ':') return std::nullopt;
    if (a != std::string::npos) {
        size_t b = line.find(' ', a + 1);
        s.key = Lower(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
        if (s.key == "-") s.key.clear();
        else if (!Identity::ParseFingerprint(s.key)) return std::nullopt;
        if (b != std::string::npos) s.name = wire::CleanText(line.substr(b + 1), kMaxServerName);
    }
    return s;
}

}  // namespace net
