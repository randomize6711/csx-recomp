// Invite codes (N9, docs/n9_invites.md): a friend joins the match this PC plays from a code, without Steam or Discord.
// The code is the server's address and its identity key, "ADDRESS/KEY" (64 hex characters): typed or pasted into the
// server browser's address (pc_browser.cpp), the key is pinned for the connection as a favourite's is, so the code
// cannot take the friend to another server. Its password (if any) is not in it: it is told apart.
//   - in a match joined through the browser: the address and key it joined (CSX_BROWSER_JOIN, CSX_SERVER_KEY);
//   - in a match hosted from Host a Match (its local server, local_server.cpp): this PC's network address (the first
//     IPv4 address of a network interface that is up, not loopback) with the server's port and key. An offline match
//     (loopback only) has none.
// "pc_invite" (PC Controls > Copy Invite Code, F6) logs the code, says it on the console and copies it to the
// clipboard (the window takes it, window_input.cpp).
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include "common/csx.h"
#include "core/log.h"
#include "game/local_server.h"

namespace {
std::mutex g_lock;
std::string g_clipboard;  // a code for the window to copy (taken once)

std::string LanAddress() {
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::string out;
    for (ifaddrs* i = list; i && out.empty(); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
            continue;
        char b[INET_ADDRSTRLEN];
        auto* a = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
        if (inet_ntop(AF_INET, &a->sin_addr, b, sizeof b)) out = b;
    }
    freeifaddrs(list);
    return out;
#else
    return {};
#endif
}

std::string Port(const std::string& address) {
    size_t c = address.rfind(':');
    return c == std::string::npos ? std::string() : address.substr(c + 1);
}
}  // namespace

namespace csx {

std::string InviteCode(std::string& why_none) {
    const char* joined = std::getenv("CSX_BROWSER_JOIN");
    const char* key = std::getenv("CSX_SERVER_KEY");
    if (joined && *joined && key && std::string(key).size() == 64) return std::string(joined) + "/" + key;
    std::string local = LocalServerAddress(), local_key, mode;
    if (const char* e = std::getenv("CSX_LOCAL_SERVER")) {  // "PID ADDRESS KEY host|offline" (local_server.cpp)
        std::istringstream in(e);
        std::string pid, address;
        in >> pid >> address >> local_key >> mode;
    }
    if (local.empty() || local_key.size() != 64) {
        why_none = "this match has no server others can join";
        return {};
    }
    if (mode != "host") {
        why_none = "this match is offline (Host a Match makes one others can join)";
        return {};
    }
    std::string lan = LanAddress();
    if (lan.empty()) {
        why_none = "this PC has no network address";
        return {};
    }
    return lan + ":" + Port(local) + "/" + local_key;
}

std::string TakeClipboardRequest() {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string s;
    s.swap(g_clipboard);
    return s;
}

}  // namespace csx

// "pc_invite" (pc_menu.cpp's PcConsoleCommand)
void PcInviteCommand() {
    std::string why;
    std::string code = csx::InviteCode(why);
    if (code.empty()) {
        LOGI("invite: none: %s", why.c_str());
        csx::QueueEngineCommand("echo \"No invite code: " + why + "\"\n");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_clipboard = code;
    }
    LOGI("invite: %s (copied to the clipboard)", code.c_str());
    csx::QueueEngineCommand("echo \"Invite code copied: " + code + "\"\n");
}
