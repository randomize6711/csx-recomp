// The server tool's pages: Overview, Configuration, Maps, Players, Bots (ui.h).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "netaddr.h"
#include "ui.h"
#include "ui_util.h"

namespace fs = std::filesystem;

namespace tool {
using namespace uiutil;

namespace {
const char* NA = "n/a";

// a small history graph from the controller's samples
void Graph(const char* id, const std::deque<Sample>& h, float Sample::*field, const char* overlay, float lo, float hi_min,
           ImVec2 size) {
    std::vector<float> v;
    v.reserve(h.size());
    float hi = hi_min;
    for (const auto& s : h) {
        float x = s.*field;
        v.push_back(x < 0 ? 0 : x);
        hi = std::max(hi, x * 1.15f);
    }
    if (v.empty()) v.push_back(0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kConsoleBg);
    ImGui::PlotLines(id, v.data(), int(v.size()), 0, overlay, lo, hi, size);
    ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    theme::Bevel(ImGui::GetWindowDrawList(), a, b, true);
}
}  // namespace

void App::GameFolderBanner() {
    if (game_check_.ok) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.36f, 0.25f, 0.16f, 1.0f});
    ImGui::BeginChild("##gamebanner", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(theme::kWarn, "Your Counter-Strike game files are needed before a server can start.");
    for (const auto& p : game_check_.problems) ImGui::BulletText("%s", p.c_str());
    ImGui::TextWrapped("Use the files from your own copy of the game (the disc's files: default.xbe, the .SXWad maps, "
                       "Media). The supported default.xbe is title ID MS-054 (PAL), SHA-256 e7698b09...308f. "
                       "Nothing is copied or changed: the server reads them from where they are.");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", game_dir_input_.c_str());
    if (ImGui::InputTextWithHint("##gamedir", "Game folder path", buf, sizeof(buf))) game_dir_input_ = buf;
    ImGui::SameLine();
    if (theme::Button("Use This Folder")) {
        ctl.game_dir = game_dir_input_;
        settings.Set("game_dir", ctl.game_dir);
        RefreshGameCheck(true);
    }
    ImGui::SameLine();
    if (theme::Button("Browse...")) ChooseGameFolder();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void App::PageOverview() {
    GameFolderBanner();
    const Status& st = ctl.status;
    bool live = st.valid && ctl.status_age < 5 && ctl.Running();
    float w = ImGui::GetContentRegionAvail().x;
    float col = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // --- Server Control
    theme::BeginPanel("Server Control", {col, 0});
    {
        bool running = ctl.Running();
        float bw = ImGui::GetFontSize() * 7.5f;
        if (theme::Button("Start Server", {bw, 0}, !running && ctl.state != ServerState::Stopping)) ctl.Start();
        ImGui::SameLine();
        if (theme::Button("Stop Server", {bw, 0}, running && !(ctl.state == ServerState::Stopping))) {
            if (settings.GetBool("confirm_stop", true) && st.valid && st.humans > 0) confirm_stop_ = true;
            else if (settings.GetBool("confirm_stop", true)) confirm_stop_ = true;
            else ctl.Stop();
        }
        ImGui::SameLine();
        if (theme::Button("Restart Server", {bw, 0}, running && ctl.state == ServerState::Online)) ctl.Restart();
        ImGui::Spacing();
        if (ImGui::BeginTable("##ctl", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::kTextDim, "Server Status");
            ImGui::TableNextColumn();
            theme::Lamp(StateColor(ctl.state), StateName(ctl.state));
            if (!ctl.state_detail.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(theme::kTextDim, "- %s", ctl.state_detail.c_str());
            }
            theme::Row("Uptime", "%s", running ? FormatDuration(ctl.Uptime()).c_str() : "-");
            theme::Row("Current Map", "%s", live && !st.map.empty() ? st.map.c_str() : "-");
            theme::Row("Players", "%s", live ? Fmt("%d humans, %d bots (%d of %d slots)", st.humans, st.bots, st.humans + st.bots, st.max_players).c_str() : "-");
            theme::Row("Process ID", "%s", running ? std::to_string(ctl.Pid()).c_str() : "-");
            ImGui::EndTable();
        }
        if (ctl.state == ServerState::Starting) {
            float t = float(std::fmod(Now(), 1.5) / 1.5);
            ImGui::ProgressBar(-1.0f * t, {-1, 0}, ctl.state_detail.c_str());
        }
        if (ctl.restart_at >= 0) {
            ImGui::TextColored(theme::kWarn, "%s", ctl.state_detail.c_str());
            ImGui::SameLine();
            if (theme::Button("Cancel Restart")) ctl.Stop();
        }
    }
    theme::EndPanel();
    ImGui::SameLine();

    // --- Server Information
    theme::BeginPanel("Server Information", {col, 0});
    if (ImGui::BeginTable("##info", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        const server::Config& c = ctl.config;
        theme::Row("Server Name", "%s", c.name.c_str());
        auto addrs = LocalAddresses();
        std::string ips;
        for (const auto& a : addrs)
            if (!a.loopback) ips += (ips.empty() ? "" : ", ") + a.address;
        theme::Row("Server IP", "%s", ips.empty() ? "127.0.0.1 (no network)" : ips.c_str());
        theme::Describe("This machine's addresses. Players on your network connect to one of them; players on the internet need your public address and the UDP port forwarded (not done by this tool).");
        theme::Row("Listening Port", "UDP %d%s", live ? st.port : c.port, live ? "" : " (configured)");
        theme::Row("Current Map", "%s", live ? st.map.c_str() : (c.map + " (starting map)").c_str());
        theme::Row("Next Map", "%s", live ? (st.next_map.empty() ? "the same map (no rotation)" : st.next_map.c_str()) : "-");
        theme::Row("Maximum Players", "%d (humans and bots share the slots)", c.max_players);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::kTextDim, "LAN Only");
        ImGui::TableNextColumn();
        bool lan = edit_.lan_only;
        if (ImGui::Checkbox("##lan", &lan)) SetValue("lan_only", lan ? "on" : "off");
        theme::Describe("Refuses players from outside private networks (10/8, 172.16/12, 192.168/16). Saved with the profile; read when the server starts.");
        ImGui::SameLine();
        ImGui::TextColored(theme::kTextDim, "%s", live ? (st.lan_only ? "(running: on)" : "(running: off)") : "");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::kTextDim, "Public Listing");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.master.empty() ? "Off (no master server set)" : ("Master servers: " + c.master).c_str());
        theme::Describe("Server lists are user-hosted master servers (Configuration > Advanced). There is no central list.");
        theme::Row("Password", "%s", (live ? st.password : !c.password.empty()) ? "Protected" : "None");
        ImGui::EndTable();
    }
    theme::EndPanel();

    // --- Server Statistics
    ImGui::Spacing();
    theme::BeginPanel("Server Statistics", {w, 0});
    if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_SizingStretchSame)) {
        auto cell = [&](const char* label, const std::string& value, const char* desc) {
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::kTextDim, "%s", label);
            theme::Describe(desc);
            ImGui::TextUnformatted(value.c_str());
            theme::Describe(desc);
        };
        ImGui::TableNextRow();
        cell("CPU Usage", ctl.meter.cpu_percent ? Fmt("%.1f %% of one core", *ctl.meter.cpu_percent) : NA,
             "The server process's CPU time per second, measured by the operating system.");
        cell("Memory Usage", ctl.meter.memory_mb ? Fmt("%.1f MB", *ctl.meter.memory_mb) : NA,
             "The server process's resident memory, measured by the operating system.");
        cell("Tick Rate", live ? Fmt("%.1f Hz (frame %.2f ms, max %.2f)", st.tick_hz, st.frame_ms, st.frame_ms_max) : NA,
             "Host frames a second, measured by the server (it runs a fixed 30 Hz tick), and the work of a frame.");
        cell("Uptime", ctl.Running() ? FormatDuration(ctl.Uptime()) : NA, "Since the server was started, across map changes.");
        ImGui::TableNextRow();
        cell("Network In", live && st.in_bps ? FormatBytesPerSecond(*st.in_bps) : NA, "UDP payload received by the server's transport.");
        cell("Network Out", live && st.out_bps ? FormatBytesPerSecond(*st.out_bps) : NA, "UDP payload sent by the server's transport.");
        cell("Connected Players", live ? Fmt("%d humans (%d connections)", st.humans, st.connections) : NA,
             "Network players in the match; connections includes players still joining.");
        cell("Bots", live ? std::to_string(st.bots) : NA, "Bots in the match.");
        ImGui::EndTable();
    }
    ImGui::Spacing();
    float gw = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
    ImVec2 gs = {gw, ImGui::GetFontSize() * 4.5f};
    std::string o1 = ctl.meter.cpu_percent ? Fmt("CPU %.0f%%", *ctl.meter.cpu_percent) : "CPU";
    Graph("##g1", ctl.history, &Sample::cpu, o1.c_str(), 0, 10, gs);
    ImGui::SameLine();
    std::string o2 = live ? Fmt("Players %d", st.humans + st.bots) : "Players";
    Graph("##g2", ctl.history, &Sample::players, o2.c_str(), 0, 4, gs);
    ImGui::SameLine();
    std::string o3 = live && st.out_bps ? "Out " + FormatBytesPerSecond(*st.out_bps) : "Out";
    Graph("##g3", ctl.history, &Sample::out_kbps, o3.c_str(), 0, 4, gs);
    ImGui::SameLine();
    std::string o4 = live ? Fmt("Tick %.0f Hz", st.tick_hz) : "Tick";
    Graph("##g4", ctl.history, &Sample::tick, o4.c_str(), 0, 35, gs);
    ImGui::TextColored(theme::kTextDim, "The last %d seconds of this run.", int(ctl.history.size()));
    theme::EndPanel();

    if (!ctl.start_problems.empty() && ctl.state == ServerState::Error) {
        ImGui::Spacing();
        theme::BeginPanel("Why the server did not start", {w, 0});
        for (const auto& p : ctl.start_problems) ImGui::BulletText("%s", p.c_str());
        theme::EndPanel();
    }
}

