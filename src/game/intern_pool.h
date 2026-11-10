// The process's string-intern pool (docs/native_splitscreen.md 15.8): its layout and the invariants a new player's
// steps must keep. No game code: tested on its own by tests/native_split_test.py.
//
// The pool at 0x562118 is a CUtlSymbolTable: a red-black tree of u16 node indices (12-byte nodes: left, right, parent,
// colour as u16, then the string's u32 offset) and a vector of the strings' bytes. The file system interns every path
// it opens (0x115D10, under its lock 0x52784C): one namespace of indices for the process, every player's lookups
// included. A new string is appended (its node the next one, its bytes at the end); nothing is ever removed but by the
// process's exit. So a new player's steps may grow it (the seventh run: player 2's files added strings, the strings'
// buffer moved), and region_evidence.py REVIEWED_POOL takes the nine fields that growth writes out of the byte
// comparison of the protected state; this check replaces that comparison with what must hold instead:
//   - the constructor's constants unchanged: the less function, the node grow, the string grow;
//   - append-only: the node count and the next node never decrease, the free list stays empty, the strings' size never
//     decreases; every count within its allocation;
//   - every index given before names the same offset and the same string (a moved buffer included);
//   - every node's offset inside the strings, its string ended inside them;
//   - the buffers are the process's heap blocks (never a player's, which its leaving or a rollback would free), big
//     enough for what they hold.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace internpool {

constexpr uint32_t kPool = 0x562118;
constexpr uint32_t kLessInsensitive = 0x115C20, kLessSensitive = 0x115BA0;  // the constructor's choice (cl)
constexpr uint16_t kInvalid = 0xFFFF;
constexpr uint32_t kNodeSize = 12, kNodeOffset = 8;
// the fields, as 0x115C70 lays them out (offsets from the pool)
constexpr uint32_t kLess = 0x00, kNodes = 0x04, kNodesAlloc = 0x08, kNodesGrow = 0x0C, kRoot = 0x10, kCount = 0x12,
                   kFree = 0x14, kNext = 0x16, kStrings = 0x18, kStringsAlloc = 0x1C, kStringsGrow = 0x20,
                   kStringsSize = 0x24, kPoolSize = 0x28;

struct Header {
    uint32_t less = 0, nodes = 0, nodes_alloc = 0, nodes_grow = 0;
    uint16_t root = kInvalid, count = 0, free = kInvalid, next = 0;
    uint32_t strings = 0, strings_alloc = 0, strings_grow = 0, strings_size = 0;
};
struct Entry {
    uint16_t index;
    uint32_t offset;
    std::string text;
};
struct State {
    Header h;
    std::vector<Entry> entries;  // nodes 0 .. next-1 (none is ever freed)
    bool complete = true;        // false: an entry could not be read (reported by Check)
};

using ReadByte = std::function<uint8_t(uint32_t)>;
inline uint32_t R32(const ReadByte& rd, uint32_t va) {
    return uint32_t(rd(va)) | uint32_t(rd(va + 1)) << 8 | uint32_t(rd(va + 2)) << 16 | uint32_t(rd(va + 3)) << 24;
}
inline uint16_t R16(const ReadByte& rd, uint32_t va) { return uint16_t(rd(va) | rd(va + 1) << 8); }

inline Header ReadHeader(const ReadByte& rd, uint32_t pool = kPool) {
    Header h;
    h.less = R32(rd, pool + kLess);
    h.nodes = R32(rd, pool + kNodes);
    h.nodes_alloc = R32(rd, pool + kNodesAlloc);
    h.nodes_grow = R32(rd, pool + kNodesGrow);
    h.root = R16(rd, pool + kRoot);
    h.count = R16(rd, pool + kCount);
    h.free = R16(rd, pool + kFree);
    h.next = R16(rd, pool + kNext);
    h.strings = R32(rd, pool + kStrings);
    h.strings_alloc = R32(rd, pool + kStringsAlloc);
    h.strings_grow = R32(rd, pool + kStringsGrow);
    h.strings_size = R32(rd, pool + kStringsSize);
    return h;
}

