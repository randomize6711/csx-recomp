// Split-screen IPC (docs/splitscreen.md): the compositor and each player process talk over one Unix SOCK_SEQPACKET
// socket (a socketpair the compositor makes; the player gets its end as an inherited descriptor, CSX_SPLIT_FD, kept
// across the title's relaunches, which exec the same process). Each message is one packet: a fixed header and a body,
// file descriptors (the exported Vulkan images) alongside as SCM_RIGHTS. Linux only (the Windows port will have its
// own transport for the same messages).
#pragma once

#include <algorithm>
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
    Solo = 8,        // player 1's relaunch continues in a window of its own (N5F): the compositor's window closes
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
    Alone = 108,     // player 1 is the only player left (u8 on): its next relaunch may leave split screen (N5F)
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
// native split-screen (docs/native_splitscreen.md 16): a view's frame (sw x sh pixels, display aspect `aspect`) placed
// in its viewport (fractions vx, vy, vw, vh of a W x H window) as FrameRect places it, clipped to the viewport: the
// pixel rectangles of the blit, source and destination (x0 < x1, y0 < y1); empty when nothing shows
struct ViewBlit {
    bool empty = true;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
};
inline ViewBlit PlaceInViewport(float W, float H, float vx, float vy, float vw, float vh, float aspect, float sw,
                                float sh) {
    ViewBlit b;
    if (!(W > 0 && H > 0 && vw > 0 && vh > 0 && aspect > 0 && sw > 0 && sh > 0)) return b;
    // the viewport in whole pixels (adjacent viewports share their edges exactly)
    int px0 = int(vx * W + 0.5f), py0 = int(vy * H + 0.5f), px1 = int((vx + vw) * W + 0.5f),
        py1 = int((vy + vh) * H + 0.5f);
    px0 = std::max(px0, 0);
    py0 = std::max(py0, 0);
    px1 = std::min(px1, int(W));
    py1 = std::min(py1, int(H));
    if (px1 <= px0 || py1 <= py0) return b;
    float fx, fy, fw, fh;
    FrameRect(float(px1 - px0), float(py1 - py0), aspect, fx, fy, fw, fh);
    // the frame's rectangle in the window, and what of it the viewport shows
    float x0 = px0 + fx, y0 = py0 + fy, x1 = x0 + fw, y1 = y0 + fh;
    float cx0 = std::max(x0, float(px0)), cy0 = std::max(y0, float(py0));
    float cx1 = std::min(x1, float(px1)), cy1 = std::min(y1, float(py1));
    if (cx1 <= cx0 || cy1 <= cy0) return b;
    b.dx0 = int(cx0 + 0.5f);
    b.dy0 = int(cy0 + 0.5f);
    b.dx1 = int(cx1 + 0.5f);
    b.dy1 = int(cy1 + 0.5f);
    // the source's matching part (the cut sides of a wider frame are not drawn)
    b.sx0 = int((cx0 - x0) / fw * sw + 0.5f);
    b.sy0 = int((cy0 - y0) / fh * sh + 0.5f);
    b.sx1 = int((cx1 - x0) / fw * sw + 0.5f);
    b.sy1 = int((cy1 - y0) / fh * sh + 0.5f);
    b.empty = b.dx1 <= b.dx0 || b.dy1 <= b.dy0 || b.sx1 <= b.sx0 || b.sy1 <= b.sy0;
    return b;
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
