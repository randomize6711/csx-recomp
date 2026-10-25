// The engine's network on the PC transport (net_engine.cpp) and the PC session code that uses it (net_session.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
// the match's Player Collision (pc_collision.cpp)
bool PlayerCollisionOn();

}  // namespace csx
