// The tool's window contents (docs/server_tool.md "The application"): menu bar, sidebar, the pages (Overview,
// Configuration, Maps, Players, Bots, Console, Logs), the status bar and the dialogs. Everything shown comes from the
// controller (the server's telemetry and output, the OS's measurements, the files); every action goes through it.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "controller.h"
#include "gamefiles.h"

struct SDL_Window;
struct ImFont;

namespace tool {

enum class Page { Overview, Configuration, Maps, Players, Bots, Console, Logs, Count };
const char* PageName(Page p);

class App {
public:
    Controller ctl;
    Ini settings;            // DATA/tool.ini
    std::string settings_path;
    SDL_Window* window = nullptr;
    ImFont* mono = nullptr;  // the console's font
    bool quit_requested = false;     // File > Exit (after the confirmation)
    bool hide_to_tray = false;       // View > Minimize to Tray (main.cpp hides the window)
    bool tray_available = false;

    void Init();       // after paths and settings: the profile, the game check, the maps
    void Frame();      // the whole window, once per frame
    void Shutdown();   // settings saved
    // the window's close button: true when the tool may close now (otherwise a confirmation is shown)
    bool RequestClose();
    // the tray menu's actions
    void TrayStartStop();
    std::string TrayStatus() const;

    // file dialogs answer on another thread: their results wait here for the next frame
    struct DialogResult {
        int kind = 0;
        std::string path;
    };
    void PostDialogResult(int kind, const std::string& path);

private:
    void MenuBar();
    void Sidebar();
    void StatusBar();
    void Dialogs();
    void PageOverview();
    void PageConfiguration();
    void PageMaps();
    void PagePlayers();
    void PageBots();
    void PageConsole();
    void PageLogs();
    void ChangesBar();
    void GameFolderBanner();

    void SelectProfile(const std::string& name);
    void RefreshMaps();
    void RefreshGameCheck(bool hash);
    void SaveEdits();
    void RevertEdits();
    bool Dirty() const;
    void Message(const std::string& title, const std::string& text);
    void HandleDialogResults();
    void ChooseGameFolder();
    void ChooseServerExe();
    void ExportConfig();
    void ImportConfig();
    void ExportLog();
    void OpenFolder(const std::string& path);

    // settings widgets (validated by csx-server's own rules, server_config.cpp)
    bool SettingText(const char* key, const char* label, const char* desc, bool password = false);
    bool SettingInt(const char* key, const char* label, const char* desc, int lo, int hi);
    bool SettingFloat(const char* key, const char* label, const char* desc, float lo, float hi, const char* fmt);
    bool SettingBool(const char* key, const char* label, const char* desc);
    bool SettingCombo(const char* key, const char* label, const char* desc, const std::vector<std::string>& options,
                      const std::vector<std::string>& labels = {});
    void ApplyBadge(const char* key);
    std::string Value(const char* key) const;
    bool SetValue(const char* key, const std::string& value);

    Page page_ = Page::Overview;
    int config_tab_ = 0;
    server::Config edit_;  // the profile's settings as being edited (saved: ctl.config)
    std::map<std::string, std::string> edit_errors_;
    std::vector<MapEntry> maps_;
    GameCheck game_check_;
    std::string game_dir_input_;
    std::vector<std::string> profiles_;

    // players
    int selected_slot_ = 0;
    char kick_reason_[64] = "";
    int ban_minutes_index_ = 2;
    char unban_text_[80] = "";
    char say_text_[128] = "";
    // maps
    std::string selected_map_;
    int rotation_selected_ = -1;
    char rotation_name_[48] = "";
    // console
    char console_input_[512] = "";
    char console_search_[96] = "";
    std::vector<std::string> console_history_;
    int history_pos_ = -1;
    bool autoscroll_ = true;
    bool show_info_ = true, show_warn_ = true, show_error_ = true, show_tool_ = true;
    uint64_t console_seen_id_ = 0;
    bool console_focus_ = false;
    // logs
    int log_selected_ = 0;  // 0: this session (live), 1..: files
    std::vector<LogFile> log_files_;
    std::vector<std::string> log_lines_;
    std::string log_loaded_;
    char log_search_[96] = "";
    bool log_info_ = true, log_warn_ = true, log_error_ = true;
    double log_list_at_ = -10;
    const std::vector<std::string>* log_lines_export_ = nullptr;  // the lines the Logs page shows (Export writes them)

public:
    // tests and screenshots (--page): show this page
    void SetPage(Page p) { page_ = p; }
    void SetConfigTab(int t) { config_tab_ = t; }
    // dialogs
    bool confirm_stop_ = false, confirm_restart_ = false, confirm_exit_ = false, show_about_ = false;
    bool new_profile_ = false, delete_profile_ = false, rename_dialog_ = false;
    char profile_name_[48] = "";
    std::string message_title_, message_text_;
    bool message_open_ = false;
    bool show_change_map_ = false;
    std::mutex dialog_lock_;
    std::vector<DialogResult> dialog_results_;
    double last_maps_scan_ = -100;
};

}  // namespace tool
