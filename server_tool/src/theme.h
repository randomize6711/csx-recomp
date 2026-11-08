// The classic olive-green look of 2003-2006 era Steam tools (an independent application, not Steam): dark olive
// window, grey-green panels, square corners, one-pixel bevels (light top-left, dark bottom-right; reversed when
// pressed), gold for the selection and headings, green console text on near black.
#pragma once

#include "imgui.h"

namespace theme {

// the palette
inline const ImVec4 kWindow = {0.290f, 0.337f, 0.259f, 1.0f};   // #4A5642
inline const ImVec4 kPanel = {0.243f, 0.275f, 0.212f, 1.0f};    // #3E4636
inline const ImVec4 kPanelDark = {0.204f, 0.231f, 0.176f, 1.0f};
inline const ImVec4 kLight = {0.541f, 0.573f, 0.506f, 1.0f};    // #8A9281 bevel highlight
inline const ImVec4 kDark = {0.137f, 0.157f, 0.114f, 1.0f};     // #23281D bevel shadow
inline const ImVec4 kText = {0.871f, 0.894f, 0.839f, 1.0f};     // #DEE4D6
inline const ImVec4 kTextDim = {0.627f, 0.667f, 0.580f, 1.0f};
inline const ImVec4 kGold = {0.769f, 0.710f, 0.314f, 1.0f};     // #C4B550 headings, selected text
inline const ImVec4 kSelect = {0.588f, 0.537f, 0.184f, 1.0f};   // #96892F selection bar
inline const ImVec4 kConsoleBg = {0.071f, 0.082f, 0.059f, 1.0f};
inline const ImVec4 kConsoleText = {0.659f, 0.812f, 0.537f, 1.0f};  // muted green
inline const ImVec4 kWarn = {0.918f, 0.780f, 0.384f, 1.0f};
inline const ImVec4 kError = {0.957f, 0.471f, 0.412f, 1.0f};
inline const ImVec4 kOnline = {0.494f, 0.835f, 0.353f, 1.0f};
inline const ImVec4 kToolText = {0.553f, 0.741f, 0.851f, 1.0f};  // the tool's own console lines

void Apply(float scale);

// a raised (or sunken) one-pixel bevel around a rectangle
void Bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sunken);
void HLine(ImDrawList* dl, float x0, float x1, float y, ImU32 c);  // one pixel high

// a framed panel with a title strip; EndPanel closes it
bool BeginPanel(const char* title, ImVec2 size, bool scroll = false);
void EndPanel();

// a button with the classic raised / pressed bevel; disabled draws it greyed and never returns true
bool Button(const char* label, ImVec2 size = {0, 0}, bool enabled = true);

// a small square lamp in a state's colour, then its text
void Lamp(const ImVec4& color, const char* text);

// a label: value row in two columns of a table that is already begun
void Row(const char* label, const char* fmt, ...);

// the tab strip's classic look is ImGui's tab bar with the theme's colours; a heading in gold
void Heading(const char* text);

// text shown in the description box when the previous item is hovered
void Describe(const char* text);
const char* TakeDescription();  // the frame's description (empty when none), reset each frame by NewFrame()
void NewFrame();

}  // namespace theme
