# Guest memory: findings and modernization proposal (G8.2B checkpoint 1, 2026-10-23)

The question: how memory works in this port today, which of its limits are artificial, and how to move toward a
scalable PC memory design without breaking the recompiled code. Static evidence: analysis/memory_architecture.md
(addresses, P/I marked). Measurements: FINDINGS.md "Guest memory" (CSX_LOG_MEMORY=1).

## 1. How it works today

- **One 4 GB guest window.** The host reserves 4 GB of its 64-bit address space without committing it
  (sdk/runtime/kernel/memory.cpp, `mmap(PROT_NONE, MAP_NORESERVE)` at host 0x100000000). Every guest address is a
  32-bit offset into it (`base + (uint32_t)addr`, cpu/x86_ops.h), so the recompiled code can only reach memory inside
  that window, and every access outside committed pages faults instead of touching host memory.
- **The "64 MB" is the contiguous pool only.** A 64 MB memory file is mapped at guest 0x80000000 (and again at
  0xF0000000, the Xbox's write-combined view). Only contiguous allocations draw from it (63 MB after the kernel's
  first MB): textures, vertex and index buffers, sound buffers, movie frames, the host's frame buffers and its 4 MB
  upload ring.
- **The game's heap is already dynamic.** CRT malloc goes to the XAPI NT heap linked into the image, which grows in
  segments of doubling size (1, 2, 4, ... MB, up to 64 segments) and gives large blocks their own reservations, all
  through NtAllocateVirtualMemory in the user range 0x10000..0x7FFE0000 (about 2 GB). The host commits those pages
  only when asked and the OS backs them only when touched. The engine has no fixed hunk or zone: its "hunk", "zone"
  and "cache" are malloc'ed lists (the -heapsize and -zone options are dead code, Sys_InitMemory 0xAD310).
- **Two more host pools**: the system pool (0xB0000000, 256 MB: kernel objects, thread blocks, host HLE objects) and
  stacks (0xD0000000, 256 MB).
- **Measured** (CSX_LOG_MEMORY=1, headless, FINDINGS.md "Guest memory"): a de_aztec match with 16 bots commits about
  37 MB of user memory (52 MB reserved, in 15 reservations between 0x10000 and 0x3640000, 1994 MB of the user range
  still free in one block) and uses 22.6 MB of the 63 MB contiguous pool; the same map with 2 bots the same within
  0.5 MB; de_airstrip from the front end 37.5 MB and 25.6 MB.
- **Nothing in the game sizes itself from the memory it is told it has.** MmQueryStatistics, which reports 64 MB
  minus all use, is read only by dead code and by XNET's random seed.

## 2. Which restrictions are artificial and which are structural

| Restriction | Kind | Why |
|---|---|---|
| The 64 MB contiguous pool | artificial (a host constant, mem::kPhysicalSize) | it can grow to 192 MB without other changes; up to 256 MB the game's own masks hold, but past 208 MB the 0xF0000000 view would reach the 0xFD000000 host thunk numbers |
| MmQueryStatistics reporting 64 MB | artificial, harmless | nothing reads it to size anything |
| The about 2 GB user range | structural | 32-bit guest pointers, and pointers above 2 GB would turn negative in signed compares the compiled code may do |
| Guest addresses are 32 bits | structural | every guest-visible structure must lie in the 4 GB window: a native 64-bit allocation cannot be handed to recompiled code without changing that code |
| D3D resources in 0x80000000..+256 MB | structural | the game stores a texture's address as VA & 0x0FFFFFFF and the host maps it back into the physical window (20 sites in the game, 31 in the D3D library) |
| 0xFD000000 and up are call targets | structural (host) | the host numbers its thunks there; data may live there, function pointers may not |
| Static data at the image's addresses | structural | compiled into instructions; moved only by [[patch]] (checkpoint 1) |
| Engine pools: 1040 edicts at 17 slots (785 + 15 per slot), the lightmap atlas, HUD sprites, particles, precache counts, 256 packet entities and 1024 entity states a frame | game limits, not memory limits | each is a constant in the game's code; raised one by one where a feature needs it |
| Out of memory | a robustness gap | the engine's allocators never check for NULL: heap exhaustion is a write to guest page 0 and a crash without a message |

