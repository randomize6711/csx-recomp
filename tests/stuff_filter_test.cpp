// tests/stuff_filter_test.py: the stufftext filter (src/game/stuff_filter.h) against what Cbuf_Execute would run
#include <cstdio>
#include <string>
#include <vector>

#include "game/stuff_filter.h"

int g_checks = 0, g_failed = 0;

// the filter's output for `in` must be `want` (what Cbuf_Execute is then given)
void Expect(const std::string& in, const std::string& want, const char* what) {
    std::vector<std::string> dropped;
    std::string got = csx::FilterStuff(in, dropped);
    g_checks++;
    if (got != want) {
        g_failed++;
        std::printf("FAIL %s\n     got  [%s]\n     want [%s]\n", what, got.c_str(), want.c_str());
    } else {
        std::printf("ok   %s\n", what);
    }
}

int main() {
    Expect("echo hello\n", "echo hello\n", "an allowed command is kept");
    Expect("bind w quit\n", "\n", "a command off the list is dropped");
    Expect("echo a;bind w quit;spk x\n", "echo a;;spk x\n", "';' outside quotes splits");
    Expect("echo \"a;b\"\n", "echo \"a;b\"\n", "';' inside quotes does not split");
    Expect("ECHO hi\n", "ECHO hi\n", "names compared without case, as the engine's lookup");
    // the review's bypasses (2026-10-25)
    Expect("echo \"x\nbind w quit\n", "\n\n", "a newline inside quotes still ends the command (Cbuf_Execute 0x3BB94)");
    Expect("echo \"", "", "text without a newline is dropped (the next stufftext would continue it)");
    Expect("echo hi\necho \"", "echo hi\n", "only the unterminated tail is dropped");
    Expect("echo \"a\";bind w quit\n", "echo \"a\";\n", "quotes balanced before a ';'");
    Expect(" bind w quit\n", "\n", "leading spaces do not hide a name");
    Expect("\"bind\" w quit\n", "\n", "a quoted name is not on the list");
    Expect("\x01" "bind w quit\n", "\n", "a control character before a name is not on the list");
    Expect("echo " + std::string(1100, 'a') + "\n", "\n", "a command over 1000 characters is dropped");
    Expect("\n\necho a\n", "\n\necho a\n", "empty lines kept as they are");
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
