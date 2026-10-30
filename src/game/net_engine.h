// The engine's network on the PC transport (net_engine.cpp) and the PC session code that uses it (net_session.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct X86Context;
namespace net {
class Endpoint;
struct Identity;
}  // namespace net

namespace csx {

uint16_t NetServerPort();  // the server endpoint's UDP port: CSX_NET_PORT, else PC setting net_port, else 27015
void StartNetwork();       // at load: SIGPIPE ignored (Linux)
void NetPump();            // both endpoints read and timed (also done by every receive)
bool NetConnected(const std::string& peer);
bool NetServerHasClients();  // the server endpoint has a network client connected
// the server endpoint: every connection ended (a map change, a shutdown), or one (a dropped client), with a reason
// (net::Reason) and a text the client shows
void NetDisconnectServer(int reason, const std::string& text);
void NetDisconnectClient(const std::string& peer, int reason, const std::string& text);
void NetClientClose();  // the client endpoint's connection ended by the engine (disconnect)
std::optional<std::string> NetPeerIdentity(const std::string& peer);  // a connected client's identity fingerprint
// the client endpoint lost its server (net_session.cpp: a map change reconnects there)
void NetServerClosed(const std::string& peer, int reason, const std::string& text);
// the client endpoint connected to a server (net_session.cpp: a later connect in this process relaunches)
void NetServerJoined();
// the process was started to join a server (`+connect host`): it opens no server endpoint (net_session.cpp)
bool NetClientOnly();
// the match takes network players (its server endpoint opens): CSX_NET_HOST=1, PC setting net_host 1, a test's
// numbered instance; never in a client-only process
bool NetHosting();
// the server endpoint as it opened (N4.7: csx-server's status to the game that started it): its identity key (64 hex
// digits), its port, or why it could not open (empty until either)
std::string ServerIdentityKey();
uint16_t ServerEndpointPort();
std::string ServerEndpointError();
// the process is the dedicated server (csx-server: CSX_DEDICATED=1; dedicated.cpp); its host frames so far
bool Dedicated();
uint64_t ServerFrames();
// the dedicated server shut down (its quit: the players told), so csx-server exits
void SetServerFinished();
bool ServerFinished();
// remote administration (server_admin.cpp): the "admin" client command of client cl (true: it was one, handled);
// the actions it deferred to the host frame's start; whether a connecting identity or address is banned
bool AdminCommand(X86Context& ctx, uint32_t cl, const std::string& text);
void RunAdminActions(X86Context& ctx);
bool Banned(const std::string& identity, const std::string& ip, std::string& why);
void DropNetworkClient(X86Context& ctx, const std::string& address, const std::string& reason, bool now);  // server_admin.cpp
// a local server the game started for its match (N4.7, server_admin.cpp): its owner's "pc_owner" client command (true:
// it was one, handled), and the owner's pause ended when another player connects
bool OwnedServer();
int OwnerSlot();  // the owner's client slot (0-based), -1 while not connected
bool OwnerCommand(X86Context& ctx, uint32_t cl, const std::string& text);
// a client's "csx_radio NAME" (N5C, pc_radio.cpp): Condition Zero's extra radio lines, checked and played
bool RadioCommand(X86Context& ctx, uint32_t client, const std::string& text);
void OwnerPauseCheck(X86Context& ctx);
bool OwnerPaused();  // the owner's pause menu stopped the simulation
// the server browser's join: the title relaunches into "+connect server" between frames (net_session.cpp)
void RequestJoin(const std::string& server);
// the party lobby's Start (N4.8): the title relaunches into that "+map ..." line between frames
void RequestMatch(const std::string& line);
// back to the front end between frames (the party leader's Return to Lobby: the local server stopped as the title's
// own end of a match stops it)
void RequestFrontEnd();
// split-screen players 2..4 (N4.8.1): the title's own end of a session (Leave Match's Yes, SystemLinkClientEndSession)
// was this player's choice; its end tells the compositor "left" (its slot is not respawned at the next map)
void SplitLeaveChosen();
// the title relaunched into another map, its players told to rejoin (net_session.cpp; the dedicated server)
[[noreturn]] void RelaunchToMap(X86Context& ctx, const std::string& map);
// the party's lobby starts its match (N4.8): the title relaunches into the lobby's "+map ..." line, a Single Player
// match's launch data (RelaunchNotify starts the local server)
[[noreturn]] void RelaunchIntoMatch(X86Context& ctx, const std::string& line);
// the match's Player Collision (pc_collision.cpp)
bool PlayerCollisionOn();
// the server browser's server side (server_query.cpp): queries answered and master servers registered with on the
// server endpoint (bound_to_one: the endpoint is bound to one address, so a LAN listener takes the broadcasts); its
// per-frame work on the game thread; the dedicated server's wait for its next tick, answering queries meanwhile
void ServerQueryStart(net::Endpoint& ep, const net::Identity& id, bool bound_to_one);
void ServerQueryStop();
void ServerQueryFrame(X86Context& ctx);
void ServerQueryWait(double seconds);

}  // namespace csx
