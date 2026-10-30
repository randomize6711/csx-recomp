// The legends' buttons as pc_prompts.cpp draws them, for the mouse's clicks on the help bar (pc_mouse.cpp).
#pragma once
#include <string>
#include <vector>

struct PcLegendSlot {
    float x0, x1;  // the bar's 640 x 480 coordinates
    int button;    // the pad's analog button index (A B X Y Black White)
};

// a legend strip's buttons (its name: Cancel_Accept_Reset ...), rightmost first; none when its words are not known
std::vector<PcLegendSlot> PcLegendSlots(const std::string& strip);
