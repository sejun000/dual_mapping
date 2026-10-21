# NOTES: proposed (dual-granularity FTL) core

`dm/ftl_proposed.c` ports `ftlsim mode=proposed` (`struct Sim` in
`/home/sejun000/dual_mapping/ftlsim/ftlsim.cpp`). Each C function mirrors the ftlsim function with the same
name, and keeps its order of operations. That includes the nested GC that can run inside a frontier
allocation, which is why state is re-read after `coarse_alloc_extent`/`fine_alloc_page`.

## What maps to which ftlsim function

| ftl_proposed.c | ftlsim `Sim::` |
|---|---|
| `prop_fine_get_or_create` / `prop_fine_release` | `fine_get_or_create` / `fine_release` (pool index: fbitmap, fslot, flast) |
| `prop_erase_block`, `prop_pop_free`, `prop_pick_victim` | `erase_block`, `pop_free`, `pick_victim` (min valid, ties -> lowest id, frontiers excluded) |
| `prop_pick_cb_fp` | `pick_victim_cb` (cgc_vict=cb: max (P-v)/v*sqrt(alloc_seq-bseq), 0-valid = 1e308) |
| `prop_coarse_starved`, `prop_alloc_block` | `alloc_block`: fine quota + space guard loop (forced fold after 4 stalls), GC loop with the "useless" (fully valid) victim test, gc_select=cost tier choice, dead-fine-block erase, forced-fold backstop (in_shrink), reserve draw, stall dies |
| `prop_fine_alloc_page` | `fine_alloc_page` (ff / fgf, fbirth on open) |
| `prop_coarse_alloc_extent` | `coarse_alloc_extent` (cf / cgf / ccf[] cascade, freelist<=1 fallback, bccls) |
| `prop_fine_write` | `fine_write` (fine_clock, flast, frag_freed, d=U extent drop) |
| `prop_eager_worth`, `prop_eager_write` | `eager_union` + `eager_worth` (eager_u=-1, cost-based) + `eager_write` |
| `prop_coarse_write`, `prop_install_extent`, `prop_invalidate_iu` | `coarse_write` (age class of the survivor), `install_extent`, `invalidate_iu` |
| `prop_fold` | `fold` (fold / refold / zero-fill sources, frag_folded / frag_freed) |
| `prop_fold_block` | `fold_block` (forced fold) |
| `prop_greedy_reclaim_block` | `greedy_reclaim_block` with fine_policy cost / cost_cold (`d*v > U*(P-v)`, cold = `flast <= fbirth[victim]`, fold_tau<0) |
| `prop_fine_reclaim` | `fine_reclaim` (cost / cost_cold branch; `v==0` erase; force_fold -> fold_block) |
| `prop_coarse_gc` | `coarse_gc` (unfold=off: refold if fragments, else clean copy with cls = victim class + 1) |
| `prop_write` | `host_write`, proposed branch (aligned full IU -> extent, or fine_full keep-fine; partial -> eager or fragments) |
| `prop_verify` | `verify()` (valid counts recomputed from the maps), plus block-tag counts and no empty pool entries |
| `prop_mem` | `report()` RAM model: final `else` branch (hash), or the `ct_on` branch (cuckoo2) |
| `prop_adapt_tick`, `prop_close_frontier` | `adapt_tick` (adapt=1 majority / adapt=2 random-cache rule, quota switch, closing fgf with uni_onefront and ccf[] on entering uniform mode) |
| `prop_cc_eff` | `cc_eff()` (age cascade off in uniform mode) |
| `prop_host_birth` | `host_birth` (rec_block) |
| `prop_ct_*` (`mix`, `b1`, `b2`, `free_in`, `load`, `room`, `insert`, `find`), `prop_ct_ensure` | `ct_mix`, `ct_b1`, `ct_b2`, `ct_free_in`, `ct_load`, `ct_room`, `ct_insert`, `ct_remove`, `ct_ensure` (eviction fold) |

### V3 additions (ftlsim changes since the original port)

1. `alloc_block` reserve draw now requires `nfree > 1` ("keep the last block: a nested coarse GC needs it").
   Applies to every configuration; prop_lo / prop_hi stay bit-identical.
2. `coarse_alloc_extent`: when the target is a full shared GC frontier cgf inside coarse GC with an empty free
   list, spill into an open class frontier ccf[] (`cgf_spill` counter; not hit on any validation trace).
3. `adapt` (1 or 2): `fq_base` = the canonical quota, `fq_uni = max(fq_base, NB - ceil(L/P) - T - (uni_onefront ? 3 : 4) - 1)`.
   `prop_adapt_tick` runs from `prop_fine_write` on every host fine page (overwrite / new) at the same point as
   ftlsim, after `fq_base*P` host fine pages decides the mode, switches `fq`, and on entering uniform mode closes
   ccf[] (and fgf with uni_onefront); unwritten tails are never programmed (forward skip, no order violation).
   Uniform mode: `cc_eff() = 0`, greedy coarse victim instead of cb.
4. `uni_onefront`: in uniform mode fine GC copies go to the host fine frontier ff.
5. `rec_block`: `bhost[b]` is set when a fine block is opened as ff (also by a GC copy in uniform+onefront mode, as
   in ftlsim); `prop_host_birth` replaces `flast` in the cost_cold test and in the eviction tie-break; cuckoo2
   entry = 8 B.
