// XACT's event heap checked on every change (FINDINGS.md "Number of Bots to 64, one team, DISPATCH_LEVEL"): XACT keeps
// its timed events in a binary heap (engine +0x160: array, capacity, count) and changes it at DISPATCH_LEVEL, alone on
// the console. Before SDK patch 0038 the DPC thread and the game thread popped it at once and its count wrapped to
// 0xFFFFFFFF (a crash in 0x3023A4 with 64 bots). The hooks at the entries and returns of insert (0x30250D, heap in eax),
// pop (0x302487, eax) and remove (0x30240B, ecx; it calls pop) note which thread is inside each heap: a second thread
// entering while one is inside is the original failure, logged as "xact: two threads in the event heap"; a count over
// the capacity is logged too. The overlap itself is rare (one crash in many runs), so each change is also checked to
// hold the DISPATCH_LEVEL lock (kernel::HoldsDispatchLevel, SDK patch 0039), the rule that keeps the overlap out:
// "xact: event heap changed without the DISPATCH_LEVEL lock". tests/scenarios/regress_xact_dispatch_level.txt expects
// none of them; with CSX_DISPATCH_LOCK=0 (the lock off, for that test's own check) the first change is logged.
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
struct Heap {
    std::atomic<uint32_t> va{0};
    std::atomic<uint32_t> owner{0};  // the thread inside, 0 when none
    uint32_t depth = 0;              // the owner's nested entries (remove calls pop)
};
Heap g_heaps[8];
std::atomic<uint64_t> g_overlaps{0};

thread_local uint32_t t_stack[8];  // the heaps this thread is inside, innermost last
thread_local uint32_t t_depth = 0;

uint32_t Tid() {
    static thread_local uint32_t tid = uint32_t(syscall(SYS_gettid));
    return tid;
}

Heap* Find(uint32_t va) {
    for (Heap& h : g_heaps) {
        uint32_t cur = h.va.load(std::memory_order_acquire);
        if (cur == va) return &h;
        if (cur == 0) {
            uint32_t expected = 0;
            if (h.va.compare_exchange_strong(expected, va) || expected == va) {
                if (expected == 0) LOGI("xact: event heap checks on (heap %08X)", va);
                return &h;
            }
        }
    }
    return nullptr;
}

void Enter(uint32_t va) {
    if (t_depth < 8) t_stack[t_depth] = va;
    t_depth++;
    Heap* h = Find(va);
    if (!h) return;
    uint32_t me = Tid(), expected = 0;
    if (h->owner.load(std::memory_order_relaxed) == me) {
        h->depth++;
    } else if (!h->owner.compare_exchange_strong(expected, me, std::memory_order_acquire)) {
        g_overlaps++;
        LOG_FIRST(8, "xact: two threads in the event heap %08X (thread %u entered while %u was inside; %llu so far)", va,
                  me, expected, (unsigned long long)g_overlaps.load());
    }
    if (!kernel::HoldsDispatchLevel())
        LOG_FIRST(8, "xact: event heap %08X changed without the DISPATCH_LEVEL lock (thread %u)", va, me);
    uint32_t capacity = kernel::R32(va + 4), count = kernel::R32(va + 8);
    if (count > capacity) LOG_FIRST(8, "xact: event heap %08X count %u over its capacity %u", va, count, capacity);
}

void Leave() {
    if (!t_depth) return;
    t_depth--;
    if (t_depth >= 8) return;
    Heap* h = Find(t_stack[t_depth]);
    if (!h || h->owner.load(std::memory_order_relaxed) != Tid()) return;
    if (h->depth)
        h->depth--;
    else
        h->owner.store(0, std::memory_order_release);
}
}  // namespace

// [[hook]] at 0x30250D and 0x302487 (config/game.toml): insert and pop, the heap in eax
void XactHeapEnterEax(X86Context& ctx, uint8_t*) { Enter(ctx.eax); }
// [[hook]] at 0x30240B: remove, the heap in ecx
void XactHeapEnterEcx(X86Context& ctx, uint8_t*) { Enter(ctx.ecx); }
// [[hook]] at the returns: 0x30251E and 0x302560 (insert), 0x30250C (pop), 0x302448 (remove)
void XactHeapLeave(X86Context&, uint8_t*) { Leave(); }
