# CS:Xbox guest memory: allocators, pools, addresses (G8.2B checkpoint 1, 2026-10-23)

Static analysis of assets/default.xbe and of the runtime (sdk/runtime/kernel/memory.cpp). The proposal built on it is
docs/memory_modernization.md; the measurements are in FINDINGS.md "Guest memory".

P = PROVED from code (VA or source line given). I = INFERRED.

## 0. Image layout
- P: the XBE image is 0x10000..0x807FD8 (header 0x10C size 0x7F7FE0): after .data come DOLBY 0x7D6F20,
  XON_RD, .data1, DSKERDAT 0x7DF580..0x803D80, $$XTIMAGE/$$XSIMAGE/.XTLID up to 0x807FD8. mem::ReserveImage takes it out
  of g_user in 64 KB granules, so the first user allocation lands at 0x810000 or above.
- P: XBE header: SizeOfHeapReserve [0x10134] = 0x100000 (1 MB), SizeOfHeapCommit [0x10138] = 0x1000. Stack commit 0x11AC0.

## 1. Allocator chain
| Layer | VA | What it does |
|---|---|---|
| CRT malloc | 0x22D86A | `_nh_malloc(n, _newmode [0x3A6688])` -> 0x22D83E (n > 0xFFFFFFE0 fails; retry through new-handler) |
| _heap_alloc | 0x22D817 | n==0 -> 1, round up to 16 (unless [0x5D8EA4]==1), `RtlAllocateHeap(XapiProcessHeap, 0, n)` |
| GetProcessHeap | 0x20192E | returns [0x5D46F8] |
| RtlAllocateHeap | 0x205390 | statically linked NT heap (lock at heap+0x580, free lists heap+0x180) |
| Heap created | 0x20398C | `RtlCreateHeap(HEAP_GROWABLE=2, base 0, reserve [0x10134]=1 MB, commit [0x10138]=4 KB, 0, params)` -> [0x5D46F8] |
| RtlCreateHeap | 0x204F6C | defaults from .data 0x36D2B0: SegmentReserve 1 MB, SegmentCommit 8 KB, DeCommitTotalFree 64 KB, DeCommitFreeBlock 4 KB; MaximumAllocationSize 0x7FFDEFFF; VirtualMemoryThreshold capped 0xFF000 (0x205021) |
| initial segment | 0x2051A7 / 0x2051E0 | NtAllocateVirtualMemory MEM_RESERVE 1 MB, MEM_COMMIT 4 KB |
| extend heap | 0x204B30..0x204C79 | scans 64 segment slots (heap+0x60, `cmpb $0x40` 0x204BA5); new segment reserve = max(request+0x1000, heap->SegmentReserve [heap+0x20]); on failure halves down to the request (0x204BFF); on success `heap->SegmentReserve += reserved` (0x204C28) -> geometric growth 1,2,4,8... MB; commit max(request, SegmentCommit) |
| commit inside segment | 0x204A87 | NtAllocateVirtualMemory MEM_COMMIT as the segment fills |
| big blocks | 0x2059FB | > VirtualMemoryThreshold: direct NtAllocateVirtualMemory MEM_COMMIT (implicit reserve), one VA region per block |
| XMemAlloc (XAPI default) | 0x202E9A | attr bit31 clear -> RtlAllocateHeap (0x20195F, zero flag from bit30); bit31 set -> XPhysicalAlloc 0x201EE0 (align 1<<nibble(attr>>24)) or XPhysicalAllocEx 0x201F23 (nibble 1: max 0xFFFFFF, align 4 KB); bit30 -> memset 0 |
| XPhysicalAlloc(Ex) | 0x201EE0 / 0x201F23 | MmAllocateContiguousMemoryEx (0x201F09 / 0x201F36); NULL -> SetLastError(8) and return 0 |

- P: no maximum besides 64 segments; each new segment is at least as big as all previous together, so the cap is the
  2 GB user VA window. Big blocks are not counted against segments at all.
