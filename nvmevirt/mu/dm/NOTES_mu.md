# μ-FTL port (`dm/ftl_mu.c`, `DM_POLICY := mu`)

Port of ftlsim `mode=mu` (`struct MuSim`, ftlsim.cpp) — μ-FTL, Lee/Jung/Kang/Kim, EMSOFT'08.
**Result: bit-identical** to ftlsim on all three validation traces, for two cache sizes and a non-default parameter set
(WAF, total programs, erases, and also `reads.map` / `map_miss` / `prog.map` / `reads.gc_copy` / `mem`).

## What was ported (ftlsim function -> C)
| MuSim | ftl_mu.c | notes |
|---|---|---|
| constructor | `mu_create` | L, NB, P from `dm_geom` (harness computes them exactly like MuSim: `L -= L%U`, `NB = (u64)(L*(1+op))/P`); `part_pages = ceil(L/parts)`, `cache_pages = max(1, cache_bytes/4096)`, `bm_cap = max(1, bitmap_cache_bytes/4)`; LIFO free list, block 0 first. Prints the same `[cfg] mode=mu L=.. NB=.. P=.. parts=.. cache=.. pages bitmap_cache=.. entries F=.. maxext=.. T=..` line (GB value via integer math, no zipf) |
| `touch_leaf` / `drop_leaf` | `mu_touch` / `mu_drop` | LRU page cache of cache_pages leaf pages; hit -> MRU (+dirty); miss -> `map_miss++`, one `DM_R_MAP` read; evict LRU while full, dirty victim -> one `DM_P_MAP` program |
| `leaf_for` | `mu_leaf_for` | same binary search (incl. the `!empty` test) |
| `insert_ext` | `mu_insert_ext` | merge with in-leaf predecessor iff logically + physically contiguous, same block, merged length <= MAXE; else insert, split leaf in half (`size/2` stay) when > F, new leaf touched dirty |
| `unmap_range` (Cases 0-3) | `mu_unmap_range` | delete / split / trim right / trim left, per leaf in ascending order, invalidation per extent in order, then the leaf is touched dirty, emptied leaves removed (if >1) + dropped from cache |
| `invalidate_phys`, `bm_flush` | `mu_invalidate_phys`, `mu_bm_flush` | per-block invalid counter; outside GC every invalidated extent adds an entry; at `bm_cap` entries: one dirty bitmap-leaf touch (key `1<<40 | blk/(F/4)`) per block group |
| `lookup` | `pmap[lpn]` | |
| `gc`, `update_block`, `write_pages`, `erase`, `pop_free` | same names | GC when `free <= T` (not re-entered), loop until `free >= T+2`; victim = max invalid, ties lowest id, all partitions' update blocks excluded (incl. full ones still registered); valid runs (<= MAXE, logically consecutive) copied through `write_pages` into the partition's update block (shared with host writes, per the paper); after a GC the caller still pops a fresh block for its partition (as MuSim does) |
| `host_write` | `mu_write` | `host_pages += n` |
| `verify` | `mu_verify` | block valid counts vs bwritten-binvalid, extent count, plus invariants of the implicit representation (extents contiguous, same block, <= MAXE; leaf fronts/counts) |
| report `mem.total_bytes` | `mu_mem` | `cache_bytes + bitmap_cache_bytes + NB` (1 B/block invalid counter) |

