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
// The radio menus (radio1, radio2, radio3; the bug-fix pass's issue 11): the server's text menu (ShowMenu, CS 1.6's
// "\w"-coloured lines) is drawn by this game's client as overlapping copies of its lines, so the menus are the pause
// menu's kind of screen instead (InGame_PCRadio1..3, PcRadioScreens): the game's commands and Condition Zero's lines
// in rows, picked with the mouse, the keys or a controller; a row sends its command and closes the menu ("!CMD;xbui
// exit", as the title's own in-game menus write it).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/csx.h"
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

namespace {
struct MenuRow {
    const char* command;  // the client's command (the server's radio commands; csx_radio for Condition Zero's)
    const char* label;
};
struct Menu {
    const char* screen;
    const char* title;
    std::vector<MenuRow> rows;
};
const std::vector<Menu>& Menus() {
    static const std::vector<Menu> m = {
        {"InGame_PCRadio1", "Radio Commands",
         {{"coverme", "Cover Me"},
          {"takepoint", "You Take the Point"},
          {"holdpos", "Hold This Position"},
          {"regroup", "Regroup Team"},
          {"followme", "Follow Me"},
          {"takingfire", "Taking Fire, Need Assistance"},
          {"csx_radio circleback", "CZ: Circle Around Back"},
          {"csx_radio flankthem", "CZ: Flank Them"},
          {"csx_radio meetme", "CZ: Meet at My Location"}}},
        {"InGame_PCRadio2", "Group Radio Commands",
         {{"go", "Go Go Go!"},
          {"fallback", "Team, Fall Back!"},
          {"sticktog", "Stick Together, Team"},
          {"getinpos", "Get in Position and Wait"},
          {"stormfront", "Storm the Front!"},
          {"report", "Report In, Team"},
          {"csx_radio matedown", "CZ: Teammate Down"},
          {"csx_radio fireassis", "CZ: Need Fire Assistance"}}},
        {"InGame_PCRadio3", "Radio Responses",
         {{"roger", "Affirmative / Roger That"},
          {"enemyspot", "Enemy Spotted"},
          {"needhelp", "Need Backup"},
          {"sectorclear", "Sector Clear"},
          {"inposition", "I'm in Position"},
          {"reportingin", "Reporting In"},
          {"getout", "Get Out of There, It's Gonna Blow!"},
          {"negative", "Negative"},
          {"enemydown", "Enemy Down"},
          {"csx_radio imhit", "CZ: I'm Hit"},
          {"csx_radio hitassist", "CZ: Hit, Need Assistance"},
          {"csx_radio bombpl", "CZ: Bomb Planted"},
          {"csx_radio bombdef", "CZ: Bomb Defused"},
          {"csx_radio hosdown", "CZ: Hostage Down"}}},
    };
    return m;
}
}  // namespace

// the radio menus' screens, for the in-game menus' text (pc_menu.cpp ExtendInGame): a panel at the left, the rows
// numbered, A (or a click, Enter) sends a row's command and closes the menu, B (Escape, the right button) closes it
std::string PcRadioScreens() {
    std::string out;
    for (const Menu& m : Menus()) {
        std::string rows;
        int n = 0;
        for (const MenuRow& r : m.rows)
            rows += "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect \"!" + std::string(r.command) +
                    ";xbui exit\"\r\n\t\t\tCellFixed \"" + std::to_string(++n % 10) + ". " + r.label + "\"\r\n\t\t}\r\n";
        out += std::string("\r\n") + m.screen + "\r\n{\r\n"
               "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_enter.wav\"\r\n"
               "\tSelectSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
               "\tPieceOffsetDegrees \"90\"\r\n\tCachedImage \"angleSelect\t\t\t\t255 255 255 255\"\r\n"
               "\tCommandSelect \"RowSelect\"\r\n"
               "\tBackgroundImage \"csxbackground\t\t100 100 80 200\t\t40 96 330 " +
               std::to_string(56 + 22 * int(m.rows.size())) + "\"\r\n"
               "\tOnButtonDownA \" \"\r\n\tOnButtonDownB \"back\"\r\n\tOnButtonDownStart \"back\"\r\n"
               "\tBackgroundText \"177 169 54 255 52 100 300 24 0.8\"\t\"" + std::string(m.title) + "\"\r\n"
               "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
               "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
               "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
               "\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tColumnJustify \"left\"\r\n"
               "\t\t\tTableVerticalWrap\r\n\t\t\tTableLocation \"56 130\"\r\n\t\t\tTableSize \"300 " +
               std::to_string(22 * int(m.rows.size()) + 10) + "\"\r\n\t\t\tTableTextScale \"0.7\"\r\n"
               "\t\t\tTableRowSpacing \"2\"\r\n\t\t\tNonSelectBG \"0 0 0 0\"\r\n\t\t\tSelectBG \"0 0 100 60\"\r\n"
               "\t\t\tNonSelectFG \"1 1 1 1\"\r\n"
               "\t\t}\r\n" + rows + "\t}\r\n}\r\n";
    }
    return out;
}

// the client: "radio1" .. "radio3" typed or bound: the radio menu's screen opened here (true: not sent to the server,
// whose text menu this game's client cannot draw)
namespace {
std::atomic<int64_t> g_menu_opened{0};  // steady clock, ms: when a radio menu was last opened
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// a radio menu opening (the front end's pause decision for a screen that opens in a single player match, 0xCA00F /
// 0xCA017: net_session.cpp): the match goes on behind it, as behind the buy menu
bool PcRadioMenuOpening() {
    // the screen shown (the menus' current screen, 0x5523D4), or one asked for a moment ago and not up yet
    uint32_t screen = R32(0x5523D4);
    if (screen && R32(screen) && kernel::ReadCString(R32(screen), 32).rfind("InGame_PCRadio", 0) == 0) return true;
    return NowMs() - g_menu_opened.load() < 1500;
}

bool PcRadioMenuCommand(const std::string& cmd) {
    if (cmd != "radio1" && cmd != "radio2" && cmd != "radio3") return false;
    g_menu_opened = NowMs();
    csx::QueueEngineCommand("xbui open InGame_PCRadio" + cmd.substr(5) + "\n");
    LOGI("radio: the %s menu opened", cmd.c_str());
    return true;
}

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
