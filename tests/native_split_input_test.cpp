// Native split-screen M4's controller seats (src/common/pad_seats.h, docs/native_splitscreen.md 18.3) without SDL or
// the game: START to take a seat, the order seats are given, a disconnect keeping the player in the game, the same
// controller (or another's START) taking the waiting seat back, two identical controllers kept apart by their keys,
// the keyboard-and-mouse player 1 with and without a controller of its own, leaving, the seats across a relaunch.
#include <cstdio>
#include <string>

#include "common/pad_seats.h"
#include "game/native_split_core.h"

using namespace seats;

static int checks = 0, failed = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL: %s\n", what);
    }
}

// keyboard and mouse + one controller: the controller's START makes player 2
static void KeyboardAndOne() {
    Table t;
    Check(t.Players() == 1 && t.InGame(0) && t.DeviceOf(0) == 0, "player 1 alone: the keyboard and mouse, no controller");
    Check(t.Connected(11, "/dev/input/event11") == -1, "a new controller takes no seat by connecting");
    Check(t.Players() == 1, "still one player");
    int seat = -1;
    Check(t.Pressed(11, true, 4, &seat) == Start::Joined && seat == 1, "its START: player 2");
    Check(t.DeviceOf(1) == 11 && t.SeatOf(11) == 1, "player 2 reads it");
    Check(t.Pressed(11, true, 4, &seat) == Start::Own && seat == 1, "its START again: its own (the game's pause)");
    Check(t.Players() == 2 && t.Mask() == 0x3, "two players");
    Check(t.DeviceOf(0) == 0, "player 1 still has no controller");
}

// two to four controllers, each its own player in START order (not connection order)
static void FourControllers() {
    Table t;
    for (uint32_t d = 21; d <= 24; d++) t.Connected(d, "pad" + std::to_string(d));
    int seat = -1;
    Check(t.Pressed(23, true, 4, &seat) == Start::Joined && seat == 1, "the third connected, first to press: player 2");
    Check(t.Pressed(21, true, 4, &seat) == Start::Joined && seat == 2, "then player 3");
    Check(t.Pressed(24, true, 4, &seat) == Start::Joined && seat == 3, "then player 4");
    Check(t.Pressed(22, true, 4, &seat) == Start::Refused, "a fifth player: refused (seat 0 is the keyboard's)");
    Check(t.Players() == 4 && t.Mask() == 0xF, "four players");
    // keyboard and mouse + three controllers is the same table; two controllers and no keyboard use: player 1 takes one
    Table u;
    u.Connected(31, "a");
    u.Connected(32, "b");
    Check(u.GiveP1Controller() == 0 && u.DeviceOf(0) == 31, "Also a Controller: player 1 the first connected");
    Check(u.Pressed(31, true, 4) == Start::Own, "player 1's controller's START is its own");
    Check(u.Pressed(32, true, 4, &seat) == Start::Joined && seat == 1, "the other: player 2");
}

// a match's layout limit, and joining closed
static void Limits() {
    Table t;
    t.Connected(1, "a");
    t.Connected(2, "b");
    t.Connected(3, "c");
    int seat = -1;
    Check(t.Pressed(1, true, 2, &seat) == Start::Joined && seat == 1, "a two-player match: player 2");
    Check(t.Pressed(2, true, 2) == Start::Refused, "no third seat in a two-player limit");
    Check(t.Pressed(3, false, 4) == Start::Refused, "joining closed: refused");
    Check(t.Players() == 2, "two players");
}

// a controller disconnecting during play: the player stays (neutral input); the same controller takes its seat back
static void DisconnectReconnect() {
    Table t;
    t.Connected(5, "/dev/input/event5");
    t.Connected(6, "/dev/input/event6");
    t.Pressed(5, true, 4);
    t.Pressed(6, true, 4);
    Check(t.Disconnected(5) == 1, "player 2's controller gone");
    Check(t.At(1).state == SeatState::Waiting && t.DeviceOf(1) == 0, "its seat waits, no controller read (neutral)");
    Check(t.InGame(1) && t.Players() == 3, "player 2 stays in the game");
    Check(t.DeviceOf(2) == 6, "player 3 untouched");
    Check(t.Connected(55, "/dev/input/event5") == 1 && t.DeviceOf(1) == 55, "it comes back (a new id): its seat again");
    Check(t.At(1).state == SeatState::Held, "held again");
    // another controller's START takes a waiting seat before any new one
    t.Disconnected(55);
    t.Connected(7, "/dev/input/event7");
    int seat = -1;
    Check(t.Pressed(7, true, 4, &seat) == Start::Resumed && seat == 1, "a new controller's START: the waiting seat");
    Check(t.Players() == 3, "no new player for it");
    Check(t.Connected(56, "/dev/input/event5") == -1, "the old controller back later: no seat waits for it now");
    Check(t.Pressed(56, true, 4, &seat) == Start::Joined && seat == 3, "its START: a new seat");
    // an unknown device's START (not connected) does nothing
    Check(t.Pressed(999, true, 4) == Start::Refused, "an unknown device: refused");
}