6. `fine_index_cuckoo2` + `map_budget_ppm`: the table (`ct_nb` buckets x 4 ways) is sized once in create() with
   ftlsim's double arithmetic; `prop_ct_ensure` runs in `prop_write` before each host `prop_fine_write` (both the
   fine_full loop and the fragment loop). `prop_fine_release` removes the IU's entries, `prop_fine_write`
   inserts (at ftlsim's `ct_insert` point), verify checks table <-> fine pool consistency (every entry lives in
   one of its two buckets, maps a fine-resident page, and #entries == #fine pages).
7. Report-only additions: `fine_host_new/ow`, `adapt{switches, uni_host, quota_base, quota_uni, uni_at_end}`,
   `cuckoo2{buckets, entries, used_end, evict_folds}`, `cgf_spill`, `reserve_draw`. Not ported (optional):
   `fine_pages_peak`, `fine_d_hist_end`, the `fine_index=cuckoo` (report-only) RAM model.

### cuckoo2 is the real fine-map index

With `fine_index_cuckoo2=1` there is no fine pool: the table stores (lpn 4 B, ppn 4 B) per entry and every fine
lpn -> ppn lookup/update (host reads, fold/RMW reads, GC liveness check, GC copy remap, invalidation, rec_block
host_birth) goes through the two candidate buckets. With `rec_block=0` a per-IU `flast[NIU]` (8 B) is also
allocated (not used by V3). Pool mode (no cuckoo2) is unchanged from the original port.

### Flag bit (2026-10-13): no per-IU bitmap

The only per-IU state is the 4 B coarse entry, which ftlsim's RAM model already counts (NIU x 4 + table):
`coarse_map[iu] = PROP_FLAG (bit 31: the IU has >= 1 fragment in the table) | chunk index (start ppn / U, 31 bits;
PROP_CNONE = 0x7FFFFFFF = no chunk)`. Chunks are U-aligned (coarse frontiers advance by U, P % U == 0), so the
index loses nothing. Chunk-index bits needed: 20 at the NVMeVirt 44 GiB geometry and SPFresh, 21 at 64 GiB, 23 for
Alibaba 1/2 and ssdtrace, 26 for Alibaba3 (2.4 TiB); at 64 TiB with OP 7%, U=16 needs 31 bits (1.15e9 chunks) and
U=32 30 bits, so the flag fits; U=8 would need 32 bits and does not (limit: 2^31-1 chunks = 128 TiB physical at
U=16, 64 TiB at U=8). create() dies if the geometry does not fit. (Independently, the table's 4 B lpn/ppn fields
limit L and NP to < 2^32 pages = 16 TiB, as before.)

- Host read (`prop_read`): coarse entry (1 access); flag clear -> chunk page; flag set -> one table lookup
  (<= 2 buckets): hit -> fine page, miss -> chunk page. Never a flash map read; <= 3 DRAM accesses.
- Host fine write: flag clear -> certainly a new fragment (insert, set the flag); flag set -> one lookup decides
  overwrite vs new.
- An IU's fragment set (`prop_fscan`) = U table lookups; it gives d, the fragment ppns and the rec_block recency.
  Used by fold / eager RMW / invalidate (fragment removal, then the flag is cleared), the eager test
  (`prop_eager_worth`), the fine-GC cost test (d + recency in one scan) and the eviction candidate choice.
- ftlsim drops the chunk pointer when d reaches U; the core cannot see d on a write without a scan, so the chunk is
  dropped lazily: coarse GC treats a flagged chunk with d == U as dead (and clears it), fold treats it as absent.
  Every other user of the pointer only reads the U - d = 0 pages not covered by fragments, so this is
  decision-equivalent (parity below). `extent_drop` now counts these lazy drops.
- Memory: the per-IU bitmap (NIU x 4 B = 6.25% of the page map at U=16) is gone; `mem_bytes` was and is
  NIU x 4 + table, now equal to what is allocated for the mapping.

Parity after the change (ftlsim with the flag-bit DRAM accounting; WAF/prog/erases unchanged, all bit-identical):
V3 U=16 at map_budget 0.10 / 0.1875 / 0.25, U=8 and U=32 at 0.1875 on skew / rand4k / kvsmoke (`validate.sh`),
SPFresh 0.1875 (14.3331, prog 212701375, erases 7039, mem 7868160, 12 lazy drops) and 0.10, `reads=1` skew (404664
host flash reads, verify ok). Kernel build clean; xmm only in the five FP helpers.

### Spill (2026-10-19): fine maps of evicted IUs go to flash translation pages (ftlsim `spill=1|2`)

Port of ftlsim `spill` / `spill_ipf` (Sim::spill, spill_one, unspill, ftp_read/ftp_write, gc_batch_*). `fold_on_read`
is not ported (rejected). Only cuckoo2 (`fine_index_cuckoo2=1`); create() dies otherwise.

- Layout: FTP g holds the U x 4 B fragment pointers of IUs [g*ipf, (g+1)*ipf) (ipf = 4096 / (U x 4 B) = 64 at U=16,
  one 4 KiB page; `spill_ipf=` overrides). Dense and fixed, so the DRAM directory (GTD) is 4 B per FTP
  (n_ftp = ceil(NIU/ipf)), charged to `map_budget_ppm`: ct_nb = (budget - NIU*4 - GTD) / (4*ent), exactly as ftlsim
  (`prop_ct_nb_fp`). `mem_bytes` = NIU*4 + GTD + table. create() logs the split, e.g. at 8 GiB / 0.125:
  `coarse 524288 + GTD 8192 + table 516096 = 1048576 B (budget 1048576 B)`.
- Coarse entry: bit 31 = has fragments (flag), bit 30 = spilled, 30-bit chunk index (`cmask` = 0x3FFFFFFF = no chunk;
  without spill the old 31-bit layout is used, so spill=0 is unchanged). A spilled IU keeps the flag set.
- `ftp[L]` (u32 per logical page, vzalloc) is the FTP content on flash: lpn -> fragment ppn of a spilled IU, NONE
  otherwise. It is not mapping DRAM; every access that a device would serve from flash is charged an FTP read where
  ftlsim charges it. `grp_spilled[n_ftp]` = spilled IUs per FTP (on a device: the spilled bits of the FTP's ipf
  coarse entries, 4 cache lines) decides whether a spill must read the old FTP first.
- Eviction (`prop_ct_ensure`): a full bucket pair spills the candidate (`prop_spill`) instead of folding it; spill=2
  also spills every other table-resident IU of the same FTP (except the IU being written) under the same map program.
  pool_cap evictions still fold. Candidate rule is shared with un-spill (`prop_ct_best` = ftlsim ct_pick).
- Host write to a spilled IU (`prop_unspill`, first thing in ct_ensure): one FTP read, pointers back into the table
  (spilling others if a bucket pair is full; no data writes, so no GC nests in here).
- Fold / overwrite / eager RMW of a spilled IU: one FTP read (ftlsim charges it in invalidate_iu, after the extent
  allocation; the port charges it at the same point: in prop_fold after the allocation, in prop_invalidate_iu for
  coarse writes). Timing caveat: the eager-RMW data reads of the fragments are issued before the (later) FTP read.
- Fine GC (greedy reclaim, forced fold_block) and coarse GC run as a batch (`prop_batch_begin/end`): each FTP is read at
  most once per victim block; copied fragments of spilled IUs update the FTP image and mark it dirty; at the end of
  the victim every dirty FTP that still has spilled IUs is programmed once. Batches nest (only the outermost one
  flushes). One FTP is held in controller SRAM (`ftp_buf`, = DFTL tpbuf): a read of the buffered FTP is free.
- Map I/O goes through `fo.read/prog(DM_NOPPN, DM_R_MAP / DM_P_MAP)` (counted in read[DM_R_MAP] / prog[DM_P_MAP],
  i.e. in WAF, exactly where ftlsim counts read_map / prog[S_MAP]). Host read of a spilled IU: map read, then the data
  read (fragment from the FTP pointer, else the chunk); the glue serializes the two.
- Bit-exact detail: ftlsim keeps the dirty FTPs of a batch in a `std::unordered_set<u64>` and programs them in its
  iteration order, so the FTP left in SRAM after a batch (= the last one programmed) depends on libc++'s order. The
  port reproduces that order (`prop_hs_*`: identity hash, bucket counts 2 -> next_prime(2n+1), max_load_factor 1,
  libc++ 18 `__emplace_unique_key_args` / `__do_rehash`, bucket count kept across clear()). Without it map-read
  counts drift (insertion order gives 2,571,754 instead of 2,571,755 map reads on rand4k); WAF does not depend on it.
- Lazy chunk drop of a spilled IU with d == U (coarse GC) needs no FTP read in ftlsim (the chunk was dropped eagerly
  there); the port does not charge one either (`spill.lazy_drop` counts them).
- DRAM-access bound: a spill=2 event touches ipf coarse entries + up to ipf fragment-set scans (U lookups each), i.e.
  <= 64 x 16 table lookups at U=16; un-spill <= U inserts x 17 tries. No recursion (spill never un-spills).
  The FTP image costs 4 B x L of kernel memory (46 MiB at the 44 GiB NVMeVirt device); n_ftp-sized batch arrays are
  negligible.
- Counters: report JSON `proposed.spill` (cumulative incl. warmup) and a `spill=2 end:` line logged by destroy().

Not ported, because neither canonical point uses them: unfold (hot/any), fine_policy fifo_fold / greedy_copy / alt /
greedy / cold / block, fixed eager_u, fold_tau >= 0, fine_cap=ram, mixed, gc_frontier=0, and the
statistics-only fields (fcnt/freq/req_id, histograms, last_fine_v, the ius/extra set in fine_reclaim,
fine_pages_peak, fine_d_hist_end), and the report-only `fine_index=cuckoo` RAM model.
The parser rejects any key it does not know with `dm_die`, so none of these can be turned on by accident.

Host reads (`prop_read`) are served from the fine slot when the page is fine-resident, else from the coarse
extent page (`DM_R_HOST`). Unwritten pages cost no read. All mapping is in DRAM, so there are no map reads
or programs.

Flash-op sources:
- Host pages go to `DM_P_HOST`. The eager filler pages go to `DM_P_RMW`.
- Coarse-GC clean copies, refolds and fine-GC fragment copies go to `DM_P_GC`.
- Fine-reclaim folds and zero-fill folds go to `DM_P_FOLD`.
- Reads: RMW/eager reads are `DM_R_RMW`, fold reads are `DM_R_FOLD`, and refold and GC-copy reads are `DM_R_GC`.

The read counts equal ftlsim's `read_rmw + read_fold_*` and `read_gc_copy`.

### Spill read-promotion (2026-10-19): ftlsim `spill_rp=K`, `spill_rp_rate=R`, `spill_rp_clean=1`

Port of ftlsim `read_promote` / `unspill(iu, rd)` / `rp_dirty` (results/spill_rp in the dual_mapping repo). All default 0;
with `spill_rp=0` the harness JSON and log are byte-identical to the pre-change build (checked, see parity below).
NOT fold_on_read: no data moves, only the IU's fragment pointers go back into the table.

- `prop_read` (spilled IU): FTP read (unless in SRAM), the data read (fragment from the FTP pointer, else the chunk),
  then `prop_read_promote`: `prop_unspill(f, iu, 1)` = the host-write un-spill (pointers back into the table; a full
  bucket pair spills the `prop_ct_best` candidate, spill=2 group semantics). Its FTP read is free (the FTP was just read).
  Promotion work is on the host read path: any FTP read-modify-write of an evicted IU's FTP is DM_R_MAP (serialized by
  the glue), the FTP programs are DM_P_MAP (in WAF).