## 3. What can be dynamic, and what must stay guest-addressable

- **Already dynamic**: the guest heap (all engine data: client records, edicts, entity states, map data, the cache),
  committed on demand; the contiguous pool's pages (a memory file, backed when touched).
- **Must stay guest-addressable**: everything recompiled code reads or writes, including every table the 65-slot
  work enlarges. Those tables move to **guest tables** (checkpoint 1, below): a reserved part of the user range with
  fixed addresses, so [[patch]] can bake them into the generated code at build time.
- **Can be native host memory**: state only host code uses (hooks, diagnostics, side tables a hook keeps, e.g. a
  per-player value for trigger_hurt's 32-bit mask in checkpoint 2). These use ordinary C++ containers; the guest never
  sees them, so their size is not limited by the window.
- **Not safe**: handing a host pointer to guest code, or storing one in guest memory. Nothing does today (the host
  keeps guest addresses in its structures, never the reverse) and the design keeps it that way.

## 4. Proposal

**During G8.2B (65 slots), only what the expansion needs:**

1. Guest tables (implemented in checkpoint 1): config/guest_tables.toml, laid out by tools/guest_tables.py,
   mapped by src/common/guest_tables.cpp before the title starts. Region 0x70000000..0x71000000 (16 MB of address
   space, only the tables' pages committed), taken out of the game's allocator; each table ends at a page with an
   uncommitted guard page after it; an access past the end or before the first page is reported with the table and
   element; Element() refuses an index out of range for host code. Its first tables are the 65-slot ones.
2. Memory measurement (CSX_LOG_MEMORY=1, added in checkpoint 1; run at 65 slots in checkpoint 2): reserved,
   committed, fragmentation, the contiguous pool. The expected growth is about 70 KB per extra slot in the heap
   (client record, frames, edicts), so about +3.3 MB at 65: no limit is near.
3. A clear report when the guest runs out of memory: log NtAllocateVirtualMemory and contiguous failures with the
   sizes in use, and name a guest page 0 access as a likely failed allocation (checkpoint 2, small).

**A separate memory-modernization milestone (after G8.2B), driven by features that need it:**

4. The contiguous pool configurable (64 MB default; 128 or 192 MB for improved textures and larger maps), with
   MmQueryStatistics reporting the real sizes.
5. The engine's fixed pools raised where larger maps and assets need them (lightmap atlas, HUD sprites, particles,
   precache counts, the edict formula, packet entities, entity states), each through [[patch]] and guest tables with
   the same audit and tests as the player tables.
6. If guest-visible data ever outgrows the user range: a second data arena in an unused window (0xC0000000..
   0xD0000000 or 0x84000000..0xB0000000, both free today) for tables reached only through patched addresses; never
   for D3D resources or code addresses.
7. The memory dumps (src/common/memory_dump.cpp) cover the whole user range, not only its first 256 MB.

**Not proposed**: a fixed 8 or 16 GB allocation (the 32-bit guest cannot address it, and the host already commits on
demand), a rewrite of the guest heap (it already grows and is the game's own code), or 64-bit guest pointers (that
would mean changing every instruction that holds one).

## 5. Risks

- A fixed-base allocation by the game inside 0x70000000..0x71000000 would fail once tables are mapped there. The
  game's reservations measured in a 16-bot match all lie in 0x10000..0x3640000 (the heap grows upward from the
  image), so the region is about 1.7 GB away from them; Map refuses to start if any part of it is already in use.
- Signed pointer compares: the guest tables stay below 2 GB for this reason; tables in a high window (item 6) would
  need each referencing loop checked.
- Guard pages catch overruns past a table's end exactly, and underruns only past the table's first page.
