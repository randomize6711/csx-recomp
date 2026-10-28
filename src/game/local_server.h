// The local server (N4.7, docs/splitscreen.md 17): a match the player starts from the game's menus (Single Player, or
// Multiplayer > Host a Match) runs in csx-server, a process of its own that the game starts and ends, instead of in the
// game's own process (the listen server, kept as the fallback: PC setting server_process 0). The game joins it as a
// client over the PC transport, so the server's frames (the bots' thinking above all) are never the game's frames.
#pragma once

#include <string>

struct X86Context;

namespace csx {

// a match from the menus runs in a local server: PC setting server_process (1, the default; 0 the listen server) or
// CSX_SERVER_PROCESS; never in csx-server itself or a split-screen player 2-4 (they join player 1's match)
bool LocalServerWanted();
// starts the local server for the menus' match line ("+map NAME +cvar value..."), host: Host a Match (the network's
// port, LAN discovery) or an offline match (this machine's loopback only); waits until it is ready. True with the
// line the game relaunches into instead ("+connect ADDRESS"), false with a message for the player
bool LocalServerStart(const std::string& match_line, bool host, std::string& connect_line, std::string& error);
// this game's local server ended (SIGTERM: its players told; SIGKILL after 5 s), and forgotten
void LocalServerStop(const char* why);
// a local server of this game runs (its process id, its address)
bool LocalServerRunning();
std::string LocalServerAddress();
// the client's trust in a server's key (net_engine.cpp): the local server's own address and key
bool LocalServerTrusted(const std::string& address, const std::string& key);
// every host frame (net_session.cpp): false once the server process ended by itself (its exit in why)
bool LocalServerAlive(std::string& why);
// a console command of the game's (Cmd_ExecuteString) that belongs to its local server: sent there as the owner's
// (pc_owner) instead of run in the game, which has no server; true when sent
bool LocalServerRoute(const std::string& line);
// the pause menu's pause in a local server's match: on when the menu opens, off when it closes (net_session.cpp)
void LocalServerPause(bool on);
// every host frame: a pause sent ends once the menu that sent it was open and no menu is (net_session.cpp)
void LocalServerPauseFrame(bool menu_open);

}  // namespace csx