- K >= 2: promote on the K-th read since the spill. Counter of ceil(log2 K) bits in the coarse entry, right below the
  spilled bit (chunk index loses those bits; `cmask` shrinks), reset in `prop_spill_one`. Consecutive page lookups of
  the same IU count once (`rp_last` = last IU looked up by `prop_read`, an SRAM register).
- `spill_rp_rate=R`: token bucket in 1/1000 FTP programs, +R per host page reaching the FTL (`prop_write`), capped at
  ipf x 1000; a promotion runs only with credit > 0 and is charged 1000 per FTP program it caused. Integer only.
- `spill_rp_clean=1` (the chosen setting): a read-promoted IU keeps its FTP image valid (`ftp[]` entries kept) and gets
  the clean bit (coarse-entry bit 29, `PROP_CLN`; chunk index loses one more bit). Evicting a clean IU (`prop_spill`)
  just drops its table entries and sets the spilled bit again: no FTP read, no program, no spill=2 group spill. The IU
  becomes dirty (bit cleared, FTP image entries forgotten) on any change of its fragment set or locations:
  `prop_fine_write` (host fine write), `prop_fine_release` (fold / refold / overwrite / eager RMW), `prop_slot_set`
  (fine-GC copy). A spilling eviction (`prop_ct_best(.., pc=1)`: ct_ensure with !over, and un-spill) prefers clean
  candidates, then the usual most-fragments / oldest-recency rule; pool_cap evictions (folds) keep the usual rule.
  A spill=2 group spill of a dirty victim also spills the FTP's clean IUs (their pointers are in the program).
