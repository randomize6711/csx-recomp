// Autobuy and Rebuy (N5C, PC Controls' Autobuy and Rebuy: "autobuy" / "rebuy", served by PcConsoleCommand). Both buy
// as the game's own buy menu does: its purchase (0xD8060, the item's name in ecx; the menu screens' "buy weapon_m4a1",
// "buy item_assaultsuit") looks the item up (0xDEC60, the name in ebx), checks the player may buy it (0xCC260, the
// item in edx: the money, the team, the grenade limits) and sends "impulse <the item's id>" (item +0x18) in the next
// move, and the server decides as for the menu: the money, the buy zone, the buy time, the team. Here the same
// three steps run without the menu open. The money the HUD shows (the Money message's value, 0x552C74, written by
// 0x1AE160) tells whether a buy went through: it drops within a moment, or the server refused.
//   Rebuy buys again what went through in the last round (the buys between two round starts: the RoundTime message,
//   0x1D4A90, starts each round on the client); the menu's own purchases are seen at 0xD8060 (a hook) and count.
//   Autobuy goes down a list (pc_settings.ini "autobuy", CS 1.6's cl_autobuy order by default): what the player may
//   not buy (the other team's weapons, too little money) is passed over at once; the first primary weapon, pistol and
//   body armour that go through are kept and the rest of their kind passed over; the defuser and grenades are tried.
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using clock_type = std::chrono::steady_clock;
constexpr uint32_t kMoney = 0x552C74;
constexpr uint32_t kFindItem = 0xDEC60;  // the buy menu's item by its name (ebx); 0 for none
constexpr uint32_t kCanBuy = 0xCC260;    // whether the player may buy the item (edx): al
constexpr uint32_t kMalloc = 0x22D86A;
constexpr char kAutobuy[] =
    "weapon_m4a1 weapon_ak47 weapon_famas weapon_galil weapon_p90 weapon_mp5navy item_assaultsuit item_kevlar defuser";

enum class Kind { Primary, Pistol, Armour, Other };

Kind KindOf(const std::string& item) {
    static const std::set<std::string> pistols = {"weapon_glock18", "weapon_usp", "weapon_p228", "weapon_deagle",
                                                  "weapon_elite", "weapon_fiveseven"};
    static const std::set<std::string> other = {"weapon_hegrenade", "weapon_flashbang", "weapon_smokegrenade",
                                                "weapon_shieldgun"};
    if (item == "item_kevlar" || item == "item_assaultsuit") return Kind::Armour;
    if (pistols.count(item)) return Kind::Pistol;
    if (item.rfind("weapon_", 0) == 0 && !other.count(item)) return Kind::Primary;
    return Kind::Other;
}

bool Valid(const std::string& item) {
    for (char c : item)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return !item.empty() && item.size() < 32;
}

int Money() { return int(kernel::R32(kMoney)); }

struct Wait {
    bool on = false;
    std::string item;
    int before = 0;
    clock_type::time_point at{};
};
Wait g_wait;                                  // a buy sent, its money not seen to drop yet
std::vector<std::string> g_round, g_last;     // what went through this round, and in the last
std::deque<std::string> g_queue;              // buys to send (Autobuy's, Rebuy's)
bool g_autobuy = false;                       // the queue is Autobuy's: a kind bought passes over the rest of it
std::set<Kind> g_bought;
const char* g_what = "";
clock_type::time_point g_next{};             // no buy sent before then

// the item's name in guest memory (a block made once, 32 characters)
uint32_t GuestName(X86Context& ctx, const std::string& item) {
    static uint32_t block = 0;
    if (!block) block = kernel::CallGuest(ctx, kMalloc, {32});
    if (!block || item.size() > 31) return 0;
    std::memcpy(kernel::HostPtr(block), item.c_str(), item.size() + 1);
    return block;
}

// the buy menu's purchase without the menu: its item, its check, its impulse; false when it may not be bought
bool SendBuy(X86Context& ctx, const std::string& item) {
    uint32_t name = GuestName(ctx, item), ebx = ctx.ebx, edx = ctx.edx;
    ctx.ebx = name;
    uint32_t found = name ? kernel::CallGuest(ctx, kFindItem, {}) : 0;
    ctx.ebx = ebx;
    if (!found) {
        LOGI("pc buy: no item %s", item.c_str());
        return false;
    }
    ctx.edx = found;
    bool may = (kernel::CallGuest(ctx, kCanBuy, {}) & 0xFF) != 0;
    ctx.edx = edx;
    if (!may) return false;
    csx::QueueEngineCommand("impulse " + std::to_string(kernel::R32(found + 0x18)) + "\n");
    return true;
}

void Start(const char* what, const std::vector<std::string>& items, bool autobuy) {
    g_queue.assign(items.begin(), items.end());
    g_autobuy = autobuy;
    g_bought.clear();
    g_what = what;
    LOGI("pc buy: %s: %zu to try, $%d", what, items.size(), Money());
}
}  // namespace

// a purchase sent (the menu's, Autobuy's, Rebuy's): watched for the money to drop
void PcBuySeen(const std::string& item) {
    if (!Valid(item)) return;
    g_wait = {true, item, Money(), clock_type::now()};
}

