// CSX_PROFILE=<file> (FINDINGS.md "High population performance", for benchmarks): a sampling profiler. SIGPROF every
// millisecond of the process's CPU time interrupts the thread that used it; the handler unwinds the host stack
// (_Unwind_Backtrace on the eh_frame tables, which the generated code has at -O1 / -O2 as well) and stores the program
// counters. The generated functions are named after their guest address (sub_00XXXXXX), and a guest call is a host
// call, so the host stack is the guest call stack: tools/profile_report.py symbolises the file with the executable's
// symbol table and gives each function's own and inclusive share, per thread. A writer thread drains the samples to
// the file and once a second appends each thread's name and CPU time (/proc/self/task/<tid>/stat), the per-thread
// CPU use. CSX_PROFILE_RATE: samples a second of CPU time (default 1000; 0 writes the thread times alone, for clean
// benchmarks). Off unless set: the signal interrupts sleeps (SA_RESTART restarts most system calls, not all).
#include <unwind.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "core/log.h"

namespace {
constexpr int kDepth = 96;
constexpr size_t kSlots = 1 << 14;  // samples in flight (13 MB): the writer drains every 50 ms, 1000 a second arrive

struct Sample {
    std::atomic<uint32_t> ready;
    uint32_t tid, depth;
    uint64_t ns;
    uint64_t pc[kDepth];
};

Sample* g_ring;
std::atomic<uint64_t> g_head{0};
std::atomic<uint64_t> g_lost{0};
std::chrono::steady_clock::time_point g_t0;

struct Walk {
    uint64_t* pc;
    uint32_t n;
};

_Unwind_Reason_Code Step(_Unwind_Context* uc, void* arg) {
    auto* w = static_cast<Walk*>(arg);
    if (w->n >= kDepth) return _URC_END_OF_STACK;
    w->pc[w->n++] = _Unwind_GetIP(uc);
    return _URC_NO_REASON;
}

void OnProf(int, siginfo_t*, void*) {
    int saved = errno;
    uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    Sample& s = g_ring[i % kSlots];
    if (s.ready.load(std::memory_order_acquire)) {  // the writer is behind: drop this one
        g_lost.fetch_add(1, std::memory_order_relaxed);
        errno = saved;
        return;
    }
    s.tid = uint32_t(syscall(SYS_gettid));
    s.ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_t0).count());
    Walk w{s.pc, 0};
    _Unwind_Backtrace(Step, &w);
    s.depth = w.n;
    s.ready.store(1, std::memory_order_release);
    errno = saved;
}

void ThreadTimes(FILE* f, uint64_t ns) {
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    long tick = sysconf(_SC_CLK_TCK);
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[64], comm[64] = "", stat[1024];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        if (FILE* c = fopen(path, "r")) {
            if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE* c = fopen(path, "r");
        if (!c) continue;
        size_t n = fread(stat, 1, sizeof stat - 1, c);
        fclose(c);
        stat[n] = 0;
        const char* p = strrchr(stat, ')');  // fields after the name: state is 3, utime 14, stime 15
        unsigned long ut = 0, st = 0;
        if (p) sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
        fprintf(f, "T %llu %s %.3f %s\n", (unsigned long long)ns, e->d_name, double(ut + st) / tick, comm);
    }
    closedir(d);
}

void Writer(FILE* f) {
    // after a second main has run and the log's clock exists: this line gives the log's time of a sample time
    std::this_thread::sleep_for(std::chrono::seconds(1));
    LOGI("profiler: sample clock %.6f", std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count());
    uint64_t tail = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        for (;;) {
            Sample& s = g_ring[tail % kSlots];
            if (!s.ready.load(std::memory_order_acquire)) break;
            fprintf(f, "S %llu %u", (unsigned long long)s.ns, s.tid);
            for (uint32_t k = 0; k < s.depth; k++) fprintf(f, " %llx", (unsigned long long)s.pc[k]);
            fputc('\n', f);
            s.ready.store(0, std::memory_order_release);
            tail++;
        }
        auto now = std::chrono::steady_clock::now();
        // the shared libraries' mappings (the report names their code by library): after the renderer has loaded its
        // driver, and again later
        static int maps_written = 0;
        double run_s = std::chrono::duration<double>(now - g_t0).count();
        if ((maps_written == 0 && run_s > 10) || (maps_written == 1 && run_s > 60)) {
            maps_written++;
            if (FILE* m = fopen("/proc/self/maps", "r")) {
                char line[512];
                while (fgets(line, sizeof line, m))
                    if (strstr(line, " r-xp ") || strstr(line, " r-x")) fprintf(f, "M %s", line);
                fclose(m);
            }
        }
        if (now >= next) {
            next = now + std::chrono::seconds(1);
            ThreadTimes(f, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_t0).count()));
            if (uint64_t lost = g_lost.exchange(0)) fprintf(f, "L %llu\n", (unsigned long long)lost);
            fflush(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

struct Start {
    Start() {
        const char* path = std::getenv("CSX_PROFILE");
        if (!path || !*path) return;
        FILE* f = fopen(path, "w");
        if (!f) return;
        g_ring = static_cast<Sample*>(mmap(nullptr, sizeof(Sample) * kSlots, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
        if (g_ring == MAP_FAILED) return;
        // the load address: the report subtracts this function's symbol from it
        g_t0 = std::chrono::steady_clock::now();
        fprintf(f, "A %p\n", reinterpret_cast<void*>(&OnProf));
        std::thread(Writer, f).detach();
        const char* rate_s = std::getenv("CSX_PROFILE_RATE");
        long rate = rate_s && *rate_s ? std::atol(rate_s) : 1000;
        if (rate <= 0) return;
        struct sigaction sa {};
        sa.sa_sigaction = OnProf;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        long us = rate >= 1000000 ? 1 : 1000000 / rate;
        itimerval it{{0, us}, {0, us}};
        setitimer(ITIMER_PROF, &it, nullptr);
    }
} g_start;
}  // namespace