- No recursion (read_promote -> unspill -> spill -> spill_one; spill never un-spills), no FP (xmm only in the five
  pre-existing FP helpers). verify() also checks: a clean IU is not spilled, has fragments, and its FTP image equals
  the table; FTP pointers exist only for spilled or clean IUs.
- Counters: report JSON `proposed.spill_rp` {K, rate, counter_bits, promotions, pages, wait, rate_skips, ftp_progs,
  ftp_reads, clean, clean_evictions, clean_dirtied} (cumulative incl. warmup; printed only when spill_rp != 0) and a
  `spill_rp=K end:` line from destroy().
- Module params (12.5 % budget, = tools/apps/common.sh `proposed_b12r`):
  `dm_params="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"`
  An older nvmev.ko rejects the new keys with dm_die (BUG at insmod): rebuild first.

Sim (results/spill_rp/SUMMARY.txt, 12.5 %, spill=2 base): SPFresh map reads per host read 0.2046 -> 0.1190 (-42 %),
WAF 14.026 -> 14.043, FTP programs per host write page 0.477 -> 0.476 (plain spill_rp=1 without clean: 0.680,
map reads -6 %). YCSB-A WAF 6.728 -> 6.729, YCSB-B map reads -32 %, TPC-C -25 %.

## Parameters (`params`, separated by ' ' or ',')

| key | meaning | default (= ftlsim) |
|---|---|---|
| `fine_quota_ppm` | ftlsim `fine_quota` x 1e6 | 22000 |
| `fine_extra` | fine quota added on top of OP | 1 |
| `fine_quota_blocks` | explicit quota; overrides the derivation | derived |
| `cgc_vict_cb` | cgc_vict=cb (else greedy) | 0 |
| `coarse_class` | cascade frontiers, 0..4 | 0 |
| `fine_policy_cost_cold` | fine_policy=cost_cold (else cost) | 0 |
| `gc_select_cost` | gc_select=cost (else coarse) | 0 |
| `fine_full` | fine_full | 0 |
| `gc_frontier` | only 1 is accepted | 1 |
| `U` | ignored here: the IU comes from `dm_geom.U` (the glue reads `U=` from dm_params) | |
| `adapt` | ftlsim adapt: 0 off, 1 majority rule, 2 random-cache rule | 0 |
| `uni_onefront` | uniform mode: fine GC copies share ff; quota_uni uses 3 frontiers | 0 |
| `rec_block` | per-IU recency from block metadata (bhost/fbirth) | 0 |
| `fine_index_cuckoo2` | ftlsim `fine_index=cuckoo2` (static 2-choice 4-way table, eviction folds) | 0 (hash) |
| `map_budget_ppm` | ftlsim `map_budget` x 1e6 (required with cuckoo2) | 0 |
| `onefront_fix` | ftlsim onefront_fix | 0 |
| `pool_cap` | ftlsim pool_cap (0, 1, 4) | 0 |
| `spill` | ftlsim spill: 0 fold on eviction, 1 spill the victim IU, 2 spill every table IU of its FTP (cuckoo2 only) | 0 |
| `spill_ipf` | ftlsim spill_ipf: IUs per FTP (0 = 4096 / (U x 4 B)) | 0 |
| `spill_rp` | ftlsim spill_rp: read-promotion of spilled IUs, 0 off, 1 every read, K (2..16) on the K-th read since the spill | 0 |
| `spill_rp_rate` | ftlsim spill_rp_rate: promotion FTP programs per 1000 host write pages (token bucket), 0 = unlimited | 0 |
| `spill_rp_clean` | ftlsim spill_rp_clean: read-promoted IUs stay clean (FTP image valid; clean eviction needs no FTP I/O) | 0 |

Everything else stays at the ftlsim default: T=2 from the geometry, unfold=off, fold_tau=-1, eager_u=-1, 1 MiB wbuf.

How the fine quota is derived (ftlsim: `fq = max(1,(int)(NB0*fine_quota))`, and `NB = NB0 + fq` if fine_extra):
- With fine_extra=0, fq comes from `dm_geom.NB`.
- With fine_extra=1, the core recovers the unique NB0 with `NB0 + fq(NB0) == NB`. That x + floor(xq) is strictly increasing is what makes NB0 unique.

This way one params string is correct for every capacity: 8 at 8 GiB/61 GiB for prop_lo, 16 for prop_hi.
The harness geometry already matches ftlsim for both points: harness op=0.09278 gives NB=373 (8 GiB) and
NB=380 (61 GiB), equal to ftlsim's `[cfg]` NB for prop_lo (NB0+fq with op=0.07) and prop_hi.
V3 (fine_extra=0, op=0.07) needs harness `op=0.07`: NB=365 at 8 GiB / 40 GiB, 372 at 61 GiB, equal to ftlsim.

## Floating point

