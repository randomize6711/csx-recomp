// Condition Zero's extra radio lines (N5C): the game's sound bank (hdcounterstrike.xsb) holds lines Condition Zero added
// (radio\circleback, flankthem, meetme, matedown, ct_imhit, fireassis, hitassist, bombpl, bombdef, hosdown) that no
// command of the game plays. "csx_radio NAME" plays one as the game's own radio commands play theirs, on our servers
// only:
//   - the server (this game's code, csx-server or a listen server): the client's string command, seen where the
//     engine parses it (net_security.cpp ClientCommandAllowed, so its sender is host_client: the authenticated
//     connection's player, never a name it gives), is checked as the game checks its radio commands (0x13E600: the
//     player's next radio time, player +0x314, not ahead of the game time; radio messages left this round, +0x318,
//     above 0; both updated the same way) and played through the game's own radio function (0x117250: the player
//     alive, the line to its teammates, the sound and the text), the sound by its file name;
//   - a server says it serves them at signon ("pc_server_features radio2", net_security.cpp SignonPlayerCollision);
//     a client sends csx_radio only to a server that said so, or to the server in its own process; elsewhere it says
//     so on the console and sends nothing (a server without them never sees the command).
// The lines' texts are written from their sounds' names (the original's #-strings for them are not in this game).
#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;

struct Line {
    const char* name;   // csx_radio's argument
    const char* sound;  // the radio function's sound: the file name (the bank's cue radio\<name>)
    const char* text;   // shown to the teammates
};
constexpr Line kLines[] = {
    {"circleback", "radio/circleback.wav", "Circle around back!"},
    {"flankthem", "radio/flankthem.wav", "Flank them!"},
    {"meetme", "radio/meetme.wav", "Meet at my location!"},
    {"matedown", "radio/matedown.wav", "Teammate down!"},
    {"imhit", "radio/ct_imhit.wav", "I'm hit!"},
    {"fireassis", "radio/fireassis.wav", "Need fire assistance!"},
    {"hitassist", "radio/hitassist.wav", "I'm hit, need assistance!"},
    {"bombpl", "radio/bombpl.wav", "The bomb is planted!"},
    {"bombdef", "radio/bombdef.wav", "The bomb is defused!"},
    {"hosdown", "radio/hosdown.wav", "Hostage down!"},
};

constexpr uint32_t kRadio = 0x117250;        // CBasePlayer::Radio(id, text, pitch): the player in ebx, callee pops
constexpr uint32_t kGlobals = 0x527CE8;      // gpGlobals (time first)
constexpr uint32_t kRadioDelay = 0x365F28;   // the double the game adds to the time for the next radio message
constexpr uint32_t kMalloc = 0x22D86A;       // the CRT's malloc
constexpr uint32_t kChannelType = 0x7812E8 + 4;  // cls.netchan.remote_address.type (3: a network server)

bool g_server_serves = false;  // the client's server said it serves the lines

const Line* Find(const std::string& name) {
    for (const Line& l : kLines)
        if (name == l.name) return &l;
    return nullptr;
}

// a guest copy of a constant string, made once
uint32_t GuestText(X86Context& ctx, const char* s) {
    static std::string made[2 * sizeof(kLines) / sizeof(kLines[0])];
    static uint32_t at[2 * sizeof(kLines) / sizeof(kLines[0])] = {};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        if (at[i] && made[i] == s) return at[i];
        if (!at[i]) {
            uint32_t n = uint32_t(std::strlen(s)) + 1;
            at[i] = kernel::CallGuest(ctx, kMalloc, {n});
            if (!at[i]) return 0;
            std::memcpy(kernel::HostPtr(at[i]), s, n);
            made[i] = s;
            return at[i];
        }
    }
    return 0;
}

std::string Arg(const std::string& text) {
    size_t a = text.find_first_of(" \t");
    if (a == std::string::npos) return {};
    a = text.find_first_not_of(" \t\"", a);
    size_t b = a == std::string::npos ? a : text.find_first_of(" \t\"\r\n", a);
    return a == std::string::npos ? std::string() : text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
}  // namespace

namespace csx {

// the server: a client's "csx_radio NAME" (net_security.cpp, client its sender); true when it was one
bool RadioCommand(X86Context& ctx, uint32_t client, const std::string& text) {
    const Line* l = Find(Arg(text));
    uint32_t edict = client ? R32(client + 0x4B9C) : 0;
    uint32_t player = edict ? R32(edict + 0x7C) : 0;
    std::string who = client ? kernel::ReadCString(client + 0x4D10, 32) : std::string("?");
    if (!l || !player) {
        LOG_FIRST(32, "radio: %s's \"%s\" refused (no such line, or no player)", who.c_str(), text.c_str());
        return true;
    }
    float now = kernel::G<float>(R32(kGlobals)), next = kernel::G<float>(player + 0x314);
    int32_t left = int32_t(R32(player + 0x318));
    if (next > now || left <= 0) return true;  // too soon, or none left this round: as the game's own radio commands
    double delay;
    std::memcpy(&delay, kernel::HostPtr(kRadioDelay), 8);
    uint32_t id = GuestText(ctx, l->sound), says = GuestText(ctx, l->text);
    if (!id || !says) return true;
    kernel::W32(player + 0x318, uint32_t(left - 1));
    kernel::G<float>(player + 0x314) = float(now + delay);
    uint32_t esp = ctx.esp, ebx = ctx.ebx;
    ctx.esp -= 0x40;  // below what the parser keeps under its stack pointer
    ctx.ebx = player;
    kernel::CallGuest(ctx, kRadio, {id, says, 1});
    ctx.esp = esp;
    ctx.ebx = ebx;
    LOGI("radio: %s: %s (%s)", who.c_str(), l->name, l->sound);
    return true;
}

}  // namespace csx

// the client: "pc_server_features ..." (the server's signon text)
void PcServerFeatures(const std::string& args) {
    g_server_serves = (" " + args + " ").find(" radio2 ") != std::string::npos;
    LOGI("radio: the server %s Condition Zero's extra radio lines", g_server_serves ? "serves" : "does not serve");
}

// the client: "csx_radio NAME" typed or bound; true when it stays here (not sent to the server)
bool PcRadioClientCommand(const std::string& name) {
    bool network = R32(kChannelType) == 3;
    if (!Find(name)) {
        LOGI("radio: no extra radio line \"%s\"", name.c_str());
        return true;
    }
    if (network && !g_server_serves) {
        LOG_FIRST(8, "radio: this server does not serve Condition Zero's extra radio lines: \"%s\" not sent", name.c_str());
        return true;
    }
    return false;
}
