// The packet overflow (FINDINGS.md "Packet overflow", G8.2B): with 64 bots the player, dead and spectating, crashed at
// a round restart with the game's stack check ("Buffer overrun detected"): SV_SendClientDatagram (0x9DF30) builds each
// client's packet in a 4000-byte buffer on its stack; the restart's packet entities passed 4000 bytes and the engine's
// bit writer wrote on past the buffer's end over the stack cookie. The bit writer (MSG_WriteBits 0x3E220 and its kin,
// the bfwrite state at 0x4319C0) writes through its own cursor and only tells the buffer (SZ_GetSpace) how far it got:
// past the capacity SZ_GetSpace marks the buffer overflowed and empties it, and the next writes, seeing an empty
// buffer, carried on past its end. It also wrote whole dwords (up to 7 bytes past the bits) and, at exactly full
// capacity, wrote into the byte after the buffer.
//
// Fixed here, at the source:
// - the bit writer is the host's (src/game/net_buffers.h): byte by byte, each byte checked to lie inside the buffer
//   before it is written, nothing written once the buffer has overflowed; the end of a bit write (the function and its
//   nine inlined copies) skips its store outside the buffer. An overflow is the engine's own (flag, empty buffer, the
//   caller drops the message), or its fatal error on a buffer not allowed to overflow.
// - SZ_GetSpace's request larger than a whole buffer allowed to overflow (it returned the buffer's start for the caller
//   to write the whole request) gets a scratch area; a negative length is the engine's fatal error.
// - capacity: a loopback client's packet is built in 60 KB (guest table net_client_datagram), its netchan datagram in
//   64 KB (net_transmit, the size of the client's net_message and of a demo message), the loopback queue holds whole
//   datagrams (4040 bytes a slot before). This is the local match's transport, the only one the game has on the PC.
//   A network client keeps the original 4000-byte packet and 4037-byte datagram: NET_SendLong splits those into
//   1295-byte parts and NET_GetLong takes 4010 bytes at most, so a larger snapshot over UDP needs a larger split
//   reassembly (docs/networking.md); past 4000 its packet overflows safely and is dropped, as the engine always did.
// - from the network: a split packet's part index, count and length (NET_GetLong) and a packet's fragment offsets and
//   lengths (Netchan_Process) are checked; a bad one drops the packet.
// CSX_LOG_DATAGRAMS=1 logs, once a second, the largest packet built and its packet entities.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_buffers.h"
#include "kernel/kernel.h"

