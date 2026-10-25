// The dedicated server's host stand ins (docs/pc_multiplayer.md "N2"): no graphics, no pads. The engine runs with no
// client (ca_dedicated, src/game/dedicated.cpp), so nothing is drawn; what the title's own boot still hands to the
// D3D layer goes nowhere: the backend keeps no images, draws nothing and presents nothing.
#include "core/input.h"
#include "gpu/backend.h"

namespace gpu {
namespace {
class ServerBackend final : public Backend {
public:
    bool Init(uint32_t, uint32_t) override { return true; }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const Surface&) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};
}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<ServerBackend>(); }

}  // namespace gpu

namespace input {

// no controller, keyboard or mouse: every port reads as unplugged
Pad Poll(int) {
    return Pad{};
}
void Rumble(int, uint16_t, uint16_t) {}

}  // namespace input
