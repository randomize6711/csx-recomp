// The split-screen compositor (docs/splitscreen.md): the window front end in its compositor mode (CSX_SPLIT_COMPOSITOR=1,
// run_game.sh --splitscreen N). It runs no game of its own: it owns the window, the Vulkan swapchain, the keyboard,
// the mouse and every controller, and starts one game process per local player (this executable again, as a player:
// CSX_SPLIT_FD, src/linux/split_player.cpp). Each player draws off screen into images it exports (vk_backend.cpp);
// the compositor imports them on the same GPU and blits each player's newest finished frame into its viewport.
//
// - Player 1 runs the front end (menus, browser, matches) in the run folder, as the game alone would. When it hosts a
//   match (offline against bots or Host a Match: its server on UDP) or joins one (the browser, an address), it says
//   where (split::Msg::Match), and the compositor starts players 2..N in their own folders (split2..split4: their own
//   disk, PC settings, identity) straight into "+connect" to that server with its key pinned (and its password). Back
//   in the menus, players 2..N are ended. Map changes relaunch every player by itself (the server's MapChange), each
//   keeping its socket to the compositor.
// - Layouts: 1 player the whole window; 2 top / bottom (or left / right: split_layout vertical); 3 player 1 across the
//   top, 2 and 3 below; 4 a 2x2 grid. A thin black line between viewports.
// - Input: the keyboard and mouse go to the player who owns them (player 1 unless split_kbm says otherwise), the
//   pointer in that player's viewport coordinates, the mouse captured while that player plays; each controller to its
//   player (in order of connection to players 2, 3, 4; with split_p1_pad 1 the first goes to player 1), as a virtual
//   gamepad in that player's process. F11 / Alt+Enter full screen, F1 the capture, as the game's window.
// - Audio: every player's own stream (its listener at its own player), mixed by the system; each at 1/sqrt(N) gain.
// - Tests: the window front end's request files (keys, mouse, text, shot) work here, plus "pad" (virtual controllers);
//   every player's output lines are copied into the compositor's log as "pN: ...".
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "linux/split_ipc.h"
#include "linux/window.h"

namespace fs = std::filesystem;

namespace {

constexpr int kMaxPlayers = 4;
constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Imported {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Player {
    int n = 0;
    pid_t pid = 0;
    int fd = -1;
    bool alive = false;
    std::string folder;
    std::array<Imported, 3> images{};
    uint32_t img_w = 0, img_h = 0;
    uint64_t generation = 0;  // bumped with each set of images (a relaunch, a resize)
    int ready = -1;           // the newest finished frame not shown yet
    int shown = -1;           // the frame drawn into the window now (held until another replaces it)
    bool wants_capture = false;
    Rect view;                // logical units
    Rect view_px;             // pixels
    SDL_JoystickID pad = 0;   // its controller (0: none)
    std::string target;       // players 2..4: the server they were started for
};

std::array<Player, kMaxPlayers> g_players;
std::mutex g_log_lock;

// ---- Vulkan -----------------------------------------------------------------------------------------------------------
struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;  // per swapchain image
    VkCommandPool pool = VK_NULL_HANDLE;
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        std::vector<std::pair<int, int>> release;  // (player, image) to release once this frame is done
    };
    std::array<Frame, 2> frames;
    uint64_t frame = 0;
    std::string uuid;
    PFN_vkGetMemoryFdPropertiesKHR fd_props = nullptr;
} g;

bool CreateGpu(SDL_Window* win) {
    uint32_t n = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> ext(sdl_ext, sdl_ext + n);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Counter-Strike split-screen";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    if (vkCreateInstance(&ci, nullptr, &g.instance) != VK_SUCCESS) return false;
    if (!SDL_Vulkan_CreateSurface(win, g.instance, nullptr, &g.surface)) return false;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(g.instance, &n, gpus.data());
    for (int pass = 0; pass < 2 && !g.phys; pass++)
        for (VkPhysicalDevice p : gpus) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vkGetPhysicalDeviceProperties2(p, &p2);
            if (p2.properties.apiVersion < VK_API_VERSION_1_3) continue;
            if (pass == 0 && p2.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
            for (uint32_t i = 0; i < qn && !g.phys; i++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(p, i, g.surface, &present);
                if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                g.phys = p;
                g.qfamily = i;
                char hex[33];
                for (int k = 0; k < 16; k++) std::snprintf(hex + 2 * k, 3, "%02x", id.deviceUUID[k]);
                g.uuid = hex;
                LOGI("split: compositor on %s (device %s)", p2.properties.deviceName, hex);
            }
            if (g.phys) break;
        }
    if (!g.phys) return false;
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = g.qfamily;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dext;
    if (vkCreateDevice(g.phys, &dci, nullptr, &g.dev) != VK_SUCCESS) return false;
    vkGetDeviceQueue(g.dev, g.qfamily, 0, &g.queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = g.qfamily;
    vkCreateCommandPool(g.dev, &pc, nullptr, &g.pool);
    for (auto& f : g.frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g.dev, &ai, &f.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g.dev, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g.dev, &si, nullptr, &f.acquired);
    }
    return true;
}

bool CreateSwapchain(SDL_Window* win) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        ext = {uint32_t(w), uint32_t(h)};
    }
    if (!ext.width || !ext.height) return false;
    vkDeviceWaitIdle(g.dev);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = std::max<uint32_t>(caps.minImageCount + 1, 3);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = kFormat;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pc::Get("vsync", "1") != "0" ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    {  // MAILBOX only when offered
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, modes.data());
        if (std::find(modes.begin(), modes.end(), ci.presentMode) == modes.end()) ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(g.dev, &ci, nullptr, &sc) != VK_SUCCESS) return false;
    if (g.swapchain) vkDestroySwapchainKHR(g.dev, g.swapchain, nullptr);
    for (auto s : g.rendered) vkDestroySemaphore(g.dev, s, nullptr);
    g.swapchain = sc;
    g.extent = ext;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g.dev, sc, &n, nullptr);
    g.images.resize(n);
    vkGetSwapchainImagesKHR(g.dev, sc, &n, g.images.data());
    g.rendered.resize(n);
    for (auto& s : g.rendered) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g.dev, &si, nullptr, &s);
    }
    LOGI("split: swapchain %ux%u", ext.width, ext.height);
    return true;
}

