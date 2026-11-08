// Helpers shared by the tool's UI files (ui.cpp, ui_pages.cpp, ui_console.cpp).
#pragma once

#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>

#include "imgui.h"
#include "logs.h"
#include "server_process.h"
#include "server/server_config.h"
#include "theme.h"
#include "ui.h"

namespace tool {

namespace uiutil {

enum DialogKind { kDlgGameFolder = 1, kDlgServerExe, kDlgExportConfig, kDlgImportConfig, kDlgExportLog };

inline const int kBanMinutes[] = {5, 30, 60, 1440, 10080, 0};
inline const char* const kBanLabels[] = {"5 minutes", "30 minutes", "1 hour", "1 day", "1 week", "Permanent"};

inline double Now() { return double(SDL_GetTicks()) / 1000.0; }

inline std::map<std::string, std::string> Values(const server::Config& c) {
    std::map<std::string, std::string> m;
    std::istringstream in(server::Describe(c, true));
    for (std::string l; std::getline(in, l);) {
        size_t sp = l.find(' ');
        m[l.substr(0, sp)] = sp == std::string::npos ? std::string() : l.substr(sp + 1);
    }
    return m;
}

inline ImVec4 StateColor(ServerState s) {
    switch (s) {
    case ServerState::Online: return theme::kOnline;
    case ServerState::Starting:
    case ServerState::Stopping: return theme::kWarn;
    case ServerState::Error: return theme::kError;
    default: return theme::kTextDim;
    }
}

inline ImVec4 SeverityColor(Severity s) {
    switch (s) {
    case Severity::Warn: return theme::kWarn;
    case Severity::Error: return theme::kError;
    case Severity::Tool: return theme::kToolText;
    case Severity::Debug: return theme::kTextDim;
    default: return theme::kConsoleText;
    }
}

inline void DialogCallback(void* user, const char* const* files, int) {
    auto* p = static_cast<std::pair<App*, int>*>(user);
    // no picker on this system (files null): the tool falls back (a path field, or the data folder)
    if (!files) p->first->PostDialogResult(-p->second, SDL_GetError());
    else if (files[0]) p->first->PostDialogResult(p->second, files[0]);
    delete p;
}

inline std::string Fmt(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return b;
}

// a centred modal's start (it opens itself the frame its flag is set)
inline bool BeginModal(const char* name, bool* open, float width) {
    if (*open && !ImGui::IsPopupOpen(name)) ImGui::OpenPopup(name);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({width, 0});
    if (!ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        *open = false;
        return false;
    }
    return true;
}

}  // namespace uiutil

}  // namespace tool
