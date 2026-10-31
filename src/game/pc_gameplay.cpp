// Gameplay options (N7, docs/n7_gameplay.md):
//   - Flashbang Effect (PC Options > Gameplay, pc_settings.ini flashbang_colour white|black): a flashbang blinds with
//     the server's ScreenFade (CS's: full white, alpha 255, the fade's time by how hard it hit). The engine's
//     handler (V_ScreenFade 0xB6500: duration, hold time, flags, r g b a into the fade at 0x74A244..0x74A258) is
//     hooked once it copied the colour (0xB6557): with Black a white fade (255 255 255, alpha over 200) is made black,
//     the time and strength unchanged. It is this player's view only (the server and the other players unchanged).
//   - Gravity (the match's rules: Custom Game's Match Options, the party's Rules, csx-server's gravity, the console):
//     "pc_gravity N" sets the server's sv_gravity, which the engine sends every client with the move variables (the
//     server's is the one that counts; a client's prediction takes it from there).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;
constexpr uint32_t kFadeColour = 0x74A254;  // screenfade r, g, b, alpha (bytes)
constexpr uint32_t kClients = 0x636CE4, kMaxClients = 0x636CE8, kClientSize = 0x5010, kClientEdict = 0x4B9C,
                   kClientName = 0x4D10;
constexpr uint32_t kEffects = 0x80 + 0x118;  // the edict's entvars effects (EF_DIMLIGHT 8: the flashlight)
constexpr uint32_t kDeadFlag = 0x80 + 0x170;  // the edict's entvars deadflag
constexpr uint32_t kNextDecal = 0x854;       // the player's next spray time (the spray impulse sets it, 0x120CE0)
}  // namespace

// the server, every frame (pc_modes.cpp's Think hook): a player's flashlight turned on or off and a spray, logged
// (what the tests see of them; the flashlight's light and the logo are the clients' drawing)
void PcGameplayThink() {
    static uint32_t effects[65] = {}, edicts[65] = {};
    static float decal[65] = {};
    uint32_t n = std::min<uint32_t>(R32(kMaxClients), 65);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t client = R32(kClients) + i * kClientSize;
        uint32_t edict = R32(client) ? R32(client + kClientEdict) : 0;
        uint32_t player = edict ? R32(edict + 0x7C) : 0;
        if (!player) {
            edicts[i] = 0;
            continue;
        }
        uint32_t fx = R32(edict + kEffects);
        // a dead player's flashlight goes off (it is not kept through the death, and a spawn starts with it off)
        if ((fx & 8) && R32(edict + kDeadFlag) != 0) {
            fx &= ~8u;
            kernel::W32(edict + kEffects, fx);
            LOG_FIRST(16, "pc gameplay: %s's flashlight off (dead)", kernel::ReadCString(client + kClientName, 32).c_str());
        }
        float next = kernel::G<float>(player + kNextDecal);
        if (edicts[i] == edict) {
            if ((fx ^ effects[i]) & 8)
                LOG_FIRST(64, "pc gameplay: %s's flashlight %s", kernel::ReadCString(client + kClientName, 32).c_str(),
                          fx & 8 ? "on" : "off");
            if (next > decal[i])
                LOG_FIRST(64, "pc gameplay: %s sprayed a logo", kernel::ReadCString(client + kClientName, 32).c_str());
        }
        edicts[i] = edict;
        effects[i] = fx;
        decal[i] = next;
    }
}

// [[hook]] at 0xB6557 (config/game.toml) in V_ScreenFade, the fade's colour just copied
void PcFlashFade(X86Context&, uint8_t*) {
    static unsigned seen = ~0u;
    static bool black = false;
    if (pc::Version() != seen) {
        seen = pc::Version();
        black = pc::Get("flashbang_colour", "white") == "black";
    }
    auto* c = static_cast<uint8_t*>(kernel::HostPtr(kFadeColour));
    if (!black || c[0] != 255 || c[1] != 255 || c[2] != 255 || c[3] <= 200) return;
    c[0] = c[1] = c[2] = 0;
    LOG_FIRST(8, "pc gameplay: a flashbang's white fade drawn black (alpha %d)", c[3]);
}

// "pc_gravity N" (pc_menu.cpp's PcConsoleCommand): the match's gravity, 50..4000 (800 the original's)
void PcGravityCommand(const std::string& arg) {
    char* end = nullptr;
    long g = std::strtol(arg.c_str(), &end, 10);
    if (arg.empty() || (end && *end) || g < 50 || g > 4000) {
        LOGW("pc gameplay: pc_gravity takes 50 to 4000, not \"%s\"", arg.c_str());
        return;
    }
    csx::QueueEngineCommand("sv_gravity " + std::to_string(g) + "\n");
    LOGI("pc gameplay: gravity %ld%s", g, g == 800 ? " (normal)" : "");
}

// Death Camera (PC Options > Gameplay, pc_settings.ini death_camera original|first_person): the client's view after
// its player dies. The original's (Counter-Strike's) leaves the eyes for a camera behind the body and then the
// spectator's views. First Person keeps the dead player's own eyes for kDeathView seconds: they sink to the floor as
// the body falls and the view rolls onto its side (Half-Life's dead view), then the original's camera takes over. The
// engine's ClientDLL_CalcRefdef (0xB5F20: the client's V_CalcRefdef 0x1A0F10 on the engine's ref_params at esp+0x10)
// is hooked after the call (0xB5F39): while alive the eyes are kept; once dead the view is set from them. Only this
// player's own view changes: nothing is sent to the server.
namespace {
constexpr float kDeathView = 3.0f, kFall = 0.6f, kDrop = 34.f, kRoll = 70.f;
}  // namespace

// [[hook]] at 0xB5F39 (config/game.toml)
void PcDeathCamera(X86Context& ctx, uint8_t*) {
    static unsigned seen = ~0u;
    static bool first = false;
    static float eyes[3], angles[3], dead_since = -1;
    static bool alive_seen = false;
    if (pc::Version() != seen) {
        seen = pc::Version();
        first = pc::Get("death_camera", "original") == "first_person";
    }
    uint32_t p = ctx.esp + 0x10;
    auto F = [&](uint32_t off) -> float& { return kernel::G<float>(p + off); };
    int32_t health = int32_t(R32(p + 0x8C));
    float time = F(0x40);
    if (R32(p + 0x44)) {  // intermission
        dead_since = -1;
        return;
    }
    if (health > 0) {
        for (int i = 0; i < 3; i++) eyes[i] = F(4 * i), angles[i] = F(0xC + 4 * i);
        alive_seen = true;
        dead_since = -1;
        return;
    }
    if (!first || !alive_seen) return;
    if (dead_since < 0 || time < dead_since) {
        dead_since = time;
        LOGI("pc gameplay: death camera: first person for %.1f s", kDeathView);
    }
    float t = time - dead_since;
    if (t >= kDeathView) return;  // the original's camera from here
    float k = std::min(1.f, t / kFall);
    k = 1 - (1 - k) * (1 - k);  // eased: fast at first, as a fall
    F(0) = eyes[0];
    F(4) = eyes[1];
    F(8) = eyes[2] - kDrop * k;
    F(0xC) = angles[0] * (1 - k);  // the pitch levels out on the floor
    F(0x10) = angles[1];
    F(0x14) = kRoll * k;
}
