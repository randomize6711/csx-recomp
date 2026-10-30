#include "linux/pad_style.h"

#include <cstring>
#include <map>
#include <string>

#include "core/log.h"

namespace padstyle {

namespace {
constexpr uint16_t kValve = 0x28DE, kMicrosoft = 0x045E, kNintendo = 0x057E;

bool Has(const char* name, const char* part) { return name && std::strstr(name, part); }
}  // namespace

const char* Name(int style) {
    static const char* const kNames[] = {"pc", "xbox", "x360", "series", "ps", "dualsense", "switch", "steam", "wii"};
    return style >= 0 && style < kCount ? kNames[style] : "x360";
}

bool SteamVirtual(SDL_JoystickID id) {
    return SDL_GetGamepadVendorForID(id) == kValve && SDL_GetGamepadProductForID(id) == 0x11FF;
}

bool SteamPhysical(SDL_JoystickID id) { return SDL_GetGamepadVendorForID(id) == kValve && !SteamVirtual(id); }

int Of(SDL_JoystickID id) {
    static std::map<SDL_JoystickID, int> known;
    if (auto it = known.find(id); it != known.end()) return it->second;
    uint16_t vendor = SDL_GetGamepadVendorForID(id), product = SDL_GetGamepadProductForID(id);
    const char* name = SDL_GetGamepadNameForID(id);
    // Steam Input's virtual controller reports what it stands for as its type (SDL reads Steam's own information)
    SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
    int s = kX360;
    if (vendor == kValve && !SteamVirtual(id)) s = kSteam;
    else if (vendor == kMicrosoft && (product == 0x0202 || product == 0x0285 || product == 0x0287 || product == 0x0288 ||
                                      product == 0x0289))
        s = kXbox;  // the original Xbox controller (the Duke, the S) through a USB adapter
    else if ((vendor == kNintendo && (product == 0x0306 || product == 0x0330)) || Has(name, "Wii"))
        s = kWii;
    else switch (type) {
            case SDL_GAMEPAD_TYPE_XBOX360: s = kX360; break;
            case SDL_GAMEPAD_TYPE_XBOXONE: s = kSeries; break;
            case SDL_GAMEPAD_TYPE_PS3:
            case SDL_GAMEPAD_TYPE_PS4: s = kPs; break;
            case SDL_GAMEPAD_TYPE_PS5: s = kDualSense; break;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            case SDL_GAMEPAD_TYPE_GAMECUBE: s = kSwitch; break;
            case SDL_GAMEPAD_TYPE_STEAM: s = kSteam; break;
            default: s = kX360; break;
        }
    known[id] = s;
    LOGI("input: controller \"%s\" (vendor %04x, product %04x, SDL type %s%s): %s prompts", name ? name : "?", vendor,
         product, SDL_GetGamepadStringForType(type) ? SDL_GetGamepadStringForType(type) : "unknown",
         SteamVirtual(id) ? ", Steam Input's virtual controller" : "", Name(s));
    return s;
}

}  // namespace padstyle
