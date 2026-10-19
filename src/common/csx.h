#pragma once

#include <string>

// Counter-Strike specific host code shared by every image
namespace csx {

// the APU sample counter the title reads from the hardware (apu_clock.cpp)
void StartApuClock();
// diagnostic guest memory dumps on request (memory_dump.cpp)
void StartMemoryDumps();
// local player state logged on request (src/game/state_probe.cpp)
void StartStateProbe();
// diagnostic write watch on guest pages, CSX_WATCH=VA[:BYTES] (write_watch.cpp, Linux only)
void StartWriteWatch();
// the launch data a relaunch left, copied to launch_seen.bin (launch_capture.cpp)
void KeepLaunchData();
// native mouse look: degrees to the right and up, applied on the game thread by the look hook (src/game/mouse_look.cpp)
void AddLook(float right_degrees, float up_degrees);
// PC keys and mouse buttons as engine keys (src/game/pc_keys.cpp): a key event (engine key number) and console text,
// handed to the engine on the game thread; the engine's current binding of a key
void QueueEngineKey(int key, bool down);
void QueueEngineCommand(const std::string& text);
std::string EngineBinding(int key);
// a menu screen of the game is open (keyboard and mouse act as the pad there; otherwise as engine keys)
bool MenuOpen();
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
