// Vulkan gpu::Backend (Vulkan 1.3: dynamic rendering, synchronization2, push descriptors), presenting into the SDL3
// window. Draws go through the runtime's shared parts: vertex assembly (vertex.cpp), texture decoding (texture.cpp),
// the HLSL generator (shaders.cpp) compiled to SPIR-V with DXC, and the constants (draw.cpp).
// docs/vulkan_plan.md has the design; this file is milestone 1 and grows by milestone.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "dxgiformat.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "linux/window.h"
#include "vulkan/vk_shader.h"

namespace gpu {

namespace {

#define VKCHECK(x)                                                         \
    do {                                                                   \
        VkResult r_ = (x);                                                 \
        if (r_ != VK_SUCCESS) LOGE("vulkan: %s failed (%d)", #x, int(r_)); \
    } while (0)

constexpr uint32_t kFrames = 2;
constexpr VkDeviceSize kUploadSize = 64u << 20;
constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;  // RADV has no D24S8; quantised in the shader later

// D3DTEXF and D3DTADDRESS values the samplers read
constexpr uint32_t kTexfLinear = 2, kTexfAnisotropic = 3;

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// Depth: RADV has no 24 bit depth format, the targets are D32F. The console compares integer depth (24 bit, 16 for
// D16), and CS depends on it: its menus draw pretransformed quads at z 0.5 units over text whose z is 0 and never clear
// depth, which only passes LESSEQUAL once 0.5 truncates to 0 (FINDINGS.md). The generated vertex shaders give z in
// depth units (zscale per 1.0, surf.z = 1 / zscale), so it is floored to whole units before it is normalised, as the
// reference renderer floors each fragment's. Per vertex here, which is exact for the flat quads that need it.
std::string QuantiseDepth(std::string src) {
    auto sub = [&](const char* from, const char* to) {
        size_t at = src.find(from);
        if (at == std::string::npos) return false;
        src.replace(at, std::strlen(from), to);
        return true;
    };
    bool done = sub("    q.z = p.z * surf.z;\n", "    q.z = floor(p.z) * surf.z;\n") ||
                sub("    o.pos = cp;\n",
                    "    if (cp.w > 0) cp.z = floor(cp.z / cp.w / surf.z) * surf.z * cp.w;\n    o.pos = cp;\n");
    if (!done) LOG_FIRST(4, "vulkan: a vertex shader without the depth line the quantisation expects");
    return src;
}

// the console's GL compare values 0x200..0x207 in Vulkan's order (never, less, equal, ...), which is the same
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// the vertex formats vertex.cpp hands over (DXGI codes) as Vulkan formats
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat TextureFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        case Host::R16Unorm: return VK_FORMAT_R16_UNORM;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false;
};

struct Target : Image {
    uint32_t data = 0;
    uint32_t pitch = 0;   // guest bytes per row, for writing results back (linear 32 bit surfaces)
    bool linear32 = false;
};

struct Texture : Image {
    TextureDesc desc;
    uint64_t sig = 0, frame = 0, palette = 0;
};

// what a pipeline is made for; hashed whole
struct PipelineKey {
    uint64_t vs, ps;
    uint32_t regs, buffers;
    uint32_t elements[16];  // format, buffer << 8, offset << 16
    uint32_t strides[16];
    uint32_t topo, strip, has_rt, has_ds;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    // polygon offset (decals): the console's bias in normalised depth (bias / zscale) and its slope factor
    float zbias;
    float zslope;
};

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize used = 0;
    std::vector<std::pair<VkImage, VkDeviceMemory>> garbage;
    std::vector<VkImageView> garbage_views;
    bool timed = false;  // both timestamps written for the frame last recorded in this slot
};

// pixel shader keys share the shader map with vertex shader keys; the salt keeps them apart (as in the D3D12 renderer)
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// adds the time to the end of the scope to a total, in nanoseconds
struct Span {
    double& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Span() { total += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count(); }
};

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void SetOverlay(const OverlayImage& o) override {
        overlay_ = o;
        overlay_pixels_.assign(o.pixels ? o.pixels : nullptr, o.pixels ? o.pixels + size_t(o.width) * o.height : nullptr);
        overlay_.pixels = overlay_pixels_.data();
    }
    void Present(const Surface& front) override;
    // the CPU is about to read a resource: a render target's GPU result written to its guest memory first (the title
    // reads its front buffer for PersistDisplay before a relaunch)
    void Sync(uint32_t va, uint32_t size) override;
    void SyncLate(uint32_t) override {}
    void Flush() override { Submit(true); }
    // the title's gamma ramp (3 x 256 bytes), applied at the present as the console's display hardware does
    void SetGamma(const uint8_t* r) override {
        std::memcpy(gamma_, r, sizeof(gamma_));
        gamma_identity_ = true;
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 256; i++) gamma_identity_ = gamma_identity_ && r[c * 256 + i] == i;
        gamma_dirty_ = true;
    }

