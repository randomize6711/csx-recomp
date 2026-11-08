#include "theme.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "imgui_internal.h"

namespace theme {

namespace {
ImU32 U(const ImVec4& c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a}); }
std::string g_description, g_last_description;
}  // namespace

void Apply(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = s.ScrollbarRounding = s.GrabRounding =
        s.TabRounding = 0.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBorderSize = 1.0f;
    s.WindowPadding = {8, 8};
    s.FramePadding = {6, 3};
    s.ItemSpacing = {6, 5};
    s.ItemInnerSpacing = {5, 4};
    s.IndentSpacing = 16;
    s.ScrollbarSize = 16;
    s.GrabMinSize = 12;
    s.CellPadding = {5, 2};
    s.SeparatorTextBorderSize = 1;
    s.AntiAliasedLines = false;  // crisp one-pixel lines, as the old toolkit drew them
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = {0.45f, 0.49f, 0.41f, 1.0f};
    c[ImGuiCol_WindowBg] = kWindow;
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = kLight;
    c[ImGuiCol_BorderShadow] = kDark;
    c[ImGuiCol_FrameBg] = kPanel;
    c[ImGuiCol_FrameBgHovered] = {0.27f, 0.30f, 0.23f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.30f, 0.34f, 0.26f, 1.0f};
    c[ImGuiCol_TitleBg] = kPanel;
    c[ImGuiCol_TitleBgActive] = {0.29f, 0.34f, 0.26f, 1.0f};
    c[ImGuiCol_TitleBgCollapsed] = kPanel;
    c[ImGuiCol_MenuBarBg] = {0.267f, 0.302f, 0.235f, 1.0f};
    c[ImGuiCol_ScrollbarBg] = {0.353f, 0.408f, 0.314f, 1.0f};
    c[ImGuiCol_ScrollbarGrab] = {0.235f, 0.271f, 0.208f, 1.0f};
    c[ImGuiCol_ScrollbarGrabHovered] = {0.29f, 0.33f, 0.25f, 1.0f};
    c[ImGuiCol_ScrollbarGrabActive] = {0.20f, 0.23f, 0.17f, 1.0f};
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = {0.42f, 0.48f, 0.37f, 1.0f};
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_Button] = {0.333f, 0.384f, 0.298f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.384f, 0.443f, 0.345f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.255f, 0.290f, 0.224f, 1.0f};
    c[ImGuiCol_Header] = kSelect;
    c[ImGuiCol_HeaderHovered] = {0.384f, 0.443f, 0.345f, 1.0f};
    c[ImGuiCol_HeaderActive] = kSelect;
    c[ImGuiCol_Separator] = kDark;
    c[ImGuiCol_SeparatorHovered] = kLight;
    c[ImGuiCol_SeparatorActive] = kGold;
    c[ImGuiCol_ResizeGrip] = {0.19f, 0.23f, 0.18f, 0.0f};
    c[ImGuiCol_ResizeGripHovered] = kLight;
    c[ImGuiCol_ResizeGripActive] = kGold;
    c[ImGuiCol_Tab] = {0.290f, 0.337f, 0.259f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.384f, 0.443f, 0.345f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.243f, 0.275f, 0.212f, 1.0f};
    c[ImGuiCol_TabSelectedOverline] = kGold;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_TabDimmedSelectedOverline] = kGold;
    c[ImGuiCol_PlotLines] = kConsoleText;
    c[ImGuiCol_PlotLinesHovered] = kGold;
    c[ImGuiCol_PlotHistogram] = kGold;
    c[ImGuiCol_PlotHistogramHovered] = {1.0f, 0.85f, 0.4f, 1.0f};
    c[ImGuiCol_TableHeaderBg] = {0.333f, 0.384f, 0.298f, 1.0f};
    c[ImGuiCol_TableBorderStrong] = kDark;
    c[ImGuiCol_TableBorderLight] = {0.20f, 0.23f, 0.17f, 1.0f};
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.03f};
    c[ImGuiCol_TextSelectedBg] = {0.588f, 0.537f, 0.184f, 0.6f};
    c[ImGuiCol_DragDropTarget] = kGold;
    c[ImGuiCol_NavHighlight] = kGold;
    c[ImGuiCol_ModalWindowDimBg] = {0.05f, 0.06f, 0.04f, 0.55f};
    s.ScaleAllSizes(scale);
}

void Bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sunken) {
    // one-pixel filled rectangles on whole pixels (thin lines straddle two pixel rows and come out uneven)
    ImU32 tl = U(sunken ? kDark : kLight), br = U(sunken ? kLight : kDark);
    a = {std::floor(a.x), std::floor(a.y)}, b = {std::floor(b.x), std::floor(b.y)};
    dl->PushClipRect(a, b, false);  // a child window clips just inside its edge: the bevel is drawn on the edge itself
    dl->AddRectFilled({a.x, a.y}, {b.x, a.y + 1}, tl);
    dl->AddRectFilled({a.x, a.y}, {a.x + 1, b.y}, tl);
    dl->AddRectFilled({a.x, b.y - 1}, {b.x, b.y}, br);
    dl->AddRectFilled({b.x - 1, a.y}, {b.x, b.y}, br);
    dl->PopClipRect();
}

void HLine(ImDrawList* dl, float x0, float x1, float y, ImU32 c) {
    dl->AddRectFilled({std::floor(x0), std::floor(y)}, {std::floor(x1), std::floor(y) + 1}, c);
}

bool BeginPanel(const char* title, ImVec2 size, bool scroll) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 6});
    bool open = ImGui::BeginChild(title, size, ImGuiChildFlags_AlwaysUseWindowPadding | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0),
                                  scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    // a sunken outer frame with a raised inner one: the classic etched group box
    Bevel(dl, p, {p.x + s.x, p.y + s.y}, true);
    Bevel(dl, {p.x + 1, p.y + 1}, {p.x + s.x - 1, p.y + s.y - 1}, false);
    const char* end = ImGui::FindRenderedTextEnd(title);
    if (end != title) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        ImGui::TextUnformatted(title, end);
        ImGui::PopStyleColor();
        ImVec2 c = ImGui::GetCursorScreenPos();
        HLine(dl, c.x, p.x + s.x - 8, c.y - 2, U(kDark));
        HLine(dl, c.x, p.x + s.x - 8, c.y - 1, U(kLight, 0.5f));
        ImGui::Dummy({0, 2});
    }
    return open;
}

void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool Button(const char* label, ImVec2 size, bool enabled) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::Button(label, size);
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopStyleVar();
    bool down = enabled && ImGui::IsItemActive() && ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    Bevel(dl, {a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, false);  // the outer frame of old buttons
    {
        ImVec2 o0 = {a.x - 1, a.y - 1}, o1 = {b.x + 1, b.y + 1};
        dl->PushClipRect(o0, o1, false);
        dl->AddRectFilled({std::floor(o0.x), std::floor(o1.y) - 1}, {std::floor(o1.x), std::floor(o1.y)}, U(kDark));
        dl->AddRectFilled({std::floor(o1.x) - 1, std::floor(o0.y)}, {std::floor(o1.x), std::floor(o1.y)}, U(kDark));
        dl->PopClipRect();
    }
    Bevel(dl, a, b, down);
    return pressed && enabled;
}

void Lamp(const ImVec4& color, const char* text) {
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float d = h * 0.62f, y = p.y + (h - d) * 0.5f;
    dl->AddRectFilled({p.x, y}, {p.x + d, y + d}, U(color));
    Bevel(dl, {p.x, y}, {p.x + d, y + d}, true);
    ImGui::Dummy({d + 4, h});
    ImGui::SameLine(0, 0);
    ImGui::TextColored(color, "%s", text);
}

void Row(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kTextDim, "%s", label);
    ImGui::TableNextColumn();
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

void Heading(const char* text) { ImGui::TextColored(kGold, "%s", text); }

void Describe(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) g_description = text;
}
const char* TakeDescription() {
    if (!g_description.empty()) g_last_description = g_description;
    return g_last_description.c_str();
}
void NewFrame() { g_description.clear(); }

}  // namespace theme