ftlsim takes these decisions in doubles:
1. **Cost-benefit victim** `(P-v)/v*sqrt(age)`. Near-ties can differ from exact math at double precision, so it is computed in double: `sqrtsd`, the same op order as clang's ftlsim build, and no FMA-contractible expressions.
2. **Eager test** `U/n < 1 + p*U/max(1,u)`, where `p = frag_folded/(frag_folded+frag_freed)` or 0.5. The core first decides with an exact integer cross-multiplication (`U*S*u < n*(S*u + a*U)`). It falls back to the double expression only when the two sides are within 2^-40 of each other, which covers exact ties such as the p=0.5 cases. The error of the double expression is about 2^-51, so outside that band both methods agree. The `eager_fp_ties` counter reports how often the fallback ran: 44 / 21 / 1 in the runs below.
3. **Fine quota derivation** `(int)(NB*fine_quota)`. It runs only in create().
4. **cuckoo2 table size** `ct_nb = (u64)((map_budget*L*4 - (L/U)*4) / (4*ent))` (create() only). `map_budget`
   arrives as `map_budget_ppm`; `(double)ppm/1e6` is correctly rounded, so it is the double nearest the
   decimal, i.e. exactly ftlsim's `atof(map_budget)` for any budget with <= 6 decimals. The expression is
   evaluated in ftlsim's order in `prop_ct_nb_fp` (the `*4` steps are exact, so FMA contraction could not change
   it either). `ct_nb` equals ftlsim's `[cfg] cuckoo2 fine table: N buckets` on every trace below.
5. **adapt=2 test** `aw_ow*L < (aw_ow+aw_new)*fq_base*P`. Every operand and product is an integer, so the
   doubles are exact while both products are <= 2^53. The core decides in u64 in that range, which covers
   every configuration up to multi-TiB. Outside it, it calls `prop_adapt_fp`, the same double expression. The
   `adapt.fp_ticks` counter reports how often that happened: 0 in all runs.

The d*v > U*(P-v) fold test works on small integers that doubles hold exactly, so it is plain integer code.
The RAM model's `hash_bytes = k*B/0.7 = 10kB/7` has a fractional part that is a multiple of 1/7, never 1/2.
That makes the integer `(20kB+7)/14` equal to ftlsim's `%.0f` result.

**Kernel FP handling:** the five routines (`prop_eager_fp`, `prop_pick_cb_fp`, `prop_fq_fp`, `prop_ct_nb_fp`, `prop_adapt_fp`) carry
`__attribute__((target("sse2"), __noinline__))`, and they take and return integers only. The rest of the
file keeps the kernel's `-mno-sse`, so no Kbuild flag changes are needed. `objdump` confirms that xmm
registers appear only in those five functions.

Each call is wrapped in `kernel_fpu_begin()/kernel_fpu_end()` (`<asm/fpu/api.h>`, `__KERNEL__` only):
- Inside the FPU regions there is no allocation and no sleeping.
- The cb region is one scan over NB blocks per coarse GC.
- The eager region runs only on near-ties.
- The adapt fallback region would run at most once per fine-pool turnover (never at realistic sizes).
- The table-sizing region runs once in create().

The core therefore needs process or softirq context where the FPU is usable, like the rest of the glue.

## Deviations

- **Wasted fine-GC slots:** ftlsim leaves an allocated fine-GC slot unwritten when the IU was rewritten by nested GC between allocation and copy. The harness requires in-order programming, so the core programs an *uncounted* pad page there: `fo.prog(np, DM_P_OTHER)` is called, but `st->prog` is not incremented, which keeps WAF equal to ftlsim's. This happened 0 / 1 / 10 / 17 times per run (the `pad_progs` counter). In the kernel it costs one page of program time.
- **Warmup stats reset:** `frag_folded`/`frag_freed` are ftlsim `Stats` fields that feed `eager_worth`, and ftlsim resets them at the warmup split. The core has no reset hook, so it detects a reset when `st->host_pages` goes backwards at write entry. The kernel glue never resets stats, so there the counters are cumulative, which matches ftlsim's behaviour without a warmup.
- **Report counters:** the core's report counters are cumulative (they include warmup), while ftlsim resets its event counters. Only WAF, prog, erases and mem are compared.
- **cuckoo2 index representation:** ftlsim stores only the lpn in `ct[]` and keeps the ppn in the pool's
  `fslot`. The core stores (lpn, ppn) in the table and has no pool. Decisions never depend on pool indices,
  so this is decision-equivalent. Parity on all traces below confirms it, and verify() checks the table
  against the per-IU bitmaps.
- **Report counters (V3):** `cuckoo2.evict_folds`, `fine_host_new/ow`, and `adapt.uni_host` are cumulative
  (they include warmup), while ftlsim resets them at the warmup split. `adapt.switches`, `quota_*`,
  `uni_at_end`, and `cuckoo2.used_end` are not reset in ftlsim either, and they match.
- **Fine pool index size (pool mode):** the index is preallocated for (fq+4)*P live entries (fq = max(fq_base, fq_uni) with adapt) (live entries <= valid fine pages). If it ever overflows it doubles with `dm_zalloc` inside the write path and logs it. That never happened in validation.

## Validation (verbatim)

V3 baseline (V3_18 = map_budget 0.1875, V3_10 = map_budget 0.10), final code:

```
$ ./validate.sh proposed "mode=proposed U=16 op=0.07 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.1875" "op=0.07 U=16 params=fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
skew  ftlsim(WAF prog erases)=[5.7124 21627648 3553]  harness(WAF prog erases order_viol)=[5.7124 21627648 3553 0]
rand4k  ftlsim(WAF prog erases)=[14.985 42422547 6990]  harness(WAF prog erases order_viol)=[14.985 42422547 6990 0]
kvsmoke  ftlsim(WAF prog erases)=[3.2331 62890691 1365]  harness(WAF prog erases order_viol)=[3.2331 62890691 1365 0]
PARITY: bit-identical

$ ./validate.sh proposed "mode=proposed U=16 op=0.07 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.10" "op=0.07 U=16 params=fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=100000"
skew  ftlsim(WAF prog erases)=[5.8188 22030401 3611]  harness(WAF prog erases order_viol)=[5.8188 22030401 3611 0]
rand4k  ftlsim(WAF prog erases)=[33.1392 93817175 15725]  harness(WAF prog erases order_viol)=[33.1392 93817175 15725 0]
kvsmoke  ftlsim(WAF prog erases)=[3.2331 62890691 1365]  harness(WAF prog erases order_viol)=[3.2331 62890691 1365 0]
PARITY: bit-identical
```

