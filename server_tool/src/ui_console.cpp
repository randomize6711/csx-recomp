// The server tool's Console and Logs pages, its dialogs and file pickers (ui.h).
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "imgui_internal.h"
#include "ui.h"
#include "ui_util.h"

namespace fs = std::filesystem;

namespace tool {
using namespace uiutil;

namespace {

bool SevShown(Severity s, bool info, bool warn, bool error, bool tool) {
    switch (s) {
    case Severity::Warn: return warn;
    case Severity::Error: return error;
    case Severity::Tool: return tool;
    default: return info;
    }
}

// the log's own time ("[   12.345] ..."), seconds since the process started; -1 when the line has none
double LogTime(const std::string& line) {
    if (line.size() < 3 || line[0] != '[') return -1;
    return std::atof(line.c_str() + 1);
}

int HistoryCallback(ImGuiInputTextCallbackData* d) {
    auto* self = static_cast<std::pair<std::vector<std::string>*, int*>*>(d->UserData);
    auto& hist = *self->first;
    int& pos = *self->second;
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || hist.empty()) return 0;
    int prev = pos;
    if (d->EventKey == ImGuiKey_UpArrow) pos = pos < 0 ? int(hist.size()) - 1 : std::max(0, pos - 1);
    else if (d->EventKey == ImGuiKey_DownArrow && pos >= 0) pos = pos + 1 >= int(hist.size()) ? -1 : pos + 1;
    if (prev != pos) {
        d->DeleteChars(0, d->BufTextLen);
        if (pos >= 0) d->InsertChars(0, hist[size_t(pos)].c_str());
    }
    return 0;
}

}  // namespace

void App::PageConsole() {
    // the filtered view is rebuilt only when the lines or the filters change, so a busy server stays cheap to show
    static std::vector<size_t> view;
    static uint64_t view_last = ~0ull;
    static std::string view_key;
    std::string key = std::string(console_search_) + char('0' + show_info_) + char('0' + show_warn_) + char('0' + show_error_) +
                      char('0' + show_tool_) + std::to_string(ctl.console.Dropped());
    const auto& lines = ctl.console.Lines();
    if (view_last != ctl.console.LastId() || view_key != key) {
        view.clear();
        for (size_t i = 0; i < lines.size(); i++)
            if (SevShown(lines[i].sev, show_info_, show_warn_, show_error_, show_tool_) && ContainsNoCase(lines[i].text, console_search_))
                view.push_back(i);
        view_last = ctl.console.LastId();
        view_key = key;
    }

    // the toolbar
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##search", "Search output", console_search_, sizeof(console_search_));
    ImGui::SameLine();
    ImGui::Checkbox("Info", &show_info_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarn);
    ImGui::Checkbox("Warnings", &show_warn_);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kError);
    ImGui::Checkbox("Errors", &show_error_);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kToolText);
    ImGui::Checkbox("Tool", &show_tool_);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoscroll_);
    theme::Describe("Follow new output. Scrolling up pauses it; scrolling to the end resumes it.");
    float bw = ImGui::GetFontSize() * 7;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x));
    if (theme::Button("Copy Console", {bw, 0})) {
        std::string all;
        for (size_t i : view) all += FormatClock(lines[i].wall_ms) + "  " + lines[i].text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    theme::Describe("Copies the lines shown (with the filters and search) to the clipboard.");
    ImGui::SameLine();
    if (theme::Button("Clear Console", {bw, 0})) ctl.console.Clear(), view_last = ~0ull;

    // the output
    float input_h = ImGui::GetFrameHeightWithSpacing() + 4;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kConsoleBg);
    if (mono) ImGui::PushFont(mono);
    ImGui::BeginChild("##out", {0, ImGui::GetContentRegionAvail().y - input_h}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        theme::Bevel(ImGui::GetWindowDrawList(), p, {p.x + s.x, p.y + s.y}, true);
    }
    ImGuiListClipper clip;
    clip.Begin(int(view.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const ConsoleLine& l = lines[view[size_t(r)]];
            ImGui::TextColored(theme::kTextDim, "%s", FormatClock(l.wall_ms).c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(l.sev));
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem(std::to_string(l.id).c_str())) {
                if (ImGui::MenuItem("Copy Line")) ImGui::SetClipboardText(l.text.c_str());
                ImGui::EndPopup();
            }
        }
    // auto-scroll: follows the end; scrolling up by hand pauses it, back at the end resumes it
    bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2;
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0 && !at_end) autoscroll_ = false;
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel < 0 && at_end) autoscroll_ = true;
    if (autoscroll_ && console_seen_id_ != ctl.console.LastId()) ImGui::SetScrollHereY(1.0f);
    console_seen_id_ = ctl.console.LastId();
    ImGui::EndChild();
    if (mono) ImGui::PopFont();
    ImGui::PopStyleColor();

    // the command line
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGold, "Command");
    ImGui::SameLine();
    std::pair<std::vector<std::string>*, int*> hist{&console_history_, &history_pos_};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
    if (console_focus_) ImGui::SetKeyboardFocusHere(), console_focus_ = false;
    bool enter = ImGui::InputTextWithHint("##cmd", ctl.Running() ? "status, say hello, admin help, mp_timelimit 20 ..." : "(server offline)",
                                          console_input_, sizeof(console_input_),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, HistoryCallback, &hist);
    theme::Describe("Engine commands, and \"admin ...\" commands as the server's owner (admin help lists them). Up and Down recall earlier commands.");
    ImGui::SameLine();
    if ((theme::Button("Submit", {-1, 0}, ctl.Running()) || enter) && console_input_[0]) {
        ctl.Command(console_input_);
        if (console_history_.empty() || console_history_.back() != console_input_) console_history_.push_back(console_input_);
        if (console_history_.size() > 200) console_history_.erase(console_history_.begin());
        history_pos_ = -1;
        console_input_[0] = 0;
        autoscroll_ = true;
        console_focus_ = true;
    }
}