void App::ChangesBar() {
    if (!Dirty()) return;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.33f, 0.31f, 0.17f, 1.0f});
    ImGui::BeginChild("##changes", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGold, "You have unsaved changes.");
    ImGui::SameLine();
    if (theme::Button("Save Changes")) SaveEdits();
    ImGui::SameLine();
    if (theme::Button("Revert")) RevertEdits();
    if (ctl.Running()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::kTextDim, "[live] settings change the running server; [restart] ones need a restart.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::PageConfiguration() {
    ChangesBar();
    float desc_h = ImGui::GetFontSize() * 4.2f;
    if (ImGui::BeginTabBar("##cfgtabs")) {
        const char* tabs[] = {"General", "Gameplay", "Advanced"};
        for (int i = 0; i < 3; i++) {
            ImGuiTabItemFlags f = 0;
            static int requested = -1;
            if (requested < 0) requested = config_tab_;
            if (requested == i) f |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(tabs[i], nullptr, f)) {
                if (requested == i) requested = 99;
                config_tab_ = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::BeginChild("##cfgbody", {0, ImGui::GetContentRegionAvail().y - desc_h - 6}, ImGuiChildFlags_AlwaysUseWindowPadding);
    std::vector<std::string> map_names;
    for (const auto& m : maps_) map_names.push_back(m.name);
    if (std::find(map_names.begin(), map_names.end(), edit_.map) == map_names.end()) map_names.insert(map_names.begin(), edit_.map);
    if (config_tab_ == 0) {
        theme::Heading("Server");
        SettingText("name", "Server Name", "The name players see in the server browser (1-63 characters; no quotes, ; or \\).");
        SettingText("password", "Server Password", "Players must give it to join. Empty: no password. Up to 32 characters, no spaces.", true);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("RCON Password");
        theme::Describe("Not offered: the engine's rcon sends its password in plain text, so this port refuses rcon altogether. Remote administration is by player key (admins.cfg in the server folder).");
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        ImGui::TextColored(theme::kTextDim, "Not used: rcon is refused; remote admins are listed by key in admins.cfg");
        theme::Describe("Not offered: the engine's rcon sends its password in plain text, so this port refuses rcon altogether. Remote administration is by player key (admins.cfg in the server folder).");
        SettingInt("max_players", "Maximum Players", "Player slots, 1-65, shared by humans and bots. The server takes none of them.", 1, 65);
        SettingCombo("map", "Default Map", "The map the server starts on. Maps come from your game folder (and imported maps).", map_names);
        ImGui::Spacing();
        theme::Heading("Network");
        SettingInt("port", "Server Port", "The UDP port players connect to (27015 by default). Open it in your firewall or router for internet players; this tool changes neither.", 1, 65535);
        SettingBool("lan_only", "LAN Only", "On: only addresses of private networks may join (10/8, 172.16/12, 192.168/16, 169.254/16, 127/8).");
        SettingText("master", "Public (Master Servers)", "HOST[:PORT] of master servers to list the server on, separated by commas. Empty: not listed (players connect by address or find it on the LAN).");
    } else if (config_tab_ == 1) {
        theme::Heading("Match");
        SettingCombo("game_mode", "Game Mode", "Classic: rounds and objectives. Deathmatch: respawns, no rounds. Gun Game: a new weapon for each kill.",
                     {"classic", "deathmatch", "gungame"}, {"Classic", "Deathmatch", "Gun Game"});
        SettingFloat("round_time", "Round Time", "Minutes a round lasts, 1-9.", 1, 9, "%.0f");
        SettingFloat("freeze_time", "Freeze Time", "Seconds players are frozen at a round's start, 0-60.", 0, 60, "%.0f");
        SettingFloat("buy_time", "Buy Time", "Minutes the buy zone stays open, 0.25-9.", 0.25f, 9, "%.2f");
        SettingInt("start_money", "Starting Money", "Money at the start, 800-16000.", 800, 16000);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Maximum Money");
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        ImGui::TextColored(theme::kTextDim, "16000 (the game's own limit, not a setting)");
        SettingBool("friendly_fire", "Friendly Fire", "On: players can hurt their own team.");
        SettingBool("player_collision", "Player Collision", "Off: players and bots pass through each other (useful in crowded matches).");
        SettingBool("flashlight", "Flashlights", "Players may use their flashlights.");
        SettingInt("gravity", "Gravity", "The match's gravity, 50-4000 (800 is the original).", 50, 4000);
        ImGui::Spacing();
        theme::Heading("Round Win Conditions");
        SettingInt("max_rounds", "Max Rounds", "Rounds before the map ends, 0 for no limit.", 0, 999);
        SettingInt("time_limit", "Time Limit", "Minutes before the map ends, 0 for no limit.", 0, 999);
        SettingInt("frag_limit", "Frag Limit", "Deathmatch: the match ends when a player reaches this many frags, 0 for none.", 0, 999);
        SettingText("gungame_order", "Gun Game Weapons", "Gun Game's weapons in order, comma separated (empty: the default ladder).");
        ImGui::Spacing();
        theme::Heading("Not available in this build");
        ImGui::TextColored(theme::kTextDim, "Team Balance / Auto Team Balance: the server has no setting for them yet, so they are not offered.");
    } else {
        theme::Heading("Server");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Server Tick Rate");
        theme::Describe("The game's simulation runs at a fixed 30 Hz, as on the console. It is shown on the Overview, measured.");
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        ImGui::TextColored(theme::kTextDim, "30 Hz, fixed (the game's simulation rate)");
        SettingInt("snapshot_rate", "Snapshot Rate", "The most snapshots a second each player gets (sv_maxupdaterate), 10-100. The tick is 30, so more sends no more.", 10, 100);
        ImGui::Spacing();
        theme::Heading("Bots");
        SettingInt("bots", "Bots", "Bots kept in the match (bot_quota), 0 to Maximum Players. The Bots page has more.", 0, 65);
        SettingCombo("bot_difficulty", "Bot Difficulty", "How well bots play.", {"0", "1", "2", "3"}, {"Easy", "Normal", "Hard", "Expert"});
        ImGui::Spacing();
        theme::Heading("Map Rotation");
        SettingText("map_rotation", "Rotation", "Maps played in turn when a match ends, comma separated (empty: the same map again). The Maps page edits it.");
        ImGui::Spacing();
        theme::Heading("Crash Recovery (this tool)");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Restart on Crash");
        const char* rdesc = "When the server ends unexpectedly, start it again after a delay that doubles each time, at most the given number of times within the window. Off: you are asked.";
        theme::Describe(rdesc);
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        bool changed = ImGui::Checkbox("##autorestart", &ctl.restart.enabled);
        theme::Describe(rdesc);
        if (ctl.restart.enabled) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            ImGui::SameLine();
            changed |= ImGui::SliderInt("attempts##ra", &ctl.restart.max_attempts, 1, 10);
            theme::Describe(rdesc);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            float window_min = float(ctl.restart.window_s / 60), delay = float(ctl.restart.base_delay_s);
            if (ImGui::SliderFloat("min window##rw", &window_min, 1, 120, "%.0f")) ctl.restart.window_s = window_min * 60, changed = true;
            theme::Describe(rdesc);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            if (ImGui::SliderFloat("s first delay##rd", &delay, 1, 60, "%.0f")) ctl.restart.base_delay_s = delay, changed = true;
            theme::Describe(rdesc);
        }
        if (changed) ctl.SaveProfile();
        ImGui::Spacing();
        theme::Heading("Logging");
        ImGui::TextColored(theme::kTextDim, "The server keeps its last 30 runs (logs/server_NNN.log) and the administration log in two files of at most 1 MB.");
        ImGui::TextColored(theme::kTextDim, "Logs page: live output, earlier sessions, search, export.");
    }
    ImGui::EndChild();
    // the description box
    theme::BeginPanel("Description", {0, desc_h});
    ImGui::PushTextWrapPos(0);
    const char* d = theme::TakeDescription();
    ImGui::TextUnformatted(*d ? d : "Point at a setting to see what it does.");
    ImGui::PopTextWrapPos();
    theme::EndPanel();
}

void App::PageMaps() {
    ChangesBar();
    const Status& st = ctl.status;
    bool live = st.valid && ctl.Running();
    std::vector<std::string> rot = SplitList(edit_.map_rotation);
    float w = ImGui::GetContentRegionAvail().x;
    float col = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    float h = ImGui::GetContentRegionAvail().y;

    theme::BeginPanel("Installed Maps", {col, h}, false);
    ImGui::TextColored(theme::kTextDim, "%zu maps in %s", maps_.size(), ctl.game_dir.empty() ? "(no game folder)" : ctl.game_dir.c_str());
    if (ImGui::BeginTable("##maps", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                          {0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.2f})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("In Rotation", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& m : maps_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool cur = live && st.map == m.name;
            std::string label = m.name + (cur ? "  (current)" : "") + (m.name == edit_.map ? "  (start)" : "");
            if (ImGui::Selectable(label.c_str(), selected_map_ == m.name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                selected_map_ = m.name;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (std::find(rot.begin(), rot.end(), m.name) == rot.end()) rot.push_back(m.name), SetValue("map_rotation", JoinList(rot));
            }
            ImGui::TableNextColumn();
            bool in = std::find(rot.begin(), rot.end(), m.name) != rot.end();
            ImGui::PushID(m.name.c_str());
            if (ImGui::Checkbox("##in", &in)) {
                if (in) rot.push_back(m.name);
                else rot.erase(std::remove(rot.begin(), rot.end(), m.name), rot.end());
                SetValue("map_rotation", rot.empty() ? std::string("none") : JoinList(rot));
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::kTextDim, "%s", m.imported ? "imported" : "game");
        }
        ImGui::EndTable();
    }
    bool sel = !selected_map_.empty();
    if (theme::Button("Change Map Now", {0, 0}, sel && live)) show_change_map_ = true;
    theme::Describe("Changes the running server to the selected map. Players are told and rejoin by themselves.");
    ImGui::SameLine();
    if (theme::Button("Set as Starting Map", {0, 0}, sel)) SetValue("map", selected_map_);
    theme::Describe("The map the server starts on (saved with the profile).");
    ImGui::SameLine();
    if (theme::Button("Refresh", {0, 0})) RefreshMaps();
    theme::EndPanel();
    ImGui::SameLine();

    theme::BeginPanel("Map Rotation", {col, h}, false);
    if (ImGui::BeginTable("##cn", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        theme::Row("Current Map", "%s", live ? st.map.c_str() : "- (server offline)");
        theme::Row("Next Map", "%s", live ? (st.next_map.empty() ? "the same map (no rotation)" : st.next_map.c_str()) : "-");
        theme::Row("Starting Map", "%s", edit_.map.c_str());
        ImGui::EndTable();
    }
    ImGui::TextColored(theme::kTextDim, "Played in this order when a match ends (time or round limit).");
    ImGui::BeginChild("##rot", {0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.4f}, ImGuiChildFlags_Border);
    if (rot.empty()) ImGui::TextColored(theme::kTextDim, "No rotation: the same map is played again.\nTick maps on the left to add them.");
    for (int i = 0; i < int(rot.size()); i++) {
        std::string label = std::to_string(i + 1) + ".  " + rot[size_t(i)];
        if (ImGui::Selectable(label.c_str(), rotation_selected_ == i)) rotation_selected_ = i;
    }
    ImGui::EndChild();
    bool has = rotation_selected_ >= 0 && rotation_selected_ < int(rot.size());
    auto apply = [&] { SetValue("map_rotation", rot.empty() ? std::string("none") : JoinList(rot)); };
    if (theme::Button("Move Up", {0, 0}, has && rotation_selected_ > 0)) {
        std::swap(rot[size_t(rotation_selected_)], rot[size_t(rotation_selected_ - 1)]);
        rotation_selected_--, apply();
    }
    ImGui::SameLine();
    if (theme::Button("Move Down", {0, 0}, has && rotation_selected_ + 1 < int(rot.size()))) {
        std::swap(rot[size_t(rotation_selected_)], rot[size_t(rotation_selected_ + 1)]);
        rotation_selected_++, apply();
    }
    ImGui::SameLine();
    if (theme::Button("Remove", {0, 0}, has)) {
        rot.erase(rot.begin() + rotation_selected_);
        rotation_selected_ = -1, apply();
    }
    ImGui::SameLine();
    if (theme::Button("Clear", {0, 0}, !rot.empty())) rot.clear(), rotation_selected_ = -1, apply();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::InputTextWithHint("##rname", "rotation name", rotation_name_, sizeof(rotation_name_));
    ImGui::SameLine();
    if (theme::Button("Save Rotation", {0, 0}, ValidProfileName(rotation_name_) && !rot.empty())) {
        if (SaveRotation(ctl.paths, rotation_name_, rot)) ctl.ToolLine(std::string("Rotation saved: ") + rotation_name_);
    }
    theme::Describe("Saves this list under the name (DATA/rotations/NAME.txt) to load it in any profile.");
    ImGui::SameLine();
    auto saved = ListRotations(ctl.paths);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("##load", "Load Rotation...")) {
        for (const auto& r : saved)
            if (ImGui::Selectable(r.c_str())) {
                std::vector<std::string> loaded;
                if (LoadRotation(ctl.paths, r, loaded)) {
                    std::vector<std::string> ok;
                    for (const auto& m : loaded)
                        if (std::any_of(maps_.begin(), maps_.end(), [&](const MapEntry& e) { return e.name == m; })) ok.push_back(m);
                    if (ok.size() != loaded.size())
                        Message("Load Rotation", std::to_string(loaded.size() - ok.size()) + " map(s) of the rotation are not in this game folder and were left out.");
                    SetValue("map_rotation", ok.empty() ? std::string("none") : JoinList(ok));
                    std::snprintf(rotation_name_, sizeof(rotation_name_), "%s", r.c_str());
                }
            }
        if (saved.empty()) ImGui::TextColored(theme::kTextDim, "(none saved)");
        ImGui::EndCombo();
    }
    theme::EndPanel();
}

void App::PagePlayers() {
    const Status& st = ctl.status;
    bool live = st.valid && ctl.Running();
    if (!ctl.Running()) {
        ImGui::TextColored(theme::kTextDim, "The server is offline. Players are listed here while it runs.");
        return;
    }
    if (!live) {
        ImGui::TextColored(theme::kTextDim, "Waiting for the server's first status...");
        return;
    }
    ImGui::TextColored(theme::kTextDim, "%d humans and %d bots in %d slots on %s. Updated every second.", st.humans, st.bots,
                       st.max_players, st.map.c_str());
    float detail_h = ImGui::GetFontSize() * 10.5f;
    const PlayerRow* sel = nullptr;
    if (ImGui::BeginTable("##players", 8,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Sortable |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          {0, ImGui::GetContentRegionAvail().y - detail_h})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Deaths", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        std::vector<const PlayerRow*> rows;
        for (const auto& p : st.players) rows.push_back(&p);
        if (ImGuiTableSortSpecs* s = ImGui::TableGetSortSpecs(); s && s->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& sp = s->Specs[0];
            auto key = [&](const PlayerRow* p) -> double {
                switch (sp.ColumnIndex) {
                case 2: return p->team == "CT" ? 2 : p->team == "T" ? 1 : 0;
                case 3: return p->score;
                case 4: return p->deaths;
                case 5: return p->ping.value_or(-1);
                case 6: return p->bot;
                case 7: return p->seconds;
                default: return p->slot;
                }
            };
            std::stable_sort(rows.begin(), rows.end(), [&](const PlayerRow* a, const PlayerRow* b) {
                if (sp.ColumnIndex == 1) return sp.SortDirection == ImGuiSortDirection_Ascending ? a->name < b->name : a->name > b->name;
                return sp.SortDirection == ImGuiSortDirection_Ascending ? key(a) < key(b) : key(a) > key(b);
            });
        }
        for (const PlayerRow* p : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(p->slot).c_str(), selected_slot_ == p->slot, ImGuiSelectableFlags_SpanAllColumns))
                selected_slot_ = p->slot;
            if (selected_slot_ == p->slot) sel = p;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p->name.c_str());
            ImGui::TableNextColumn();
            ImVec4 tc = p->team == "CT" ? ImVec4{0.55f, 0.72f, 0.95f, 1} : p->team == "T" ? ImVec4{0.95f, 0.66f, 0.45f, 1} : theme::kTextDim;
            ImGui::TextColored(tc, "%s", p->team.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", p->score);
            ImGui::TableNextColumn();
            ImGui::Text("%d", p->deaths);
            ImGui::TableNextColumn();
            if (p->ping) ImGui::Text("%d", *p->ping);
            else ImGui::TextColored(theme::kTextDim, "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(p->bot ? theme::kTextDim : theme::kText, "%s", p->bot ? "Bot" : "Human");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatDuration(p->seconds).c_str());
        }
        ImGui::EndTable();
    }
    theme::BeginPanel(sel ? ("Player: " + sel->name).c_str() : "Player", {0, 0});
    if (!sel) {
        ImGui::TextColored(theme::kTextDim, "Select a player to see their connection and act on them.");
    } else if (sel->bot) {
        ImGui::Text("Slot %d, bot on %s. Bots are managed on the Bots page (their count and difficulty).", sel->slot, sel->team.c_str());
    } else {
        ImGui::Text("Slot %d   Address %s   Ping %s   Connected %s", sel->slot, sel->address.c_str(),
                    sel->ping ? (std::to_string(*sel->ping) + " ms").c_str() : "-", FormatDuration(sel->seconds).c_str());
        ImGui::TextColored(theme::kTextDim, "Identity key %s", sel->key.empty() ? "(none)" : sel->key.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Key")) ImGui::SetClipboardText(sel->key.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##reason", "reason (optional)", kick_reason_, sizeof(kick_reason_));
        ImGui::SameLine();
        if (theme::Button("Kick")) ctl.Kick(sel->slot, kick_reason_);
        theme::Describe("Disconnects the player with the reason. They can join again.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::Combo("##banfor", &ban_minutes_index_, kBanLabels, 6);
        ImGui::SameLine();
        if (theme::Button("Ban")) ctl.Ban(sel->slot, kBanMinutes[ban_minutes_index_]);
        theme::Describe("Kicks the player and refuses their identity key (or address, without one) for the time chosen. Kept in bans.txt in the server folder.");
        ImGui::SameLine();
        theme::Button("Move to Other Team", {0, 0}, false);
        theme::Describe("Not available: the game has no server command to move a player to another team.");
    }
    theme::EndPanel();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::InputTextWithHint("##unban", "key or IP address to unban", unban_text_, sizeof(unban_text_));
    ImGui::SameLine();
    if (theme::Button("Unban", {0, 0}, unban_text_[0] != 0)) ctl.Unban(unban_text_), unban_text_[0] = 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    bool enter = ImGui::InputTextWithHint("##say", "message to all players", say_text_, sizeof(say_text_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((theme::Button("Say", {0, 0}, say_text_[0] != 0) || enter) && say_text_[0]) ctl.Say(say_text_), say_text_[0] = 0;
}

void App::PageBots() {
    ChangesBar();
    const Status& st = ctl.status;
    bool live = st.valid && ctl.Running();
    theme::BeginPanel("Bots", {0, 0});
    if (ImGui::BeginTable("##botinfo", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        theme::Row("Bots in the match", "%s", live ? std::to_string(st.bots).c_str() : "- (server offline)");
        theme::Row("Humans in the match", "%s", live ? std::to_string(st.humans).c_str() : "-");
        theme::Row("Player slots", "%d (humans and bots share them; up to %d bots)", edit_.max_players, edit_.max_players);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bot Quota");
    const char* qdesc = "The number of bots kept in the match (bot_quota), 0 to Maximum Players. The game sets its bots when a match starts, so a change applies from the next map; Apply Now reloads the current map.";
    theme::Describe(qdesc);
    ImGui::SameLine(ImGui::GetFontSize() * 12);
    if (theme::Button("Remove Bot", {0, 0}, edit_.bots > 0)) SetValue("bots", std::to_string(edit_.bots - 1));
    theme::Describe(qdesc);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    int b = edit_.bots;
    if (ImGui::SliderInt("##bots", &b, 0, edit_.max_players)) SetValue("bots", std::to_string(b));
    theme::Describe(qdesc);
    ImGui::SameLine();
    if (theme::Button("Add Bot", {0, 0}, edit_.bots < edit_.max_players)) SetValue("bots", std::to_string(edit_.bots + 1));
    theme::Describe(qdesc);
    ImGui::SameLine();
    if (theme::Button("Remove All Bots", {0, 0}, edit_.bots > 0)) SetValue("bots", "0");
    ApplyBadge("bots");
    SettingCombo("bot_difficulty", "Bot Difficulty", "How well bots play, from the next map.", {"0", "1", "2", "3"},
                 {"Easy", "Normal", "Hard", "Expert"});
    ImGui::Spacing();
    bool differs = live && (edit_.bots != st.bots || edit_.bot_difficulty != ctl.config.bot_difficulty);
    if (theme::Button("Apply Now (Reload Map)", {0, 0}, live)) {
        if (Dirty()) SaveEdits();
        ctl.SetBotsNextMap(edit_.bots);
        ctl.SetBotDifficultyNextMap(edit_.bot_difficulty);
        ctl.ChangeMap(st.map);
    }
    theme::Describe("Saves the bot settings and reloads the current map with them. Players are told and rejoin by themselves.");
    if (differs) {
        ImGui::SameLine();
        ImGui::TextColored(theme::kWarn, "The match has %d bots; %d from the next map.", st.bots, edit_.bots);
    }
    theme::EndPanel();
    ImGui::Spacing();
    theme::BeginPanel("Not available in this build", {0, 0});
    ImGui::TextWrapped("Bot team selection, automatic filling (bots leaving as humans join) and separate quotas per team are "
                       "not offered: the game's bots are set once when a match starts and the server has no setting for "
                       "them. The quota above is the total, placed on both teams by the game.");
    theme::EndPanel();
}

}  // namespace tool