namespace {
using kernel::R16;
using kernel::R32;
using kernel::W16;
using kernel::W32;

constexpr uint32_t kBitCount = 0x4319C0, kBitPtr = 0x4319C4, kBitBuf = 0x4319C8;  // bfwrite
constexpr uint32_t kGetSpace = 0x3ECE0;   // SZ_GetSpace(buf, length), cdecl
constexpr uint32_t kSysError = 0xAC090;   // Sys_Error(format, ...), cdecl: does not return
constexpr uint32_t kTooLarge = 0x32DC98;  // "SZ_GetSpace: %i is > full buffer size on %s"
constexpr uint32_t kReadCount = 0x748404, kNetMessageSize = 0x6BA590;  // msg_readcount, net_message.cursize
constexpr uint32_t kLoopback = 1;         // netadrtype_t NA_LOOPBACK
constexpr uint32_t kNetIp = 3;            // NA_IP: a network peer on the PC transport
constexpr uint32_t kClientChannel = 0x20, kClientName = 0x4D10;  // client_t: netchan (remote address at +4), name
constexpr uint32_t kLoopBuild = 0xF000;   // a loopback client's packet: room left in the datagram for the netchan's
                                          // header and a reliable message (3990 bytes) or fragment (1304)

struct GuestMem {
    uint8_t R8(uint32_t va) { return kernel::G<uint8_t>(va); }
    void W8(uint32_t va, uint8_t v) { kernel::G<uint8_t>(va) = v; }
    uint16_t R16(uint32_t va) { return kernel::R16(va); }
    uint32_t R32(uint32_t va) { return kernel::R32(va); }
    void W32(uint32_t va, uint32_t v) { kernel::W32(va, v); }
};
GuestMem g_mem;

const guest_tables::Table& TableOf(size_t k) { return guest_tables::kTables[k]; }

bool LogDatagrams() {
    static const bool on = std::getenv("CSX_LOG_DATAGRAMS") && *std::getenv("CSX_LOG_DATAGRAMS") == '1';
    return on;
}

std::string BufName(uint32_t buf) {
    uint32_t name = R32(buf + netbuf::kName);
    return name ? kernel::ReadCString(name, 64) : std::string("unnamed");
}

// the bit writer reached its buffer's capacity: the engine's overflow, which SZ_GetSpace would have made of the next
// reservation (a buffer not allowed to overflow is the engine's fatal error, through SZ_GetSpace itself)
void BitOverflow(X86Context& ctx, uint32_t buf) {
    uint16_t flags = R16(buf + netbuf::kFlags);
    uint32_t max = R32(buf + netbuf::kMaxSize);
    if (!(flags & netbuf::kAllowOverflow)) kernel::CallGuest(ctx, kGetSpace, {buf, max + 1});
    LOG_FIRST(64, "net: %s overflowed in the bit writer (%u bytes): emptied and dropped by its owner, nothing written "
              "past it", BufName(buf).c_str(), max);
    W16(buf + netbuf::kFlags, uint16_t(flags | netbuf::kOverflowed));
    W32(buf + netbuf::kCurSize, 0);
}

netbuf::Writer LoadWriter() { return {R32(kBitCount), R32(kBitPtr), R32(kBitBuf)}; }
void StoreWriter(const netbuf::Writer& w) {
    W32(kBitCount, w.bit);
    W32(kBitPtr, w.ptr);
}

// the largest packet of the last second, for CSX_LOG_DATAGRAMS
struct Stats {
    uint32_t packets = 0, largest = 0, entities = 0, overflowed = 0, before_entities = 0, loop_largest = 0;
    uint32_t second = 0;
} g_stats;

std::mutex g_loop_lock;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_loop;  // the loopback slots' datagrams, by slot address
}  // namespace

// [[hook]] at 0x3E220 (MSG_WriteBits: eax the value, ecx the bit count), on_true 0x3E2DF (its ret): replaced
bool NetWriteBits(X86Context& ctx, uint8_t*) {
    netbuf::Writer w = LoadWriter();
    netbuf::Result r = netbuf::WriteBits(g_mem, w, ctx.eax, int32_t(ctx.ecx));
    StoreWriter(w);
    if (r == netbuf::Result::Overflow) BitOverflow(ctx, w.buf);
    ctx.eax = w.ptr;
    return true;
}

// [[hook]] at 0x3E160 (MSG_WriteOneBit(value), cdecl), on_true 0x3E1CC (its ret): replaced
bool NetWriteOneBit(X86Context& ctx, uint8_t*) {
    netbuf::Writer w = LoadWriter();
    netbuf::Result r = netbuf::WriteOneBit(g_mem, w, R32(ctx.esp + 4) != 0);
    StoreWriter(w);
    if (r == netbuf::Result::Overflow) BitOverflow(ctx, w.buf);
    ctx.eax = w.ptr;
    return true;
}

// [[hook]] at 0x3E36F (MSG_WriteSBits: esi the value clamped, edi the bit count, about to write its sign bit), on_true
// 0x3E40C (where it writes the magnitude through MSG_WriteBits)
bool NetWriteSignBit(X86Context& ctx, uint8_t*) {
    netbuf::Writer w = LoadWriter();
    netbuf::Result r = netbuf::WriteOneBit(g_mem, w, int32_t(ctx.esi) < 0);
    StoreWriter(w);
    if (r == netbuf::Result::Overflow) BitOverflow(ctx, w.buf);
    return true;
}

// [[hook]] at MSG_EndBitWriting's store of the current byte (0x3E1FA and the nine inlined copies, eax the byte):
// true skips the store, the byte being outside the buffer (the reservation that follows overflows it)
bool NetEndBitStore(X86Context& ctx, uint8_t*) {
    netbuf::Writer w = LoadWriter();
    w.ptr = ctx.eax;
    return w.buf && !netbuf::EndStoreAllowed(g_mem, w);
}

