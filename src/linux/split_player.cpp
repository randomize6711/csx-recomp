// A split-screen player process's side (split_player.h; docs/splitscreen.md).
#include "linux/split_player.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "linux/split_ipc.h"

namespace splitp {
namespace {

int g_fd = -1;
SDL_Window* g_win = nullptr;
std::mutex g_send_lock;  // the renderer's notifier, the game thread and the window thread all send
std::atomic<uint32_t> g_pw{0}, g_ph{0}, g_lw{0}, g_lh{0};
std::atomic<uint64_t> g_size_serial{0};
std::atomic<float> g_match_aspect{0};  // this player's viewport aspect in a match of every configured player
std::atomic<float> g_menu_aspect{0};   // the whole window's (player 1 alone in the menus)
uint64_t g_size_applied = 0;
std::atomic<bool> g_focus{true}, g_captured{false};
std::atomic<uint32_t> g_held{0};  // bit i: image i shown or about to be shown by the compositor

struct PadOp {
    uint8_t kind, index;
    int16_t value;
};
std::mutex g_pad_lock;
std::deque<PadOp> g_pad_ops;
SDL_Joystick* g_vpad = nullptr;
SDL_JoystickID g_vpad_id = 0;

// the text of text-input events: SDL keeps the pointer, so the strings stay (the last 64)
std::mutex g_text_lock;
std::deque<std::string> g_texts;

bool SendLocked(const split::Message& m, const int* fds = nullptr, int nfds = 0) {
    std::lock_guard<std::mutex> lock(g_send_lock);
    return g_fd >= 0 && split::Send(g_fd, m, fds, nfds);
}

void Quit(const char* why) {
    LOGI("split: %s: the player process ends", why);
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

void Receiver() {
    for (;;) {
        split::Message m;
        if (!split::Receive(g_fd, m)) {
            Quit("the compositor is gone");
            return;
        }
        size_t at = 0;
        switch (m.type) {
        case split::Msg::Size: {
            uint32_t pw = 0, ph = 0, lw = 0, lh = 0;
            float match = 0;
            if (m.Get(at, pw) && m.Get(at, ph) && m.Get(at, lw) && m.Get(at, lh) && pw && ph) {
                g_pw = pw, g_ph = ph, g_lw = lw, g_lh = lh;
                if (m.Get(at, match) && match > 0) g_match_aspect = match;
                if (float menu = 0; m.Get(at, menu) && menu > 0) g_menu_aspect = menu;
                g_size_serial++;
            }
            break;
        }
        case split::Msg::Event: {
            SDL_Event e{};
            if (m.body.size() < sizeof(SDL_Event)) break;
            std::memcpy(&e, m.body.data(), sizeof(SDL_Event));
            // only input events (nothing that carries pointers or targets windows of the compositor's process)
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.key.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                e.motion.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                e.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                e.wheel.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            case SDL_EVENT_TEXT_INPUT: {
                std::lock_guard<std::mutex> lock(g_text_lock);
                g_texts.emplace_back(m.body.begin() + long(sizeof(SDL_Event)), m.body.end());
                if (g_texts.size() > 64) g_texts.pop_front();
                e.text.text = g_texts.back().c_str();
                e.text.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            }
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                e.window.windowID = g_win ? SDL_GetWindowID(g_win) : 0;
                break;
            default:
                continue;
            }
            SDL_PushEvent(&e);
            break;
        }
        case split::Msg::Release: {
            uint32_t i = 0;
            if (m.Get(at, i) && i < 32) g_held &= ~(1u << i);
            break;
        }
        case split::Msg::Pad: {
            PadOp op{};
            if (m.Get(at, op.kind) && m.Get(at, op.index) && m.Get(at, op.value)) {
                std::lock_guard<std::mutex> lock(g_pad_lock);
                g_pad_ops.push_back(op);
            }
            break;
        }
        case split::Msg::Focus: {
            uint8_t focus = 1, captured = 0;
            m.Get(at, focus);
            m.Get(at, captured);
            g_focus = focus != 0;
            g_captured = captured != 0;
            break;
        }
        case split::Msg::State: {
            csx::SetSplitState(m.Text());
            // the Split Screen screen's Local Players shows the players the compositor has now (--splitscreen N, a
            // controller's START), in memory: saved only when that screen's Accept saves it
            int players = 0;
            if (std::sscanf(m.Text().c_str(), "players %d", &players) == 1 && players >= 1 && players <= 4 &&
                pc::Get("split_players", "1") != std::to_string(players))
                pc::Set("split_players", std::to_string(players));
            break;
        }
        case split::Msg::Quit:
            Quit("the compositor ended the player");
            return;
        default:
            break;
        }
        for (int f : m.fds) ::close(f);
    }
}

// the next boot's display aspect (pc_wide.cpp reads CSX_DISPLAY_ASPECT once per boot): a match's viewport, or the
// whole window back in the menus (player 1 is alone there)
void PrepareRelaunch(bool match) {
    float a = match ? g_match_aspect.load() : g_menu_aspect.load();
    if (match && a <= 0) a = g_lh ? float(g_lw) / float(g_lh) : 0.0f;
    if (a <= 0) return;
    char text[32];
    std::snprintf(text, sizeof text, "%.5f", double(a));
    setenv("CSX_DISPLAY_ASPECT", text, 1);
    LOGI("split: relaunching %s at aspect %s", match ? "into a match" : "to the menus", text);
}

void Sender(int kind, const std::string& text) {
    split::Message m;
    m.type = split::Msg(kind);
    m.PutText(text.substr(0, split::kMaxBody));
    SendLocked(m);
}

}  // namespace

bool Active() {
    static const bool on = std::getenv("CSX_SPLIT_FD") != nullptr;
    return on;
}

int Player() { return csx::SplitPlayer(); }

void Start(void* sdl_window) {
    if (!Active()) return;
    g_win = static_cast<SDL_Window*>(sdl_window);
    g_fd = std::atoi(std::getenv("CSX_SPLIT_FD"));
    setenv("RELAUNCH_KEEP_FD", std::getenv("CSX_SPLIT_FD"), 1);  // the title's relaunches keep it (SDK patch 0044)
    csx::SetSplitSender(Sender);
    csx::SetSplitPrepareRelaunch(PrepareRelaunch);
    split::Message hello;
    hello.type = split::Msg::Hello;
    hello.Put(uint8_t(Player()));
    if (!SendLocked(hello)) {
        LOGE("split: no compositor on descriptor %d", g_fd);
        return;
    }
    LOGI("split: player %d, the compositor on descriptor %d", Player(), g_fd);
    std::thread(Receiver).detach();
}

void Frame() {
    if (!Active()) return;
    if (uint64_t s = g_size_serial; s != g_size_applied && g_win) {
        g_size_applied = s;
        SDL_SetWindowSize(g_win, int(g_lw.load()), int(g_lh.load()));
        LOGI("split: viewport %ux%u pixels (%ux%u)", g_pw.load(), g_ph.load(), g_lw.load(), g_lh.load());
    }
    std::deque<PadOp> ops;
    {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        ops.swap(g_pad_ops);
    }
    for (const PadOp& op : ops) {
        if (op.kind == 2 && !g_vpad) {
            SDL_VirtualJoystickDesc d;
            SDL_INIT_INTERFACE(&d);
            d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            d.naxes = SDL_GAMEPAD_AXIS_COUNT;
            d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            d.name = "Split-screen controller";
            g_vpad_id = SDL_AttachVirtualJoystick(&d);
            g_vpad = g_vpad_id ? SDL_OpenJoystick(g_vpad_id) : nullptr;
            // a virtual gamepad's triggers are full-range joystick axes (raw -32768 released .. 32767 pulled): at rest
            // 0 they would read half pulled (N4.6: a held-down trigger, one shot from a semi-automatic weapon)
            if (g_vpad) {
                SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
                SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
            }
            LOGI("split: controller attached%s", g_vpad ? "" : " (failed)");
        } else if (op.kind == 3 && g_vpad) {
            SDL_CloseJoystick(g_vpad);
            SDL_DetachVirtualJoystick(g_vpad_id);
            g_vpad = nullptr;
            LOGI("split: controller detached");
        } else if (op.kind == 0 && g_vpad) {
            // the compositor sends what its gamepad reads: a trigger 0 (released) .. 32767, the sticks -32768 .. 32767
            int v = op.value;
            if (op.index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || op.index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                v = std::max(0, v) * 2 - 32768;
            SDL_SetJoystickVirtualAxis(g_vpad, op.index, int16_t(std::clamp(v, -32768, 32767)));
        } else if (op.kind == 1 && g_vpad) {
            SDL_SetJoystickVirtualButton(g_vpad, op.index, op.value != 0);

        }
    }
}

bool Size(uint32_t& w, uint32_t& h) {
    w = g_pw;
    h = g_ph;
    return w && h;
}
uint64_t SizeSerial() { return g_size_serial; }
bool Focused() { return g_focus; }
bool Captured() { return g_captured; }

void SendImages(uint32_t w, uint32_t h, const std::vector<ImageInfo>& images, float frame_aspect) {
    split::Message m;
    m.type = split::Msg::Images;
    m.Put(w);
    m.Put(h);
    m.Put(uint32_t(images.size()));
    std::vector<int> fds;
    for (const auto& i : images) {
        m.Put(i.size);
        m.Put(i.memory_type);
        fds.push_back(i.fd);
    }
    m.Put(frame_aspect);
    g_held = 0;
    SendLocked(m, fds.data(), int(fds.size()));
}

void SendReady(uint32_t index) {
    g_held |= 1u << index;
    split::Message m;
    m.type = split::Msg::Ready;
    m.Put(index);
    SendLocked(m);
}

bool Held(uint32_t index) { return (g_held >> index) & 1u; }

void SendCapture(bool want) {
    static int sent = -1;
    if (sent == int(want)) return;
    sent = want;
    split::Message m;
    m.type = split::Msg::Capture;
    m.Put(uint8_t(want));
    SendLocked(m);
}

void Send(int kind, const std::string& text) { Sender(kind, text); }

bool StartFromWindow(int players) {
    std::string hdd = (app::ExeDir() / "hdd").string();
    std::vector<uint8_t> page(4096, 0);  // the launch page (tests/launch/README.md), launch data 1: the front end
    const uint8_t head[] = {0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x53, 0x4D};
    std::memcpy(page.data(), head, sizeof(head));
    std::memcpy(page.data() + 8, "\\Device\\CdRom0;default.xbe", 26);
    const uint8_t mid[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(page.data() + 0x210, mid, sizeof(mid));
    std::memcpy(page.data() + 0x21C, "\\Device\\CdRom0\0default.xbe", 26);
    page[0x400] = 1;
    if (FILE* f = std::fopen((hdd + "/launch.bin").c_str(), "wb")) {
        std::fwrite(page.data(), 1, page.size(), f);
        std::fclose(f);
    }
    std::remove((hdd + "/Z/releasemap.txt").c_str());
    setenv("CSX_SPLIT_COMPOSITOR", "1", 1);
    setenv("CSX_SPLIT_PLAYERS", std::to_string(players).c_str(), 1);
    setenv("CSX_RENDERER", "vulkan", 1);
    LOGI("split: starting split screen for %d players: the window restarts as the compositor", players);
    logx::Flush();
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char* game = std::getenv("GAME_DIR");
    for (int fd = 3; fd < 1024; fd++) fcntl(fd, F_SETFD, FD_CLOEXEC);
    execl(exe, exe, game ? game : "", nullptr);
    return false;
}

}  // namespace splitp
