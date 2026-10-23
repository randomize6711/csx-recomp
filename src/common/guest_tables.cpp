#include "common/guest_tables.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "kernel/memory.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace guest_tables {

namespace {

constexpr uint32_t kPage = mem::kPage;
constexpr uint32_t kGranule = 0x10000;  // the guest allocator's reservation granularity

// mapped tables, for Element() checks and the fault report (written before any guest code runs, then only read)
std::vector<Table> g_mapped;
Region g_region{};

std::string Fmt(const char* f, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return buf;
}

// the fault report: a guest access in a guard page names the table and the element, then the fault proceeds to
// the previous handler (the crash report); in signal context, so a fixed buffer and write(2) / OutputDebugString
void Report(uintptr_t host_addr) {
    uint8_t* base = mem::Base();
    if (!base || host_addr < uintptr_t(base) || host_addr >= uintptr_t(base) + (1ull << 32)) return;
    uint32_t va = uint32_t(host_addr - uintptr_t(base)), past = 0, before = 0;
    const Table* t = GuardOwner(va, &past);
    const Table* next = GuardBefore(va, &before);
    if (!t && !next) return;
    char line[640];
    int n = 0;
    if (t)
        n += std::snprintf(line + n, sizeof(line) - n,
                           "guest table overrun: %s (%u x 0x%X bytes at %08X): access at %08X is %u bytes past its end "
                           "(element %u of %u)\n",
                           t->name, t->count, t->elem, t->va, va, past, t->count + past / t->elem, t->count);
    if (next)
        n += std::snprintf(line + n, sizeof(line) - n,
                           "guest table underrun: %s (%u x 0x%X bytes at %08X): access at %08X is %u bytes before its "
                           "first element\n",
                           next->name, next->count, next->elem, next->va, va, before);
#if defined(_WIN32)
    OutputDebugStringA(line);
    std::fputs(line, stderr);
#else
    if (n > 0) (void)!write(2, line, size_t(n));
#endif
}

#if defined(_WIN32)
LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
        Report(uintptr_t(e->ExceptionRecord->ExceptionInformation[1]));
    return EXCEPTION_CONTINUE_SEARCH;
}
void InstallReport() { AddVectoredExceptionHandler(1, OnException); }
#else
struct sigaction g_old_segv;
void OnSegv(int sig, siginfo_t* si, void* ctx) {
    Report(reinterpret_cast<uintptr_t>(si->si_addr));
    if (g_old_segv.sa_flags & SA_SIGINFO && g_old_segv.sa_sigaction) return g_old_segv.sa_sigaction(sig, si, ctx);
    if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN && !(g_old_segv.sa_flags & SA_SIGINFO))
        return g_old_segv.sa_handler(sig);
    signal(SIGSEGV, SIG_DFL);  // returning re-runs the access, which now ends the process as before
}
void InstallReport() {
    struct sigaction sa{};
    sa.sa_sigaction = OnSegv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
}
#endif

}  // namespace

std::string Validate(const Region& r, const Table* tables, size_t count) {
    uint64_t lo = r.base, hi = uint64_t(r.base) + r.size;
    if (!r.size || r.base % kGranule || r.size % kGranule) return "the region is not on 64 KB boundaries";
    if (lo < mem::kUserLow || hi > mem::kUserHigh) return Fmt("the region %08llX..%08llX is outside the guest user range", lo, hi);
    uint64_t prev_end = lo;  // the end of the previous table's pages
    for (size_t i = 0; i < count; i++) {
        const Table& t = tables[i];
        std::string n = t.name ? t.name : "?";
        if (!t.elem || !t.count) return n + ": empty";
        if (!t.align || (t.align & (t.align - 1)) || t.align > kPage) return n + ": align is not a power of two up to a page";
        uint64_t size = uint64_t(t.elem) * t.count, start = t.va, end = start + size;
        if (start % t.align) return n + Fmt(": %08llX is not aligned to %llu", start, t.align);
        if (end % kPage) return n + Fmt(": it ends at %08llX, not at a page boundary (no guard page right after it)", end);
        uint64_t first_page = start & ~uint64_t(kPage - 1);
        if (first_page < prev_end + kPage) return n + ": it overlaps the previous table or its guard page";
        if (end + kPage > hi) return n + ": it or its guard page is outside the region";
        prev_end = end;
    }
    return "";
}

bool Map(const Region& r, const Table* tables, size_t count) {
    std::string why = Validate(r, tables, count);
    if (!why.empty()) {
        LOGE("guest tables: %s", why.c_str());
        return false;
    }
    mem::RegionInfo info;
    if (!mem::QueryVirtual(r.base, info) || info.state != 0x10000 /* MEM_FREE */ || info.size < r.size) {
        LOGE("guest tables: the region %08X+%X is not free in the guest's user space", r.base, r.size);
        return false;
    }
    uint32_t base = r.base, size = r.size;
    uint32_t st = mem::AllocateVirtual(base, size, mem::kMemReserve, 4 /* PAGE_READWRITE */);
    if (st || base != r.base) {
        LOGE("guest tables: reserving %08X+%X failed (status %08X)", r.base, r.size, st);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const Table& t = tables[i];
        uint32_t first = t.va & ~(kPage - 1), bytes = t.va + t.elem * t.count - first;
        uint32_t a = first, z = bytes;
        st = mem::AllocateVirtual(a, z, mem::kMemCommit, 4);
        if (st) {
            LOGE("guest tables: committing %s at %08X+%X failed (status %08X)", t.name, first, bytes, st);
            uint32_t fb = r.base, fz = 0;
            mem::FreeVirtual(fb, fz, mem::kMemRelease);
            return false;
        }
        std::memset(mem::Base() + first, 0, bytes);
    }
    g_mapped.assign(tables, tables + count);
    g_region = r;
    InstallReport();
    uint64_t committed = 0;
    for (size_t i = 0; i < count; i++) committed += (tables[i].va + uint64_t(tables[i].elem) * tables[i].count) -
                                                    (tables[i].va & ~(kPage - 1));
    LOGI("guest tables: %zu mapped in %08X+%X, %llu KB committed, guard pages after each", count, r.base, r.size,
         (unsigned long long)(committed >> 10));
    return true;
}

uint32_t Element(const Table& t, uint32_t index, const char* caller) {
    if (index < t.count) return t.va + index * t.elem;
    static std::mutex m;
    static std::set<std::pair<const void*, std::string>> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert({&t, caller ? caller : ""}).second)
        LOGE("guest tables: %s asked for element %u of %s, which has %u", caller ? caller : "?", index, t.name, t.count);
    return 0;
}

const Table* GuardOwner(uint32_t va, uint32_t* past_end) {
    for (const Table& t : g_mapped) {
        uint32_t end = t.va + t.elem * t.count;
        if (va >= end && va < end + kPage) {
            if (past_end) *past_end = va - end;
            return &t;
        }
    }
    return nullptr;
}

const Table* GuardBefore(uint32_t va, uint32_t* before_start) {
    for (const Table& t : g_mapped) {
        uint32_t first = t.va & ~(kPage - 1);
        if (va < first && va >= first - kPage) {
            if (before_start) *before_start = t.va - va;
            return &t;
        }
    }
    return nullptr;
}

void Start() {
    if (!kTableCount) return;
    if (!Map(kRegion, kTables, kTableCount)) {
        std::fprintf(stderr, "guest tables could not be mapped (see the log): the program cannot run safely\n");
        std::exit(70);
    }
}

}  // namespace guest_tables
