# FAST core (`dm/ftl_fast.c`, `DM_POLICY := fast`)

Port of ftlsim `mode=fast` (`struct HybridSim` in `/home/sejun000/dual_mapping/ftlsim/ftlsim.cpp`),
FAST log-buffer hybrid FTL (Lee et al., TECS 2007). Only the FAST paths are ported (no LAST / Janus).

## What was ported (ftlsim function -> C)
| ftlsim (HybridSim) | ftl_fast.c |
|---|---|
| constructor geometry (L, NLB, NB, nlog) | `fast_create` |
| `pop_free` / `erase` (LIFO free list, block 0 first) | `fast_pop_free` / `fast_erase` |
| `drop_log_copy`, `invalidate`, `latest` | `fast_drop_log_copy`, `fast_invalidate`, `fast_latest` |
| `full_merge`, `switch_merge`, `partial_merge` | `fast_full_merge`, `fast_switch_merge`, `fast_partial_merge` |
| `merge_reclaim`, `seq_reclaim` | `fast_merge_reclaim`, `fast_seq_reclaim` |
| `log_append`, `fast_write_page` | `fast_log_append`, `fast_write_page` |
| `verify` | `fast_verify` (valid counts from `latest()`, plus lmap/p2l/tag consistency and the log-page count) |
| `report` mem (fast) | `fast_mem` |

Semantics kept exactly: offset 0 of an LB starts a new SW (the old SW is reclaimed first); SW appended while
offsets are sequential, reclaimed when full; same-LB out-of-order write reclaims the SW then goes to RW;
RW = FIFO of at most nlog-1 blocks, victim = oldest, dead -> erase, else full merge of every associated LB;
SW reclaim: dead -> erase, full & all-valid -> switch, valid == next -> partial, else full merges.
Full merge programs all P offsets (never-written offsets included, as ftlsim) into a fresh block, then erases
the old data block (valid count forced to 0 as in ftlsim). No GC trigger: the pool is NLB + nlog + T.