void App::PageLogs() {
    std::string dir = ProfileServerDir(ctl.paths, ctl.profile);
    if (Now() - log_list_at_ > 3) {
        log_files_ = ListLogFiles(dir);
        log_list_at_ = Now();
    }
    float list_w = ImGui::GetFontSize() * 15;
    float h = ImGui::GetContentRegionAvail().y;
    theme::BeginPanel("Sessions", {list_w, h}, true);
    if (ImGui::Selectable("This session (live)", log_selected_ == 0)) log_selected_ = 0;
    for (size_t i = 0; i < log_files_.size(); i++) {
        const LogFile& f = log_files_[i];
        std::string label = f.name + "  " + FormatClock(f.modified_unix * 1000) + Fmt("  %.0f KB", double(f.size) / 1024);
        if (ImGui::Selectable(label.c_str(), log_selected_ == int(i) + 1)) log_selected_ = int(i) + 1;
    }
    if (log_files_.empty()) ImGui::TextColored(theme::kTextDim, "No log files yet.");
    theme::EndPanel();
    ImGui::SameLine();
    ImGui::BeginGroup();
    // the lines to show: the live output, or a file (read once; Reload reads it again)
    std::vector<std::pair<std::string, Severity>> shown;
    bool live = log_selected_ == 0 || log_selected_ > int(log_files_.size());
    if (!live) {
        const LogFile& f = log_files_[size_t(log_selected_ - 1)];
        if (log_loaded_ != f.path) {
            log_lines_ = ReadLogTail(f.path);
            log_loaded_ = f.path;
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13);
    ImGui::InputTextWithHint("##lsearch", "Search", log_search_, sizeof(log_search_));
    ImGui::SameLine();
    ImGui::Checkbox("Info##l", &log_info_);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings##l", &log_warn_);
    ImGui::SameLine();
    ImGui::Checkbox("Errors##l", &log_error_);
    ImGui::SameLine();
    bool copy = theme::Button("Copy");
    ImGui::SameLine();
    if (theme::Button("Export...")) ExportLog();
    theme::Describe("Saves the lines shown to a file of your choice.");
    ImGui::SameLine();
    if (theme::Button("Clear")) {
        if (live) ctl.console.Clear();
        else log_lines_.clear();
    }
    theme::Describe("Clears the lines shown here (the log files themselves are kept).");
    ImGui::SameLine();
    if (!live && theme::Button("Reload")) log_loaded_.clear();
    if (!live) ImGui::SameLine();
    if (theme::Button("Open Logs Folder")) OpenFolder((fs::path(dir) / "logs").string());

    // absolute times for a file's lines: the server logs its wall clock at each (re)start
    std::vector<std::string> times;
    if (live) {
        for (const auto& l : ctl.console.Lines())
            if (l.sev != Severity::Tool && SevShown(l.sev, log_info_, log_warn_, log_error_, false) && ContainsNoCase(l.text, log_search_))
                shown.push_back({l.text, l.sev}), times.push_back(FormatClock(l.wall_ms));
    } else {
        double base = -1;  // Unix seconds at log time 0
        for (const auto& t : log_lines_) {
            size_t wc = t.find("server: wall clock ");
            if (wc != std::string::npos) base = std::atof(t.c_str() + wc + 19) - std::max(0.0, LogTime(t));
            Severity s = LineSeverity(t);
            if (!SevShown(s, log_info_, log_warn_, log_error_, false) || !ContainsNoCase(t, log_search_)) continue;
            shown.push_back({t, s});
            double lt = LogTime(t);
            times.push_back(base >= 0 && lt >= 0 ? FormatClock(int64_t((base + lt) * 1000)) : "--:--:--");
        }
    }
    if (copy) {
        std::string all;
        for (size_t i = 0; i < shown.size(); i++) all += times[i] + "  " + shown[i].first + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    static std::vector<std::string> export_lines;  // what Export writes (the lines shown when it was chosen)
    export_lines.clear();
    for (size_t i = 0; i < shown.size(); i++) export_lines.push_back(times[i] + "  " + shown[i].first);
    log_lines_export_ = &export_lines;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kConsoleBg);
    if (mono) ImGui::PushFont(mono);
    ImGui::BeginChild("##logview", {0, ImGui::GetContentRegionAvail().y}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        theme::Bevel(ImGui::GetWindowDrawList(), p, {p.x + s.x, p.y + s.y}, true);
    }
    ImGuiListClipper clip;
    clip.Begin(int(shown.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            ImGui::TextColored(theme::kTextDim, "%s", times[size_t(r)].c_str());
            ImGui::SameLine();
            ImGui::TextColored(SeverityColor(shown[size_t(r)].second), "%s", shown[size_t(r)].first.c_str());
        }
    if (live && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (mono) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------------------------------------------
// files

void App::ChooseGameFolder() {
    auto* ud = new std::pair<App*, int>(this, kDlgGameFolder);
    SDL_ShowOpenFolderDialog(DialogCallback, ud, window, ctl.game_dir.empty() ? nullptr : ctl.game_dir.c_str(), false);
}

void App::ChooseServerExe() {
    auto* ud = new std::pair<App*, int>(this, kDlgServerExe);
    SDL_ShowOpenFileDialog(DialogCallback, ud, window, nullptr, 0, ctl.server_exe.empty() ? nullptr : ctl.server_exe.c_str(), false);
}

void App::ExportConfig() {
    static const SDL_DialogFileFilter f[] = {{"Server configuration", "cfg"}};
    auto* ud = new std::pair<App*, int>(this, kDlgExportConfig);
    SDL_ShowSaveFileDialog(DialogCallback, ud, window, f, 1, (ctl.profile + ".cfg").c_str());
}

void App::ImportConfig() {
    static const SDL_DialogFileFilter f[] = {{"Server configuration", "cfg;txt"}};
    auto* ud = new std::pair<App*, int>(this, kDlgImportConfig);
    SDL_ShowOpenFileDialog(DialogCallback, ud, window, f, 1, nullptr, false);
}

void App::ExportLog() {
    static const SDL_DialogFileFilter f[] = {{"Text", "txt;log"}};
    auto* ud = new std::pair<App*, int>(this, kDlgExportLog);
    SDL_ShowSaveFileDialog(DialogCallback, ud, window, f, 1, "server-log.txt");
}

void App::HandleDialogResults() {
    std::vector<DialogResult> results;
    {
        std::lock_guard<std::mutex> l(dialog_lock_);
        results.swap(dialog_results_);
    }
    for (const auto& r : results) {
        if (r.kind < 0) {  // the system has no file picker: the fallbacks
            std::string why = r.path;
            std::string exports = (fs::path(ctl.paths.data) / "exports").string();
            int kind = -r.kind;
            if (kind == kDlgGameFolder) {
                page_ = Page::Overview;
                game_check_.ok = false;
                Message("Game Folder", "No folder picker is available on this system (" + why + ").\nType or paste the folder's path in the box on the Overview page.");
            } else if (kind == kDlgExportConfig) {
                PostDialogResult(kDlgExportConfig, (fs::path(exports) / (ctl.profile + ".cfg")).string());
            } else if (kind == kDlgExportLog) {
                PostDialogResult(kDlgExportLog, (fs::path(exports) / "server-log.txt").string());
            } else {
                Message("File", "No file picker is available on this system (" + why + ").\nPut the file in " + exports + " and edit the profile's server.cfg in " +
                                    ProfileDir(ctl.paths, ctl.profile) + ".");
            }
            continue;
        }
        if (r.kind == kDlgGameFolder) {
            ctl.game_dir = r.path;
            game_dir_input_ = r.path;
            settings.Set("game_dir", r.path);
            RefreshGameCheck(true);
            if (!game_check_.ok) Message("Game Folder", "This folder is not usable yet:\n  - " + game_check_.problems.front());
        } else if (r.kind == kDlgServerExe) {
            ctl.server_exe = r.path;
            settings.Set("server_exe", r.path);
            RefreshMaps();
        } else if (r.kind == kDlgExportConfig) {
            std::string text = "# csx-server settings exported from profile " + ctl.profile + " (csx-server --config FILE)\n" +
                               server::Describe(edit_, true);
            if (WriteFileAtomic(r.path, text, true)) Message("Export Configuration", "Saved to " + r.path + "\n(It contains the password, if one is set.)");
            else Message("Export Configuration", "Could not write " + r.path);
        } else if (r.kind == kDlgImportConfig) {
            std::string text;
            if (!ReadFileText(r.path, text)) {
                Message("Import Configuration", "Could not read " + r.path);
                continue;
            }
            server::Config c;
            std::vector<server::Error> errors;
            server::ParseFile(c, text, fs::path(r.path).filename().string(), errors);
            server::CheckTogether(c, errors);
            if (!errors.empty()) {
                std::string m = "Not imported; the file has problems:\n";
                for (size_t i = 0; i < errors.size() && i < 12; i++) m += "  " + errors[i].where + ": " + errors[i].text + "\n";
                Message("Import Configuration", m);
                continue;
            }
            edit_ = c;
            edit_errors_.clear();
            Message("Import Configuration", "Imported into the editor. Save Changes keeps them in profile " + ctl.profile + ".");
        } else if (r.kind == kDlgExportLog) {
            std::string text;
            if (log_lines_export_)
                for (const auto& l : *log_lines_export_) text += l + "\n";
            if (WriteFileAtomic(r.path, text)) Message("Export Log", "Saved " + std::to_string(log_lines_export_ ? log_lines_export_->size() : 0) + " lines to " + r.path);
            else Message("Export Log", "Could not write " + r.path);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// dialogs

void App::Dialogs() {
    float w = ImGui::GetFontSize() * 28;
    if (BeginModal("Stop Server", &confirm_stop_, w)) {
        int humans = ctl.status.valid ? ctl.status.humans : 0;
        ImGui::TextWrapped("%s", humans > 0 ? Fmt("%d player(s) are connected. They are told the server is shutting down. Stop the server?", humans).c_str()
                                            : "Stop the server?");
        if (theme::Button("Stop Server", {w * 0.3f, 0})) ctl.Stop(), confirm_stop_ = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (theme::Button("Cancel", {w * 0.3f, 0})) confirm_stop_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("Server Ended", &confirm_restart_, w)) {
        ImGui::TextWrapped("The server ended unexpectedly (exit code %d).", ctl.last_exit_code);
        if (!ctl.state_detail.empty()) ImGui::TextColored(theme::kError, "%s", ctl.state_detail.c_str());
        ImGui::TextWrapped("The Console and Logs pages have its last output. Start it again?");
        if (theme::Button("Restart Server", {w * 0.3f, 0})) ctl.Start(), confirm_restart_ = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (theme::Button("Show Console", {w * 0.3f, 0})) page_ = Page::Console, confirm_restart_ = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (theme::Button("Close", {w * 0.3f, 0})) confirm_restart_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("Exit", &confirm_exit_, w)) {
        ImGui::TextWrapped("The server is running%s. Closing the tool stops it.",
                           ctl.status.valid && ctl.status.humans ? Fmt(" with %d player(s)", ctl.status.humans).c_str() : "");
        if (theme::Button("Stop and Exit", {w * 0.3f, 0})) {
            ctl.Stop();
            quit_requested = true;
            confirm_exit_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (tray_available && theme::Button("Minimize to Tray", {w * 0.3f, 0})) hide_to_tray = true, confirm_exit_ = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (theme::Button("Cancel", {w * 0.25f, 0})) confirm_exit_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("Change Map", &show_change_map_, w)) {
        ImGui::TextWrapped("Change the running server to %s now? Every player rejoins by themselves after a few seconds.", selected_map_.c_str());
        if (theme::Button("Change Map", {w * 0.3f, 0})) ctl.ChangeMap(selected_map_), show_change_map_ = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (theme::Button("Cancel", {w * 0.3f, 0})) show_change_map_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("New Profile", &new_profile_, w)) {
        ImGui::TextWrapped("A profile is a server with its own settings and folder. The new one starts as a copy of \"%s\".", ctl.profile.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##pname", profile_name_, sizeof(profile_name_));
        bool exists = std::find(profiles_.begin(), profiles_.end(), profile_name_) != profiles_.end();
        bool ok = ValidProfileName(profile_name_) && !exists;
        if (!ValidProfileName(profile_name_)) ImGui::TextColored(theme::kTextDim, "Letters, digits, spaces, - and _ (up to 40).");
        if (exists) ImGui::TextColored(theme::kError, "A profile of that name exists.");
        if (theme::Button("Create", {w * 0.3f, 0}, ok && !ctl.Running())) {
            SaveProfileConfig(ctl.paths, profile_name_, edit_);
            Ini t;
            t.Load((fs::path(ProfileDir(ctl.paths, ctl.profile)) / "tool.ini").string());
            t.Save((fs::path(ProfileDir(ctl.paths, profile_name_)) / "tool.ini").string());
            profiles_ = ListProfiles(ctl.paths);
            SelectProfile(profile_name_);
            new_profile_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (ctl.Running()) ImGui::TextColored(theme::kTextDim, "Stop the server first.");
        ImGui::SameLine();
        if (theme::Button("Cancel", {w * 0.3f, 0})) new_profile_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("Delete Profile", &delete_profile_, w)) {
        ImGui::TextWrapped("Delete profile \"%s\" with its server folder (its identity key, admins, bans and logs)? This cannot be undone.",
                           ctl.profile.c_str());
        if (theme::Button("Delete", {w * 0.3f, 0}, !ctl.Running() && profiles_.size() > 1)) {
            std::string gone = ctl.profile;
            DeleteProfile(ctl.paths, gone);
            profiles_ = ListProfiles(ctl.paths);
            if (!profiles_.empty()) SelectProfile(profiles_.front());
            ctl.ToolLine("Profile deleted: " + gone);
            delete_profile_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (theme::Button("Cancel", {w * 0.3f, 0})) delete_profile_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal("About", &show_about_, w * 1.1f)) {
        theme::Heading("csx-server-tool");
        ImGui::TextWrapped("The dedicated server tool of the Counter-Strike Xbox PC port (csx-recomp). It runs and administers "
                           "csx-server, the port's dedicated server, on this computer.");
        ImGui::TextWrapped("An unofficial fan project, not affiliated with Valve, Microsoft or Steam. No game files are included: "
                           "the server uses your own copy of the game.");
        ImGui::TextColored(theme::kTextDim, "Built with Dear ImGui and SDL3. MIT License.");
        if (theme::Button("Close", {w * 0.3f, 0})) show_about_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (BeginModal(message_title_.empty() ? "Message###msg" : (message_title_ + "###msg").c_str(), &message_open_, w * 1.2f)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(message_text_.c_str());
        ImGui::PopTextWrapPos();
        if (theme::Button("OK", {w * 0.3f, 0})) message_open_ = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace tool