void DropImages(Player& p) {
    if (p.images[0].image || p.images[1].image || p.images[2].image) vkDeviceWaitIdle(g.dev);
    for (auto& f : g.frames)
        f.release.erase(std::remove_if(f.release.begin(), f.release.end(), [&](auto& r) { return r.first == p.n; }),
                        f.release.end());
    for (auto& im : p.images) {
        if (im.image) vkDestroyImage(g.dev, im.image, nullptr);
        if (im.memory) vkFreeMemory(g.dev, im.memory, nullptr);
        im = {};
    }
    p.ready = p.shown = -1;
    p.img_w = p.img_h = 0;
}

// a player's exported images, imported with the parameters it made them with
bool ImportImages(Player& p, const split::Message& m) {
    size_t at = 0;
    uint32_t w = 0, h = 0, count = 0;
    if (!m.Get(at, w) || !m.Get(at, h) || !m.Get(at, count) || count != 3 || m.fds.size() != 3) return false;
    DropImages(p);
    for (uint32_t i = 0; i < 3; i++) {
        uint64_t size = 0;
        uint32_t type = 0;
        m.Get(at, size);
        m.Get(at, type);
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFormat;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Imported& im = p.images[i];
        if (vkCreateImage(g.dev, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = im.image;
        VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imp.fd = m.fds[i];  // the import owns the descriptor from here
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
        ma.allocationSize = size;
        ma.memoryTypeIndex = type;
        if (vkAllocateMemory(g.dev, &ma, nullptr, &im.memory) != VK_SUCCESS) {
            ::close(m.fds[i]);
            LOGE("split: player %d's image %u cannot be imported", p.n, i);
            return false;
        }
        vkBindImageMemory(g.dev, im.image, im.memory, 0);
    }
    p.img_w = w;
    p.img_h = h;
    p.generation++;
    LOGI("split: player %d's output %ux%u imported", p.n, w, h);
    return true;
}

// ---- layouts ------------------------------------------------------------------------------------------------------------
// the viewports of n players in a w x h window (logical units or pixels alike), a 2-unit gap between them
std::vector<Rect> Layout(int n, int w, int h, bool vertical, int gap) {
    int hw = (w - gap) / 2, hh = (h - gap) / 2;
    switch (n) {
    case 2:
        if (vertical) return {{0, 0, hw, h}, {w - hw, 0, hw, h}};
        return {{0, 0, w, hh}, {0, h - hh, w, hh}};
    case 3:
        return {{0, 0, w, hh}, {0, h - hh, hw, hh}, {w - hw, h - hh, hw, hh}};
    case 4:
        return {{0, 0, hw, hh}, {w - hw, 0, hw, hh}, {0, h - hh, hw, hh}, {w - hw, h - hh, hw, hh}};
    default:
        return {{0, 0, w, h}};
    }
}

int g_configured = 1;      // local players asked for (--splitscreen N, the setup screen)
bool g_vertical = false;   // two players side by side
SDL_Window* g_win = nullptr;
bool g_focus = true, g_captured = false, g_capture_play = true;
int g_kbm = 1;             // the player who has the keyboard and mouse
bool g_test_only = false;  // CSX_TEST_INPUT_ONLY: only the test's input (no host keyboard, mouse or controllers)

int Active() {  // players whose processes run (the layout's count)
    int n = 0;
    for (auto& p : g_players) n += p.alive;
    return std::max(1, n);
}

void SendSize(Player& p) {
    split::Message m;
    m.type = split::Msg::Size;
    m.Put(uint32_t(std::max(1, p.view_px.w)));
    m.Put(uint32_t(std::max(1, p.view_px.h)));
    m.Put(uint32_t(std::max(1, p.view.w)));
    m.Put(uint32_t(std::max(1, p.view.h)));
    // the aspect this player's next match will have (its viewport in the layout of every configured player)
    int ww = 0, wh = 0;
    SDL_GetWindowSize(g_win, &ww, &wh);
    auto match = Layout(g_configured, ww, wh, g_vertical, 2);
    const Rect& r = match[size_t(std::min(p.n, int(match.size())) - 1)];
    m.Put(float(r.h > 0 ? double(r.w) / r.h : 4.0 / 3.0));
    m.Put(float(wh > 0 ? double(ww) / wh : 4.0 / 3.0));  // and in the menus (alone: the whole window)
    if (p.fd >= 0) split::Send(p.fd, m);
}

void SendFocus(Player& p) {
    split::Message m;
    m.type = split::Msg::Focus;
    m.Put(uint8_t(g_focus));
    m.Put(uint8_t(g_captured && p.n == g_kbm));
    if (p.fd >= 0) split::Send(p.fd, m);
}

void Relayout() {
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    int n = Active();
    auto logical = Layout(n, ww, wh, g_vertical, 2);
    auto pixels = Layout(n, pw, ph, g_vertical, std::max(2, 2 * pw / std::max(1, ww)));
    int k = 0;
    for (auto& p : g_players) {
        if (!p.alive) continue;
        Rect l = logical[size_t(std::min(k, int(logical.size()) - 1))], px = pixels[size_t(std::min(k, int(pixels.size()) - 1))];
        k++;
        if (l.w == p.view.w && l.h == p.view.h && l.x == p.view.x && l.y == p.view.y && px.w == p.view_px.w &&
            px.h == p.view_px.h)
            continue;
        p.view = l;
        p.view_px = px;
        SendSize(p);
        LOGI("split: player %d's viewport %dx%d at %d,%d (%dx%d pixels)", p.n, l.w, l.h, l.x, l.y, px.w, px.h);
    }
}

// ---- player processes ---------------------------------------------------------------------------------------------------
std::string g_game, g_exe, g_root;

void Relay(int n, int fd) {
    std::thread([n, fd] {
        FILE* f = fdopen(fd, "r");
        char line[4096];
        while (f && std::fgets(line, sizeof line, f)) {
            size_t len = std::strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            // the player's own log lines carry their time and level; kept whole after the player's number
            const char* text = line;
            if (const char* b = std::strstr(line, "] ["); b && line[0] == '[') {
                if (const char* c = std::strchr(b + 3, ']')) text = c + 2;  // past "[time] [level] [thread]"
                if (const char* t = std::strstr(text, "] "); t && text[0] == '[') text = t + 2;
            }
            LOGI("p%d: %s", n, text);
        }
        if (f) std::fclose(f);
    }).detach();
}

void CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
}

// the launch page every match fixture has (tests/launch/README.md)
std::vector<uint8_t> LaunchPage() {
    std::vector<uint8_t> p(4096, 0);
    const uint8_t head[] = {0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x53, 0x4D};
    std::memcpy(p.data(), head, sizeof(head));
    std::memcpy(p.data() + 8, "\\Device\\CdRom0;default.xbe", 26);
    const uint8_t mid[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(p.data() + 0x210, mid, sizeof(mid));
    std::memcpy(p.data() + 0x21C, "\\Device\\CdRom0\0default.xbe", 26);
    p[0x400] = 3;
    return p;
}

// players 2..4's folders: their disk with the match line, the Z cache, their PC settings (the display settings of
// player 1's, their own name and controller bindings)
void PrepareFolder(Player& p, const std::string& line) {
    fs::path dir = p.folder;
    std::error_code ec;
    fs::remove_all(dir / "hdd", ec);
    fs::create_directories(dir / "hdd" / "Z", ec);
    const char* zcache = std::getenv("CSX_SPLIT_ZCACHE");
    if (zcache && fs::is_directory(zcache)) CopyTree(zcache, dir / "hdd" / "Z");
    else if (fs::is_directory(fs::path(g_root) / "hdd" / "Z")) CopyTree(fs::path(g_root) / "hdd" / "Z", dir / "hdd" / "Z");
    fs::remove(dir / "hdd" / "Z" / "releasemap.txt", ec);
    auto page = LaunchPage();
    std::ofstream(dir / "hdd" / "launch.bin", std::ios::binary).write(reinterpret_cast<const char*>(page.data()), long(page.size()));
    std::ofstream(dir / "hdd" / "Z" / "releasemap.txt", std::ios::binary) << line << '\0';
    // the settings that should look alike in every viewport, from player 1's (the run folder's)
    std::map<std::string, std::string> mine;
    if (std::ifstream in{dir / "pc_settings.ini"}) {
        for (std::string l; std::getline(in, l);)
            if (auto eq = l.find(" = "); eq != std::string::npos && l[0] != '#') mine[l.substr(0, eq)] = l.substr(eq + 3);
    }
    for (const char* k : {"fov", "frame_rate", "vsync", "xbox_effects", "auto_exposure", "bloom", "resolution",
                          "master_volume", "mouse_sensitivity", "mouse_invert", "net_rate", "net_update_rate", "net_cmd_rate"}) {
        std::string v = pc::Get(k, "");
        if (!v.empty()) mine[k] = v;
    }
    std::ofstream out(dir / "pc_settings.ini", std::ios::trunc);
    out << "# a split-screen player's PC settings (player 1's display settings copied at each start)\n";
    for (auto& [k, v] : mine) out << k << " = " << v << "\n";
}

void Spawn(Player& p, const std::string& line, const std::string& key, const std::string& password) {
    int mine = -1, theirs = -1;
    if (!split::Pair(mine, theirs)) return;
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return;
    if (p.n > 1) PrepareFolder(p, line);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(g_win, &ww, &wh);
    auto match = Layout(p.n == 1 ? 1 : g_configured, ww, wh, g_vertical, 2);
    const Rect& r = match[size_t(std::min(p.n, int(match.size())) - 1)];
    char aspect[32];
    std::snprintf(aspect, sizeof aspect, "%.5f", r.h > 0 ? double(r.w) / r.h : 4.0 / 3.0);
    char gain[32];
    std::snprintf(gain, sizeof gain, "%.3f", 1.0 / std::sqrt(double(std::max(1, g_configured))));
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(p.folder.c_str()) != 0) _exit(127);
        dup2(out[1], 1);
        dup2(out[1], 2);
        setenv("CSX_SPLIT_FD", std::to_string(theirs).c_str(), 1);
        setenv("CSX_SPLIT_PLAYER", std::to_string(p.n).c_str(), 1);
        setenv("CSX_SPLIT_PLAYERS", std::to_string(g_configured).c_str(), 1);
        setenv("CSX_SPLIT_GPU", g.uuid.c_str(), 1);
        setenv("CSX_RENDERER", "vulkan", 1);
        setenv("CSX_DISPLAY_ASPECT", aspect, 1);
        setenv("CSX_AUDIO_GAIN", gain, 1);
        unsetenv("CSX_SPLIT_COMPOSITOR");
        unsetenv("ENABLE_GAMESCOPE_WSI");  // a player has no surface: no window system layer
        unsetenv("CSX_TEST_INPUT_ONLY");
        unsetenv("INSTANCE");
        if (p.n > 1) {
            setenv("CSX_PLAYER_NAME", pc::Get("split_name_" + std::to_string(p.n), "Player " + std::to_string(p.n)).c_str(), 1);
            if (!key.empty()) setenv("CSX_SERVER_KEY", key.c_str(), 1);
            if (!password.empty()) setenv("CSX_NET_PASSWORD", password.c_str(), 1);
            else unsetenv("CSX_NET_PASSWORD");
            unsetenv("CSX_BROWSER_JOIN");
            unsetenv("CSX_NET_BIND");
        }
        execl(g_exe.c_str(), g_exe.c_str(), g_game.c_str(), nullptr);
        _exit(127);
    }
    ::close(theirs);
    ::close(out[1]);
    p.pid = pid;
    p.fd = mine;
    p.alive = pid > 0;
    p.view = {};
    p.view_px = {};
    p.target = line;
    Relay(p.n, out[0]);
    LOGI("split: player %d started (pid %d, %s)%s%s", p.n, int(pid), p.folder.c_str(), line.empty() ? "" : ": ",
         line.c_str());
    Relayout();
    SendFocus(p);
    // a controller already assigned to this player is attached in its process
    if (p.pad) {
        split::Message pm;
        pm.type = split::Msg::Pad;
        pm.Put(uint8_t(2));
        pm.Put(uint8_t(0));
        pm.Put(int16_t(0));
        split::Send(p.fd, pm);
    }
}

