// The launch data a quick reboot left (hdd/launch.bin, which kernel::Boot reads and deletes) copied to launch_seen.bin
// in the run folder before boot consumes it: the front end's relaunch into a match, kept as a test fixture lets
// scenarios start in the match directly (tools/drive.py --launch).
#include <filesystem>
#include <system_error>

#include "app/app.h"
#include "common/csx.h"
#include "core/log.h"

namespace csx {

void KeepLaunchData() {
    std::error_code ec;
    auto from = app::ExeDir() / "hdd" / "launch.bin";
    if (!std::filesystem::exists(from, ec)) return;
    std::filesystem::copy_file(from, app::ExeDir() / "launch_seen.bin", std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) LOGI("launch data kept as launch_seen.bin");
}

}  // namespace csx
