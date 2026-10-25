Counter-Strike (original Xbox), statically recompiled: Windows x86-64 build

This package holds no game data. You need the files of your own copy of the game (the folder with default.xbe,
Media\ and the .sxwad maps); they are only read, never changed.

Run:   run_game.bat "C:\path\to\Counter-Strike"
       (or: CounterStrike.exe "C:\path\to\Counter-Strike" --renderer vulkan, from this folder)

Needs: Windows 10 or later (64-bit), a GPU driver with Vulkan 1.3 (vulkan-1.dll comes with the driver).
Writes: logs\ (the log) and hdd\ (the Xbox hard disk: saves, settings, cache) in this folder.

Settings (environment variables):
  CSX_MOUSE_SENS=N     mouse sensitivity (default 3, the game's default)
  CSX_MOUSE_INVERT=1   inverted pitch
  CSX_AUDIO=null       no sound
  FPS=N                frame pacing (default 30, the console's rate; 0 = the game's own 72 frames/s cap)
  CSX_RENDERER=software  the CPU reference renderer (very slow; for comparisons)

Controls (the game reads an Xbox controller; a gamepad works directly, keyboard and mouse stand in for it):
  W A S D       left stick (move)            mouse        look (click the window to capture, F1 releases)
  left mouse    right trigger (fire)         right mouse  left trigger
  Space         A                            Left Ctrl    B
  R             X                            Q            Y
  1 / 2         white / black                arrows       d-pad
  Enter or Esc  START                        Tab          BACK
  Left Shift    left stick click             middle mouse right stick click
  F11 or Alt+Enter  fullscreen               closing the window quits

Third-party: SDL3.dll (zlib license, licenses\SDL3-LICENSE.txt), dxcompiler.dll (DirectX Shader Compiler,
licenses\dxc\), libsodium 1.0.22 linked into the exe (ISC license, https://libsodium.org: the network transport's
cryptography).
