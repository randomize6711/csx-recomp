#pragma once

// Counter-Strike specific host code shared by every image
namespace csx {

// the APU sample counter the title reads from the hardware (apu_clock.cpp)
void StartApuClock();
// diagnostic guest memory dumps on request (memory_dump.cpp)
void StartMemoryDumps();
// local player state logged on request (src/game/state_probe.cpp)
void StartStateProbe();
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
