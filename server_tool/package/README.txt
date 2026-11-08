Counter-Strike Xbox Dedicated Server (csx-recomp)
=================================================

An unofficial dedicated server for the Counter-Strike Xbox PC port, with a graphical server tool. Not affiliated with
Valve, Microsoft or Steam. NO GAME FILES ARE INCLUDED: the server runs from your own copy of the game.

What is in this folder
----------------------
  csx-server-tool        start here: the server tool (it sets up bin/lib for the programs)
  bin/csx-server-tool    the server tool's program
  bin/csx-server         the dedicated server (no window; the tool runs it, or run it yourself, below)
  bin/lib/               libSDL3 and libsodium for the programs (zlib and ISC licenses, in licenses/)
  maps/                  imported maps (optional): NAME.SXWad with its NAME.json report
  server.cfg.example     every server setting with its meaning
  csx-server-tool-data/  made on first start: profiles, settings, each server's folder and logs (portable: keep
                         the whole folder together)

Requirements
------------
- 64-bit Linux with glibc 2.43 or newer (a current distribution).
- Your own Counter-Strike (Xbox) game files: the disc's files (default.xbe, FrontEnd.SXWad, the cs_*/de_* .SXWad maps,
  the Media folder). The supported default.xbe is title ID MS-054 (PAL), 3 973 120 bytes,
  SHA-256 e7698b09ed7322b69cf4c9c6d3778bb0864a15594b30258b5637ddcf8ee8308f.
  The tool checks this before a server starts. Your files are read where they are and never changed.

Starting a server
-----------------
1. Run ./csx-server-tool
2. On the Overview page, choose your game folder (Browse..., or paste its path and press Use This Folder).
3. Configuration: the server's name, password, maximum players (up to 65, humans and bots together), the map, the
   rules. Maps: the starting map and the rotation. Bots: how many and how good.
4. Overview > Start Server. The status turns Online when the map is loaded.

Connecting from the game
------------------------
- In the PC port's main menu: Multiplayer > Server Browser (LAN tab), or Direct-IP / Code with this computer's
  address (the Overview page lists it) and the port (27015 by default).
- From a terminal: ./run_game.sh --connect ADDRESS[:PORT] (add --password P for a passworded server).
- Players on the internet need your public address and the UDP port forwarded in your router and allowed in your
  firewall. The tool changes neither. Internet play was tested on one machine and a LAN only.

Administering
-------------
Players: kick, ban (by the player's identity key), unban, messages. Maps: change the map now, edit the rotation.
Bots: the count and difficulty apply from the next map (the game sets its bots when a match starts); Apply Now reloads
the current map. Console: the server's live output and its commands (admin help lists the administration commands).
Logs: this session and earlier ones, search, export.

Stopping
--------
Overview > Stop Server (or Server > Stop Server). Connected players are told the server is shutting down. Closing the
tool while a server runs asks first and then stops it the same way.

Running the server without the tool
-----------------------------------
  cd SOMEWHERE_FOR_ITS_STATE && LD_LIBRARY_PATH=/path/to/bin/lib /path/to/bin/csx-server --game GAME_FOLDER --map de_dust2
  (--help lists every setting; --config FILE reads them from a file like server.cfg.example)
