#pragma once

#include <string>
#include <vector>

// Counter-Strike specific host code shared by every image
namespace csx {

// the APU sample counter the title reads from the hardware (apu_clock.cpp)
void StartApuClock();
// diagnostic guest memory dumps on request (memory_dump.cpp)
void StartMemoryDumps();
// local player state logged on request (src/game/state_probe.cpp)
void StartStateProbe();
// guest memory use logged every 10 s, CSX_LOG_MEMORY=1 (memory_stats.cpp)
void StartMemoryStats();
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
// invite codes (N9, src/game/pc_invite.cpp): this match's "ADDRESS/KEY" (empty with why_none set when it has none),
// and a code pc_invite asked the window to copy to the clipboard (empty: none; taken once)
std::string InviteCode(std::string& why_none);
std::string TakeClipboardRequest();
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
// outside the window), a click (1 left, 2 right, 11 a left double click, -1 the left button let go), wheel notches
// (up positive); the pad
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
enum class TextKey { Backspace, Left, Right, Enter, Escape, Delete, Home, End };
bool TextEntryOpen();
// a profile name screen (Create / Rename Profile): its first character key starts typing (Space, Enter, Escape keep
// their menu actions until then: the on-screen keyboard's A, Done, back)
bool TextEntryArmed();
void StartTextEntry();
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
// a typed number in a PC settings screen (N5B, src/game/pc_menu.cpp): whether the selected row takes one (a digit
// or a point starts typing it there), whether one is being typed (the keyboard goes there, nothing else); input:
// a character ('0'..'9', '.'), or kNumberCancel / kNumberBackspace / kNumberEnter
constexpr int kNumberCancel = -1, kNumberBackspace = -2, kNumberEnter = -3;
bool NumberRowSelected();
bool NumberTyping();
// a click on a number row's value (pc_mouse.cpp): typing starts there with the value shown selected (a key replaces it)
void NumberEdit();
// the HUD editor (N10, src/game/pc_hud_layout.cpp): on while it edits (the keys and pad go to it); input: 'n' / 'p'
// next / previous element, 'l' 'r' 'u' 'd' move (upper case: larger steps), '+' '-' size, 'x' reset it, 'X' reset all,
// 's' save, 'c' cancel
bool HudEditing();
void HudEditInput(int key);
void NumberInput(int c);
// the device last used (N5D, src/game/pc_prompts.cpp; linux/pad_style.h's styles): the prompts' automatic style
void SetPromptDevice(int style);
// the window's keyboard focus (src/linux/sdl_audio.cpp: Audio in the Background, N5E)
void SetWindowFocused(bool focused);
int PromptDevice();
// a map list is open (N5B, src/game/pc_mapsearch.cpp): its search field takes letters, digits, - and _ (characters
// as themselves; -2 Backspace, -3 Delete, -4 Left, -5 Right, -6 Home, -7 End, -8 Enter, -9 Escape, -10 Ctrl+A,
// -11 Ctrl+X's cut), and while it is being typed in (MapSearchEditing) every editing key; the selected text for Ctrl+C
bool MapSearchKeys();
bool MapSearchEditing();
void MapSearchInput(int c);
std::string MapSearchSelection();
// a menu screen of the game is open (keyboard and mouse act as the pad there; otherwise as engine keys)
bool MenuOpen();
// the keyboard and mouse may act on the open screen (native split-screen: player 1 drives the shared front end; else
// always): the PC screens' input (pc_mouse, pc_menu, pc_browser, pc_mapsearch) leaves another driver's screen alone
bool PcDrivesScreen();
// one of the buy menu's screens is open (window_input.cpp: the ammunition keys still buy there)
bool BuyMenuOpen();
// a server browser screen is open (src/game/pc_browser.cpp): Escape is the pad's B there, F5 its X (refresh),
// Page Up / Page Down its triggers (the tabs)
bool BrowserKeys();
// split-screen (docs/splitscreen.md): this process's local player (1..4; 0 when the game runs on its own, the usual
// case), a message to the compositor (split::Msg kind and text; nothing without one: the headless harness, the
// dedicated server), the compositor's latest state text for player 1's setup screen
int SplitPlayer();
enum SplitKind { kSplitMatch = 5, kSplitLog = 6, kSplitSetup = 7 };  // src/linux/split_ipc.h's Msg values
void SplitSend(int kind, const std::string& text);
void SetSplitSender(void (*send)(int kind, const std::string& text));
std::string SplitState();
void SetSplitState(const std::string& text);
// player 1's party screens (N4.8, src/game/pc_party.cpp): what the compositor sent (split::Msg::Party), taken by the
// game thread's menu frame in arrival order
void PushPartyInput(const std::string& text);
std::vector<std::string> TakePartyInput();
// the game alone in its window (no compositor) shows a party screen: the window's input watches every controller, and
// another controller's A or START asks for split screen (pc_party.cpp restarts the window as the compositor)
void SetPartyScreenOpen(bool open);
bool PartyScreenOpen();
void PartyJoinRequest();
bool TakePartyJoinRequest();
// the game alone in its window on Instant Action (2026-10-31, its local players): a controller other than player 1's
// own asks to join with START only (A there is the menus'); the controller that asked, kept for the compositor to
// join it at once after the restart (SDL's GUID text)
void SetMenuJoin(bool on);
bool MenuJoin();
void SetPartyJoinGuid(const std::string& guid);
std::string PartyJoinGuid();
// native split-screen's controller seats (docs/native_splitscreen.md 18.3, src/common/pad_seats.h): the window's
// input keeps them and publishes which seats are in the game (bit k: player k+1); the game adds player k+1 only while
// its bit is set, and tells the window when a player left by its own choice (its seat freed). Unknown (~0u: the
// headless harness, no window) means every configured player
void SetNativeSeats(unsigned mask);
// native split-screen: player 1's view in the window (fractions; w 0: none drawn), set at its turn, so the menus'
// pointer maps into the view its menus are drawn in (2026-11-16 playtest: the pause menu's clicks were off)
void SetNativeP1View(float x, float y, float w, float h);
bool NativeP1View(float& x, float& y, float& w, float& h);  // false: not native split-screen
unsigned NativeSeats();
void NativeSeatLeave(int k);
unsigned TakeNativeSeatLeaves();
// docs 18.6: the seats with a controller connected (bit k), and the seats as a match's launch takes them (pad_seats.h
// SaveCompact: "k=key;...", the players N); published by the window with SetNativeSeats' mask
void SetNativeSeatsDetail(unsigned held, const std::string& launch, int players);
unsigned NativeSeatsHeld();
std::string NativeSeatsLaunch(int* players);
// split-screen's engine (PC setting split_engine): "native" (one process, docs/native_splitscreen.md; the default) or
// "processes" (the multi-process compositor, docs/splitscreen.md: the fallback)
bool SplitEngineNative();
// a split-screen player is about to relaunch (into a match or back to the menus): its next display aspect set
void SplitPrepareRelaunch(bool match);
void SetSplitPrepareRelaunch(void (*prepare)(bool match));
int SplitPlayers();  // the local players configured (CSX_SPLIT_PLAYERS, the setup screen's), 1 without split-screen
// the game alone in its window starts split-screen (the window front end restarts as the compositor); false where
// there is none (the headless harness)
bool SplitStart(int players);
// the whole game closes (the party leader's Quit Game, N4.8): the window front end registers how (its window's quit)
void AppQuit();
void SetAppQuitter(void (*quit)());
void SetSplitStarter(bool (*start)(int players));
// the network's process settings (src/game/net_engine.cpp)
void StartNetwork();
// what GameInfo::loaded starts
void Loaded();

}  // namespace csx
