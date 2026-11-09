// Native split-screen's heap ownership (docs/native_splitscreen.md 13, the M2 revision): every guest heap block's
// owner (the player whose turn or initialisation allocated it, else the process) and its kind (from the code that
// allocated it: a player's object, a shared registry, shared scratch). No game code: tested on its own by
// tests/native_split_test.py.
//
// Players are no longer made from a copy of player 1's state (the snapshot-and-scrub of the first M2 runs, retired:
// docs 12): a new player starts from the boot image, which holds no heap pointer, and its own steps make its objects.
// This file decides who may free or destroy what, and counts the heap pointers of a state (to verify a new player's).
#pragma once

#include <algorithm>
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
    // a block made through an allocation helper: its site and kind are the helper's caller's (false: not a block start)
    bool Reattribute(uint32_t va, uint32_t site, Kind kind) {
        auto it = blocks_.find(va);
        if (it == blocks_.end()) return false;
        it->second.site = site;
        it->second.kind = kind;
        return true;
    }
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

// a value's kind, as the start-up check weighs it (docs/native_splitscreen.md 14.6): a pointer is told from a number by
// what it names, never by its range alone (run 5's false links were byte counts in the image's range)
enum class ValueKind {
    Number,        // nothing it could name: a count, a size, a float, a flag
    Function,      // a verified function entry (the recompiler's dispatch table)
    ReadOnly,      // an address in .rdata the code takes (a string, a vtable, a constant table)
    StaticObject,  // an address in .data the code takes (a list head, a static object)
    Region,        // inside the player's own regions (a list head pointing at itself)
    Heap,          // inside a known heap block (the object check's)
};
inline const char* Name(ValueKind k) {
    switch (k) {
    case ValueKind::Number: return "number";
    case ValueKind::Function: return "function";
    case ValueKind::ReadOnly: return "read-only data";
    case ValueKind::StaticObject: return "static object";
    case ValueKind::Region: return "own region";
    case ValueKind::Heap: return "heap block";
    }
    return "?";
}
inline bool IsLink(ValueKind k) {
    return k == ValueKind::Function || k == ValueKind::ReadOnly || k == ValueKind::StaticObject ||
           k == ValueKind::Region;
}

// what the classification knows of the image
struct ValueMap {
    bool (*function)(uint32_t) = nullptr;  // a verified function entry
    const uint32_t* taken = nullptr;       // the static addresses the code takes, sorted
    size_t taken_count = 0;
    uint32_t rdata_lo = 0x3210C0, rdata_hi = 0x36C890;
    const std::vector<Span>* regions = nullptr;  // the player's regions
};
inline ValueKind Classify(uint32_t v, const ValueMap& m, const Blocks& blocks) {
    if (v < 0x10000) return ValueKind::Number;
    if (blocks.Holding(v)) return ValueKind::Heap;
    if (m.function && m.function(v)) return ValueKind::Function;
    if (m.regions)
        for (const Span& sp : *m.regions)
            if (v >= sp.va && v - sp.va < sp.size) return ValueKind::Region;
    if (m.taken) {
        const uint32_t* end = m.taken + m.taken_count;
        const uint32_t* it = std::lower_bound(m.taken, end, v);
        if (it != end && *it == v) return v >= m.rdata_lo && v < m.rdata_hi ? ValueKind::ReadOnly : ValueKind::StaticObject;
    }
    return ValueKind::Number;
}

// a start-up value a new player lacks (docs/native_splitscreen.md 13.8, 14.6): a word of the regions that the process's
// start-up changed for player 1 (the reference differs from the boot image) and that the new player's own steps left
// at its boot value. The title's code set it once, for the process: either it is shared state wrongly in a player's
// regions, or an initialisation step the new player lacks. A link (Classify: a function, read-only data, a static
// object the code takes, the player's own regions) is a failure; a number is reported.
struct StartupGap {
    uint32_t va;
    uint32_t reference, boot;
    bool link;
    ValueKind kind;
};
constexpr uint32_t kImageLo = 0x10000, kImageHi = 0x7D6F20;  // the XBE's code and data
inline bool IsImageAddress(uint32_t v) { return v >= kImageLo && v < kImageHi; }

// boot and reference are copies (the regions end to end); mine(va) reads the new player's word at a guest address;
// heap pointers are left to the object check (HeapPointers)
template <typename Read>
std::vector<StartupGap> StartupGaps(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& reference,
                                    const std::vector<Span>& spans, const Blocks& blocks, const ValueMap& map,
                                    Read mine) {
    std::vector<StartupGap> out;
    for (const Span& sp : spans) {
        uint32_t first = (sp.va + 3) & ~3u;
        for (uint32_t va = first; va + 4 <= sp.va + sp.size; va += 4) {
            size_t off = sp.offset + (va - sp.va);
            if (off + 4 > boot.size() || off + 4 > reference.size()) break;
            uint32_t b, r;
            std::memcpy(&b, boot.data() + off, 4);
            std::memcpy(&r, reference.data() + off, 4);
            if (b == r || uint32_t(mine(va)) != b) continue;
            ValueKind k = Classify(r, map, blocks);
            if (k == ValueKind::Heap) continue;  // a heap object: the object check's
            out.push_back({va, r, b, IsLink(k), k});
        }
    }
    return out;
}

// the protected start-up state (client_context_protected.inc: what the process's start-up set once and no player's
// code writes; the static cvars' list links): its bytes before a new player's steps, and the runs they changed
struct Range {
    uint32_t va, size;
};
template <typename ReadByte>
std::vector<uint8_t> Snapshot(const std::vector<Range>& ranges, ReadByte read) {
    std::vector<uint8_t> out;
    for (const Range& r : ranges)
        for (uint32_t k = 0; k < r.size; k++) out.push_back(uint8_t(read(r.va + k)));
    return out;
}
// the changed runs (guest address, length) of the ranges since the snapshot `before`; a run never crosses a range
template <typename ReadByte>
std::vector<Range> Changed(const std::vector<Range>& ranges, const std::vector<uint8_t>& before, ReadByte read) {
    std::vector<Range> out;
    size_t at = 0;
    for (const Range& r : ranges) {
        bool open = false;
        for (uint32_t k = 0; k < r.size; k++, at++) {
            bool diff = at >= before.size() || uint8_t(read(r.va + k)) != before[at];
            if (diff && open) out.back().size++;
            else if (diff) out.push_back({r.va + k, 1});
            open = diff;
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
