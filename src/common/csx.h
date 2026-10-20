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
// the profile name's text entry (src/game/pc_mouse.cpp): on after a click on the name field of a virtual keyboard
// screen, until Enter, Escape, a click elsewhere or the screen closes; the window's typed text and editing keys go to
// the game's own edit box through its virtual keyboard's functions
enum class TextKey { Backspace, Left, Right, Enter, Escape };
bool TextEntryOpen();
void QueueTextEntry(const std::string& text);
void QueueTextEntryKey(TextKey key);
// press-a-key rebinding in PC Controls (src/game/pc_menu.cpp): whether such a screen is open with a binding row
// selected (Enter starts a capture there; the keyboard is not the pad's stick there), whether a row waits for its
// key (the window's next key, mouse button or wheel notch goes there, nothing else); a capture started on the
// selected row; the key pressed (engine number, keys.c), -1 cancelling
bool BindRowSelected();
bool BindCapturing();
void BindStart();
void BindInput(int key);
// a menu screen of the game is open (keyboard and mouse act as the pad there; otherwise as engine keys)
bool MenuOpen();
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
