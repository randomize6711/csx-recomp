// Native split-screen's M2 lifecycle without the game (docs/native_splitscreen.md 15.12): which console buffer a line
// goes to and where it runs, the front end's commands, the shared voice channels' owner, a leaving player's cleanup
// order and what of its own it may release, and the bound on what leaving players leave behind. Tested on its own by
// tests/native_split_lifecycle_test.py; src/game/native_split.cpp drives it from the game.
#pragma once

#include <array>
#include <map>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "game/heap_owner.h"
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
enum class CleanupFn { DropClient, Free, Delete552C70, Delete552C9C, Destroy552CA8, FreeDeltaSlot, DetachChild };
struct CleanupCall {
    CleanupFn id;
    const char* name;
    uint32_t fn;      // the function called
    uint8_t regs;     // its register inputs (GuestReg): every one set to the call's object
    uint8_t args;     // the stack arguments pushed (exactly these)
    uint8_t pops;     // what its ret pops (thiscall: 4); CallGuest restores esp whatever it is
    uint32_t vtable;  // nonzero: reached through the object's vtable, which must be this one (its slot 0 fn)
    int16_t slot;     // >= 0: reached through the object's vtable at this offset, whatever its class: that entry must
                      // be fn (the title's `call [eax+slot]`); -1 not
    bool frees;       // it can free (each free checked by NativeSplitHeapFree: never another's block)
    bool shared;      // it changes process-wide state (the server's client slot)
};
inline constexpr CleanupCall kCleanupCalls[] = {
    // cdecl (client, crash, format, ...): the format "%s" and its one argument
    {CleanupFn::DropClient, "SV_DropClient", 0x5C600, 0, 4, 0, 0, -1, true, true},
    // the C runtime's free (cdecl, its SEH frame from 0x22E53C)
    {CleanupFn::Free, "free", 0x22C609, 0, 1, 0, 0, -1, true, false},
    // `mov ecx, [0x552C70]; mov eax, [ecx]; push 1; call [eax]` (0x1A7F70): thiscall, the delete flag
    {CleanupFn::Delete552C70, "0x552C70's deleting destructor", 0x1A7AE0, kRegEcx, 1, 4, 0x35EB78, -1, true, false},
    // `mov ecx, [0x552C9C]; mov eax, [ecx]; push 1; call [eax]` (0x1B6670)
    {CleanupFn::Delete552C9C, "0x552C9C's deleting destructor", 0x1B6980, kRegEcx, 1, 4, 0x361C8C, -1, true, false},
    // `mov esi, [0x552CA8]; mov edi, esi; call 0x1BAD90` (0x1BAE4E, 0x1A6CF9): esi the object (edi only the callers'
    // copy for their free after it: callee-saved, never read by 0x1BAD90)
    {CleanupFn::Destroy552CA8, "0x552CA8's destructor", 0x1BAD90, kRegEsi, 0, 0, 0, -1, true, false},
    // esi the slot (0x2FBAD)
    {CleanupFn::FreeDeltaSlot, "a delta slot's free", 0x45670, kRegEsi, 0, 0, 0, -1, true, false},
    // a VGUI panel's removeChild (docs 15.17): `mov ecx, [child+0x38]; mov eax, [ecx]; push child; call [eax+0x4C]`
    // (0x10ACCD setParent, 0x10AD14 addChild, 0x10AD96 insertChildAt): thiscall, the parent; the child; ret 4. It
    // clears the child's parent (+0x38) and its inherited +0x3C and takes it out of the parent's children (0xB51B0:
    // the entries after it moved down, the count less one); it allocates and frees nothing. Slot 0x4C of every panel
    // vtable (the classes the title builds on Panel 0x10A4E0)
    {CleanupFn::DetachChild, "a panel's removeChild", 0x10ADC0, kRegEcx, 1, 4, 0, 0x4C, false, true},
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
// and a call through a vtable slot only when the object's vtable has the contract's function there
inline bool SlotMatches(const CleanupCall& c, uint32_t slot_entry) { return c.slot < 0 || slot_entry == c.fn; }

// 0x552C70's destructor (0x1A7B10 -> 0x1A78B0) deletes the eight objects at +0x1C..+0x38 through their own vtables:
// it runs only when each is absent or released by the same rule as the object itself (else the whole object is kept)
constexpr uint32_t k552C70Children = 0x1C, k552C70ChildCount = 8;
inline bool ChildrenReleasable(const Release* each, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (each[i] != Release::Release && each[i] != Release::None) return false;
    return true;
}

// ---- the VGUI hierarchy's shared parents (docs 15.17) --------------------------------------------------------------
// A VGUI panel (Panel 0x10A4E0; no virtual destructor: the title never deletes one) keeps its children in a vector at
// +0x2C {count, capacity, data} (four reserved at its construction: a 16-byte buffer from 0xB52DA) and its parent at
// +0x38; setParent (slot 0x40, 0x10ACC0) takes it from its old parent (slot 0x4C, removeChild 0x10ADC0) and appends it
// to the new parent's vector (0xB5220, unique; grown by 0xB52B0). Each player's VGui_Startup (0x1BAE90, from
// 0x18B7E0 in its HUD's video initialisation) makes its own viewport (0xA84 bytes, 0x1B03B0, vtable 0x35FD10, kept at
// 0x552B7C in its regions) and sets its parent to the engine's root panel (the client's engine function [0x54321C]):
// one panel the process made at its start, so every local player's viewport is a child of the one process-owned panel.
constexpr uint32_t kPanelChildren = 0x2C, kPanelParent = 0x38, kPanelSetParentSlot = 0x40, kPanelRemoveChildSlot = 0x4C;
constexpr uint32_t kPanelSetParent = 0x10ACC0, kPanelRemoveChild = 0x10ADC0, kViewport = 0x552B7C;
constexpr uint32_t kViewportVtable = 0x35FD10, kVectorGrow = 0xB52B0;
// a panel: its vtable has the Panel's setParent and removeChild (every class the title builds on 0x10A4E0 keeps both)
inline bool IsPanelVtable(uint32_t set_parent_entry, uint32_t remove_child_entry) {
    return set_parent_entry == kPanelSetParent && remove_child_entry == kPanelRemoveChild;
}

// 0xB52B0 (ebx a vector's header {count, capacity, data}, eax the count wanted; it allocates the larger buffer and
// frees the old one): whose its new buffer is. A vector's buffer is its header's owner's: a header in a process block,
// or in the image outside the players' regions (a process-wide static), is the process's, so a player's turn grows it
// as the process (its old buffer the process's, freed by the process); a header in a player's block or regions, on
// the stack, or in no known block, stays the acting owner's (unchanged). No allocation when it does not grow.
enum class GrowAs { Acting, Process };
// A header in a block a departed player left behind (handed to the process: left_by) is not the process's own vector
// (docs 15.18): nobody should grow it, so it is left to the acting owner, whose free of the old buffer the guard then
// refuses and reports (never a left-behind buffer freed in silence as the process's).
inline GrowAs VectorGrowOwner(int32_t wanted, int32_t capacity, bool in_block, int block_owner, bool in_image,
                              bool in_regions, int acting, bool left_behind = false) {
    if (wanted <= capacity || acting < 0) return GrowAs::Acting;  // no grow, or the process already
    if (in_block) return block_owner == heapown::kProcess && !left_behind ? GrowAs::Process : GrowAs::Acting;
    return in_image && !in_regions ? GrowAs::Process : GrowAs::Acting;
}

// a leaving player's viewport taken off its parent (removeChild, as the title's setParent does) when: it is its own
// block, a panel (its vtable has the Panel's methods), its parent is a panel that is not its own (the root: the
// process's), the parent's vector is sane and lists it exactly once. Never deleted: the title has no destructor for
// it (its block is left behind, the leak reported); a player that keeps its context (player 1's departure) keeps it.
enum class Detach { None, NotOwn, NotPanel, NoParent, OwnParent, ParentNotPanel, BadList, NotListed, Duplicate, Detach };
inline const char* Name(Detach d) {
    switch (d) {
        case Detach::None: return "no viewport";
        case Detach::NotOwn: return "not its own block";
        case Detach::NotPanel: return "not a panel";
        case Detach::NoParent: return "no parent";
        case Detach::OwnParent: return "its parent is its own";
        case Detach::ParentNotPanel: return "its parent is not a panel";
        case Detach::BadList: return "its parent's children are not a sane vector";
        case Detach::NotListed: return "not among its parent's children";
        case Detach::Duplicate: return "listed more than once";
        case Detach::Detach: return "detached";
    }
    return "?";
}
struct PanelView {  // what the cleanup read: the viewport's and its parent's words
    bool known = false;  // the viewport is a known block
    int owner = -2;      // its block's owner
    bool panel = false;  // its vtable has the Panel's methods
    uint32_t parent = 0;
    int parent_owner = -2;  // the parent's block's owner (-2: no known block)
    bool parent_panel = false;
    int32_t count = 0, capacity = 0;
    bool data_ok = false;  // the children's buffer is a known block holding capacity entries
    int listed = 0;        // how many times the viewport is in it
};
inline Detach DetachCheck(uint32_t viewport, const PanelView& v, int leaving) {
    if (!viewport) return Detach::None;
    if (!v.known || v.owner != leaving) return Detach::NotOwn;
    if (!v.panel) return Detach::NotPanel;
    if (!v.parent) return Detach::NoParent;
    if (v.parent_owner == leaving) return Detach::OwnParent;
    if (!v.parent_panel) return Detach::ParentNotPanel;
    if (v.count < 0 || v.capacity < v.count || v.capacity > 4096 || !v.data_ok) return Detach::BadList;
    if (v.listed == 0) return Detach::NotListed;
    if (v.listed > 1) return Detach::Duplicate;
    return Detach::Detach;
}

// a shared parent's children (read live) a departed player left (its block handed to the process: left_by) or that
// is no known block any more (freed): stale (docs 15.17); with the entries duplicated
struct ChildFinding {
    uint32_t child;
    int left_by;  // >= 0 a departed player's; -2 no known block
    bool duplicate;
};
template <typename Read>
std::vector<ChildFinding> StaleChildren(uint32_t parent, const heapown::Blocks& blocks, Read read) {
    std::vector<ChildFinding> out;
    int32_t n = int32_t(read(parent + kPanelChildren)), cap = int32_t(read(parent + kPanelChildren + 4));
    uint32_t data = read(parent + kPanelChildren + 8);
    if (n < 0 || n > cap || cap > 4096 || (n && !data)) return out;
    std::set<uint32_t> seen;
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = read(data + 4 * uint32_t(i));
        bool dup = !seen.insert(c).second;
        uint32_t start = c >= 0x10000 ? blocks.Holding(c) : 0;  // a panel may be a member of a larger object
        const heapown::Block* b = start ? blocks.At(start) : nullptr;
        if (!b && !heapown::IsImageAddress(c)) out.push_back({c, -2, dup});
        else if (b && b->left_by >= 0) out.push_back({c, b->left_by, dup});
        else if (dup) out.push_back({c, -1, true});
    }
    return out;
}

