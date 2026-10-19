#pragma once

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
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
