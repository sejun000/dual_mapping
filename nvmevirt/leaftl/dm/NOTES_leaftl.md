# leaftl core (dm/ftl_leaftl.c)

Port of `ftlsim mode=leaftl` (LeaFTL, Sun et al., ASPLOS'23; ftlsim's port of platformxlab/LeaFTL
`wiscsim/learnedftl.py`). Result: **bit-identical** with ftlsim (WAF, total programs, erases, and also
`reads.map`, `map_miss`, `mem.total_bytes`, `leaftl.segments/accurate/consecutive/full_table_bytes/frames/lookups`)
on all three validation traces, for three configurations (small cache, large cache, gamma 4).

## What was ported (ftlsim.cpp, struct Sim)
| ftlsim | here |
|---|---|
| `page_write`, `fine_alloc_page` (host frontier / GC frontier), `pick_victim(B_FINE)`, `erase_block` | `leaftl_page_write`, `leaftl_alloc_page`, `leaftl_pick_victim`, `leaftl_erase_block` (same as page4k) |
| `alloc_block` (is_pg path incl. the dead-block escape when a GC round frees no net block) | `leaftl_alloc_block` |
| `page_gc`, `if (is_lf())` branch: OOB scan, LPN sort (`leaftl_gcsort`), relocate, erase, relearn | `leaftl_page_gc` |
| `host_write` -> `lf_host` (stage, duplicate -> flush first, flush at `leaftl_batch`) | `leaftl_write` / `leaftl_host` |
| `lf_flush_stage` (sort, `lf_touch_lookup` for mapped LPNs, program, drop pairs GC already moved, `lf_insert`) | `leaftl_flush_stage` |
| `lf_learn` + `lf_emit` + `pl_from` + `f16` (greedy PLR state machine, gamma, float16 slope, llround accuracy test, consecutive test) | `leaftl_learn` / `leaftl_emit` / `leaftl_f16` / `leaftl_llround` |
| `lf_insert` (per-frame learning, shadowed coverage removed from older segments, new segs first) | `leaftl_insert` |
| `seg_mem` / `lf_set_mem` (5 B single, 8 B consecutive, 8 B + boundary runs, +4 B/frame) | `leaftl_set_mem` |
| `lf_make_resident`, `lf_enforce` (LRU, budget `cache_bytes`, dirty eviction = 1 map program, +1 map read if partial), `lf_touch_lookup` | same names with `leaftl_` prefix |
| `lf_predict` / `lf_verify` | `leaftl_predict_fp` / `leaftl_verify_fp` (+ structural checks in `leaftl_verify`) |
| RAM model `mem.total_bytes` = resident frame bytes + 4 B x ceil(L/256) (GTD) | `leaftl_mem` |
| `"leaftl"` JSON block | `leaftl_report` (same keys + `gamma_u`, `resident`, `cached_bytes`, `read_mispredict`, `read_oob_fail`) |

Flash-op mapping: GC copy read = `DM_R_GC` at the old ppn; frame load / partial merge = `DM_R_MAP` at
`DM_NOPPN` (+`map_miss` exactly where ftlsim counts it); dirty frame eviction = `DM_P_MAP` at `DM_NOPPN`
(counted in total programs / WAF, as in ftlsim's `prog.map`). `lookups` is not reset at warmup (ftlsim keeps it
outside `Stats` too).

