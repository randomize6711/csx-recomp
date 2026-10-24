# N0: split-screen in Counter-Strike: Xbox (default.xbe 5659), read-only investigation

Part of N0 (docs/pc_multiplayer.md section 10). build/run_ui/uidump/*.txt are text dumps of the game's menu files made
by an earlier run (build/ is not in git); the line numbers refer to those dumps.

VERDICT: no split-screen / multiple local players. One active controller, one profile, one client.

## Evidence
- Strings: no "split"/"splitscreen"/"player 2"/"cl_splitscreen"/"ss_" (only NET "Split packet" msgs 0x3332d0..). 
- Menus (build/run_ui/uidump/FrontEndUI.txt:280-480): main screen rows = Training, Single Player, Xbox Live, System Link, Options, Change Profile. No local MP mode.
  InGameUI.txt:6534 ControllerChooseNew "Please press START to select a new controller." ; 6559-6768 ControllerUnplugged{One..Four}[SP] "reconnect the controller to port N".
- Input: generic XBInput-like object at 0x384E70 (4 pads, stride 0xE0, init 0xBD7E0 called from 0xBF715; poll 0xBDA30 loops 4 ports -> XInputGetState 0x319670).
  Active port = single dword 0x3852F8. Writers: 0xE1A90 (pad whose wButtons has START 0x10 becomes active), 0xBF745 (from 0x5259E8 at boot), 0xC74BB, 0xC85EE/0xC8624 (re-pick after unplug).
  0xC83A8: jump table on 0x3852F8 chooses "xbui open ControllerUnplugged<N>[SP]" (strings 0x33B1A0..0x33B2A4).
  Gameplay buttons->keys 0x19DFD0 reads pad[0x3852F8] only; VGUI cursor 0x1094A0 same; rumble 0x1A7C22 compares port to it.
  Live code (0xE9C33, 0xEB6F6, 0xEE7AF, ...) passes 0x3852F8 as user index (inferred: XOnline user index).
- Client singletons: cl 0x749D40..0x780DC8 (analysis/player_slots_engine_client.md:16), cls.state 0x7812E0 / cls.signon 0x784800 (inferred from V_RenderView 0xB5EC2/0xB5ECF: ==5 ca_active, ==2),
  gHUD 0x5623D0 (src/game/pc_keys.cpp:122), cl_entities ptr 0x551890.
- Loopback: NET_SendLoopPacket 0x6BBA0: loopbacks[sock^1] at 0x440C00 stride 0x3F38, 4 msgs x 0xFCC -> exactly 2 sides (client/server). One local client.
- View: V_RenderView 0xB5E90 loops while ref_params.nextView (+0xE0) != 0, calling client CalcRefdef (fn ptr 0x7D6CEC = 0x1A0F10) and R_RenderView 0x4EF00;
  0xB5D10 copies ref_params.viewport[4] (+0xD0..+0xDC) -> r_refdef.vrect 0x6E7A20..0x6E7A2C, onlyClientDraw (+0xE4) -> 0x6E7AAC.
  Client spectator code sets nextView=1 + inset viewport (0x1A0CFF..0x1A0DE3), cvar spec_pip (0x35EF4C). => multi-view-per-frame into sub-viewports EXISTS (spectator PIP).
- Audio: one DSound 3D listener (0x3038A0, FINDINGS.md:205,325).

## Options
A. Two processes, composed into one window: needs PC UDP transport (docs/networking.md section 4.1, not done) or a shared-memory loopback bridge; frame sharing across processes; two audio streams.
B. One process, swap singleton state per frame (cl, cls, client DLL globals, gHUD, pmove, prediction, sound listener, VGUI/xbui menu state): state spans hundreds of KB of scattered BSS; huge audit. 
C. Second loopback pair + second client_t: server side fine (17/65 slots), client side still singleton -> reduces to B.
Cheapest: A with a host-side IPC net transport (inferred).