- P: the engine has no hunk/zone of its own. Memory_Init 0xC9220 only registers commands (flush, x_memprintstats,
  x_cachebytesize, x_cacheitemcount, x_expensivechecks, x_alloclocations, mem_dbgfile) and checks that "-zone" has an
  argument (Sys_Error 0x33B324); the size is never read. "XBox mem lists" are malloc'ed linked blocks with 16-byte headers:
  | list | head | alloc fn |
  |---|---|---|
  | Mem_Alloc / Z_Malloc | count 0x552318 | 0xC8E90 malloc+memset, Mem_Calloc 0xC91A0, Mem_Malloc 0xC9170, strdup 0xC91D0 |
  | LowHunk | 0x552324 | 0xC8EE0 malloc(n+16) |
  | HighHunk / temp | 0x55231C | 0xC8F30 (frees previous temp block first, flag 0x5DF760) |
  | Cache | 0x55232C (LRU tail 0x552330) | Cache_Alloc 0xC9130 -> evict 0xC89E0 -> malloc(n+16) |
- P: cache eviction 0xC89E0: evicts LRU while (cache bytes + request > [0x37A7F8]=0x100000 (1 MB)) AND (items+1 >
  [0x37A7FC]=0x100 (256)). So the cache holds at least 256 items or 1 MB, whichever is more, and is otherwise unbounded.
  Both minimums are console-settable (x_cachebytesize / x_cacheitemcount).
- P: Sys_InitMemory 0xAD310: GlobalMemoryStatus into a local, overwrites dwTotalPhys local with 0x3000000, sets
  host_parms.memsize [0x6CE0D4] = 0x1800000, parses "-heapsize" and atoi()s it (0xAD35D) but discards the result, then
  sets memsize = 0x400. host_parms.membase [0x6CE0D0] stays 0. Host_Init 0x5D9E0 copies the parms; nothing sizes a
  pool from them. -heapsize and the memory status are dead.

### Who uses contiguous memory
| User | VA / source | Size |
|---|---|---|
| Game D3D resources via XMemAlloc attr 0xB7800000 (textures, align 128) | 0x136F6 0x16016 0x72AA6 0x72C85 0xBDF2C 0xBE310 0xBFD0D 0xBFD93 0xBFE17 0xC70E0 0x107EDE | per texture, wad/lightmap/sprites/models: unbounded, grows with content |
| Game vertex/index buffers attr 0xBC800000 (align 4 KB) | 0xC7652 0xC77A9 0x10840D | per model |
| Two static ~1 MB physical buffers (0x104000 each) | 0x23CDC0 -> [0x553ED0],[0x553ED4] | 2.03 MB |
| movie / video (attr 0xB2800000) | 0x194C87 0x196002 0x1971A8 | movie frame buffers |
| XNET packet pool | 0x26B881 MmAllocateContiguousMemoryEx((n+2)<<11) | small (2 KB per buffer) |
| DSOUND lib | 0x247B3B (guest), host HLE dsound.cpp:551/765 AllocContiguous per buffer | all sound buffers |
| D3D lib | 0x306AD4 (size+0x40, max 0x7FFFFFF, align 0x40) and 10 more XMemAlloc phys sites in D3D/XGRPH | push buffer etc. (mostly replaced by HLE) |
| XPP (USB) | 0x318EF9 MmAllocateContiguousMemory | small |
| XLaunch data page | 0x201D68 4 KB + MmPersistContiguousMemory | 4 KB |
| Host HLE | d3d8.cpp:221 NewSurface (frame/depth buffers pitch*h), :199 scratch push >=64 KB, :1377 upload ring 4 MB; boot.cpp:51/140 | ~4 MB ring + 640x480x4 surfaces |
| Kernel data | memory.cpp g_contig starts at 0x80100000 | first 1 MB reserved |
- I: exact contiguous peak is not logged (AllocContiguous only logs on failure). FINDINGS.md/TODO.md "9 MB of 64 MB" is the
  upload-ring peak, not the total. Textures for a map are the large term.

