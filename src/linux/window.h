#pragma once
// The SDL3 window front end for Linux: the headless harness's host (kernel, XDK runtime, reference renderer) with a
// window that shows each presented frame and SDL input in place of the pad script.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace window {

// the latest presented frame, BGRA rows tightly packed, written by the backend, read by the window thread
struct Frame {
    std::mutex lock;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint64_t serial = 0;  // bumped per presented frame
    float aspect = 4.0f / 3.0f;  // the display the title renders for (its present parameters' width : height)
};
Frame& Latest();

// input the window thread gathers for input::Poll
void HandleEvent(const void* sdl_event);  // an SDL_Event
void EndFrameInput();                     // after a batch of events: mouse motion turns into this frame's look input
void UpdateConsole();                     // once a frame: follow the developer console opening or closing
bool ConsoleShown();                      // the console is open (the keyboard is its, the mouse not captured)
std::string KeyLabels();                  // CSX_KEY_LABELS: the punctuation keys as the system layout prints them
void SetPlayCapture(bool on);             // the mouse captured while playing (F1, a click in the window)
bool PlayCapture();
bool MouseFree();                         // a menu or the console has the mouse (the pointer shown)

// the SDL window (an SDL_Window*), and which renderer draws into it
void* SdlWindow();
bool UseVulkan();
// the device id scenario input carries (SDL keyboard and mouse ids), so a test run can tell it from the host's
constexpr uint32_t kInjectedDevice = 0x7E57;
// a window shot request ("shot" file) handed to a renderer that presents by itself (Vulkan); empty when none
std::string TakeShotRequest();
void RequestShot(const std::string& name);

}  // namespace window
