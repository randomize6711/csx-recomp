// Logs local player state on request, for control tests (tools/drive.py "state" steps).
//
// A file named state in the run folder holding a name asks for one line: "state <name> x y z yaw clip view_yaw
// view_pitch weapon". The
// addresses were found by diffing memory dumps (FINDINGS.md, "Player control"): the float triple at 0x3AE3E0 moves by
// the run speed while the left stick is held and stays put otherwise; 0x3AE3F0 holds the yaw in degrees (it matches
// the 3D audio listener's forward vector at 0x3038C0); the int at 0x3A2988 drops by one per shot (20 for a full Glock).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "app/app.h"
#include "common/csx.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace csx {

namespace {
constexpr uint32_t kPosition = 0x3AE3E0, kYaw = 0x3AE3F0, kClip = 0x3A2988, kViewYaw = 0x552C10, kViewPitch = 0x552C14;
// the weapon in hand (17 Glock, 29 knife, 4 HE grenade, 25 flashbang, 9 smoke grenade, 6 C4...): the client data of
// the engine's first frame record (static, 0x1B60 bytes each, all of them alike once the weapon has been held a moment)
constexpr uint32_t kWeapon = 0x74B3D8;

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
        const fs::path look = app::ExeDir() / "look";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::error_code ec;
            // look: "RIGHT UP" degrees for the mouse look hook, as a mouse would give them (tools/drive.py "look")
            if (fs::exists(look, ec)) {
                float right = 0, up = 0;
                if (FILE* f = std::fopen(look.string().c_str(), "r")) {
                    if (std::fscanf(f, "%f %f", &right, &up) == 2) {
                        AddLook(right, up);
                        LOGI("look: %g right, %g up", right, up);
                    }
                    std::fclose(f);
                }
                fs::remove(look, ec);
            }
            if (!fs::exists(req, ec)) continue;
            char name[64] = "state";
            if (FILE* f = std::fopen(req.string().c_str(), "r")) {
                if (std::fscanf(f, "%63s", name) != 1) std::snprintf(name, sizeof name, "state");
                std::fclose(f);
            }
            fs::remove(req, ec);
            // then the view yaw and pitch the look routine keeps (0x552C10 / 0x552C14, src/game/mouse_look.cpp) and
            // the weapon in hand
            LOGI("state %s %.3f %.3f %.3f %.3f %d %.3f %.3f %d", name, F(kPosition), F(kPosition + 4), F(kPosition + 8),
                 F(kYaw), int32_t(kernel::R32(kClip)), F(kViewYaw), F(kViewPitch), int32_t(kernel::R32(kWeapon)));
            logx::Flush();
        }
    }).detach();
}

}  // namespace csx
