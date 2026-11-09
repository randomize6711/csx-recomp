// Native split-screen's heap ownership (docs/native_splitscreen.md, M2): which local player allocated each guest heap
// block, and the scrub of a new player's copy of the per-player regions. No game code: tested on its own by
// tests/native_split_test.py.
//
// A new player's regions start as player 1's right after the process's initialisation (the end of Host_Init), so they
// hold player 1's heap pointers (the HUD's lists, the particle manager, ...). The client's own initialisation frees
// what it finds there before allocating (gHUD.Init frees the list at gHUD + 0, 0x1BAE40 the object at 0x552CA8): run
// as is, the new player would free player 1's objects. So every dword of the copy that points into a block allocated
// during the process's initialisation is cleared first; the new player's initialisation then allocates its own. The
// ones it does not replace are shared objects (a registered cvar, a registry) and get their value back afterwards.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace heapown {

constexpr int kProcess = -1;  // allocated outside every player's turn: the process's initialisation, the server

struct Block {
    uint32_t size = 0;
    int owner = kProcess;
    uint64_t serial = 0;  // the allocation's number: a later block at the same address is a different object
};

class Blocks {
public:
    void Allocated(uint32_t va, uint32_t size, int owner) {
        if (!va) return;
        blocks_[va] = {size ? size : 1, owner, ++serial_};
    }
    // the block freed; its owner, or kNone when the address was not a known block's start
    static constexpr int kNone = -2;
    int Freed(uint32_t va) {
        auto it = blocks_.find(va);
        if (it == blocks_.end()) return kNone;
        int owner = it->second.owner;
        blocks_.erase(it);
        return owner;
    }
    // the block holding va (its start), or 0
    uint32_t Holding(uint32_t va) const {
        auto it = blocks_.upper_bound(va);
        if (it == blocks_.begin()) return 0;
        --it;
        return va - it->first < it->second.size ? it->first : 0;
    }
    const Block* At(uint32_t start) const {
        auto it = blocks_.find(start);
        return it == blocks_.end() ? nullptr : &it->second;
    }
    size_t Count() const { return blocks_.size(); }
    // every block's owner set to o (the snapshot: what the process allocated so far is the process's)
    void OwnAll(int o) {
        for (auto& [va, b] : blocks_) b.owner = o;
    }
    // a player gone: its blocks still allocated (count, bytes), handed to the process (they are not freed: the
    // objects' destructors are not run), so a later free of one is not taken for that player's
    std::pair<size_t, size_t> Orphan(int owner) {
        size_t n = 0, bytes = 0;
        for (auto& [va, b] : blocks_)
            if (b.owner == owner) {
                n++;
                bytes += b.size;
                b.owner = kProcess;
            }
        return {n, bytes};
    }

private:
    std::map<uint32_t, Block> blocks_;
    uint64_t serial_ = 0;
};

// a free by player `freeing` (kProcess: outside the players' turns) of a block owned by `owner`: a problem when a
// player frees another player's block, or a player other than the first frees a block from the process's
// initialisation (which the first player's state refers to). A free outside the turns is not attributed: the server
// and a map change free resources a player's turn loaded (a precached model).
inline bool FreeIsForeign(int owner, int freeing) {
    if (owner == Blocks::kNone) return false;
    if (owner >= 0 && freeing >= 0) return owner != freeing;
    if (owner == kProcess && freeing > 0) return true;
    return false;
}

struct Cleared {
    size_t offset;      // in the copy
    uint32_t value;     // the pointer that was there
    uint32_t block;     // the block it pointed into (its start)
    uint64_t serial;    // that block's allocation then
};

// every aligned dword of the copy pointing into a block live now, cleared; what was cleared. Run on the initial state
// when it is taken (the end of Host_Init), when its pointers are current: later, player 1's own code frees and reuses
// blocks those pointers name (its map load, its connect), and a pointer to a block since freed or reused could no
// longer be told from data (the M2 run of 2026-11-09: one pointer found at the add, 44 of player 1's blocks freed).
inline std::vector<Cleared> Scrub(std::vector<uint8_t>& copy, const Blocks& blocks) {
    std::vector<Cleared> out;
    for (size_t off = 0; off + 4 <= copy.size(); off += 4) {
        uint32_t v;
        std::memcpy(&v, copy.data() + off, 4);
        uint32_t start = v < 0x10000 ? 0 : blocks.Holding(v);
        if (!start) continue;
        out.push_back({off, v, start, blocks.At(start)->serial});
        std::memset(copy.data() + off, 0, 4);
    }
    return out;
}

// whether a cleared pointer may be given back to a new player: its initialisation left it at 0 (it made no object of
// its own there: a shared object) and the very allocation it named is still alive (not freed, not another block at
// the same address since)
inline bool Restorable(const Cleared& c, uint32_t now, const Blocks& blocks) {
    if (now) return false;
    const Block* b = blocks.At(c.block);
    return b && b->serial == c.serial;
}

// after the new player's initialisation (a copy): the restorable ones get their value back; the count restored
inline size_t RestoreUnreplaced(std::vector<uint8_t>& copy, const std::vector<Cleared>& cleared, const Blocks& blocks) {
    size_t n = 0;
    for (const Cleared& c : cleared) {
        uint32_t v;
        std::memcpy(&v, copy.data() + c.offset, 4);
        if (!Restorable(c, v, blocks)) continue;
        std::memcpy(copy.data() + c.offset, &c.value, 4);
        n++;
    }
    return n;
}

// the safety net: a free by a player other than the first (in its initialisation or its turns) of a block that is not
// its own is not done (logged): another player's object, or one of the process's initialisation that player 1's state
// names, stays alive; at worst a leak, never a block freed under another player
inline bool SkipFree(int owner, int freeing) { return freeing > 0 && FreeIsForeign(owner, freeing); }

}  // namespace heapown
