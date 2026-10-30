// Button prompts (N5D): the menus show the buttons of the controller in use, in one of nine styles (PC keyboard and
// mouse, the original Xbox controller, Xbox 360, Xbox Series X|S, PlayStation 4, DualSense, Nintendo Switch, Steam
// Controller, Wii Remote and Nunchuk), chosen by PC Options > Controller > Button Prompts: Automatic (the device last
// used: linux/pad_style.h names a controller's style from SDL's type, its vendor and product) or one of them.
//   - Artwork: tools/make_prompts.py draws every style's buttons in the original legends' manner into one XPR1
//     (CSXP_<style>_<slot>) plus the slots the menus draw (CSXP_<slot>); the window copies it onto the run's hard disk
//     (Z:\media\textures\csx_prompts.xpr) and a hook after the launcher's own textures (0xAF69F) loads it into the
//     same registry (0xC4E80, registry 0x553E78), where the menus' images are looked up by name (0xC4C20).
//   - A style is applied by copying its glyphs' pixels into the CSXP_<slot> textures (their data, gpu::Describe) and
//     marking them changed (gpu::MarkDirty: the renderer uploads them again): no menu text changes.
//   - The legends: the title's legend strips (launcher.xpr's Cancel_Accept_Reset ...: the Xbox buttons drawn into one
//     1024x128 picture) are drawn transparent, where a screen uses one, and each button is a glyph and the title's
//     own text ("RESET", "ACCEPT", "CANCEL", "CONFIGURE BOTS"), laid out right to left by the words' widths
//     (ExtendPrompts; PcLegendSlots gives pc_mouse.cpp the same places for clicks on the bar). Up to five buttons; a
//     strip with a word not known here stays as it was; with no artwork on the disk every strip stays as it was.
//   - Text: hints that name a button (the PC screens' "Y Details") take the style's name for it (PcPromptName): the
//     text fallback where no picture is drawn.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "game/pc_prompts.h"

float PcTextUnits(const std::string& s);  // pc_menu.cpp

