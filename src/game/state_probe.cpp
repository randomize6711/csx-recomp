// Logs local player state on request, for control tests (tools/drive.py "state" steps).
//
// A file named state in the run folder holding a name asks for one line: "state <name> x y z yaw clip view_yaw
// view_pitch weapon"; "NAME SECONDS" follows the position that long first (every 5 ms) and logs "track NAME over S s:
// rose Z at most (highest less lowest), moved D" (jump heights, distances over a fixed real time). The
// addresses were found by diffing memory dumps (FINDINGS.md, "Player control"): the float triple at 0x3AE3E0 moves by
// the run speed while the left stick is held and stays put otherwise; 0x3AE3F0 holds the yaw in degrees (it matches
// the 3D audio listener's forward vector at 0x3038C0); the int at 0x3A2988 drops by one per shot (20 for a full Glock).
// The triple and the yaw are predicted_players[0]'s origin and angles (+0x10, +0x20), which G8.2B moved to the guest
// table cl_predicted_players: they are read there.
// A file named setview holding "X Y Z YAW PITCH" holds the player there (tools/drive.py "setview": a fixed camera for
// picture tests, e.g. over de_aztec's water, which no spawn sees): the server's origin of client 0's edict (svs.clients
// 0x636CE4, client_t::edict +0x4B9C, entvars origin +0x88, velocity +0xA0) written every 20 ms with no velocity, so
// the player neither falls nor drifts, and the view angles the client sends set once; "off" lets go.
// A file named place holding "N X Y Z" puts client N (0 the local player, then the bots; client_t 0x5010 bytes each,
// svs.maxclients at 0x636CE8) at that origin once, with no velocity, and lets it go (tools/drive.py "place": bodies
// put in each other's way for the player collision tests). A file named players holding a name logs one line per
// active client (client_t +0 active): "players NAME N x y z health" (entvars health at edict +0x1E0), for a network
// client also "playerping NAME N MS", and ends with "players NAME end". A file named spawns holding a name logs the map's spawn points and the edicts in use: "spawns
// NAME ct C t T, edicts N of M, maxclients K" (info_player_start the Counter-Terrorists', info_player_deathmatch the
// Terrorists'; the classname at entvars +0, a string_t from the base 0x329F2B; sv.num_edicts 0x62B418,
// sv.max_edicts 0x62B41C, sv.edicts 0x62B420, 0x318 bytes each).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "app/app.h"
#include "common/csx.h"
#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"

namespace csx {

namespace {

// predicted_players (G8.2B moved them to a guest table) and the local player's entry in it: its player number
// (cl.playernum 0x771224: 0 on a listen server's own player, the client's slot on a network client)
uint32_t Predicted() { return guest_tables::Production(guest_tables::k_cl_predicted_players).va; }
uint32_t Local() { return Predicted() + std::min<uint32_t>(kernel::R32(0x771224), 64) * 0x28; }
uint32_t Position() { return Local() + 0x10; }
uint32_t Yaw() { return Local() + 0x20; }
constexpr uint32_t kClip = 0x3A2988, kViewYaw = 0x552C10, kViewPitch = 0x552C14;
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
constexpr uint32_t kEdictViewAngle = 0xF4;  // entvars v_angle (pitch, yaw, roll)
// the client's last parsed frame (cl.parsecountmod), its player number and maxclients (src/game/player_slots.cpp)
constexpr uint32_t kParseCountMod = 0x74A064, kPlayerNum = 0x771224, kClMaxClients = 0x778A5C;
constexpr uint32_t kStateOrigin = 0x44;  // a frame player state's origin (0xFB bytes each)
constexpr uint32_t kNumEdicts = 0x62B418, kMaxEdicts = 0x62B41C, kEdicts = 0x62B420, kEdictSize = 0x318,
                   kEdictClassname = 0x80;
constexpr uint32_t kStringBase = 0x329F2B;  // string_t's base (STRING(x) = base + x): the player edict's classname
                                            // 0x1D6A9 is the static "player" at 0x3475D4

// the guest's NUL-terminated string at va, empty when va is 0
std::string GuestString(uint32_t va) {
    std::string s;
    for (uint32_t i = 0; va && i < 64; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va + i))[0];
        if (!c) break;
        s += c;
    }
    return s;
}

