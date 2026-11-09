// Native split-screen's shared-once operations (docs/native_splitscreen.md 15): the parts of a client's connect that
// build process-wide or world-wide state (the renderer's world, a UI bundle, the client game's material list) run once
// for a map generation, by whichever local player gets there first, as the process (their allocations the process's,
// not that player's); every other player of the same generation does only its own part. Without the game: tested by
// tests/native_split_test.cpp; src/game/native_split.cpp drives it from the hooks.
//
// The state is the process's, never a player's: a player leaving changes nothing here, and the world's end (the
// built-in server shut down, its hunk and models freed) forgets every operation, so the next world builds them again.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace sharedonce {

enum class Op {
    WorldRenderer,  // R_NewMap's shared part (0x4F8D0): particles, decals, lightmaps, skies, the world's textures
    SignonUi,       // the signon's UI bundle unload (0xC4770 "PieMenuUI" at 0x2D3EF)
    Materials,      // the client game's material list (0x1DC0C0 at 0x1AE5FA; one list, 0x57F090)
    kCount,
};

inline const char* Name(Op op) {
    switch (op) {
    case Op::WorldRenderer: return "the renderer's world (R_NewMap)";
    case Op::SignonUi: return "the signon's UI bundle unload";
    case Op::Materials: return "the material list";
    case Op::kCount: break;
    }
    return "?";
}

// a map generation: the server's spawn count as the client got it (cl.servercount), the world model, and the map's
// name (the material list's key, as the client game's own: its files are per map). Two generations are the same
// world only when all three are, and none is empty
struct Generation {
    uint32_t spawncount = 0;
    uint32_t world = 0;
    std::string name;
    bool Valid() const { return world != 0 || !name.empty(); }
    bool operator==(const Generation& o) const {
        return spawncount == o.spawncount && world == o.world && name == o.name;
    }
};

// what a call of a shared-once operation does
enum class Action {
    Run,            // the first for this generation: run it, as the process; then Finished
    PerPlayerOnly,  // already done for this generation: only the caller's own part
    Inner,          // the operation itself, called by its own Run (the hook's call of the original): run as it is
    Title,          // no generation to key (no world, no name): run as the title does, nothing recorded
};

inline const char* Name(Action a) {
    switch (a) {
    case Action::Run: return "run";
    case Action::PerPlayerOnly: return "per player only";
    case Action::Inner: return "inner";
    case Action::Title: return "as the title";
    }
    return "?";
}

class World {
public:
    // a player's call of `op` for generation `g`
    Action Begin(Op op, const Generation& g) {
        Entry& e = at(op);
        if (e.running) return Action::Inner;
        if (!g.Valid()) {
            e.title++;
            return Action::Title;
        }
        if (e.done && e.gen == g) {
            e.skipped++;
            return Action::PerPlayerOnly;
        }
        e.running = true;
        e.pending = g;
        return Action::Run;
    }
    // the Run ended: ok records it for its generation; a failure (or a run cut short) records nothing, so the next
    // caller of the generation runs it again (never a half-built world taken as built)
    void Finished(Op op, bool ok) {
        Entry& e = at(op);
        if (!e.running) return;
        e.running = false;
        if (ok) {
            e.done = true;
            e.gen = e.pending;
            e.runs++;
        } else {
            e.done = false;
            e.failed++;
        }
    }
    // the world's end (the server shut down, its hunk freed): every operation is built again for the next world
    void Invalidate() {
        for (Entry& e : ops_) {
            if (e.done) e.invalidated++;
            e.done = false;
            e.gen = {};
        }
    }
    // a Run in progress (its allocations and frees are the process's)
    bool Running() const {
        for (const Entry& e : ops_)
            if (e.running) return true;
        return false;
    }
    bool Done(Op op, const Generation& g) const { return at(op).done && at(op).gen == g; }
    uint64_t Runs(Op op) const { return at(op).runs; }
    uint64_t Skipped(Op op) const { return at(op).skipped; }
    uint64_t Failed(Op op) const { return at(op).failed; }
    uint64_t TitleRuns(Op op) const { return at(op).title; }

private:
    struct Entry {
        bool done = false, running = false;
        Generation gen, pending;
        uint64_t runs = 0, skipped = 0, failed = 0, title = 0, invalidated = 0;
    };
    Entry& at(Op op) { return ops_[size_t(op)]; }
    const Entry& at(Op op) const { return ops_[size_t(op)]; }
    std::array<Entry, size_t(Op::kCount)> ops_{};
};

}  // namespace sharedonce
