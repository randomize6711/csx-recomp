// Native split-screen's in-process link (docs/native_splitscreen.md, M2): the engine's loopback with one channel per
// local player. The title's loopback has two sides (client, server: loopbacks[2] at 0x440C00, four messages each, the
// newest kept); here each local player k has its own inbox from the server, and the server's inbox records which
// player sent each packet (the engine sees it as the loopback address with port k). No game code: tested on its own by
// tests/native_split_test.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace loopnet {

constexpr int kMaxPlayers = 4;
constexpr size_t kPerQueue = 4;  // the title's loopback keeps the newest four messages a side

class Channels {
public:
    // everything dropped (the server shut down, a map change)
    void Clear() {
        server_.clear();
        for (auto& q : clients_) q.clear();
    }

    // player k's client sends to the server; false for no such player
    bool ToServer(int k, const uint8_t* data, size_t n) {
        if (k < 0 || k >= kMaxPlayers) return false;
        // the newest kPerQueue a player, as the title's ring: a player's oldest dropped when it is over
        size_t mine = 0;
        for (const Packet& p : server_) mine += p.from == k;
        if (mine >= kPerQueue)
            for (auto it = server_.begin(); it != server_.end(); ++it)
                if (it->from == k) {
                    server_.erase(it);
                    dropped_++;
                    break;
                }
        server_.push_back({k, std::vector<uint8_t>(data, data + n)});
        return true;
    }

    // the server sends to player k's client; false for no such player
    bool ToClient(int k, const uint8_t* data, size_t n) {
        if (k < 0 || k >= kMaxPlayers) return false;
        auto& q = clients_[size_t(k)];
        if (q.size() >= kPerQueue) {
            q.pop_front();
            dropped_++;
        }
        q.push_back({-1, std::vector<uint8_t>(data, data + n)});
        return true;
    }

    // the server's next packet (in the order sent) and its sender
    bool ServerGet(int& from, std::vector<uint8_t>& out) {
        if (server_.empty()) return false;
        from = server_.front().from;
        out.swap(server_.front().data);
        server_.pop_front();
        return true;
    }

    // player k's next packet from the server
    bool ClientGet(int k, std::vector<uint8_t>& out) {
        if (k < 0 || k >= kMaxPlayers || clients_[size_t(k)].empty()) return false;
        auto& q = clients_[size_t(k)];
        out.swap(q.front().data);
        q.pop_front();
        return true;
    }

    // a player gone: its packets both ways dropped
    void Forget(int k) {
        if (k < 0 || k >= kMaxPlayers) return;
        clients_[size_t(k)].clear();
        for (auto it = server_.begin(); it != server_.end();)
            it = it->from == k ? server_.erase(it) : it + 1;
    }

    size_t ServerPending() const { return server_.size(); }
    size_t ClientPending(int k) const { return k >= 0 && k < kMaxPlayers ? clients_[size_t(k)].size() : 0; }
    uint64_t Dropped() const { return dropped_; }

private:
    struct Packet {
        int from;
        std::vector<uint8_t> data;
    };
    std::deque<Packet> server_;
    std::deque<Packet> clients_[kMaxPlayers];
    uint64_t dropped_ = 0;
};

// two netadr_t, as NET_CompareAdr / SV_ReadPackets weigh them after their own checks: the title takes any two loopback
// addresses as one; with a player in the port, two loopback addresses are one only when their ports agree. Without
// native split-screen every loopback port is 0, so this agrees with the title everywhere.
inline bool AddressesMatch(uint32_t type, uint16_t port_a, uint16_t port_b) {
    constexpr uint32_t kLoopback = 1;
    return type != kLoopback || port_a == port_b;
}

// the loopback address's port carries the player: player 1 (index 0) is port 0, the address the title always used
inline uint16_t PortOf(int k) { return uint16_t(k); }
inline int PlayerOf(uint16_t port) { return port < kMaxPlayers ? int(port) : -1; }

}  // namespace loopnet
