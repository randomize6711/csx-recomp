// Mods (N8, src/common/mods.h, docs/n8_mods.md)
#include "common/mods.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#include "common/pc_settings.h"
#include "core/log.h"

namespace csx::mods {

namespace {
namespace fs = std::filesystem;

std::mutex g_lock;
std::set<std::string> g_overridden;  // normalised names (lower case, '/')
std::string g_signature;
std::vector<std::string> g_report;  // what Apply logged (it runs before the game's log is open): logged again by
                                    // TakeReport's caller

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string Normal(std::string name) {
    name = Lower(name);
    std::replace(name.begin(), name.end(), '\\', '/');
    for (const char* p : {"d:/cstrike/", "d:/", "cstrike/", "/"})
        if (name.rfind(p, 0) == 0) name = name.substr(std::char_traits<char>::length(p));
    return name;
}

bool IdValid(const std::string& id) {
    return !id.empty() && id.size() <= 40 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-.") == std::string::npos;
}

bool VersionValid(const std::string& v) {
    return !v.empty() && v.size() <= 20 && v.find_first_not_of("0123456789.") == std::string::npos && v[0] != '.';
}

// the assets a mod may replace or add: no configs (they run commands), no code, no executables
bool AssetAllowed(const std::string& rel) {
    static const char* const kExt[] = {".mdl", ".spr", ".tga", ".bmp", ".wad", ".xpr", ".sxwad", ".json", ".nav",
                                       ".txt"};
    std::string ext = fs::path(rel).extension().string();
    if (rel.find("..") != std::string::npos) return false;
    for (const char* e : kExt)
        if (ext == e) return ext != ".txt" || rel.rfind("resource/", 0) == 0;  // texts: the game's strings only
    return false;
}

std::vector<Requirement> Requirements(const std::string& list, std::vector<std::string>& problems, const char* what) {
    std::vector<Requirement> out;
    for (const std::string& item : Split(list)) {
        Requirement r;
        size_t op = item.find_first_of(">=");
        r.id = Trim(item.substr(0, op));
        if (op != std::string::npos) {
            std::string rest = item.substr(op);
            r.op = rest.rfind(">=", 0) == 0 ? ">=" : "=";
            r.version = Trim(rest.substr(r.op.size()));
        }
        if (!IdValid(r.id) || (!r.op.empty() && !VersionValid(r.version)))
            problems.push_back(std::string(what) + " \"" + item + "\" is not ID, ID >= VERSION or ID = VERSION");
        else out.push_back(r);
    }
    return out;
}

bool Satisfies(const Mod& m, const Requirement& r) {
    if (m.id != r.id) return false;
    if (r.op.empty()) return true;
    int c = CompareVersions(m.version, r.version);
    return r.op == ">=" ? c >= 0 : c == 0;
}

std::string Hash(const Mod& m) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, 16);
    std::vector<std::string> parts = {"mod.ini"};
    for (const std::string& f : m.files) parts.push_back("files/" + f);
    for (const std::string& p : parts) {
        std::ifstream in(fs::path(m.folder) / p, std::ios::binary);
        crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(p.data()), p.size());
        char buf[65536];
        while (in.read(buf, sizeof buf) || in.gcount())
            crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(buf), size_t(in.gcount()));
    }
    unsigned char out[16];
    crypto_generichash_final(&st, out, sizeof out);
    char hex[33];
    for (int i = 0; i < 16; i++) std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
    return hex;
}

