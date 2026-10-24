// src/game/net_buffers.h on its own (tests/net_buffers_test.py builds and runs it): the bounded bit writer against a
// model of the engine's (0x3E220: dword writes, no bounds) for the same bits; writes at exactly the capacity, one bit
// past it, after an overflow, from a full buffer; bit counts out of range and saturated values; MSG_EndBitWriting's
// store; SZ_GetSpace's lengths; split packet headers and netchan fragments from the network.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "game/net_buffers.h"

namespace {
int g_failed = 0, g_checks = 0;

void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// guest memory: a sizebuf at kBuf, its data at kData, then guard bytes the writer must never touch
constexpr uint32_t kBuf = 0x10, kData = 0x100, kGuardFill = 0xCC, kMem = 0x10000;
struct Mem {
    std::vector<uint8_t> b = std::vector<uint8_t>(kMem, kGuardFill);
    uint8_t R8(uint32_t a) { return b.at(a); }
    void W8(uint32_t a, uint8_t v) { b.at(a) = v; }
    uint16_t R16(uint32_t a) { return uint16_t(b.at(a) | b.at(a + 1) << 8); }
    uint32_t R32(uint32_t a) { return uint32_t(R16(a)) | uint32_t(R16(a + 2)) << 16; }
    void W16(uint32_t a, uint16_t v) { W8(a, uint8_t(v)), W8(a + 1, uint8_t(v >> 8)); }
    void W32(uint32_t a, uint32_t v) { W16(a, uint16_t(v)), W16(a + 2, uint16_t(v >> 16)); }
};

// a buffer of `max` bytes with `cur` reserved, flags as given; the writer started on it (MSG_StartBitWriting)
netbuf::Writer Start(Mem& m, uint32_t max, uint32_t cur = 0, uint16_t flags = netbuf::kAllowOverflow) {
    m.W32(kBuf + netbuf::kName, 0);
    m.W16(kBuf + netbuf::kFlags, flags);
    m.W32(kBuf + netbuf::kData, kData);
    m.W32(kBuf + netbuf::kMaxSize, max);
    m.W32(kBuf + netbuf::kCurSize, cur);
    for (uint32_t i = 0; i < max; i++) m.W8(kData + i, uint8_t(i * 7));  // stale content, as on a stack
    return {0, kData + cur, kBuf};
}

bool GuardIntact(Mem& m, uint32_t max) {
    for (uint32_t a = kData + max; a < kMem; a++)
        if (m.b[a] != kGuardFill) return false;
    return true;
}

bool Overflowed(Mem& m) { return m.R16(kBuf + netbuf::kFlags) & netbuf::kOverflowed; }

// MSG_EndBitWriting as the engine does it, with the store checked
void End(Mem& m, netbuf::Writer& w) {
    if (Overflowed(m)) return;
    if (netbuf::EndStoreAllowed(m, w)) m.W8(w.ptr, uint8_t(m.R8(w.ptr) & (0xFF >> (8 - w.bit))));
    if (!netbuf::Reserve(m, w.buf, 1)) m.W16(kBuf + netbuf::kFlags, m.R16(kBuf + netbuf::kFlags) | netbuf::kOverflowed);
}

// the engine's MSG_WriteBits (0x3E220) on an unbounded buffer: what the bytes it reserves hold
struct Original {
    std::vector<uint8_t> d = std::vector<uint8_t>(0x8000, 0);
    uint32_t bit = 0, ptr = 0, cur = 0;
    uint32_t R(uint32_t p) { return uint32_t(d[p]) | d[p + 1] << 8 | d[p + 2] << 16 | uint32_t(d[p + 3]) << 24; }
    void W(uint32_t p, uint32_t v) { for (int i = 0; i < 4; i++) d[p + i] = uint8_t(v >> (8 * i)); }
    void WriteBits(uint32_t v, int n) {
        if (n < 32 && v >= (1u << n)) v = (1u << n) - 1;
        uint32_t extra = 0;
        if (bit >= 8) extra = 1, ptr++, bit = 0;
        uint32_t total = bit + uint32_t(n);
        uint32_t lowmask = bit ? (1u << bit) - 1 : 0;
        if (total > 32) {
            cur += extra + 4;
            W(ptr, (R(ptr) & lowmask) | v << bit);
            W(ptr + 4, v >> (32 - bit));
            ptr += 4;
            bit = total & 7;
        } else {
            uint32_t step = total >> 3;
            if (!(total & 7)) step--;
            cur += extra + step;
            W(ptr, (R(ptr) & lowmask) | v << bit);
            bit = (total & 7) ? (total & 7) : 8;
            ptr += step;
        }
    }
};

// random bit writes on a buffer large enough: the bounded writer reserves the same bytes and leaves the same bits as
// the engine's, and keeps its cursor in step
void SameAsOriginal() {
    std::mt19937 rng(7);
    for (int run = 0; run < 200; run++) {
        Mem m;
        Original o;
        netbuf::Writer w = Start(m, 0x4000);
        for (uint32_t i = 0; i < 0x4000; i++) m.W8(kData + i, 0);
        int writes = 1 + int(rng() % 300);
        for (int k = 0; k < writes; k++) {
            int n = 1 + int(rng() % 32);
            uint32_t v = rng();
            if (rng() % 4 == 0 && n < 32) v &= (1u << n) - 1;  // in range as well as saturated
            o.WriteBits(v, n);
            netbuf::WriteBits(m, w, v, n);
        }
        uint32_t cur = m.R32(kBuf + netbuf::kCurSize);
        bool same = cur == o.cur && w.ptr - kData == o.ptr && w.bit == o.bit;
        for (uint32_t i = 0; same && i < cur; i++) same = m.R8(kData + i) == o.d[i];
        if (same && w.bit && w.bit < 8)
            same = (m.R8(w.ptr) & ((1u << w.bit) - 1)) == (o.d[o.ptr] & ((1u << w.bit) - 1));
        Check(same, "the same bits as the engine's writer, run " + std::to_string(run));
    }
}

void Capacity() {
    for (uint32_t max : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 13u, 4000u}) {
        std::string at = " (" + std::to_string(max) + " bytes)";
        // exactly the capacity: every bit fits, ending reserves the last byte, nothing past it
        Mem m;
        netbuf::Writer w = Start(m, max);
        uint32_t bits = max * 8, written = 0;
        bool ok = true;
        while (written < bits) {
            int n = int(std::min<uint32_t>(bits - written, 1 + written % 32));
            ok &= netbuf::WriteBits(m, w, 0xFFFFFFFFu, n) == netbuf::Result::Done;
            written += uint32_t(n);
        }
        End(m, w);
        Check(ok && !Overflowed(m) && m.R32(kBuf + netbuf::kCurSize) == max, "exactly full fits" + at);
        Check(GuardIntact(m, max), "exactly full writes nothing past the buffer" + at);
        // one bit more: an overflow, nothing past the buffer, and the writes after it do nothing
        Mem m2;
        netbuf::Writer w2 = Start(m2, max);
        for (uint32_t i = 0; i < bits; i++) netbuf::WriteOneBit(m2, w2, true);
        Check(netbuf::WriteOneBit(m2, w2, true) == netbuf::Result::Overflow, "one bit past the capacity overflows" + at);
        m2.W16(kBuf + netbuf::kFlags, m2.R16(kBuf + netbuf::kFlags) | netbuf::kOverflowed);  // as BitOverflow does
        m2.W32(kBuf + netbuf::kCurSize, 0);
        for (int k = 0; k < 2000; k++)
            Check(netbuf::WriteBits(m2, w2, 0xFFFFFFFFu, 32) == netbuf::Result::Skipped, "no writes after an overflow" + at);
        End(m2, w2);
        Check(GuardIntact(m2, max), "an overflow writes nothing past the buffer" + at);
        // a large write crossing the end (the engine's dwords reached up to 7 bytes past its bits)
        Mem m3;
        netbuf::Writer w3 = Start(m3, max, max - 1);
        w3.bit = 3;
        Check(netbuf::WriteBits(m3, w3, 0xFFFFFFFFu, 32) == netbuf::Result::Overflow, "32 bits from the last byte overflow" + at);
        Check(GuardIntact(m3, max), "32 bits from the last byte write nothing past it" + at);
        // a write session started on a full buffer: its first bit and its end touch nothing
        Mem m4;
        netbuf::Writer w4 = Start(m4, max, max);
        Check(netbuf::WriteOneBit(m4, w4, true) == netbuf::Result::Overflow, "a full buffer's first bit overflows" + at);
        Mem m5;
        netbuf::Writer w5 = Start(m5, max, max);
        End(m5, w5);  // nothing written, ended: the engine stored a byte at data[max]
        Check(GuardIntact(m5, max) && Overflowed(m5), "ending on a full buffer stores nothing past it" + at);
    }
}

