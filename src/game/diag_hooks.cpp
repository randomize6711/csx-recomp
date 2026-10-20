// Diagnostic instruction hooks for bring-up, named by [[hook]] entries in config/game.toml.
#include <cstdlib>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

// 0x108470: the title's fatal error screen (Reset, clear, one quad, Swap, then Sleep(1000) forever)
void LogFatalScreen(X86Context& ctx, uint8_t*) {
    // entered by call and by tail jumps, so the stack shows arguments before the return address
    uint32_t sp = ctx.esp;
    LOGE("fatal error screen: stack %08X %08X %08X %08X %08X %08X %08X %08X, eax %08X ecx %08X edx %08X esi %08X",
         kernel::R32(sp), kernel::R32(sp + 4), kernel::R32(sp + 8), kernel::R32(sp + 12), kernel::R32(sp + 16),
         kernel::R32(sp + 20), kernel::R32(sp + 24), kernel::R32(sp + 28), ctx.eax, ctx.ecx, ctx.edx, ctx.esi);
    logx::Flush();
}

// every site that calls or jumps to the fatal error screen, so the log names the one taken
#define CSX_FATAL_SITES(X) X(001058FC) X(001059EB) X(00105BAA) X(00105C42) X(00105FD4) X(00106062) X(00106275) X(001062FA) X(001065FA) X(00106675) X(00106797) X(001067B4) X(001067F5) X(00106827) X(00106E63) X(00106EB1) X(00106EE0) X(00106EFF) X(000AD88A) X(000ADC0E) X(000ADE32) X(000BF330) X(000C30E4) X(000C6FA4) X(000C6FBA) X(000C6FEA) X(000C6FFF) X(000C7358) X(000F33FC)
#define CSX_FATAL_SITE_HOOK(a) \
    void LogFatalSite_##a(X86Context& ctx, uint8_t*) { LOGE("fatal error screen from site %s, eax %08X", #a, ctx.eax); logx::Flush(); }
CSX_FATAL_SITES(CSX_FATAL_SITE_HOOK)


// 0xC5A00: the menus' sound player's entry (cdecl: the sound's name, 0), logged with CSX_LOG_MENU_SOUNDS=1, so a test
// finds each menu sound the game plays (its own d-pad's HighlightSound as well as the mouse's) in an audio capture
void LogMenuSound(X86Context& ctx, uint8_t*) {
    static const bool on = std::getenv("CSX_LOG_MENU_SOUNDS") && *std::getenv("CSX_LOG_MENU_SOUNDS") == '1';
    uint32_t name = kernel::R32(ctx.esp + 4);
    if (on && name) LOGI("menu sound %s", static_cast<const char*>(kernel::HostPtr(name)));
}