Mod Load(const fs::path& folder) {
    Mod m;
    m.folder = folder.string();
    std::map<std::string, std::string> v;
    std::ifstream in(folder / "mod.ini");
    if (!in) m.problems.push_back("no mod.ini");
    for (std::string line; std::getline(in, line);) {
        line = Trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos) continue;
        v[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    m.id = v["id"];
    m.name = v["name"].empty() ? m.id : v["name"].substr(0, 40);
    m.version = v["version"];
    m.author = v["author"].substr(0, 40);
    m.description = v["description"].substr(0, 200);
    m.needs = v["requires"];
    if (!v["kind"].empty()) m.kind = Lower(v["kind"]);
    if (!IdValid(m.id)) m.problems.push_back("id \"" + m.id + "\": lower case letters, digits, _ - . (40 at most)");
    if (m.id != Lower(folder.filename().string())) m.problems.push_back("its folder is not named as its id");
    if (!VersionValid(m.version)) m.problems.push_back("version \"" + m.version + "\" is not numbers and dots");
    if (m.kind != "cosmetic" && m.kind != "content") m.problems.push_back("kind is cosmetic or content");
    if (!m.needs.empty() && (!VersionValid(m.needs) || CompareVersions(m.needs, kGameVersion) > 0))
        m.problems.push_back("needs mod interface " + m.needs + " (this game's is " + kGameVersion + ")");
    m.depends = Requirements(v["depends"], m.problems, "dependency");
    m.conflicts = Requirements(v["conflicts"], m.problems, "conflict");
    std::error_code ec;
    fs::path files = folder / "files";
    if (fs::is_directory(files, ec))
        for (auto it = fs::recursive_directory_iterator(files, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_symlink(ec)) {  // a link could reach outside the mod
                m.refused.push_back(fs::relative(it->path(), files, ec).generic_string() + " (a link)");
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string rel = fs::relative(it->path(), files, ec).generic_string();
            if (rel != Lower(rel)) {
                m.refused.push_back(rel + " (paths are lower case)");
            } else if (AssetAllowed(rel)) {
                m.files.push_back(rel);
            } else {
                m.refused.push_back(rel);
            }
        }
    std::sort(m.files.begin(), m.files.end());
    if (m.files.empty()) m.problems.push_back("no files it can use under files/");
    if (m.problems.empty()) m.hash = Hash(m);
    return m;
}

// the link made in the overlay only: a folder on the way that is itself a link (the overlay's links to the game's own
// files and folders) is never written through, so the game's folder is never changed
void Link(const fs::path& from, const fs::path& to, const fs::path& overlay) {
    std::error_code ec;
    fs::path at = overlay;
    for (const fs::path& part : fs::relative(to.parent_path(), overlay, ec)) {
        at /= part;
        if (fs::is_symlink(at, ec)) {
            LOGW("mods: %s not linked: %s is the game's own", to.string().c_str(), at.string().c_str());
            return;
        }
    }
    fs::create_directories(to.parent_path(), ec);
    fs::remove(to, ec);
    fs::create_symlink(fs::absolute(from), to, ec);
    if (ec) LOGW("mods: %s not linked: %s", to.string().c_str(), ec.message().c_str());
}
}  // namespace

std::vector<std::string> Split(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream in(list);
    for (std::string item; std::getline(in, item, ',');)
        if (!Trim(item).empty()) out.push_back(Trim(item));
    return out;
}

std::string Join(const std::vector<std::string>& ids) {
    std::string s;
    for (const std::string& id : ids) s += (s.empty() ? "" : ",") + id;
    return s;
}

int CompareVersions(const std::string& a, const std::string& b) {
    std::stringstream x(a), y(b);
    for (;;) {
        std::string p, q;
        bool more_a = bool(std::getline(x, p, '.')), more_b = bool(std::getline(y, q, '.'));
        if (!more_a && !more_b) return 0;
        long i = more_a ? std::atol(p.c_str()) : 0, j = more_b ? std::atol(q.c_str()) : 0;
        if (i != j) return i < j ? -1 : 1;
    }
}

std::string Folder() {
    if (const char* d = std::getenv("CSX_MODS")) return d;
    std::error_code ec;
    fs::path exe = fs::canonical("/proc/self/exe", ec);
    return ec ? std::string() : (exe.parent_path().parent_path() / "mods").string();
}

std::vector<Mod> Scan() {
    std::vector<Mod> out;
    std::string folder = Folder();
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return out;
    for (const auto& e : fs::directory_iterator(folder, ec))
        if (e.is_directory(ec)) out.push_back(Load(e.path()));
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.id < b.id; });
    return out;
}