// [[hook]] at 0x3ED73 (SZ_GetSpace about to hand out space: esi the buffer, edi the length), on_true 0x3ED7D (its
// return with eax the space and ecx the new cursize)
bool NetGetSpaceCheck(X86Context& ctx, uint8_t*) {
    const auto& scratch = TableOf(guest_tables::k_net_getspace_scratch);
    switch (netbuf::GetSpaceAction(int32_t(ctx.edi), R32(ctx.esi + netbuf::kMaxSize), scratch.count)) {
    case netbuf::Space::Normal:
        return false;
    case netbuf::Space::Scratch:
        LOG_FIRST(16, "net: %s was asked for %d bytes, more than it holds (%u): written to a scratch area, the buffer "
                  "dropped", BufName(ctx.esi).c_str(), int32_t(ctx.edi), R32(ctx.esi + netbuf::kMaxSize));
        ctx.eax = scratch.va;
        ctx.ecx = 0;
        return true;
    case netbuf::Space::Fatal:
        LOGE("net: %s was asked for %d bytes (it holds %u)", BufName(ctx.esi).c_str(), int32_t(ctx.edi),
             R32(ctx.esi + netbuf::kMaxSize));
        kernel::CallGuest(ctx, kSysError, {kTooLarge, ctx.edi, R32(ctx.esi + netbuf::kName)});
        return false;
    }
    return false;
}

// [[hook]] at 0x9DF73 (SV_SendClientDatagram: its message set up at esp+0x18, esi the client): a loopback or network
// client's packet is built in net_client_datagram instead of the 4000 bytes on the stack (the PC transport carries
// 64 KB to a network client, src/net/transport.h; one call builds and sends it, so one buffer serves every client)
void SvDatagramBuffer(X86Context& ctx, uint8_t*) {
    uint32_t type = R32(ctx.esi + kClientChannel + 4);
    if (type != kLoopback && type != kNetIp) return;
    const auto& t = TableOf(guest_tables::k_net_client_datagram);
    uint32_t msg = ctx.esp + 0x18;
    W32(msg + netbuf::kData, t.va);
    W32(msg + netbuf::kMaxSize, std::min<uint32_t>(kLoopBuild, t.count));
}

// [[hook]] at 0x9DFB9 and 0x9DFC5 (around SV_WriteEntitiesToClient: the message at esp+0x14, then at esp+0x18)
void SvDatagramEntitiesBegin(X86Context& ctx, uint8_t*) {
    g_stats.before_entities = R32(ctx.esp + 0x14 + netbuf::kCurSize);
}
void SvDatagramEntitiesEnd(X86Context& ctx, uint8_t*) {
    uint32_t after = R32(ctx.esp + 0x18 + netbuf::kCurSize);
    if (after >= g_stats.before_entities) g_stats.entities = std::max(g_stats.entities, after - g_stats.before_entities);
}

// [[hook]] at 0x9E027 (SV_SendClientDatagram found its message overflowed, esi the client: it warns, empties it)
void SvDatagramOverflowed(X86Context& ctx, uint8_t*) {
    g_stats.overflowed++;
    LOG_FIRST(64, "net: the packet for %s overflowed: sent without its entities (the engine's handling)",
              kernel::ReadCString(ctx.esi + kClientName, 32).c_str());
}

// [[hook]] at 0x9E052 (Netchan_Transmit(channel, length, data) about to be called with the stack buffer's address):
// the message's own buffer, which SvDatagramBuffer may have moved (the message's data at esp+0x24)
void SvDatagramTransmit(X86Context& ctx, uint8_t*) {
    W32(ctx.esp + 8, R32(ctx.esp + 0x24));
    uint32_t len = R32(ctx.esp + 4);
    g_stats.packets++;
    g_stats.largest = std::max(g_stats.largest, len);
    if (!LogDatagrams()) return;
    static auto last = std::chrono::steady_clock::now();
    auto t = std::chrono::steady_clock::now();
    if (t - last < std::chrono::seconds(1)) return;
    last = t;
    LOGI("net: %u packets a second, the largest %u bytes (packet entities %u), %u overflowed; the largest loopback "
         "datagram %u bytes", g_stats.packets, g_stats.largest, g_stats.entities, g_stats.overflowed,
         g_stats.loop_largest);
    g_stats = Stats{};
}

