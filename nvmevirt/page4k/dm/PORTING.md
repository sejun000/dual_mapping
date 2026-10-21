# Porting an FTL policy to NVMeVirt (dual_mapping)

One folder per policy: `~/nvmevirt_<policy>` (a copy of `~/nvmevirt_base`). You write ONE file:
`dm/ftl_<policy>.c`, defining `const struct dm_ftl_ops dm_policy` (see `dm/ftlcore.h`), and set
`DM_POLICY := <policy>` in `Kbuild`. Everything else (NVMeVirt timing glue `dm_ftl.c`, the shared IU write
buffer `dm/dm_wbuf.c`, the harness) is shared and must not be modified; if you need a glue change, say so in
your notes instead.

## Reference implementation = ftlsim
`/home/sejun000/dual_mapping/ftlsim/ftlsim.cpp` (C++). Port the corresponding `mode=` faithfully:
same data structures' semantics, same GC trigger (free blocks <= T=2, no GC re-entry while GC runs, LIFO free
list with block 0 first), same victim selection and tie-breaking (lowest block id), same frontiers, same order
of operations. `dm/ftl_page4k.c` is the template (it is bit-identical to `ftlsim mode=page4k`).

## Hard rules for core code (it compiles in the kernel AND in userspace)
- Include only `ftlcore.h` / `dm_port.h`. Allocate with `dm_zalloc/dm_free` (vzalloc in the kernel: may sleep;
  allocate large arrays in create(), avoid per-I/O allocation where you can; if you must allocate per op,
  keep it small and never inside an FPU section).
- No floating point by default (the kernel has no FPU context). Prefer exact integer math (cross-multiply
  comparisons, `dm_isqrt`). If a policy genuinely needs doubles to stay bit-identical with ftlsim (e.g. LeaFTL's
  PLR / float16 slopes, cost-benefit sqrt scores), you may compile that file with FPU support:
  in Kbuild: `CFLAGS_dm/ftl_<policy>.o += $(CC_FLAGS_FPU)` and `CFLAGS_REMOVE_dm/ftl_<policy>.o += $(CC_FLAGS_NO_FPU)`,
  and wrap every FP region in the kernel with `kernel_fpu_begin()/kernel_fpu_end()` (from <asm/fpu/api.h>,
  guard with `#ifdef __KERNEL__`); no sleeping/allocation inside those regions. Document it.
- Bit ops: use `dm_popcount/dm_popcount64/dm_ctz64/dm_clz64`, never `__builtin_popcount*` (no libgcc in kernel).
- Prefix ALL your static functions/types with the policy name (the kernel defines macros like `alloc_page`).
- Sorting: `dm_sort(base, n, size, cmp)`.
- Flash ops: every program -> `fo.prog(ctx, ppn, DM_P_*)`, every read -> `fo.read(ctx, ppn, DM_R_*)`, every
  erase -> `fo.erase(ctx, blk)`, and count them in `dm_stats` (prog[], read[], erases, map_miss). Mapping-table
  (translation page) I/O uses `ppn = DM_NOPPN` with src `DM_P_MAP` / `DM_R_MAP`. A read whose result the next
  read depends on (misprediction/OOB check) uses `DM_R_OTHER`; the glue serializes MAP/OTHER reads before the data read.
- Pages inside a block must be programmed in increasing offset order starting at 0 after each erase (the
  harness counts violations; must be 0). If ftlsim leaves holes (e.g. padding a partial IU slot), program or
  skip in order — skipping offsets forward is fine, going backward is not.
- `write()` must add n to `st->host_pages`. `read()` must NOT count host_read_pages (the caller does).
- `mem_bytes()` = the same RAM model ftlsim reports in `"mem"` (`total_bytes`).
- `verify()` recomputes block valid counts from the maps (and any policy invariants) and returns 0 if OK.
- `params`: parse options with `dm_param_u64(params, "key", &v)` (supports K/M/G suffixes). Defaults must equal
  ftlsim's defaults for that mode. Geometry comes from `dm_geom` (L, P, NB, U, T); if ftlsim derives something
  from op/fine_quota (e.g. fine quota blocks), take it as a param with a sensible default and document it.

## Validation (required)
1. `cd dm/harness && make POLICY=<policy>`, then
   `./validate.sh <policy> "<ftlsim args>" "<harness args>"` — runs ftlsim and the harness on three traces
   (8 GiB skewed mixed-size, 8 GiB 4K-random, 61 GiB KV-cache) with identical geometry and prints WAF /
   total programs / erases. Goal: **bit-identical** (`PARITY: bit-identical`). The harness uses op=0.09278 and
   ftlsim's auto P; pass the same `op=`/`U=`/`P=` to both if your mode needs different ones. Policy options go
   through the harness as `params="k=v k=v"`.
   If exact parity is impossible (e.g. integer vs double rounding), get WAF within 0.1% and explain why.
2. Kernel build: `make -j16` in the folder root must produce `nvmev.ko` with no errors and no new warnings
   (ignore "compiler differs"). Check `modpost` reports no undefined symbols.
3. Write `dm/NOTES_<policy>.md`: what was ported from which ftlsim functions, parameters and their defaults,
   the exact validate.sh command lines and their output, RAM model, any deviations, kernel-specific concerns
   (memory footprint at 64 GiB logical, per-I/O allocation, FPU use).
4. Commit in your folder's git repo (local only, never push).

The module cannot be loaded yet (no memmap reservation); do NOT insmod, do NOT reboot, do NOT touch GRUB.