void End(Player& p, const char* why) {
    if (!p.alive) return;
    split::Message m;
    m.type = split::Msg::Quit;
    split::Send(p.fd, m);
    LOGI("split: player %d ended (%s)", p.n, why);
    for (int i = 0; i < 100 && p.pid > 0; i++) {
        if (waitpid(p.pid, nullptr, WNOHANG) == p.pid) {
            p.pid = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (p.pid > 0) kill(p.pid, SIGTERM);
    DropImages(p);
    ::close(p.fd);
    p.fd = -1;
    p.alive = false;
    p.target.clear();
    Relayout();
}

std::string g_match_line, g_match_key, g_match_password;  // where player 1 plays now ("" in the menus)

void SendPad(Player& p, uint8_t kind, uint8_t index, int16_t value);
void SendState();

// player 1 plays somewhere: players 2..N follow it there; back in the menus, they end
void OnMatch(const std::string& text) {
    LOGI("split: player 1: %s", text.c_str());
    char kind[16] = {}, addr[128] = {}, key[80] = {}, password[64] = {};
    int got = std::sscanf(text.c_str(), "%15s %127s %79s %63s", kind, addr, key, password);
    if (got >= 1 && !std::strcmp(kind, "menu")) {
        g_match_line.clear();
        for (int i = 1; i < kMaxPlayers; i++) End(g_players[size_t(i)], "player 1 is back in the menus");
        return;
    }
    if (got < 2) return;
    g_match_line = std::string("+connect ") + addr;
    g_match_key = got >= 3 ? key : "";
    g_match_password = got >= 4 && std::strcmp(password, "-") ? password : "";
    for (int i = 1; i < g_configured; i++) {
        Player& p = g_players[size_t(i)];
        if (p.alive && p.target == g_match_line) continue;  // already there (a map change brings it back by itself)
        End(p, "a new match");
        Spawn(p, g_match_line, g_match_key, g_match_password);
    }
}

// ---- controllers ----------------------------------------------------------------------------------------------------------
std::vector<SDL_Gamepad*> g_pads;

Player* PadOwner(SDL_JoystickID id) {
    for (auto& p : g_players)
        if (p.pad == id) return &p;
    return nullptr;
}

void SendPad(Player& p, uint8_t kind, uint8_t index, int16_t value) {
    if (p.fd < 0) return;
    split::Message m;
    m.type = split::Msg::Pad;
    m.Put(kind);
    m.Put(index);
    m.Put(value);
    split::Send(p.fd, m);
}

// a controller connected: the next player without one (2, 3, 4; player 1 first with split_p1_pad 1)
void AssignPad(SDL_JoystickID id) {
    if (PadOwner(id)) return;
    bool p1 = pc::Get("split_p1_pad", "0") == "1" || g_kbm != 1;
    for (auto& p : g_players) {
        if (p.pad || (p.n == 1 && !p1) || p.n > g_configured) continue;
        p.pad = id;
        LOGI("split: controller %s (%u) is player %d's", SDL_GetGamepadNameForID(id), unsigned(id), p.n);
        if (p.alive) SendPad(p, 2, 0, 0);
        return;
    }
    LOGI("split: controller %s (%u) has no player (all %d have one)", SDL_GetGamepadNameForID(id), unsigned(id),
         g_configured);
}

void UnassignPad(SDL_JoystickID id) {
    if (Player* p = PadOwner(id)) {
        LOGI("split: player %d's controller disconnected", p->n);
        if (p->alive) SendPad(*p, 3, 0, 0);
        p->pad = 0;
    }
}

// ---- input --------------------------------------------------------------------------------------------------------------
void StartToJoin(SDL_JoystickID id);
void OnSetup(const std::string& text);
void Forward(Player& p, const SDL_Event& e, const std::string& text = "") {
    if (!p.alive || p.fd < 0) return;
    split::Message m;
    m.type = split::Msg::Event;
    m.body.resize(sizeof(SDL_Event));
    std::memcpy(m.body.data(), &e, sizeof(SDL_Event));
    m.PutText(text);
    split::Send(p.fd, m);
}

void UpdateCapture() {
    Player& k = g_players[size_t(g_kbm - 1)];
    bool want = k.alive && k.wants_capture && g_capture_play && g_focus;
    if (want != g_captured) {
        if (SDL_SetWindowRelativeMouseMode(g_win, want) || !want) {
            g_captured = want;
            LOGI("split: mouse %s (player %d)", want ? "captured" : "free", g_kbm);
            for (auto& p : g_players) SendFocus(p);
        }
    }
}

void HandleInput(const SDL_Event& e) {
    Player& k = g_players[size_t(g_kbm - 1)];
    if (g_test_only) {  // the host's keyboard and mouse are not the test's
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.which != window::kInjectedDevice) return;
        if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != window::kInjectedDevice) return;
        if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            e.button.which != window::kInjectedDevice)
            return;
        if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.which != window::kInjectedDevice) return;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
            bool full = !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN);
            SDL_SetWindowFullscreen(g_win, full);
            LOGI("split: %s", full ? "fullscreen" : "windowed");
            return;
        }
        if (!e.key.repeat && e.key.key == SDLK_F1 && k.wants_capture) {
            g_capture_play = !g_capture_play;
            UpdateCapture();
            return;
        }
        [[fallthrough]];
    case SDL_EVENT_KEY_UP:
        Forward(k, e);
        break;
    case SDL_EVENT_TEXT_INPUT:
        Forward(k, e, e.text.text ? e.text.text : "");
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Event t = e;
        t.motion.x -= float(k.view.x);
        t.motion.y -= float(k.view.y);
        Forward(k, t);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!g_captured && k.wants_capture && e.button.which != window::kInjectedDevice) {
            g_capture_play = true;  // a click in the window while playing captures the mouse again
            UpdateCapture();
            break;
        }
        [[fallthrough]];
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        SDL_Event t = e;
        t.button.x -= float(k.view.x);
        t.button.y -= float(k.view.y);
        Forward(k, t);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        Forward(k, e);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        // a test run takes only its own controllers (the "pad" requests: virtual ones), none of the machine's
        if (g_test_only && !SDL_IsJoystickVirtual(e.gdevice.which)) break;
        if (SDL_Gamepad* gp = SDL_OpenGamepad(e.gdevice.which)) g_pads.push_back(gp);
        AssignPad(e.gdevice.which);
        SendState();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        UnassignPad(e.gdevice.which);
        g_pads.erase(std::remove_if(g_pads.begin(), g_pads.end(), [&](SDL_Gamepad* gp) {
                         if (SDL_GetGamepadID(gp) != e.gdevice.which) return false;
                         SDL_CloseGamepad(gp);
                         return true;
                     }),
                     g_pads.end());
        SendState();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (Player* p = PadOwner(e.gaxis.which)) SendPad(*p, 0, e.gaxis.axis, e.gaxis.value);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (Player* p = PadOwner(e.gbutton.which)) {
            SendPad(*p, 1, e.gbutton.button, e.gbutton.down ? 1 : 0);
            if (e.gbutton.down)
                LOGI("split: player %d's controller: %s", p->n,
                     SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button)));
        } else if (e.gbutton.down) {
            LOGI("split: a controller with no player: %s", SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button)));
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) StartToJoin(e.gbutton.which);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focus = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if (!g_focus) {
            SDL_Event lost = e;
            for (auto& p : g_players) Forward(p, lost);  // keys and buttons held are let go
        }
        for (auto& p : g_players) SendFocus(p);
        UpdateCapture();
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        Forward(k, e);
        break;
    default:
        break;
    }
}