Implementation differences that do not change any count:
- ftlsim's per-log-block `assoc` hash (LB -> valid pages) is not kept; `merge_reclaim` finds the associated
  LBs by scanning the victim's written offsets (`lmap[p2l[ppn]] == ppn`). Merge order therefore differs from
  libstdc++ `unordered_map` iteration order, but each full merge is independent (P programs into a fresh
  block, reads = existing copies, one erase of the old data block), so programs/reads/erases are identical;
  only the physical block ids chosen differ. (FAST's SW is always in order, so `inorder` is not stored.)
- RW FIFO is a ring buffer (ftlsim uses `vector::erase(begin)`).
- Full and partial merge share one copy loop `fast_copy_lb` (per offset: read of the newest copy if any, then
  the program, in offset order, exactly as before); it avoids the per-page u64 div/mod of `fast_latest` and adds
  the dm_stats counters once per merge (the flash callbacks never read dm_stats). See "CPU cost" below.

## Source mapping
- programs: `S_HOST` -> `DM_P_HOST`; `S_FULLMERGE` and `S_PARTIALMERGE` -> `DM_P_GC` (FAST uses no other
  sources; `S_ZEROFILL` is not used by FAST — never-written offsets in a merge are counted by ftlsim as
  fullmerge/partialmerge, so they are `DM_P_GC` here too).
- reads: merge copy of an existing page (ftlsim `read_gc_copy`) -> `DM_R_GC`; host read -> `DM_R_HOST`
  (ftlsim has no read path: newest copy = `lmap[lpn]` if set, else `dmap[lb]*P + off`; never-written -> no read).
- No mapping-table I/O (`DM_P_MAP`/`DM_R_MAP` unused: the model keeps all maps in DRAM).
- Check (skew, auto P): harness prog gc 40283864 = ftlsim fullmerge 38504448 + partialmerge 1779416;
  harness read gc 40283864 = ftlsim gc_copy 40283864.

## Geometry / parameters
- HybridSim truncates L to a multiple of P: `Lc = L - L % P`, `NLB = Lc / P`, `NB = (Lc*(1+op))/P`,
  `nlog = NB - NLB - T`. The harness (and glue) compute L as a multiple of U only and NB from that L, so with
  auto P the harness NB can be one larger (8 GiB, P=6144: harness NB=373, ftlsim NB=372).
  The core therefore recomputes `NB = Lc*(1e6+op_ppm)/1e6/P` (capped at g->NB) when `L % P != 0`;
  the extra block stays unused. **Param `op_ppm`** (default 92780 = harness op 0.09278). When L % P == 0,
  g->NB is used unchanged.
- Logical pages `>= Lc` (the partial last LB; 2048 pages at 8 GiB/auto P) do not exist in ftlsim's device;
  the core ignores writes/reads there (counted in host_pages, no flash ops). Glue concern: for `fast` the
  namespace should expose `Lc` pages (`ns->size = Lc*4096`); not changed (glue is shared).
- T = g->T (2). No other policy parameters (ftlsim FAST has none).
- `report` emits `"fast": {NLB, nlog, switch, partial, full, dead_reclaim, merge_reclaim, assoc_sum,
  log_pages_end}`; event counters are cumulative since create (the harness only resets dm_stats at warmup).

## RAM model (`mem_bytes`)
ftlsim fast: `NLB*4 + hash_bytes(log_pages, 8) = NLB*4 + log_pages*8/0.7`, printed "%.0f".
Integer form: `NLB*4 + (log_pages*80 + 3)/7` (nearest; k*80/7 never ties). Skew/auto P: 518564 = ftlsim 518564.

## Validation
1. Prescribed command (auto P):
```
$ ./validate.sh fast "mode=fast op=0.09278"
skew  ftlsim(WAF prog erases)=[11.6068 43944049 7155]  harness(WAF prog erases order_viol)=[11.6068 43944049 7155 0]
rand4k  ftlsim(WAF prog erases)=[]  harness(WAF prog erases order_viol)=[13.6623 38677920 6296 0]
kvsmoke  ftlsim(WAF prog erases)=[]  harness(WAF prog erases order_viol)=[7.2659 141339285 3069 0]
PARITY: MISMATCH
```
   skew is bit-identical. For rand4k and kvsmoke **ftlsim itself aborts**: `FATAL: trace dense space exceeds
   device (8185 regions)` / `(62641 regions)` — HybridSim's L truncation to a multiple of P (2095104 pages at
   8 GiB, 16035840 at 61.2 GiB) is smaller than the traces' dense space (8185*256 = 2095360, 62641*256 =
   16036096). This is a property of ftlsim mode=fast at auto P, not of the port (the harness's L is untruncated,
   so it runs; its numbers there have no reference).