Geometry, table size and end state (ftlsim `[cfg]` / JSON vs harness `dm_log` / JSON). In every row NB, P,
fine_quota, buckets, `mem.total_bytes` == `mem_bytes`, `cuckoo2.used_end`, `adapt.switches` and `uni_at_end`
are identical:

```
trace        budget  NB  P      fq_base/uni  ct buckets  mem_bytes  used_end  switches  uni_at_end
skew 8G      0.1875  365  6144  15/17        32768       1572864    8594      0         0
skew 8G      0.10    365  6144  15/17        9830        838848     12159     0         0
rand4k 8G    0.1875  365  6144  15/17        32768       1572864    81527     1         1
rand4k 8G    0.10    365  6144  15/17        9830        838848     31657     1         1
kvsmoke 61G  0.1875  372  46080 15/17        250604      12028992   1252      0         0
kvsmoke 61G  0.10    372  46080 15/17        75181       6415456    1252      0         0
spfresh 40G  0.1875  365  30720 15/17        163920      7868160    414435    1         1
spfresh 40G  0.10    365  30720 15/17        49176       4196352    156140    1         1
mix 8G       0.1875  365  6144  15/17        32768       1572864    78872     3         1
mix 8G       0.10    365  6144  15/17        9830        838848     31557     3         1
```
e.g. ftlsim `[cfg] cuckoo2 fine table: 163920 buckets x 4 = 655680 entries (5.00 MiB), budget 0.1875 of page map`
vs core `proposed: cuckoo2 fine table: 163920 buckets x 4 = 655680 entries, entry 8 B, budget 187500 ppm of page map`.

SPFresh (`/mnt/nvme_mounted/spfresh_trace/spf.packed.csv`, capacity=42970644480; uniform mode for the whole
measured phase, heavy eviction folds):

```
V3_18 ftlsim   WAF=14.3331 prog=212701375 erases=7039 mem=7868160  (total_prog/host = 14.3330791312, WAF_ftl 14.3328)
V3_18 harness  WAF=14.3331 prog=212701375 erases=7039 mem=7868160  order_viol=0  (14.3330791312)  MATCH
V3_10 ftlsim   WAF=30.9311 prog=459014953 erases=15412 mem=4196352 (30.9311476888)
V3_10 harness  WAF=30.9311 prog=459014953 erases=15412 mem=4196352 order_viol=0  (30.9311476888)  MATCH
```
Eviction folds (harness, cumulative incl. warmup; ftlsim counts the measured half only): SPFresh 638559 / 5408593
(ftlsim 341988 / 2880656), rand4k 142352 / 1151137.

`mix` = rand4k.csv + skew.csv + rand4k.csv concatenated (8 GiB). It exercises uniform -> canonical -> uniform
switching (3 switches), which no validate.sh trace does. Coverage counters: `reserve_draw` (liveness fix 1) was
hit once (mix, 0.1875). `cgf_spill` (liveness fix 2) was never hit. Read path through the table (`reads=1`,
skew, V3_18): verify ok, 404664 host flash reads, the same as pool mode.

Previous canonical points and ftlsim defaults, re-run on the final code (unchanged):

```
$ ./validate.sh proposed "mode=proposed op=0.07 fine_extra=1 fine_quota=0.022 cgc_vict=cb coarse_class=2" "params=fine_extra=1,fine_quota_ppm=22000,cgc_vict_cb=1,coarse_class=2"
skew  ftlsim(WAF prog erases)=[4.8415 18330406 2983]  harness(WAF prog erases order_viol)=[4.8415 18330406 2983 0]
rand4k  ftlsim(WAF prog erases)=[29.2351 82764824 13736]  harness(WAF prog erases order_viol)=[29.2351 82764824 13736 0]
kvsmoke  ftlsim(WAF prog erases)=[2.5393 49396115 1073]  harness(WAF prog erases order_viol)=[2.5393 49396115 1073 0]
PARITY: bit-identical

$ ./validate.sh proposed "mode=proposed U=8 op=0.09278 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2" "U=8 'params=fine_extra=0 fine_quota_ppm=42900 fine_full=1 fine_policy_cost_cold=1 gc_select_cost=1 cgc_vict_cb=1 coarse_class=2'"
skew  ftlsim(WAF prog erases)=[4.3002 16280951 2658]  harness(WAF prog erases order_viol)=[4.3002 16280951 2658 0]
rand4k  ftlsim(WAF prog erases)=[14.9435 42305257 6943]  harness(WAF prog erases order_viol)=[14.9435 42305257 6943 0]
kvsmoke  ftlsim(WAF prog erases)=[2.5964 50505203 1085]  harness(WAF prog erases order_viol)=[2.5964 50505203 1085 0]
PARITY: bit-identical

$ ./validate.sh proposed "mode=proposed op=0.07" ""          # ftlsim proposed defaults (greedy cgc, no cascade)
skew  ftlsim(WAF prog erases)=[5.761 21811410 3551]  harness(WAF prog erases order_viol)=[5.761 21811410 3551 0]
rand4k  ftlsim(WAF prog erases)=[28.4091 80426236 13194]  harness(WAF prog erases order_viol)=[28.4091 80426236 13194 0]
kvsmoke  ftlsim(WAF prog erases)=[2.4784 48209604 1046]  harness(WAF prog erases order_viol)=[2.4784 48209604 1046 0]
PARITY: bit-identical
```

RAM model, prop_lo / prop_hi (original port): ftlsim `mem.total_bytes` vs harness `mem_bytes` (end state):

```
LO skew    ftlsim 635858  fine_ius=3669  fine_pages=14789 fq=8  | harness 635858  (same ius/pages/fq)  MEM MATCH
LO rand4k  ftlsim 1063074 fine_ius=28524 fine_pages=32825 fq=8  | harness 1063074                      MEM MATCH
LO kvsmoke ftlsim 4015155 fine_ius=136   fine_pages=887   fq=8  | harness 4015155                      MEM MATCH
HI skew    ftlsim 1169260 fine_ius=3649  fine_pages=11926 fq=16 | harness 1169260                      MEM MATCH
HI rand4k  ftlsim 2937556 fine_ius=76614 fine_pages=89175 fq=16 | harness 2937556                      MEM MATCH
HI kvsmoke ftlsim 8028160 fine_ius=221   fine_pages=1103  fq=16 | harness 8028160                      MEM MATCH
```