// ---- the setup screen's settings, the players' devices -------------------------------------------------------------------
// what player 1's Split Screen screen shows: "slot N TEXT" lines (a device, "join" for a free slot, "-" for none)
void SendState() {
    std::string t = "players " + std::to_string(g_configured) + "\n";
    for (auto& p : g_players) {
        std::string what = "-";
        if (p.n <= g_configured) {
            std::vector<std::string> devices;
            if (p.n == g_kbm) devices.push_back("keyboard and mouse");
            if (p.pad) devices.push_back(SDL_GetGamepadNameForID(p.pad) ? SDL_GetGamepadNameForID(p.pad) : "controller");
            what = devices.empty() ? "join" : devices[0] + (devices.size() > 1 ? " and " + devices[1] : "");
        }
        t += "slot " + std::to_string(p.n) + " " + what + "\n";
    }
    Player& one = g_players[0];
    if (one.alive && one.fd >= 0) {
        split::Message m;
        m.type = split::Msg::State;
        m.PutText(t);
        split::Send(one.fd, m);
    }
}

// the controllers given out again (a change of player count, of player 1's controller setting): in connection order
void Reassign() {
    for (auto& p : g_players)
        if (p.pad) {
            if (p.alive) SendPad(p, 3, 0, 0);
            p.pad = 0;
        }
    for (SDL_Gamepad* gp : g_pads) AssignPad(SDL_GetGamepadID(gp));
}