Plan Resolve(const std::vector<Mod>& all, const std::vector<std::string>& enabled) {
    Plan plan;
    std::set<std::string> seen;
    for (const std::string& id : enabled) {
        if (!seen.insert(id).second) continue;
        auto it = std::find_if(all.begin(), all.end(), [&](const Mod& m) { return m.id == id; });
        if (it == all.end()) {
            plan.messages.push_back(id + ": not installed");
            continue;
        }
        if (!it->problems.empty()) {
            plan.messages.push_back(id + ": " + it->problems[0]);
            continue;
        }
        plan.active.push_back(*it);
    }
    // dependencies (enabled, at a version that satisfies; order free) and conflicts, until nothing more drops out
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < plan.active.size(); i++) {
            const Mod& m = plan.active[i];
            std::string why;
            for (const Requirement& r : m.depends)
                if (std::none_of(plan.active.begin(), plan.active.end(), [&](const Mod& o) { return Satisfies(o, r); }))
                    why = "needs " + r.id + (r.op.empty() ? "" : " " + r.op + " " + r.version) + ", not enabled";
            for (const Requirement& r : m.conflicts)
                for (const Mod& o : plan.active)
                    if (&o != &m && Satisfies(o, r)) why = "conflicts with " + o.id;
            if (why.empty()) continue;
            plan.messages.push_back(m.id + ": " + why);
            plan.active.erase(plan.active.begin() + long(i));
            changed = true;
            break;
        }
    }
    return plan;
}

std::vector<std::string> Enabled(const std::vector<Mod>& all) {
    std::vector<std::pair<int, std::string>> on;
    for (const Mod& m : all)
        if (pc::Get("mod." + m.id, "0") == "1")
            on.push_back({std::atoi(pc::Get("mod_order." + m.id, "50").c_str()), m.id});
    std::sort(on.begin(), on.end());
    std::vector<std::string> ids;
    for (auto& [order, id] : on) ids.push_back(id);
    return ids;
}

std::vector<Mod> Apply(const std::string& overlay, bool link) {
    std::vector<Mod> all = Scan();
    std::vector<std::string> enabled = Enabled(all);
    bool original = pc::Get("mods_original", "0") == "1";
    Plan plan = Resolve(all, original ? std::vector<std::string>{} : enabled);
    std::vector<std::string> report;
    for (const std::string& m : plan.messages) {
        LOGW("mods: %s (left out)", m.c_str());
        report.push_back("mods: " + m + " (left out)");
    }
    std::set<std::string> overridden;
    std::string signature;
    std::error_code ec;
    for (const Mod& m : plan.active) {
        for (const std::string& f : m.files) {
            fs::path src = fs::path(m.folder) / "files" / f;
            if (!link) {
                if (f.rfind("maps/", 0) != 0) overridden.insert(f);
                continue;
            }
            if (f.rfind("maps/", 0) == 0) {  // a map: beside the game's archives, as an imported one (never in place
                                             // of one of the game's own)
                fs::path to = fs::path(overlay) / fs::path(f).filename();
                if (fs::is_symlink(to, ec) && !fs::equivalent(fs::read_symlink(to, ec), src, ec)) {
                    LOGW("mods: %s: %s is the game's own map, not replaced", m.id.c_str(), f.c_str());
                    continue;
                }
                Link(src, to, overlay);
            } else {
                Link(src, fs::path(overlay) / "cstrike" / f, overlay);
                overridden.insert(f);
            }
        }
        if (m.kind == "content") signature += (signature.empty() ? "" : " ") + m.id + "@" + m.version + ":" + m.hash;
        for (const std::string& r : m.refused) LOG_FIRST(32, "mods: %s: %s not taken (not an asset)", m.id.c_str(), r.c_str());
        LOGI("mods: %s %s (%s, %zu files)", m.id.c_str(), m.version.c_str(), m.kind.c_str(), m.files.size());
        report.push_back("mods: " + m.id + " " + m.version + " (" + m.kind + ", " + std::to_string(m.files.size()) +
                         " files)");
    }
    if (!plan.active.empty() || !enabled.empty())
        LOGI("mods: %zu active of %zu installed (%s)%s", plan.active.size(), all.size(), Folder().c_str(),
             original ? ": the original's looks chosen" : "");
    std::lock_guard<std::mutex> lock(g_lock);
    g_overridden = std::move(overridden);
    g_signature = signature;
    g_report.insert(g_report.end(), report.begin(), report.end());
    return plan.active;
}

bool Overridden(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_lock);
    return !g_overridden.empty() && g_overridden.count(Normal(name)) != 0;
}

void Note(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_report.push_back(line);
}

std::vector<std::string> TakeReport() {
    std::lock_guard<std::mutex> lock(g_lock);
    std::vector<std::string> r;
    r.swap(g_report);
    return r;
}

std::string ContentSignature() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_signature;
}

}  // namespace csx::mods