private:
    bool CreateInstance();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    void CreateFrames();
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    bool MakeImage(Image& im, uint32_t w, uint32_t h, VkFormat fmt, uint32_t levels, VkImageUsageFlags usage, bool depth);
    void FreeImage(Image& im);
    void Barrier(Image& im, VkImageLayout to);
    VkCommandBuffer Cmd();
    void Submit(bool wait);
    struct Alloc {
        VkDeviceSize offset;
        uint8_t* cpu;
    };
    Alloc Upload(VkDeviceSize size, VkDeviceSize align);
    Target* EnsureTarget(const Surface& s, bool depth);
    void BeginRendering(Target* rt, Target* ds);
    void EndRendering();
    Image* EnsureTexture(const State& s, int stage, Target* drawing, TexInfo& info, uint32_t& w, uint32_t& h);
    Target* FindTarget(uint32_t data, uint32_t w, uint32_t h);
    VkSampler EnsureSampler(const State& s, int stage, const TexInfo& info);
    VkPipeline EnsurePipeline(const PipelineKey& k, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps);
    // compiled SPIR-V (owned by vk_shader's cache) and its hash, by shader key
    struct Shader {
        const std::vector<uint32_t>* code = nullptr;
        uint64_t hash = 0;
    };
    std::unordered_map<uint64_t, Shader> shaders_;
    template <typename Source>
    const Shader& EnsureShader(uint64_t key, vk_shader::Stage stage, Source&& source) {
        Shader& sh = shaders_[key];
        if (!sh.code) {
            sh.code = &vk_shader::Compile(source(), stage);
            sh.hash = Hash(sh.code->data(), sh.code->size() * 4);
        }
        return sh;
    }
    void SaveShot(Image& t, const std::string& name);
    Image* ComposeOverlay(Image& t);
    Image* ApplyGamma(Image& src);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t qfamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops_{};
    PFN_vkCmdPushDescriptorSetKHR push_descriptors_ = nullptr;
    // VK_EXT_depth_bias_control with a float representation: a depth bias added as it is, in normalised depth
    bool bias_float_ = false;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images_;
    std::vector<VkSemaphore> swap_ready_;  // per swapchain image: rendering done, present may run
    VkExtent2D swap_extent_{};

    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<Frame, kFrames> frames_{};
    uint64_t frame_ = 0;
    bool recording_ = false;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    Image dummy_;  // 1x1 for unused texture slots
    // the video overlay (movies): its pixels, an image of them, and the presented target with it composited
    OverlayImage overlay_;
    std::vector<uint32_t> overlay_pixels_;
    Image overlay_img_, overlay_target_;
    // the gamma ramp, its 256 x 1 lookup image and the presented image mapped through it
    uint8_t gamma_[768] = {};
    bool gamma_identity_ = true, gamma_dirty_ = false;
    Image gamma_lut_;
    Target gamma_target_;
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;

    Target* rendering_rt_ = nullptr;
    Target* rendering_ds_ = nullptr;
    bool rendering_ = false;
    uint64_t presents_ = 0;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    unsigned long long draws_ = 0, dropped_[4] = {}, wraps_ = 0, peak_ = 0, frame_draws_ = 0;
    // performance: GPU time from timestamps, CPU time inside Clear / Draw (recording) and Present, per logged span
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    float timestamp_ns_ = 1;
    bool frame_open_ = false;
    double gpu_ns_ = 0, record_ns_ = 0, present_ns_ = 0;
    unsigned gpu_frames_ = 0;
    std::string step_name_;  // totals since start: draws, and draws not recorded by cause
};

VulkanBackend::~VulkanBackend() {
    if (dev_) vkDeviceWaitIdle(dev_);
}

bool VulkanBackend::CreateInstance() {
    uint32_t n = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> ext(sdl_ext, sdl_ext + n);
    std::vector<const char*> layers;
    if (const char* v = std::getenv("CSX_VK_VALIDATION"); v && *v == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Counter-Strike";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        LOGE("vulkan: no Vulkan 1.3 instance");
        return false;
    }
    auto* win = static_cast<SDL_Window*>(window::SdlWindow());
    if (!win || !SDL_Vulkan_CreateSurface(win, instance_, nullptr, &surface_)) {
        LOGE("vulkan: no surface for the window: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    for (int pass = 0; pass < 2 && !gpu_; pass++) {
        for (VkPhysicalDevice g : gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(g, &p);
            if (p.apiVersion < VK_API_VERSION_1_3) continue;
            if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
            for (uint32_t i = 0; i < qn; i++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface_, &present);
                if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    gpu_ = g;
                    qfamily_ = i;
                    LOGI("vulkan: %s, driver %u.%u.%u, API %u.%u.%u", p.deviceName, VK_VERSION_MAJOR(p.driverVersion),
                         VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion),
                         VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
                    break;
                }
            }
            if (gpu_) break;
        }
    }
    if (!gpu_) {
        LOGE("vulkan: no Vulkan 1.3 GPU that presents to the window");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memprops_);
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(gpu_, &have);
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    f2.features.geometryShader = have.geometryShader;
    f2.features.fillModeNonSolid = have.fillModeNonSolid;
    f2.features.depthBiasClamp = have.depthBiasClamp;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f2.pNext = &f13;
    // the console offsets depth by whole units of its integer buffer; the targets here are D32F, whose constant bias
    // scales with each primitive's depth exponent, so the bias is given as a float in normalised depth instead
    VkPhysicalDeviceDepthBiasControlFeaturesEXT bias_have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, props.data());
        for (const auto& e : props)
            if (!std::strcmp(e.extensionName, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)) {
                VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bias_have};
                vkGetPhysicalDeviceFeatures2(gpu_, &q);
                bias_float_ = bias_have.depthBiasControl && bias_have.floatRepresentation && bias_have.depthBiasExact;
            }
    }
    VkPhysicalDeviceDepthBiasControlFeaturesEXT bias_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
    bias_on.depthBiasControl = bias_on.floatRepresentation = bias_on.depthBiasExact = VK_TRUE;
    if (bias_float_) f13.pNext = &bias_on;
    LOGI("vulkan: polygon offset %s", bias_float_ ? "as a float depth bias (VK_EXT_depth_bias_control)"
                                                   : "approximated in 24 bit units (no VK_EXT_depth_bias_control)");
    std::vector<const char*> ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (bias_float_) ext.push_back(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    if (vkCreateDevice(gpu_, &ci, nullptr, &dev_) != VK_SUCCESS) {
        LOGE("vulkan: device creation failed (dynamic rendering, synchronization2, push descriptors needed)");
        return false;
    }
    vkGetDeviceQueue(dev_, qfamily_, 0, &queue_);
    push_descriptors_ =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));
    return push_descriptors_ != nullptr;
}

bool VulkanBackend::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window::SdlWindow()), &w, &h);
        ext = {uint32_t(w), uint32_t(h)};
    }
    if (!ext.width || !ext.height) return false;  // minimised
    uint32_t count = std::max<uint32_t>(caps.minImageCount + 1, 3);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = kColorFormat;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO (vsync, always available); CSX_VK_PRESENT=immediate or mailbox when the surface offers it, for measuring
    // what the renderer sustains past the display's rate (the title's own pacing still applies, FPS=0 lifts it)
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (const char* want = std::getenv("CSX_VK_PRESENT")) {
        VkPresentModeKHR mode = std::strcmp(want, "immediate") == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                : std::strcmp(want, "mailbox") == 0 ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                    : VK_PRESENT_MODE_FIFO_KHR;
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
        if (std::find(modes.begin(), modes.end(), mode) != modes.end()) ci.presentMode = mode;
        LOG_FIRST(1, "vulkan: present mode %s %s", want, ci.presentMode == mode ? "in use" : "not offered, FIFO");
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &sc) != VK_SUCCESS) {
        LOGE("vulkan: swapchain creation failed");
        return false;
    }
    DestroySwapchain();
    swapchain_ = sc;
    swap_extent_ = ext;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
    swap_images_.resize(n);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, swap_images_.data());
    swap_ready_.resize(n);
    for (auto& s : swap_ready_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev_, &si, nullptr, &s);
    }
    LOGI("vulkan: swapchain %ux%u, %u images", ext.width, ext.height, n);
    return true;
}