// the pool as it is: its header and every node's offset and string (a string read up to the strings' size; a node or
// string outside what the header allows is not read: Check reports it)
inline State Capture(const ReadByte& rd, uint32_t pool = kPool) {
    State s;
    s.h = ReadHeader(rd, pool);
    if (s.h.next > s.h.nodes_alloc || (s.h.next && !s.h.nodes) || (s.h.strings_size && !s.h.strings)) {
        s.complete = false;
        return s;
    }
    for (uint32_t i = 0; i < s.h.next; i++) {
        uint32_t off = R32(rd, s.h.nodes + i * kNodeSize + kNodeOffset);
        Entry e{uint16_t(i), off, {}};
        if (off >= s.h.strings_size) {
            s.complete = false;
            s.entries.push_back(e);
            continue;
        }
        uint32_t k = off;
        while (k < s.h.strings_size && rd(s.h.strings + k)) e.text.push_back(char(rd(s.h.strings + k++)));
        if (k >= s.h.strings_size) s.complete = false;  // no terminator inside the strings
        s.entries.push_back(std::move(e));
    }
    return s;
}

// a heap block as the ownership tracker knows it: whether it is known, its owner (heap_owner.h kProcess -1, else a
// player's slot) and its size
struct BlockInfo {
    bool known = false;
    int owner = -1;
    uint32_t size = 0;
};
using BlockOf = std::function<BlockInfo(uint32_t)>;

// the problems between `before` (the pool before a new player's steps) and `after` (after them); none: every
// invariant above holds. Growth is not a problem.
inline std::vector<std::string> Check(const State& before, const State& after, const BlockOf& block) {
    std::vector<std::string> out;
    auto hex = [](uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "0x%X", v);
        return std::string(b);
    };
    const Header &a = before.h, &b = after.h;
    if (b.less != a.less || (b.less != kLessInsensitive && b.less != kLessSensitive))
        out.push_back("the less function changed: " + hex(a.less) + " -> " + hex(b.less));
    if (b.nodes_grow != a.nodes_grow) out.push_back("the node grow changed: " + hex(a.nodes_grow) + " -> " + hex(b.nodes_grow));
    if (b.strings_grow != a.strings_grow)
        out.push_back("the string grow changed: " + hex(a.strings_grow) + " -> " + hex(b.strings_grow));
    if (b.count < a.count || b.next < a.next)
        out.push_back("nodes removed: count " + std::to_string(a.count) + " -> " + std::to_string(b.count) + ", next " +
                      std::to_string(a.next) + " -> " + std::to_string(b.next));
    if (b.count != b.next || b.free != kInvalid)
        out.push_back("not append-only: count " + std::to_string(b.count) + ", next " + std::to_string(b.next) +
                      ", free list " + hex(b.free));
    if (b.next > b.nodes_alloc)
        out.push_back("more nodes than allocated: " + std::to_string(b.next) + " of " + std::to_string(b.nodes_alloc));
    if (b.strings_size > b.strings_alloc)
        out.push_back("more string bytes than allocated: " + std::to_string(b.strings_size) + " of " +
                      std::to_string(b.strings_alloc));
    if (b.strings_size < a.strings_size)
        out.push_back("the strings shrank: " + std::to_string(a.strings_size) + " -> " + std::to_string(b.strings_size));
    if (b.count == 0 ? b.root != kInvalid : b.root >= b.next) out.push_back("the root " + hex(b.root) + " is no node");
    if (!after.complete) out.push_back("a node's offset or string lies outside the strings");
    // the indices given before: the same offset, the same string
    size_t moved = 0;
    for (const Entry& e : before.entries) {
        if (e.index >= after.entries.size()) {
            out.push_back("index " + std::to_string(e.index) + " is gone");
            continue;
        }
        const Entry& f = after.entries[e.index];
        if (f.offset != e.offset || f.text != e.text) {
            if (moved++ < 4)
                out.push_back("index " + std::to_string(e.index) + " (\"" + e.text + "\") now names offset " +
                              hex(f.offset) + " (\"" + f.text + "\")");
        }
    }
    if (moved > 4) out.push_back(std::to_string(moved - 4) + " more indices changed");
    // the buffers: the process's, big enough
    auto buffer = [&](const char* what, uint32_t va, uint32_t bytes) {
        if (!va) return;
        BlockInfo bi = block(va);
        if (!bi.known)
            out.push_back(std::string(what) + " " + hex(va) + " is no known heap block");
        else if (bi.owner != -1)
            out.push_back(std::string(what) + " " + hex(va) + " is player " + std::to_string(bi.owner + 1) +
                          "'s block, not the process's");
        else if (bi.size < bytes)
            out.push_back(std::string(what) + " " + hex(va) + " holds " + std::to_string(bi.size) + " bytes, " +
                          std::to_string(bytes) + " allocated");
    };
    buffer("the node array", b.nodes, b.nodes_alloc * kNodeSize);
    buffer("the strings' buffer", b.strings, b.strings_alloc);
    return out;
}

}  // namespace internpool