2. All three traces with a P that divides the capacity (1 MiB blocks):
```
$ ./validate.sh fast "mode=fast op=0.09278 P=256" "P=256"
skew  ftlsim(WAF prog erases)=[4.3103 16319024 63751]  harness(WAF prog erases order_viol)=[4.3103 16319024 63751 0]
rand4k  ftlsim(WAF prog erases)=[11.83 33490807 130837]  harness(WAF prog erases order_viol)=[11.83 33490807 130837 0]
kvsmoke  ftlsim(WAF prog erases)=[1.012 19685204 76101]  harness(WAF prog erases order_viol)=[1.012 19685204 76101 0]
PARITY: bit-identical
```
3. Per trace with large blocks near auto P (same comparison as validate.sh, ftlsim `mode=fast op=0.09278 P=<P>`,
   harness `P=<P>`; 46624 = 2^5*31*47 divides the kv capacity's 16038656 pages):
```
skew P=8192  ftlsim(WAF prog erases)=[12.9238 48930417 5976]  harness(WAF prog erases order_viol)=[12.9238 48930417 5976 0]  MATCH
rand4k P=4096  ftlsim(WAF prog erases)=[13.2356 37470005 9152]  harness(WAF prog erases order_viol)=[13.2356 37470005 9152 0]  MATCH
rand4k P=8192  ftlsim(WAF prog erases)=[14.3136 40521913 4947]  harness(WAF prog erases order_viol)=[14.3136 40521913 4947 0]  MATCH
kvsmoke P=46624  ftlsim(WAF prog erases)=[7.3339 142660699 3064]  harness(WAF prog erases order_viol)=[7.3339 142660699 3064 0]  MATCH
```
   Order violations are 0 everywhere (merges program a fresh block 0..P-1; partial merge continues the SW
   from `next`). ftlsim runtimes were seconds (kv ~4 s), so no trace hit the 30-minute limit.
   `reads=1` run: verify ok, host reads go to DM_R_HOST.
4. Kernel: `make -j16` builds `nvmev.ko`, no warnings from dm/ftl_fast.c, modpost reports no undefined symbols.

## CPU cost and preemption (2026-10-01)
Trigger: on NVMeVirt (OP 7%: NB=373, P=67584, L=23559648, kernel params default, i.e. `op_ppm` 92780 -> core
NB=min(380, 373)=373, NLB=348, nlog=23) a 4 KiB random-write fio run hit `soft lockup - CPU#94 stuck for 23s`
in `nvmev_dispatcher`, then NVMe timeouts.

Reproduction in userspace: harness seq fill + 4 KiB uniform random writes over the dense space, instrumented
copy of replay.c (clock_gettime around every `dm_policy.write`, flash ops counted per call), geometry
`capacity=96500318208 op=0.07001 P=67584` -> `[cfg] NB=373 P=67584`, core `NB=373 (of 373) NLB=348 nlog=23`
(`op=0.07` gives NB=372 in the harness: 23559648*1.07/67584 = 372.99; NVMeVirt derives L from NB=373).
Traces: 2x capacity (47.1M writes) and its first quarter-capacity prefix (5.9M).

Hotspot: there is no super-linear loop in the core; the cost is the number of flash operations one host page
write must issue synchronously. The worst call is the first RW-victim reclaim after the 22 RW blocks fill:
its 67584 random pages belong to all 348 LBs, so it full-merges all of them = **23,789,746 flash ops (11.9M
programs + 11.9M reads + 349 erases) inside one `write()`** (later reclaims merge ~16 LBs on average:
10502 merges / 660 reclaims over the 2x trace). In the kernel each op goes through `dm_cb_prog/read` ->
`ssd_advance_nand` (+ `chmodel_request`, `cpu_clock`); at ~1 us/op that is ~24 s in one call = the 23 s soft
lockup (the mu kernel run of the same cycle averaged <= 3.3 us per flash op: ~307M ops in ~1020 s).
Profile of the 2x run (in-process SIGPROF sampler; perf is blocked by perf_event_paranoid=4): host-path
`fast_invalidate` 25% (one random lmap/dmap miss per host page, inherent), harness callbacks/parsing ~35%,
merge loop `fast_latest` 8.7% + `fast_full_merge`/`fast_rd`/`fast_prog` 6.7%. Per-page core work in the merge
loop was a `fast_latest` (u64 div+mod) + stats increments per page; now a straight copy loop (`fast_copy_lb`). The merge-reclaim offset scan prefetches `lmap[p2l[ppn+16]]`.

Preemption: `dm_cond_resched()` (dm_port.h; `cond_resched()` in the kernel, no-op in userspace) every 4096
offsets in `fast_copy_lb` (full/partial merge), `fast_switch_merge`, the merge-reclaim scan, every 1M pages
in `fast_verify` and every 16M entries of the create() p2l init. The core holds no lock and uses no FPU; the
glue calls it from the dispatcher kthread (process context, no spinlock), so this removes the soft lockup.
It does NOT shorten the command: the write that triggers the 348-LB reclaim still needs ~24M glue callbacks
before it completes, so with the current glue it can still exceed the 30 s NVMe timeout (see glue note).

Timing (userspace harness, same machine, wall time inside `dm_policy.write`; results unchanged):
| trace | version | write time | per host page | worst call (flash ops) | calls > 10 ms |
|---|---|---|---|---|---|
| 0.25x | before | 1.51 s | 51 ns | 76.9 ms (23.8M) | 9 |
| 0.25x | after  | 1.29 s | 44 ns | 52.5 ms (23.8M) | 9 |
| 2x    | before | 10.91 s | 154 ns | 77.0 ms (23.8M) | 92 |
| 2x    | after  | 9.23 s | 131 ns | 51.1 ms (23.8M) | 52 |
(per host page = write time / pages handed to the core incl. the 23.5M-page seq fill; userspace callbacks
are counters, ~2 ns/op.) The "few ms per call" target is not reachable without changing FAST's semantics: the
worst call's 23.8M ops are what ftlsim's synchronous reclaim issues; bounding the call needs incremental
(deferred) merging, which changes the free-block state seen by later writes and therefore the results.
Results on the 2x trace before and after (identical JSON): WAF 17.3782, total_prog 409423669, erases 6057,
gc reads 385904303, full 10502 / switch 1035 / partial 687 / merge_reclaim 660.

Validation after the change: `./validate.sh fast "mode=fast op=0.09278 P=256" "P=256"` -> PARITY: bit-identical
(same numbers as in Validation 2), the four large-P runs of Validation 3 -> MATCH (same numbers), the auto-P run
reproduces the numbers of Validation 1 (skew bit-identical; ftlsim still aborts on rand4k/kvsmoke), read path
(skew, P=256, reads=1) identical to the old core. Kernel: `make -j8` (6.8.0-142) builds nvmev.ko, no warnings from
dm/, every undefined symbol of nvmev.ko is in the kernel's Module.symvers (`__SCT__cond_resched` included).

**Glue note (not changed, shared code):** `channel_model.c:chmodel_request()` prints
`NVMEV_ERROR("... Need to increase array size ...")` (printk KERN_ERR, not rate-limited) whenever a request
starts more than NR_CREDIT_ENTRIES x 4 us = 393 ms after now. A 348-LB reclaim queues 11.9M programs =
1.49M oneshot programs over 16 LUNs x 185 us = ~17 s of NAND backlog per LUN, so after the first ~2k programs
per LUN essentially every following NAND command of that reclaim (and of the host I/O behind it) prints one
line. Console loglevel here is 4 (`/proc/sys/kernel/printk` = 4 4 1 7), so KERN_ERR also goes to the console;
`/var/log/kern.log` has grown to 13.9 GB since Sep 23. This printk storm is the likely dominant per-op cost in
the kernel; suggest `printk_ratelimited`/NVMEV_DEBUG there (and/or clamping instead of logging) in the shared
NVMeVirt code.

## Kernel concerns
- No FPU, no per-I/O allocation (all arrays in create(); verify() allocates NB*4 B).
- DRAM at 64 GiB logical (~16.8M pages, NB*P ~ 18.3M): lmap L*4 B (64 MiB) + p2l NB*P*4 B (~70 MiB) + small
  per-block arrays. The dense lmap is an implementation shortcut; the reported RAM model is ftlsim's
  (block map + hash over live log pages). ppn is u32 (create() dies if NB*P does not fit).
- A single full merge copies P pages synchronously inside one host write (P*assoc prog/read callbacks; at
  NVMeVirt OP 7% the worst reclaim is 348 x 67584 pages = 23.8M ops). The loops call `dm_cond_resched()`
  (see "CPU cost and preemption"), so the dispatcher no longer trips the soft-lockup watchdog, but the
  command itself stays long-running.
- Tail pages >= Lc get no flash timing (see Geometry).
