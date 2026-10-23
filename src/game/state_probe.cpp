// Logs local player state on request, for control tests (tools/drive.py "state" steps).
//
// A file named state in the run folder holding a name asks for one line: "state <name> x y z yaw clip view_yaw
// view_pitch weapon"; "NAME SECONDS" follows the position that long first (every 5 ms) and logs "track NAME over S s:
// rose Z at most (highest less lowest), moved D" (jump heights, distances over a fixed real time). The
// addresses were found by diffing memory dumps (FINDINGS.md, "Player control"): the float triple at 0x3AE3E0 moves by
// the run speed while the left stick is held and stays put otherwise; 0x3AE3F0 holds the yaw in degrees (it matches
// the 3D audio listener's forward vector at 0x3038C0); the int at 0x3A2988 drops by one per shot (20 for a full Glock).
// A file named setview holding "X Y Z YAW PITCH" holds the player there (tools/drive.py "setview": a fixed camera for
// picture tests, e.g. over de_aztec's water, which no spawn sees): the server's origin of client 0's edict (svs.clients
// 0x636CE4, client_t::edict +0x4B9C, entvars origin +0x88, velocity +0xA0) written every 20 ms with no velocity, so
// the player neither falls nor drifts, and the view angles the client sends set once; "off" lets go.
// A file named place holding "N X Y Z" puts client N (0 the local player, then the bots; client_t 0x5010 bytes each,
// svs.maxclients at 0x636CE8) at that origin once, with no velocity, and lets it go (tools/drive.py "place": bodies
// put in each other's way for the player collision tests). A file named players holding a name logs one line per
// active client (client_t +0 active): "players NAME N x y z health" (entvars health at edict +0x1E0) and ends with
// "players NAME end".
#include <algorithm>
#include <chrono>
#include <cmath>
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
void WF(uint32_t va, float f) { std::memcpy(kernel::HostPtr(va), &f, 4); }
constexpr uint32_t kClients = 0x636CE4, kClientEdict = 0x4B9C, kEdictOrigin = 0x88, kEdictVelocity = 0xA0;
constexpr uint32_t kMaxClients = 0x636CE8, kClientSize = 0x5010, kEdictHealth = 0x1E0;

// client N's edict, 0 when there is no such active client
uint32_t ClientEdict(uint32_t n) {
    uint32_t clients = kernel::R32(kClients);
    if (!clients || n >= kernel::R32(kMaxClients)) return 0;
    uint32_t c = clients + n * kClientSize;
    return kernel::R32(c) ? kernel::R32(c + kClientEdict) : 0;
}
}  // namespace

void StartStateProbe() {
    std::thread([] {
        namespace fs = std::filesystem;
        const fs::path req = app::ExeDir() / "state";
        const fs::path look = app::ExeDir() / "look";
        const fs::path setview = app::ExeDir() / "setview";
        const fs::path place = app::ExeDir() / "place";
        const fs::path players = app::ExeDir() / "players";
        bool held = false;
        float hold[3] = {};
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
            uint32_t clients = kernel::R32(kClients), edict = clients ? kernel::R32(clients + kClientEdict) : 0;
            if (fs::exists(setview, ec)) {
                float v[5];
                char word[8] = "";
                FILE* f = std::fopen(setview.string().c_str(), "r");
                if (f && std::fscanf(f, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5 && edict) {
                    held = true;
                    std::memcpy(hold, v, sizeof hold);
                    WF(kViewYaw, v[3]);
                    WF(kViewPitch, v[4]);
                    LOGI("setview: %g %g %g, yaw %g pitch %g (edict %08X)", v[0], v[1], v[2], v[3], v[4], edict);
                } else if (f && std::fscanf(f, "%7s", word) == 1 && !std::strcmp(word, "off")) {
                    held = false;
                    LOGI("setview: off");
                } else {
                    LOGW("setview: no player edict or not X Y Z YAW PITCH");
                }
                if (f) std::fclose(f);
                fs::remove(setview, ec);
            }
            if (fs::exists(place, ec)) {
                unsigned n = 0;
                float v[3];
                FILE* f = std::fopen(place.string().c_str(), "r");
                uint32_t e = 0;
                if (f && std::fscanf(f, "%u %f %f %f", &n, &v[0], &v[1], &v[2]) == 4 && (e = ClientEdict(n))) {
                    for (int i = 0; i < 3; i++) {
                        WF(e + kEdictOrigin + i * 4, v[i]);
                        WF(e + kEdictVelocity + i * 4, 0);
                    }
                    LOGI("place: client %u at %g %g %g", n, v[0], v[1], v[2]);
                } else {
                    LOGW("place: no such client or not N X Y Z");
                }
                if (f) std::fclose(f);
                fs::remove(place, ec);
            }
            if (fs::exists(players, ec)) {
                char name[64] = "players";
                if (FILE* f = std::fopen(players.string().c_str(), "r")) {
                    if (std::fscanf(f, "%63s", name) != 1) std::snprintf(name, sizeof name, "players");
                    std::fclose(f);
                }
                fs::remove(players, ec);
                for (uint32_t n = 0, max = kernel::R32(kMaxClients); n < max; n++)
                    if (uint32_t e = ClientEdict(n))
                        LOGI("players %s %u %.3f %.3f %.3f %.0f", name, n, F(e + kEdictOrigin), F(e + kEdictOrigin + 4),
                             F(e + kEdictOrigin + 8), F(e + kEdictHealth));
                LOGI("players %s end", name);
                logx::Flush();
            }
            if (held && edict)
                for (int i = 0; i < 3; i++) {
                    WF(edict + kEdictOrigin + i * 4, hold[i]);
                    WF(edict + kEdictVelocity + i * 4, 0);
                }
            if (!fs::exists(req, ec)) continue;
            char name[64] = "state";
            float track = 0;  // "NAME SECONDS": the position followed that long (every 5 ms) first
            if (FILE* f = std::fopen(req.string().c_str(), "r")) {
                if (std::fscanf(f, "%63s %f", name, &track) < 1) std::snprintf(name, sizeof name, "state");
                std::fclose(f);
            }
            fs::remove(req, ec);
            if (track > 0) {
                float x0 = F(kPosition), y0 = F(kPosition + 4), top = F(kPosition + 8), bottom = top;
                auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(track * 1000));
                while (std::chrono::steady_clock::now() < end) {
                    top = std::max(top, F(kPosition + 8));
                    bottom = std::min(bottom, F(kPosition + 8));
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                float dx = F(kPosition) - x0, dy = F(kPosition + 4) - y0;
                LOGI("track %s over %.2f s: rose %.2f at most, moved %.2f", name, track, top - bottom,
                     std::sqrt(dx * dx + dy * dy));
            }
            // then the view yaw and pitch the look routine keeps (0x552C10 / 0x552C14, src/game/mouse_look.cpp) and
            // the weapon in hand
            LOGI("state %s %.3f %.3f %.3f %.3f %d %.3f %.3f %d", name, F(kPosition), F(kPosition + 4), F(kPosition + 8),
                 F(kYaw), int32_t(kernel::R32(kClip)), F(kViewYaw), F(kViewPitch), int32_t(kernel::R32(kWeapon)));
            logx::Flush();
        }
    }).detach();
}

}  // namespace csx