void VulkanBackend::DestroySwapchain() {
    if (!swapchain_) return;
    vkDeviceWaitIdle(dev_);
    for (auto s : swap_ready_) vkDestroySemaphore(dev_, s, nullptr);
    swap_ready_.clear();
    vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[10] = {};
    for (uint32_t i = 0; i < 10; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 10;
    sl.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &sl, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    VKCHECK(vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VKCHECK(vkCreateSampler(dev_, &si, nullptr, &dummy_sampler_));
    return true;
}

void VulkanBackend::CreateFrames() {
    // two timestamps per frame slot: the frame's GPU time, read when the slot comes round again
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    timestamp_ns_ = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * kFrames;
    if (vkCreateQueryPool(dev_, &qi, nullptr, &timestamps_) != VK_SUCCESS) timestamps_ = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = qfamily_;
    VKCHECK(vkCreateCommandPool(dev_, &pc, nullptr, &pool_));
    for (auto& f : frames_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(dev_, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(dev_, &si, nullptr, &f.acquired));
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kUploadSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VKCHECK(vkCreateBuffer(dev_, &bi, nullptr, &f.upload));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev_, f.upload, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKCHECK(vkAllocateMemory(dev_, &ma, nullptr, &f.upload_memory));
        VKCHECK(vkBindBufferMemory(dev_, f.upload, f.upload_memory, 0));
        VKCHECK(vkMapMemory(dev_, f.upload_memory, 0, kUploadSize, 0, reinterpret_cast<void**>(&f.map)));
    }
}

uint32_t VulkanBackend::MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if ((bits >> i & 1) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return i;
    LOGE("vulkan: no memory type %X for %X", want, bits);
    return 0;
}

bool VulkanBackend::MakeImage(Image& im, uint32_t w, uint32_t h, VkFormat fmt, uint32_t levels, VkImageUsageFlags usage,
                              bool depth) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev_, im.image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ma, nullptr, &im.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev_, im.image, im.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, levels,
                           0, 1};
    if (vkCreateImageView(dev_, &vi, nullptr, &im.view) != VK_SUCCESS) return false;
    im.format = fmt;
    im.w = w;
    im.h = h;
    im.levels = levels;
    im.depth = depth;
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// freed once the GPU is past the frame that may still use it
void VulkanBackend::FreeImage(Image& im) {
    Frame& f = frames_[frame_ % kFrames];
    if (im.view) f.garbage_views.push_back(im.view);
    if (im.image) f.garbage.push_back({im.image, im.memory});
    im = Image{};
}

void VulkanBackend::Barrier(Image& im, VkImageLayout to) {
    if (im.layout == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.image = im.image;
    b.subresourceRange = {
        VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
        0, im.levels, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(Cmd(), &d);
    im.layout = to;
}

// the frame's command buffer, recording; starting a frame waits until the GPU is done with its previous use
VkCommandBuffer VulkanBackend::Cmd() {
    Frame& f = frames_[frame_ % kFrames];
    if (!recording_) {
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        for (VkImageView v : f.garbage_views) vkDestroyImageView(dev_, v, nullptr);
        for (auto& [img, mem] : f.garbage) {
            vkDestroyImage(dev_, img, nullptr);
            vkFreeMemory(dev_, mem, nullptr);
        }
        f.garbage.clear();
        f.garbage_views.clear();
        f.used = 0;
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        recording_ = true;
        if (!frame_open_ && timestamps_) {
            // this slot's previous frame is done (its fence waited above): its GPU time, then the new frame's start
            uint32_t q = uint32_t(frame_ % kFrames) * 2;
            uint64_t t[2];
            if (f.timed && vkGetQueryPoolResults(dev_, timestamps_, q, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                gpu_ns_ += double(t[1] - t[0]) * timestamp_ns_;
                gpu_frames_++;
            }
            vkCmdResetQueryPool(f.cmd, timestamps_, q, 2);
            vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, q);
            f.timed = false;
        }
        frame_open_ = true;
    }
    return f.cmd;
}

// mid-frame submit (the upload ring ran out, a flush): the same frame slot goes on with a fresh command buffer
void VulkanBackend::Submit(bool wait) {
    if (!recording_) return;
    EndRendering();
    Frame& f = frames_[frame_ % kFrames];
    vkEndCommandBuffer(f.cmd);
    vkResetFences(dev_, 1, &f.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    VKCHECK(vkQueueSubmit(queue_, 1, &si, f.fence));
    recording_ = false;
    if (wait) vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
}

VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    Cmd();
    Frame* f = &frames_[frame_ % kFrames];
    VkDeviceSize off = (f->used + align - 1) & ~(align - 1);
    if (off + size > kUploadSize) {
        wraps_++;
        bool was = rendering_;
        Target *rt = rendering_rt_, *ds = rendering_ds_;
        Submit(true);
        Cmd();
        f = &frames_[frame_ % kFrames];
        off = 0;
        if (was) BeginRendering(rt, ds);
    }
    f->used = off + size;
    peak_ = std::max<unsigned long long>(peak_, f->used);
    return {off, f->map + off};
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t w = 0, h = 0;
    SurfaceSize(s, w, h);
    uint64_t key = uint64_t(s.data) | (depth ? 1ull << 63 : 0) | uint64_t(w) << 32 | uint64_t(h) << 46;
    Target& t = targets_[key];
    if (!t.image) {
        VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!MakeImage(t, w, h, depth ? kDepthFormat : kColorFormat, 1, usage, depth)) {
            LOGE("vulkan: target %ux%u failed", w, h);
            targets_.erase(key);
            return nullptr;
        }
        t.data = s.data;
        TextureDesc d;
        if (!depth && DescribeSurface(s, d) && d.linear && Format(d.format).bits == 32 && Format(d.format).host == Host::BGRA8) {
            t.linear32 = true;
            t.pitch = d.pitch ? d.pitch : w * 4;
        }
        LOGI("vulkan: %s target %08X %ux%u", depth ? "depth" : "colour", s.data, w, h);
        // a new target starts cleared, as fresh console memory reads; its guest contents are not loaded yet
        VkCommandBuffer cb = Cmd();
        EndRendering();
        Barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (depth) {
            VkClearDepthStencilValue v{1.0f, 0};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        } else {
            VkClearColorValue v{};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return &t;
}

void VulkanBackend::BeginRendering(Target* rt, Target* ds) {
    if (rendering_ && rt == rendering_rt_ && ds == rendering_ds_) return;
    EndRendering();
    VkCommandBuffer cb = Cmd();
    if (rt) Barrier(*rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (ds) Barrier(*ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    Image* any = rt ? static_cast<Image*>(rt) : ds;
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (rt) {
        ca.imageView = rt->view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (ds) {
        da.imageView = ds->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    rendering_ = true;
    rendering_rt_ = rt;
    rendering_ds_ = ds;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    vkCmdEndRendering(frames_[frame_ % kFrames].cmd);
    rendering_ = false;
    rendering_rt_ = rendering_ds_ = nullptr;
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    if (!CreateInstance() || !CreateDevice() || !CreateLayouts()) return false;
    CreateFrames();
    if (!CreateSwapchain()) return false;
    MakeImage(dummy_, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              false);
    Barrier(dummy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue v{};
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(Cmd(), dummy_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    Barrier(dummy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LOGI("renderer: vulkan, %ux%u", width, height);
    return true;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    Span span{record_ns_};
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;
    // the bound pair, so the clear runs inside the same rendering scope draws use
    BeginRendering(s.rt.header ? EnsureTarget(s.rt, false) : nullptr, s.zs.header ? EnsureTarget(s.zs, true) : nullptr);
    VkClearAttachment at[2];
    uint32_t n = 0;
    if (rt) {
        at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        at[n].colorAttachment = 0;
        at[n].clearValue.color = {{((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f,
                                   (color >> 24) / 255.0f}};
        n++;
    }
    if (ds) {
        at[n].aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                           ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        at[n].clearValue.depthStencil = {z, stencil & 0xFF};
        n++;
    }
    Image* any = rendering_rt_ ? static_cast<Image*>(rendering_rt_) : rendering_ds_;
    std::vector<VkClearRect> cr;
    auto add = [&](int x0, int y0, int x1, int y1) {
        x0 = std::clamp(x0, 0, int(any->w));
        x1 = std::clamp(x1, 0, int(any->w));
        y0 = std::clamp(y0, 0, int(any->h));
        y1 = std::clamp(y1, 0, int(any->h));
        if (x1 > x0 && y1 > y0) cr.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + i * 16, 16);
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        add(int(s.viewport.x), int(s.viewport.y), int(s.viewport.x + s.viewport.w), int(s.viewport.y + s.viewport.h));
    }
    if (!cr.empty()) vkCmdClearAttachments(Cmd(), n, at, uint32_t(cr.size()), cr.data());
}

// palette of a P8 stage: 256 colours, fewer by the size code in the top two bits (as the D3D12 renderer reads it)
uint64_t ReadPalette(const State& s, int stage, const TextureDesc& d, uint32_t (&colours)[256], uint32_t& va) {
    constexpr uint32_t kFmtP8 = 0x0B;
    va = 0;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return 0;
    uint32_t header = s.palettes[size_t(stage)];
    va = mem::VirtualFromPhysical(kernel::R32(header + 4));
    uint32_t size_code = kernel::R32(header) >> 30;
    std::memset(colours, 0, sizeof(colours));
    std::memcpy(colours, kernel::HostPtr(va), (256u >> size_code) * 4);
    return Hash(colours, sizeof(colours));
}

// a cheap signature of guest bytes: 64 samples plus the size
uint64_t Signature(uint32_t data, uint32_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t h = Hash(&bytes, 4);
    if (bytes < 4096) return Hash(p, bytes, h);
    for (uint32_t i = 0; i < 64; i++) h = Hash(p + uint64_t(bytes - 64) * i / 63, 64, h);
    return h;
}

// a colour target with that guest surface: what the title renders and then samples (CS's blur chain renders to
// 320x240, 320x120, 160x120 and 40x60 surfaces and draws them back); its guest memory never holds the GPU's result
Target* VulkanBackend::FindTarget(uint32_t data, uint32_t w, uint32_t h) {
    uint64_t key = uint64_t(data) | uint64_t(w) << 32 | uint64_t(h) << 46;
    auto it = targets_.find(key);
    return it == targets_.end() ? nullptr : &it->second;
}

// drawing: the draw's colour target, which a texture must not be (the rendering scope open may be the last draw's)
Image* VulkanBackend::EnsureTexture(const State& s, int stage, Target* drawing, TexInfo& info, uint32_t& w, uint32_t& h) {
    info = {};
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    w = d.width;
    h = d.height;
    if (!d.cube && d.depth == 1 && !Format(d.format).compressed && !Format(d.format).depth) {
        // the texture's data is a virtual address (0x8xxxxxxx), a surface's the physical one the header holds
        if (Target* t = FindTarget(mem::PhysicalFromVirtual(d.data), d.width, d.height)) {
            info.kind = kTex2D;
            info.rect = d.linear;
            if (t == drawing) {
                LOG_FIRST(4, "vulkan: a draw samples the target it renders to (feedback copy not done yet)");
                return nullptr;
            }
            EndRendering();
            Barrier(*t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return t;
        }
    }
    if (d.cube || d.depth > 1) {
        LOG_FIRST(4, "vulkan: cube and volume textures are not drawn yet");
        return nullptr;
    }
    const FormatInfo& fi = Format(d.format);
    if (fi.depth) {
        LOG_FIRST(4, "vulkan: depth textures (shadow maps) are not drawn yet");
        return nullptr;
    }
    info.kind = kTex2D;
    info.rect = d.linear;
    uint32_t colours[256];
    uint32_t pva = 0;
    uint64_t phash = ReadPalette(s, stage, d, colours, pva);
    uint64_t key = uint64_t(pva) << 32 | d.data;
    Texture& t = textures_[key];
    bool same = t.image && t.desc.format == d.format && t.desc.width == d.width && t.desc.height == d.height &&
                t.desc.levels == d.levels;
    if (!same) {
        if (t.image) FreeImage(t);
        uint32_t levels = std::max<uint32_t>(d.levels, 1);
        if (fi.compressed && ((d.width & 3) || (d.height & 3))) levels = 1;
        if (!MakeImage(t, d.width, d.height, TextureFormat(fi.host), levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) {
            textures_.erase(key);
            return nullptr;
        }
        t.desc = d;
        t.sig = 0;
    }
    if (t.frame != presents_ + 1 || !same || t.palette != phash) {
        t.frame = presents_ + 1;
        t.palette = phash;
        uint64_t sig = Signature(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ phash;
        if (sig != t.sig) {
            t.sig = sig;
            EndRendering();
            Barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
            for (uint32_t level = 0; level < t.levels; level++) {
                std::vector<uint8_t> px;
                uint32_t pitch = 0;
                DecodeLevel(d, level, 0, guest, pva ? colours : nullptr, px, pitch);
                if (px.empty()) continue;
                Alloc a = Upload(px.size(), 16);
                std::memcpy(a.cpu, px.data(), px.size());
                VkBufferImageCopy c{};
                c.bufferOffset = a.offset;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                c.imageExtent = {std::max<uint32_t>(d.width >> level, 1), std::max<uint32_t>(d.height >> level, 1), 1};
                vkCmdCopyBufferToImage(Cmd(), frames_[frame_ % kFrames].upload, t.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            Barrier(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    return &t;
}

VkSampler VulkanBackend::EnsureSampler(const State& s, int stage, const TexInfo& info) {
    const auto& t = s.tss[size_t(stage)];
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    si.magFilter = lin(t[kTssMagFilter]) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = lin(t[kTssMinFilter]) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = t[kTssMipFilter] == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = Address(t[kTssAddressU]);
    si.addressModeV = Address(t[kTssAddressV]);
    si.addressModeW = Address(t[kTssAddressW]);
    si.mipLodBias = Bits(t[kTssMipLodBias]);
    si.minLod = float(t[kTssMaxMipLevel]);
    si.maxLod = t[kTssMipFilter] ? VK_LOD_CLAMP_NONE : si.minLod;
    uint32_t mn = t[kTssMinFilter], mg = t[kTssMagFilter];
    if (mn == kTexfAnisotropic || mg == kTexfAnisotropic) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = float(std::clamp<uint32_t>(t[kTssMaxAnisotropy], 1, 16));
    }
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;  // custom border colours: later
    si.unnormalizedCoordinates = VK_FALSE;
    (void)info;
    uint64_t key = Hash(&si, sizeof(si));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSampler smp = VK_NULL_HANDLE;
    VKCHECK(vkCreateSampler(dev_, &si, nullptr, &smp));
    samplers_[key] = smp;
    return smp;
}

VkPipeline VulkanBackend::EnsurePipeline(const PipelineKey& k, const std::vector<uint32_t>& vs,
                                         const std::vector<uint32_t>& ps) {
    uint64_t key = Hash(&k, sizeof(k));
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    auto module = [&](const std::vector<uint32_t>& code) {
        uint64_t h = Hash(code.data(), code.size() * 4);
        auto mit = modules_.find(h);
        if (mit != modules_.end()) return mit->second;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        VKCHECK(vkCreateShaderModule(dev_, &ci, nullptr, &m));
        modules_[h] = m;
        return m;
    };
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = module(vs);
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = module(ps);
    st[1].pName = "main";

    // vertex input: location i is the i-th present register (the generator declares them in order)
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nva = 0;
    for (uint32_t b = 0; b < k.buffers; b++) vb[b] = {b, k.strides[b], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!(k.regs >> r & 1)) continue;
        uint32_t e = k.elements[r];
        va[nva] = {nva, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
        nva++;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topo == uint32_t(Topology::Points) ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : k.topo == uint32_t(Topology::Lines)
                      ? (k.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                      : (k.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == 0x1B00 ? VK_POLYGON_MODE_POINT : k.fill == 0x1B01 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // the viewport is flipped (negative height), which keeps D3D's winding: front is clockwise unless culling CW
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkDepthBiasRepresentationInfoEXT bias_rep{VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
    if (k.zbias != 0 || k.zslope != 0) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasSlopeFactor = k.zslope;
        if (bias_float_) {
            bias_rep.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
            bias_rep.depthBiasExact = VK_TRUE;
            rs.pNext = &bias_rep;
            rs.depthBiasConstantFactor = k.zbias;
        } else {
            rs.depthBiasConstantFactor = k.zbias * 16777216.0f;  // exact for depths in [0.5, 1)
        }
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zen && k.zwrite;
    ds.depthCompareOp = k.zen ? Cmp(k.zfunc) : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = k.blend;
    cba.srcColorBlendFactor = Blend(k.src, false);
    cba.dstColorBlendFactor = Blend(k.dst, false);
    cba.colorBlendOp = BlendOp(k.op);
    cba.srcAlphaBlendFactor = Blend(k.src, true);
    cba.dstAlphaBlendFactor = Blend(k.dst, true);
    cba.alphaBlendOp = BlendOp(k.op);
    cba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkFormat cf = kColorFormat;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = k.has_rt ? 1 : 0;
    pr.pColorAttachmentFormats = &cf;
    pr.depthAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &pr;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) {
        LOGE("vulkan: pipeline creation failed");
        p = VK_NULL_HANDLE;
    }
    pipelines_[key] = p;
    return p;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Span span{record_ns_};
    // CSX_VK_STEPSHOTS=NAME:N: in the frame after shot NAME, the target saved before every Nth draw (finding the draw
    // that paints a region)
    static const char* step_env = std::getenv("CSX_VK_STEPSHOTS");
    static const std::string step_shot = step_env && std::strchr(step_env, ':')
                                             ? std::string(step_env, size_t(std::strchr(step_env, ':') - step_env)) : "";
    static const long step = step_shot.empty() ? 0 : std::atol(std::strchr(step_env, ':') + 1);
    if (step && step_name_ == step_shot && frame_draws_ % step == 0 && s.rt.header) {
        if (Target* t = EnsureTarget(s.rt, false)) {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "_step%04llu", frame_draws_);
            SaveShot(*t, step_name_ + suffix);
        }
    }
    frame_draws_++;
    Assembled a;
    draws_++;
    if (!Assemble(s, prim, start, count, indices, a)) {
        dropped_[0]++;
        return;
    }
    if (a.topology == Topology::Points) {
        LOG_FIRST(2, "vulkan: point sprites are not drawn yet");
        dropped_[1]++;
        return;
    }
    Target* rt = s.rt.header ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = s.zs.header ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;

    // shaders, by the keys the HLSL is a function of (generating and hashing the source per draw cost most of the
    // recording time)
    ShaderChoice sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    const Shader& vs = EnsureShader(VertexKey(s, a, sh), vk_shader::Stage::Vertex, [&] {
        std::string src = QuantiseDepth(VertexHlsl(s, a, sh));
        return sh.quad ? QuadHlsl(std::move(src)) : src;
    });
    // textures before the rendering scope: uploads are transfers
    TexInfo tex[4];
    Image* stage_tex[4] = {};
    uint32_t tex_w[4] = {}, tex_h[4] = {};
    for (int i = 0; i < 4; i++) stage_tex[i] = EnsureTexture(s, i, rt, tex[i], tex_w[i], tex_h[i]);
    PixelKey pk = MakePixelKey(s, tex);
    pk.quad = sh.quad;
    // PixelHlsl applies QuadHlsl for pk.quad
    const Shader& ps = EnsureShader(Hash(&pk, sizeof(pk)) ^ kPixelKeySalt, vk_shader::Stage::Pixel, [&] { return PixelHlsl(pk); });
    // CSX_VK_LOGFRAME=N: every draw of the Nth presented frame in the log (and of the CSX_VK_STEPSHOTS frame)
    static const long log_frame = std::getenv("CSX_VK_LOGFRAME") ? std::atol(std::getenv("CSX_VK_LOGFRAME")) : -1;
    if (long(presents_) == log_frame || (step && step_name_ == step_shot))
        LOGI("vulkan: draw %llu prim %u n %u regs %X pre %d prog %d quad %d idx %u verts %u rt %08X ds %08X z %u zf %X "
             "zw %u cull %X blend %u vp %u %u %u %u tex0 %d", frame_draws_ - 1, prim, count, a.regs,
             int(a.pretransformed), int(sh.prog), int(sh.quad), a.index_count, a.vertex_count, rt ? rt->data : 0,
             ds ? ds->data : 0, s.rs[kRsZEnable], SimpleState(s, kMZFunc), SimpleState(s, kMZWriteEnable),
             s.rs[kRsCullMode], SimpleState(s, kMBlendEnable), s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h,
             int(stage_tex[0] != nullptr));
    if (!vs.code || vs.code->empty() || !ps.code || ps.code->empty()) {
        dropped_[2]++;
        return;
    }

    // pipeline
    PipelineKey k{};
    k.vs = vs.hash;
    k.ps = ps.hash;
    k.regs = a.regs;
    k.buffers = a.buffer_count;
    for (uint32_t r = 0; r < 16; r++)
        if (a.regs >> r & 1)
            k.elements[r] = a.elements[r].format | uint32_t(a.elements[r].buffer) << 8 | uint32_t(a.elements[r].offset) << 16;
    for (uint32_t b = 0; b < a.buffer_count; b++) k.strides[b] = a.buffers[b].stride;
    k.topo = uint32_t(a.topology);
    k.strip = a.strip;
    k.has_rt = rt != nullptr;
    k.has_ds = ds != nullptr;
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    if (ds) {
        k.zen = s.rs[kRsZEnable] != 0;
        if (k.zen) {
            k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
            k.zfunc = SimpleState(s, kMZFunc);
        }
        k.sten = s.rs[kRsStencilEnable] != 0;
        if (k.sten) {
            k.sfunc = SimpleState(s, kMStencilFunc);
            k.smask = SimpleState(s, kMStencilMask) & 0xFF;
            k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
            k.sfail = s.rs[kRsStencilFail];
            k.szfail = SimpleState(s, kMStencilZFail);
            k.spass = SimpleState(s, kMStencilPass);
        }
    }
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == 0x900;  // D3DCULL_CW (GL_CW): culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
    // polygon offset of filled triangles (NV097_SET_POLY_OFFSET_FILL_ENABLE, its factor and its bias in depth units)
    if (ds && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = Bits(SimpleState(s, kMDepthOffset)) / s.zscale;
        k.zslope = Bits(SimpleState(s, kMSlopeScale));
    }
    VkPipeline pipe = EnsurePipeline(k, *vs.code, *ps.code);
    if (!pipe) {
        dropped_[3]++;
        return;
    }

    // constants, vertices and indices into the upload ring
    GuestExtent size;
    SurfaceSize(rt ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    for (int i = 0; i < 4; i++) {
        if (stage_tex[i]) {
            c.texsize[i][0] = float(tex_w[i]);
            c.texsize[i][1] = float(tex_h[i]);
        }
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(c), 256);
    std::memcpy(dc.cpu, &c, sizeof(c));
    VkDeviceSize vbo[16];
    for (uint32_t b = 0; b < a.buffer_count; b++) {
        Alloc v = Upload(a.buffers[b].bytes, 16);
        std::memcpy(v.cpu, a.buffers[b].src, a.buffers[b].bytes);
        vbo[b] = v.offset;
    }
    VkDeviceSize ibo = 0;
    if (a.index_count) {
        uint32_t bytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(bytes, 4);
        std::memcpy(ib.cpu, a.index_src, bytes);
        ibo = ib.offset;
    }

    BeginRendering(rt, ds);
    VkCommandBuffer cb = Cmd();
    Frame& f = frames_[frame_ % kFrames];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorBufferInfo bi[2] = {{f.upload, vc.offset, sizeof(s.vsc)}, {f.upload, dc.offset, sizeof(c)}};
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[10];
    for (uint32_t i = 0; i < 10; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (int i = 0; i < 2; i++) {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    for (int i = 0; i < 4; i++) {
        ii[i] = {VK_NULL_HANDLE, stage_tex[i] ? stage_tex[i]->view : dummy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        si[i] = {stage_tex[i] ? EnsureSampler(s, i, tex[i]) : dummy_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[2 + i].pImageInfo = &ii[i];
        w[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[6 + i].pImageInfo = &si[i];
    }
    push_descriptors_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 10, w);

    // the whole target as viewport (the guest viewport is in the shaders), flipped to keep D3D's y up
    Image* any = rt ? static_cast<Image*>(rt) : ds;
    VkViewport v{0, float(any->h), float(any->w), -float(any->h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &v);
    int x0 = int(s.viewport.x), y0 = int(s.viewport.y), x1 = int(s.viewport.x + s.viewport.w),
        y1 = int(s.viewport.y + s.viewport.h);
    if (s.scissor_count) {
        x0 = std::max(x0, int(s.scissors[0]));
        y0 = std::max(y0, int(s.scissors[1]));
        x1 = std::min(x1, int(s.scissors[2]));
        y1 = std::min(y1, int(s.scissors[3]));
    }
    x0 = std::clamp(x0, 0, int(any->w));
    x1 = std::clamp(x1, 0, int(any->w));
    y0 = std::clamp(y0, 0, int(any->h));
    y1 = std::clamp(y1, 0, int(any->h));
    VkRect2D sc{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(cb, bf);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
    VkBuffer bufs[16];
    for (uint32_t b = 0; b < a.buffer_count; b++) bufs[b] = f.upload;
    if (a.buffer_count) vkCmdBindVertexBuffers(cb, 0, a.buffer_count, bufs, vbo);
    if (a.index_count) {
        vkCmdBindIndexBuffer(cb, f.upload, ibo, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cb, a.draw_count, 1, 0, 0);
    }
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t, uint32_t, uint32_t) {
    (void)src;
    (void)dst;
    LOG_FIRST(4, "vulkan: CopyRects is not done yet");
}

void VulkanBackend::Sync(uint32_t va, uint32_t size) {
    Target* t = nullptr;
    for (auto& [key, other] : targets_)
        if (!(key >> 63) && other.data == mem::PhysicalFromVirtual(va)) t = &other;
    if (!t) return;
    if (!t->linear32) {
        LOG_FIRST(4, "vulkan: a guest read of a swizzled or non 32 bit target is not written back");
        return;
    }
    EndRendering();
    VkDeviceSize bytes = VkDeviceSize(t->w) * t->h * 4;
    Alloc a = Upload(bytes, 16);
    Barrier(*t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t->w, t->h, 1};
    vkCmdCopyImageToBuffer(Cmd(), t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frames_[frame_ % kFrames].upload, 1, &c);
    Submit(true);
    uint32_t rows = std::min<uint32_t>(t->h, t->pitch ? size / t->pitch : 0);
    for (uint32_t y = 0; y < rows; y++)
        std::memcpy(kernel::HostPtr(va + y * t->pitch), static_cast<const uint8_t*>(a.cpu) + size_t(y) * t->w * 4,
                    t->w * 4);
    LOG_FIRST(4, "vulkan: target %08X (%ux%u) written back for a guest read", t->data, t->w, t->h);
}

void VulkanBackend::SaveShot(Image& t, const std::string& name) {
    // the target to a host buffer, after this frame's work
    EndRendering();
    VkDeviceSize bytes = VkDeviceSize(t.w) * t.h * 4;
    Alloc a = Upload(bytes, 16);
    Barrier(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.w, t.h, 1};
    vkCmdCopyImageToBuffer(Cmd(), t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frames_[frame_ % kFrames].upload, 1, &c);
    Submit(true);
    if (SDL_Surface* s = SDL_CreateSurfaceFrom(int(t.w), int(t.h), SDL_PIXELFORMAT_ARGB8888, a.cpu, int(t.w * 4))) {
        std::filesystem::create_directories(app::ExeDir() / "shots");
        std::string path = (app::ExeDir() / "shots" / (name + ".bmp")).string();
        SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
        LOGI("window: shot %s", path.c_str());
    }
}

// the presented target with the video overlay over it, in an image of its own (the title's buffer stays as drawn): the
// overlay's pixels uploaded and blitted, filtered, into its destination rectangle scaled from the video mode to the
// target; the target itself when no overlay shows
Image* VulkanBackend::ComposeOverlay(Image& t) {
    const OverlayImage& o = overlay_;
    if (!o.enabled || !o.pixels || !o.width || !o.height || !o.display_w || !o.display_h) return &t;
    if (o.use_color_key) LOG_FIRST(1, "vulkan: the overlay's colour key is not applied");
    auto ensure = [&](Image& im, uint32_t w, uint32_t h) {
        if (im.image && im.w == w && im.h == h) return true;
        if (im.image) FreeImage(im);
        return MakeImage(im, w, h, kColorFormat, 1,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false);
    };
    if (!ensure(overlay_target_, t.w, t.h) || !ensure(overlay_img_, o.width, o.height)) return &t;
    EndRendering();
    VkDeviceSize bytes = VkDeviceSize(o.width) * o.height * 4;
    Alloc a = Upload(bytes, 16);
    std::memcpy(a.cpu, o.pixels, bytes);
    VkCommandBuffer cb = Cmd();
    Barrier(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(overlay_target_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, overlay_target_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(overlay_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy up{};
    up.bufferOffset = a.offset;
    up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    up.imageExtent = {o.width, o.height, 1};
    vkCmdCopyBufferToImage(cb, frames_[frame_ % kFrames].upload, overlay_img_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &up);
    Barrier(overlay_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    float sx = float(t.w) / o.display_w, sy = float(t.h) / o.display_h;
    int x0 = std::max(0, int(o.dst[0] * sx)), y0 = std::max(0, int(o.dst[1] * sy));
    int x1 = std::min(int(t.w), int(o.dst[2] * sx)), y1 = std::min(int(t.h), int(o.dst[3] * sy));
    if (x1 > x0 && y1 > y0) {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int(o.width), int(o.height), 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cb, overlay_img_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, overlay_target_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    return &overlay_target_;
}

// the presented image through the title's gamma ramp (a full screen pass reading the image and a 256 x 1 lookup of
// the three ramps), into an image of its own; the image itself while the ramp is the identity
Image* VulkanBackend::ApplyGamma(Image& src) {
    if (gamma_identity_) return &src;
    if (!gamma_lut_.image && !MakeImage(gamma_lut_, 256, 1, kColorFormat, 1,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false))
        return &src;
    if (gamma_target_.image && (gamma_target_.w != src.w || gamma_target_.h != src.h)) FreeImage(gamma_target_);
    if (!gamma_target_.image &&
        !MakeImage(gamma_target_, src.w, src.h, kColorFormat, 1,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   false))
        return &src;
    static const char* kVs = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
})";
    static const char* kPs = R"(
Texture2D<float4> image : register(t0);
Texture2D<float4> ramp : register(t1);
float4 main(float4 pos : SV_Position) : SV_Target {
    float3 c = image.Load(int3(pos.xy, 0)).rgb;
    int3 i = int3(c * 255 + 0.5);
    return float4(ramp.Load(int3(i.r, 0, 0)).r, ramp.Load(int3(i.g, 0, 0)).g, ramp.Load(int3(i.b, 0, 0)).b, 1);
})";
    const auto& vs = vk_shader::Compile(kVs, vk_shader::Stage::Vertex);
    const auto& ps = vk_shader::Compile(kPs, vk_shader::Stage::Pixel);
    if (vs.empty() || ps.empty()) return &src;
    PipelineKey k{};
    k.vs = Hash(vs.data(), vs.size() * 4);
    k.ps = Hash(ps.data(), ps.size() * 4);
    k.topo = uint32_t(Topology::Triangles);
    k.has_rt = 1;
    k.wmask = 0xF;
    k.fill = 0x1B02;
    VkPipeline pipe = EnsurePipeline(k, vs, ps);
    if (!pipe) return &src;
    EndRendering();
    VkCommandBuffer cb = Cmd();
    if (gamma_dirty_) {
        Alloc a = Upload(256 * 4, 16);
        uint8_t* p = static_cast<uint8_t*>(a.cpu);
        for (int i = 0; i < 256; i++) p[i * 4] = gamma_[512 + i], p[i * 4 + 1] = gamma_[256 + i], p[i * 4 + 2] = gamma_[i], p[i * 4 + 3] = 255;
        Barrier(gamma_lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy up{};
        up.bufferOffset = a.offset;
        up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        up.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(cb, frames_[frame_ % kFrames].upload, gamma_lut_.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        gamma_dirty_ = false;
    }
    Barrier(gamma_lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginRendering(&gamma_target_, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {VK_NULL_HANDLE, gamma_lut_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = 2 + i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    push_descriptors_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, w);
    VkViewport v{0, 0, float(src.w), float(src.h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &v);
    VkRect2D sc{{0, 0}, {src.w, src.h}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    EndRendering();
    return &gamma_target_;
}

void VulkanBackend::Present(const Surface& front) {
    Span span{present_ns_};
    Target* t = EnsureTarget(front, false);
    if (!t) return;
    // a shot request (the window's "shot" file): the presented target, as the reference renderer's captures are
    std::string shot = window::TakeShotRequest();
    step_name_.clear();
    frame_draws_ = 0;
    // the frame buffer through the gamma ramp, then the overlay (the console mixes its video plane after the ramp)
    Image* shown = ComposeOverlay(*ApplyGamma(*t));
    if (!shot.empty()) {
        SaveShot(*shown, shot);
        step_name_ = shot;
        // CSX_VK_SHOT_TARGETS=1: every colour target too, for comparing intermediate passes (glow chain) with the
        // reference renderer's guest memory
        static const bool all = std::getenv("CSX_VK_SHOT_TARGETS") != nullptr;
        if (all) {
            for (auto& [key, other] : targets_) {
                if (key >> 63 || &other == t) continue;
                char name[64];
                std::snprintf(name, sizeof name, "_%08X_%ux%u", other.data, other.w, other.h);
                SaveShot(other, shot + name);
            }
        }
    }
    EndRendering();
    VkCommandBuffer cb = Cmd();
    Frame& f = frames_[frame_ % kFrames];
    uint32_t index = 0;
    VkResult ar = swapchain_ ? vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index)
                             : VK_ERROR_OUT_OF_DATE_KHR;
    bool have_image = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
    if (have_image) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = swap_images_[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &d);
        VkClearColorValue black{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, swap_images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        // the target at the display's 4:3, letterboxed
        float W = float(swap_extent_.width), H = float(swap_extent_.height), tw = W, th = W * 3 / 4;
        if (th > H) {
            th = H;
            tw = H * 4 / 3;
        }
        int dx = int((W - tw) / 2), dy = int((H - th) / 2);
        Barrier(*shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int(shown->w), int(shown->h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {dx, dy, 0};
        blit.dstOffsets[1] = {dx + int(tw), dy + int(th), 1};
        vkCmdBlitImage(cb, shown->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_images_[index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(cb, &d);
    }
    if (timestamps_) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, uint32_t(frame_ % kFrames) * 2 + 1);
        f.timed = true;
    }
    frame_open_ = false;
    // CSX_VK_MAGENTA=1: the presented target cleared to magenta afterwards, so what the next frame on it leaves
    // undrawn shows (the title clears colour rarely: CS clears depth and stencil only in game)
    static const bool magenta = std::getenv("CSX_VK_MAGENTA") != nullptr;
    if (magenta) {
        Barrier(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue m{{1, 0, 1, 1}};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &m, 1, &r);
    }
    vkEndCommandBuffer(cb);
    vkResetFences(dev_, 1, &f.fence);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &swap_ready_[index];
    }
    VKCHECK(vkQueueSubmit(queue_, 1, &si, f.fence));
    recording_ = false;
    bool remake = !have_image;
    if (have_image) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swap_ready_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &index;
        VkResult pr = vkQueuePresentKHR(queue_, &pi);
        remake = pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || ar == VK_SUBOPTIMAL_KHR;
    }
    if (!remake) {
        // a Wayland surface takes the swapchain's size and reports neither out of date nor suboptimal when the window
        // changes size (fullscreen, a resize): the window's pixel size is compared instead
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window::SdlWindow()), &w, &h);
        remake = w > 0 && h > 0 && (uint32_t(w) != swap_extent_.width || uint32_t(h) != swap_extent_.height);
    }
    if (remake) CreateSwapchain();
    frame_++;
    presents_++;
    harness::g_frames++;  // frames shown, as the software window counts them (the window's relaunch handover)
    if (presents_ % 30 == 0) {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - fps_t_).count();
        LOGI("renderer: frame %llu, %.1f frames/s, %llu draws, dropped: assembly %llu, points %llu, shaders %llu, "
             "pipeline %llu, upload wraps %llu, peak %llu KB", (unsigned long long)presents_, secs > 0 ? 30.0 / secs : 0.0,
             draws_, dropped_[0], dropped_[1], dropped_[2], dropped_[3], wraps_, peak_ >> 10);
        LOGI("renderer: per frame: GPU %.3f ms, CPU recording %.3f ms, present %.3f ms",
             gpu_frames_ ? gpu_ns_ / gpu_frames_ / 1e6 : 0.0, record_ns_ / 30 / 1e6, present_ns_ / 30 / 1e6);
        peak_ = 0;
        gpu_ns_ = record_ns_ = present_ns_ = 0;
        gpu_frames_ = 0;
        fps_t_ = now;
    }
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
