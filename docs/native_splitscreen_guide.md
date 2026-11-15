# Native split-screen: playing it, and what to check by hand

The engineering record is docs/native_splitscreen.md (M0-M8, every run). This page is for playing and for the manual
test. Native split-screen is 2-4 local players in **one** game process, one window, the game's own server, one world:
each player has its own camera, HUD, weapon, controller and in-game menus. The earlier split-screen (one game process
per player, docs/splitscreen.md) is kept as a fallback until this one is accepted.

## Starting

- **From the menus (the usual way):** `./run_game.sh`, then Main menu > **Instant Action**. Player 1 plays with the
  keyboard and mouse (the first controller is player 1's in the menus). Every other controller joins with **START**
  on the Instant Action screen (the panel on the right lists P2-P4); its **BACK** leaves. **Start Match** starts the
  match with everyone who joined, in one game. With only player 1 it is the ordinary single-player match.
- **Straight into a match (development and quick tests):** `./run_game.sh --native 2 [MAP] [--bots N]` (2-4 players;
  `--match` options apply): players 2..N take their places with START on any controller.
- **The fallback:** Options > Split Screen Options > Split-Screen Engine: Separate Processes (or
  `./run_game.sh --splitscreen N`), the earlier multi-process split-screen.

## Controllers

- Player 1: keyboard and mouse; with Split Screen Options > Player 1: Also a Controller, the first controller too.
- Players 2-4: a controller each, given by the order of their **START** presses (not the order they were plugged in).
  The START that takes a place is not passed to the game.
- **A controller unplugged in a match:** its player stays in the match, standing still. Plugging the same controller
  back gives the place back at once (no START); another controller's START takes it too.
- **Joining in a match:** START on a free controller joins (up to four). A player who joins a two-player match makes the
  layout four quarters; the views keep their HUD whole (the frames were made for two players: a quarter shows the
  middle of a wide view). The next match starts with the right shape.
- Rumble goes to each player's own controller.

## In the match

- Every player has its **own** team screen, class choice, buy menu (several open at once are fine), scoreboard and
  pause menu, driven by its own controller (player 1: its keys and mouse, as before).
- **Pause:** with two or more players a pause menu is that player's own; the match goes on for everyone. Alone, the
  game pauses as before.
- Players 2-4's pause menu: Return to Game, Match Statistics, Player Settings, **Leave Split-Screen** (confirmed): that
  player leaves, the others play on, the layout closes up. Its START joins again later.
- Player 1's pause menu is the host's (Restart Match, Change Map, Quit: Quit ends the match for everyone).
- Layouts: two players top and bottom (Split Screen Options > Two Players: Side by Side), three or four in quarters
  (with three, the fourth quarter stays black). Split View Field of View: As Full Screen (the default: each view shows
  the field of view a full screen would, less stretching at the sides) or Widest.

## Known limitations

- **Memory per leave:** a player 2-4 who leaves leaves about 0.9 MB of the game's own objects behind (they are never
  freed on a guess: docs 15.20). The game allows 8 MB of this; past that, joining again is refused for the rest of the
  match (nothing breaks; a new match starts fresh). About eight leave-and-rejoin cycles per match.
- **Sound** has one listener, player 1's: every sound is heard once, as player 1's client places it (players 2-4's own
  shots and footsteps come through player 1's client, at their place in the world: quieter the farther they are from
  player 1). Their own sounds are not started a second time (no doubling); a per-player mix is not done yet.
- **One profile:** the title has one profile (player 1's); players 2-4 play as guests ("Player 2"...). Their Player
  Settings change the shared PC settings.
- **The frame shape** is chosen when a match starts; joins and leaves inside a match change the layout, not the frame
  (views are cut at the sides or shown whole, never stretched; the HUD is never cut).
- A solo match stays the single-player game: others join from Instant Action, or at a later match.
- The pie menus' bundle cache has two slots: with more different pies open at once the title loads them uncached
  (seen working with four players, run 25; not yet watched over a long session).
- Windows: native split-screen is built and tested on Linux only (the Windows window front end has no seats yet).

## What has been verified in the game (automated runs) and what has not

Verified by runs 17-25 (docs/native_splitscreen.md 16.7, 17.14, 18.9, 18.10, 18.13):
- two and four players in one process: their own views, HUDs, weapons, cameras, FOV; movement and aiming isolated
  (checked at the server for every player); a player's departure and rejoin; the server kept;
- the seats: START taking a place (and not reaching the game), a controller unplugged and plugged back (four players);
- own menus: team screens and buy menus of every player open at once, purchases at the server, players 2-4's pause
  menu, Leave Split-Screen, a rejoin by START; no ownership fault, no server write into a player's state;
- performance at 30 frames a second with four views: each player's turn about 4 ms, GPU under 1 ms a frame.

Not verified by a run: Instant Action's START-to-join and Start Match into a native match (run 27 prepared); physical
controllers (only SDL virtual ones); keyboard-and-mouse player with a controller of its own; rumble; sound; frame
rates above 30 with several players; long sessions; a map change with several players; the side-by-side layout.

## Manual test checklist

Prepare: two to four physical controllers (Xbox-style; mixed brands welcome), the keyboard and mouse, speakers or a
headset. Start with `./run_game.sh`. Note anything odd with the time (the log is build/play/logs).

1. **Instant Action join.** Main menu > Instant Action. Press START on a second controller: P2 appears in the panel.
   BACK on it: P2 leaves. Join again; join a third and fourth controller. Press START on the first controller (player
   1's): it is the menus' button, nobody joins.
2. **Start Match** with two players: one loading, then two views top and bottom. Both team screens open at once.
3. **Team choice:** player 1 with the keyboard (arrows and Space, or the mouse), player 2 with its controller, at the
   same time. Each gets the team it chose.
4. **Buying at the round start:** both open their buy menus at once; buy different things; each gets what it bought;
   one closing its menu does not close or disturb the other's.
5. **Playing:** move, aim, shoot, reload, switch weapons, crouch, jump on each player; nothing one does moves the other.
   The mouse aims only player 1; a controller only its own player.
6. **Pause:** player 2's START: its own pause menu; player 1 keeps playing; the game keeps running. Player 1's Escape:
   its pause menu (the host's); player 2 keeps playing.
7. **Unplug** player 2's controller mid-round: player 2 stands still, nobody is disconnected; plug it back: it plays
   again with no button press. Unplug it again and press START on a different free controller: that one takes over.
8. **Leave Split-Screen** from player 2's pause menu: one view, player 1 plays on; player 2's START: it joins again
   (its team screen), two views.
9. **Three and four players:** join more controllers in a match (START): quarters; every HUD whole; play a round.
10. **Rumble:** take damage on player 2: only its controller rumbles.
11. **Sound:** shots and footsteps heard; nothing doubled; nothing missing (see the limitation above).
12. **Feel and performance:** aim with the mouse (raw input on and off) and with controllers; at 1080p, 1440p and 4K
    if you can; with VSync off and a higher frame limit; with bots. Is it smooth with two? with four?
13. **Map change:** let a match end (or Change Map from player 1's pause menu): every player comes back after the load.
14. **Options:** Split Screen Options: Two Players: Side by Side; Split View Field of View: Widest and back.
15. **The fallback:** Split-Screen Engine: Separate Processes, then Instant Action with two players: the earlier
    split-screen still works.
