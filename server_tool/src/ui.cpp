#include "ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h"
#include "netaddr.h"
#include "theme.h"
#include "ui_util.h"

namespace fs = std::filesystem;

namespace tool {
using namespace uiutil;


const char* PageName(Page p) {
    static const char* const n[] = {"Overview", "Configuration", "Maps", "Players", "Bots", "Console", "Logs"};
    return n[int(p)];
}

void App::PostDialogResult(int kind, const std::string& path) {
    std::lock_guard<std::mutex> l(dialog_lock_);
    dialog_results_.push_back({kind, path});
}

void App::Message(const std::string& title, const std::string& text) {
    message_title_ = title;
    message_text_ = text;
    message_open_ = true;
}

void App::OpenFolder(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    std::string url = "file://" + fs::absolute(path, ec).generic_string();
#ifdef _WIN32
    url = "file:///" + fs::absolute(path, ec).generic_string();
#endif
    if (!SDL_OpenURL(url.c_str())) Message("Open Folder", "The folder could not be opened:\n" + path + "\n\n" + SDL_GetError());
}

// ---------------------------------------------------------------------------------------------------------------
// state

void App::Init() {
    page_ = Page(std::clamp(settings.GetInt("page", 0), 0, int(Page::Count) - 1));
    config_tab_ = std::clamp(settings.GetInt("config_tab", 0), 0, 2);
    autoscroll_ = settings.GetBool("console_autoscroll", true);
    ctl.game_dir = settings.Get("game_dir");
    std::string exe = settings.Get("server_exe");
    std::error_code ec;
    ctl.server_exe = !exe.empty() && fs::exists(exe, ec) ? exe : FindServerExe(ctl.paths.tool_dir);
    game_dir_input_ = ctl.game_dir;
    profiles_ = ListProfiles(ctl.paths);
    if (profiles_.empty()) {  // the first start: a profile with the server's defaults
        SaveProfileConfig(ctl.paths, "Default", server::Config());
        profiles_ = ListProfiles(ctl.paths);
    }
    std::string want = settings.Get("profile", "Default");
    if (std::find(profiles_.begin(), profiles_.end(), want) == profiles_.end()) want = profiles_.front();
    SelectProfile(want);
    RefreshGameCheck(true);
    ctl.ToolLine("csx-server-tool started; data folder " + ctl.paths.data + (ctl.paths.portable ? " (portable)" : ""));
    if (ctl.server_exe.empty()) ctl.ToolLine("The server program (csx-server) was not found: Tools > Server Program", Severity::Warn);
}

void App::Shutdown() {
    settings.SetInt("page", int(page_));
    settings.SetInt("config_tab", config_tab_);
    settings.SetBool("console_autoscroll", autoscroll_);
    settings.Set("profile", ctl.profile);
    settings.Set("game_dir", ctl.game_dir);
    settings.Set("server_exe", ctl.server_exe);
    settings.Save(settings_path);
}

void App::SelectProfile(const std::string& name) {
    if (ctl.Running()) {
        Message("Profiles", "Stop the server before switching to another profile.");
        return;
    }
    if (!ctl.LoadProfile(name)) {
        std::string text = "The profile's server.cfg has problems; they are shown with their defaults:\n";
        for (auto& p : ctl.start_problems) text += "  " + p + "\n";
        Message("Profile " + name, text);
    }
    edit_ = ctl.config;
    edit_errors_.clear();
    rotation_selected_ = -1;
    settings.Set("profile", name);
    log_selected_ = 0;
    log_list_at_ = -10;
}

void App::RefreshMaps() {
    maps_ = ScanMaps(ctl.game_dir, ImportsDirFor(ctl.server_exe));
    last_maps_scan_ = Now();
}

void App::RefreshGameCheck(bool hash) {
    game_check_ = CheckGameFolder(ctl.game_dir, hash);
    RefreshMaps();
}

bool App::Dirty() const { return server::Describe(edit_, true) != server::Describe(ctl.config, true); }

void App::RevertEdits() {
    edit_ = ctl.config;
    edit_errors_.clear();
}

void App::SaveEdits() {
    std::vector<server::Error> errors;
    server::CheckTogether(edit_, errors);
    if (!errors.empty()) {
        Message("Settings", "Not saved: " + errors.front().text);
        return;
    }
    auto before = Values(ctl.config), after = Values(edit_);
    ctl.config = edit_;
    if (!ctl.SaveProfile()) {
        Message("Settings", "The profile could not be written to " + ProfileDir(ctl.paths, ctl.profile));
        return;
    }
    ctl.ToolLine("Settings saved to profile " + ctl.profile);
    if (!ctl.Running()) return;
    // a running server gets what it can take now; the rest waits for the next map or a restart
    int restart_needed = 0;
    for (const auto& [k, v] : after) {
        if (before[k] == v) continue;
        switch (HowApplied(k)) {
        case Apply::Live:
            if (k == "friendly_fire") ctl.SetFriendlyFireLive(edit_.friendly_fire);
            else if (k == "round_time") ctl.SetRoundTimeLive(int(std::lround(edit_.round_time)));
            else if (k == "password") ctl.SetPasswordLive(edit_.password);
            else if (k == "map_rotation") ctl.SetRotationLive(SplitList(edit_.map_rotation));
            break;
        case Apply::NextMap:
            if (k == "bots") ctl.SetBotsNextMap(edit_.bots);
            else ctl.SetBotDifficultyNextMap(edit_.bot_difficulty);
            break;
        case Apply::Restart: restart_needed++; break;
        }
    }
    if (restart_needed)
        ctl.ToolLine(std::to_string(restart_needed) + " changed setting(s) take effect when the server is restarted",
                     Severity::Warn);
}

std::string App::Value(const char* key) const { return Values(edit_)[key]; }

bool App::SetValue(const char* key, const std::string& value) {
    server::Config c = edit_;
    std::string error;
    if (!server::Set(c, key, value, error)) {
        edit_errors_[key] = error;
        return false;
    }
    edit_errors_.erase(key);
    edit_ = c;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// settings widgets

void App::ApplyBadge(const char* key) {
    ImGui::SameLine();
    switch (HowApplied(key)) {
    case Apply::Live: ImGui::TextColored(theme::kOnline, "[live]"); theme::Describe("Changes the running server at once when saved."); break;
    case Apply::NextMap: ImGui::TextColored(theme::kWarn, "[next map]"); theme::Describe("Takes effect from the next map (the game sets its bots when a match starts). The Bots page can reload the map now."); break;
    case Apply::Restart: ImGui::TextColored(theme::kTextDim, "[restart]"); theme::Describe("Read when the server starts: restart the server to apply it."); break;
    }
    auto it = edit_errors_.find(key);
    if (it != edit_errors_.end()) ImGui::TextColored(theme::kError, "  %s", it->second.c_str());
}

static void Label(const char* label, const char* desc) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    theme::Describe(desc);
    ImGui::SameLine(ImGui::GetFontSize() * 12);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
}

bool App::SettingText(const char* key, const char* label, const char* desc, bool password) {
    Label(label, desc);
    std::string v = Value(key);
    if (std::strcmp(key, "map_rotation") == 0 && v == "none") v.clear();
    if (std::strcmp(key, "gungame_order") == 0 && v == "default") v.clear();
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s", v.c_str());
    ImGui::PushID(key);
    bool changed = ImGui::InputText("##v", buf, sizeof(buf), password ? ImGuiInputTextFlags_Password : 0);
    theme::Describe(desc);
    ImGui::PopID();
    if (changed) SetValue(key, buf);
    ApplyBadge(key);
    return changed;
}

bool App::SettingInt(const char* key, const char* label, const char* desc, int lo, int hi) {
    Label(label, desc);
    int v = std::atoi(Value(key).c_str());
    ImGui::PushID(key);
    bool changed = hi - lo <= 200 ? ImGui::SliderInt("##v", &v, lo, hi) : ImGui::InputInt("##v", &v, 1, 100);
    theme::Describe(desc);
    ImGui::PopID();
    if (changed) SetValue(key, std::to_string(std::clamp(v, lo, hi)));
    ApplyBadge(key);
    return changed;
}

bool App::SettingFloat(const char* key, const char* label, const char* desc, float lo, float hi, const char* fmt) {
    Label(label, desc);
    float v = float(std::atof(Value(key).c_str()));
    ImGui::PushID(key);
    bool changed = ImGui::SliderFloat("##v", &v, lo, hi, fmt);
    theme::Describe(desc);
    ImGui::PopID();
    if (changed) {
        char b[32];
        std::snprintf(b, sizeof(b), fmt, double(std::clamp(v, lo, hi)));
        SetValue(key, b);
    }
    ApplyBadge(key);
    return changed;
}

bool App::SettingBool(const char* key, const char* label, const char* desc) {
    Label(label, desc);
    bool v = Value(key) == "on";
    ImGui::PushID(key);
    bool changed = ImGui::Checkbox("##v", &v);
    theme::Describe(desc);
    ImGui::PopID();
    if (changed) SetValue(key, v ? "on" : "off");
    ApplyBadge(key);
    return changed;
}

bool App::SettingCombo(const char* key, const char* label, const char* desc, const std::vector<std::string>& options,
                       const std::vector<std::string>& labels) {
    Label(label, desc);
    std::string v = Value(key);
    bool changed = false;
    auto shown = [&](size_t i) { return labels.size() > i ? labels[i].c_str() : options[i].c_str(); };
    std::string preview = v;
    for (size_t i = 0; i < options.size(); i++)
        if (options[i] == v) preview = shown(i);
    ImGui::PushID(key);
    if (ImGui::BeginCombo("##v", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < options.size(); i++)
            if (ImGui::Selectable(shown(i), options[i] == v)) changed = SetValue(key, options[i]);
        ImGui::EndCombo();
    }
    theme::Describe(desc);
    ImGui::PopID();
    ApplyBadge(key);
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------
// frame

void App::Frame() {
    theme::NewFrame();
    HandleDialogResults();
    ctl.Update();
    if (Now() - last_maps_scan_ > 30) RefreshMaps();  // maps added to the folder appear without a restart
    if (ctl.crash_prompt && !message_open_) {
        ctl.crash_prompt = false;
        confirm_restart_ = true;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    MenuBar();
    float status_h = ImGui::GetFrameHeight() + 4;
    float side_w = std::max(240.0f, ImGui::GetFontSize() * 18.5f);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    Sidebar();
    ImGui::SameLine();
    ImGui::BeginChild("##page", {avail.x - side_w - ImGui::GetStyle().ItemSpacing.x, avail.y - status_h}, 0,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        theme::Bevel(dl, p, {p.x + s.x, p.y + s.y}, true);
    }
    ImGui::SetCursorPos({8, 6});
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kGold);
    ImGui::TextUnformatted(PageName(page_));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(theme::kTextDim, " -  Counter-Strike Xbox Dedicated Server  -  profile \"%s\"", ctl.profile.c_str());
    ImGui::EndGroup();
    ImGui::SetCursorPosX(8);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4, 2});
    ImGui::BeginChild("##pagebody", {ImGui::GetContentRegionAvail().x - 8, ImGui::GetContentRegionAvail().y - 6},
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (page_) {
    case Page::Overview: PageOverview(); break;
    case Page::Configuration: PageConfiguration(); break;
    case Page::Maps: PageMaps(); break;
    case Page::Players: PagePlayers(); break;
    case Page::Bots: PageBots(); break;
    case Page::Console: PageConsole(); break;
    case Page::Logs: PageLogs(); break;
    default: break;
    }
    ImGui::EndChild();
    ImGui::EndChild();
    StatusBar();
    Dialogs();
    ImGui::End();
}

void App::MenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    bool running = ctl.Running();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Profile...")) new_profile_ = true, profile_name_[0] = 0;
        if (ImGui::MenuItem("Duplicate Profile...")) new_profile_ = true, std::snprintf(profile_name_, sizeof(profile_name_), "%s copy", ctl.profile.c_str());
        if (ImGui::MenuItem("Delete Profile...", nullptr, false, !running && profiles_.size() > 1)) delete_profile_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Import Configuration...", nullptr, false, !running)) ImportConfig();
        if (ImGui::MenuItem("Export Configuration...")) ExportConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            if (RequestClose()) quit_requested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Server")) {
        if (ImGui::MenuItem("Start Server", nullptr, false, !running)) ctl.Start();
        if (ImGui::MenuItem("Stop Server", nullptr, false, running)) confirm_stop_ = true;
        if (ImGui::MenuItem("Restart Server", nullptr, false, running)) ctl.Restart();
        ImGui::Separator();
        if (ImGui::MenuItem("Change Map...", nullptr, false, running)) page_ = Page::Maps;
        if (ImGui::MenuItem("Restart Round", nullptr, false, running)) ctl.RestartRound(1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int i = 0; i < int(Page::Count); i++)
            if (ImGui::MenuItem(PageName(Page(i)), nullptr, page_ == Page(i))) page_ = Page(i);
        ImGui::Separator();
        if (ImGui::MenuItem("Minimize to Tray", nullptr, false, tray_available)) hide_to_tray = true;
        bool tray_min = settings.GetBool("minimize_to_tray", false), start_min = settings.GetBool("start_minimized", false);
        if (ImGui::MenuItem("Close to Tray", nullptr, &tray_min, tray_available)) settings.SetBool("minimize_to_tray", tray_min);
        if (ImGui::MenuItem("Start Minimized", nullptr, &start_min)) settings.SetBool("start_minimized", start_min);
        bool confirm = settings.GetBool("confirm_stop", true);
        if (ImGui::MenuItem("Confirm Before Stopping", nullptr, &confirm)) settings.SetBool("confirm_stop", confirm);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Game Folder...")) ChooseGameFolder();
        if (ImGui::MenuItem("Validate Game Files")) {
            RefreshGameCheck(true);
            std::string text = game_check_.ok ? "The game files are complete and default.xbe is the supported version.\n"
                                              : "Problems:\n";
            for (auto& p : game_check_.problems) text += "  - " + p + "\n";
            for (auto& n : game_check_.notes) text += "  " + n + "\n";
            Message("Validate Game Files", text);
        }
        if (ImGui::MenuItem("Server Program...")) ChooseServerExe();
        ImGui::Separator();
        if (ImGui::MenuItem("Open Server Folder")) OpenFolder(ProfileServerDir(ctl.paths, ctl.profile));
        if (ImGui::MenuItem("Open Logs Folder")) OpenFolder((fs::path(ProfileServerDir(ctl.paths, ctl.profile)) / "logs").string());
        if (ImGui::MenuItem("Open Data Folder")) OpenFolder(ctl.paths.data);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About csx-server-tool")) show_about_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::Sidebar() {
    float side_w = std::max(240.0f, ImGui::GetFontSize() * 18.5f);
    float h = ImGui::GetContentRegionAvail().y - (ImGui::GetFrameHeight() + 4);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPanelDark);
    ImGui::BeginChild("##side", {side_w, h}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    theme::Bevel(dl, p, {p.x + s.x, p.y + s.y}, true);

    auto heading = [&](const char* t) {
        ImGui::Dummy({0, 2});
        ImGui::TextColored(theme::kGold, "%s", t);
        ImVec2 c = ImGui::GetCursorScreenPos();
        theme::HLine(dl, c.x, p.x + s.x - 8, c.y - 1, ImGui::GetColorU32(theme::kDark));
        ImGui::Dummy({0, 2});
    };
    heading("Dedicated Servers");
    ImGui::Indent(8);
    ImGui::PushStyleColor(ImGuiCol_Header, theme::kSelect);
    ImGui::Selectable("Counter-Strike Xbox Dedicated Server", true, ImGuiSelectableFlags_None);
    ImGui::PopStyleColor();
    ImGui::Indent(10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kTextDim, "Profile");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##profile", ctl.profile.c_str())) {
        for (const auto& name : profiles_)
            if (ImGui::Selectable(name.c_str(), name == ctl.profile) && name != ctl.profile) SelectProfile(name);
        ImGui::EndCombo();
    }
    theme::Describe("Each profile is a server: its settings, its folder (identity key, admins, bans, logs). File > New Profile adds one.");
    if (ctl.Running()) ImGui::TextColored(theme::kTextDim, "(stop the server to switch)");
    ImGui::Unindent(10);
    ImGui::Unindent(8);
    heading("Server Management");
    ImGui::Indent(8);
    for (int i = 0; i < int(Page::Count); i++) {
        bool sel = page_ == Page(i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 0.96f, 0.80f, 1});
        if (ImGui::Selectable(PageName(Page(i)), sel)) page_ = Page(i);
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::Unindent(8);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::StatusBar() {
    ImGui::Dummy({0, 1});
    theme::Lamp(StateColor(ctl.state), StateName(ctl.state));
    ImGui::SameLine();
    std::string detail = ctl.state_detail;
    if (ctl.state == ServerState::Online && ctl.status.valid)
        detail = Fmt("%s   %d/%d players (%d bots)   up %s", ctl.status.map.c_str(), ctl.status.humans + ctl.status.bots,
                     ctl.status.max_players, ctl.status.bots, FormatDuration(ctl.Uptime()).c_str());
    ImGui::TextColored(theme::kTextDim, "  %s", detail.c_str());
    std::string right = std::string(ctl.paths.portable ? "Portable" : "Per-user") + " data: " + ctl.paths.data;
    float w = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 14));
    ImGui::TextColored(theme::kTextDim, "%s", right.c_str());
}

std::string App::TrayStatus() const {
    std::string s = std::string("CS Xbox server: ") + StateName(ctl.state);
    if (ctl.status.valid && ctl.state == ServerState::Online)
        s += " - " + ctl.status.map + ", " + std::to_string(ctl.status.humans + ctl.status.bots) + " players";
    return s;
}

void App::TrayStartStop() {
    if (ctl.Running()) ctl.Stop();
    else ctl.Start();
}

bool App::RequestClose() {
    if (!ctl.Running()) return true;
    confirm_exit_ = true;
    return false;
}

}  // namespace tool
