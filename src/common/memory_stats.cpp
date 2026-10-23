// Guest memory diagnostics, CSX_LOG_MEMORY=1: every 10 s one log line with the guest's user space (reserved,
// committed, the number of reservations, what is free and its largest block: fragmentation), the 64 MB contiguous
// pool's use (textures, vertex buffers, sound, the host's GPU buffers) and the system pool's, with the peaks so far.
// For the memory architecture work (FINDINGS.md "Guest memory"); off unless the variable is set.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/csx.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace csx {

void StartMemoryStats() {
    const char* v = std::getenv("CSX_LOG_MEMORY");
    if (!v || !*v || *v == '0') return;
    std::thread([] {
        constexpr double kMB = 1.0 / (1 << 20);
        uint64_t peak_committed = 0, peak_contig = 0, peak_reserved = 0, least_largest = ~0ull;
        for (;;) {
            mem::UserSpace u = mem::QueryUserSpace();
            peak_committed = std::max(peak_committed, u.committed);
            peak_reserved = std::max(peak_reserved, u.reserved);
            peak_contig = std::max(peak_contig, u.contiguous_used);
            least_largest = std::min(least_largest, u.largest_free);
            LOGI("memory: user %.1f MB reserved, %.1f MB committed in %u reservations (%08X..%08X), %.1f MB free "
                 "(largest block %.1f MB); contiguous %.1f of %.0f MB; system %.1f MB; peaks: committed %.1f, reserved "
                 "%.1f, contiguous %.1f MB, smallest largest-free %.1f MB",
                 u.reserved * kMB, u.committed * kMB, u.reservations, u.lowest, u.highest, u.free * kMB, u.largest_free * kMB,
                 u.contiguous_used * kMB, (mem::kPhysicalSize - mem::kKernelDataSize) * kMB, u.system_used * kMB,
                 peak_committed * kMB, peak_reserved * kMB, peak_contig * kMB, least_largest * kMB);
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }).detach();
}

}  // namespace csx
