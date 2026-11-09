// Native split-screen's heap ownership (docs/native_splitscreen.md 13, the M2 revision): every guest heap block's
// owner (the player whose turn or initialisation allocated it, else the process) and its kind (from the code that
// allocated it: a player's object, a shared registry, shared scratch). No game code: tested on its own by
// tests/native_split_test.py.
//
// Players are no longer made from a copy of player 1's state (the snapshot-and-scrub of the first M2 runs, retired:
// docs 12): a new player starts from the boot image, which holds no heap pointer, and its own steps make its objects.
// This file decides who may free or destroy what, and counts the heap pointers of a state (to verify a new player's).
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace heapown {

constexpr int kProcess = -1;  // allocated outside every player's turn: the process's initialisation, the server

// what kind of object a block is, from the code that allocated it
enum class Kind {
    Owned,           // a player's object (or the process's): freed or destroyed by its owner only
    SharedRegistry,  // an entry of a registry every player uses (a cvar, a command, a user message, a delta
                     // description): built once, never a player's
    SharedScratch,   // shared working memory the title's own code frees and remakes whoever runs next (the command
                     // tokenizer's arguments, cvar value strings, the transparent-entity list, cache entries)
};
inline const char* Name(Kind k) {
    switch (k) {
    case Kind::Owned: return "owned";
    case Kind::SharedRegistry: return "shared registry";
    case Kind::SharedScratch: return "shared scratch";
    }
    return "?";
}

// the kinds by allocating code: a site in [lo, hi) is of `kind`
struct SiteRule {
    uint32_t lo, hi;
    Kind kind;
    const char* what;
};
inline Kind KindOfSite(uint32_t site, const SiteRule* rules, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (site >= rules[i].lo && site < rules[i].hi) return rules[i].kind;
    return Kind::Owned;
}

struct Block {
    uint32_t size = 0;
    int owner = kProcess;
    uint64_t serial = 0;  // the allocation's number: a later block at the same address is a different object
    uint32_t site = 0;    // the code that asked for it (the caller's return address)
    Kind kind = Kind::Owned;
};

class Blocks {
public:
    void Allocated(uint32_t va, uint32_t size, int owner, uint32_t site = 0, Kind kind = Kind::Owned) {
        if (!va) return;
        blocks_[va] = {size ? size : 1, owner, ++serial_, site, kind};
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
    template <typename F>
    void ForEach(F f) const {
        for (const auto& [va, b] : blocks_) f(va, b);
    }
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

// a heap pointer found in a state: where (offset in the copy, guest address), the block it names
struct Pointer {
    size_t offset;
    uint32_t va;
    uint32_t value;
    uint32_t block;
    uint64_t serial;
};

// a region of the copy: where its bytes are in the copy, and in guest memory
struct Span {
    size_t offset;
    uint32_t va;
    uint32_t size;
};

// every heap pointer of a state (the regions laid end to end), read at each region's own guest alignment (regions
// start at any address: 0x78A943, 0x742FCF, ...) and never across a region's end. Changes nothing: it verifies.
inline std::vector<Pointer> HeapPointers(const std::vector<uint8_t>& copy, const std::vector<Span>& spans,
                                         const Blocks& blocks) {
    std::vector<Pointer> out;
    for (const Span& sp : spans) {
        uint32_t first = (sp.va + 3) & ~3u;
        for (uint32_t va = first; va + 4 <= sp.va + sp.size; va += 4) {
            size_t off = sp.offset + (va - sp.va);
            if (off + 4 > copy.size()) break;
            uint32_t v;
            std::memcpy(&v, copy.data() + off, 4);
            uint32_t start = v < 0x10000 ? 0 : blocks.Holding(v);
            if (start) out.push_back({off, va, v, start, blocks.At(start)->serial});
        }
    }
    return out;
}

// one span of a whole copy whose guest address is its offset (tests)
inline std::vector<Span> Flat(size_t size, uint32_t va = 0) { return {{0, va, uint32_t(size)}}; }

// the safety net: a free by a player other than the first (in its initialisation or its turns) of a block that is not
// its own is not done (logged): another player's object, or one of the process's initialisation that player 1's state
// names, stays alive; at worst a leak, never a block freed under another player
inline bool SkipFree(int owner, int freeing) { return freeing > 0 && FreeIsForeign(owner, freeing); }

// a block's free or destruction, its kind weighed: the shared kinds are anyone's to free (the title's own code
// manages them that way); an owned block follows FreeIsForeign / MayDestroy
inline bool FreeIsForeign(const Block& b, int freeing) {
    return b.kind == Kind::Owned && FreeIsForeign(b.owner, freeing);
}

// whether the object at `owner_of_object` may be destroyed by `acting` (a player in its initialisation or turn, or
// kProcess): the check made before a destructor runs, not only before the free (a destructor that has run has already
// changed the object, whatever happens to its memory)
inline bool MayDestroy(int owner_of_object, int acting) {
    if (acting <= 0) return true;                         // player 1 and the process act as the title
    return owner_of_object == acting;                     // another player: only its own objects
}
inline bool MayDestroy(const Block& b, int acting) { return b.kind != Kind::Owned || MayDestroy(b.owner, acting); }

}  // namespace heapown