// "players N layout horizontal|vertical p1pad 0|1" from player 1's Split Screen screen: applied at once (players
// beyond N ended; added ones join player 1's match when it plays one)
void OnSetup(const std::string& text) {
    char layout[16] = "horizontal";
    int players = g_configured, p1pad = 0;
    std::sscanf(text.c_str(), "players %d layout %15s p1pad %d", &players, layout, &p1pad);
    players = std::clamp(players, 1, kMaxPlayers);
    LOGI("split: setup: %d players, %s, player 1 %s", players, layout, p1pad ? "with a controller too" : "keyboard and mouse");
    bool pads = p1pad != (pc::Get("split_p1_pad", "0") == "1") || players != g_configured;
    g_configured = players;
    g_vertical = !std::strcmp(layout, "vertical");
    pc::Set("split_p1_pad", p1pad ? "1" : "0");
    for (int i = players; i < kMaxPlayers; i++) End(g_players[size_t(i)], "fewer local players");
    if (pads) Reassign();
    for (auto& p : g_players) p.view = {}, p.view_px = {};
    Relayout();
    if (!g_match_line.empty())
        for (int i = 1; i < g_configured; i++)
            if (!g_players[size_t(i)].alive) Spawn(g_players[size_t(i)], g_match_line, g_match_key, g_match_password);
    SendState();
}

// a controller without a player pressed START: a new local player (up to four), into player 1's match if it plays one
void StartToJoin(SDL_JoystickID id) {
    if (PadOwner(id) || g_configured >= kMaxPlayers) return;
    g_configured++;
    LOGI("split: a controller's START: %d local players", g_configured);
    AssignPad(id);
    for (auto& p : g_players) p.view = {}, p.view_px = {};
    Relayout();
    Player& p = g_players[size_t(g_configured - 1)];
    if (!g_match_line.empty() && !p.alive) Spawn(p, g_match_line, g_match_key, g_match_password);
    SendState();
}