Read path smoke test (`reads=1`, skew): verify ok; 404664 host flash reads out of 418584 host read pages.
The rest were served from the write buffer or were never written.

Kernel: `make -j16` builds `nvmev.ko` with no warnings apart from "compiler differs". modpost reports no
undefined symbols. The new imports are `kernel_fpu_begin_mask` and `kernel_fpu_end`, both GPL-exported, and the module is GPL v2.
V3 update: a clean `make -j16` against 6.8.0-142-generic (vermagic `6.8.0-142-generic SMP preempt mod_unload
modversions`) gives the same result. `ftl_proposed.o` imports no libgcc helpers (u64 `%` is a native div on
x86_64; double->u64 is inline SSE2), and xmm registers appear only in the five FP helpers.

### Spill parity (2026-10-19, verbatim from `validate.sh`, which now also compares map programs / map reads)

V3 + onefront_fix=1 pool_cap=4, op=0.07, U=16 (ftlsim args `mode=proposed U=16 op=0.07 fine_extra=0 fine_quota=0.0429
fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2
rec_block=1 onefront_fix=1 pool_cap=4 map_budget=<b> [spill=<s>]`, harness `op=0.07 U=16 params=...,spill=<s>`;
READS=1 = ftlsim dram_stats=1 + harness reads=1). Columns: WAF prog erases map_prog map_read (harness: + order_viol).
```
spill=2 0.125        skew    [5.7124 21627648 3553 0 0]                      = harness
                     rand4k  [15.3535 43465935 6784 1798170 2571755]         = harness (READS=1: map_read 2647238 =)
                     kvsmoke [3.2331 62890691 1365 0 0]                      = harness
spill=0 0.125        skew/rand4k/kvsmoke [5.7124 21627648 3553] [21.8933 61980024 10521] [3.2331 62890691 1365], 0 map I/O =
                     harness JSON byte-identical to the pre-spill build (reads=0 and reads=1)
spill=1 0.125 READS  rand4k  [15.6457 44293168 6826 2368897 2801814]         = harness
spill=2 0.10  READS  rand4k  [15.4975 43873431 6785 2192935 3267032]         = harness
spill=2 0.08  READS  skew    [5.7144 21635305 3553 7657 14095]               = harness
                     rand4k  [15.7259 44520126 6848 2475663 3722894]         = harness
spill=2 0.1875       rand4k  [14.9577 42345232 6789 643997 913950]           = harness
SPFresh packed (spf.packed.csv, 42970644480 B), 0.125:
  spill=0 [21.6266 320937017 10914 0 0]  spill=1 [14.7938 219537765 6819 10158045 11990234]
  spill=2 [14.4989 215162451 6769 7317563 11349845]                         all = harness
NVMeVirt-geometry traces with host reads (ftlsim JSON from results/spill/<t>/s2_0.125.json, dram_stats=1; harness reads=1):
  spfresh (dm_trace_sampled, 21566324736 B, P=15104) [14.0262 103551420 6625 3522700 35561668], mem 2632584 = harness
  ali3    (59581595648 B, P=41728)                   [3.3577 77272081 1862 0 0], mem 7273140              = harness
```
All with order_viol 0, verify ok at warmup and end (incl. the spill invariants: no table entry of a spilled IU, FTP
pointers only for spilled IUs, per-FTP spilled counts, table entries + spilled pages = fine pages = fpages).

### Spill read-promotion parity (2026-10-19, verbatim from `validate.sh` with READS=1, SIM = results/spill_rp/ftlsim.bin)

ftlsim args as the spill parity above + `map_budget=<b> spill=2 spill_rp=K spill_rp_rate=R spill_rp_clean=C`, harness
`params=...,map_budget_ppm=<b>,spill=2,spill_rp=K,spill_rp_rate=R,spill_rp_clean=C`. Columns: WAF prog erases map_prog map_read.
```
b      K R  C  skew                               rand4k                                   kvsmoke
0.125  1 0  0  [5.7124 21627648 3553 0 0]         [15.3963 43586893 6794 1852399 2709906]  [3.2331 62890691 1365 0 0]  = harness
0.125  2 0  0  same                               [15.3484 43451464 6781 1809591 2661448]  same                        = harness
0.125  4 0  0  same                               [15.3535 43465935 6784 1798170 2647237]  same                        = harness
0.125  1 50 0 / 1 1 0                             = K1 R0 (guard never empty)                                          = harness
0.125  2 20 0                                     = K2 R0                                                              = harness
0.08   1 0  0  [5.7144 21635280 3553 7632 13557]  [15.7643 44628688 6860 2485980 3732336]  same                        = harness
0.08   2 0  0  [5.7144 21635102 3553 7454 13891]  [15.7334 44541347 6835 2552208 3808998]  same                        = harness
0.1875 2 0  0  [5.7124 21627648 3553 0 0]         [14.9875 42429600 6804 639790 930932]    same                        = harness
0.125  1 0  1  [5.7124 21627648 3553 0 0]         [15.3467 43446651 6776 1827487 2677792]  same                        = harness
0.125  1 1  1                                     = K1 R0 C1                                                           = harness
0.125  2 0  1                                     [15.327 43390818 6763 1837957 2695811]   same                        = harness
0.125  4 0  1                                     [15.3535 43465935 6784 1798170 2647237]  same                        = harness
0.08   1 0  1  [5.7144 21635039 3553 7391 13513]  [15.74 44559952 6851 2482951 3727880]    same                        = harness
0.08   1 2  1  [5.7144 21635039 3553 7391 13513]  [15.7491 44585916 6835 2597060 3858353]  same  (guard active)       = harness
0.08   2 0  1  [5.7144 21635209 3553 7561 13514]  [15.6965 44436902 6845 2421209 3662384]  same                        = harness
0.1875 1 0  1  [5.7124 21627648 3553 0 0]         [14.9432 42304414 6783 640467 930345]    same                        = harness
SPFresh (dm_trace_sampled, 21566324736 B, P=15104, op=0.070002161355), 0.125, harness reads=1 vs results/spill_rp/spfresh/<tag>.json:
  base (spill_rp=0)   [14.0262 103551420 6625 3522700 35561668]  mem 2632584  = harness
  rp1  K1             [14.224 105011713 6624 5018546 33904338]                = harness
  rp2  K2             [14.0907 104027469 6613 4196919 34071478]               = harness
  rp4  K4             [14.0919 104036337 6638 3788129 33674567]               = harness
  rpc1 K1 clean       [14.0428 103674036 6632 3511841 22205709]               = harness (17.27M promotions, 10.10M clean evictions)
  rpc2 K2 clean       [14.0086 103421234 6613 3555099 25382659]               = harness
```
All order_viol 0, verify ok at warmup and end. spill_rp=0: harness JSON + stderr log byte-identical to the pre-change
build on 30 runs (spill=2 / spill=1 / spill=0 / defaults; budgets 0.08-0.1875; skew, rand4k, kvsmoke; reads=0/1).
`make -j16`: no new warnings (only "compiler differs"); xmm/FP only in the five pre-existing FP helpers.

