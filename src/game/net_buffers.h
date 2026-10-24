// The engine's message buffers made safe (src/game/net_buffers.cpp; FINDINGS.md "Packet overflow"): the bit writer
// (MSG_WriteBits and its kin, which wrote through their own cursor past a buffer's end), SZ_GetSpace's request larger
// than a whole buffer, and the checks on split packets and netchan fragments from the network. Pure functions over a
// memory accessor (guest memory in the game, an array in tests/net_buffers_test.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace netbuf {

// sizebuf_t: name, flags (u16), data, maxsize, cursize
constexpr uint32_t kName = 0x0, kFlags = 0x4, kData = 0x8, kMaxSize = 0xC, kCurSize = 0x10;
constexpr uint16_t kAllowOverflow = 1, kOverflowed = 2;  // FSB_ALLOWOVERFLOW, FSB_OVERFLOWED

// the bit writer's state (the engine's bfwrite at 0x4319C0): the bits used of the current byte (0 to 8: 8 is a full
// byte the cursor has not left yet), the current byte, the buffer. The bytes before the current one are reserved in
// the buffer (cursize); the current byte is reserved when the cursor leaves it or the writing ends.
struct Writer {
    uint32_t bit = 0, ptr = 0, buf = 0;
};

enum class Result { Done, Skipped, Overflow };  // Skipped: no buffer or already overflowed; Overflow: no room now

// the current byte lies inside the buffer's capacity
template <class Mem>
bool CurrentInside(Mem& m, const Writer& w) {
    uint32_t data = m.R32(w.buf + kData), max = m.R32(w.buf + kMaxSize);
    return w.ptr >= data && w.ptr - data < max;
}

// reserves n more bytes; false (nothing changed) when they do not fit
template <class Mem>
bool Reserve(Mem& m, uint32_t buf, uint32_t n) {
    uint32_t cur = m.R32(buf + kCurSize), max = m.R32(buf + kMaxSize);
    if (cur > max || max - cur < n) return false;
    m.W32(buf + kCurSize, cur + n);
    return true;
}

// leaves a full current byte for the next one: reserves it
template <class Mem>
bool Advance(Mem& m, Writer& w) {
    if (w.bit < 8) return true;
    if (!Reserve(m, w.buf, 1)) return false;
    w.ptr++;
    w.bit = 0;
    return true;
}

// MSG_WriteBits (0x3E220): the low n bits of value, a value too large for them saturated to all ones (the engine's
// rule), n clamped to 0..32. Byte by byte, each byte checked to lie inside the buffer before it is written; the
// engine's version wrote whole dwords (up to 3 bytes past the bits, 7 when the bits crossed a dword) and went on
// after an overflow. Nothing is written once the buffer has overflowed.
template <class Mem>
Result WriteBits(Mem& m, Writer& w, uint32_t value, int n) {
    if (!w.buf || (m.R16(w.buf + kFlags) & kOverflowed)) return Result::Skipped;
    if (n <= 0) return Result::Done;
    if (n > 32) n = 32;
    if (n < 32 && value >= (1u << n)) value = (1u << n) - 1;
    while (n > 0) {
        if (!Advance(m, w) || !CurrentInside(m, w)) return Result::Overflow;
        int take = std::min(8 - int(w.bit), n);
        uint32_t b = m.R8(w.ptr) & ((1u << w.bit) - 1);  // the bits above are cleared, as the engine's mask did
        b |= (value & ((1u << take) - 1)) << w.bit;
        m.W8(w.ptr, uint8_t(b));
        value = take < 32 ? value >> take : 0;
        n -= take;
        w.bit += uint32_t(take);
    }
    return Result::Done;
}

// MSG_WriteOneBit (0x3E160) and WriteSBits' sign bit: one bit, set when value is non-zero
template <class Mem>
Result WriteOneBit(Mem& m, Writer& w, bool value) {
    return WriteBits(m, w, value ? 1u : 0u, 1);
}

// MSG_EndBitWriting (0x3E1D0 and its nine inlined copies) stores the current byte with its unused bits cleared,
// then reserves it: the store is skipped when the byte lies outside the buffer (the reservation then overflows it)
template <class Mem>
bool EndStoreAllowed(Mem& m, const Writer& w) {
    return CurrentInside(m, w);
}

// SZ_GetSpace (0x3ECE0) about to return space for a request of len bytes (after its own overflow handling, which on
// a buffer allowed to overflow empties it): what to do
enum class Space { Normal, Scratch, Fatal };
inline Space GetSpaceAction(int32_t len, uint32_t maxsize, uint32_t scratch_size) {
    if (len < 0) return Space::Fatal;  // a negative length would shrink cursize and copy "len" bytes
    if (uint32_t(len) <= maxsize) return Space::Normal;
    // larger than the whole buffer (only an overflowing buffer allowed to overflow gets here): the engine returned the
    // buffer's start and its caller wrote len bytes there; it gets a scratch area instead, the buffer stays overflowed
    return uint32_t(len) <= scratch_size ? Space::Scratch : Space::Fatal;
}

// NET_GetLong (0x6C1A0): a split packet's header byte is part index (high nibble) and part count (low nibble), its
// payload follows a 9-byte header. The engine checked neither: a part index past the count or past MAX_SPLIT wrote
// past the five sequence slots (0x378838) and the reassembly buffer (0x6A95CC, 4020 bytes), a part longer than a
// part's 1295 bytes wrote past its place.
constexpr int kMaxSplit = 5;              // the sequence slots at 0x378838 (the send counter follows at 0x37884C)
constexpr int kSplitPart = 0x50F;         // a part's payload (NET_SendLong sends 0x50E, places them 0x50F apart)
constexpr int kSplitBuffer = 0xFB4;       // 0x6A95CC up to in_message's data at 0x6AA580
constexpr int kSplitHeader = 9;
inline bool SplitPartValid(int count, int index, int packet_len) {
    if (count < 1 || count > kMaxSplit || index < 0 || index >= count) return false;
    int len = packet_len - kSplitHeader;
    if (len < 0 || len > kSplitPart) return false;
    return index * kSplitPart + len <= kSplitBuffer;
}

// Netchan_Process (0x6A2C0): up to two fragments (the normal and the file stream) per packet, each an offset and a
// length into the message after its header (readcount), in order; the engine copied each into a 1304-byte fragment
// buffer and moved the rest of the message down without checking either against the message
constexpr int kFragmentSize = 0x518;
struct Fragment {
    bool present = false;
    int32_t offset = 0, length = 0;
};
inline bool FragmentsValid(const Fragment (&f)[2], int32_t readcount, int32_t cursize) {
    int64_t end = 0;  // the end of the previous fragment, in the message's own offsets
    for (const Fragment& x : f) {
        if (!x.present) continue;
        if (x.offset < 0 || x.length < 0 || x.length > kFragmentSize || x.offset < end) return false;
        end = int64_t(x.offset) + x.length;
        if (int64_t(readcount) + end > cursize) return false;
    }
    return true;
}

}  // namespace netbuf
