// Logs local player state on request, for control tests (tools/drive.py "state" steps).
//
// A file named state in the run folder holding a name asks for one line: "state <name> x y z yaw clip". The
// addresses were found by diffing memory dumps (FINDINGS.md, "Player control"): the float triple at 0x3AE3E0 moves by
// the run speed while the left stick is held and stays put otherwise; 0x3AE3F0 holds the yaw in degrees (it matches
// the 3D audio listener's forward vector at 0x3038C0); the int at 0x3A2988 drops by one per shot (20 for a full Glock).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace csx {

namespace {
constexpr uint32_t kPosition = 0x3AE3E0, kYaw = 0x3AE3F0, kClip = 0x3A2988;

float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
}  // namespace

void StartStateProbe() {
    std::thread([] {
        namespace fs = std::filesystem;
        const fs::path req = app::ExeDir() / "state";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::error_code ec;
            if (!fs::exists(req, ec)) continue;
            char name[64] = "state";
            if (FILE* f = std::fopen(req.string().c_str(), "r")) {
                if (std::fscanf(f, "%63s", name) != 1) std::snprintf(name, sizeof name, "state");
                std::fclose(f);
            }
            fs::remove(req, ec);
            LOGI("state %s %.3f %.3f %.3f %.3f %d", name, F(kPosition), F(kPosition + 4), F(kPosition + 8), F(kYaw),
                 int32_t(kernel::R32(kClip)));
            logx::Flush();
        }
    }).detach();
}

}  // namespace csx