### Program / read source mapping
| ftlsim | dm |
|---|---|
| `prog[S_HOST]` | `DM_P_HOST` (ppn) |
| `prog[S_PGC]` (GC copies) | `DM_P_GC` (ppn) |
| `prog[S_MAP]` (dirty μ-tree / bitmap leaf evicted) | `DM_P_MAP`, `ppn = DM_NOPPN` |
| `read_gc_copy` | `DM_R_GC` (ppn, one per copied page, issued before the copy's programs) |
| `read_map` / `map_miss` (cache miss in `touch_leaf`) | `DM_R_MAP`, `ppn = DM_NOPPN`, and `st->map_miss++` at the same place |
| (no read path in ftlsim) | `mu_read`: touch (clean) the leaf holding lpn — a miss costs one `DM_R_MAP` + `map_miss` (and possibly a dirty eviction `DM_P_MAP`) — then `DM_R_HOST` at `pmap[lpn]` if mapped |

`flush()` is a no-op: ftlsim never writes back dirty tree pages at the end, so neither do we (parity).

## Parameters (defaults = ftlsim)
`cache_bytes` = 12 MiB (12582912), `bitmap_cache_bytes` = 32768, `mu_parts` = 8, `mu_max_extent` = 128, `mu_leaf_fanout` = 256.
Geometry (L, P, NB, T) from `dm_geom`.

## Representation (kernel-friendly) and why it is equivalent
MuSim keeps `vector<Leaf{id, vector<Ext{l,p,n}>}>`. The port keeps the extent map implicitly:
- `pmap[L]` (u32 lpn->ppn) and an **extent-start bitmap** (1 bit/page). An extent = a start bit followed by mapped pages
  up to the next start bit or the first unmapped page. Extents never exceed MAXE pages and never cross a block, so
  `x.p + (lpn - x.l) == pmap[lpn]`, and MuSim's merge test `pr.l+pr.n==x.l && pr.p+pr.n==x.p && same block` becomes
  `pmap[l-1] != NONE && pmap[l-1]+1 == p && same block` (pred length = `l - prev_start(l-1)`).
- leaves: a sorted array of `{front lpn, extent count, leaf id}` (16 B). A leaf owns the extents whose start is in
  `[front_i, front_{i+1})`; split selects the `cnt/2`-th start bit from the front (popcount walk); fronts are updated
  when the first extent is deleted (case 0) or trimmed left (case 3). Leaf ids are allocated in the same order as
  `next_leaf_id++`, so the cache sees identical keys.
- `leaf_for` search acceleration (performance only, results unchanged): MuSim's binary search returns the last
  leaf with `cnt && front <= l` (leaf 0 if none). Fronts strictly increase and an empty leaf exists only when it
  is the only leaf (both checked by `verify`), so the predicate is monotone and the answer is the unique
  last-true index; any window `[lo, hi)` with `(lo == 0 || pred(lo))` and `(hi == nleaves || !pred(hi))` gives the
  same leaf. The core tries, in order: the previous result (`lhint`, typically unmap_range -> insert_ext on the
  same lpn), the guide window `lg[l >> 12] .. lg[(l >> 12) + 1]` (one u32 per 4096 logical pages, shifted by
  +-1 on leaf insert/remove, repaired after a failed check), then the full search. Each candidate is verified
  exactly before use, so a stale hint only costs time. Searches are branch-free.
- LRU touch of the page that is already MRU (unmap_range then insert_ext on the same leaf) returns early:
  moving the head to the front is a no-op, so cache contents and dirty bits are unchanged.
- `p2l[NP]` (OOB) for GC, per-block `btag/binvalid/bwritten`, free list, `upd[parts]`.
- LRU cache: preallocated pool of `cache_pages` nodes + a chained hash (power-of-two buckets), O(1) touch.
- **libc++ hash-table model.** `bm_flush` touches bitmap leaves in the iteration order of an `unordered_set<u64>` built
  from iterating `unordered_map<u32,u32> bm_runs`; with a small cache this order changes which pages get evicted, so it
  affects `prog.map`. ftlsim is built with zig c++ (libc++), so `mu_lht_*` reimplements exactly libc++'s
  `__hash_table` for integer keys: identity hash, `__constrain_hash` (mask for power-of-two bucket counts, else `%`),
  growth `max(2*bc + !is_hash_pow2(bc), size+1)` rounded to the next prime (1 -> 2), `__do_rehash` splicing, insert at
  bucket head / list head, `remove` bucket fix-ups, `clear()` keeping the bucket count. Keys are block ids (< NB) and
  group ids, so nodes are index arrays preallocated in create(). (If ftlsim were rebuilt with libstdc++, only
  `prog.map` under very small caches could differ.)

## Validation (userspace harness vs ftlsim; op=0.09278, auto P, U=16)
```
$ ./validate.sh mu "mode=mu op=0.09278 cache_bytes=6291456" "params=cache_bytes=6291456"
skew  ftlsim(WAF prog erases)=[12.8517 48657420 7986]  harness(WAF prog erases order_viol)=[12.8517 48657420 7986 0]
rand4k  ftlsim(WAF prog erases)=[13.5208 38277463 4908]  harness(WAF prog erases order_viol)=[13.5208 38277463 4908 0]
kvsmoke  ftlsim(WAF prog erases)=[15.9296 309866703 6780]  harness(WAF prog erases order_viol)=[15.9296 309866703 6780 0]
PARITY: bit-identical

$ ./validate.sh mu "mode=mu op=0.09278 cache_bytes=409600" "params=cache_bytes=409600"
skew  ftlsim(WAF prog erases)=[13.0153 49276813 7986]  harness(WAF prog erases order_viol)=[13.0153 49276813 7986 0]
rand4k  ftlsim(WAF prog erases)=[19.2159 54400261 4908]  harness(WAF prog erases order_viol)=[19.2159 54400261 4908 0]
kvsmoke  ftlsim(WAF prog erases)=[15.9364 309999518 6780]  harness(WAF prog erases order_viol)=[15.9364 309999518 6780 0]
PARITY: bit-identical

$ ./validate.sh mu "mode=mu op=0.09278 cache_bytes=409600 bitmap_cache_bytes=4096 mu_parts=4 mu_max_extent=64 mu_leaf_fanout=64" \
                   "params=cache_bytes=409600,bitmap_cache_bytes=4096,mu_parts=4,mu_max_extent=64,mu_leaf_fanout=64"
skew  ftlsim(WAF prog erases)=[15.4949 58664696 9273]  harness(WAF prog erases order_viol)=[15.4949 58664696 9273 0]
rand4k  ftlsim(WAF prog erases)=[45.0316 127484774 10942]  harness(WAF prog erases order_viol)=[45.0316 127484774 10942 0]
kvsmoke  ftlsim(WAF prog erases)=[24.5619 477784839 10400]  harness(WAF prog erases order_viol)=[24.5619 477784839 10400 0]
PARITY: bit-identical
```
Mapping-I/O counters (ftlsim `reads.map` / `events.map_miss` / `prog.map` vs harness `read.map` / `map_miss` / `prog.map`), all equal:
```
cache 6291456: skew 223/223/0          rand4k 8434916/8434916/8434916    kvsmoke 352/352/0
cache  409600: skew 619393/619393/619393  rand4k 24557714/24557714/24557714  kvsmoke 133122/133122/132815
```
`reads.gc_copy` == harness `read.gc` and ftlsim `mem.total_bytes` == harness `mem_bytes` in every run.
Run times: ftlsim <= 60 s per trace, harness ~1-8 s (the 61 GiB kv trace is not slow for μ-FTL).
Read path smoke test (`reads=1`, skew, 400 KB cache): 404664 host data reads, map reads 619393 -> 627827; verify ok.

Kernel: `make -j16` builds `nvmev.ko`; no warnings in ftl_mu.c (only "compiler differs"), modpost reports no undefined symbols.

## RAM model / real footprint
- Reported (`mem_bytes`, = ftlsim): `cache_bytes + bitmap_cache_bytes + NB` (the μ-tree itself lives in flash).
- Actual emulator DRAM (the emulator must hold the whole map to serve reads): `pmap` 4 B x L + start bitmap L/8 +
  `p2l` 4 B x NB·P + leaves 16 B each (initial capacity max(1024, L/256)) + cache pool 24 B x cache_pages + O(NB).
  At 64 GiB logical (L = 16 Mi pages, ~18.3 Mi physical pages): ~64 + 2 + 70 + 1 MiB ≈ 137 MiB
  (harness peak RSS on the 61 GiB kv trace: 134 MiB), i.e. about page4k's footprint; no per-extent objects.

## Kernel-specific concerns
- No FPU, no libgcc builtins (`dm_popcount64/dm_ctz64/dm_clz64`), all helpers prefixed `mu_`.
- All arrays allocated in `create()` (vzalloc). The only run-time allocation: the leaf array doubles (vzalloc + memcpy
  + vfree) when the number of leaves exceeds its capacity (bounded by #extents <= L; with the initial L/256 capacity
  it happens only for extremely fragmented maps). It runs in the NVMe dispatcher's process context (the glue holds no
  spinlock around `dm_policy.write`), never in an atomic/FPU section.
