// A fingerprint of a generated table as it was compiled (docs/native_splitscreen.md 15.7): the region model's tables
// (client_context_regions.inc, _protected.inc, _addresses.inc) are hashed at compile time, from the very arrays the
// executable uses, into a stamp with a fixed marker. tools/check_tables.py finds each stamp in an executable and
// compares it with the same hash of the tables in the source tree: an object compiled from an older table (a stale
// build, whatever the timestamps say) is reported, as is a missing or doubled stamp.
//
// The hash: FNV-1a 64 over little-endian 32-bit words, the entry count first, then each entry's numbers in order
// (a region / protected range: its address, then its size; an address: itself). The descriptions are not hashed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tablefp {

constexpr uint64_t kBasis = 0xCBF29CE484222325ull, kPrime = 0x100000001B3ull;

constexpr uint64_t Word(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= kPrime;
    }
    return h;
}

// a table of {va, size, ...} entries
template <class T, size_t N>
constexpr uint64_t Ranges(const T (&t)[N]) {
    uint64_t h = Word(kBasis, uint32_t(N));
    for (size_t i = 0; i < N; i++) h = Word(Word(h, t[i].va), t[i].size);
    return h;
}
template <class T, size_t N>
constexpr uint32_t Bytes(const T (&t)[N]) {
    uint32_t n = 0;
    for (size_t i = 0; i < N; i++) n += t[i].size;
    return n;
}
// a table of addresses
template <size_t N>
constexpr uint64_t Words(const uint32_t (&t)[N]) {
    uint64_t h = Word(kBasis, uint32_t(N));
    for (size_t i = 0; i < N; i++) h = Word(h, t[i]);
    return h;
}

// the stamp in the executable: 16 bytes of marker ("CSXFP1:" and the table's name, NUL-padded), the hash, the entry
// count and the bytes covered (0 for an address table); 32 bytes, little-endian
struct Stamp {
    char marker[16];
    uint64_t hash;
    uint32_t count;
    uint32_t bytes;
};
static_assert(sizeof(Stamp) == 32, "the stamp's layout is tools/check_tables.py's");

}  // namespace tablefp
