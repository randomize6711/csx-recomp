// A split-screen player process's side (docs/splitscreen.md): the game runs as usual but without a window of its
// own. The compositor (compositor.cpp) gives it its viewport's size, its input (SDL events for the keyboard and mouse
// it owns, its controller's state for a virtual SDL gamepad) and the window's focus; it renders off screen into
// images exported to the compositor (vk_backend.cpp) and says when each frame is ready, whether it wants the mouse
// captured and where it plays (src/game/pc_split.cpp through csx::SplitSend). Linux only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splitp {

bool Active();      // this process is a split-screen player (CSX_SPLIT_FD)
int Player();       // 1..4 (CSX_SPLIT_PLAYER)
void Start(void* sdl_window);  // the receiver thread; the hidden window takes the viewport's size
void Frame();       // on the window thread each frame: the controller's queued state, size and focus applied

// the viewport: pixels (the renderer's output) and logical units (the hidden window, the menus' pointer)
bool Size(uint32_t& pixel_w, uint32_t& pixel_h);
uint64_t SizeSerial();  // bumped when the compositor sends a new size
bool Focused();         // the compositor's window has the keyboard focus
bool Captured();        // the compositor captured the mouse for this player

// the renderer's output images
struct ImageInfo {
    uint64_t size;
    uint32_t memory_type;
    int fd;
};
void SendImages(uint32_t w, uint32_t h, const std::vector<ImageInfo>& images);
void SendReady(uint32_t index);
bool Held(uint32_t index);  // the compositor still reads it
void SendCapture(bool want);
void Send(int kind, const std::string& text);  // a split::Msg with text (Match, Log, Setup)
// the game alone in its window starts split-screen: this process becomes the compositor (exec), which starts it again
// as player 1 back on the main menu (the title's own launch data for a return to the front end)
bool StartFromWindow(int players);

}  // namespace splitp
