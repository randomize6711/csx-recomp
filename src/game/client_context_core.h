// Native split-screen client contexts, the part without the game (docs/native_splitscreen.md, M1): a list of guest
// memory regions that hold one local player's client state, and the switch between players' copies of them.
//
// The game's client code reaches its state at fixed addresses, so several local players take turns at the same
// addresses: before a player's part of the frame its copy of every region is copied in, after it copied out. The
// active player's state is always the live memory itself; the others' are held in their saved copies. With one
// player nothing is ever copied.
//
// Tested on its own by tests/client_context_test.py (a synthetic memory, no game).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ctx {

struct Region {
    uint32_t start = 0;  // guest virtual address
    uint32_t size = 0;   // bytes
    const char* what = "";
};

// regions sorted by start, none empty, none overlapping: "" when so, else what is wrong
inline std::string CheckRegions(const std::vector<Region>& regions) {
    for (size_t i = 0; i < regions.size(); i++) {
        if (!regions[i].size) return std::string("empty region: ") + regions[i].what;
        if (uint64_t(regions[i].start) + regions[i].size > 0x100000000ull)
            return std::string("region past 4 GB: ") + regions[i].what;
        if (i && regions[i].start < regions[i - 1].start + regions[i - 1].size)
            return std::string("overlapping or unsorted: ") + regions[i - 1].what + " / " + regions[i].what;
    }
    return "";
}

// the players' contexts over one memory. Map(va) gives the host address of a guest address; every region must be
// contiguous there (the guest memory is one mapping).
template <typename MapFn>
class Switcher {
public:
    Switcher(std::vector<Region> regions, MapFn map) : regions_(std::move(regions)), map_(map) {
        for (const Region& r : regions_) total_ += r.size;
        saved_.emplace_back();  // context 0: the live memory (its copy is filled when another becomes active)
    }

    const std::vector<Region>& Regions() const { return regions_; }
    size_t Bytes() const { return total_; }  // per context
    int Count() const { return int(saved_.size()); }
    int Active() const { return active_; }

    // the regions' contents now, kept as the state a new context starts from (taken at boot after the static
    // constructors and before the engine's first client initialisation: client_context.cpp)
    void CaptureInitial() { initial_ = Copy(); }
    bool HasInitial() const { return !initial_.empty(); }
    std::vector<uint8_t>& Initial() { return initial_; }  // the state new contexts start from (scrubbed in place)

    // a new context; its index. Its state is the initial one when captured (its own initialisation then allocates its
    // own objects, never freeing the first player's), else a copy of the active one's live memory
    int Add() {
        saved_.push_back(HasInitial() ? initial_ : Copy());
        return Count() - 1;
    }

    // the last context removed (it must not be active)
    bool RemoveLast() {
        if (Count() <= 1 || active_ == Count() - 1) return false;
        saved_.pop_back();
        return true;
    }

    // context k removed (not 0, not the active one); the contexts after it move down one index
    bool Remove(int k) {
        if (k <= 0 || k >= Count() || k == active_) return false;
        saved_.erase(saved_.begin() + k);
        if (active_ > k) active_--;
        return true;
    }

    // the offset in a copy of a guest address, -1 when it is in no region
    long OffsetOf(uint32_t va) const {
        size_t at = 0;
        for (const Region& r : regions_) {
            if (va >= r.start && va - r.start < r.size) return long(at + (va - r.start));
            at += r.size;
        }
        return -1;
    }

    // player k's state made the live memory: the active one's saved, k's copied in
    bool Switch(int k) {
        if (k < 0 || k >= Count()) return false;
        if (k == active_) return true;
        std::vector<uint8_t>& out = saved_[size_t(active_)];
        out.resize(total_);
        const std::vector<uint8_t>& in = saved_[size_t(k)];
        size_t at = 0;
        for (const Region& r : regions_) {
            uint8_t* live = map_(r.start);
            std::memcpy(out.data() + at, live, r.size);
            std::memcpy(live, in.data() + at, r.size);
            at += r.size;
        }
        active_ = k;
        switches_++;
        return true;
    }

    uint64_t Switches() const { return switches_; }

    // context k's saved copy (the regions end to end); the active one's is stale until it is switched out
    std::vector<uint8_t>& Saved(int k) { return saved_[size_t(k)]; }
    // the guest address of a byte of a copy, 0 past the end
    uint32_t AddressOf(size_t offset) const {
        for (const Region& r : regions_) {
            if (offset < r.size) return r.start + uint32_t(offset);
            offset -= r.size;
        }
        return 0;
    }

private:
    std::vector<uint8_t> Copy() const {
        std::vector<uint8_t> copy(total_);
        size_t at = 0;
        for (const Region& r : regions_) {
            std::memcpy(copy.data() + at, map_(r.start), r.size);
            at += r.size;
        }
        return copy;
    }

    std::vector<Region> regions_;
    MapFn map_;
    size_t total_ = 0;
    std::vector<std::vector<uint8_t>> saved_;  // per context; the active one's is stale until it is switched out
    std::vector<uint8_t> initial_;             // the state a new context starts from (empty: a copy of the active one)
    int active_ = 0;
    uint64_t switches_ = 0;
};

// the frame's loops over the contexts (_Host_Frame: each player's input before the server frame, then each player's
// client half after it): after a player's turn, the next player's index, or -1 when every player has had its turn
// (the caller then makes player 0 active again for the rest of the frame)
inline int NextTurn(int active, int count) { return active + 1 < count ? active + 1 : -1; }

}  // namespace ctx