- Leaf insert/remove is a memmove of the leaf array (as MuSim's `vector::insert`): O(#leaves) x 16 B per split
  (~1k leaves after seq fill at 64 GiB, 131k = 2 MiB after 2x-capacity 4K random writes at 92 GiB), plus a
  pass over the 23 KB guide. Measured: ~2-3% of core time on that trace (a blocked leaf array was tried; it
  removed the memmove but its two-level search was slower overall, 85 s vs 62 s, so it was not kept).
- `mu_next_start` / `mu_select` scan the start bitmap word-by-word; scans are bounded by MAXE pages except the leaf
  split / front update, which can cross unmapped gaps on a sparsely written (not seq-filled) device.
- Victim selection is O(NB x parts) per GC victim (NB ≈ 373 with the auto P).
- Preemption: `dm_cond_resched()` (dm_port.h: `cond_resched()` in the kernel, no-op in userspace) every 1024
  victim pages inside the GC copy loop and after every victim erase, every 1M pages / 4096 leaves in `verify`,
  every 16M entries of the create() p2l init. The core holds no lock and uses no FPU; the glue calls it from the
  NVMeVirt dispatcher kthread (process context). See "CPU cost" below.

## Deviations
- None in counted behavior. The `[cfg]` line omits `zipf` and computes the GB figure with integer rounding.
- `report()` prints `victim_valid_sum` (integer) instead of `avg_victim_valid` (no floats); `map_flush` = `prog.map`.
- Read path is new (ftlsim has none) — see mapping table above.

## CPU cost and preemption (2026-10-01)
Trigger: NVMeVirt OP 7% (NB=373, P=67584, L=23559648, `cache_bytes=17669736`), seq precondition then 4 KiB random
writes QD32: `soft lockup - CPU#94 stuck for 23s/49s [nvmev_dispatche]`, NVMe timeouts, controller disabled
(kernel log of that run: 34.07M host pages, 127.7M GC programs, 20.7M map programs in ~1020 s, i.e. <= 3.3 us per
flash op in the glue).

Userspace reproduction: harness seq fill + uniform 4 KiB random writes over the dense space, `capacity=96500318208
op=0.07001 P=67584 params=cache_bytes=17669736` -> `[cfg] NB=373 P=67584` (op=0.07 gives NB=372 in the harness:
23559648*1.07/67584 = 372.99). Traces: 2x capacity (47.1M writes) and its quarter-capacity prefix. Instrumented
copy of replay.c: clock_gettime around every `dm_policy.write`, flash ops counted per call; profile by an
in-process SIGPROF PC sampler + addr2line (perf is blocked: perf_event_paranoid=4; gprof misattributes the
isra clones).

Hotspots (before; 2x trace, 138 s inside write(); shares are of all 144k 1-ms samples of the run, which
include ~16% trace parsing / harness):
1. The worst calls are GC: at 7% OP a victim holds ~63k valid of 67584 pages (victim_valid_sum/gc = 721M/11435),
   so one `mu_gc` (loop until free >= T+2) copies millions of single-page runs; the worst write() issued
   16.35M flash ops (GC reads/programs + mu-tree map reads/programs) and took 1.05 s. That op count is MuSim's.
2. Per copied page, `mu_leaf_for` ran twice (unmap_range, insert_ext): a binary search over up to 131k leaves
   (2 MiB array, ~17 mispredicted branches + cache misses): `mu_leaf_for` 49.2% of samples.
3. Random `pmap[]` misses in the GC valid check (`mu_gc` self 17.5%) and in the copy's unmap/insert
   (`mu_unmap_range` 5.7%).
4. LRU touch (`mu_touch`/`mu_cfind`/`mu_cunlink`/`mu_cremove`/`mu_cpush_front` 11.7%), twice per copy on the
   same leaf.
Fixes: (2) hint + guide + branch-free search (see Representation), (3) GC prefetch of `pmap[p2l[s0+16]]` and
its estart word, (4) MRU early return; plus `dm_cond_resched()` in GC.

Timing (userspace, wall time inside `dm_policy.write`, results identical before/after):
| trace | version | write time | per host page | worst call | calls > 100 ms | > 1 s |
|---|---|---|---|---|---|---|
| 0.25x | before | 5.04 s | 171 ns | 232.8 ms (6.24M ops) | 7 | 0 |
| 0.25x | after  | 2.99 s | 101 ns | 98.1 ms (6.24M ops) | 0 | 0 |
| 2x    | before | 138.4 s | 1958 ns | 1051 ms (16.35M ops) | 451 | 2 |
| 2x    | after  | 60.1 s | 851 ns | 428 ms (15.7M ops; the 16.35M-op call: 413 ms) | 95 | 0 |
(per host page = write time / pages handed to the core incl. the 23.5M-page seq fill.) Results on the 2x trace
(identical JSON before/after): WAF 27.3059, total_prog 643318342 (host 23559665, gc 382792369, map 236966308),
erases 6050, map_miss 236966308, gc 11435, extents_end 23127962, leaves_end 131679. Read path (skew, reads=1,
cache 400 KB) also identical.
The "few ms per call" target is not reachable without changing μ-FTL's semantics: the worst call's ~16M flash
ops are what MuSim's synchronous GC issues (~26 ns/op left in userspace, mostly memory latency and callbacks);
bounding it needs incremental GC, which changes later allocation and therefore the results. With
`cond_resched()` the dispatcher no longer trips the soft-lockup watchdog, but such a command still takes
(#ops x glue cost per op) wall time in the kernel.

**Glue note (not changed, shared code):** `channel_model.c:chmodel_request()` prints `NVMEV_ERROR("... Need to
increase array size ...")` (printk KERN_ERR, not rate-limited) for every request that starts more than
NR_CREDIT_ENTRIES x 4 us = 393 ms in the future. One μ-FTL GC call queues millions of programs (16 LUNs x
185 us per oneshot program), i.e. seconds of NAND backlog, so nearly every following NAND command logs a line;
console loglevel is 4 (KERN_ERR reaches the console) and `/var/log/kern.log` has grown to 13.9 GB since Sep 23.
This is the likely dominant per-op cost in the kernel; suggest `printk_ratelimited`/NVMEV_DEBUG there in the
shared NVMeVirt code.

Validation after the change (all three NOTES lines re-run): PARITY: bit-identical, same numbers as above.
Kernel: `make -j8` (6.8.0-142) builds nvmev.ko, no warnings from dm/, every undefined symbol of nvmev.ko is in
the kernel's Module.symvers (`__SCT__cond_resched` included).