## 2. Fixed-size pools / budgets
| Item | Evidence | Size |
|---|---|---|
| edicts | 0xA0221: max_edicts = maxclients*15 + 0x311 (785) -> [0x62B41C]; LowHunk alloc max_edicts*0x318 (0xA026F), no NULL check | 17 pl: 1040 edicts, 0.8 MB |
| client_t | docs/player_slots_plan.md: constant 17 x 0x5010 (push 0x55120 at 0xA1A0B) | 348 KB |
| map image (wad/bsp) temp | 0x4E953 / 0x4EB2D malloc(0x300000) | 3 MB each, Sys_Error on NULL |
| lightmap atlas | 0x52555 `cmp $0x4000` -> "AllocBlock: full" 0x32FCB4 | fixed block grid |
| Cache | 0x37A7F8 / 0x37A7FC | min 1 MB / 256 items (see above) |
| HUD sprites | "cannot allocate more than %d HUD sprites" 0x32ADBC (0x22BD9) | fixed count |
| particles | "Particleman is out of memory" 0x365858 (0x1F9086) | fixed pool |
| precache limits | PF_precache_* strings 0x333F38.. | MAX_MODELS / MAX_SOUNDS style counts |
| MmQueryStatistics users | 0x200E62 GlobalMemoryStatus (only caller Sys_InitMemory 0xAD321, result ignored); 0x2724F3 XNET entropy (stats hashed into a random seed) | nothing sized from them |

## 3. Allocation failure handling
| Path | Behaviour | Evidence |
|---|---|---|
| Z_Malloc / Mem_Calloc / Mem_Malloc | no check: memset(NULL, n) -> guest write to 0x0 -> host SIGSEGV (guest page 0 not committed) | 0xC8EB1..0xC8ECB, 0xC9175, 0xC91AC |
| Hunk_Alloc (Low/High) | header store skipped, memset at 0x10 -> crash | 0xC8EF7, 0xC8F6C |
| Cache_Alloc | stores into NULL+0xC -> crash | 0xC914C..0xC9166 |
| edict block | inlined hunk alloc, same crash | 0xA0283 |
| map images | Sys_Error "Not enough memory to load map images (1)/(2)" | 0x4E96A, 0x4EB44 |
| D3D texture create (detail textures etc.) | Sys_Error "Create texture failed" (E_OUTOFMEMORY test 0x8007000E) | 0x1373F..0x1374C |
| Sys_Error | 0xAC090 prints "FATAL ERROR (shutting down): %s" (0x3397B0); 334 call sites | 0xAC112 |
| XPhysicalAlloc | returns 0 + SetLastError(8) | 0x201F15 |
| host AllocContiguous | LOGE "contiguous allocation of %X bytes failed ... KB in use", returns 0 | memory.cpp AllocContiguous |
| other | hpak/netchan/ED_Alloc "no free edicts" (0x7B42A) are Sys_Error/Con_Printf | strings 0x330C70.., 0x33464C |
- I: so heap exhaustion = guest null-page fault (crash, no message); contiguous exhaustion = mostly Sys_Error or NULL
  propagated into D3D resources.

## 4. Hardcoded addresses / pointer assumptions
| Assumption | Evidence | Note |
|---|---|---|
| physical = VA & 0x0FFFFFFF for D3D resource Data | 20 `andl $0xfffffff` in .text right after phys XMemAlloc (0x13712, 0x16032, 0x72AC2, 0x72CA5 ...), 31 in D3D (0x306AE8) | works for 0x80000000..0x8FFFFFFF only |
| host maps Data back with VirtualFromPhysical: pa < kPhysicalSize -> 0x80000000\|pa else identity | memory.cpp; used by texture.cpp:277/430, vertex.cpp:302/393, d3d12.cpp, render_thread.cpp:364, d3d8.cpp | a heap VA < 0x10000000 put in Data passes through unchanged |
| report address mask 0xFFFFFF (first 16 MB) | d3d8.cpp:1492; AllocContiguous allocates top-down to keep low 16 MB free | |
| NV2A registers 0xFD000000.. | guest D3D 0x30D675/0x30D67B/0x30D683 (0xFD001804), XPP 0x31991E/0x31A6C6 (0xFD050F80) | I: HLE'd/unreached; would fault (unmapped) |
| USB OHCI 0xFED00000 | XPP 0x318972 | I: XPP HLE |
| host thunk IDs at 0xFD000000+4i, kernel thunks at 0xFE000000+4*ord | dispatch.cpp:35/58; x86_icall treats any target >= 0xFD000000 as host thunk | only call targets; collides with the WC view if physical ever exceeds 208 MB |
| kernel variables in first physical MB | memory.h kKernelDataBase 0x80000000 size 1 MB | |
| 0x7FFE0000 user top | GlobalMemoryStatus 0x200E88 (dwTotalVirtual), RtlCreateHeap max 0x7FFDEFFF | |
| high-bit tests on pointers | I: none found. `orl/andl $0x80000000` (e.g. 0x452FB, 0x876E5) and `andl $0x7fffffff` (0x43FBF..) look like flag words and fabs; not verified one by one |
| memory dump | src/common/memory_dump.cpp:58 dumps only 0x10000..0x10000000 + physical | user allocations above 256 MB would be missing from dumps |

