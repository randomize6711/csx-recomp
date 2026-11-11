// Native split-screen's M2 lifecycle without the game (docs/native_splitscreen.md 15.12): which console buffer a line
// goes to and where it runs, the front end's commands, the shared voice channels' owner, a leaving player's cleanup
// order and what of its own it may release, and the bound on what leaving players leave behind. Tested on its own by
// tests/native_split_lifecycle_test.py; src/game/native_split.cpp drives it from the game.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/native_split_core.h"

namespace nsplit {

// ---- the console's command buffer (Cbuf, docs 15.12) ---------------------------------------------------------------
//
// The title has one command buffer (cmd_text 0x748660: its data a process heap block of 0x4000 bytes), appended to by
// Cbuf_AddText 0x3B970 (edi the text), prepended to by Cbuf_InsertText 0x3B9D0 (eax) and 0x3BA80 (its stack argument,
// with a newline each side), and run by Cbuf_Execute 0x3BB50: once a frame by _Host_Frame at 0x5D3AB, before the
// players' turns (so in player 1's context), and at once by the callers that add a line and run it (0x127960,
// 0x1A5B80, 0x1B69F0, 0x5BC40, 0x65ED0, 0xAED30, the exec command 0x3BF60, ...). A line a player's client adds in its
// own turn (svc_stufftext, its client game's ClientCmd, its menus' "!" lines) is that player's: run in player 1's
// context it would act on player 1's client (its cvars, its connection: a stuffed "disconnect" would end player 1's).
//
// Where a line goes, by where it is added:
enum class Origin {
    Process,   // outside the players' turns (the console, the menus' own frame, the host): the shared buffer
    Server,    // the built-in server's frame (SV_Frame, 0x5D3CD..0x5D3D2): the shared buffer
    Player,    // a player's turn, or a line its own lines add while they run: that player's own queue
    FrontEnd,  // the shared front end's or the voice channels' scope (run as the process): the shared buffer
};
constexpr int kOrigins = 4;
inline const char* Name(Origin o) {
    switch (o) {
    case Origin::Process: return "the process";
    case Origin::Server: return "the built-in server";
    case Origin::Player: return "a player";
    case Origin::FrontEnd: return "the shared front end";
    }
    return "?";
}
struct Route {
    Origin origin;
    int player;   // the player whose queue it goes to (Origin::Player), else -1
    bool native;  // false: the hook keeps it (a player's queue); true: the title's own code appends it
};
// `running`: the player whose queue is being run (its lines are in the title's buffer meanwhile), -1 none
inline Route RouteText(int running, bool shared_scope, bool server_frame, bool in_turn, int turn_player) {
    // the shared front end's (or the voice channels') scope first: its lines are the process's even while a player's
    // run has the title's buffer (native_split.cpp puts them with the shared lines kept aside)
    if (shared_scope) return {Origin::FrontEnd, -1, true};
    if (running >= 0) return {Origin::Player, running, true};      // its own lines' lines: run with them
    if (server_frame) return {Origin::Server, -1, true};
    if (in_turn && turn_player >= 0) return {Origin::Player, turn_player, false};
    return {Origin::Process, -1, true};                            // also a turn of no known player
}
// Cbuf_Execute called in a player's turn (a line added and run at once) runs that player's queue, never the shared
// buffer in its context; outside the turns (and while a queue already runs) the title's
inline bool ExecuteRunsQueue(int running, bool in_turn, int turn_player) {
    return running < 0 && in_turn && turn_player >= 0;
}

// each player's queue: its lines in order, as the title's buffer would hold them (the same bound: an addition that
// would fill it is refused, as Cbuf_AddText's "overflow")
constexpr size_t kQueueMax = 0x4000;
class CommandQueues {
public:
    bool Add(int k, const std::string& text) {
        if (!Valid(k) || q_[size_t(k)].size() + text.size() >= kQueueMax) return Overflow(k);
        q_[size_t(k)] += text;
        return true;
    }
    bool Insert(int k, const std::string& text) {
        if (!Valid(k) || q_[size_t(k)].size() + text.size() >= kQueueMax) return Overflow(k);
        q_[size_t(k)].insert(0, text);
        return true;
    }
    // its lines to run (the queue empty after), and what they did not run (a "wait") back in front of what was added
    // meanwhile
    std::string Take(int k) {
        if (!Valid(k)) return {};
        std::string s;
        s.swap(q_[size_t(k)]);
        return s;
    }
    void Return(int k, const std::string& rest) {
        if (Valid(k) && !rest.empty()) q_[size_t(k)].insert(0, rest);
    }
    // a player leaving: its lines dropped (never run in another context); the bytes dropped
    size_t Drop(int k) {
        if (!Valid(k)) return 0;
        size_t n = q_[size_t(k)].size();
        q_[size_t(k)].clear();
        return n;
    }
    size_t Size(int k) const { return Valid(k) ? q_[size_t(k)].size() : 0; }
    uint64_t Overflows(int k) const { return Valid(k) ? overflow_[size_t(k)] : 0; }

private:
    static bool Valid(int k) { return k >= 0 && k < kMaxPlayers; }
    bool Overflow(int k) {
        if (Valid(k)) overflow_[size_t(k)]++;
        return false;
    }
    std::array<std::string, kMaxPlayers> q_{};
    std::array<uint64_t, kMaxPlayers> overflow_{};
};

// the client game's front-end command (0xCA6F0: "xbui <command>", or "!<line>" a console line) in a player's turn:
// the driver's goes on (its "xbui" lines run in the shared front end's scope, 0xD7450's gate); another player's
// "!" line is its own console line (slot, impulse, menu selections: its queue; any "xbui" part in it is decided by
// the xbui gate when it runs), its plain "xbui" line - a shared screen opened or closed - withheld
enum class FrontEnd { Run, Withheld };
inline FrontEnd FrontEndCommand(const char* text, bool driver) {
    if (driver) return FrontEnd::Run;
    return text && text[0] == '!' ? FrontEnd::Run : FrontEnd::Withheld;
}

// ---- the voice channels (docs 15.12) -------------------------------------------------------------------------------
//
// One set for the process: 5 channels at 0x3855B8 (0x5688 each: the entity +0, a codec object +0x5650 released through
// its vtable by 0x101EB0), fed to the one mixer. Voice_Idle 0x101FD0 (their timeouts, the local microphone; at 0x5D46C
// in each player's client half), the channel assignment 0x102620 (edi the speaker) and the data's add 0x102760 (from
// CL_ParseVoiceData 0x31B80, each client's parse) run only in context 0's turn (the one window's and mixer's, as the
// players' sounds: M4 decides per view), in a scope of their own, as the process: the codecs are the process's, never
// tied to a player's lifetime. In another context's turn they are not run (the data was read: the message stays in
// step). Outside the turns as the title's (Host_Shutdown's 0x101EB0 at 0x5DDF5)
inline SharedUi VoiceDecide(bool in_turn, int context) {
    if (!in_turn) return SharedUi::AsTitle;
    return context == 0 ? SharedUi::AsShared : SharedUi::Withheld;
}

// ---- a leaving player's cleanup (docs 15.12) -----------------------------------------------------------------------

// in this order: nothing reaches it any more before anything of its own is released; its objects released by the
// title's own code in its own context, the dependants first; then the rest reported; its context last
enum class Cleanup {
    ServerSlot,      // its server client dropped (SV_DropClient: no packet to it, its entity freed by the server)
    Channels,        // its loopback channels emptied
    Commands,        // its command queue dropped (never run in another context)
    SharedUi,        // the shared front end handed to another player (it never runs in its scope again)
    Plan,            // what of its own is named by anyone else (shared data, another context, another owner's block)
    Entities,        // its client entities (0x2E2E0's block, the array the HUD's objects index)
    ClientObjects,   // the client game's objects 0x552C70 / 0x552C9C (deleting destructors), 0x552CA8 (0x1BAD90, free)
    HudList,         // the HUD element list's nodes (0x5623D0)
    DeltaSlot,       // its delta descriptions' slot (0x45670)
    Residual,        // what it still owns: reported (a leak, with its sites), released only with rollback releasing
    Context,         // its context removed
    Slot,            // its slot empty
};
constexpr int kCleanupSteps = 12;
inline std::array<Cleanup, kCleanupSteps> CleanupOrder() {
    return {Cleanup::ServerSlot, Cleanup::Channels, Cleanup::Commands,      Cleanup::SharedUi,
            Cleanup::Plan,       Cleanup::Entities, Cleanup::ClientObjects, Cleanup::HudList,
            Cleanup::DeltaSlot,  Cleanup::Residual, Cleanup::Context,       Cleanup::Slot};
}

// an object a cleanup step would release: only its own, of the owned kind, named by nobody else
enum class Release { Release, NotOwn, SharedKind, Referenced, None };
inline const char* Name(Release r) {
    switch (r) {
    case Release::Release: return "released";
    case Release::NotOwn: return "kept: not its own";
    case Release::SharedKind: return "kept: a shared kind";
    case Release::Referenced: return "kept: named by another";
    case Release::None: return "none";
    }
    return "?";
}
inline Release ReleaseCheck(bool known, int owner, bool owned_kind, int leaving, bool referenced) {
    if (!known) return Release::None;
    if (owner != leaving) return Release::NotOwn;
    if (!owned_kind) return Release::SharedKind;
    if (referenced) return Release::Referenced;
    return Release::Release;
}

// ---- the cleanup's guest calls (docs 15.16) ------------------------------------------------------------------------
// Each call the cleanup makes into the title, with its contract as the title's own callers make it: the registers it
// reads before writing them (each set to the one object the call is about), the dwords it reads from the stack, the
// bytes its ret pops, and for a call through an object's vtable the vtable its constructor stores (slot 0 the
// function). tests/guest_call_contract_test.py checks every entry against the original instructions (tools/guest_regs.py)
// and that the cleanup makes no guest call except through this table (native_split.cpp CallCleanup, which sets exactly
// these registers and pushes exactly these arguments; CallGuest puts every register and esp back afterwards, so a
// callee's ret N never reaches the caller). The cleanup called 0x1BAD90 with edi set where it reads esi (since 8f52386):
// esi was _Host_Frame's 0 and its first read faulted at 0x414 (the twelfth run, docs 15.16).
enum GuestReg : uint8_t { kRegEax = 1, kRegEcx = 2, kRegEdx = 4, kRegEbx = 8, kRegEsi = 16, kRegEdi = 32 };
enum class CleanupFn { DropClient, Free, Delete552C70, Delete552C9C, Destroy552CA8, FreeDeltaSlot };
struct CleanupCall {
    CleanupFn id;
    const char* name;
    uint32_t fn;      // the function called
    uint8_t regs;     // its register inputs (GuestReg): every one set to the call's object
    uint8_t args;     // the stack arguments pushed (exactly these)
    uint8_t pops;     // what its ret pops (thiscall: 4); CallGuest restores esp whatever it is
    uint32_t vtable;  // nonzero: reached through the object's vtable, which must be this one (its slot 0 fn)
    bool frees;       // it can free (each free checked by NativeSplitHeapFree: never another's block)
    bool shared;      // it changes process-wide state (the server's client slot)
};
inline constexpr CleanupCall kCleanupCalls[] = {
    // cdecl (client, crash, format, ...): the format "%s" and its one argument
    {CleanupFn::DropClient, "SV_DropClient", 0x5C600, 0, 4, 0, 0, true, true},
    // the C runtime's free (cdecl, its SEH frame from 0x22E53C)
    {CleanupFn::Free, "free", 0x22C609, 0, 1, 0, 0, true, false},
    // `mov ecx, [0x552C70]; mov eax, [ecx]; push 1; call [eax]` (0x1A7F70): thiscall, the delete flag
    {CleanupFn::Delete552C70, "0x552C70's deleting destructor", 0x1A7AE0, kRegEcx, 1, 4, 0x35EB78, true, false},
    // `mov ecx, [0x552C9C]; mov eax, [ecx]; push 1; call [eax]` (0x1B6670)
    {CleanupFn::Delete552C9C, "0x552C9C's deleting destructor", 0x1B6980, kRegEcx, 1, 4, 0x361C8C, true, false},
    // `mov esi, [0x552CA8]; mov edi, esi; call 0x1BAD90` (0x1BAE4E, 0x1A6CF9): esi the object (edi only the callers'
    // copy for their free after it: callee-saved, never read by 0x1BAD90)
    {CleanupFn::Destroy552CA8, "0x552CA8's destructor", 0x1BAD90, kRegEsi, 0, 0, 0, true, false},
    // esi the slot (0x2FBAD)
    {CleanupFn::FreeDeltaSlot, "a delta slot's free", 0x45670, kRegEsi, 0, 0, 0, true, false},
};
constexpr size_t kCleanupCallCount = sizeof kCleanupCalls / sizeof kCleanupCalls[0];
inline const CleanupCall& Contract(CleanupFn id) { return kCleanupCalls[size_t(id)]; }
// a call made with the registers `set` and `nargs` stack arguments meets its contract: every input register set, the
// arguments exactly the contract's
inline bool MeetsContract(const CleanupCall& c, uint8_t set, size_t nargs) {
    return (set & c.regs) == c.regs && nargs == c.args;
}
// a vtable call is made only when the object's vtable is its constructor's (an object overwritten or of another class
// is kept, not called: its target is not known)
inline bool VtableMatches(const CleanupCall& c, uint32_t object_vtable) { return !c.vtable || object_vtable == c.vtable; }

// 0x552C70's destructor (0x1A7B10 -> 0x1A78B0) deletes the eight objects at +0x1C..+0x38 through their own vtables:
// it runs only when each is absent or released by the same rule as the object itself (else the whole object is kept)
constexpr uint32_t k552C70Children = 0x1C, k552C70ChildCount = 8;
inline bool ChildrenReleasable(const Release* each, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (each[i] != Release::Release && each[i] != Release::None) return false;
    return true;
}

// what leaving players leave behind (kept or unreleased) is bounded: past the limit no player joins again this session
// (each join and leave would otherwise add its residue without end)
struct LeakBudget {
    size_t limit = size_t(8) << 20;  // bytes
    size_t left_behind = 0;
    int leaves = 0;
    void Left(size_t bytes) {
        left_behind += bytes;
        leaves++;
    }
    bool MayJoin() const { return left_behind <= limit; }
};

}  // namespace nsplit
