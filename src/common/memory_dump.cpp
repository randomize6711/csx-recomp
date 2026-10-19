// Diagnostic guest memory dumps for reverse engineering game state (player position, angles ...).
//
// A file named memdump in the run folder holding a name asks for a dump: the committed pages of the user range and the
// contiguous (physical) range are written to dumps/<name>.bin as records of u32 address, u32 length, then the bytes,
// and the request file is removed. The title keeps running meanwhile, so a dump is a snapshot within about a frame.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace csx {

namespace {

void WriteRange(FILE* f, uint32_t lo, uint32_t hi, bool check) {
    // pages are copied one by one into a buffer and the record written once complete, so a page the title
    // decommits meanwhile ends the run instead of leaving a record shorter than its header says
    std::vector<uint8_t> buf;
    uint32_t start = 0;
    auto flush = [&] {
        if (buf.empty()) return;
        uint32_t n = uint32_t(buf.size());
        std::fwrite(&start, 4, 1, f);
        std::fwrite(&n, 4, 1, f);
        std::fwrite(buf.data(), 1, n, f);
        buf.clear();
    };
    for (uint64_t va = lo; va < hi; va += mem::kPage) {
        if (!check || mem::IsCommitted(uint32_t(va))) {
            if (buf.empty()) start = uint32_t(va);
            size_t at = buf.size();
            buf.resize(at + mem::kPage);
            std::memcpy(buf.data() + at, mem::Base() + va, mem::kPage);
        } else {
            flush();
        }
    }
    flush();
}

void Dump(const std::string& name) {
    namespace fs = std::filesystem;
    fs::path dir = app::ExeDir() / "dumps";
    fs::create_directories(dir);
    fs::path out = dir / (name + ".bin");
    FILE* f = std::fopen(out.string().c_str(), "wb");
    if (!f) return;
    WriteRange(f, mem::kUserLow, 0x10000000, true);  // image, heaps
    WriteRange(f, mem::kContiguousBase, mem::kContiguousBase + mem::kPhysicalSize, false);
    std::fclose(f);
    LOGI("memdump: %s written", out.string().c_str());
}

}  // namespace

void StartMemoryDumps() {
    std::thread([] {
        namespace fs = std::filesystem;
        const fs::path req = app::ExeDir() / "memdump";
        const fs::path poke = app::ExeDir() / "poke";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::error_code ec;
            // poke: "VA FLOAT" written to guest memory once (diagnostics: which copy of a value the title reads)
            if (fs::exists(poke, ec)) {
                unsigned va = 0;
                float v = 0;
                if (FILE* f = std::fopen(poke.string().c_str(), "r")) {
                    // one or more "VA FLOAT" pairs, all written at once
                    unsigned first = 0, count = 0;
                    while (std::fscanf(f, "%x %f", &va, &v) == 2) {
                        if (!va) continue;
                        uint32_t bits;
                        std::memcpy(&bits, &v, 4);
                        kernel::W32(va, bits);
                        if (!count++) first = va;
                    }
                    if (count) LOGI("poke: %08X = %g (%u writes)", first, v, count);
                    std::fclose(f);
                }
                fs::remove(poke, ec);
            }
            if (!fs::exists(req, ec)) continue;
            char name[64] = "dump";
            if (FILE* f = std::fopen(req.string().c_str(), "r")) {
                if (std::fscanf(f, "%63s", name) != 1) std::snprintf(name, sizeof name, "dump");
                std::fclose(f);
            }
            fs::remove(req, ec);
            Dump(name);
        }
    }).detach();
}

}  // namespace csx