namespace {

constexpr uint32_t kRegistry = 0x553E78;    // the menus' named textures
constexpr uint32_t kLoadXpr = 0xC4E80;      // (registry, path, 0, 0): an XPR's textures into it
constexpr uint32_t kFindTexture = 0xC4C20;  // edi the registry, the name pushed: its D3D texture, 0 for none
constexpr uint32_t kMalloc = 0x22D86A;
constexpr char kGuestPath[] = "Z:\\media\\textures\\csx_prompts.xpr";

const char* const kStyles[] = {"pc", "xbox", "x360", "series", "ps", "dualsense", "switch", "steam", "wii"};
constexpr int kStyleCount = 9;
const char* const kSlots[] = {"a",  "b",  "x",  "y",  "back", "start", "lb",  "rb",
                              "lt", "rt", "ls", "rs", "dup",  "ddown", "dleft", "dright"};

std::atomic<int> g_device{1};  // the device last used (the window's), Xbox's until one is
bool g_loaded = false;
int g_applied = -1;

uint32_t GuestText(X86Context& ctx, const std::string& s) {
    static uint32_t block = 0;  // 64 characters, reused
    if (!block) block = kernel::CallGuest(ctx, kMalloc, {64});
    if (!block || s.size() > 63) return 0;
    std::memcpy(kernel::HostPtr(block), s.c_str(), s.size() + 1);
    return block;
}

uint32_t FindTexture(X86Context& ctx, const std::string& name) {
    uint32_t text = GuestText(ctx, name), edi = ctx.edi;
    if (!text) return 0;
    ctx.edi = kRegistry;
    uint32_t t = kernel::CallGuest(ctx, kFindTexture, {text});
    ctx.edi = edi;
    return t;
}

int StyleIndex(const std::string& name) {
    for (int i = 0; i < kStyleCount; i++)
        if (name == kStyles[i]) return i;
    return -1;
}

int Wanted() {
    static unsigned seen = ~0u;
    static int chosen = -1;  // -1: Automatic
    if (pc::Version() != seen) {
        seen = pc::Version();
        chosen = StyleIndex(pc::Get("prompt_style", "auto"));
    }
    return chosen >= 0 ? chosen : g_device.load();
}

// the button names a style's controller prints (the text fallback, and the hints' words)
const char* NameFor(int style, const std::string& slot) {
    struct Row {
        const char *slot, *names[kStyleCount];  // pc xbox x360 series ps dualsense switch steam wii
    };
    static const Row kRows[] = {
        {"a", {"Space", "A", "A", "A", "Cross", "Cross", "B", "A", "A"}},
        {"b", {"Esc", "B", "B", "B", "Circle", "Circle", "A", "B", "B"}},
        {"x", {"R", "X", "X", "X", "Square", "Square", "Y", "X", "1"}},
        {"y", {"Q", "Y", "Y", "Y", "Triangle", "Triangle", "X", "Y", "2"}},
        {"back", {"Tab", "Back", "Back", "View", "Share", "Create", "-", "Back", "-"}},
        {"start", {"Enter", "Start", "Start", "Menu", "Options", "Options", "+", "Start", "+"}},
        {"lb", {"1", "White", "LB", "LB", "L1", "L1", "L", "LB", "C"}},
        {"rb", {"2", "Black", "RB", "RB", "R1", "R1", "R", "RB", "Z"}},
        {"lt", {"Page Up", "L", "LT", "LT", "L2", "L2", "ZL", "LT", "LT"}},
        {"rt", {"Page Down", "R", "RT", "RT", "R2", "R2", "ZR", "RT", "RT"}},
    };
    for (const Row& r : kRows)
        if (slot == r.slot) return r.names[std::clamp(style, 0, kStyleCount - 1)];
    return slot.c_str();
}

// the legends: a strip's name's words as buttons (the strip shows them right to left in this order), each with the
// button the screens' OnButtonDown handlers give it (button: the pad's analog button index, A B X Y Black White)
struct Word {
    const char *word, *slot, *label;
    int button;
};
const Word kWords[] = {
    {"Accept", "a", "ACCEPT", 0},          {"Back", "b", "BACK", 1},
    {"Cancel", "b", "CANCEL", 1},          {"Continue", "a", "CONTINUE", 0},
    {"Select", "a", "SELECT", 0},          {"Reset", "x", "RESET", 2},
    {"Overwrite", "a", "OVERWRITE", 0},    {"Configure", "y", "CONFIGURE BOTS", 3},  // Custom Game's Match Options
    {"ControllerLayout", "y", "CONTROLLER LAYOUT", 3},                               // Controller Options
    {"Join", "a", "JOIN", 0},              {"RefreshResults", "x", "REFRESH", 2},    // the server lists
    {"HostAMatch", "y", "HOST A MATCH", 3},
    {"Remove", "x", "REMOVE", 2},          {"New", "y", "NEW", 3},                   // Change Profile
    {"Rename", "lb", "RENAME", 5},
    {"REMOVE", "a", "REMOVE", 0},  // the profile delete confirmations (Back_REMOVE: A removes)
    {"Host", "a", "HOST", 0},
};

bool LegendItems(const std::string& name, std::vector<const Word*>& out) {
    out.clear();
    size_t at = 0;
    while (at <= name.size()) {
        size_t end = name.find('_', at);
        std::string w = name.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const Word* found = nullptr;
        for (const Word& k : kWords)
            if (w == k.word) found = &k;
        if (!found) return false;
        out.push_back(found);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return !out.empty() && out.size() <= 5;
}

// the legend's layout: right to left from the bar's right edge, a 24 x 24 glyph, the word beside it at the text
// scale, a gap; narrower text when the words would reach past the bar's left (PcLegendSlots: the mouse's slots)
constexpr float kRight = 586, kLeftLimit = 48, kGlyph = 24, kGlyphGap = 4, kItemGap = 16;
struct Placed {
    float glyph, text, end, scale;
};
std::vector<Placed> Layout(const std::vector<const Word*>& items) {
    std::vector<Placed> out;
    for (float scale : {0.7f, 0.6f, 0.5f}) {
        out.clear();
        float right = kRight;
        for (const Word* w : items) {
            float width = PcTextUnits(w->label) * 14.3f * scale;
            float glyph = right - width - kGlyphGap - kGlyph;
            out.push_back({glyph, glyph + kGlyph + kGlyphGap, right, scale});
            right = glyph - kItemGap;
        }
        if (out.back().glyph >= kLeftLimit) break;
    }
    return out;
}

size_t Count(const std::string& text, size_t from, size_t to, const char* what) {
    size_t n = 0;
    for (size_t at = text.find(what, from); at != std::string::npos && at < to; at = text.find(what, at + 1)) n++;
    return n;
}

}  // namespace

namespace csx {
void SetPromptDevice(int style) {
    if (style >= 0 && style < kStyleCount) g_device = style;
}
int PromptDevice() { return g_device; }
}  // namespace csx

// a button's name in the style shown (the hints' words; "a" ... "rt")
std::string PcPromptName(const std::string& slot) { return NameFor(g_applied >= 0 ? g_applied : Wanted(), slot); }

// a legend strip's buttons as ExtendPrompts draws them (left, right, the pad's button), rightmost first; none for a
// strip whose words are not all known
std::vector<PcLegendSlot> PcLegendSlots(const std::string& strip) {
    std::vector<const Word*> items;
    std::vector<PcLegendSlot> out;
    if (!LegendItems(strip, items)) return out;
    std::vector<Placed> placed = Layout(items);
    for (size_t k = 0; k < items.size(); k++) out.push_back({placed[k].glyph - 4, placed[k].end + 6, items[k]->button});
    return out;
}

// [[hook]] at 0xAF69F (config/game.toml): the launcher's own textures just loaded into the menus' registry; the
// prompts' artwork goes in beside them
void PcPromptsLoad(X86Context& ctx, uint8_t*) {
    uint32_t path = GuestText(ctx, kGuestPath);
    if (!path) return;
    kernel::CallGuest(ctx, kLoadXpr, {kRegistry, path, 0, 0});
    g_loaded = FindTexture(ctx, "CSXP_a") != 0;
    g_applied = -1;
    LOGI("pc prompts: artwork %s", g_loaded ? "loaded" : "not found (the legends' text stands alone)");
}

// on the game thread every frame (pc_menu.cpp's PcMenuFrame): the style wanted applied when it changes
void PcPromptsFrame(X86Context& ctx) {
    int s = Wanted();
    if (!g_loaded || s == g_applied) return;
    int copied = 0;
    for (const char* slot : kSlots) {
        uint32_t dst = FindTexture(ctx, std::string("CSXP_") + slot);
        uint32_t src = FindTexture(ctx, std::string("CSXP_") + kStyles[s] + "_" + slot);
        gpu::TextureDesc a, b;
        if (!dst || !src || !gpu::Describe(dst, a) || !gpu::Describe(src, b)) continue;
        uint32_t n = std::min(gpu::TotalSize(a), gpu::TotalSize(b));
        std::memcpy(kernel::HostPtr(a.data), kernel::HostPtr(b.data), n);
        gpu::MarkDirty(dst);
        copied++;
    }
    LOGI("pc prompts: %s (%s, %d buttons)", kStyles[s], pc::Get("prompt_style", "auto") == "auto" ? "automatic" : "chosen",
         copied);
    g_applied = s;
}

// the menu text: every legend strip whose buttons are known, on a screen with room for the glyphs, as glyphs and
// the title's own text; the text as it was without the artwork on the disk
std::string ExtendPrompts(const std::string& text) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(app::ExeDir() / "hdd" / "Z" / "media" / "textures" / "csx_prompts.xpr", ec) &&
        !std::filesystem::is_regular_file(app::ExeDir() / ("hdd" + std::to_string(app::Instance())) / "Z" / "media" /
                                              "textures" / "csx_prompts.xpr",
                                          ec))
        return text;
    std::string out = text;
    int replaced = 0, kept = 0;
    std::vector<const Word*> items;
    for (size_t at = out.rfind("BackgroundImage \""); at != std::string::npos;
         at = at ? out.rfind("BackgroundImage \"", at - 1) : std::string::npos) {
        size_t q1 = at + std::strlen("BackgroundImage \""), q2 = out.find('"', q1);
        if (q2 == std::string::npos) continue;
        std::string spec = out.substr(q1, q2 - q1);
        std::string name = spec.substr(0, spec.find_first_of(" \t"));
        // a legend strip: the bottom's 640 x 69 (its other numbers are its colour)
        if (spec.size() < 12 || spec.compare(spec.size() - 12, 12, "0 411 640 69") != 0) continue;
        if (!LegendItems(name, items)) {
            kept++;
            continue;
        }
        size_t line = out.rfind('\n', at) + 1, line_end = out.find('\n', q2);
        size_t screen = out.rfind("\n{", at), close = out.find("\n}", at);
        if (screen == std::string::npos || close == std::string::npos) continue;
        // the title's limits: 8 images and 60 texts a screen. One button too many (Change Profile's five): the strip's
        // own image goes, and a comment names the legend for pc_mouse.cpp instead (the strip is the screen's only one)
        size_t images_now = Count(out, screen, close, "BackgroundImage");
        bool drop = images_now + items.size() > 8 && images_now + items.size() == 9 &&
                    Count(out, screen, close, " 0 411 640 69\"") == 1;
        if ((images_now + items.size() > 8 && !drop) || Count(out, screen, close, "BackgroundText") + items.size() > 60) {
            kept++;
            continue;
        }
        // the strip itself stays, drawn transparent (its name still tells pc_mouse.cpp which button each click on the
        // bar is: PcLegendSlots, the same layout), and each button goes right to left: a glyph 24 x 24, the word in the
        // title's text beside it
        std::vector<Placed> placed = Layout(items);
        std::string images = drop ? "\t// csx legend " + name + "\r\n"
                                  : out.substr(line, q1 - line) + name + " 0 0 0 0 0 411 640 69\"\r\n",
                    texts;
        char num[64];
        for (size_t k = 0; k < items.size(); k++) {
            const Word* w = items[k];
            const Placed& p = placed[k];
            images += "\tBackgroundImage \"CSXP_" + std::string(w->slot) + " 255 255 255 255 " +
                      std::to_string(int(p.glyph + 0.5f)) + " 416 24 24\"\r\n";
            std::snprintf(num, sizeof num, " %d 418 %d 24 %.2f", int(p.text + 0.5f), int(p.end - p.text + 10), p.scale);
            texts += "\tBackgroundText \"54 83 107 255" + std::string(num) + "\"\t\"" + w->label + "\"\r\n";
        }
        // the texts after the screen's own (their places in it unchanged: commands set lines by their order)
        out.insert(close + 1, texts);
        out.replace(line, line_end + 1 - line, images);
        replaced++;
    }
    LOGI("pc prompts: %d legends drawn as prompts, %d left as the original's", replaced, kept);
    return out;
}
