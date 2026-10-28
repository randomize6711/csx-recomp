// Split-screen IPC (docs/splitscreen.md): the compositor and each player process talk over one Unix SOCK_SEQPACKET
// socket (a socketpair the compositor makes; the player gets its end as an inherited descriptor, CSX_SPLIT_FD, kept
// across the title's relaunches, which exec the same process). Each message is one packet: a fixed header and a body,
// file descriptors (the exported Vulkan images) alongside as SCM_RIGHTS. Linux only (the Windows port will have its
// own transport for the same messages).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace split {

enum class Msg : uint16_t {
    // player -> compositor
    Hello = 1,       // a player process started (u8 player); after every relaunch
    Images = 2,      // its output images (u32 width, height, count; per image u64 size, u32 memory type bits; fds;
                     // then f32 the game frame's aspect, set at its boot)
    Ready = 3,       // image i holds a finished frame (u32 index)
    Capture = 4,     // the player wants the mouse captured (u8 on): playing, not in a menu or the console
    Match = 5,       // where the player plays (text: "host ADDRESS KEY" / "join ADDRESS KEY PASSWORD" / "menu")
    Log = 6,         // a line for the compositor's log (text)
    Setup = 7,       // player 1's setup screen asks (text: "open", "close", "players N", "layout NAME", "end", "start")
    // compositor -> player
    Size = 100,      // the viewport's pixel size (u32 width, height)
    Event = 101,     // an SDL_Event, as bytes
    Release = 102,   // the compositor is done reading image i (u32 index)
    Pad = 103,       // the player's controller: (u8 kind 0 axis / 1 button / 2 attached / 3 detached, u8 index, i16 value)
    Focus = 104,     // the window has the input focus (u8 on); the player's mouse captured by the compositor (u8 on)
    State = 105,     // for player 1's setup screen (text: the players' lines, "slot N device|join|-")
    Quit = 106,      // end the process (players 2-4 when their match ended)
    Party = 107,     // player 1's party screens (N4.8): another member's input or a party event (text: "N BUTTON",
                     // "N joined", "N left", "N pad on|off"; BUTTON a b x y start back up down left right lb rb)
};

constexpr size_t kMaxBody = 4096;

// where a split-screen player's game frame (its aspect set at boot: pc_wide.cpp) shows in its W x H viewport: fitted
// (letterboxed), but a frame wider than a viewport of at least 4:3 fills its height, the sides cropped: the menus' and
// the HUD's 640x480 stay whole in the frame's 4:3 middle. A viewport narrowed while the player plays (a fourth player
// joining a three-player layout) is filled so; the player's next boot takes the new aspect.
inline void FrameRect(float W, float H, float aspect, float& x, float& y, float& w, float& h) {
    w = W;
    h = W / aspect;
    if (h > H || (W / H >= 4.0f / 3.0f - 0.001f && aspect > W / H)) {
        h = H;
        w = H * aspect;
    }
    x = (W - w) / 2;
    y = (H - h) / 2;
}
constexpr int kMaxFds = 4;

struct Message {
    Msg type{};
    std::vector<uint8_t> body;
    std::vector<int> fds;  // received descriptors (the receiver owns them)

    template <typename T>
    void Put(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        body.insert(body.end(), p, p + sizeof(T));
    }
    void PutText(const std::string& s) { body.insert(body.end(), s.begin(), s.end()); }
    template <typename T>
    bool Get(size_t& at, T& v) const {
        if (body.size() < at + sizeof(T)) return false;
        std::memcpy(&v, body.data() + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
    std::string Text() const { return std::string(body.begin(), body.end()); }
};

// a connected pair (compositor end, player end), both close-on-exec but the player's, which the child keeps
bool Pair(int& compositor_end, int& player_end);
// one message; false when the peer is gone (or the socket would block, for non-blocking sockets: then *again is set)
bool Send(int fd, const Message& m, const int* fds = nullptr, int nfds = 0);
bool Receive(int fd, Message& m, bool* again = nullptr);
void SetNonBlocking(int fd);

}  // namespace split
