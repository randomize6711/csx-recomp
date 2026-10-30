// Mods' files in place of the archive's (N8, common/mods.h, docs/n8_mods.md). The game reads its files from the
// current SXWad archive (*0x552744: its name -> entry map; 0x1FC4F0 "is it here", 0x1FC6B0 "open it") and, where the
// archive has no such entry, from the disk (the game folder: CreateFile on the same name, e.g. COM_LoadFile's 0x13F34).
// For a name an active mod replaces, both answer "not here", so the game reads the mod's file, linked into the overlay
// folder (mods.cpp Apply) at the name's path.
#include <cstdint>
#include <string>

#include "common/mods.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
thread_local bool g_exists_override = false, g_open_override = false;

bool Check(X86Context& ctx, const char* what) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        for (const std::string& r : csx::mods::TakeReport()) LOGI("%s", r.c_str());
    }
    uint32_t name = kernel::R32(ctx.esp + 4);
    if (!name) return false;
    std::string n = kernel::ReadCString(name, 260);
    LOG_FIRST(6, "mods: the archive asked for \"%s\" (%s)", n.c_str(), what);
    if (!csx::mods::Overridden(n)) return false;
    LOG_FIRST(64, "mods: %s from a mod (%s)", n.c_str(), what);
    return true;
}
}  // namespace

// [[hook]] at 0x1FC4F0 (config/game.toml): the archive's "is NAME here" (NAME at [esp+4])
void PcModsExistsEntry(X86Context& ctx, uint8_t*) { g_exists_override = Check(ctx, "exists"); }

// [[hook]] at 0x1FC5A8: its answer in al
void PcModsExistsResult(X86Context& ctx, uint8_t*) {
    if (!g_exists_override) return;
    g_exists_override = false;
    ctx.eax &= ~0xFFu;
}

// [[hook]] at 0x1FC6B0: the archive's "open NAME" (NAME at [esp+4])
void PcModsOpenEntry(X86Context& ctx, uint8_t*) { g_open_override = Check(ctx, "open"); }

// [[hook]] at 0x1FC79F, the name looked up: true goes to its "not here" (0x1FC7B0, al 0)
bool PcModsOpenFound(X86Context&, uint8_t*) {
    bool o = g_open_override;
    g_open_override = false;
    return o;
}