// [[hook]] at 0x6996E (Netchan_Transmit, its datagram set up at esp+0x20, edi the channel): a loopback or network
// channel's datagram is built in net_transmit (the transport sends up to 64 KB)
void NetchanSendBuffer(X86Context& ctx, uint8_t*) {
    uint32_t type = R32(ctx.edi + 4);
    if (type != kLoopback && type != kNetIp) return;
    const auto& t = TableOf(guest_tables::k_net_transmit);
    uint32_t send = ctx.esp + 0x20;
    W32(send + netbuf::kData, t.va);
    W32(send + netbuf::kMaxSize, t.count);
}

// [[hook]] at 0x6BBF1 (NET_SendLoopPacket's copy into the other side's slot: eax the slot, ecx the data, edx the
// length), on_true 0x6BBF6: the datagram is kept whole by the host (a slot holds 4040 bytes)
bool NetLoopSend(X86Context& ctx, uint8_t*) {
    std::lock_guard<std::mutex> lock(g_loop_lock);
    const uint8_t* src = static_cast<const uint8_t*>(kernel::HostPtr(ctx.ecx));
    g_loop[ctx.eax].assign(src, src + ctx.edx);
    g_stats.loop_largest = std::max(g_stats.loop_largest, ctx.edx);
    return true;
}

// [[hook]] at 0x6BB49 (NET_GetLoopPacket's copy out: ecx the slot, eax the message's data, edx the length, edi the
// message), on_true 0x6BB4E (esi becomes its cursize): a datagram that does not fit the message is dropped
bool NetLoopGet(X86Context& ctx, uint8_t*) {
    std::lock_guard<std::mutex> lock(g_loop_lock);
    auto it = g_loop.find(ctx.ecx);
    uint32_t max = R32(ctx.edi + netbuf::kMaxSize);
    if (it == g_loop.end() || it->second.size() != ctx.edx || ctx.edx > max) {
        LOG_FIRST(16, "net: a loopback datagram of %u bytes dropped (the message holds %u)", ctx.edx, max);
        ctx.esi = 0;
        return true;
    }
    std::memcpy(kernel::HostPtr(ctx.eax), it->second.data(), ctx.edx);
    return true;
}

// [[hook]] at 0x6C1FB (NET_GetLong: ebx the part count, edi the part index, ebp the packet's length), on_true 0x6C2E4
// (returns 0: no packet)
bool NetSplitCheck(X86Context& ctx, uint8_t*) {
    if (netbuf::SplitPartValid(int32_t(ctx.ebx), int32_t(ctx.edi), int32_t(ctx.ebp))) return false;
    LOG_FIRST(16, "net: a split packet dropped: part %d of %d, %d bytes", int32_t(ctx.edi), int32_t(ctx.ebx),
              int32_t(ctx.ebp));
    return true;
}

// [[hook]] at 0x6A71D (Netchan_Process about to take the packet's fragments: per stream at esp+0x3C present, +0x24
// offset, +0x34 length), on_true 0x6A3A1 (returns 0: the packet dropped)
bool NetFragmentsCheck(X86Context& ctx, uint8_t*) {
    netbuf::Fragment f[2];
    for (uint32_t i = 0; i < 2; i++) {
        f[i].present = R32(ctx.esp + 0x3C + 4 * i) != 0;
        f[i].offset = int32_t(R32(ctx.esp + 0x24 + 4 * i));
        f[i].length = int32_t(R32(ctx.esp + 0x34 + 4 * i));
    }
    if (netbuf::FragmentsValid(f, int32_t(R32(kReadCount)), int32_t(R32(kNetMessageSize)))) return false;
    LOG_FIRST(16, "net: a packet's fragments out of range dropped (%d+%d, %d+%d of %d)", f[0].offset, f[0].length,
              f[1].offset, f[1].length, int32_t(R32(kNetMessageSize)));
    return true;
}