void Lengths() {
    Mem m;
    netbuf::Writer w = Start(m, 64);
    Check(netbuf::WriteBits(m, w, 5, 0) == netbuf::Result::Done && w.bit == 0 && w.ptr == kData, "0 bits write nothing");
    Check(netbuf::WriteBits(m, w, 5, -3) == netbuf::Result::Done && w.bit == 0, "a negative count writes nothing");
    Check(netbuf::WriteBits(m, w, 0xFFFFFFFFu, 64) == netbuf::Result::Done && w.ptr == kData + 3 && w.bit == 8,
          "a count past 32 is 32");
    Check(m.R32(kData) == 0xFFFFFFFFu, "the 32 bits");
    Mem m2;
    netbuf::Writer w2 = Start(m2, 64);
    for (uint32_t i = 0; i < 64; i++) m2.W8(kData + i, 0);
    netbuf::WriteBits(m2, w2, 300, 8);  // saturates to 255, as the engine does
    Check(m2.R8(kData) == 0xFF, "a value too large saturates");
    Mem m3;
    netbuf::Writer w3{};
    Check(netbuf::WriteBits(m3, w3, 1, 1) == netbuf::Result::Skipped, "no buffer: nothing");
    // a buffer whose cursize is past its capacity (corrupt): no reservation
    Mem m4;
    netbuf::Writer w4 = Start(m4, 8, 9);
    w4.bit = 8;
    Check(netbuf::WriteOneBit(m4, w4, true) == netbuf::Result::Overflow && GuardIntact(m4, 8),
          "cursize past maxsize: refused");
}

