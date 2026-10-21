# dftl core (dm/ftl_dftl.c) — port notes

Port of `ftlsim mode=dftl` (DFTL, Gupta et al. ASPLOS'09; `cftl` is an ftlsim alias).

## What was ported (ftlsim.cpp, struct Sim)
| ftlsim | dm/ftl_dftl.c |
|---|---|
| `page_write` (is_pg path, `fine_alloc_page` host/GC frontiers) | `dftl_page_write`, `dftl_alloc_page` |
| `page_gc` (greedy, ties -> lowest id, frontiers excluded; `cmt_gc_flush` after emptying, before erase) | `dftl_do_gc` |
| `alloc_block` (GC while free <= T, no re-entry; LIFO free list, block 0 first) | `dftl_alloc_block` |
| `erase_block` | `dftl_erase_block` |
| `cmt_update` (host write) | `dftl_cmt_update` / `dftl_miss(dirty=1)` |
| `cmt_evict_one` (dirty victim: read-modify-write of its translation page, clean all cached entries of that page) | `dftl_evict_one` |
| `cmt_gc_update`, `cmt_gc_flush` (GC batch translation-page updates) | `dftl_cmt_gc_update`, `dftl_cmt_gc_flush` |
| `tp_buf` / `dftl_tpbuf` | `f->tp_buf` / `f->tpbuf` |
| `report()` RAM model | `dftl_mem` |

Data path = `ftl_page4k.c` verbatim; the full `pmap` stands for the on-flash translation pages (as in
ftlsim), only their I/O is modeled. Translation-page I/O goes out as `fo.read/prog(ctx, DM_NOPPN,
DM_R_MAP/DM_P_MAP)` and is counted in `read[DM_R_MAP]`, `prog[DM_P_MAP]`, `map_miss` at exactly the points
where ftlsim increments `read_map`, `prog[S_MAP]`, `map_miss` (same order: miss read before evictions).

### CMT data structures (kernel-safe, no per-op allocation)
- Pool of `cmt_cap + 1` nodes `{lpn, prev, next, hnext, dirty}` (u32 indices, 20 B) allocated in create();
  doubly linked LRU (head = MRU, evict tail = ftlsim `cmt_lru.back()`), chained hash (power-of-two buckets
  >= cmt_cap+1, Fibonacci hash), free-node list.
- GC translation-page set (`gc_tps`): byte bitmap over `ceil(L/1024)` translation pages + a list of at most
  P entries (one insert per relocated page). ftlsim only counts it, so order is irrelevant.

### cmt_cap (exact)
`cmt_cap = (u64)(cache_bytes * 0.7 / 8.0)` evaluated as IEEE double in ftlsim. For 409600 the product is an
exact integer (35840): an integer shortcut such as `floor(cb*M/2^56)` would give 35839, so the double
rounding must be reproduced. `dftl_cmt_cap()`
emulates the double exactly without FPU: 0.7 = 0x16666666666666 / 2^53, 128-bit product, round-to-nearest-even
to 53 significant bits, then >> 56 (/2^53, /8, truncate). Checked against Python doubles for 400k random
values (incl. multiples of 80): 0 mismatches. Results: 409600 -> 35840, 6291456 -> 550502, 12 MiB -> 1101004.
The u128 ops (mul/shift) compile inline on x86_64 (no libgcc symbols; modpost clean).

### RAM model (`mem_bytes`)
ftlsim: `hash_bytes(cmt.size(), 8) + (L/1024)*4` printed `%.0f`. Integer: `(80*size + 3)/7 + (L/1024)*4`
(size*8/0.7 = 80*size/7 + epsilon; the fraction k/7 is never near .5, so this equals the rounded double;
checked for 200k values). Matches ftlsim `mem.total_bytes` on all runs below.

## Read path (ftlsim has none) — DFTL read
Per page: CMT lookup. Hit -> move to MRU (dirty bit unchanged). Miss -> `map_miss++`, one translation-page
read `DM_R_MAP` (skipped if `dftl_tpbuf` and it is the page held in SRAM), evict until size < cap (dirty
eviction = translation-page write-back, same as the write path), `tp_buf = tp`, insert the entry CLEAN at MRU.
Then the data read `DM_R_HOST` if the lpn is mapped. Reads therefore compete with writes for CMT space and
can change WAF (they are off in validate.sh; `reads=1` exercises them: verify ok, 0 order violations).

## Parameters (dm_params / harness `params=`)
- `cache_bytes` (default 12 MiB = ftlsim default), K/M/G suffixes.
- `dftl_tpbuf` (default 0 = ftlsim default).
Options may be separated by spaces or commas (see "glue" below).

## Validation (verbatim)
Note: validate.sh word-splits `$HARGS`, so `"params=cache_bytes=409600 dftl_tpbuf=1"` reaches replay as two
args and replay aborts (`unknown key dftl_tpbuf`). The commands therefore use a comma:

```
./validate.sh dftl "mode=dftl op=0.09278 cache_bytes=409600 dftl_tpbuf=1" "params=cache_bytes=409600,dftl_tpbuf=1"
skew  ftlsim(WAF prog erases)=[5.3024 20075242 3188]  harness(WAF prog erases order_viol)=[5.3024 20075242 3188 0]
rand4k  ftlsim(WAF prog erases)=[8.4724 23985291 3006]  harness(WAF prog erases order_viol)=[8.4724 23985291 3006 0]
kvsmoke  ftlsim(WAF prog erases)=[2.448 47618297 1032]  harness(WAF prog erases order_viol)=[2.448 47618297 1032 0]
PARITY: bit-identical

./validate.sh dftl "mode=dftl op=0.09278 cache_bytes=409600 dftl_tpbuf=0" "params=cache_bytes=409600,dftl_tpbuf=0"
skew  ftlsim(WAF prog erases)=[5.3024 20075242 3188]  harness(WAF prog erases order_viol)=[5.3024 20075242 3188 0]
rand4k  ftlsim(WAF prog erases)=[8.4724 23985291 3006]  harness(WAF prog erases order_viol)=[8.4724 23985291 3006 0]
kvsmoke  ftlsim(WAF prog erases)=[2.448 47618297 1032]  harness(WAF prog erases order_viol)=[2.448 47618297 1032 0]
PARITY: bit-identical

./validate.sh dftl "mode=dftl op=0.09278 cache_bytes=6291456 dftl_tpbuf=1" "params=cache_bytes=6291456,dftl_tpbuf=1"
skew  ftlsim(WAF prog erases)=[5.2146 19742939 3188]  harness(WAF prog erases order_viol)=[5.2146 19742939 3188 0]
rand4k  ftlsim(WAF prog erases)=[8.1186 22983875 3006]  harness(WAF prog erases order_viol)=[8.1186 22983875 3006 0]
kvsmoke  ftlsim(WAF prog erases)=[2.4479 47617504 1032]  harness(WAF prog erases order_viol)=[2.4479 47617504 1032 0]
PARITY: bit-identical
```
(tpbuf only changes translation-page READS, so tpbuf=0/1 have identical programs — the read counters below
are what distinguish them.)

Map-I/O parity (ftlsim JSON `reads.map, map_miss, prog.map, mem.total_bytes` vs harness JSON
`read.map, map_miss, prog.map, mem_bytes`), binaries run directly as validate.sh does:
```
skew   cb=409600  tpbuf=1: ftlsim=(584072, 1695427, 483629, 417792)   harness=(584072, 1695427, 483629, 417792)   MATCH
skew   cb=409600  tpbuf=0: ftlsim=(2179056, 1695427, 483629, 417792)  harness=(2179056, 1695427, 483629, 417792)  MATCH
rand4k cb=6291456 tpbuf=1: ftlsim=(6594250, 2087953, 4510162, 6299643) harness=(6594250, 2087953, 4510162, 6299643) MATCH
kv     cb=409600  tpbuf=1: ftlsim=(100060, 19452463, 68642, 472248)   harness=(100060, 19452463, 68642, 472248)   MATCH
```
Kernel: `make -j16` -> nvmev.ko, only "compiler differs" warning, modpost no undefined symbols.

## Deviations
- None in the write/GC path. Read path is new (above). `verify()` also checks CMT invariants (LRU links,
  every node findable, size <= cap, GC set empty).
- Eviction cleaning loop is clamped to lpn < L (entries beyond L can't be cached; same result).
- ftlsim's `map_flush` counter is not reported separately (always == `prog[DM_P_MAP]`).

## Kernel concerns
- Memory at 64 GiB logical (L = 16.7 M): pmap 64 MiB + p2l ~70 MiB (same as page4k; the full table is kept
  to model translation-page contents), CMT at the 12 MiB default: 1.1 M nodes x 20 B = 22 MiB + 8 MiB buckets;
  gc_tp bitmap 16 KiB, gc_tp list 4*P B. All vzalloc'd in create(); create() also does one small
  params copy. No per-I/O allocation, no FPU.
- Eviction does up to 1024 hash lookups per dirty eviction (as ftlsim); O(1) otherwise.

## Glue change suggested
- `dm/harness/validate.sh`: pass `$HARGS` so multi-option `params=` survive (e.g. `eval` it, or make replay
  accept commas). Until then this core accepts `,` as an option separator; kernel `dm_params` works with
  spaces as documented.
