// tests/pc_mapsearch_filter_test.py builds and runs this: src/game/pc_mapsearch_filter.h on its own
#include <chrono>
#include <cstdio>
#include <string>

#include "game/pc_mapsearch_filter.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    using namespace mapsearch;
    // the title's maps, a DLC map (pc_dlc.cpp's rows) and a custom one from the game folder
    const std::vector<Entry> maps = Index({{"Airstrip", "de_airstrip"},
                                           {"Dust", "de_dust"},
                                           {"Dust II", "de_dust2"},
                                           {"Italy", "cs_italy"},
                                           {"Fastline (DLC)", "de_fastline"},
                                           {"aim_Map Custom", "aim_custommap"}});
    auto names = [&](const std::vector<size_t>& shown) {
        std::string s;
        for (size_t i : shown) s += maps[i].value + " ";
        return s;
    };
    Check(names(Filter(maps, "dust")) == "de_dust de_dust2 ", "partial: dust shows de_dust and de_dust2");
    Check(names(Filter(maps, "DUST")) == "de_dust de_dust2 ", "case-insensitive");
    Check(names(Filter(maps, "Dust II")) == "de_dust2 ", "the display name");
    Check(names(Filter(maps, "de_dust2")) == "de_dust2 ", "the file name");
    Check(Filter(maps, "").size() == maps.size() && Filter(maps, "   ").size() == maps.size(), "empty: every map");
    Check(Filter(maps, "  ital ").size() == 1, "spaces at the ends do not count");
    Check(Filter(maps, "zzz").empty(), "no results");
    Check(names(Filter(maps, "fastline")) == "de_fastline ", "a DLC map");
    Check(names(Filter(maps, "custom")) == "aim_custommap ", "a custom map");
    Check(names(Filter(maps, "cs_")) == "cs_italy ", "a prefix");
    // the selection across a change of the search
    const std::vector<size_t> dust = Filter(maps, "dust");
    Check(SelectAfter(2, dust) == 1, "the same map kept (de_dust2 now the second row)");
    Check(SelectAfter(0, dust) == 0, "a map filtered out: the first shown");
    Check(SelectAfter(3, Filter(maps, "zzz")) == -1, "no results: none selected");
    Check(SelectAfter(4, Filter(maps, "")) == 4, "back to every map: the same map");
    Check(FirstShown(0, 9, 20, 4) == 6 && FirstShown(10, 3, 20, 4) == 3 && FirstShown(5, 1, 2, 4) == 0,
          "the selection scrolled into view");
    Check(FirstShown(3, -1, 0, 4) == 0, "an empty list from its top");
    // the field's editing
    Field f;
    Check(f.Shown(false) == "Search maps..." && f.Shown(true) == "|", "the placeholder, the caret");
    f.Insert("dst");
    f.Left();
    f.Left();
    f.Insert("u");
    Check(f.text == "dust" && f.cursor == 2, "typed at the caret");
    f.End();
    f.Backspace();
    Check(f.text == "dus", "Backspace");
    f.Home();
    f.Delete();
    Check(f.text == "us" && f.cursor == 0, "Home, Delete");
    f.SelectAll();
    Check(f.Shown(true) == "[us]", "Ctrl+A shows everything selected");
    f.Insert("i");
    Check(f.text == "i", "typing replaces the selection");
    f.SelectAll();
    Check(f.Cut() == "i" && f.text.empty(), "Ctrl+X");
    f.Insert(std::string(40, 'a'));
    Check(f.text.size() == Field::kMax, "the field's length");
    f.Clear();
    f.Insert("de_\x01" "dust\n");
    Check(f.text == "de_dust", "pasted control characters left out");
    // a large list: 5000 maps, filtered on each keystroke within a millisecond or so
    std::vector<std::pair<std::string, std::string>> many;
    for (int i = 0; i < 5000; i++) many.push_back({"Custom Map " + std::to_string(i), "cm_" + std::to_string(i)});
    const std::vector<Entry> big = Index(many);
    auto t0 = std::chrono::steady_clock::now();
    size_t found = 0;
    for (const char* q : {"c", "cu", "cus", "cust", "custom map 4", "custom map 49", "cm_4999"}) found += Filter(big, q).size();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("5000 maps, 7 searches: %.2f ms\n", ms);
    Check(Filter(big, "cm_4999").size() == 1 && Filter(big, "custom map 49").size() == 111, "a large list's results");
    Check(ms < 50, "a large list stays responsive");
    (void)found;
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
