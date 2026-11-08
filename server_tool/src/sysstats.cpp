#include "sysstats.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace tool {

namespace {
double WallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void ProcessMeter::Sample(long long pid) {
    if (pid != pid_) {
        pid_ = pid;
        last_cpu_s_ = -1;
        cpu_percent.reset();
        memory_mb.reset();
        threads.reset();
    }
    if (pid <= 0) return;
    double cpu_s = -1;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(pid));
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (h) {
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            auto v = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
            cpu_s = v(k) + v(u);
        }
        PROCESS_MEMORY_COUNTERS m{};
        if (GetProcessMemoryInfo(h, &m, sizeof(m))) memory_mb = double(m.WorkingSetSize) / (1024.0 * 1024.0);
        CloseHandle(h);
    }
#else
    // /proc/PID/stat: utime (14) and stime (15) in clock ticks, num_threads (20); the name (2) may hold spaces, so
    // the fields are counted after its closing parenthesis
    std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
    std::string text((std::istreambuf_iterator<char>(st)), {});
    size_t close = text.rfind(')');
    if (close != std::string::npos) {
        std::istringstream f(text.substr(close + 2));
        std::string field;
        double utime = 0, stime = 0;
        int n = 3;
        while (f >> field) {
            if (n == 14) utime = std::atof(field.c_str());
            if (n == 15) stime = std::atof(field.c_str());
            if (n == 20) threads = std::atoi(field.c_str());
            if (n == 20) break;
            n++;
        }
        cpu_s = (utime + stime) / double(sysconf(_SC_CLK_TCK));
    }
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("VmRSS:", 0) == 0) memory_mb = std::atof(line.c_str() + 6) / 1024.0;  // kB
#endif
    double wall = WallSeconds();
    if (cpu_s >= 0 && last_cpu_s_ >= 0 && wall > last_wall_s_)
        cpu_percent = 100.0 * (cpu_s - last_cpu_s_) / (wall - last_wall_s_);
    if (cpu_s >= 0) last_cpu_s_ = cpu_s, last_wall_s_ = wall;
}

}  // namespace tool