// two identical controllers (the same GUID): the window's keys (device path, else a running number) keep them apart
static void IdenticalControllers() {
    Table t;
    t.Connected(1, "045e:028e#1");
    t.Connected(2, "045e:028e#2");
    t.Pressed(1, true, 4);
    t.Pressed(2, true, 4);
    t.Disconnected(2);
    Check(t.Connected(3, "045e:028e#1") == -1, "the other one's key does not take player 3's seat");
    Check(t.At(2).state == SeatState::Waiting, "player 3 still waits");
    Check(t.Connected(4, "045e:028e#2") == 2, "its own key does");
}

// player 1's own controller disconnecting: player 1 plays on with the keyboard and mouse
static void Player1Controller() {
    Table t;
    t.Connected(9, "p1pad");
    t.GiveP1Controller();
    Check(t.Disconnected(9) == 0, "player 1's controller gone");
    Check(t.At(0).state == SeatState::Held && t.InGame(0), "player 1 stays held (the keyboard and mouse)");
    Check(t.Connected(10, "p1pad") == 0 && t.DeviceOf(0) == 10, "back: player 1's again");
    t.Leave(0);
    Check(t.InGame(0) && t.DeviceOf(0) == 0, "player 1 lets its controller go, still player 1");
}

// leaving: the seat empty, the controller free to join again
static void Leaving() {
    Table t;
    t.Connected(1, "a");
    t.Connected(2, "b");
    t.Pressed(1, true, 4);
    t.Pressed(2, true, 4);
    t.Leave(1);
    Check(!t.InGame(1) && t.InGame(2) && t.Players() == 2, "player 2 left; player 3 stays (no renumbering)");
    int seat = -1;
    Check(t.Pressed(1, true, 4, &seat) == Start::Joined && seat == 1, "its START again: the empty seat 2");
    Check(t.FirstUnseated() == 0, "no unseated controller");
}

// the seats across the match's relaunch
static void Relaunch() {
    Table t;
    t.Connected(1, "/dev/input/event3");
    t.Connected(2, "/dev/input/event4");
    t.Pressed(2, true, 4);
    t.Pressed(1, true, 4);
    std::string saved = t.Save();
    Check(saved == "1=/dev/input/event4;2=/dev/input/event3", "saved: each seat's controller key");
    Table u;
    Check(u.Load(saved) == 2, "two seats restored");
    Check(u.InGame(1) && u.InGame(2) && u.At(1).state == SeatState::Waiting, "they wait for their controllers");
    Check(u.Connected(40, "/dev/input/event3") == 2 && u.Connected(41, "/dev/input/event4") == 1,
          "the new process's connection events give each its own seat back");
    Check(u.Players() == 3, "three players");
    Table v;
    Check(v.Load("1=;5=x;=y;junk") == 1 && v.InGame(1), "a seat without a key waits for any START; junk ignored");
    v.Connected(70, "z");
    Check(v.Pressed(70, false, 2) == Start::Resumed && v.DeviceOf(1) == 70, "any START takes it (even joining closed)");
}

// the game's side: a player is added only while its seat is in the game (nsplit::Players::SeatSlot)
static void GameFollowsSeats() {
    nsplit::Players p;
    Check(p.SeatSlot(~0u, 2) == 1, "unknown seats (headless): the first free slot");
    Check(p.SeatSlot(0x1, 4) == -1, "only player 1's seat: nobody added");
    Check(p.SeatSlot(0x5, 4) == 2, "seat 2 (player 3) in the game, seat 1 not: player 3 added");
    Check(p.SeatSlot(0xF, 2) == 1 && p.SeatSlot(0xD, 2) == -1, "never past the match's players");
    p.Begin(1, 1, "p2");
    Check(p.SeatSlot(0x3, 4) == -1, "player 2 already there: nobody else wanted");
    Check(p.SeatSlot(0xB, 4) == 3, "player 4's seat: slot 3");
}

int main() {
    GameFollowsSeats();
    KeyboardAndOne();
    FourControllers();
    Limits();
    DisconnectReconnect();
    IdenticalControllers();
    Player1Controller();
    Leaving();
    Relaunch();
    std::printf("native_split_input_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