// ---- messages from the players -----------------------------------------------------------------------------------------
void HandleMessage(Player& p, const split::Message& m) {
    size_t at = 0;
    switch (m.type) {
    case split::Msg::Hello:
        DropImages(p);  // a relaunch: its images are new ones
        p.view = {};
        p.view_px = {};
        Relayout();
        SendFocus(p);
        if (p.pad) SendPad(p, 2, 0, 0);
        if (p.n == 1) SendState();
        LOGI("split: player %d (re)started", p.n);
        break;
    case split::Msg::Images:
        if (!ImportImages(p, m)) {
            for (int f : m.fds) ::close(f);
            DropImages(p);
        }
        break;
    case split::Msg::Ready: {
        uint32_t i = 0;
        if (m.Get(at, i) && i < 3) {
            // a ready frame replaced by a newer one before it was shown goes back at once
            if (p.ready >= 0 && p.ready != int(i)) {
                split::Message r;
                r.type = split::Msg::Release;
                r.Put(uint32_t(p.ready));
                split::Send(p.fd, r);
            }
            p.ready = int(i);
        }
        break;
    }
    case split::Msg::Capture: {
        uint8_t on = 0;
        m.Get(at, on);
        p.wants_capture = on != 0;
        if (p.n == g_kbm) UpdateCapture();
        break;
    }
    case split::Msg::Match:
        if (p.n == 1) OnMatch(m.Text());
        break;
    case split::Msg::Log:
        LOGI("p%d: %s", p.n, m.Text().c_str());
        break;
    case split::Msg::Setup:
        if (p.n == 1) OnSetup(m.Text());
        break;
    default:
        break;
    }
}

// ---- the composite -------------------------------------------------------------------------------------------------------
void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t src_family,
             uint32_t dst_family, VkAccessFlags2 src_access, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

std::string g_shot;  // a window shot asked for (the "shot" file): saved from the next composite

void SaveShot(VkImage image, const std::string& name) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(g.extent.width) * g.extent.height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    vkCreateBuffer(g.dev, &bi, nullptr, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    VkDeviceMemory mem;
    vkAllocateMemory(g.dev, &ma, nullptr, &mem);
    vkBindBufferMemory(g.dev, buf, mem, 0);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g.dev, &ai, &cb);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin);
    Barrier(cb, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {g.extent.width, g.extent.height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_2_TRANSFER_READ_BIT, 0);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    void* data = nullptr;
    vkMapMemory(g.dev, mem, 0, bi.size, 0, &data);
    fs::create_directories(app::ExeDir() / "shots");
    std::string path = (app::ExeDir() / "shots" / (name + ".bmp")).string();
    if (SDL_Surface* s = SDL_CreateSurfaceFrom(int(g.extent.width), int(g.extent.height), SDL_PIXELFORMAT_ARGB8888, data,
                                               int(g.extent.width * 4))) {
        SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
        LOGI("window: shot %s", path.c_str());
    }
    vkUnmapMemory(g.dev, mem);
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
    vkDestroyBuffer(g.dev, buf, nullptr);
    vkFreeMemory(g.dev, mem, nullptr);
}

void Composite() {
    auto& f = g.frames[g.frame % 2];
    vkWaitForFences(g.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    // the frames this slot showed last time are the players' again
    for (auto [n, i] : f.release) {
        Player& p = g_players[size_t(n - 1)];
        if (p.alive && p.fd >= 0 && p.shown != i) {
            split::Message r;
            r.type = split::Msg::Release;
            r.Put(uint32_t(i));
            split::Send(p.fd, r);
        }
    }
    f.release.clear();
    uint32_t index = 0;
    VkResult ar = vkAcquireNextImageKHR(g.dev, g.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        CreateSwapchain(g_win);
        return;
    }
    vkResetFences(g.dev, 1, &f.fence);
    VkCommandBuffer cb = f.cmd;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin);
    VkImage target = g.images[index];
    Barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    Barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (auto& p : g_players) {
        if (!p.alive || !p.img_w) continue;
        if (p.ready >= 0) {  // a newer frame: shown from now; the previous one goes back after this frame
            if (p.shown >= 0) f.release.push_back({p.n, p.shown});
            p.shown = p.ready;
            p.ready = -1;
        }
        if (p.shown < 0 || p.view_px.w <= 0) continue;
        VkImage src = p.images[size_t(p.shown)].image;
        Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL,
                g.qfamily, 0, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int(p.img_w), int(p.img_h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // the player's frame in its viewport, its own aspect kept when the sizes differ for a moment (a resize)
        float sw = float(p.img_w), sh = float(p.img_h), dw = float(p.view_px.w), dh = float(p.view_px.h);
        float scale = std::min(dw / sw, dh / sh);
        int ow = int(sw * scale), oh = int(sh * scale);
        int ox = p.view_px.x + (p.view_px.w - ow) / 2, oy = p.view_px.y + (p.view_px.h - oh) / 2;
        blit.dstOffsets[0] = {ox, oy, 0};
        blit.dstOffsets[1] = {ox + ow, oy + oh, 1};
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.qfamily,
                VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_2_TRANSFER_READ_BIT, 0);
    }
    Barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.rendered[index];
    vkQueueSubmit(g.queue, 1, &si, f.fence);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    if (!g_shot.empty()) {  // the composite as presented (before the present gives the image away)
        vkWaitForFences(g.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        SaveShot(target, g_shot);
        g_shot.clear();
    }
    VkResult pr = vkQueuePresentKHR(g.queue, &pi);
    g.frame++;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR ||
        (w > 0 && h > 0 && (uint32_t(w) != g.extent.width || uint32_t(h) != g.extent.height))) {
        CreateSwapchain(g_win);
        Relayout();
    }
}

// ---- the tests' request files (as the game's window, window_main.cpp) ----------------------------------------------------
struct Held {
    double until;
    SDL_Keycode key;
    SDL_Scancode sc;
    int button;
};
std::vector<Held> g_held;
std::deque<std::string> g_typed;
std::map<int, SDL_JoystickID> g_test_pads;  // "pad N attach": virtual controllers, as a real one connected
std::map<int, SDL_Joystick*> g_test_joys;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

FILE* Request(const char* name) {
    std::string path = (app::ExeDir() / name).string();
    FILE* f = std::fopen(path.c_str(), "r");
    return f;
}
void Done(const char* name) { std::remove((app::ExeDir() / name).string().c_str()); }

