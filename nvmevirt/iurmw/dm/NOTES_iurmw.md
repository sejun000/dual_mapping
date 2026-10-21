# iurmw — coarse IU mapping + read-modify-write (ftlsim `mode=iu_rmw`)

## What was ported (ftlsim.cpp, struct Sim)
| dm/ftl_iurmw.c | ftlsim |
|---|---|
| `iurmw_write` | `host_write`, `iu_rmw` branch: per IU slice [s,e), w=e-s; w<U -> `read_rmw += U-w`, `coarse_write(iu, S_HOST, w, S_RMW)`; w==U -> `coarse_write(iu, S_HOST)` |
| `iurmw_coarse_write` | `coarse_write` + `install_extent` + `invalidate_iu` (no fine tier in this mode, so invalidate = old extent `bvalid -= U`) |
| `iurmw_alloc_extent` | `coarse_alloc_extent` with coarse_class=0, gc_frontier=1: host -> `cf`, GC -> `cgf` (the straddle pad is kept but unreachable since P % U == 0) |
| `iurmw_coarse_gc` | `coarse_gc` with `cgc_vict=greedy`, unfold=off: per extent slot, dead (coarse_map != ppn) skipped, live -> `read_gc_copy += U`, `coarse_write(iu, S_CGC_CLEAN)` on the GC frontier; `in_cgc` blocks nested GC |
| `iurmw_pick_victim` | `pick_victim(B_COARSE)`: min valid, strict `<` (ties -> lowest id), frontiers (cf, cgf) excluded |
| `iurmw_alloc_block` | `alloc_block` generic GC loop, iu_rmw-reachable part: while free <= T: "useless" test (best coarse victim fully valid -> skip GC), else coarse_gc; stall counter, die at >64 (fine-tier escapes are unreachable: no fine blocks). Then `pop_free` (LIFO, block 0 first, bvalid=0) |
| `iurmw_erase_block` | `erase_block` (p2l not cleared, as in ftlsim) |
| `iurmw_verify` | `verify()`: recompute per-block valid from coarse_map (U per mapped IU); additionally checks U-alignment and p2l back-pointers of every live extent |
| `iurmw_mem` | RAM model `NIU * 4` (`mem.total_bytes`) |

Source mapping: w new pages -> `DM_P_HOST`, U-w filler pages -> `DM_P_RMW` (programmed in offset order
ppn+0..U-1, each with its own src); coarse-GC copies -> `DM_P_GC` with U `DM_R_GC` reads of the old extent
issued before the copy. RMW reads (`DM_R_RMW`) of the U-w missing offsets at `old+off` are issued BEFORE the
extent allocation (which may run GC and move the old extent), matching where ftlsim counts them.
Read path (ftlsim has none): host read of lpn -> `DM_R_HOST` at `coarse_map[lpn/U] + lpn%U` if mapped.

## Parameters
None (`params` ignored). Geometry from dm_geom (L, P, NB, U=16, T=2). ftlsim defaults used: coarse_class=0,
gc_frontier=1, cgc_vict=greedy, unfold=off.

## Validation
```
$ cd dm/harness && ./validate.sh iurmw "mode=iu_rmw op=0.09278"
skew  ftlsim(WAF prog erases)=[7.0065 26527104 4317]  harness(WAF prog erases order_viol)=[7.0065 26527104 4317 0]
rand4k  ftlsim(WAF prog erases)=[103.0207 291652208 47469]  harness(WAF prog erases order_viol)=[103.0207 291652208 47469 0]
kvsmoke  ftlsim(WAF prog erases)=[2.38 46296160 1004]  harness(WAF prog erases order_viol)=[2.38 46296160 1004 0]
PARITY: bit-identical
```
cfg check (skew, 8G): ftlsim `[cfg] mode=iu_rmw L=2097152 ... NB=373 P=6144 U=16 ... T=2`,
harness `[cfg] policy=iurmw L=2097152 NB=373 P=6144 U=16 T=2`. Per-source breakdown also identical (skew):
host 3660185 / gc(cgc_clean) 21860304 / rmw 1006615 programs; reads rmw 1006615, gc 21860304; mem 524288 B.
Kernel: `make -j16` builds nvmev.ko, no warnings from dm/ftl_iurmw.c, modpost reports no undefined symbols.

## Deviations / notes
- ftlsim counts `read_rmw += U-w` even when the IU is unmapped; the port counts `st->read[DM_R_RMW]` the same
  way (parity) but issues `fo.read` only for a mapped IU (no physical page to read). Unreachable after the
  seq fill (every IU mapped).
- The "useless victim" die path (>64 stalls) is kept for fidelity; ftlsim dies there too.
- Report fragment: cgc count, average victim valid, dead/clean extents, free_end (cumulative, not reset at warmup).

## Kernel concerns
- No FPU, no per-I/O allocation (only `verify()` allocates a NB-sized array). Large arrays allocated in create().
- Footprint at 64 GiB logical (L=16M, op=0.09278): coarse_map 4 MiB (the modelled mapping RAM), p2l (OOB model)
  NB*P*4 ~ 73 MiB, per-block arrays negligible. ppn is u32 (create() dies if NB*P >= 2^32-1).
- GC victim selection is an O(NB) scan per GC (plus one more for the "useless" test), like ftlsim; runs inline
  in the write path.