## Kernel memory at 64 GiB logical

Worked example with ftlsim's geometry: L = 16,777,216 pages, P = 49,152, NB = 373 (NP about 18.3 M pages).
The glue's real P/NB come from ssdparams, but they scale the same way.

| structure | size | U=16 (prop_lo) | U=8 (prop_hi) |
|---|---|---|---|
| coarse_map + fine_idx | 8 B x NIU | 8 MiB | 16 MiB |
| p2l (OOB) | 4 B x NP | 70 MiB | 70 MiB |
| per-block arrays | about 26 B x NB | negligible | negligible |
| fine pool index | (fq+4)*P entries x (16 + 4U) B | 12 x 49152 x 80 B = 45 MiB | 20 x 49152 x 48 B = 45 MiB |
| **total** | | **about 123 MiB** | **about 131 MiB** |

All of this is allocated once with vzalloc in create(). There is no per-I/O allocation, except the unused
pool growth fallback.

The modeled mapping DRAM (`mem_bytes`) is 4 B x NIU (4 MiB at U=16, 8 MiB at U=8) plus the fine hash and
slots, which the fine quota bounds.

### V3 (cuckoo2) at 64 GiB logical

ftlsim geometry with op=0.07 and fine_extra=0: L = 16,777,216, P = 49,152, NB = 365 (NP = 17,940,480),
U = 16, NIU = 1,048,576, fq_base = 15, fq_uni = 17.

| structure | size | V3_18 (map_budget 0.1875) | V3_10 (map_budget 0.10) |
|---|---|---|---|
| coarse_map | 4 B x NIU | 4 MiB | 4 MiB |
| cuckoo2 table (lpn + ppn) | 8 B x 4 x ct_nb | 262,144 buckets = 8 MiB | 78,643 buckets = 2.40 MiB |
| per-IU fragment bitmap `fbitmap[NIU]` (outside the RAM model) | 4 B x NIU | 4 MiB | 4 MiB |
| p2l (OOB) | 4 B x NP | 68.4 MiB | 68.4 MiB |
| per-block arrays (+ bhost) | about 27 B x NB | negligible | negligible |
| **total** | | **about 84.4 MiB** | **about 78.8 MiB** |
| modeled `mem_bytes` (= ftlsim) | NIU*4 + ct_nb*32 | 12,582,912 (12 MiB = 0.1875 x 64 MiB) | 6,710,880 (6.4 MiB) |

The table sizes and mem_bytes above were checked by instantiating both ftlsim and the harness at capacity=64G
(one-write trace). ftlsim reports `262144 buckets` / `78643 buckets`, and the core reports the same bucket counts
with mem_bytes 12582912 / 6710880.

The pool arrays (fine_idx, fslot, flast, pool free list) are not allocated in this mode, which saves about
49 MiB compared with prop_lo. The only per-IU array kept for simulation-equivalence is `fbitmap` (4 MiB). It
could shrink to a u16 at U=16 (2 MiB), or be dropped entirely by probing the table U times per bitmap query.
Everything is allocated once in create(), with no per-I/O allocation. Table operations touch two buckets
(ct_mix hash, u64 modulo). An eviction fold scans the 8 entries, each costing one popcount plus up to d table
lookups for host_birth.

## Glue notes

- No glue change is required. The glue already takes the IU from `dm_params "U=8"`, and the core reads `U` from `dm_geom`.
- Example module params: prop_hi is `dm_params="U=8 fine_extra=0 fine_quota_ppm=42900 fine_full=1 fine_policy_cost_cold=1 gc_select_cost=1 cgc_vict_cb=1 coarse_class=2"`, and prop_lo is `dm_params="cgc_vict_cb=1 coarse_class=2"`. With fine_extra=1 the core recovers NB0 from the glue's NB, so NB must have the form NB0+fq. Otherwise pass `fine_quota_blocks=` explicitly; create() dies with a message if NB does not fit.
- V3 module params (the glue already passes `U=`; op/NB come from the glue's ssdparams, fine_extra=0 takes the quota from NB):
  - V3_18: `dm_params="U=16 fine_extra=0 fine_quota_ppm=42900 fine_full=1 fine_policy_cost_cold=1 gc_select_cost=1 cgc_vict_cb=1 coarse_class=2 adapt=2 uni_onefront=1 rec_block=1 fine_index_cuckoo2=1 map_budget_ppm=187500"`
  - V3_10: the same with `map_budget_ppm=100000`.
  The harness equivalents are in the Validation section (commas or spaces both work as separators).
- Optional: a stats-reset hook in `dm_ftl_ops` would make the frag_folded/frag_freed reset explicit instead of inferred.
- Optional: the glue treats `DM_P_FOLD` as host-path timing, but here fine-reclaim folds run inside space reclamation (GC). The glue may want to time them as GC.
