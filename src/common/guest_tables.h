#pragma once
// Guest tables: guest-addressable tables the host maps in a reserved part of the guest's user space, each followed
// by an uncommitted guard page (config/guest_tables.toml, laid out by tools/guest_tables.py). Recompiled code reaches
// them through [[patch]]ed addresses; host code through Element(), which refuses an index out of range.
#include <cstddef>
#include <cstdint>
#include <string>

namespace guest_tables {

struct Region {
    uint32_t base, size;
};

struct Table {
    const char* name;
    uint32_t va;     // its first element
    uint32_t elem;   // bytes per element
    uint32_t count;  // elements
    uint32_t align;
};

// a layout's problem, "" when it is sound: the region in the user range on 64 KB boundaries, each table aligned,
// inside the region, ending at a page boundary, with an unused page before it and after it, no overlap, no 32-bit
// overflow
std::string Validate(const Region& region, const Table* tables, size_t count);

// reserves the region in the guest's allocator (refused when any part of it is in use), commits each table's pages
// zeroed and leaves the rest reserved and inaccessible; installs the guard fault report. false with the reason logged
bool Map(const Region& region, const Table* tables, size_t count);

// element `index` of a mapped table, 0 when the index is out of range (logged, once per table and caller)
uint32_t Element(const Table& table, uint32_t index, const char* caller);

// the mapped table whose guard page holds va (the page after its end), with how far past its end va is
const Table* GuardOwner(uint32_t va, uint32_t* past_end);
// the mapped table whose first page follows the guard page holding va, with how far before its first element va is
const Table* GuardBefore(uint32_t va, uint32_t* before_start);

// the production layout (guest_tables_layout.h): mapped before the title starts; nothing while it has no table.
// A layout that cannot be mapped stops the program with the reason (patched code would use unmapped memory)
void Start();

}  // namespace guest_tables