// [[hook]] at 0xD8060 (config/game.toml): the buy menu's purchase, the item's name in ecx
void PcBuyMenuItem(X86Context& ctx, uint8_t*) {
    if (ctx.ecx) PcBuySeen(kernel::ReadCString(ctx.ecx, 32));
}

// CS 1.6's buy aliases (the commands its players bind) as this game's items; "" for a command that is none
std::string PcBuyAlias(const std::string& cmd) {
    static const std::pair<const char*, const char*> kAliases[] = {
        {"glock", "weapon_glock18"}, {"usp", "weapon_usp"}, {"p228", "weapon_p228"}, {"deagle", "weapon_deagle"},
        {"elites", "weapon_elite"}, {"fn57", "weapon_fiveseven"}, {"m3", "weapon_m3"}, {"xm1014", "weapon_xm1014"},
        {"mac10", "weapon_mac10"}, {"tmp", "weapon_tmp"}, {"mp5", "weapon_mp5navy"}, {"ump45", "weapon_ump45"},
        {"p90", "weapon_p90"}, {"galil", "weapon_galil"}, {"defender", "weapon_galil"}, {"famas", "weapon_famas"},
        {"clarion", "weapon_famas"}, {"ak47", "weapon_ak47"}, {"cv47", "weapon_ak47"}, {"m4a1", "weapon_m4a1"},
        {"scout", "weapon_scout"}, {"sg552", "weapon_sg552"}, {"krieg552", "weapon_sg552"}, {"aug", "weapon_aug"},
        {"bullpup", "weapon_aug"}, {"sg550", "weapon_sg550"}, {"krieg550", "weapon_sg550"}, {"awp", "weapon_awp"},
        {"magnum", "weapon_awp"}, {"g3sg1", "weapon_g3sg1"}, {"d3au1", "weapon_g3sg1"}, {"m249", "weapon_m249"},
        {"vest", "item_kevlar"}, {"vesthelm", "item_assaultsuit"}, {"flash", "weapon_flashbang"},
        {"hegren", "weapon_hegrenade"}, {"sgren", "weapon_smokegrenade"}, {"defuser", "defuser"},
        {"shield", "weapon_shieldgun"},
    };
    for (auto& [alias, item] : kAliases)
        if (cmd == alias) return item;
    return {};
}

// "buyitem ITEM" (the buy menu's name: weapon_ak47, item_kevlar, defuser...) and CS 1.6's aliases: one purchase
void PcBuyItem(const std::string& item) {
    if (!Valid(item)) return;
    Start("Buy", {item}, false);
}

// "autobuy" / "rebuy"
void PcBuyCommand(const std::string& cmd) {
    if (cmd == "rebuy") {
        if (g_last.empty()) {
            LOGI("pc buy: Rebuy: nothing bought last round");
            return;
        }
        return Start("Rebuy", g_last, false);
    }
    std::vector<std::string> items;
    std::istringstream in(pc::Get("autobuy", kAutobuy));
    for (std::string w; in >> w;)
        if (Valid(w)) items.push_back(w);
    Start("Autobuy", items, true);
}

// [[hook]] at 0x1D4A90 (config/game.toml): the client's RoundTime message, at a round's start: the round's buys become
// the last round's
void PcBuyRoundStart(X86Context&, uint8_t*) {
    if (g_round.empty()) return;
    g_last.swap(g_round);
    g_round.clear();
    LOGI("pc buy: a round starts: Rebuy has %zu buys", g_last.size());
}

// on the game thread every frame (pc_keys.cpp's PcKeysFrame)
void PcBuyFrame(X86Context& ctx) {
    auto now = clock_type::now();
    static int shown = -1;
    if (int m = Money(); m != shown) {  // the HUD's money as it changes (what a buy or a round did)
        if (shown >= 0) LOGI("pc buy: money $%d (was $%d)", m, shown);
        shown = m;
    }
    if (g_wait.on) {
        int money = Money();
        if (money < g_wait.before) {
            g_round.push_back(g_wait.item);
            if (g_autobuy) g_bought.insert(KindOf(g_wait.item));
            LOGI("pc buy: %s bought ($%d, was $%d)", g_wait.item.c_str(), money, g_wait.before);
            g_wait.on = false;
        } else if (now - g_wait.at > std::chrono::milliseconds(700)) {
            if (!g_queue.empty() || g_autobuy) LOGI("pc buy: %s not bought", g_wait.item.c_str());
            g_wait.on = false;
        }
        if (g_wait.on) return;
        g_next = now + std::chrono::milliseconds(150);  // the next buy in a later move: one impulse a move
    }
    if (now < g_next) return;
    while (!g_queue.empty()) {
        std::string item = g_queue.front();
        g_queue.pop_front();
        Kind k = KindOf(item);
        if (g_autobuy && k != Kind::Other && g_bought.count(k)) continue;  // one of its kind went through
        if (!SendBuy(ctx, item)) continue;  // not this player's to buy (team, money): the next
        PcBuySeen(item);
        if (g_queue.empty()) LOGI("pc buy: %s sent its last", g_what);
        return;
    }
}