## 5. Host side
- P: guest access is `base + (uint32_t)addr` (x86_ops.h:18 X86_MEM32); HostPtr/R32/W32 (kernel.h:62-80) have no bounds
  or commit checks. All 4 GB is reserved PROT_NONE at host 0x100000000 (MAP_NORESERVE), so any guest VA is host-safe
  to address and faults only if not committed.
- P: host code stores guest VAs in host structures (Object::guest, gpu caches keyed by guest VA, written_ sets,
  g_scratch_push, g_up_ring) and never host pointers in guest memory: the only uintptr_t uses are main.cpp:47 (fault
  address minus g_base) and write_watch.cpp (host fault addresses).
- P: allocators: user VA (g_user, first fit bottom-up, 64 KB granules) via NtAllocateVirtualMemory; contiguous
  (g_contig, top-down) 0x80100000..0x84000000; system pool 0xB0000000+256 MB (ExAllocatePool, MmAllocateSystemMemory,
  thread KPCR/ETHREAD, host HLE objects, render_thread 1 MB header ring); stacks 0xD0000000+256 MB. g_committed is a
  1 Mi-entry map covering all 4 GB.
- Free guest VA today:
  | Range | Status | Safe for new guest-visible allocations? |
  |---|---|---|
  | 0x00810000..0x7FFE0000 | g_user, heap grows here | only through mem::AllocateVirtual (else NtAllocateVirtualMemory collides) |
  | 0x7FFE0000..0x80000000 | outside every allocator; QueryVirtual returns false for >= kUserHigh | yes (128 KB), but Xbox-reserved |
  | 0x84000000..0xB0000000 | reserved, no allocator, InPhysical false | yes for data. Not for D3D resource data (pa = VA&0x0FFFFFFF >= 64 MB -> VirtualFromPhysical identity = wrong) |
  | 0xC0000000..0xD0000000 | unused (Xbox page tables) | yes |
  | 0xE0000000..0xF0000000 | unused | yes |
  | 0xF4000000..0xFD000000 | unused (beyond the 64 MB WC view) | yes for data |
  | 0xFD000000..0xFFFFFFFF | thunk ID space for indirect calls | data yes, function pointers no |

## 6. Is 64 MB a real constraint?
- P: the memfd (kPhysicalSize 64 MB) backs only 0x80000000+64 MB and its 0xF0000000 alias. CRT malloc, the engine's
  hunk/zone/cache lists, edicts, client_t, map images all come from the XAPI heap -> NtAllocateVirtualMemory -> user VA
  backed by anonymous host memory, limited only by 0x7FFE0000 (about 2 GB). System pool and stacks are separate 256 MB
  windows.
- P: only contiguous allocations (textures, vertex buffers, sound buffers, frame buffers, push/upload rings, XNET
  packets) draw from the 63 MB (64 minus the 1 MB kernel data) pool.
- P: mem::Query counts user committed + contiguous + system pages against 64 MB, so MmQueryStatistics can report 0
  available while plenty is free. The only consumers are Sys_InitMemory (ignored) and XNET entropy, so reporting more
  memory changes nothing in game behaviour.
- I: raising kPhysicalSize to 128 or 256 MB is consistent with the guest's & 0x0FFFFFFF masks (<= 256 MB) and with
  VirtualFromPhysical (it uses kPhysicalSize). Beyond 208 MB the 0xF0000000 view would overlap the 0xFD000000 thunk IDs
  (harmless for data, but it must not hold call targets). The 16 MB report mask keeps the low-memory need.
- I: for more players the growth is in malloc'ed heap (client_t 0x5010 each, edicts 15*0x318 = 11.6 KB per slot,
  per-client frame/baseline data), which is outside the 64 MB pool; texture use does not scale with players.