## Parameters (defaults = ftlsim defaults)
| param | default | meaning |
|---|---|---|
| `cache_bytes` | 12582912 (12 MiB) | DRAM budget for resident frames (LRU) |
| `leaftl_gamma_u` | 100 (= 1e-4) | PLR error bound in micro-units; gamma = (double)gamma_u / 1e6 (same double as ftlsim's `1e-4` / `atof`) |
| `leaftl_batch` | 2048 | host pages sorted per flash-allocation batch |
| `leaftl_gcsort` | 1 | GC relocates in LPN order |

## Floating point
The PLR must reproduce ftlsim's doubles bit for bit. Important finding: the reference binary is built by
`zig c++ -O2` for the native CPU, so clang contracted several `a*b+c` into FMAs. I read the disassembly of
`Sim::lf_insert` (lf_learn inlined), `Sim::lf_emit` and `Sim::verify` and reproduced the exact placement with
`__builtin_fma`:
`b = fma(-k, x1, y1)` (pl_from), `sy = fma(up.k, lo.b, -(lo.k*up.b)) / (up.k - lo.k)`, bound tests
`fma(x, k, b)`, emitted intercept `fma(-k, sx, sy)`, accuracy / prediction `llround(fma(x, k, b))`. Everything
else is plain IEEE ops in the same order. `f16()` (frexp/nearbyint/ldexp) is done exactly on the bit pattern
(round significand to 11 bits, ties-to-even; subnormal via exact 2^64 scaling). `llround` = trunc + exact
fractional test (no libm).

Kernel safety: all FP code is in `noinline` functions with `target("sse2,fma")` (learn, predict, verify range,
gamma conversion), called only between `kernel_fpu_begin()/kernel_fpu_end()`; each FPU section is short
(one frame <= 256 points; verify in 65536-LPN slices) and contains no allocation, sleeping or locking. The rest
of the file is under `#pragma GCC target("general-regs-only")` (placed before the includes), so gcc cannot
emit SSE/AVX (e.g. vectorised bitmap loops or struct copies) outside FPU sections; doubles are stored as u64
bit patterns so non-FP code only moves integers. Checked with objdump: xmm/ymm instructions appear only in
`leaftl_learn`, `leaftl_predict_fp`, `leaftl_verify_fp`, `leaftl_gamma_bits` (kernel object and harness).
create() refuses to run on a CPU without FMA/AVX. Kbuild: this object gets `$(CC_FLAGS_FPU)` /
`CFLAGS_REMOVE $(CC_FLAGS_NO_FPU)`, with a fallback (`-mhard-float -msse -msse2` / `-mno-sse -mno-sse2`)
because x86 kernels before 6.10 (this machine: 6.8) do not define `CC_FLAGS_FPU`.
Undefined symbols of the object: `kernel_fpu_begin_mask`, `kernel_fpu_end`, `boot_cpu_data` (all exported);
no libm/libgcc calls.

## Data structures / memory (kernel concerns)
- `pmap` 4 B x L, `p2l` 4 B x NB*P, per-block arrays (as page4k; ftlsim also keeps pmap as the OOB/ground truth).
- Frames: 24 B each (`segs` pointer, LRU prev/next, mem, nseg, class, flags) for ceil(L/256) frames.
- Coverage: segments of one frame have disjoint coverage (shadowing removes it), so instead of ftlsim's
  256-bit bitmap per segment there is a per-LPN owner byte `own[lpn]` = segment index (L bytes total).
  A frame has at most 256 segments; 0xFF means "none" unless the frame holds exactly 256 segments (then every
  LPN is covered). Boundary runs for `seg_mem` are counted in one pass over the owner bytes.
- Segment: 24 B (k, b as bit patterns, x1/x2 frame offsets, accurate, consecutive). Per-frame arrays come from
  a pooled allocator (size classes 1..256 segments, 256 KiB chunks from `dm_zalloc`, free lists); a chunk is
  allocated only when a class runs dry (write path, never inside an FPU section).
- Stage: `leaftl_batch` u32 + open-addressing hash set (4 x batch slots, memset on flush) for duplicate detection.
- Scratch (learned segments, remap tables) lives in the core struct (no big stack frames).
- 64 GiB logical (L = 16.8 M pages, 65536 frames): pmap 64 MiB, p2l ~70 MiB, owner 16 MiB, frames 1.5 MiB,
  segments 24 B each (+ <2x pool slack). Sequential/KV data: ~1 segment per frame (kvsmoke: 72.5 K segments,
  < 4 MiB). Worst case (pure 4K random) approaches 1 segment per page (rand4k at 8 GiB: 1.66 M segments for 2 M
  pages) -> ~400 MiB at 64 GiB (ftlsim needs ~64 B + vector overhead per segment for the same thing).

## Read path (not in ftlsim; not part of the parity check)
Host read of an LPN (the glue already served write-buffer hits):
1. LPN still in the staging batch -> served from the controller buffer, no flash op.
2. Frame never written (no GTD entry) -> nothing. Otherwise `lf_touch_lookup` (frame not resident -> one
   `DM_R_MAP` read + `map_miss`; resident but partial -> merge read), then `lf_enforce` (reads would otherwise
   grow the cache without bound; the write path keeps ftlsim's behaviour of enforcing only after inserts).
3. LPN not covered -> unmapped, nothing. Else predict `llround(fma(lpn, k, b))`.
4. Accurate segment -> one `DM_R_HOST` read.
5. Approximate segment -> artifact `FlashMetadata.lpn_to_ppn`: window W = min(2*gamma, 128 B OOB / 4) = 32 max
   (`num_p2l_entries`), integer `floor(2*gamma_u/1e6)`; pmap stands in for the OOB contents.
   - |real - pred| <= W: if a page in the 16-entry `last_oob_page` FIFO covers real (|real - p| <= W) or
     pred == real -> 1 read (`DM_R_HOST` at real); else `DM_R_OTHER` at pred + `DM_R_HOST` at real. pred is
     pushed into the FIFO.
   - else (wrong block): `DM_R_OTHER` at pred, then the neighbouring block's boundary page (previous block's
     last page if pred is in the first half of its block, else next block's first page); if that is the real
     page it is the data read (`DM_R_HOST`), else it is `DM_R_OTHER` and then `DM_R_HOST` at real. If the
     neighbour's OOB window does not cover real the artifact would assert; we count `read_oob_fail` and still
     read the real page.
   Predicted/neighbour ppns outside the device are clamped for the flash op.
   With gamma = 1e-4, W = 0, so the few approximate segments (float16 slope rounding makes ~0.02% of segments
   approximate) always take the wrong-block path; smoke run skew reads=1: 9 mispredictions, 7 oob_fail.
   gamma = 4 (W = 8): 40291 mispredictions, 3364 extra `DM_R_OTHER` reads (FIFO absorbs most).

## Deviations
- None on the write/GC/mapping path (bit-identical). Coverage representation differs (owner bytes instead of
  per-segment bitmaps) but is semantically identical (coverage is disjoint), and segment order is kept.
- Read path is new (see above); the read-path `lf_enforce` means runs with `reads=1` are not comparable to
  ftlsim (which ignores reads).
- `map_flush` (ftlsim) has no dm_stats counterpart; it equals `prog.map`.

## Validation (dm/harness)
```
./validate.sh leaftl "mode=leaftl op=0.09278 cache_bytes=409600" "params=cache_bytes=409600"
skew  ftlsim(WAF prog erases)=[5.2572 19904245 3201]  harness(WAF prog erases order_viol)=[5.2572 19904245 3201 0]
rand4k  ftlsim(WAF prog erases)=[10.6401 30122234 3005]  harness(WAF prog erases order_viol)=[10.6401 30122234 3005 0]
kvsmoke  ftlsim(WAF prog erases)=[2.4474 47607133 1031]  harness(WAF prog erases order_viol)=[2.4474 47607133 1031 0]
PARITY: bit-identical
./validate.sh leaftl "mode=leaftl op=0.09278 cache_bytes=6291456" "params=cache_bytes=6291456"
skew  ftlsim(WAF prog erases)=[5.1957 19671133 3201]  harness(WAF prog erases order_viol)=[5.1957 19671133 3201 0]
rand4k  ftlsim(WAF prog erases)=[8.0088 22672897 3005]  harness(WAF prog erases order_viol)=[8.0088 22672897 3005 0]
kvsmoke  ftlsim(WAF prog erases)=[2.4422 47505727 1031]  harness(WAF prog erases order_viol)=[2.4422 47505727 1031 0]
PARITY: bit-identical
./validate.sh leaftl "mode=leaftl op=0.09278 cache_bytes=409600 leaftl_gamma=4" "params=cache_bytes=409600,leaftl_gamma_u=4000000"
skew  ftlsim(WAF prog erases)=[5.2466 19864039 3201]  harness(WAF prog erases order_viol)=[5.2466 19864039 3201 0]
rand4k  ftlsim(WAF prog erases)=[10.6244 30077723 3005]  harness(WAF prog erases order_viol)=[10.6244 30077723 3005 0]
kvsmoke  ftlsim(WAF prog erases)=[2.4474 47607091 1031]  harness(WAF prog erases order_viol)=[2.4474 47607091 1031 0]
PARITY: bit-identical
```
Extra fields (ftlsim vs harness, identical in every case):
```
config      trace   reads.map  map_miss  mem.total_bytes  segments  accurate  consecutive  full_table_bytes  frames  lookups
c=400K      skew       232260     45300           442358    111780    111762       111780            899786    8192  7320954
c=400K      rand4k   11884066   2463296           442338   1656426   1656353      1656426           9627968    8192  5661312
c=400K      kvsmoke     96541     76107           660204     72521     72520        72521            829446   62651 38907663
c=6M        skew            0         0           932554    111780    111762       111780            899786    8192  7320954
c=6M        rand4k    4249492   1814063          6324188   1656426   1656353      1656426           9627968    8192  5661312
c=6M        kvsmoke         0         0          1080050     72521     72520        72521            829446   62651 38907663
c=400K,g=4  skew       191777     39455           442334     93121     77499        76820            793938    8192  7320954
c=400K,g=4  rand4k   11839065   2456441           441284   1490209   1450375      1380288           9158222    8192  5661312
c=400K,g=4  kvsmoke     96502     76107           660195     72456     72398        72391            829177   62651 38907663
```
Kernel build: `make -j4` -> nvmev.ko, no warnings besides "compiler differs", modpost clean.

## Glue changes that would help (not made)
- None required. Optional: a per-namespace hint to call `flush()` (drain the staging batch) on NVMe Flush /
  FUA writes; today staged pages are only persisted when the batch fills, a duplicate arrives, or at removal
  (the glue's timing model does not need it, but a real device would).
