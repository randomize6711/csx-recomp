// The saved player profiles (src/common/profiles.h) on their own (tests/profiles_test.py): created with a stable id,
// listed, renamed, two with the same name kept apart, their scoped settings saved and read back, unscoped and bad
// values not kept, deleted; ids that could name a path refused; everything still there for a new Store (a restart).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/profiles.h"

namespace {
int g_failed = 0, g_checks = 0;
void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::string dir = argv[1];
    using profiles::Store;
    Check(profiles::NameValid("Ace") && profiles::NameValid("Player 2") && profiles::NameValid(std::string(20, 'x')) &&
              !profiles::NameValid(std::string(21, 'x')) && !profiles::NameValid("") && !profiles::NameValid("   ") &&
              !profiles::NameValid("a\"b") && !profiles::NameValid("a;b") && !profiles::NameValid("a\\b"),
          "names: 1-20 of the allowed characters");
    Check(profiles::IdValid("0123456789abcdef") && !profiles::IdValid("../../etc/passwd") && !profiles::IdValid("0123456789ABCDEF") &&
              !profiles::IdValid("0123"),
          "ids: 16 lower-case hex digits only");
    Check(profiles::Scoped("mouse_sensitivity") && profiles::Scoped("fov") && profiles::Scoped("bind.attack") &&
              profiles::Scoped("pad.invert") && !profiles::Scoped("vsync") && !profiles::Scoped("resolution") &&
              !profiles::Scoped("name"),
          "scoped settings: mouse, FOV, bindings, the title's controller options; not display or name");
    Store s(dir);
    Check(s.List().empty(), "a fresh folder has no profiles");
    std::string err;
    std::string a = s.Create("Ace", {{"mouse_sensitivity", "4"}, {"fov", "100"}, {"vsync", "0"}}, err);
    std::string b = s.Create("Ace", {{"mouse_sensitivity", "2"}}, err);
    std::string bad = s.Create("a;quit", {}, err);
    Check(profiles::IdValid(a) && profiles::IdValid(b) && a != b, "two profiles with one name: two ids");
    Check(bad.empty() && !err.empty(), "an invalid name is refused with a message");
    Check(s.List().size() == 2 && s.Name(a) == "Ace" && s.Name(b) == "Ace", "both listed under their name");
    auto va = s.Values(a), vb = s.Values(b);
    Check(va["mouse_sensitivity"] == "4" && va["fov"] == "100" && !va.count("vsync") && vb["mouse_sensitivity"] == "2" &&
              !vb.count("fov"),
          "each keeps its own scoped values, not the PC's other settings");
    Check(s.Rename(b, "Bravo", err) && s.Name(b) == "Bravo" && s.Name(a) == "Ace", "a rename changes that profile only");
    Check(!s.Rename(b, "x\ny", err) && s.Name(b) == "Bravo", "a bad rename keeps the old name");
    va["bind.attack"] = "MOUSE1";
    va["mouse_invert"] = "1\nname = evil";
    Check(s.Save(a, va, err), "values saved");
    Store again(dir);  // a restart
    auto r = again.Values(a);
    Check(r["bind.attack"] == "MOUSE1" && !r.count("mouse_invert") && again.Name(a) == "Ace",
          "read back after a restart; a value with a line break not written");
    Check(!again.Exists("0000000000000000") && again.Name("../profiles") == "", "unknown or path-like ids are no profile");
    Check(again.Remove(b, err) && again.List().size() == 1 && again.List()[0].id == a, "delete removes that profile only");
    Check(!again.Remove(b, err), "deleting it twice fails");
    // a damaged file (no name line) is not listed
    std::ofstream(std::filesystem::path(dir) / "profiles" / "00000000000000aa.ini") << "fov = 90\n";
    Check(again.List().size() == 1, "a file without a valid name is not a profile");
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