void GetSpace() {
    using netbuf::Space;
    Check(netbuf::GetSpaceAction(0, 4000, 0x10000) == Space::Normal, "0 bytes");
    Check(netbuf::GetSpaceAction(4000, 4000, 0x10000) == Space::Normal, "the whole buffer");
    Check(netbuf::GetSpaceAction(4001, 4000, 0x10000) == Space::Scratch, "one byte more than the buffer: scratch");
    Check(netbuf::GetSpaceAction(0x10000, 4000, 0x10000) == Space::Scratch, "the scratch's size");
    Check(netbuf::GetSpaceAction(0x10001, 4000, 0x10000) == Space::Fatal, "more than the scratch: fatal");
    Check(netbuf::GetSpaceAction(-1, 4000, 0x10000) == Space::Fatal, "a negative length: fatal");
    Check(netbuf::GetSpaceAction(INT32_MIN, 4000, 0x10000) == Space::Fatal, "the most negative length: fatal");
}

void Splits() {
    using netbuf::SplitPartValid;
    // what NET_SendLong sends: a 4010-byte datagram in 4 parts of 1294 bytes and the rest (each with its header)
    for (int i = 0; i < 3; i++) Check(SplitPartValid(4, i, 9 + 0x50E), "a full part " + std::to_string(i));
    Check(SplitPartValid(4, 3, 9 + 4010 - 3 * 0x50E), "the last part of 4010 bytes");
    Check(SplitPartValid(1, 0, 9), "an empty single part");
    Check(SplitPartValid(5, 3, 9 + 0xFB4 - 3 * 0x50F), "the fourth part up to the buffer's end");
    Check(!SplitPartValid(5, 3, 9 + 0xFB4 - 3 * 0x50F + 1), "the fourth part one byte past the buffer");
    Check(!SplitPartValid(5, 4, 9), "a fifth part starts past the buffer (4 x 1295 > 4020)");
    Check(!SplitPartValid(0, 0, 100), "a count of 0");
    Check(!SplitPartValid(6, 0, 100), "a count past MAX_SPLIT");
    Check(!SplitPartValid(15, 14, 100), "the largest header values");
    Check(!SplitPartValid(3, 3, 100), "an index equal to the count");
    Check(!SplitPartValid(2, 5, 100), "an index past the count");
    Check(!SplitPartValid(2, 0, 9 + 0x50F + 1), "a part longer than a part");
    Check(!SplitPartValid(2, 0, 8), "shorter than the header");
    Check(!SplitPartValid(2, -1, 100), "a negative index");
}

void Fragments() {
    using netbuf::Fragment;
    auto valid = [](Fragment a, Fragment b, int rc, int cur) {
        Fragment f[2] = {a, b};
        return netbuf::FragmentsValid(f, rc, cur);
    };
    Fragment none{};
    Check(valid(none, none, 12, 12), "no fragments");
    Check(valid({true, 0, 100}, none, 12, 112), "a fragment ending at the message's end");
    Check(!valid({true, 0, 101}, none, 12, 112), "a fragment one byte past the message");
    Check(valid({true, 0, 0x518}, none, 12, 12 + 0x518), "a whole fragment buffer");
    Check(!valid({true, 0, 0x519}, none, 12, 12 + 0x600), "more than a fragment buffer");
    Check(!valid({true, -1, 10}, none, 12, 100), "a negative offset");
    Check(!valid({true, 0, -1}, none, 12, 100), "a negative length");
    Check(valid({true, 0, 50}, {true, 50, 50}, 12, 112), "two fragments one after the other");
    Check(!valid({true, 0, 50}, {true, 49, 50}, 12, 112), "two fragments overlapping");
    Check(!valid({true, 0, 50}, {true, 50, 51}, 12, 112), "the second fragment past the message");
    Check(valid(none, {true, 10, 20}, 12, 42), "only the file stream's fragment");
    Check(!valid({true, 0x7FFFFFFF, 0x518}, none, 12, 100), "an offset that overflows 32 bits");
}
}  // namespace

int main() {
    SameAsOriginal();
    Capacity();
    Lengths();
    GetSpace();
    Splits();
    Fragments();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
