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
// the developer console (src/game/pc_console.cpp): keys and typed text for it, handled on the game thread; whether
// it is shown (the window sends the keyboard there while it is)
enum class ConsoleKey {
    Toggle, Close, Text, Backspace, Enter, HistoryBack, HistoryForward, Complete, PageUp, PageDown
};
void QueueConsoleKey(ConsoleKey key);
void QueueConsoleText(const std::string& text);
bool ConsoleOpen();
// mouse navigation in the game's menus (src/game/pc_mouse.cpp): the pointer in the menus' 640x480 coordinates (-1 -1
// outside the window), a click (1 left, 2 right, 11 a left double click), wheel notches (up positive); the pad
// filter that turns them into the game's pad presses (GameInfo::pad)
void MenuPointer(float x, float y);
void MenuClick(int button);
void MenuWheel(int notches);
}  // namespace csx
namespace input { struct Pad; }
namespace csx {
void MenuPad(int port, input::Pad& pad);
// a menu screen of the game is open (keyboard and mouse act as the pad there; otherwise as engine keys)
bool MenuOpen();
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