void PushKey(SDL_Keycode k, SDL_Scancode sc, bool down) {
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = k;
    ev.key.scancode = sc;
    ev.key.down = down;
    ev.key.which = window::kInjectedDevice;
    ev.key.windowID = SDL_GetWindowID(g_win);
    SDL_PushEvent(&ev);
}

void PushButton(int b, bool down, float x, float y) {
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = uint8_t(b == 11 ? SDL_BUTTON_LEFT : b);
    ev.button.down = down;
    ev.button.clicks = b == 11 ? 2 : 1;
    ev.button.x = x;
    ev.button.y = y;
    ev.button.which = window::kInjectedDevice;
    ev.button.windowID = SDL_GetWindowID(g_win);
    SDL_PushEvent(&ev);
}

float g_px = 0, g_py = 0;  // the injected pointer (window units)

void PollRequests() {
    double now = Now();
    if (FILE* f = Request("keys")) {  // "NAME[#SCANCODE] SECONDS" lines
        char name[64], secs[32];
        while (std::fscanf(f, "%63s %31s", name, secs) == 2) {
            char* at = std::strchr(name, '#');
            if (at) *at++ = 0;
            SDL_Keycode k = SDL_GetKeyFromName(name);
            SDL_Scancode sc = at ? SDL_GetScancodeFromName(at) : SDL_GetScancodeFromKey(k, nullptr);
            PushKey(k, sc, true);
            g_held.push_back({now + std::atof(secs), k, sc, 0});
        }
        std::fclose(f);
        Done("keys");
    }
    if (FILE* f = Request("text")) {
        char buf[512] = {};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        Done("text");
        std::string t(buf, n);
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        g_typed.push_back(t);
        if (g_typed.size() > 16) g_typed.pop_front();
        SDL_Event ev{};
        ev.type = SDL_EVENT_TEXT_INPUT;
        ev.text.windowID = SDL_GetWindowID(g_win);
        ev.text.text = g_typed.back().c_str();
        SDL_PushEvent(&ev);
    }
    if (FILE* f = Request("mouse")) {  // "menu X Y" (player 1's menus), "window X Y", "button B SECONDS", "wheel N"
        char kind[16];
        float a = 0, b = 0;
        while (std::fscanf(f, "%15s", kind) == 1) {
            if (!std::strcmp(kind, "menu") || !std::strcmp(kind, "window")) {
                if (std::fscanf(f, "%f %f", &a, &b) != 2) break;
                Player& k = g_players[size_t(g_kbm - 1)];
                if (!std::strcmp(kind, "menu")) {  // the menus' 640x480 in the keyboard player's viewport, letterboxed
                    float vw = float(k.view.w), vh = float(k.view.h);
                    float aspect = std::clamp(vh > 0 ? vw / vh : 4.0f / 3.0f, 4.0f / 3.0f, 2.5f);
                    float tw = vw, th = tw / aspect;
                    if (th > vh) {
                        th = vh;
                        tw = th * aspect;
                    }
                    float frame_w = 480.0f * aspect;
                    a = float(k.view.x) + (vw - tw) / 2 + (a + (frame_w - 640.0f) / 2) / frame_w * tw;
                    b = float(k.view.y) + (vh - th) / 2 + b / 480.0f * th;
                }
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.windowID = SDL_GetWindowID(g_win);
                ev.motion.which = window::kInjectedDevice;
                ev.motion.x = a;
                ev.motion.y = b;
                ev.motion.xrel = a - g_px;
                ev.motion.yrel = b - g_py;
                g_px = a;
                g_py = b;
                SDL_PushEvent(&ev);
            } else if (!std::strcmp(kind, "button")) {
                int btn = 1;
                char secs[32] = "0.12";
                if (std::fscanf(f, "%d %31s", &btn, secs) < 1) break;
                PushButton(btn, true, g_px, g_py);
                g_held.push_back({now + std::atof(secs), 0, SDL_SCANCODE_UNKNOWN, btn});
            } else if (!std::strcmp(kind, "wheel")) {
                int n = 0;
                if (std::fscanf(f, "%d", &n) != 1) break;
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_WHEEL;
                ev.wheel.windowID = SDL_GetWindowID(g_win);
                ev.wheel.which = window::kInjectedDevice;
                ev.wheel.y = float(n);
                ev.wheel.integer_y = n;
                SDL_PushEvent(&ev);
            }
        }
        std::fclose(f);
        Done("mouse");
    }
    // "pad N attach | detach | button NAME SECONDS | axis NAME VALUE": test controller N, a virtual SDL gamepad here, so
    // it reaches its player exactly as a real one would
    if (FILE* f = Request("pad")) {
        int n = 0;
        char what[16], name[32], value[32];
        while (std::fscanf(f, "%d %15s", &n, what) == 2) {
            if (!std::strcmp(what, "attach") && !g_test_pads.count(n)) {
                SDL_VirtualJoystickDesc d;
                SDL_INIT_INTERFACE(&d);
                d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                d.naxes = SDL_GAMEPAD_AXIS_COUNT;
                d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                d.name = "Test controller";
                SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
                g_test_pads[n] = id;
                g_test_joys[n] = SDL_OpenJoystick(id);
                LOGI("split: test controller %d attached (%u)", n, unsigned(id));
            } else if (!std::strcmp(what, "detach") && g_test_pads.count(n)) {
                SDL_CloseJoystick(g_test_joys[n]);
                SDL_DetachVirtualJoystick(g_test_pads[n]);
                g_test_pads.erase(n);
                g_test_joys.erase(n);
                LOGI("split: test controller %d detached", n);
            } else if (!std::strcmp(what, "button") && std::fscanf(f, "%31s %31s", name, value) == 2 && g_test_joys.count(n)) {
                SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
                if (b != SDL_GAMEPAD_BUTTON_INVALID) {
                    SDL_SetJoystickVirtualButton(g_test_joys[n], b, true);
                    g_held.push_back({now + std::atof(value), SDL_Keycode(1000 + n * 100 + b), SDL_SCANCODE_UNKNOWN, -1});
                }
            } else if (!std::strcmp(what, "axis") && std::fscanf(f, "%31s %31s", name, value) == 2 && g_test_joys.count(n)) {
                SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
                if (a != SDL_GAMEPAD_AXIS_INVALID) SDL_SetJoystickVirtualAxis(g_test_joys[n], a, int16_t(std::atoi(value)));
            }
        }
        std::fclose(f);
        Done("pad");
    }
    if (FILE* f = Request("shot")) {
        char name[64] = {};
        if (std::fscanf(f, "%63s", name) == 1) g_shot = name;
        std::fclose(f);
        Done("shot");
    }
    for (size_t i = 0; i < g_held.size();) {
        if (now < g_held[i].until) {
            i++;
            continue;
        }
        const Held& h = g_held[i];
        if (h.button > 0) PushButton(h.button, false, g_px, g_py);
        else if (h.button < 0) {  // a test controller's button
            int code = int(h.key) - 1000, n = code / 100, b = code % 100;
            if (g_test_joys.count(n)) SDL_SetJoystickVirtualButton(g_test_joys[n], b, false);
            LOGI("split: test controller %d released button %d", n, b);
        } else PushKey(h.key, h.sc, false);
        g_held.erase(g_held.begin() + long(i));
    }
}

}  // namespace