// what leaving players leave behind (kept or unreleased) is bounded: past the limit no player joins again this session
// (each join and leave would otherwise add its residue without end)
struct LeakBudget {
    size_t limit = size_t(8) << 20;  // bytes
    size_t left_behind = 0;          // historical: everything the leaves left, summed at each leave
    size_t outstanding = 0;          // docs 15.20: of that, still allocated now (heap_owner.h LeftBehindBytes)
    int leaves = 0;
    // a leave that left `bytes`; `now`: the blocks every leave left still allocated after it (the heap's count)
    void Left(size_t bytes, size_t now) {
        left_behind += bytes;
        leaves++;
        outstanding = now;
    }
    // without a heap to ask (the simulations): nothing left is ever freed again
    void Left(size_t bytes) { Left(bytes, outstanding + bytes); }
    // a failed initialisation's leftovers (not a leave: no player had joined): in the history and the count too
    void Abandoned(size_t bytes, size_t now) {
        left_behind += bytes;
        outstanding = now;
    }
    // the heap's count between leaves (the game freeing what a leave left)
    void Observe(size_t now) { outstanding = now; }
    // the bound is on what leaving players still hold (docs 15.20); never on less than the heap's own count
    bool MayJoin() const { return outstanding <= limit; }
    // the outstanding count above the historical sum: the two disagree (an accounting fault): reported
    bool Consistent() const { return outstanding <= left_behind; }
    // released by the game since (the historical sum less what is outstanding)
    size_t ReleasedSince() const { return Consistent() ? left_behind - outstanding : 0; }
    // how many more leaves at the average still held so far keep joins open (outstanding stays within the limit): a
    // projection from the leaves so far, not a guaranteed outcome; -1 with nothing to average yet
    long Projected() const {
        if (!leaves || !outstanding) return -1;
        if (outstanding > limit) return 0;
        size_t avg = outstanding / size_t(leaves);
        return avg ? long((limit - outstanding) / avg) : -1;
    }
};

