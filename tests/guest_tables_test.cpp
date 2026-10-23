// Guest tables against the runtime's guest memory (built and run by tests/guest_tables_test.py): the layout
// tools/guest_tables.py generated (test_layout.h) validates and maps, tables are aligned, zeroed and usable to their
// last byte, Element() refuses indices past the end, mapping refuses a region in use, bad layouts are named, and an
// access past a table's end or before its first page faults with the table and element reported.
// Usage: guest_tables_test [case]   (a fault case runs in a child process; the parent checks how it ended)
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/guest_tables.h"
#include "kernel/memory.h"
#include "test_layout.h"

using namespace guest_tables;

static int g_failed = 0;
static void Check(const char* name, bool ok, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, ok || detail.empty() ? "" : ": ", ok ? "" : detail.c_str());
    if (!ok) g_failed++;
}

// runs fn in a child with stderr captured: (how it ended, what it printed)
template <typename F> static std::pair<int, std::string> Child(F fn) {
    int p[2];
    if (pipe(p)) return {-1, ""};
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 2);
        close(p[0]);
        fn();
        _exit(0);
    }
    close(p[1]);
    std::string out;
    char buf[512];
    for (ssize_t n; (n = read(p[0], buf, sizeof(buf))) > 0;) out.append(buf, size_t(n));
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return {st, out};
}

int main() {
    if (!mem::Init()) {
        std::printf("FAIL guest memory\n");
        return 1;
    }
    Check("the generated layout validates", Validate(kRegion, kTables, kTableCount).empty(),
          Validate(kRegion, kTables, kTableCount));

    // layouts that are refused, each with its reason
    Table one = kTables[1];  // align 4
    struct Bad {
        const char* name;
        Region r;
        Table t;
        const char* want;
    } bad[] = {
        {"region off 64 KB", {kRegion.base + 0x1000, kRegion.size}, one, "64 KB"},
        {"region outside the user range", {0x7FF00000u, 0x00200000u}, one, "outside the guest user range"},
        {"misaligned table", kRegion, {one.name, one.va + 2, one.elem, one.count, 4}, "not aligned"},
        {"table not ending at a page", kRegion, {one.name, one.va - 4, one.elem, one.count, 4}, "not at a page boundary"},
        {"no guard page before it", kRegion, {one.name, kRegion.base, 0x1000, 1, 4}, "overlaps"},
        {"guard page outside the region", kRegion, {one.name, kRegion.base + kRegion.size - 0x1000, 0x1000, 1, 4}, "outside the region"},
        {"empty table", kRegion, {one.name, one.va, one.elem, 0, 4}, "empty"},
        {"align not a power of two", kRegion, {one.name, one.va, one.elem, one.count, 12}, "power of two"},
    };
    for (auto& b : bad) {
        std::string why = Validate(b.r, &b.t, 1);
        Check((std::string("refused: ") + b.name).c_str(), why.find(b.want) != std::string::npos, why.empty() ? "accepted" : why);
    }
    // a second table inside the first one's pages, ending at a page boundary: only the overlap is wrong
    Table overlap[] = {kTables[0], {"second", (kTables[0].va & ~(mem::kPage - 1)) + 0x2000, 0x10, 0x100, 4}};
    Check("refused: two tables overlapping", Validate(kRegion, overlap, 2).find("overlaps") != std::string::npos);

    // a region the guest already uses is not taken
    {
        uint32_t base = 0x71000000u, size = 0x10000;
        mem::AllocateVirtual(base, size, mem::kMemReserve, 4);
        Region used{0x71000000u, 0x00100000u};
        Table t{"t", 0x71001000u + 0x1000 - 0x40, 0x10, 4, 4};
        auto [st, out] = Child([&] { std::_Exit(Map(used, &t, 1) ? 0 : 3); });
        Check("refused: a region in use", WIFEXITED(st) && WEXITSTATUS(st) == 3 && out.find("not free") != std::string::npos, out);
    }

    // the generated layout maps
    bool mapped = Map(kRegion, kTables, kTableCount);
    Check("the layout maps", mapped);
    if (!mapped) return 1;
    bool aligned = true, zero = true, ends = true;
    for (size_t i = 0; i < kTableCount; i++) {
        const Table& t = kTables[i];
        aligned &= t.va % t.align == 0;
        ends &= (t.va + t.elem * t.count) % mem::kPage == 0;
        for (uint32_t b = 0; b < t.elem * t.count; b++) zero &= mem::Base()[t.va + b] == 0;
        std::memset(mem::Base() + t.va, 0xA5, t.elem * t.count);  // every byte writable
    }
    Check("every table aligned", aligned);
    Check("every table ends at its guard page", ends);
    Check("tables start zeroed and are writable to their last byte", zero);
    Check("the guard pages are not committed", !mem::IsCommitted(kTables[0].va + kTables[0].elem * kTables[0].count) &&
                                                   !mem::IsCommitted(kRegion.base));
    mem::RegionInfo info;
    Check("the region is the guest allocator's (a later request cannot take it)",
          mem::QueryVirtual(kRegion.base + 0x10, info) && info.alloc_base == kRegion.base);

    // indices
    const Table& t0 = kTables[0];
    Check("Element: the last index", Element(t0, t0.count - 1, "test") == t0.va + (t0.count - 1) * t0.elem);
    auto [est, eout] = Child([&] { std::_Exit(Element(t0, t0.count, "test caller") == 0 ? 0 : 3); });
    Check("Element: one past the end refused and logged", WIFEXITED(est) && WEXITSTATUS(est) == 0 &&
                                                              eout.find("test caller asked for element") != std::string::npos,
          eout);
    Check("Element: a huge index refused", Element(t0, 0xFFFFFFFFu, "test") == 0);

    // faults: one byte past the end, one element past, and before the first page
    struct Fault {
        const char* name;
        uint32_t va;
        const char* want;
    } faults[] = {
        {"overrun by one byte", t0.va + t0.elem * t0.count, "guest table overrun: "},
        {"overrun by one element", t0.va + t0.elem * t0.count + t0.elem - 1, "element "},
        {"underrun before the first page", (t0.va & ~(mem::kPage - 1)) - 1, "guest table underrun: "},
    };
    for (auto& f : faults) {
        auto [st, out] = Child([&] { mem::Base()[f.va] = 1; });
        bool named = out.find(f.want) != std::string::npos && out.find(t0.name) != std::string::npos;
        Check((std::string("fault: ") + f.name).c_str(), WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV && named,
              "status " + std::to_string(st) + ": " + out);
    }
    auto [ost, oout] = Child([&] { mem::Base()[kTables[1].va + 5] = 1; std::_Exit(0); });
    Check("an access inside a table does not fault", WIFEXITED(ost) && WEXITSTATUS(ost) == 0, oout);
    auto [ust, uout] = Child([&] { mem::Base()[0x72000000u] = 1; });
    Check("a fault elsewhere is not reported as a table's", WIFSIGNALED(ust) && uout.find("guest table") == std::string::npos, uout);

    std::printf(g_failed ? "%d failed\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
