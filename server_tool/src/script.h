// The tool's script mode (csx-server-tool --script FILE): the controller the GUI uses, driven by a text script against
// the real server, for tests (tests/server_tool/*.txt, tests/server_tool_test.py). One command a line:
//   game DIR | exe PATH | profile NAME | set KEY VALUE | save
//   start | stop | restart | kill (SIGKILL the server: a crash) | expect_start_fails
//   wait_online SECONDS | wait_offline SECONDS | wait_state STATE SECONDS | wait_line TEXT... (60 s)
//   wait_status FIELD OP VALUE [SECONDS]   (FIELD: map next_map bots humans players max_players tick_hz state port)
//   expect_status FIELD OP VALUE           (OP: == != >= <= > <)
//   command TEXT... | say TEXT | kick SLOT | ban SLOT MINUTES | map NAME | rotation a,b|- | bots N | difficulty N
//   restart_policy on|off ATTEMPTS BASE_DELAY | expect_line TEXT... | expect_no_line TEXT... | sleep SECONDS
//   expect_meter (CPU and memory were measured) | expect_exit_code N | print
// Lines starting with # are comments. Exit code 0 when every step passed.
#pragma once

#include <string>

#include "storage.h"

namespace tool {
int RunScript(const std::string& path, const Paths& paths, const std::string& tool_dir);
}