// ---- retention, focus and the cleanup's destructors (docs 15.18) ---------------------------------------------------

// The VGUI App (one static object of the process: 0x384B58, built by the static initialiser 0x23CBF0 with App's
// constructor 0x109530, which stores it at 0x552740; vtable 0x33A4A0). Its focus fields hold panels: +0x14 the key
// focus, +0x18 the previous mouse focus, +0x1C the mouse focus, +0x20 the mouse capture, +0x24 the wanted key focus.
// App's tick (0x109EA0, slot 0xD8) sets the mouse focus each tick to the panel under the cursor (0x109B40 ->
// setMouseFocus 0x10A400, which calls the old focus's slot 0x1D0 and the new one's slot 0x1CC); removeSurfaceBase
// (0x109B20) zeroes +0x1C, +0x20 and +0x14 directly, the constructor zeroes all five. A leaving player's panel left
// there (run 14: its viewport at +0x1C, 0x384B74) would have that player's code called at the next tick, in whatever
// context runs it: cleared to 0 as the title's own removal does, without calling it.
constexpr uint32_t kApp = 0x384B58, kAppInstance = 0x552740, kAppVtable = 0x33A4A0;
struct FocusField {
    uint32_t offset;
    const char* name;
};
inline constexpr FocusField kAppFocus[] = {{0x14, "key focus"},
                                           {0x18, "previous mouse focus"},
                                           {0x1C, "mouse focus"},
                                           {0x20, "mouse capture"},
                                           {0x24, "wanted key focus"}};
