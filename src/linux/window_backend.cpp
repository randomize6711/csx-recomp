// gpu::Backend for the Linux window: every frame drawn by the CPU reference renderer into guest memory, the presented
// surface decoded to BGRA for the window thread. Slow (the reference renderer is single threaded) but exact to what the
// headless captures show.
#include <chrono>
#include <cstring>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "linux/window.h"
#include "reference_renderer.h"

namespace window {
Frame& Latest() {
    static Frame f;
    return f;
}
}  // namespace window

namespace gpu {

namespace {

class WindowBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        LOGI("renderer: window, reference renderer, %ux%u", w, h);
        reference::BeginFrame();
        return true;
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        reference::Draw(s, prim, start, count, indices);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        reference::Copy(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        TextureDesc d;
        if (DescribeSurface(front, d) && Format(d.format).host == Host::BGRA8) {
            std::vector<uint8_t> px;
            uint32_t pitch = 0;
            DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
            auto& f = window::Latest();
            std::lock_guard<std::mutex> lock(f.lock);
            f.pixels.swap(px);
            f.width = d.width;
            f.height = d.height;
            f.serial++;
        } else {
            LOG_FIRST(1, "renderer: front surface format %X is not shown", d.format);
        }
        reference::BeginFrame();
        uint64_t n = ++harness::g_frames;
        if (n % 30 == 0) {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - last_).count();
            LOGI("renderer: frame %llu, %.1f frames/s", (unsigned long long)n, secs > 0 ? 30.0 / secs : 0.0);
            last_ = now;
        }
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    void SetGamma(const uint8_t*) override { LOG_FIRST(1, "renderer: gamma ramps are not applied in the window"); }
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<WindowBackend>(); }

}  // namespace gpu