namespace {
// the loopback interface up (player 1's offline server is on 127.0.0.1): the game's sandbox (run_game.sh, the tests:
// unshare -rn) starts a network namespace with it down; a normal namespace has it up already (and refuses, harmlessly)
void LoopbackUp() {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    ifreq r{};
    std::strncpy(r.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &r) == 0 && !(r.ifr_flags & IFF_UP)) {
        r.ifr_flags |= IFF_UP;
        LOGI("split: loopback %s", ioctl(s, SIOCSIFFLAGS, &r) == 0 ? "brought up" : "down (cannot bring it up)");
    }
    ::close(s);
}
}  // namespace

// the split-screen compositor's main (window_main.cpp starts it for CSX_SPLIT_COMPOSITOR=1)
int CompositorMain(const std::string& game) {
    logx::Init((app::ExeDir() / "logs").string(), "split");
    signal(SIGPIPE, SIG_IGN);
    LoopbackUp();
    g_game = game;
    g_root = app::ExeDir().string();
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    g_exe = n > 0 ? std::string(exe, size_t(n)) : "";
    g_configured = std::clamp(std::getenv("CSX_SPLIT_PLAYERS") ? std::atoi(std::getenv("CSX_SPLIT_PLAYERS")) : 2, 1, 4);
    g_vertical = pc::Get("split_layout", "horizontal") == "vertical";
    g_kbm = std::clamp(std::atoi(pc::Get("split_kbm", "1").c_str()), 1, 4);
    g_test_only = std::getenv("CSX_TEST_INPUT_ONLY") && *std::getenv("CSX_TEST_INPUT_ONLY") == '1';
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        LOGE("split: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, pc::Get("mouse_raw", "1") != "0" ? "0" : "1");
    int w = 1280, h = 720;
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->w > 0) {
        w = dm->w * 9 / 10;
        h = dm->h * 9 / 10;
    }
    g_win = SDL_CreateWindow("Counter-Strike (split-screen)", w, h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win || !CreateGpu(g_win) || !CreateSwapchain(g_win)) {
        LOGE("split: no window or no Vulkan device for the compositor: %s", SDL_GetError());
        return 1;
    }
    if (pc::Get("display_mode", "windowed") == "fullscreen") SDL_SetWindowFullscreen(g_win, true);
    LOGI("split: compositor for %d local players, layout %s, keyboard and mouse player %d", g_configured,
         g_vertical ? "vertical" : "horizontal", g_kbm);
    for (int i = 0; i < kMaxPlayers; i++) {
        g_players[size_t(i)].n = i + 1;
        g_players[size_t(i)].folder = i == 0 ? g_root : (fs::path(g_root) / ("split" + std::to_string(i + 1))).string();
        if (i > 0) fs::create_directories(g_players[size_t(i)].folder);
    }
    Spawn(g_players[0], "", "", "");
    bool quit = false;
    auto last_flush = std::chrono::steady_clock::now();
    while (!quit) {
        PollRequests();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) Relayout();
            HandleInput(e);
        }
        // the players' messages
        std::vector<pollfd> fds;
        for (auto& p : g_players)
            if (p.alive && p.fd >= 0) fds.push_back({p.fd, POLLIN, 0});
        if (!fds.empty()) ::poll(fds.data(), nfds_t(fds.size()), 0);
        for (auto& pf : fds) {
            Player* p = nullptr;
            for (auto& q : g_players)
                if (q.fd == pf.fd) p = &q;
            if (!p) continue;
            if (pf.revents & POLLIN) {
                split::Message m;
                bool again = false;
                split::SetNonBlocking(p->fd);
                while (split::Receive(p->fd, m, &again)) HandleMessage(*p, m);
                if (!again) pf.revents |= POLLHUP;
            }
            if (pf.revents & (POLLHUP | POLLERR)) {
                int status = 0;
                if (p->pid > 0) waitpid(p->pid, &status, 0);
                LOGI("split: player %d's process ended (%s %d)", p->n, WIFSIGNALED(status) ? "signal" : "exit",
                     WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                p->pid = 0;
                DropImages(*p);
                ::close(p->fd);
                p->fd = -1;
                p->alive = false;
                p->target.clear();
                if (p->n == 1) quit = true;  // player 1 is the game: its end is the compositor's
                Relayout();
            }
        }
        Composite();
        auto now = std::chrono::steady_clock::now();
        if (now - last_flush > std::chrono::seconds(1)) {
            logx::Flush();
            last_flush = now;
        }
    }
    for (auto& p : g_players) End(p, "the window closed");
    LOGI("split: closed");
    logx::Flush();
    return 0;
}
