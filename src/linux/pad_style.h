// Which button prompts a controller's (N5D, src/game/pc_prompts.cpp): the style of artwork the menus show for it,
// from SDL's controller type, its USB vendor and product and its name.
#pragma once

#include <SDL3/SDL.h>

namespace padstyle {

// the prompt styles (src/game/pc_prompts.cpp's kStyles, tools/make_prompts.py's STYLES: the same order)
enum Style : int { kPc = 0, kXbox, kX360, kSeries, kPs, kDualSense, kSwitch, kSteam, kWii, kCount };

const char* Name(int style);
// an opened or listed controller's style (kX360 for one SDL knows nothing about: SDL lays every gamepad out as an Xbox
// controller), logged once a controller with what it was told from
int Of(SDL_JoystickID id);
// Steam Input's virtual controller (Steam's own, standing for a real one it reads itself)
bool SteamVirtual(SDL_JoystickID id);
// a Valve controller read directly (USB or Bluetooth)
bool SteamPhysical(SDL_JoystickID id);

}  // namespace padstyle
