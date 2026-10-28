// Split-screen hooks the game code calls whatever the front end (csx.h): the local player number from the
// environment, and a sender the window front end's player side (src/linux/split_player.cpp) registers; the headless
// harness and the dedicated server register none, so their messages go nowhere.
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "common/csx.h"

namespace {
std::atomic<void (*)(int, const std::string&)> g_send{nullptr};
std::atomic<void (*)(bool)> g_prepare{nullptr};
std::atomic<bool (*)(int)> g_starter{nullptr};
std::atomic<void (*)()> g_quitter{nullptr};
std::atomic<bool> g_party_open{false}, g_party_join{false};
std::mutex g_lock;
std::string g_state;
std::vector<std::string> g_party;
}  // namespace

namespace csx {

int SplitPlayer() {
    static const int player = [] {
        const char* p = std::getenv("CSX_SPLIT_PLAYER");
        int n = p ? std::atoi(p) : 0;
        return n >= 1 && n <= 4 ? n : 0;
    }();
    return player;
}

void SplitSend(int kind, const std::string& text) {
    if (auto f = g_send.load()) f(kind, text);
}

void SetSplitSender(void (*send)(int, const std::string&)) { g_send = send; }

std::string SplitState() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_state;
}

void SetSplitState(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_state = text;
}

void PushPartyInput(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_party.size() < 256) g_party.push_back(text);
}

std::vector<std::string> TakePartyInput() {
    std::lock_guard<std::mutex> lock(g_lock);
    std::vector<std::string> out;
    out.swap(g_party);
    return out;
}

void SetPartyScreenOpen(bool open) { g_party_open = open; }
bool PartyScreenOpen() { return g_party_open; }
void PartyJoinRequest() { g_party_join = true; }
bool TakePartyJoinRequest() { return g_party_join.exchange(false); }

void SplitPrepareRelaunch(bool match) {
    if (auto f = g_prepare.load()) f(match);
}

void SetSplitPrepareRelaunch(void (*prepare)(bool)) { g_prepare = prepare; }

bool SplitStart(int players) {
    auto f = g_starter.load();
    return f && f(players);
}

void SetSplitStarter(bool (*start)(int)) { g_starter = start; }

void AppQuit() {
    if (auto f = g_quitter.load()) f();
}

void SetAppQuitter(void (*quit)()) { g_quitter = quit; }

int SplitPlayers() {
    if (!SplitPlayer()) return 1;
    // the compositor's count (its State: "players N" first; a controller's START adds one), else the boot's
    if (std::string state = SplitState(); state.rfind("players ", 0) == 0) {
        int n = std::atoi(state.c_str() + 8);
        if (n >= 1 && n <= 4) return n;
    }
    const char* p = std::getenv("CSX_SPLIT_PLAYERS");
    int n = p ? std::atoi(p) : 1;
    return n >= 1 && n <= 4 ? n : 1;
}

}  // namespace csx
