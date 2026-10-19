// The MCPX audio processor's global sample counter, which the title reads straight from the hardware.
//
// NV_PAPU_XGSCNT (APU registers at 0xFE800000, +0x200C) counts the samples the APU has processed at 48 kHz.
// XACT reads it at 0x003026FD (sub_003026E9) and the XPP section at sub_0031D572; with nothing mapped there the
// read faults. The runtime models no APU registers, so this commits that one page and keeps the counter running
// from the host clock: the APU runs in real time on its own clock, and the counter advances in whole 32 sample
// frames, as xemu models it (mcpx_apu_read: ep_frame_div * NUM_SAMPLES_PER_FRAME).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "common/csx.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace csx {

namespace {
constexpr uint32_t kApuRegs = 0xFE800000;
constexpr uint32_t kXgscnt = kApuRegs + 0x200C;
constexpr uint32_t kSamplesPerFrame = 32;
constexpr uint64_t kRate = 48000;
}  // namespace

void StartApuClock() {
    if (!mem::Commit(kXgscnt & ~0xFFFu, 0x1000)) {
        LOGE("apu: cannot commit the XGSCNT page");
        return;
    }
    auto* counter = reinterpret_cast<std::atomic<uint32_t>*>(mem::Base() + kXgscnt);
    std::thread([counter] {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            uint64_t frames = ns * kRate / kSamplesPerFrame / 1000000000ull;
            counter->store(uint32_t(frames * kSamplesPerFrame), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }).detach();
    LOGI("apu: XGSCNT at %08X runs at %llu Hz", kXgscnt, (unsigned long long)kRate);
}

}  // namespace csx