// App's tick signals {count +0x1D0, capacity, data +0x1D8} (each called every tick: 0x109F7C) and its surfaces (+0x8):
// reported when a leaving player's object is among them (no verified removal: kept)
constexpr uint32_t kAppTickSignals = 0x1D0, kAppSurfaces = 0x08;
// a focus field cleared at a leave: it names a panel of the leaving player (one of players 2..4: player 1's departure
// keeps its context and its panels)
inline bool ClearFocus(bool panel, int value_owner, int leaving) { return leaving >= 1 && panel && value_owner == leaving; }

// what a call of a process-owned panel's setParent / addChild does to its children (classified at its entry; both
// take the child off its old parent first, then append it once: 0xB5220 / the inline loop at 0x10AD1D)
enum class Link { Nothing, New, Reattach, Change, Detach, Duplicate, Cycle };
inline constexpr int kLinks = 7;
inline const char* Name(Link l) {
    switch (l) {
        case Link::Cycle: return "a temporary remove / re-add (no net change)";
        case Link::Nothing: return "nothing";
        case Link::New: return "a new attachment";
        case Link::Reattach: return "a re-attachment (already its child: no change)";
        case Link::Change: return "a parent change";
        case Link::Detach: return "a detachment";
        case Link::Duplicate: return "a duplicate attachment attempt";
    }
    return "?";
}
// old: the child's parent (+0x38) before the call; parent: the new one (0: setParent(0)); listed: how many times the
// child is among the new parent's children before the call
inline Link ClassifyLink(uint32_t old, uint32_t parent, int listed) {
    if (!parent) return old ? Link::Detach : Link::Nothing;
    if (listed > 1 || (listed == 1 && old != parent)) return Link::Duplicate;
    if (old == parent) return Link::Reattach;
    return old ? Link::Change : Link::New;
}
// removeChild 0x10ADC0's own callers inside setParent (0x10ACCD), addChild (0x10AD14), insertChildAt (0x10AD96):
// part of a link, not a detachment of its own (their return addresses)
inline constexpr uint32_t kLinkRemoveReturns[] = {0x10ACD0, 0x10AD17, 0x10AD99};
inline bool PartOfLink(uint32_t ret) {
    for (uint32_t r : kLinkRemoveReturns)
        if (r == ret) return true;
    return false;
}

