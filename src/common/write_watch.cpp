// Diagnostic write watch (Linux): CSX_WATCH=VA[:BYTES] write protects the guest pages there and logs every distinct
// host instruction that writes them to watch.log in the run folder (as an offset into the executable, for addr2line), then lets the write go ahead by
// single stepping it with the pages open and protecting them again. Finds the code that fills a guest buffer, e.g. a
// vertex buffer whose contents look wrong. Off unless the variable is set.
#include "common/csx.h"

#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ucontext.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace csx {

namespace {

uintptr_t g_lo = 0, g_hi = 0;          // the protected pages
uintptr_t g_want_lo = 0, g_want_hi = 0;  // the watched bytes
int g_fd = -1;  // watch.log in the run folder
constexpr int kMaxSeen = 256;
uintptr_t g_seen[kMaxSeen];
int g_seen_count = 0;
struct sigaction g_old_segv, g_old_trap;

void Protect(int prot) { mprotect(reinterpret_cast<void*>(g_lo), g_hi - g_lo, prot); }

void OnSegv(int sig, siginfo_t* si, void* ctx) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr);
    if (addr < g_lo || addr >= g_hi) {
        if (g_old_segv.sa_flags & SA_SIGINFO && g_old_segv.sa_sigaction) return g_old_segv.sa_sigaction(sig, si, ctx);
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    auto* uc = static_cast<ucontext_t*>(ctx);
    uintptr_t rip = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
    // the writer and its callers (a host memcpy says little by itself); each distinct call stack logged once
    void* frames[12];
    int count = backtrace(frames, 12);
    frames[0] = reinterpret_cast<void*>(rip);  // in place of this handler
    uintptr_t key = 0;
    for (int i = 0; i < count; i++) key = key * 1000003u ^ uintptr_t(frames[i]);
    bool known = addr < g_want_lo || addr >= g_want_hi;  // other writes to the pages go ahead unlogged
    for (int i = 0; i < g_seen_count; i++) known |= g_seen[i] == key;
    if (!known && g_seen_count < kMaxSeen) {
        g_seen[g_seen_count++] = key;
        // signal context: a fixed buffer written with write(2), not the logger (its lock may be held)
        char line[2048];
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int n = std::snprintf(line, sizeof line, "%ld.%03ld write to +%#lx (now %08X) from", long(now.tv_sec),
                              now.tv_nsec / 1000000, (unsigned long)(addr - g_want_lo),
                              *reinterpret_cast<const uint32_t*>(addr & ~uintptr_t(3)));
        for (int i = 0; i < count && n < int(sizeof line) - 128; i++) {
            Dl_info di{};
            if (dladdr(frames[i], &di) && di.dli_fname) {
                const char* name = std::strrchr(di.dli_fname, '/');
                n += std::snprintf(line + n, sizeof line - n, " %s+%#lx", name ? name + 1 : di.dli_fname,
                                   (unsigned long)(uintptr_t(frames[i]) - uintptr_t(di.dli_fbase)));
            } else {
                n += std::snprintf(line + n, sizeof line - n, " %p", frames[i]);
            }
        }
        n += std::snprintf(line + n, sizeof line - n, "\n");
        if (g_fd >= 0 && write(g_fd, line, size_t(n)) < 0) {}
    }
    Protect(PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap after this instruction
}

void OnTrap(int sig, siginfo_t* si, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    if (!(uc->uc_mcontext.gregs[REG_EFL] & 0x100)) {
        if (g_old_trap.sa_flags & SA_SIGINFO && g_old_trap.sa_sigaction) return g_old_trap.sa_sigaction(sig, si, ctx);
        return;
    }
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    Protect(PROT_READ);
}

}  // namespace

void StartWriteWatch() {
    const char* env = std::getenv("CSX_WATCH");
    if (!env) return;
    char* end = nullptr;
    uint32_t va = uint32_t(std::strtoul(env, &end, 16));
    uint32_t bytes = end && *end == ':' ? uint32_t(std::strtoul(end + 1, nullptr, 0)) : 0x1000;
    uintptr_t lo = reinterpret_cast<uintptr_t>(kernel::HostPtr(va));
    g_want_lo = lo;
    g_want_hi = lo + bytes;
    g_lo = lo & ~uintptr_t(0xFFF);
    g_hi = (lo + bytes + 0xFFF) & ~uintptr_t(0xFFF);
    g_fd = open((app::ExeDir() / "watch.log").string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = OnSegv;
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sa.sa_sigaction = OnTrap;
    sigaction(SIGTRAP, &sa, &g_old_trap);
    Protect(PROT_READ);
    LOGI("watch: guest %08X..+%#x protected (host %#lx..%#lx)", va, bytes, (unsigned long)g_lo, (unsigned long)g_hi);
}

}  // namespace csx

#else

namespace csx {
void StartWriteWatch() {}
}  // namespace csx

#endif
