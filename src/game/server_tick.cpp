// The server's timing (docs/pc_multiplayer.md "N2 timing"): when each client gets its next snapshot.
//
// SV_SendClientMessages (0x9E3F0) sends a client a snapshot on a host frame where realtime + host_frametime reaches
// its next_messagetime (client +0x3580, 0x9E495), and then sets next_messagetime = realtime + host_frametime +
// 1/cl_updaterate (+0x3588; 0x9E5AD..0x9E5CA, the same on its choke path 0x9E626..0x9E64F). That assumes server
// frames much shorter than the interval (GoldSrc's dedicated servers ran 100 or more a second, listen servers up to
// 1000): with a 30 Hz tick and 30 updates a second the next frame lands exactly on the deadline and jitter decides,
// so about every other frame was skipped (N1 measured 17-21 snapshots a second against 30 asked). The deadline now
// advances by the interval from the previous one (a schedule, not "interval after now"), so each client gets its rate
// on average whenever the tick allows it; a schedule that fell more than an interval behind (a long frame, the first
// snapshot) restarts from now rather than sending a burst.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kHostClient = 0x6CDFEC;  // the client SV_SendClientMessages is at
constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010;
constexpr uint32_t kRealtime = 0x6CE040;    // double
constexpr uint32_t kNextMessage = 0x3580, kMessageInterval = 0x3588;  // doubles in client_t

double R64(uint32_t a) {
    double d;
    std::memcpy(&d, kernel::HostPtr(a), 8);
    return d;
}
void W64(uint32_t a, double d) {
    std::memcpy(kernel::HostPtr(a), &d, 8);
}

double g_old_next = 0;  // the client's deadline before the engine replaced it
uint32_t g_old_client = 0;

// CSX_LOG_NET=1: snapshots a second per network client, once a second
std::vector<unsigned> g_sent;
std::chrono::steady_clock::time_point g_second = std::chrono::steady_clock::now();

void Count(uint32_t cl) {
    static const bool on = std::getenv("CSX_LOG_NET") && *std::getenv("CSX_LOG_NET") == '1';
    if (!on) return;
    uint32_t base = kernel::R32(kClients), n = kernel::R32(kMaxClients);
    if (cl < base || n == 0 || n > 256) return;
    uint32_t index = (cl - base) / kClientSize;
    if (g_sent.size() != n) g_sent.assign(n, 0);
    if (index < n) g_sent[index]++;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - g_second).count();
    if (dt < 1.0) return;
    g_second = now;
    unsigned lo = ~0u, hi = 0, total = 0, clients = 0;
    for (unsigned s : g_sent) {
        if (!s) continue;
        lo = std::min(lo, s);
        hi = std::max(hi, s);
        total += s;
        clients++;
    }
    if (clients)
        LOGI("net stats: snapshots a second per client: %.1f mean, %.1f to %.1f (%u clients)", total / dt / clients,
             lo / dt, hi / dt, clients);
    std::fill(g_sent.begin(), g_sent.end(), 0u);
}

void Schedule(bool sent) {
    uint32_t cl = kernel::R32(kHostClient);
    if (cl != g_old_client) return;
    double now = R64(kRealtime), interval = R64(cl + kMessageInterval);
    double next = g_old_next + interval;
    if (next < now - interval || next > now + 2 * interval) next = now + interval;  // behind, first, or a new rate
    W64(cl + kNextMessage, next);
    if (sent && kernel::R32(cl + 0x20 + 4) == 3) Count(cl);  // a network client (its netchan address NA_IP), not a bot
}
}  // namespace

// [[hook]] at 0x9E5AD and 0x9E61E (SV_SendClientMessages about to compute host_client's next_messagetime, esi the
// client): its deadline before
void SnapshotKeep(X86Context& ctx, uint8_t*) {
    g_old_client = ctx.esi;
    g_old_next = R64(ctx.esi + kNextMessage);
}

// [[hook]] at 0x9E5D0 (just after the store, a snapshot about to be sent) and 0x9E655 (the choke path's: the client's
// rate allowed nothing this frame): the deadline one interval after the previous one
void SnapshotSchedule(X86Context&, uint8_t*) {
    Schedule(true);
}
void SnapshotScheduleChoked(X86Context&, uint8_t*) {
    Schedule(false);
}
