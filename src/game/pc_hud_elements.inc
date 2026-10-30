// HUD elements (src/game/pc_hud_layout.cpp, docs/n10_hud.md), mapped from a CSX_LOG_HUDDRAW=1 run
// (tests/scenarios/diag_hud_draws.txt: every caller of the engine's HUD drawing functions with its place) and the
// client's code: each element's Draw (a vtable entry, hooked in config/game.toml as PcHudEnterN in this order) and
// the code that draws it directly. The number helpers (kShared) draw for the element whose Draw ran last.
#ifdef CSX_HUD_ELEMENTS
constexpr Element kElements[] = {
    {"health", "Health", 0x1D3E30, 0x1D3E30, 0x1D4060},     // the cross at 68,412, its digits through the helpers
    {"armor", "Armor", 0x1D6470, 0x1D6470, 0x1D6720},       // the kevlar icon at 164,412
    {"timer", "Round Timer", 0x1D4B70, 0x1D4B70, 0x1D4DE0}, // the stopwatch at 264,412, the digits at 308..353
    {"money", "Money", 0x1ADDC0, 0x1ADDC0, 0x1AE160},       // the dollar sign and amount at 466,324 (and its change)
    {"ammo", "Ammunition", 0x1ACC70, 0x1ACC70, 0x1AD200},   // bottom right, 558,409
    {"status", "Status Icons", 0x1D7640, 0x1D7640, 0x1D7720}, // the buy zone and others at 68,208
    {"killfeed", "Kill Feed", 0x1D6830, 0x1D6830, 0x1D6BD0},  // names and the weapon at the top right, 408..497,91
    // drawn from another element's Draw: their own code only
    {"radar", "Radar", 0, 0x1D5170, 0x1D5A10},             // its backdrop at 32,24 (0x1D5310, called by health's Draw)
                                                            // and its dots (0x1D5170)
    {"history", "Pickups", 0, 0x1DB570, 0x1DB8B0},          // weapons and ammunition picked up (from ammo's Draw)
};
constexpr Range kShared[] = {{0x1AEC60, 0x1AECD0}, {0x1AECD0, 0x1AEDB0}, {0x1AEDB0, 0x1AF0A0}};
#endif