// the host's player's slot: 0 on a listen server (its own player); on a local server the game started (N4.7), the
// owner's (the bots joined before it)
uint32_t HostSlot() {
    if (!csx::OwnedServer()) return 0;
    int slot = csx::OwnerSlot();
    return slot < 0 ? ~0u : uint32_t(slot);
}

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
            uint32_t edict = ClientEdict(HostSlot());
            if (fs::exists(setview, ec)) {
                float v[5];
                char word[8] = "";
                FILE* f = std::fopen(setview.string().c_str(), "r");
                bool five = f && std::fscanf(f, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5;
                if (five && edict) {
                    held = true;
                    std::memcpy(hold, v, sizeof hold);
                    WF(kViewYaw, v[3]);
                    WF(kViewPitch, v[4]);
                    LOGI("setview: %g %g %g, yaw %g pitch %g (edict %08X)", v[0], v[1], v[2], v[3], v[4], edict);
                } else if (five && csx::NetClientOnly()) {
                    // a client with no server of its own (a local server's game, N4.7): its view; the server's probe
                    // holds the position (tools/drive.py sends both)
                    WF(kViewYaw, v[3]);
                    WF(kViewPitch, v[4]);
                    LOGI("setview: view yaw %g pitch %g (the server holds the position)", v[3], v[4]);
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
                    if (uint32_t e = ClientEdict(n)) {
                        LOGI("players %s %u %.3f %.3f %.3f %.0f", name, n, F(e + kEdictOrigin), F(e + kEdictOrigin + 4),
                             F(e + kEdictOrigin + 8), F(e + kEdictHealth));
                        // the server's measured latency of a network client (client_t +0x25A0, seconds: the
                        // scoreboard's ping), "playerping NAME N MS"
                        uint32_t cl = kernel::R32(kClients) + n * kClientSize;
                        if (kernel::R32(cl + 0x20 + 4) == 3) LOGI("playerping %s %u %.0f", name, n, F(cl + 0x25A0) * 1000);
                    }
                LOGI("players %s end", name);
                logx::Flush();
            }
            // bring: "N DIST" puts client N DIST units in front of client 0 (the host's player), along its view, on
            // the server, with no velocity (tools/drive.py "bring": two players facing each other for the network tests)
            const fs::path bring = app::ExeDir() / "bring";
            if (fs::exists(bring, ec)) {
                unsigned n = 0;
                float dist = 0;
                FILE* f = std::fopen(bring.string().c_str(), "r");
                uint32_t e = 0, host = ClientEdict(HostSlot());
                if (f && std::fscanf(f, "%u %f", &n, &dist) == 2 && (e = ClientEdict(n)) && host) {
                    float yaw = F(host + kEdictViewAngle + 4) * 3.14159265f / 180;
                    float to[3] = {F(host + kEdictOrigin) + dist * std::cos(yaw), F(host + kEdictOrigin + 4) + dist * std::sin(yaw),
                                   F(host + kEdictOrigin + 8)};
                    for (int i = 0; i < 3; i++) {
                        WF(e + kEdictOrigin + i * 4, to[i]);
                        WF(e + kEdictVelocity + i * 4, 0);
                    }
                    LOGI("bring: client %u to %g %g %g, %g in front of client 0", n, to[0], to[1], to[2], dist);
                } else {
                    LOGW("bring: no such client or not N DIST");
                }
                if (f) std::fclose(f);
                fs::remove(bring, ec);
            }
            // aim: the view turned to the nearest other player this client knows (its last frame's player states,
            // cl_frame_playerstates: the origin at +0x44, found against the server's), as a player would aim
            // (tools/drive.py "aim")
            const fs::path aim = app::ExeDir() / "aim";
            if (fs::exists(aim, ec)) {
                fs::remove(aim, ec);
                const auto& t = guest_tables::Production(guest_tables::k_cl_frame_playerstates);
                uint32_t frame = kernel::R32(kParseCountMod) & 15, me = kernel::R32(kPlayerNum),
                         max = std::min<uint32_t>(kernel::R32(kClMaxClients), 65);
                float own[3] = {F(Position()), F(Position() + 4), F(Position() + 8)};
                float best = 1e30f, target[3] = {};
                int who = -1;
                for (uint32_t i = 0; t.count && i < max; i++) {
                    if (i == me) continue;
                    uint32_t st = t.va + frame * t.elem + i * 0xFB;
                    float o[3] = {F(st + kStateOrigin), F(st + kStateOrigin + 4), F(st + kStateOrigin + 8)};
                    if (o[0] == 0 && o[1] == 0 && o[2] == 0) continue;
                    float d = std::hypot(o[0] - own[0], o[1] - own[1], o[2] - own[2]);
                    if (d < best) best = d, who = int(i), std::memcpy(target, o, sizeof target);
                }
                if (who < 0) {
                    LOGW("aim: no other player known");
                } else {
                    float dx = target[0] - own[0], dy = target[1] - own[1], dz = target[2] - own[2];
                    float yaw = std::atan2(dy, dx) * 180 / 3.14159265f;
                    float pitch = -std::atan2(dz, std::hypot(dx, dy)) * 180 / 3.14159265f;
                    WF(kViewYaw, yaw);
                    WF(kViewPitch, pitch);
                    LOGI("aim: player %d at %g %g %g, %g away: yaw %g pitch %g", who + 1, target[0], target[1], target[2],
                         best, yaw, pitch);
                }
                logx::Flush();
            }
            const fs::path spawns = app::ExeDir() / "spawns";
            if (fs::exists(spawns, ec)) {
                char name[64] = "spawns";
                if (FILE* f = std::fopen(spawns.string().c_str(), "r")) {
                    if (std::fscanf(f, "%63s", name) != 1) std::snprintf(name, sizeof name, "spawns");
                    std::fclose(f);
                }
                fs::remove(spawns, ec);
                uint32_t n = kernel::R32(kNumEdicts), base = kernel::R32(kEdicts), ct = 0, tt = 0;
                for (uint32_t i = 0; base && i < n; i++) {
                    uint32_t e = base + i * kEdictSize;
                    if (kernel::R32(e)) continue;  // free
                    uint32_t cls_id = kernel::R32(e + kEdictClassname);
                    std::string cls = cls_id ? GuestString(kStringBase + cls_id) : std::string();
                    ct += cls == "info_player_start";
                    tt += cls == "info_player_deathmatch";
                }
                LOGI("spawns %s ct %u t %u, edicts %u of %u, maxclients %u", name, ct, tt, n, kernel::R32(kMaxEdicts),
                     kernel::R32(kMaxClients));
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
                float x0 = F(Position()), y0 = F(Position() + 4), top = F(Position() + 8), bottom = top;
                auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(track * 1000));
                while (std::chrono::steady_clock::now() < end) {
                    top = std::max(top, F(Position() + 8));
                    bottom = std::min(bottom, F(Position() + 8));
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                float dx = F(Position()) - x0, dy = F(Position() + 4) - y0;
                LOGI("track %s over %.2f s: rose %.2f at most, moved %.2f", name, track, top - bottom,
                     std::sqrt(dx * dx + dy * dy));
            }
            // then the view yaw and pitch the look routine keeps (0x552C10 / 0x552C14, src/game/mouse_look.cpp) and
            // the weapon in hand
            LOGI("state %s %.3f %.3f %.3f %.3f %d %.3f %.3f %d", name, F(Position()), F(Position() + 4), F(Position() + 8),
                 F(Yaw()), int32_t(kernel::R32(kClip)), F(kViewYaw), F(kViewPitch), int32_t(kernel::R32(kWeapon)));
            logx::Flush();
        }
    }).detach();
}

}  // namespace csx