// docs 15.20: the title's own temporary removals. 0x108BB0 moves a child to the end of its parent's children (to the
// front): removeChild through the parent's slot 0x4C (0x108BC2, return 0x108BC5), then addChild of the same child
// ([esi+0x1C] both times) through slot 0x44 (0x108BD3, return 0x108BD6). Every standalone removal from a process panel
// is pending until the frame's end: the same child added to the same parent before then completes a temporary
// remove / re-add (a cycle, no structural change: counted, not logged); to another parent, a parent change; still
// pending at the frame's end, a permanent detachment. The cleanup's own detach is permanent at once (counted apart)
inline constexpr uint32_t kReorderRemoveReturn = 0x108BC5, kReorderAddReturn = 0x108BD6;
class LinkTracker {
public:
    // a standalone removal (not part of a setParent / addChild / insertChildAt) of child from a process panel
    void Removed(uint32_t parent, uint32_t child, uint32_t ret) { pending_[child] = {parent, ret}; }
    // an attachment of child to parent (a process panel), `base` as ClassifyLink saw it at the call: a pending removal
    // of the child completes a cycle (same parent) or a parent change (another); otherwise `base`
    Link Attached(uint32_t parent, uint32_t child, Link base, uint32_t ret) {
        auto it = pending_.find(child);
        if (it == pending_.end()) return base;
        bool same = it->second.parent == parent;
        bool reorder = same && it->second.ret == kReorderRemoveReturn && ret == kReorderAddReturn;
        pending_.erase(it);
        if (!same) return Link::Change;
        cycles_++;
        reorders_ += reorder;
        return Link::Cycle;
    }
    // the frame's end: what is still pending was a permanent detachment; how many
    size_t Settle() {
        size_t n = pending_.size();
        permanent_ += n;
        pending_.clear();
        return n;
    }
    // the removals still pending (in this frame), with their parent and return
    template <class F> void ForEachPending(F f) const {
        for (const auto& [child, p] : pending_) f(p.parent, child, p.ret);
    }
    size_t Pending() const { return pending_.size(); }
    uint64_t Cycles() const { return cycles_; }
    uint64_t Reorders() const { return reorders_; }
    uint64_t Permanent() const { return permanent_; }

private:
    struct P {
        uint32_t parent, ret;
    };
    std::map<uint32_t, P> pending_;
    uint64_t cycles_ = 0, reorders_ = 0, permanent_ = 0;
};

// a retention root's evidence: verified (a field whose meaning the title's code establishes), unverified (a word inside
// bytes no code reads as a pointer: a file's or a message's or a texture's bytes, or a block's never written tail), or
// unclassified (not established either way). Every kind stays conservatively kept: the evidence only explains it.
enum class Evidence { Verified, Unverified, Unclassified };
inline const char* Name(Evidence e) {
    switch (e) {
        case Evidence::Verified: return "verified";
        case Evidence::Unverified: return "unverified pointer-like bytes";
        case Evidence::Unclassified: return "unclassified";
    }
    return "?";
}
struct StaticArea {
    uint32_t lo, hi;
    Evidence evidence;
    const char* what;
};
inline constexpr StaticArea kStaticAreas[] = {
    {kApp + 0x14, kApp + 0x28, Evidence::Verified, "the VGUI App's focus fields"},
    {0x5622B0, 0x5622B4, Evidence::Verified, "the string-intern pool's search key"},
    // 0xBEB70: 64 x 64 16-bit texels written from 0x5D14C8 (add ecx, 2), then handed to the texture's creation
    // (0x2D0D01); no other instruction names the area
    {0x5D14C8, 0x5D34C8, Evidence::Unverified, "a 64x64 16-bit texture's texels (0xBEB70)"},
    // 0x9FEF0: the sizebuf 0x62DBC8 {data 0x62DBD4, maxsize 0x8000, cursize}: message bytes
    {0x62DBD4, 0x635BD4, Evidence::Unverified, "the 32 KB message buffer's bytes (sizebuf 0x62DBC8)"},
};
inline const StaticArea* StaticAreaOf(uint32_t va) {
    for (const StaticArea& a : kStaticAreas)
        if (va >= a.lo && va < a.hi) return &a;
    return nullptr;
}
// blocks whose words are bytes no code reads as a pointer (by their allocation site)
struct ByteSite {
    uint32_t site;
    const char* what;
};
inline constexpr ByteSite kByteSites[] = {
    // 0x108230: CreateFile, GetFileSize, the size rounded up to 2 KB (lea edi, [eax+0x7FF]; shr 11; shl 11), malloc,
    // ReadFile: a whole file's bytes, the tail past the file never written (the heap's earlier contents)
    {0x10829B, "a whole file read by 0x108230 (its size rounded up to 2 KB: the tail never written)"},
};
inline const ByteSite* ByteSiteOf(uint32_t site) {
    for (const ByteSite& b : kByteSites)
        if (b.site == site) return &b;
    return nullptr;
}
// where a root word is
enum class RootKind { StaticWord, RegionWord, ProcessBlock, PlayerBlock, LeftBehindBlock, SharedKind };
inline const char* Name(RootKind r) {
    switch (r) {
        case RootKind::StaticWord: return "a word of the process's static data";
        case RootKind::RegionWord: return "a word of an active player's regions";
        case RootKind::ProcessBlock: return "a live process block";
        case RootKind::PlayerBlock: return "another active player's block";
        case RootKind::LeftBehindBlock: return "a block an earlier departed player left behind";
        case RootKind::SharedKind: return "a shared kind";
    }
    return "?";
}
inline Evidence EvidenceOf(RootKind r, uint32_t at, uint32_t site) {
    if (r == RootKind::SharedKind) return Evidence::Verified;
    if (r == RootKind::StaticWord) {
        const StaticArea* a = StaticAreaOf(at);
        return a ? a->evidence : Evidence::Unclassified;
    }
    if ((r == RootKind::ProcessBlock || r == RootKind::PlayerBlock || r == RootKind::LeftBehindBlock) && ByteSiteOf(site))
        return Evidence::Unverified;
    return Evidence::Unclassified;
}

// a player's allocations the title itself keeps for its whole life (no destructor, or none that may run): left
// behind at every leave by design, reported as such (their sites: the allocating call's return address)
struct PersistentSite {
    uint32_t site;
    const char* what;
};
inline constexpr PersistentSite kPersistentSites[] = {
    // CMiniMem 0x1F9B90 (from its Instance 0x1F9EC0: 30000 >> 4 entries of [0x552CE0] + 0x14 bytes; run 14: 637500 =
    // 1875 x 340). Its destructor 0x1F9D10 (only through the vtable 0x365854, never called by the title) runs each
    // entry's destructor 0x1F9B30, which frees the entry's +4: a pointer into this same block (0x1F9C25)
    {0x1F9BBE, "the particle manager's memory pool (never destroyed: its destructor frees pointers into the pool)"},
    {0x1F9EE6, "the particle manager's CMiniMem object (its singleton 0x552CE4, made on first use, never deleted)"},
    {0x1BAF1A, "a viewport (VGui_Startup's: no destructor; detached, never deleted)"},
};
inline const PersistentSite* PersistentSiteOf(uint32_t site) {
    for (const PersistentSite& p : kPersistentSites)
        if (p.site == site) return &p;
    return nullptr;
}

// a leaving player's block after its cleanup, counted once, in this order: freed by the cleanup, kept by the original
// for its whole life, reachable from a verified root, kept conservatively (only unverified or unclassified roots),
// named by nobody (no release mechanism: no destructor the cleanup may run frees it)
enum class Retained { Released, Persistent, Verified, Conservative, Unreferenced };
inline const char* Name(Retained r) {
    switch (r) {
        case Retained::Released: return "released";
        case Retained::Persistent: return "persistent by the original";
        case Retained::Verified: return "reachable from verified roots";
        case Retained::Conservative: return "kept conservatively";
        case Retained::Unreferenced: return "named by nobody (no release mechanism)";
    }
    return "?";
}
inline Retained Categorize(bool persistent, bool kept, bool verified_reach) {
    if (persistent) return Retained::Persistent;
    if (!kept) return Retained::Unreferenced;
    return verified_reach ? Retained::Verified : Retained::Conservative;
}

// a cleanup destructor's outcome (logged each time): not considered (no object), considered and skipped (the reason),
// invoked and returned (its object as its contract leaves it), invoked and failed (it returned, its object not as the
// contract leaves it: a deleting destructor's object still allocated, a destructor's own object gone)
enum class Step { NotConsidered, Skipped, Returned, Failed };
inline const char* Name(Step s) {
    switch (s) {
        case Step::NotConsidered: return "not considered";
        case Step::Skipped: return "considered, skipped";
        case Step::Returned: return "invoked and returned";
        case Step::Failed: return "invoked and failed";
    }
    return "?";
}
enum class Post { Deletes, Keeps, NotABlock };  // what the call does to its object's block
inline Step AfterCall(bool called, Post post, bool object_after) {
    if (!called) return Step::Skipped;
    if (post == Post::Deletes) return object_after ? Step::Failed : Step::Returned;
    if (post == Post::Keeps) return object_after ? Step::Returned : Step::Failed;
    return Step::Returned;
}

}  // namespace nsplit
