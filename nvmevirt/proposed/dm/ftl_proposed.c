// SPDX-License-Identifier: GPL-2.0-only
/*
 * proposed: dual-granularity FTL (the paper's policy). Port of ftlsim `mode=proposed` (struct Sim).
 *
 *  - coarse tier: IU map (coarse_map[NIU] -> start ppn of a U-page extent), extents written on coarse
 *    frontiers (host cf, GC cgf, optional SepBIT-style age-class cascade ccf[] for coarse-GC survivors);
 *  - fine tier: 4 KiB fragments of partially written IUs (fine_idx -> pool entry with bitmap + U slots),
 *    written on the fine host frontier ff / fine GC frontier fgf, capped by fine_quota_blocks;
 *  - write-time selective RMW (eager_worth, cost-based) completes nearly full IUs into an extent;
 *  - fine reclaim: min-valid fine victim, per-IU cost test fold iff d*v > U*(P-v) (+ parameter-free cold
 *    test flast <= fbirth[victim] for fine_policy=cost_cold), otherwise copy the fragment on fgf;
 *  - coarse GC: greedy or cost-benefit victim; live extents are refolded (fragments absorbed) or copied;
 *  - gc_select=cost picks the fine vs coarse victim by valid count when free blocks run low;
 *  - adapt=1/2: per fine-pool turnover of host fine pages, switch between the canonical policy and a uniform
 *    mode (fine quota = all spare OP, greedy coarse victim, no age cascade; uni_onefront: fine GC copies share
 *    the host fine frontier);
 *  - rec_block: per-IU recency from block metadata (bhost/fbirth) instead of a per-entry write time;
 *  - fine_index=cuckoo2: the fine map is a statically sized 2-choice 4-way bucket table of (lpn, ppn) entries
 *    sized from map_budget; a NEW fine page whose two buckets are full first folds the IU with the most
 *    fragments among the 8 entries (eviction fold). In this mode the table IS the fine lpn->ppn index (no
 *    per-IU slot pool, no per-IU bitmap): the only per-IU state is the 4 B coarse entry, whose top bit says
 *    "this IU has fragments in the table" (see NOTES, "flag bit").
 *  - spill=1|2 (cuckoo2 only, ftlsim spill): a full bucket pair SPILLS its victim IU instead of folding it: the
 *    IU's fragment pointers move to its flash translation page (FTP: dense, fixed location, ipf IUs x U x 4 B =
 *    4 KiB), the table entries are freed and a second coarse-entry bit ("spilled") is set; spill=2 also spills the
 *    FTP's other table-resident IUs in the same map program. GTD = 4 B per FTP, charged to map_budget. FTP I/O is
 *    fo.read/prog(DM_NOPPN, DM_R_MAP / DM_P_MAP); one FTP is held in controller SRAM (ftp_buf). See NOTES, "spill".
 *  - spill_rp=K (ftlsim spill_rp, default 0 = off): read-promotion. A host read of a spilled IU un-spills it after its
 *    FTP read and data read (pointers back into the table as on a host write, possibly spilling others; no data moves):
 *    K = 1 on every read, K >= 2 on the K-th read since the spill (ceil(log2 K)-bit counter in the coarse entry;
 *    consecutive lookups of the same IU count once). spill_rp_rate=R caps promotions at R FTP programs per 1000 host
 *    write pages (token bucket). spill_rp_clean=1: a read-promoted IU stays clean (FTP image left valid, one more
 *    coarse-entry bit) until its fragments change; evicting a clean IU needs no FTP I/O, and spilling evictions prefer
 *    clean candidates. See NOTES, "spill read-promotion".
 * Every function mirrors the same-named ftlsim function; order of operations (incl. nested GC inside
 * allocations) is kept so results are bit-identical. See dm/NOTES_proposed.md.
 *
 * FP: the cost-benefit score (P-v)/v*sqrt(age), the eager-merge cost test, the fine quota derivation, the
 * cuckoo2 table sizing and (only when products exceed 2^53) the adapt=2 test use doubles exactly as ftlsim
 * does. Those routines are compiled with target("sse2") and, in the kernel, called inside
 * kernel_fpu_begin()/kernel_fpu_end(); they take and return integers only. The eager and adapt tests first
 * decide in exact integers and only fall back to the FPU near a tie / outside the exact range.
 */
#include "ftlcore.h"
#ifdef __KERNEL__
#include <asm/fpu/api.h>
#define PROP_FPU_BEGIN() kernel_fpu_begin()
#define PROP_FPU_END() kernel_fpu_end()
#else
#define PROP_FPU_BEGIN() do { } while (0)
#define PROP_FPU_END() do { } while (0)
#endif
#if defined(__x86_64__)
#define PROP_FP __attribute__((target("sse2"), __noinline__))
#else
#error "ftl_proposed: FP helpers are written for x86_64 (SSE2)"
#endif

#define PROP_MAXCC 4
enum { PROP_FREE = 0, PROP_FINE = 1, PROP_COARSE = 2 };
/* ftlsim program sources used by this mode */
enum { PS_HOST = 0, PS_FOLD, PS_REFOLD, PS_CLEAN, PS_ZEROFILL };

struct prop_fr { s32 blk; u32 next; };

struct prop {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u64 L, NIU; u32 P, U, NB, T; s32 fq;
	/* options (ftlsim Cfg) */
	int fine_extra, cgc_cb, coarse_class, cost_cold, gc_cost, fine_full;
	int adapt, uni_onefront, rec_block, ct_on;
	int onefront_fix, pcap, want_ff;   /* ftlsim onefront_fix / pool_cap (1, 4); want_ff: allocating for ff */
	u64 fpages;                            /* fine-resident pages (= table entries), ftlsim Sim::fpages */
	u64 fine_quota_ppm, map_budget_ppm;
	/* adapt state: canonical vs uniform-mode quota, absorption window (Sim members, never reset) */
	s32 fq_base, fq_uni; int uni; u64 aw_new, aw_ow, n_switch;
	/* maps. Pool mode: fine_idx[iu] -> pool entry (fbitmap, fslot[U], flast); coarse_map[iu] = chunk start ppn.
	 * cuckoo2 mode: no pool and no per-IU bitmap; the "pool index" of an IU is the IU itself, coarse_map[iu] =
	 * PROP_FLAG (IU has >= 1 fragment in the table) | chunk index (start ppn / U, PROP_CNONE = no chunk), the
	 * fine lpn -> ppn map is the bucket table ct_lpn/ct_ppn, and an IU's fragment set is found with U table
	 * lookups (prop_fscan). flast[NIU] exists only if !rec_block. A chunk whose U pages are all superseded
	 * (d == U) is dropped lazily (coarse GC), which is decision-equivalent to ftlsim's eager drop. */
	u32 *coarse_map; s32 *fine_idx;
	u32 *fbitmap; u32 *fslot; u64 *flast; s32 *fpool_free; u32 npool, pool_cap, nfpool_free;
	u64 ct_nb; u32 *ct_lpn, *ct_ppn; u32 ct_ent;
	/* physical */
	u32 *p2l; u8 *btag; s32 *bvalid; u64 *bseq; u64 alloc_seq;
	s32 *freelist; u32 nfree;
	s32 fine_blocks, coarse_blocks;
	struct prop_fr ff, cf, fgf, cgf, ccf[PROP_MAXCC];
	u8 *bccls; u64 *fbirth; u8 *bhost;
	int cgc_cls, in_cgc, in_fine_reclaim, in_shrink;
	s32 reclaim_victim, victim_v;
	u64 fine_clock;
	u64 fepoch;   /* bumped on every change of a fragment set or fragment location (greedy reclaim scan memo) */
	/* ftlsim Stats fields that feed decisions (reset with the stats, see prop_write) */
	u64 frag_folded, frag_freed, last_hp;
	/* informational counters (not reset) */
	u64 n_fold, n_refold, n_eager, n_cgc, n_greedy, n_forced, n_shrink, n_drop, n_wasted, n_pad, n_eager_fp;
	u64 n_pool_evict, n_onefront_reuse;
	u64 n_evict, n_adapt_fp, uni_host, fine_host_new, fine_host_ow, n_spill, n_reserve_draw;
	/* coarse entry layout (cuckoo2): PROP_FLAG | [PROP_SPL if spill] | chunk index; cmask = chunk-index mask = "no chunk" */
	u32 cmask;
	/* spill (ftlsim Sim::spill & co.). ftp[] is the FTP content (flash, not DRAM): lpn -> fragment ppn of a spilled IU
	 * (DM_NONE32 otherwise). grp_spilled[g] = spilled IUs of FTP g (= spilled bits among its ipf coarse entries).
	 * Fine-GC batch (ftlsim gc_batch / gc_ftp_rd / gc_ftp_dirty): FTPs read in this victim (rd_ep == batch_id) and
	 * FTPs to rewrite at its end, kept in libc++ unordered_set iteration order (prop_hs) so the FTP left in SRAM
	 * after the batch -- the last one programmed -- is the same as ftlsim's. */
	int spill; u32 ipf; u64 n_ftp, gtd_bytes, spages, ftp_buf;
	u32 *ftp, *grp_spilled, *rd_ep, *dirty_ep, batch_id; int gc_batch;
	struct { u32 *key; s32 *nxt; s32 *bkt; u64 bc, bcap; u32 size; s32 head; } hs;
	u64 sp_n, sp_ius, sp_pages, sp_unspill, sp_unspill_pages, sp_fold, sp_refold, sp_gcfold, sp_copy, sp_lazy_drop;
	u64 sp_rd[5], sp_wr[2], rd_spilled, rd_ftp;
	/* spill_rp (ftlsim spill_rp / spill_rp_rate): K, read-counter bits rp_bits at coarse-entry bit rp_shift (K >= 2),
	 * last IU looked up by a host read (SRAM register), token bucket in 1/1000 FTP programs; counters */
	int spill_rp, rp_bits, rp_clean; u32 rp_shift; u64 spill_rp_rate, rp_last; s64 rp_credit;
	u64 rp_n, rp_pages, rp_wait, rp_rate, rp_wr, rp_rd, rp_cevict, rp_cdirty;
};

/* ------------------------------------------------------------------ FP helpers (integers in/out) */
static PROP_FP int prop_eager_fp(u32 U, u32 n, u32 u, u64 ff, u64 fr)
{
	double p = (ff + fr) ? (double)ff / (double)(ff + fr) : 0.5;
	return (double)U / (double)n < 1.0 + p * (double)U / (double)(u > 1 ? u : 1);
}

static PROP_FP s32 prop_fq_fp(u32 nb, u64 ppm)
{
	double q = (double)ppm / 1000000.0;
	s32 v = (s32)((double)(s32)nb * q);
	return v > 1 ? v : 1;
}

/* ftlsim cuckoo2 sizing: fine_bytes = map_budget*L*4 - (L/U)*4 (- GTD bytes with spill); ct_nb = (u64)(fine_bytes / (4*ent)); 0 = no room.
 * map_budget = ppm/1e6: the correctly rounded quotient is the double nearest the decimal, i.e. atof()'s value. */
static PROP_FP u64 prop_ct_nb_fp(u64 ppm, u64 L, u32 U, u32 ent, u64 gtd)
{
	double mb = (double)ppm / 1000000.0, e = (double)ent;
	double fine_bytes = mb * (double)L * 4 - (double)(L / U) * 4;
	if (gtd) fine_bytes -= (double)gtd;   /* spill: the FTP directory comes out of the same budget */
	if (fine_bytes < 4 * e * 16) return 0;
	return (u64)(fine_bytes / (4 * e));
}

/* ftlsim adapt_tick (adapt=2): aw_ow*L < (aw_ow+aw_new)*fine_quota_base*P in doubles */
static PROP_FP int prop_adapt_fp(u64 ow, u64 sum, u64 L, s32 fqb, u32 P)
{
	return (double)ow * (double)L < (double)sum * (double)fqb * (double)P;
}

static inline __attribute__((target("sse2"), __always_inline__)) double prop_sqrt(double x)
{
	double r;
	__asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
	return r;
}

static int prop_is_frontier(const struct prop *f, s32 b)
{
	int i;
	if (b == f->ff.blk || b == f->cf.blk || b == f->fgf.blk || b == f->cgf.blk) return 1;
	for (i = 0; i < PROP_MAXCC; i++) if (b == f->ccf[i].blk) return 1;
	return 0;
}

/* pick_victim_cb: max (P-v)/v * sqrt(alloc_seq - bseq), strict > so ties -> lowest block id */
static PROP_FP s32 prop_pick_cb_fp(const struct prop *f)
{
	s32 best = -1; double bs = -1; u32 b;
	for (b = 0; b < f->NB; b++) {
		double score;
		if (f->btag[b] != PROP_COARSE || prop_is_frontier(f, (s32)b)) continue;
		score = f->bvalid[b] == 0 ? 1e308
			: (double)((s32)f->P - f->bvalid[b]) / (double)f->bvalid[b] * prop_sqrt((double)(f->alloc_seq - f->bseq[b]));
		if (score > bs) { bs = score; best = (s32)b; }
	}
	return best;
}

/* ------------------------------------------------------------------ flash op helpers */
static inline void prop_prog(struct prop *f, u64 ppn, int src) { f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn, src); }
static inline void prop_rd(struct prop *f, u64 ppn, int src) { f->st->read[src]++; f->fo.read(f->fo.ctx, ppn, src); }
static inline u32 prop_blk(const struct prop *f, u32 ppn) { return ppn / f->P; }

/* ------------------------------------------------------------------ cuckoo2 fine table (ftlsim ct_*) */
static inline u64 prop_ct_mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static inline u64 prop_ct_b1(const struct prop *f, u64 lpn) { return prop_ct_mix(lpn) % f->ct_nb; }
static inline u64 prop_ct_b2(const struct prop *f, u64 lpn)
{
	u64 b = prop_ct_mix(lpn ^ 0x9E3779B97F4A7C15ULL) % f->ct_nb, a = prop_ct_b1(f, lpn);
	return b == a ? (b + 1) % f->ct_nb : b;
}
static int prop_ct_free_in(const struct prop *f, u64 b) { int k; for (k = 0; k < 4; k++) if (f->ct_lpn[b * 4 + k] == DM_NONE32) return k; return -1; }
static int prop_ct_load(const struct prop *f, u64 b) { int k, n = 0; for (k = 0; k < 4; k++) n += f->ct_lpn[b * 4 + k] != DM_NONE32; return n; }
static int prop_ct_room(const struct prop *f, u64 lpn) { return prop_ct_free_in(f, prop_ct_b1(f, lpn)) >= 0 || prop_ct_free_in(f, prop_ct_b2(f, lpn)) >= 0; }
/* entry index of lpn (bucket b1 first, then b2), or -1 */
static s64 prop_ct_find(const struct prop *f, u64 lpn)
{
	u64 a = prop_ct_b1(f, lpn), b; int k;
	for (k = 0; k < 4; k++) if (f->ct_lpn[a * 4 + k] == (u32)lpn) return (s64)(a * 4 + k);
	b = prop_ct_mix(lpn ^ 0x9E3779B97F4A7C15ULL) % f->ct_nb;   /* = prop_ct_b2, reusing b1 */
	if (b == a) b = (b + 1) % f->ct_nb;
	for (k = 0; k < 4; k++) if (f->ct_lpn[b * 4 + k] == (u32)lpn) return (s64)(b * 4 + k);
	return -1;
}
/* less-loaded bucket first (tie -> b1), first free way */
static void prop_ct_insert(struct prop *f, u64 lpn, u32 ppn)
{
	u64 a = prop_ct_b1(f, lpn), b = prop_ct_b2(f, lpn), t = prop_ct_load(f, a) <= prop_ct_load(f, b) ? a : b;
	int k = prop_ct_free_in(f, t);
	if (k < 0) { t = (t == a) ? b : a; k = prop_ct_free_in(f, t); }
	if (k < 0) dm_die("proposed: cuckoo2 insert without room (lpn %llu)\n", (unsigned long long)lpn);
	f->ct_lpn[t * 4 + k] = (u32)lpn; f->ct_ppn[t * 4 + k] = ppn;
}
static inline s64 prop_ct_must(const struct prop *f, u64 lpn)
{
	s64 e = prop_ct_find(f, lpn);
	if (e < 0) dm_die("proposed: cuckoo2 lookup of absent lpn %llu\n", (unsigned long long)lpn);
	return e;
}

/* ------------------------------------------------------------------ coarse entry (cuckoo2: flag bit + chunk index) */
#define PROP_FLAG  0x80000000u
#define PROP_SPL   0x40000000u   /* spill only: the IU's fragment pointers are in its FTP, not in the table */
#define PROP_CNONE 0x7FFFFFFFu   /* chunk-index mask / "no chunk" without spill (with spill: 0x3FFFFFFF) */
static inline int prop_flag(const struct prop *f, u64 iu) { return (f->coarse_map[iu] & PROP_FLAG) != 0; }
static inline int prop_spl(const struct prop *f, u64 iu) { return f->spill && (f->coarse_map[iu] & PROP_SPL); }
#define PROP_CLN   0x20000000u   /* spill_rp_clean only: table-resident IU whose FTP image is still valid (clean) */
static inline int prop_cln(const struct prop *f, u64 iu) { return f->rp_clean && (f->coarse_map[iu] & PROP_CLN); }
/* ftlsim rp_dirty: a clean IU's fragment set / locations are about to change -> its FTP image becomes stale */
static void prop_rp_dirty(struct prop *f, u64 iu)
{
	u32 k;
	if (!prop_cln(f, iu)) return;
	f->coarse_map[iu] &= ~PROP_CLN; f->rp_cdirty++;
	for (k = 0; k < f->U; k++) f->ftp[iu * f->U + k] = DM_NONE32;
}
/* chunk start ppn of iu, or DM_NONE32 */
static inline u32 prop_cm(const struct prop *f, u64 iu)
{
	u32 e = f->coarse_map[iu];
	if (!f->ct_on) return e;
	e &= f->cmask;
	return e == f->cmask ? DM_NONE32 : e * f->U;
}
/* set the chunk (DM_NONE32 = none); the flag bits are kept */
static inline void prop_cm_set(struct prop *f, u64 iu, u32 ppn)
{
	if (!f->ct_on) { f->coarse_map[iu] = ppn; return; }
	f->coarse_map[iu] = (f->coarse_map[iu] & ~f->cmask) | (ppn == DM_NONE32 ? f->cmask : ppn / f->U);
}

/* ------------------------------------------------------------------ fine index accessors (pool or cuckoo2) */
static inline s32 prop_fi(const struct prop *f, u64 iu) { return f->ct_on ? (prop_flag(f, iu) ? (s32)iu : -1) : f->fine_idx[iu]; }
/* fragment mask of a fine-resident IU (fi >= 0); cuckoo2: U table lookups. With rec_block and birth != NULL also
 * the latest birth of a host-frontier fine block holding one of the fragments (ftlsim host_birth, 0 = none). */
static u32 prop_fscan(const struct prop *f, s32 fi, u64 *birth)
{
	u32 m = 0, k; u64 bm = 0;
	int spl = f->ct_on && prop_spl(f, (u64)fi);
	for (k = 0; k < f->U; k++) {
		u32 ppn, b;
		if (spl) { ppn = f->ftp[(u64)fi * f->U + k]; if (ppn == DM_NONE32) continue; }
		else if (f->ct_on) { s64 e = prop_ct_find(f, (u64)fi * f->U + k); if (e < 0) continue; ppn = f->ct_ppn[e]; }
		else { if (!(f->fbitmap[fi] & (1u << k))) continue; ppn = f->fslot[(u64)fi * f->U + k]; }
		m |= 1u << k;
		if (birth) { b = ppn / f->P; if (f->bhost[b] && f->fbirth[b] > bm) bm = f->fbirth[b]; }
	}
	if (birth) *birth = bm;
	return m;
}
static inline u32 prop_fmask(const struct prop *f, s32 fi) { return f->ct_on ? prop_fscan(f, fi, NULL) : f->fbitmap[fi]; }
/* one scan of a fine-resident IU kept for reuse within a fold (fragment reads, invalidation, entry removal);
 * a spilled IU's pointers come from its FTP image (the caller has charged the FTP read), ent = -1 */
struct prop_fs { u32 mask; u32 ppn[32]; s64 ent[32]; };
static void prop_fscan_full(const struct prop *f, s32 fi, struct prop_fs *fs)
{
	u32 k; int spl = f->ct_on && prop_spl(f, (u64)fi);
	fs->mask = 0;
	for (k = 0; k < f->U; k++) {
		if (spl) { u32 p = f->ftp[(u64)fi * f->U + k]; if (p == DM_NONE32) continue; fs->ent[k] = -1; fs->ppn[k] = p; }
		else if (f->ct_on) { s64 e = prop_ct_find(f, (u64)fi * f->U + k); if (e < 0) continue; fs->ent[k] = e; fs->ppn[k] = f->ct_ppn[e]; }
		else { if (!(f->fbitmap[fi] & (1u << k))) continue; fs->ent[k] = -1; fs->ppn[k] = f->fslot[(u64)fi * f->U + k]; }
		fs->mask |= 1u << k;
	}
}
/* cuckoo2: does every page of the IU live in the table (d == U)? Stops at the first absent page. */
static int prop_iu_full(const struct prop *f, u64 iu)
{
	u32 k;
	if (prop_spl(f, iu)) { for (k = 0; k < f->U; k++) if (f->ftp[iu * f->U + k] == DM_NONE32) return 0; return 1; }
	for (k = 0; k < f->U; k++) if (prop_ct_find(f, iu * f->U + k) < 0) return 0;
	return 1;
}
static inline u32 prop_slot(const struct prop *f, s32 fi, u32 off)
{
	if (!f->ct_on) return f->fslot[(u64)fi * f->U + off];
	if (prop_spl(f, (u64)fi)) return f->ftp[(u64)fi * f->U + off];
	return f->ct_ppn[prop_ct_must(f, (u64)fi * f->U + off)];
}
static inline void prop_slot_set(struct prop *f, s32 fi, u32 off, u32 ppn)
{
	f->fepoch++;
	if (!f->ct_on) f->fslot[(u64)fi * f->U + off] = ppn;
	else if (prop_spl(f, (u64)fi)) f->ftp[(u64)fi * f->U + off] = ppn;   /* the FTP image; caller marks the FTP dirty */
	else { prop_rp_dirty(f, (u64)fi); f->ct_ppn[prop_ct_must(f, (u64)fi * f->U + off)] = ppn; }
}

/* rec_block host_birth (latest birth of a host-frontier fine block holding one of the IU's fragments) is computed
 * by prop_fscan together with the fragment mask. */

/* ------------------------------------------------------------------ spill: FTP I/O and the fine-GC batch */
/* ftlsim ftp_read: one 4 KiB map read unless the FTP is in SRAM or was already read in this GC batch */
static void prop_ftp_read(struct prop *f, u64 g, int why)
{
	if (f->gc_batch && f->rd_ep[g] == f->batch_id) return;
	if (g == f->ftp_buf) return;
	f->st->read[DM_R_MAP]++; f->fo.read(f->fo.ctx, DM_NOPPN, DM_R_MAP);
	f->sp_rd[why]++; f->ftp_buf = g;
	if (f->gc_batch) f->rd_ep[g] = f->batch_id;
}
/* ftlsim ftp_write: the FTP is rewritten out of place (new GTD entry); the new image stays in SRAM */
static void prop_ftp_write(struct prop *f, u64 g, int why)
{
	f->st->prog[DM_P_MAP]++; f->fo.prog(f->fo.ctx, DM_NOPPN, DM_P_MAP);
	f->sp_wr[why]++; f->ftp_buf = g;
}

/* The dirty-FTP set of a batch, in the iteration order of ftlsim's std::unordered_set<u64> (libc++: identity hash,
 * max_load_factor 1, bucket count 2 -> primes, kept across clear(); __emplace_unique_key_args / __do_rehash).
 * Only the order matters (it decides which FTP is left in SRAM); membership is dirty_ep[g] == batch_id. */
#define PROP_HS_END (-1)   /* end of the node list */
#define PROP_HS_NIL (-2)   /* empty bucket */
#define PROP_HS_BB  (-3)   /* bucket points at the before-begin node */
static inline u64 prop_hs_cons(u64 h, u64 bc) { return !(bc & (bc - 1)) ? h & (bc - 1) : (h < bc ? h : h % bc); }
static inline s32 prop_hs_next(const struct prop *f, s32 p) { return p == PROP_HS_BB ? f->hs.head : f->hs.nxt[p]; }
static inline void prop_hs_set_next(struct prop *f, s32 p, s32 v) { if (p == PROP_HS_BB) f->hs.head = v; else f->hs.nxt[p] = v; }
static u64 prop_next_prime(u64 n)
{
	for (;; n++) {
		u64 d; int pr = n >= 2;
		for (d = 2; pr && d * d <= n; d++) if (n % d == 0) pr = 0;
		if (pr) return n;
	}
}
static void prop_hs_rehash(struct prop *f, u64 n)
{
	s32 pp, cp; u64 i, ch, ph;
	if (n == 1) n = 2; else if (n & (n - 1)) n = prop_next_prime(n);
	if (n <= f->hs.bc) return;   /* growth only (a set is never shrunk) */
	if (n > f->hs.bcap) dm_die("proposed: spill dirty-set bucket array too small (%llu > %llu)\n", (unsigned long long)n, (unsigned long long)f->hs.bcap);
	f->hs.bc = n;
	for (i = 0; i < n; i++) f->hs.bkt[i] = PROP_HS_NIL;
	cp = f->hs.head;
	if (cp == PROP_HS_END) return;
	ch = prop_hs_cons(f->hs.key[cp], n); f->hs.bkt[ch] = PROP_HS_BB; ph = ch;
	for (pp = cp, cp = f->hs.nxt[cp]; cp != PROP_HS_END; cp = f->hs.nxt[pp]) {
		ch = prop_hs_cons(f->hs.key[cp], n);
		if (ch == ph) pp = cp;
		else if (f->hs.bkt[ch] == PROP_HS_NIL) { f->hs.bkt[ch] = pp; pp = cp; ph = ch; }
		else {
			f->hs.nxt[pp] = f->hs.nxt[cp];
			f->hs.nxt[cp] = prop_hs_next(f, f->hs.bkt[ch]);
			prop_hs_set_next(f, f->hs.bkt[ch], cp);
		}
	}
}
static void prop_hs_insert(struct prop *f, u32 key)   /* key not present */
{
	u64 bc = f->hs.bc, ch; s32 nd = (s32)f->hs.size, pn;
	if ((u64)f->hs.size + 1 > bc || bc == 0) {
		u64 a = 2 * bc + !(bc > 2 && !(bc & (bc - 1))), b = (u64)f->hs.size + 1;
		prop_hs_rehash(f, a > b ? a : b);
		bc = f->hs.bc;
	}
	ch = prop_hs_cons(key, bc);
	f->hs.key[nd] = key;
	pn = f->hs.bkt[ch];
	if (pn == PROP_HS_NIL) {
		f->hs.nxt[nd] = f->hs.head; f->hs.head = nd;
		f->hs.bkt[ch] = PROP_HS_BB;
		if (f->hs.nxt[nd] != PROP_HS_END) f->hs.bkt[prop_hs_cons(f->hs.key[f->hs.nxt[nd]], bc)] = nd;
	} else {
		f->hs.nxt[nd] = prop_hs_next(f, pn);
		prop_hs_set_next(f, pn, nd);
	}
	f->hs.size++;
}
static void prop_ftp_dirty(struct prop *f, u64 g)
{
	if (f->dirty_ep[g] == f->batch_id) return;
	f->dirty_ep[g] = f->batch_id;
	prop_hs_insert(f, (u32)g);
}
/* ftlsim gc_batch_begin / gc_batch_end (nesting: only the outermost batch reads/programs) */
static void prop_batch_begin(struct prop *f)
{
	if (!f->spill || f->gc_batch++) return;
	if (++f->batch_id == 0) {   /* epoch wrap: forget every stale mark */
		memset(f->rd_ep, 0, sizeof(u32) * f->n_ftp); memset(f->dirty_ep, 0, sizeof(u32) * f->n_ftp);
		f->batch_id = 1;
	}
}
static void prop_batch_end(struct prop *f)
{
	s32 p;
	if (!f->spill || --f->gc_batch > 0) return;
	/* FTPs whose IUs all left the spilled state need no update */
	for (p = f->hs.head; p != PROP_HS_END; p = f->hs.nxt[p]) if (f->grp_spilled[f->hs.key[p]]) prop_ftp_write(f, f->hs.key[p], 1);
	for (p = f->hs.head; p != PROP_HS_END; p = f->hs.nxt[p]) f->hs.bkt[prop_hs_cons(f->hs.key[p], f->hs.bc)] = PROP_HS_NIL;   /* clear() */
	f->hs.head = PROP_HS_END; f->hs.size = 0;
}

/* ------------------------------------------------------------------ fine pool index */
static void prop_pool_grow(struct prop *f)
{
	u32 nc = f->pool_cap * 2, U = f->U;
	u32 *nb = dm_zalloc(sizeof(u32) * (u64)nc), *ns = dm_zalloc(sizeof(u32) * (u64)nc * U);
	u64 *nl = dm_zalloc(sizeof(u64) * (u64)nc); s32 *nf = dm_zalloc(sizeof(s32) * (u64)nc);
	if (!nb || !ns || !nl || !nf) dm_die("proposed: fine pool grow to %u failed\n", nc);
	memcpy(nb, f->fbitmap, sizeof(u32) * (u64)f->pool_cap);
	memcpy(ns, f->fslot, sizeof(u32) * (u64)f->pool_cap * U);
	memcpy(nl, f->flast, sizeof(u64) * (u64)f->pool_cap);
	memcpy(nf, f->fpool_free, sizeof(s32) * (u64)f->nfpool_free);
	dm_free(f->fbitmap); dm_free(f->fslot); dm_free(f->flast); dm_free(f->fpool_free);
	f->fbitmap = nb; f->fslot = ns; f->flast = nl; f->fpool_free = nf; f->pool_cap = nc;
	dm_log("proposed: fine pool index grown to %u entries\n", nc);
}

static s32 prop_fine_get_or_create(struct prop *f, u64 iu)
{
	s32 i;
	if (f->ct_on) {   /* the IU's own slot; the flag is set when the first fragment is inserted; flast 0 */
		if (!prop_flag(f, iu) && f->flast) f->flast[iu] = 0;
		return (s32)iu;
	}
	i = f->fine_idx[iu];
	if (i >= 0) return i;
	if (f->nfpool_free) i = f->fpool_free[--f->nfpool_free];
	else { if (f->npool == f->pool_cap) prop_pool_grow(f); i = (s32)f->npool++; }
	f->fbitmap[i] = 0; f->flast[i] = 0;
	f->fine_idx[iu] = i;
	return i;
}

static void prop_fine_release(struct prop *f, u64 iu, const struct prop_fs *fs)
{
	s32 i;
	f->fepoch++;
	if (f->rp_clean) prop_rp_dirty(f, iu);
	if (f->ct_on && prop_spl(f, iu)) {   /* ftlsim fine_release of a spilled IU: its FTP copy just becomes stale */
		u32 bm = fs->mask;
		while (bm) { u32 k = (u32)dm_ctz64(bm); bm &= bm - 1; f->ftp[iu * f->U + k] = DM_NONE32; }
		f->coarse_map[iu] &= ~(PROP_FLAG | PROP_SPL);
		f->grp_spilled[iu / f->ipf]--; f->spages -= dm_popcount(fs->mask);
		return;
	}
	if (f->ct_on) {   /* ftlsim: ct_remove every fragment of the IU (entries from the scan), clear the flag */
		u32 bm = fs->mask;
		while (bm) {
			u32 k = (u32)dm_ctz64(bm); s64 e = fs->ent[k]; bm &= bm - 1;
			f->ct_lpn[e] = DM_NONE32; f->ct_ppn[e] = DM_NONE32;
		}
		f->coarse_map[iu] &= ~PROP_FLAG;
		return;
	}
	i = f->fine_idx[iu];
	if (i < 0) return;
	f->fine_idx[iu] = -1;
	f->fbitmap[i] = 0;
	f->fpool_free[f->nfpool_free++] = i;
}

/* ------------------------------------------------------------------ blocks */
static void prop_erase_block(struct prop *f, s32 b)
{
	if (f->bvalid[b] != 0) dm_die("proposed: erase block %d with valid=%d tag=%d\n", b, f->bvalid[b], f->btag[b]);
	if (f->btag[b] == PROP_FINE) f->fine_blocks--;
	else if (f->btag[b] == PROP_COARSE) f->coarse_blocks--;
	else dm_die("proposed: erase free block %d\n", b);
	f->btag[b] = PROP_FREE;
	f->freelist[f->nfree++] = b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, (u32)b);
}

static s32 prop_pop_free(struct prop *f, int t)
{
	s32 b;
	if (!f->nfree) dm_die("proposed: free list exhausted (T=%u too small?)\n", f->T);
	b = f->freelist[--f->nfree];
	f->btag[b] = (u8)t; f->bvalid[b] = 0; f->bseq[b] = ++f->alloc_seq;
	if (t == PROP_FINE) f->fine_blocks++; else f->coarse_blocks++;
	return b;
}

static s32 prop_pick_victim(const struct prop *f, int t)
{
	s32 best = -1, bv = 0x7fffffff; u32 b;
	for (b = 0; b < f->NB; b++) {
		if (f->btag[b] != t || prop_is_frontier(f, (s32)b)) continue;
		if (f->bvalid[b] < bv) { bv = f->bvalid[b]; best = (s32)b; }
	}
	return best;
}

static void prop_fine_reclaim(struct prop *f, int force_fold);
static void prop_coarse_gc(struct prop *f);

static int prop_coarse_starved(const struct prop *f)
{
	u64 fv = 0, resident, slack; u32 b;
	for (b = 0; b < f->NB; b++) if (f->btag[b] == PROP_FINE) fv += (u64)(s64)f->bvalid[b];
	resident = f->L - fv;
	slack = resident / 32; if (slack < f->P) slack = f->P;
	return (u64)(s64)((s32)f->NB - f->fine_blocks - 1 - (s32)f->T) * (u64)f->P < resident + slack;
}

/* ftlsim Sim::alloc_block (proposed, fine_cap=blocks) */
static s32 prop_alloc_block(struct prop *f, int t)
{
	if (t == PROP_FINE && !f->in_fine_reclaim) {
		int stall = 0;
		while (f->fine_blocks >= f->fq || prop_coarse_starved(f)) {
			s32 before = f->fine_blocks;
			if (f->onefront_fix && f->want_ff && f->ff.blk >= 0 && f->ff.next < f->P) { f->n_onefront_reuse++; return -1; }
			prop_fine_reclaim(f, stall >= 4);
			if (f->fine_blocks >= before) stall++; else stall = 0;
			if (stall > 64) dm_die("proposed: fine tier livelock: quota=%d\n", f->fq);
		}
	}
	if (!f->in_cgc) {
		int stall = 0;
		while (f->nfree <= f->T) {
			u32 before = f->nfree;
			int useless;
			s32 cv = prop_pick_victim(f, PROP_COARSE);
			useless = cv >= 0 && f->bvalid[cv] >= (s32)f->P;
			if (!useless) {
				if (f->gc_cost && !f->in_fine_reclaim && !f->in_shrink) {
					s32 vc = prop_pick_victim(f, PROP_COARSE), vf = prop_pick_victim(f, PROP_FINE);
					int use_fine = vf >= 0 && (vc < 0 || f->bvalid[vf] < f->bvalid[vc]);
					if (use_fine) { f->in_shrink = 1; prop_fine_reclaim(f, 0); f->in_shrink = 0; f->n_shrink++; }
					else prop_coarse_gc(f);
				} else prop_coarse_gc(f);
			}
			if (useless || f->nfree <= before) {
				s32 dead = -1; u32 b2;
				++stall;
				for (b2 = 0; b2 < f->NB; b2++)
					if (f->btag[b2] == PROP_FINE && !prop_is_frontier(f, (s32)b2) && (s32)b2 != f->reclaim_victim && f->bvalid[b2] == 0) { dead = (s32)b2; break; }
				if (dead >= 0) { prop_erase_block(f, dead); stall = 0; continue; }
				if ((useless || stall > 56) && !f->in_shrink && !f->in_fine_reclaim && prop_pick_victim(f, PROP_FINE) >= 0) {
					f->in_shrink = 1; prop_fine_reclaim(f, 1); f->in_shrink = 0;
					f->n_shrink++;
					if (f->nfree > before) { stall = 0; continue; }
				}
				/* draw on the reserve, but keep the last block: a nested coarse GC needs it */
				if ((useless || stall > 56) && (f->in_shrink || f->in_fine_reclaim) && f->nfree > 1) { f->n_reserve_draw++; break; }
				if (stall > 64) dm_die("proposed: GC makes no progress (free=%u fine=%d coarse=%d quota=%d NB=%u)\n",
						       f->nfree, f->fine_blocks, f->coarse_blocks, f->fq, f->NB);
			} else stall = 0;
		}
	}
	return prop_pop_free(f, t);
}

/* ftlsim cc_eff(): the age cascade is off in uniform mode */
static inline int prop_cc_eff(const struct prop *f) { return f->uni ? 0 : f->coarse_class; }

/* one page slot on the fine frontier (gc_frontier=1: separate GC frontier, except uniform mode + uni_onefront) */
static u32 prop_fine_alloc_page(struct prop *f, int gc)
{
	int sep = gc && !(f->uni && f->uni_onefront);
	struct prop_fr *fr = sep ? &f->fgf : &f->ff;
	if (fr->blk < 0 || fr->next == f->P) {
		int saved = f->want_ff; s32 nb;
		f->want_ff = fr == &f->ff;
		nb = prop_alloc_block(f, PROP_FINE);
		f->want_ff = saved;
		if (nb >= 0) { fr->blk = nb; fr->next = 0; f->fbirth[fr->blk] = f->fine_clock; f->bhost[fr->blk] = fr == &f->ff; }
		/* else (onefront_fix): the fine reclaim's GC copies re-opened ff; keep filling it */
	}
	return (u32)fr->blk * f->P + fr->next++;
}

/* U consecutive slots on a coarse frontier, incl. the coarse_class cascade and its fallbacks */
static u32 prop_coarse_alloc_extent(struct prop *f, int gc, int cls)
{
	int ccls = cls, cc = prop_cc_eff(f), i; struct prop_fr *fp; u32 ppn;
	if (cc > PROP_MAXCC) cc = PROP_MAXCC;
	if (ccls > cc) ccls = cc;
	fp = gc ? (ccls > 0 ? &f->ccf[ccls - 1] : &f->cgf) : &f->cf;
	if (ccls > 0 && (fp->blk < 0 || f->P - fp->next < f->U) && f->nfree <= 1) {
		if (f->cgf.blk >= 0 && f->P - f->cgf.next >= f->U) fp = &f->cgf;
		else { for (i = 0; i < PROP_MAXCC; i++) if (f->ccf[i].blk >= 0 && f->P - f->ccf[i].next >= f->U) { fp = &f->ccf[i]; break; } }
	} else if (fp == &f->cgf && (f->cgf.blk < 0 || f->P - f->cgf.next < f->U) && f->in_cgc && !f->nfree) {
		/* last resort: inside coarse GC with no free block left, a full shared GC frontier spills into an
		 * open class frontier instead of popping an empty free list */
		for (i = 0; i < PROP_MAXCC; i++) if (f->ccf[i].blk >= 0 && f->P - f->ccf[i].next >= f->U) { fp = &f->ccf[i]; f->n_spill++; break; }
	}
	if (fp->blk >= 0 && f->P - fp->next < f->U) { u32 k; for (k = fp->next; k < f->P; k++) f->p2l[(u32)fp->blk * f->P + k] = DM_NONE32; fp->next = f->P; }
	if (fp->blk < 0 || fp->next == f->P) {
		fp->blk = prop_alloc_block(f, PROP_COARSE); fp->next = 0;
		f->bccls[fp->blk] = (u8)(fp == &f->cf ? 0 : (fp == &f->cgf ? 1 : ccls));
	}
	ppn = (u32)fp->blk * f->P + fp->next; fp->next += f->U;
	return ppn;
}

/* ------------------------------------------------------------------ write primitives */
/* fs: the IU's fragment scan if the caller has a current one (NULL = scan here) */
static void prop_invalidate_iu(struct prop *f, u64 iu, const struct prop_fs *fs)
{
	s32 fi = prop_fi(f, iu); u32 d = 0, old; struct prop_fs own;
	if (fi >= 0) {
		u32 bm;
		if (!fs) {
			if (prop_spl(f, iu)) prop_ftp_read(f, iu / f->ipf, 2);   /* the fragment pointers to invalidate */
			prop_fscan_full(f, fi, &own); fs = &own;
		}
		bm = fs->mask; d = dm_popcount(bm); f->fpages -= d;
		while (bm) {
			u32 off = (u32)dm_ctz64(bm); bm &= bm - 1;
			f->bvalid[prop_blk(f, fs->ppn[off])]--;
		}
		prop_fine_release(f, iu, fs);
	}
	old = prop_cm(f, iu);   /* d == U: a not-yet-dropped dead chunk, U - d = 0 */
	if (old != DM_NONE32) f->bvalid[prop_blk(f, old)] -= (s32)(f->U - d);
}

/* hostmask: offsets programmed as DM_P_HOST, the rest as src2 (eager: RMW filler); else all use src */
static void prop_install_extent(struct prop *f, u64 iu, u32 ppn, int src, u32 hostmask, const struct prop_fs *fs)
{
	u32 k; int dsrc;
	for (k = 0; k < f->U; k++) f->p2l[ppn + k] = (u32)(iu * f->U + k);
	f->bvalid[prop_blk(f, ppn)] += (s32)f->U;
	dsrc = src == PS_HOST ? DM_P_HOST : src == PS_CLEAN || src == PS_REFOLD ? DM_P_GC : DM_P_FOLD;
	for (k = 0; k < f->U; k++) {
		int s = dsrc;
		if (src == PS_HOST && hostmask && !(hostmask & (1u << k))) s = DM_P_RMW;
		prop_prog(f, ppn + k, s);
	}
	prop_invalidate_iu(f, iu, fs);   /* the flash ops above do not change FTL state: a caller's scan is still current */
	prop_cm_set(f, iu, ppn);
}

/* close an open frontier: its unwritten tail becomes ordinary garbage (never programmed) */
static void prop_close_frontier(struct prop *f, struct prop_fr *fr)
{
	u32 j;
	for (j = fr->next; j < f->P; j++) f->p2l[(u32)fr->blk * f->P + j] = DM_NONE32;
	fr->blk = -1; fr->next = 0;
}

/* ftlsim adapt_tick: after each canonical fine-pool turnover of host fine pages, pick the mode */
static void prop_adapt_tick(struct prop *f, int ow)
{
	u64 sum, win = (u64)f->fq_base * f->P; int u, i;
	if (ow) f->aw_ow++; else f->aw_new++;
	sum = f->aw_ow + f->aw_new;
	if (sum < win) return;
	if (f->adapt == 2) {
		/* uniform iff absorption <= that of a random cache of the pool's size: aw_ow*L < sum*fq_base*P.
		 * Exact in integers; ftlsim's doubles are exact too while both products stay below 2^53. */
		const u64 lim = 1ull << 53;
		if (f->aw_ow <= lim / f->L && sum <= lim / win) u = f->aw_ow * f->L < sum * win;
		else { f->n_adapt_fp++; PROP_FPU_BEGIN(); u = prop_adapt_fp(f->aw_ow, sum, f->L, f->fq_base, f->P); PROP_FPU_END(); }
	} else u = 2 * f->aw_ow < sum;
	f->aw_ow = f->aw_new = 0;
	if (u == f->uni) return;
	f->uni = u; f->n_switch++;
	f->fq = f->uni ? f->fq_uni : f->fq_base;
	if (f->uni && f->uni_onefront && f->fgf.blk >= 0) prop_close_frontier(f, &f->fgf);   /* GC copies join ff */
	if (f->uni) for (i = 0; i < PROP_MAXCC; i++) if (f->ccf[i].blk >= 0) prop_close_frontier(f, &f->ccf[i]);
}

/* host 4K write into the fine tier */
static void prop_fine_write(struct prop *f, u64 lpn)
{
	u32 ppn = prop_fine_alloc_page(f, 0), bit, off; u64 iu; s32 fi;
	f->p2l[ppn] = (u32)lpn;
	f->bvalid[prop_blk(f, ppn)]++;
	prop_prog(f, ppn, DM_P_HOST); f->fine_clock++;
	iu = lpn / f->U; off = (u32)(lpn % f->U);
	fi = prop_fine_get_or_create(f, iu);
	if (f->rp_clean) prop_rp_dirty(f, iu);
	if (f->flast) f->flast[fi] = f->fine_clock;
	bit = 1u << off;
	f->fepoch++;
	if (f->ct_on) {   /* flag clear: certainly a new fragment; else one lookup decides */
		s64 e = prop_flag(f, iu) ? prop_ct_find(f, lpn) : -1;
		if (e >= 0) {
			f->bvalid[prop_blk(f, f->ct_ppn[e])]--;
			f->frag_freed++; f->fine_host_ow++;
			if (f->adapt) prop_adapt_tick(f, 1);
			f->ct_ppn[e] = ppn;
		} else {
			u32 cm = prop_cm(f, iu);
			f->fine_host_new++;
			if (f->adapt) prop_adapt_tick(f, 0);
			prop_ct_insert(f, lpn, ppn); f->fpages++;
			if (cm != DM_NONE32) f->bvalid[prop_blk(f, cm)]--;
			f->coarse_map[iu] |= PROP_FLAG;
			/* ftlsim drops the chunk when d reaches U; here it is dropped lazily (prop_coarse_gc) */
		}
		return;
	}
	if (f->fbitmap[fi] & bit) {
		u32 old = f->fslot[(u64)fi * f->U + off];
		f->bvalid[prop_blk(f, old)]--;
		f->frag_freed++; f->fine_host_ow++;
		if (f->adapt) prop_adapt_tick(f, 1);
		f->fslot[(u64)fi * f->U + off] = ppn;
	} else {
		f->fine_host_new++;
		if (f->adapt) prop_adapt_tick(f, 0);
		f->fslot[(u64)fi * f->U + off] = ppn; f->fpages++;
		if (f->coarse_map[iu] != DM_NONE32) f->bvalid[prop_blk(f, f->coarse_map[iu])]--;
		f->fbitmap[fi] |= bit;
		if (dm_popcount(f->fbitmap[fi]) == f->U && f->coarse_map[iu] != DM_NONE32) { f->coarse_map[iu] = DM_NONE32; f->n_drop++; }
	}
}

static u32 prop_req_mask(const struct prop *f, u64 start, u32 n)
{
	u32 mask = 0, i;
	for (i = 0; i < n; i++) mask |= 1u << ((start + i) % f->U);
	return mask;
}

/* eager_worth with eager_u = -1 (cost-based): U/n < 1 + p*U/max(1,u) */
/* the cost test as a function of u = |request | fragments| (ftlsim eager_worth's expression) */
static int prop_eager_test(struct prop *f, u32 n, u32 u)
{
	u32 uu = u > 1 ? u : 1;
	u64 a, S, lhs, rhs, diff, big; int r;
	/* exact: U*S*uu < n*(S*uu + a*U) with p = a/S; decide in integers unless within 2^-40 of a tie */
	if (f->frag_folded + f->frag_freed) { a = f->frag_folded; S = f->frag_folded + f->frag_freed; } else { a = 1; S = 2; }
	lhs = (u64)f->U * S * uu; rhs = (u64)n * (S * uu + a * f->U);
	diff = lhs > rhs ? lhs - rhs : rhs - lhs; big = lhs > rhs ? lhs : rhs;
	if (diff > (big >> 40) + 2) return lhs < rhs;
	f->n_eager_fp++;
	PROP_FPU_BEGIN();
	r = prop_eager_fp(f->U, n, u, f->frag_folded, f->frag_freed);
	PROP_FPU_END();
	return r;
}

/* eager_worth with eager_u = -1 (cost-based): U/n < 1 + p*U/max(1,u). The test is monotone in u (true for small
 * u, in exact and in double arithmetic), and n <= u <= U, so the IU's fragment set (U table lookups) is scanned
 * only when the two bounds disagree. */
static int prop_eager_worth(struct prop *f, u64 iu, u64 start, u32 n)
{
	u32 mask = prop_req_mask(f, start, n); s32 fi = prop_fi(f, iu);
	if (fi < 0) return prop_eager_test(f, n, dm_popcount(mask));
	if (!prop_eager_test(f, n, dm_popcount(mask))) return 0;
	if (prop_eager_test(f, n, f->U)) return 1;
	return prop_eager_test(f, n, dm_popcount(mask | prop_fmask(f, fi)));
}

static void prop_coarse_write(struct prop *f, u64 iu, int src, u32 hostmask)
{
	int cls = 0; u32 ppn;
	if (prop_cc_eff(f) > 0 && src != PS_HOST) cls = (src == PS_CLEAN) ? f->cgc_cls + 1 : 1;
	ppn = prop_coarse_alloc_extent(f, src != PS_HOST, cls);
	prop_install_extent(f, iu, ppn, src, hostmask, NULL);
}

static void prop_eager_write(struct prop *f, u64 iu, u64 start, u32 n)
{
	u32 req = prop_req_mask(f, start, n), mask = req, old = prop_cm(f, iu), k;
	s32 fi = prop_fi(f, iu);
	if (fi >= 0) {
		u32 bm = prop_fmask(f, fi); mask |= bm;
		for (k = 0; k < f->U; k++) if (bm & (1u << k)) prop_rd(f, prop_slot(f, fi, k), DM_R_RMW);  /* fragments folded in */
	}
	if (old != DM_NONE32) for (k = 0; k < f->U; k++) if (!(mask & (1u << k))) prop_rd(f, old + k, DM_R_RMW);  /* rest from old chunk */
	f->n_eager++;
	prop_coarse_write(f, iu, PS_HOST, req);   /* n host pages + (U-n) filler counted as RMW */
}

/* Fold(IU): fragments + extent -> new extent on the coarse GC frontier; refold = inside coarse GC */
static void prop_fold(struct prop *f, u64 iu, int refold)
{
	u32 ppn = prop_coarse_alloc_extent(f, 1, 0);   /* may run nested coarse GC -> evaluate state AFTER */
	s32 fi = prop_fi(f, iu); struct prop_fs fs;
	u32 bm, d, old = prop_cm(f, iu), k;
	int src, rs = refold ? DM_R_GC : DM_R_FOLD;
	fs.mask = 0;
	if (fi >= 0) {
		if (prop_spl(f, iu)) {   /* the fragment pointers to fold in: one FTP read (ftlsim: in invalidate_iu) */
			if (refold) f->sp_refold++; else f->sp_fold++;
			prop_ftp_read(f, iu / f->ipf, 2);
		}
		prop_fscan_full(f, fi, &fs);
	}
	bm = fs.mask; d = dm_popcount(bm);
	for (k = 0; k < f->U; k++) if (bm & (1u << k)) prop_rd(f, fs.ppn[k], rs);
	if (old != DM_NONE32 && d == f->U) old = DM_NONE32;   /* cuckoo2 lazy drop: the chunk is dead (ftlsim: NONE) */
	if (old != DM_NONE32) { for (k = 0; k < f->U; k++) if (!(bm & (1u << k))) prop_rd(f, old + k, rs); src = refold ? PS_REFOLD : PS_FOLD; }
	else if (d == f->U) src = refold ? PS_REFOLD : PS_FOLD;
	else src = PS_ZEROFILL;
	if (refold) { f->n_refold++; f->frag_freed += d; } else { f->n_fold++; f->frag_folded += d; }
	prop_install_extent(f, iu, ppn, src, 0, fi >= 0 ? &fs : NULL);
}

/* ------------------------------------------------------------------ reclaim */
static int prop_fine_live(const struct prop *f, u32 ppn, u32 lpn, s32 *fip)
{
	u64 iu = lpn / f->U; u32 off = lpn % f->U; s32 fi = prop_fi(f, iu);
	*fip = fi;
	if (fi < 0) return 0;
	if (prop_spl(f, iu)) return f->ftp[lpn] == ppn;   /* simulator check (the OOB lpn + FTP pointer on a device) */
	if (f->ct_on) { s64 e = prop_ct_find(f, lpn); return e >= 0 && f->ct_ppn[e] == ppn; }
	return (f->fbitmap[fi] & (1u << off)) && f->fslot[(u64)fi * f->U + off] == ppn;
}

/* fold every IU with valid fragments in block b, then erase (forced fold) */
static void prop_fold_block(struct prop *f, s32 b)
{
	s32 rv_save = f->reclaim_victim, fi; u32 base = (u32)b * f->P, s;
	f->reclaim_victim = b;
	prop_batch_begin(f);
	for (s = 0; s < f->P && f->bvalid[b] > 0; s++) {
		u32 ppn = base + s, lpn = f->p2l[ppn];
		if (lpn == DM_NONE32) continue;
		if (!prop_fine_live(f, ppn, lpn, &fi)) continue;
		prop_fold(f, lpn / f->U, 0);
	}
	if (f->bvalid[b] != 0) dm_die("proposed: fold_block left valid=%d\n", f->bvalid[b]);
	prop_batch_end(f);
	prop_erase_block(f, b);
	f->reclaim_victim = rv_save;
}

/* ftlsim's per-IU fold decision in greedy_reclaim_block: fold iff d*v > U*(P-v) (cost) and, for cost_cold, the IU is
 * cold: its latest host-frontier fragment block (rec_block host_birth, else flast) is not newer than the victim.
 * Exact, but the fragment-set scan stops as soon as the outcome is decided (v = victim valid, constant per victim):
 * d can no longer exceed the threshold -> no fold; a fragment in a newer host block -> not cold -> no fold. */
static int prop_greedy_test(const struct prop *f, s32 fi, s32 b)
{
	s64 v = f->victim_v, lim = (s64)f->U * ((s64)f->P - v);   /* fold needs d*v > lim */
	u32 k, found = 0, cheaper = 0; u64 fb = f->fbirth[b];
	if (2 * v <= (s64)f->P) return 0;                          /* d <= U: never cheaper */
	if (f->cost_cold && !f->rec_block && f->flast[fi] > fb) return 0;
	int spl = f->ct_on && prop_spl(f, (u64)fi);
	for (k = 0; k < f->U; k++) {
		u32 ppn;
		if (spl) { ppn = f->ftp[(u64)fi * f->U + k]; if (ppn == DM_NONE32) goto absent; }
		else if (f->ct_on) { s64 e = prop_ct_find(f, (u64)fi * f->U + k); if (e < 0) goto absent; ppn = f->ct_ppn[e]; }
		else { if (!(f->fbitmap[fi] & (1u << k))) goto absent; ppn = f->fslot[(u64)fi * f->U + k]; }
		found++;
		if (f->cost_cold && f->rec_block) { u32 bk = ppn / f->P; if (f->bhost[bk] && f->fbirth[bk] > fb) return 0; }   /* not cold */
		if (!cheaper && (s64)found * v > lim) { cheaper = 1; if (!(f->cost_cold && f->rec_block)) return 1; }
		continue;
	absent:
		if (!cheaper && (s64)(found + f->U - k - 1) * v <= lim) return 0;   /* the rest cannot make it cheaper */
	}
	return cheaper;
}

/* greedy_reclaim_block with fine_policy cost / cost_cold (parameter-free cold test, fold_tau < 0) */
static void prop_greedy_reclaim_block(struct prop *f, s32 b)
{
	s32 rv_save = f->reclaim_victim, fi; u32 base = (u32)b * f->P, s0;
	/* decision memo: a "copy" decision for an IU stays valid for its next live page in this victim while only our
	 * own copies changed its fragments (fepoch): d is unchanged and recency can only grow. */
	u64 m_iu = ~0ull, m_ep = 0;
	f->reclaim_victim = b;
	f->n_greedy++;
	prop_batch_begin(f);
	for (s0 = 0; s0 < f->P && f->bvalid[b] > 0; s0++) {
		u32 ppn = base + s0, lpn = f->p2l[ppn], np; u64 iu; int sp;
		if (lpn == DM_NONE32) continue;
		if (!prop_fine_live(f, ppn, lpn, &fi)) continue;
		iu = lpn / f->U;
		sp = prop_spl(f, iu);
		if (sp) prop_ftp_read(f, iu / f->ipf, 3);   /* d + recency of a spilled IU come from its FTP */
		if (!(m_iu == iu && m_ep == f->fepoch)) {
			if (prop_greedy_test(f, fi, b)) { if (sp) f->sp_gcfold++; prop_fold(f, iu, 0); continue; }
			m_iu = iu; m_ep = f->fepoch;
		}
		np = prop_fine_alloc_page(f, 1);
		if (!prop_fine_live(f, ppn, lpn, &fi)) {
			f->n_wasted++;
			/* ftlsim leaves this slot unwritten; keep in-order programming with an uncounted pad page */
			f->n_pad++; f->fo.prog(f->fo.ctx, np, DM_P_OTHER);
			continue;
		}
		prop_rd(f, ppn, DM_R_GC);
		f->p2l[np] = lpn; f->bvalid[prop_blk(f, np)]++; prop_prog(f, np, DM_P_GC);
		prop_slot_set(f, fi, lpn % f->U, np); f->bvalid[b]--;
		if (prop_spl(f, iu)) {   /* the pointer lives in the FTP: rewrite it once at the end of this victim */
			prop_ftp_read(f, iu / f->ipf, 3); prop_ftp_dirty(f, iu / f->ipf); f->sp_copy++;
		}
		if (m_iu == iu && m_ep + 1 == f->fepoch) m_ep = f->fepoch;   /* only our copy changed the IU */
	}
	if (f->bvalid[b] != 0) dm_die("proposed: greedy_reclaim left valid=%d\n", f->bvalid[b]);
	prop_batch_end(f);
	prop_erase_block(f, b);
	f->reclaim_victim = rv_save;
}

static void prop_fine_reclaim(struct prop *f, int force_fold)
{
	s32 b, v;
	f->in_fine_reclaim = 1;
	b = prop_pick_victim(f, PROP_FINE);
	if (b < 0) dm_die("proposed: no fine victim\n");
	v = f->bvalid[b];
	if (v == 0) { prop_erase_block(f, b); f->in_fine_reclaim = 0; return; }
	if (force_fold) { f->n_forced++; prop_fold_block(f, b); f->in_fine_reclaim = 0; return; }
	f->victim_v = v; prop_greedy_reclaim_block(f, b);
	f->in_fine_reclaim = 0;
}

static void prop_coarse_gc(struct prop *f)
{
	s32 b; u32 base, e;
	f->in_cgc = 1;
	if (f->cgc_cb && !f->uni) { PROP_FPU_BEGIN(); b = prop_pick_cb_fp(f); PROP_FPU_END(); }   /* uniform mode: greedy */
	else b = prop_pick_victim(f, PROP_COARSE);
	if (b < 0) dm_die("proposed: no coarse victim (free=%u fine=%d coarse=%d)\n", f->nfree, f->fine_blocks, f->coarse_blocks);
	f->n_cgc++;
	prop_batch_begin(f);   /* refolds of spilled IUs: one FTP read per FTP per victim */
	f->cgc_cls = f->bccls[b];
	base = (u32)b * f->P;
	for (e = 0; e < f->P / f->U && f->bvalid[b] > 0; e++) {
		u32 ppn = base + e * f->U, lpn = f->p2l[ppn], k; u64 iu; s32 fi;
		if (lpn == DM_NONE32) continue;
		iu = lpn / f->U;
		if (prop_cm(f, iu) != ppn) continue;
		fi = prop_fi(f, iu);
		if (fi >= 0 && f->ct_on && prop_iu_full(f, iu)) {
			/* cuckoo2: every page superseded by a fragment -> dead chunk (ftlsim dropped it at d == U) */
			prop_cm_set(f, iu, DM_NONE32); f->n_drop++; if (prop_spl(f, iu)) f->sp_lazy_drop++;
			continue;
		}
		if (fi >= 0 && (f->ct_on || f->fbitmap[fi])) prop_fold(f, iu, 1);
		else {
			for (k = 0; k < f->U; k++) prop_rd(f, ppn + k, DM_R_GC);
			prop_coarse_write(f, iu, PS_CLEAN, 0);
		}
	}
	if (f->bvalid[b] != 0) dm_die("proposed: coarse_gc left valid=%d\n", f->bvalid[b]);
	prop_batch_end(f);
	prop_erase_block(f, b);
	f->in_cgc = 0;
}

/* ftlsim ct_pick / ct_ensure's candidate rule: among the IUs (other than cur_iu) of the <= 8 entries of lpn's bucket
 * pair, the one with the most fragments, ties -> older recency (host_birth with rec_block, else flast), first found.
 * An IU seen before scores the same again and cannot win the strict test: its fragment-set scan is skipped. */
static u64 prop_ct_best(const struct prop *f, u64 lpn, u64 cur_iu, int pc)   /* pc: prefer clean IUs (spill_rp_clean, spilling eviction) */
{
	u64 bb[2], best = ~0ull, bl = ~0ull, seen[8]; int bd = -1, bcl = -1, i, k, nseen = 0, q, dup;
	bb[0] = prop_ct_b1(f, lpn); bb[1] = prop_ct_b2(f, lpn);
	for (i = 0; i < 2; i++) for (k = 0; k < 4; k++) {
		u32 e = f->ct_lpn[bb[i] * 4 + k]; u64 iu, rc; s32 f2; int d;
		if (e == DM_NONE32) continue;
		iu = e / f->U; if (iu == cur_iu) continue;
		f2 = prop_fi(f, iu); if (f2 < 0) continue;
		for (q = 0, dup = 0; q < nseen; q++) dup |= seen[q] == iu;
		if (dup) continue;
		seen[nseen++] = iu;
		{ u64 hb = 0; d = dm_popcount(prop_fscan(f, f2, f->rec_block ? &hb : NULL)); rc = f->rec_block ? hb : f->flast[f2]; }
		{ int cl = pc ? prop_cln(f, iu) : 0;
		  if (cl > bcl || (cl == bcl && (d > bd || (d == bd && rc < bl)))) { bcl = cl; bd = d; bl = rc; best = iu; } }
	}
	return best;
}

/* ftlsim spill_one: move one table-resident IU to the spilled state (its entries leave the table, their ppns go to the
 * FTP image, spilled bit set); no FTP program here, the caller programs the FTP once */
static void prop_spill_one(struct prop *f, u64 iu)
{
	struct prop_fs fs; u32 bm, d;
	prop_fscan_full(f, (s32)iu, &fs);
	bm = fs.mask; d = dm_popcount(bm);
	while (bm) {
		u32 k = (u32)dm_ctz64(bm); s64 e = fs.ent[k]; bm &= bm - 1;
		f->ct_lpn[e] = DM_NONE32; f->ct_ppn[e] = DM_NONE32;
		f->ftp[iu * f->U + k] = fs.ppn[k];
	}
	f->coarse_map[iu] |= PROP_SPL; f->grp_spilled[iu / f->ipf]++; f->spages += d;
	if (f->rp_clean) f->coarse_map[iu] &= ~PROP_CLN;   /* only with spill_rp_clean: bit 29 is chunk index otherwise */
	if (f->rp_bits) {   /* spill_rp >= 2: the read counter restarts; a read run in progress no longer counts as one */
		f->coarse_map[iu] &= ~(((1u << f->rp_bits) - 1) << f->rp_shift);
		if (f->rp_last == iu) f->rp_last = ~0ull;
	}
	f->sp_ius++; f->sp_pages += d; f->fepoch++;
}

/* ftlsim spill: spill victim iu (spill=2: plus every other table-resident IU of its FTP except cur_iu) with one FTP
 * read-modify-write (the read only if the FTP holds other spilled IUs' pointers and is not in SRAM) */
static void prop_spill(struct prop *f, u64 iu, u64 cur_iu)
{
	u64 g = iu / f->ipf;
	if (prop_cln(f, iu)) { f->rp_cevict++; prop_spill_one(f, iu); return; }   /* clean: FTP image still valid, no FTP I/O */
	if (f->grp_spilled[g]) prop_ftp_read(f, g, 4);
	f->sp_n++;
	prop_spill_one(f, iu);
	if (f->spill == 2) {   /* the FTP's ipf coarse entries (4 cache lines at ipf = 64) */
		u64 lo = g * f->ipf, hi = lo + f->ipf, j;
		if (hi > f->NIU) hi = f->NIU;
		for (j = lo; j < hi; j++) if (j != iu && j != cur_iu && prop_flag(f, j) && !prop_spl(f, j)) prop_spill_one(f, j);
	}
	prop_ftp_write(f, g, 0);
}

/* ftlsim unspill: a host write to a spilled IU reads its FTP and puts the fragment pointers back into the table
 * (making room by spilling others: no flash data writes, so no GC runs in here); the FTP copy becomes stale */
static void prop_unspill(struct prop *f, u64 iu, int rd)   /* rd: read-promotion (spill_rp) */
{
	u64 g = iu / f->ipf; u32 bm = 0, ppn[32], k, d;
	prop_ftp_read(f, g, 1);
	for (k = 0; k < f->U; k++) { u32 p = f->ftp[iu * f->U + k]; if (p != DM_NONE32) { bm |= 1u << k; ppn[k] = p; } }
	d = dm_popcount(bm);
	f->coarse_map[iu] &= ~PROP_SPL; f->grp_spilled[g]--; f->spages -= d;
	if (rd) { f->rp_n++; f->rp_pages += d; } else { f->sp_unspill++; f->sp_unspill_pages += d; }
	if (rd && f->rp_clean) f->coarse_map[iu] |= PROP_CLN;   /* clean promotion: the FTP image stays valid */
	f->fepoch++;
	while (bm) {
		u64 l; int tries;
		k = (u32)dm_ctz64(bm); bm &= bm - 1; l = iu * f->U + k;
		if (!(rd && f->rp_clean)) f->ftp[l] = DM_NONE32;
		for (tries = 0; !prop_ct_room(f, l); tries++) {
			u64 v = prop_ct_best(f, l, iu, f->rp_clean);
			if (v == ~0ull || tries > 16) dm_die("proposed: spill: cannot make room to un-spill IU %llu\n", (unsigned long long)iu);
			prop_spill(f, v, iu);
		}
		prop_ct_insert(f, l, ppn[k]);
	}
}

/* ftlsim read_promote (spill_rp): after the host read that paid the FTP read of spilled IU iu (and its data read),
 * un-spill it like a host write would; same = the previous host-read lookup was of this IU (one request). K >= 2: the
 * coarse entry's read counter (reads since the spill) must reach K-1 first. spill_rp_rate: only with credit > 0,
 * charged 1000 per FTP program spent (spills of evicted IUs). No recursion: unspill -> spill never un-spills. */
static void prop_read_promote(struct prop *f, u64 iu, int same)
{
	u64 w0, r0, dw;
	if (f->spill_rp >= 2) {
		u32 m = (1u << f->rp_bits) - 1, cnt = (f->coarse_map[iu] >> f->rp_shift) & m;
		if (same) return;
		if (cnt < (u32)f->spill_rp - 1) {
			f->coarse_map[iu] = (f->coarse_map[iu] & ~(m << f->rp_shift)) | ((cnt + 1) << f->rp_shift);
			f->rp_wait++; return;
		}
	}
	if (f->spill_rp_rate && f->rp_credit <= 0) { f->rp_rate++; return; }
	w0 = f->st->prog[DM_P_MAP]; r0 = f->st->read[DM_R_MAP];
	prop_unspill(f, iu, 1);
	dw = f->st->prog[DM_P_MAP] - w0;
	f->rp_wr += dw; f->rp_rd += f->st->read[DM_R_MAP] - r0;
	if (f->spill_rp_rate) f->rp_credit -= 1000 * (s64)dw;
}

/* ftlsim ct_ensure: make room for a NEW fine page of lpn's IU before the host write enters the fine pool.
 * While both buckets are full, fold the IU (other than the one being written) with the most fragments among
 * the 8 entries (cheapest fold), ties -> older recency (host_birth with rec_block, else flast), first found. */
static void prop_ct_ensure(struct prop *f, u64 lpn)
{
	u64 cur_iu = lpn / f->U; s32 fi; int tries;
	if (!f->ct_on) return;
	fi = prop_fi(f, cur_iu);
	if (prop_spl(f, cur_iu)) prop_unspill(f, cur_iu, 0);   /* spilled bit: FTP read, pointers back into the table */
	if (fi >= 0 && prop_ct_find(f, lpn) >= 0) return;   /* overwrite of a resident page: no new entry */
	for (tries = 0; ; ++tries) {
		u64 best;
		/* pool_cap (1 always, 4 uniform mode only): at the fine pool's copy-GC liveness bound
		 * (quota - open fine frontiers - 1) x P a new page evicts like a full bucket pair, at most once */
		s32 capb = f->fq - ((f->uni && f->uni_onefront) ? 1 : 2) - 1;
		int room = prop_ct_room(f, lpn);
		int over = room && tries == 0 && ((f->pcap & 1) || ((f->pcap & 4) && f->uni)) && capb > 0 &&
			   f->fpages >= (u64)capb * f->P;
		if (room && !over) break;
		if (tries > 16) dm_die("proposed: cuckoo2 cannot make room for lpn %llu\n", (unsigned long long)lpn);
		best = prop_ct_best(f, lpn, cur_iu, f->rp_clean && !over);
		if (best == ~0ull) { if (over) break; dm_die("proposed: cuckoo2 buckets hold only the IU being written\n"); }
		if (over) f->n_pool_evict++; else f->n_evict++;
		if (f->spill && !over) prop_spill(f, best, cur_iu);   /* DRAM-driven eviction: spill instead of fold */
		else prop_fold(f, best, 0);   /* pool_cap eviction (liveness) still folds */
	}
}

/* ------------------------------------------------------------------ interface */
static int prop_sep(char c) { return c == ' ' || c == ',' || c == '\t'; }

/* key=value tokens separated by ' ' or ',' (the harness splits its args on spaces); unknown keys die */
static void prop_parse(struct prop *f, const char *p, u64 *fq_blocks)
{
	static const char *keys[] = { "U", "fine_quota_blocks", "fine_quota_ppm", "fine_extra", "cgc_vict_cb", "coarse_class",
				      "fine_policy_cost_cold", "gc_select_cost", "fine_full", "gc_frontier", "adapt", "uni_onefront",
				      "rec_block", "fine_index_cuckoo2", "map_budget_ppm", "onefront_fix", "pool_cap", "spill", "spill_ipf", "spill_rp", "spill_rp_rate", "spill_rp_clean", NULL };
	while (p && *p) {
		const char *k, *ke; u64 v = 0; int i;
		while (*p && prop_sep(*p)) p++;
		if (!*p) break;
		k = p; while (*p && *p != '=' && !prop_sep(*p)) p++;
		ke = p;
		if (*p != '=') dm_die("proposed: bad param token '%.*s'\n", (int)(ke - k), k);
		p++;
		if (*p < '0' || *p > '9') dm_die("proposed: param %.*s needs an unsigned integer\n", (int)(ke - k), k);
		while (*p >= '0' && *p <= '9') v = v * 10 + (u64)(*p++ - '0');
		if (*p == 'K') { v <<= 10; p++; } else if (*p == 'M') { v <<= 20; p++; } else if (*p == 'G') { v <<= 30; p++; }
		if (*p && !prop_sep(*p)) dm_die("proposed: bad value for %.*s\n", (int)(ke - k), k);
		for (i = 0; keys[i]; i++) if ((size_t)(ke - k) == strlen(keys[i]) && !strncmp(k, keys[i], ke - k)) break;
		if (!keys[i]) dm_die("proposed: unsupported option '%.*s' (supported: fine_quota_blocks fine_quota_ppm fine_extra cgc_vict_cb coarse_class fine_policy_cost_cold gc_select_cost fine_full adapt uni_onefront rec_block fine_index_cuckoo2 map_budget_ppm onefront_fix pool_cap spill spill_ipf spill_rp spill_rp_rate spill_rp_clean)\n", (int)(ke - k), k);
		switch (i) {
		case 0: break;                                  /* IU size: taken from dm_geom.U */
		case 1: *fq_blocks = v; break;
		case 2: f->fine_quota_ppm = v; break;
		case 3: f->fine_extra = v != 0; break;
		case 4: f->cgc_cb = v != 0; break;
		case 5: if (v > PROP_MAXCC) dm_die("proposed: coarse_class=%llu > %d unsupported\n", (unsigned long long)v, PROP_MAXCC);
			f->coarse_class = (int)v; break;
		case 6: f->cost_cold = v != 0; break;
		case 7: f->gc_cost = v != 0; break;
		case 8: f->fine_full = v != 0; break;
		case 9: if (v != 1) dm_die("proposed: gc_frontier=0 unsupported\n"); break;
		case 10: if (v > 2) dm_die("proposed: adapt=%llu unsupported\n", (unsigned long long)v);
			f->adapt = (int)v; break;
		case 11: f->uni_onefront = v != 0; break;
		case 12: f->rec_block = v != 0; break;
		case 13: f->ct_on = v != 0; break;
		case 14: f->map_budget_ppm = v; break;
		case 15: f->onefront_fix = v != 0; break;
		case 16: if (v != 0 && v != 1 && v != 4) dm_die("proposed: pool_cap=%llu unsupported (0, 1, 4)\n", (unsigned long long)v);
			f->pcap = (int)v; break;
		case 17: if (v > 2) dm_die("proposed: spill=%llu unsupported (0, 1, 2)\n", (unsigned long long)v);
			f->spill = (int)v; break;
		case 18: if (v > 4096) dm_die("proposed: spill_ipf=%llu too large\n", (unsigned long long)v);
			f->ipf = (u32)v; break;
		case 19: if (v > 16) dm_die("proposed: spill_rp=%llu unsupported (0..16)\n", (unsigned long long)v);
			f->spill_rp = (int)v; break;
		case 20: if (v > 1000000000ull) dm_die("proposed: spill_rp_rate=%llu too large\n", (unsigned long long)v);
			f->spill_rp_rate = v; break;
		case 21: f->rp_clean = v != 0; break;
		}
	}
}

static void *prop_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct prop *f = dm_zalloc(sizeof(*f)); u64 i, np, fqb = 0;
	int k;
	if (!f) return NULL;
	f->g = *g; f->fo = *fo; f->st = st;
	f->L = g->L; f->P = g->P; f->U = g->U; f->NB = g->NB; f->T = g->T;
	if (f->U < 1 || f->U > 32 || f->P % f->U || f->L % f->U) dm_die("proposed: bad geometry P=%u U=%u L=%llu\n", f->P, f->U, (unsigned long long)f->L);
	f->NIU = f->L / f->U;
	/* ftlsim defaults: fine_quota=0.022, fine_extra=1, cgc_vict=greedy, coarse_class=0, fine_policy=cost, gc_select=coarse */
	f->fine_quota_ppm = 22000; f->fine_extra = 1;
	prop_parse(f, params, &fqb);
	if (fqb) f->fq = (s32)fqb;
	else {
		/* ftlsim: fq = max(1, (int)(NB0 * fine_quota)); NB = NB0 (+ fq if fine_extra). Recover NB0 from NB. */
		PROP_FPU_BEGIN();
		if (!f->fine_extra) f->fq = prop_fq_fp(f->NB, f->fine_quota_ppm);
		else {
			u32 nb0; f->fq = 0;
			for (nb0 = f->NB; nb0 >= 1; nb0--) { s32 q = prop_fq_fp(nb0, f->fine_quota_ppm); if (nb0 + (u32)q == f->NB) { f->fq = q; break; } }
		}
		PROP_FPU_END();
		if (!f->fq) dm_die("proposed: NB=%u is not NB0 + fine quota for fine_quota_ppm=%llu\n", f->NB, (unsigned long long)f->fine_quota_ppm);
	}
	f->fq_base = f->fq;
	{	/* uniform-mode quota: every block not needed for the logical data, the GC reserve and the open
		 * frontiers (4, or 3 with uni_onefront) -- geometry only */
		s32 data_blocks = (s32)((f->L + f->P - 1) / f->P);
		s32 qu = (s32)f->NB - data_blocks - (s32)f->T - (f->uni_onefront ? 3 : 4) - 1;
		f->fq_uni = qu > f->fq_base ? qu : f->fq_base;
	}
	f->cmask = PROP_CNONE;
	if (f->spill && !f->ct_on) dm_die("proposed: spill needs fine_index_cuckoo2=1\n");
	if ((f->spill_rp || f->spill_rp_rate) && !f->spill) dm_die("proposed: spill_rp needs spill\n");
	if (f->rp_clean && !f->spill_rp) dm_die("proposed: spill_rp_clean needs spill_rp\n");
	if (f->ct_on) {
		/* ftlsim: ent = 4 + 4 (+4 write time when the cold test needs per-IU recency and rec_block=0);
		 * fine_bytes = map_budget*L*4 - NIU*4 (- GTD with spill); ct_nb = fine_bytes / (4*ent) */
		f->ct_ent = 8 + (!f->rec_block && f->cost_cold ? 4 : 0);
		if (f->spill) {   /* dense fixed FTPs of ipf IUs (4096 / (U x 4 B) = 64 at U=16); GTD = 4 B per FTP */
			if (!f->ipf) { f->ipf = 4096 / (f->U * 4); if (!f->ipf) f->ipf = 1; }
			f->n_ftp = (f->NIU + f->ipf - 1) / f->ipf; f->gtd_bytes = f->n_ftp * 4;
			f->cmask = PROP_CNONE >> 1;   /* chunk index loses one bit to the spilled flag */
			if (f->rp_clean) f->cmask >>= 1;   /* bit 29: clean */
			if (f->spill_rp >= 2) {   /* + ceil(log2 K) read-counter bits right below the spilled (/ clean) bit */
				while ((1 << f->rp_bits) < f->spill_rp) f->rp_bits++;
				f->rp_shift = 30 - (u32)f->rp_clean - (u32)f->rp_bits; f->cmask >>= f->rp_bits;
			}
			f->rp_last = ~0ull;
		}
		PROP_FPU_BEGIN();
		f->ct_nb = prop_ct_nb_fp(f->map_budget_ppm, f->L, f->U, f->ct_ent, f->gtd_bytes);
		PROP_FPU_END();
		if (!f->ct_nb) dm_die("proposed: map_budget_ppm=%llu leaves no room for the fine table\n", (unsigned long long)f->map_budget_ppm);
		if (f->L > 0xFFFFFFFEull || f->NIU > 0x7FFFFFFFull) dm_die("proposed: cuckoo2 needs L < 2^32\n");
		/* coarse entry = 1 flag bit (+ 1 spilled bit) + 31 (30)-bit chunk index (start ppn / U; cmask = "no chunk") */
		if ((u64)f->NB * f->P / f->U >= f->cmask) dm_die("proposed: %llu chunks do not fit the %d-bit chunk index\n",
								 (unsigned long long)((u64)f->NB * f->P / f->U), (f->spill ? 30 : 31) - f->rp_bits - f->rp_clean);
		{	u64 nc = (u64)f->NB * f->P / f->U; int bits = 0; while ((1ull << bits) <= nc) bits++;   /* chunk ids 0..nc-1 + cmask */
			dm_log("proposed: coarse entry 4 B = %s + chunk index (%llu chunks need %d of %d bits); no per-IU bitmap\n",
			       f->spill ? "fragment flag bit + spilled bit" : "1 fragment flag bit", (unsigned long long)nc, bits, (f->spill ? 30 : 31) - f->rp_bits - f->rp_clean); }
		if (f->spill_rp)
			dm_log("proposed: spill_rp=%d: read-promotion of spilled IUs (%s), read counter %d bits in the coarse entry, rate guard %llu FTP programs / 1000 host pages%s, clean promotion %d\n",
			       f->spill_rp, f->spill_rp == 1 ? "every read" : "K-th read since the spill", f->rp_bits,
			       (unsigned long long)f->spill_rp_rate, f->spill_rp_rate ? "" : " (off)", f->rp_clean);
		if (f->spill) {
			u64 coarse = f->NIU * 4, table = f->ct_nb * 4 * f->ct_ent, budget = f->map_budget_ppm * f->L * 4 / 1000000;
			dm_log("proposed: spill=%d: %llu FTPs x %u IUs (FTP %u B), GTD %llu B; mapping DRAM coarse %llu + GTD %llu + table %llu = %llu B "
			       "(budget %llu B = %llu ppm x L x 4 B)\n", f->spill, (unsigned long long)f->n_ftp, f->ipf, f->ipf * f->U * 4,
			       (unsigned long long)f->gtd_bytes, (unsigned long long)coarse, (unsigned long long)f->gtd_bytes, (unsigned long long)table,
			       (unsigned long long)(coarse + f->gtd_bytes + table), (unsigned long long)budget, (unsigned long long)f->map_budget_ppm);
		}
		dm_log("proposed: cuckoo2 fine table: %llu buckets x 4 = %llu entries, entry %u B, budget %llu ppm of page map\n",
		       (unsigned long long)f->ct_nb, (unsigned long long)(f->ct_nb * 4), f->ct_ent, (unsigned long long)f->map_budget_ppm);
	}
	np = (u64)f->NB * f->P;
	if (np > 0xFFFFFFFEull) dm_die("proposed: too many physical pages for u32 ppn\n");
	f->coarse_map = dm_zalloc(sizeof(u32) * f->NIU);
	f->p2l = dm_zalloc(sizeof(u32) * np);
	f->btag = dm_zalloc(f->NB); f->bvalid = dm_zalloc(sizeof(s32) * f->NB); f->bseq = dm_zalloc(sizeof(u64) * f->NB);
	f->freelist = dm_zalloc(sizeof(s32) * f->NB);
	f->bccls = dm_zalloc(f->NB + 1); f->fbirth = dm_zalloc(sizeof(u64) * (f->NB + 1)); f->bhost = dm_zalloc(f->NB + 1);
	if (f->ct_on) {
		/* the table is the fine lpn->ppn index; the "has fragments" flag lives in the coarse entry
		 * (+ per-IU write time only if !rec_block, not used by V3) */
		u64 ne = f->ct_nb * 4;
		f->ct_lpn = dm_zalloc(sizeof(u32) * ne); f->ct_ppn = dm_zalloc(sizeof(u32) * ne);
		if (!f->rec_block) f->flast = dm_zalloc(sizeof(u64) * f->NIU);
		if (!f->ct_lpn || !f->ct_ppn || (!f->rec_block && !f->flast)) dm_die("proposed: alloc failed\n");
		for (i = 0; i < ne; i++) f->ct_lpn[i] = f->ct_ppn[i] = DM_NONE32;
		if (f->spill) {
			/* FTP images (flash content, 4 B per logical page), per-FTP spilled count, batch marks, dirty-set order */
			f->ftp = dm_zalloc(sizeof(u32) * f->L);
			f->grp_spilled = dm_zalloc(sizeof(u32) * f->n_ftp);
			f->rd_ep = dm_zalloc(sizeof(u32) * f->n_ftp); f->dirty_ep = dm_zalloc(sizeof(u32) * f->n_ftp);
			f->hs.key = dm_zalloc(sizeof(u32) * f->n_ftp); f->hs.nxt = dm_zalloc(sizeof(s32) * f->n_ftp);
			f->hs.bcap = 2 * f->n_ftp + 4096; f->hs.bkt = dm_zalloc(sizeof(s32) * f->hs.bcap);
			if (!f->ftp || !f->grp_spilled || !f->rd_ep || !f->dirty_ep || !f->hs.key || !f->hs.nxt || !f->hs.bkt)
				dm_die("proposed: spill alloc failed\n");
			for (i = 0; i < f->L; i++) f->ftp[i] = DM_NONE32;
			f->hs.head = PROP_HS_END; f->hs.bc = 0; f->batch_id = 1; f->ftp_buf = ~0ull;
		}
	} else {
		/* fine pool index: live entries <= valid fine pages; start at (quota + 4) blocks worth, grow if needed */
		u64 cap = (u64)((f->adapt && f->fq_uni > f->fq ? f->fq_uni : f->fq) + 4) * f->P; if (cap > f->NIU) cap = f->NIU; if (cap < 64) cap = 64; f->pool_cap = (u32)cap;
		f->fine_idx = dm_zalloc(sizeof(s32) * f->NIU);
		f->fbitmap = dm_zalloc(sizeof(u32) * (u64)f->pool_cap); f->fslot = dm_zalloc(sizeof(u32) * (u64)f->pool_cap * f->U);
		f->flast = dm_zalloc(sizeof(u64) * (u64)f->pool_cap); f->fpool_free = dm_zalloc(sizeof(s32) * (u64)f->pool_cap);
		if (!f->fine_idx || !f->fbitmap || !f->fslot || !f->flast || !f->fpool_free) dm_die("proposed: alloc failed\n");
		for (i = 0; i < f->NIU; i++) f->fine_idx[i] = -1;
	}
	if (!f->coarse_map || !f->p2l || !f->btag || !f->bvalid || !f->bseq || !f->freelist || !f->bccls ||
	    !f->fbirth || !f->bhost) dm_die("proposed: alloc failed\n");
	for (i = 0; i < f->NIU; i++) f->coarse_map[i] = f->ct_on ? f->cmask : DM_NONE32;
	for (i = 0; i < np; i++) f->p2l[i] = DM_NONE32;
	for (i = 0; i < f->NB; i++) f->freelist[i] = (s32)(f->NB - 1 - i);
	f->nfree = f->NB;
	f->ff.blk = f->cf.blk = f->fgf.blk = f->cgf.blk = -1;
	for (k = 0; k < PROP_MAXCC; k++) f->ccf[k].blk = -1;
	f->reclaim_victim = -1;
	dm_log("proposed: L=%llu NIU=%llu NB=%u P=%u U=%u T=%u fine_quota_blocks=%d fine_extra=%d cgc_vict=%s coarse_class=%d fine_policy=%s gc_select=%s fine_full=%d "
	       "adapt=%d quota_uni=%d uni_onefront=%d rec_block=%d fine_index=%s onefront_fix=%d pool_cap=%d spill=%d\n",
	       (unsigned long long)f->L, (unsigned long long)f->NIU, f->NB, f->P, f->U, f->T, f->fq, f->fine_extra,
	       f->cgc_cb ? "cb" : "greedy", f->coarse_class, f->cost_cold ? "cost_cold" : "cost", f->gc_cost ? "cost" : "coarse", f->fine_full,
	       f->adapt, f->fq_uni, f->uni_onefront, f->rec_block, f->ct_on ? "cuckoo2" : "hash", f->onefront_fix, f->pcap, f->spill);
	return f;
}

static void prop_destroy(void *c)
{
	struct prop *f = c;
	if (f->spill)   /* the glue prints no core report: leave the spill counters in the log (cumulative incl. warmup) */
		dm_log("proposed: spill=%d end: spills %llu (ius %llu pages %llu) unspills %llu (pages %llu) spilled_fold %llu refold %llu gcfold %llu "
		       "gccopy %llu ftp_reads host %llu unspill %llu fold %llu gc %llu rmw %llu ftp_progs spill %llu gc %llu spilled_pages_end %llu "
		       "reads_spilled %llu reads_ftp %llu\n", f->spill, (unsigned long long)f->sp_n, (unsigned long long)f->sp_ius,
		       (unsigned long long)f->sp_pages, (unsigned long long)f->sp_unspill, (unsigned long long)f->sp_unspill_pages,
		       (unsigned long long)f->sp_fold, (unsigned long long)f->sp_refold, (unsigned long long)f->sp_gcfold, (unsigned long long)f->sp_copy,
		       (unsigned long long)f->sp_rd[0], (unsigned long long)f->sp_rd[1], (unsigned long long)f->sp_rd[2], (unsigned long long)f->sp_rd[3],
		       (unsigned long long)f->sp_rd[4], (unsigned long long)f->sp_wr[0], (unsigned long long)f->sp_wr[1], (unsigned long long)f->spages,
		       (unsigned long long)f->rd_spilled, (unsigned long long)f->rd_ftp);
	if (f->spill_rp)
		dm_log("proposed: spill_rp=%d end: promotions %llu (pages %llu) counted %llu rate_skips %llu ftp_progs %llu ftp_reads %llu credit %lld clean %d evictions %llu dirtied %llu\n",
		       f->spill_rp, (unsigned long long)f->rp_n, (unsigned long long)f->rp_pages, (unsigned long long)f->rp_wait,
		       (unsigned long long)f->rp_rate, (unsigned long long)f->rp_wr, (unsigned long long)f->rp_rd, (long long)f->rp_credit,
		       f->rp_clean, (unsigned long long)f->rp_cevict, (unsigned long long)f->rp_cdirty);
	dm_free(f->coarse_map); dm_free(f->fine_idx); dm_free(f->p2l); dm_free(f->btag); dm_free(f->bvalid); dm_free(f->bseq);
	dm_free(f->freelist); dm_free(f->bccls); dm_free(f->fbirth); dm_free(f->bhost); dm_free(f->ct_lpn); dm_free(f->ct_ppn);
	dm_free(f->fbitmap); dm_free(f->fslot); dm_free(f->flast); dm_free(f->fpool_free);
	dm_free(f->ftp); dm_free(f->grp_spilled); dm_free(f->rd_ep); dm_free(f->dirty_ep); dm_free(f->hs.key); dm_free(f->hs.nxt); dm_free(f->hs.bkt);
	dm_free(f);
}

/* ftlsim Sim::host_write, proposed branch */
static void prop_write(void *c, u64 lpn, u64 n)
{
	struct prop *f = c; u64 cur = lpn, end = lpn + n, U = f->U;
	/* ftlsim keeps frag_folded/frag_freed in Stats, reset with the stats at warmup: follow a stats reset */
	if (f->st->host_pages < f->last_hp) f->frag_folded = f->frag_freed = 0;
	f->st->host_pages += n;
	if (f->spill_rp_rate) {   /* spill_rp rate guard: refill per host page reaching the FTL, burst ipf programs */
		f->rp_credit += (s64)n * (s64)f->spill_rp_rate;
		if (f->rp_credit > 1000 * (s64)f->ipf) f->rp_credit = 1000 * (s64)f->ipf;
	}
	if (f->uni) f->uni_host += n;
	while (cur < end) {
		if (cur % U == 0 && cur + U <= end) {
			u64 iu = cur / U, i;
			if (f->fine_full && prop_fi(f, iu) >= 0 && f->fine_blocks < f->fq) {
				/* hot (fine-resident) IU fully overwritten: keep it page-mapped */
				for (i = 0; i < U; i++) { prop_ct_ensure(f, cur + i); prop_fine_write(f, cur + i); }
			} else prop_coarse_write(f, iu, PS_HOST, 0);
			cur += U;
		} else {
			u64 iu = cur / U, e = (iu + 1) * U; u32 m; int keep_fine;
			if (e > end) e = end;
			m = (u32)(e - cur);
			keep_fine = f->fine_full && prop_fi(f, iu) >= 0 && f->fine_blocks < f->fq;
			if (!keep_fine && prop_eager_worth(f, iu, cur, m)) { prop_eager_write(f, iu, cur, m); cur = e; }
			else { while (cur < e) { prop_ct_ensure(f, cur); prop_fine_write(f, cur); cur++; } }
		}
	}
	f->last_hp = f->st->host_pages;
}

static void prop_read(void *c, u64 lpn, u64 n)
{
	struct prop *f = c; u64 p;
	for (p = lpn; p < lpn + n; p++) {
		u64 iu = p / f->U; u32 off = (u32)(p % f->U); s32 fi = prop_fi(f, iu); u32 cm;
		int same = iu == f->rp_last;
		if (f->spill_rp >= 2) f->rp_last = iu;
		if (f->ct_on && fi >= 0 && prop_spl(f, iu)) {
			/* spilled bit: the page may be a fragment or in the chunk -> read the FTP first (unless in SRAM);
			 * the glue serializes the map read before the dependent data read */
			u64 m0 = f->st->read[DM_R_MAP]; u32 q;
			f->rd_spilled++;
			prop_ftp_read(f, iu / f->ipf, 0);
			if (f->st->read[DM_R_MAP] > m0) f->rd_ftp++;
			q = f->ftp[p];
			if (f->spill_rp) {   /* data read first (from the FTP pointer or the chunk), then the promotion */
				if (q != DM_NONE32) prop_rd(f, q, DM_R_HOST);
				else if ((cm = prop_cm(f, iu)) != DM_NONE32) prop_rd(f, cm + off, DM_R_HOST);
				prop_read_promote(f, iu, same);
				continue;
			}
			if (q != DM_NONE32) { prop_rd(f, q, DM_R_HOST); continue; }
		} else if (f->ct_on) {   /* flag clear: 1 coarse-entry access; flag set: + one table lookup (<= 2 buckets) */
			s64 e = fi >= 0 ? prop_ct_find(f, p) : -1;
			if (e >= 0) { prop_rd(f, f->ct_ppn[e], DM_R_HOST); continue; }
		} else if (fi >= 0 && (f->fbitmap[fi] & (1u << off))) { prop_rd(f, f->fslot[(u64)fi * f->U + off], DM_R_HOST); continue; }
		cm = prop_cm(f, iu);
		if (cm != DM_NONE32) prop_rd(f, cm + off, DM_R_HOST);
	}
}

static void prop_flush(void *c) { (void)c; }

static void prop_counts(const struct prop *f, u64 *ius, u64 *pages)
{
	u64 i, a = 0, b = 0;
	if (f->ct_on) {   /* flagged IUs, table entries */
		for (i = 0; i < f->NIU; i++) a += prop_flag(f, i);
		for (i = 0; i < f->ct_nb * 4; i++) b += f->ct_lpn[i] != DM_NONE32;
	} else for (i = 0; i < f->NIU; i++) { s32 fi = prop_fi(f, i); if (fi >= 0) { a++; b += dm_popcount(f->fbitmap[fi]); } }
	*ius = a; *pages = b;
}

/* ftlsim report() RAM model: NIU*4 + hash_bytes(fine_ius, 4+2+4 (+4 cost_cold)) + fine_pages*4, printed %.0f.
 * hash_bytes = k*B/0.7 = 10kB/7 whose fraction is a multiple of 1/7 (never 1/2): round-to-nearest in integers. */
static u64 prop_mem(void *c)
{
	struct prop *f = c; u64 ius, pages, B;
	if (f->ct_on) return f->NIU * 4 + f->gtd_bytes + f->ct_nb * 4 * f->ct_ent;   /* ftlsim: NIU*4 (+ GTD) + ct_nb*4*cuckoo_entry() */
	prop_counts(f, &ius, &pages);
	B = 10 + (f->cost_cold ? 4 : 0);
	return f->NIU * 4 + pages * 4 + (ius * B * 20 + 7) / 14;
}

static int prop_verify(void *c)
{
	struct prop *f = c; u64 l, fine_valid = 0; u32 b; int bad = 0; s32 nf = 0, nc = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->NB);
	if (!v) return 1;
	u64 sp_pages = 0, nsp = 0; u32 *gs = NULL;
	if (f->spill) { gs = dm_zalloc(sizeof(u32) * f->n_ftp); if (!gs) { dm_free(v); return 1; } }
	for (l = 0; l < f->L; l++) {
		u64 iu = l / f->U; u32 off = (u32)(l % f->U); s32 fi = prop_fi(f, iu); s64 e = -1; int spl = f->ct_on && prop_spl(f, iu);
		if (spl && fi < 0) { if (!bad) dm_log("proposed verify: spilled bit without the flag on IU %llu\n", (unsigned long long)iu); bad = 1; }
		if (f->ct_on && fi >= 0) e = prop_ct_find(f, l);
		if (spl && e >= 0) { if (!bad) dm_log("proposed verify: spilled IU %llu has a table entry\n", (unsigned long long)iu); bad = 1; }
		if (prop_cln(f, iu) && (spl || fi < 0)) { if (!bad) dm_log("proposed verify: clean IU %llu is spilled or has no fragments\n", (unsigned long long)iu); bad = 1; }
		if (prop_cln(f, iu) && !spl && f->ftp[l] != (e >= 0 ? f->ct_ppn[e] : DM_NONE32)) { if (!bad) dm_log("proposed verify: stale FTP image of clean lpn %llu\n", (unsigned long long)l); bad = 1; }
		if (f->spill && !spl && !prop_cln(f, iu) && f->ftp[l] != DM_NONE32) { if (!bad) dm_log("proposed verify: FTP pointer of lpn %llu on a non-spilled IU\n", (unsigned long long)l); bad = 1; }
		if (spl && off == 0) { nsp++; gs[iu / f->ipf]++; }
		if (f->ct_on && fi >= 0 && off == 0 && !prop_fmask(f, fi)) { if (!bad) dm_log("proposed verify: flag set on IU %llu without fragments\n", (unsigned long long)iu); bad = 1; }
		if (spl ? f->ftp[l] != DM_NONE32 : f->ct_on ? e >= 0 : (fi >= 0 && (f->fbitmap[fi] & (1u << off)))) {
			u32 ppn;
			fine_valid++;
			if (spl) sp_pages++;
			ppn = spl ? f->ftp[l] : f->ct_on ? f->ct_ppn[e] : f->fslot[(u64)fi * f->U + off];
			if (ppn / f->P >= f->NB) { if (!bad) dm_log("proposed verify: fine lpn %llu bad ppn %u\n", (unsigned long long)l, ppn); bad = 1; continue; }
			v[prop_blk(f, ppn)]++;
		}
		else if (prop_cm(f, iu) != DM_NONE32) v[prop_blk(f, prop_cm(f, iu) + off)]++;
	}
	if (f->ct_on) {   /* table <-> fine pool: every fine page has exactly one entry, nothing else does */
		u64 used = 0, e;
		for (e = 0; e < f->ct_nb * 4; e++) {
			u32 x = f->ct_lpn[e];
			if (x == DM_NONE32) continue;
			used++;
			if (x >= f->L || !prop_flag(f, x / f->U) || prop_spl(f, x / f->U) || (e / 4 != prop_ct_b1(f, x) && e / 4 != prop_ct_b2(f, x))) {
				if (!bad) dm_log("proposed verify: cuckoo2 stale/misplaced entry %u\n", x);
				bad = 1;
			}
		}
		if (used + f->spages != fine_valid) { dm_log("proposed verify: cuckoo2 has %llu entries + %llu spilled pages, fine pool %llu pages\n", (unsigned long long)used, (unsigned long long)f->spages, (unsigned long long)fine_valid); bad = 1; }
		if (fine_valid != f->fpages) { dm_log("proposed verify: fine pages %llu != tracked %llu\n", (unsigned long long)fine_valid, (unsigned long long)f->fpages); bad = 1; }
	}
	if (f->spill) {
		u64 g;
		if (sp_pages != f->spages) { dm_log("proposed verify: spilled pages %llu != tracked %llu\n", (unsigned long long)sp_pages, (unsigned long long)f->spages); bad = 1; }
		for (g = 0; g < f->n_ftp; g++) if (gs[g] != f->grp_spilled[g]) { if (!bad) dm_log("proposed verify: FTP %llu spilled count %u != %u\n", (unsigned long long)g, f->grp_spilled[g], gs[g]); bad = 1; }
		if (f->gc_batch || f->hs.size) { dm_log("proposed verify: GC batch left open\n"); bad = 1; }
		if (!bad) dm_log("proposed verify: spill ok: %llu spilled IUs, %llu spilled pages\n", (unsigned long long)nsp, (unsigned long long)sp_pages);
		dm_free(gs);
	}
	for (b = 0; b < f->NB; b++) {
		if (v[b] != f->bvalid[b]) { if (!bad) dm_log("proposed verify: block %u tag=%u computed=%d tracked=%d\n", b, f->btag[b], v[b], f->bvalid[b]); bad = 1; }
		if (f->btag[b] == PROP_FINE) nf++; else if (f->btag[b] == PROP_COARSE) nc++;
	}
	if (nf != f->fine_blocks || nc != f->coarse_blocks || (u32)(nf + nc) + f->nfree != f->NB) { dm_log("proposed verify: block counts\n"); bad = 1; }
	if (!f->ct_on) for (l = 0; l < f->NIU; l++) if (f->fine_idx[l] >= 0 && !f->fbitmap[f->fine_idx[l]]) { dm_log("proposed verify: empty fine entry\n"); bad = 1; break; }
	dm_free(v);
	return bad;
}

static void prop_report(void *c, char *buf, int len)
{
	struct prop *f = c; u64 ius, pages;
	prop_counts(f, &ius, &pages);
	snprintf(buf, len, "\"proposed\": {\"fine_quota_blocks\": %d, \"fold\": %llu, \"refold\": %llu, \"eager\": %llu, \"eager_fp_ties\": %llu, "
		 "\"cgc\": %llu, \"greedy\": %llu, \"forced_fold\": %llu, \"shrink_fold\": %llu, \"extent_drop\": %llu, \"wasted_slots\": %llu, "
		 "\"pad_progs\": %llu, \"fine_ius_end\": %llu, \"fine_pages_end\": %llu, \"fine_blocks_end\": %d, \"coarse_blocks_end\": %d, \"free_end\": %u}",
		 f->fq, (unsigned long long)f->n_fold, (unsigned long long)f->n_refold, (unsigned long long)f->n_eager,
		 (unsigned long long)f->n_eager_fp, (unsigned long long)f->n_cgc, (unsigned long long)f->n_greedy,
		 (unsigned long long)f->n_forced, (unsigned long long)f->n_shrink, (unsigned long long)f->n_drop,
		 (unsigned long long)f->n_wasted, (unsigned long long)f->n_pad, (unsigned long long)ius, (unsigned long long)pages,
		 f->fine_blocks, f->coarse_blocks, f->nfree);
	{	/* adapt / cuckoo2 counters (cumulative incl. warmup) */
		u64 used = 0, e; int o = (int)strlen(buf);
		if (f->ct_on) for (e = 0; e < f->ct_nb * 4; e++) used += f->ct_lpn[e] != DM_NONE32;
		if (o > 0 && o < len) buf[--o] = 0;   /* drop the closing brace */
		snprintf(buf + o, len - o, ", \"fine_host_new\": %llu, \"fine_host_ow\": %llu, "
			 "\"adapt\": {\"switches\": %llu, \"uni_host\": %llu, \"quota_base\": %d, \"quota_uni\": %d, \"uni_at_end\": %d, \"fp_ticks\": %llu}, "
			 "\"cuckoo2\": {\"buckets\": %llu, \"entries\": %llu, \"used_end\": %llu, \"evict_folds\": %llu}, \"cgf_spill\": %llu, \"reserve_draw\": %llu, \"pool_evict_folds\": %llu, \"onefront_reuse\": %llu}",
			 (unsigned long long)f->fine_host_new, (unsigned long long)f->fine_host_ow,
			 (unsigned long long)f->n_switch, (unsigned long long)f->uni_host, f->fq_base, f->fq_uni, f->uni, (unsigned long long)f->n_adapt_fp,
			 (unsigned long long)f->ct_nb, (unsigned long long)(f->ct_nb * 4), (unsigned long long)used, (unsigned long long)f->n_evict,
			 (unsigned long long)f->n_spill, (unsigned long long)f->n_reserve_draw, (unsigned long long)f->n_pool_evict, (unsigned long long)f->n_onefront_reuse);
	}
	if (f->spill) {   /* ftlsim "spill" block (cumulative incl. warmup) */
		int o = (int)strlen(buf);
		if (o > 0 && o < len) buf[--o] = 0;
		snprintf(buf + o, len - o, ", \"spill\": {\"mode\": %d, \"ius_per_ftp\": %u, \"ftps\": %llu, \"gtd_bytes\": %llu, \"table_bytes\": %llu, "
			 "\"spills\": %llu, \"spilled_ius\": %llu, \"spilled_pages\": %llu, \"unspills\": %llu, \"unspill_pages\": %llu, "
			 "\"spilled_fold\": %llu, \"spilled_refold\": %llu, \"spilled_gcfold\": %llu, \"spilled_gccopy\": %llu, \"lazy_drop\": %llu, "
			 "\"ftp_reads\": {\"host_read\": %llu, \"unspill\": %llu, \"fold\": %llu, \"gc\": %llu, \"spill_rmw\": %llu}, "
			 "\"ftp_progs\": {\"spill\": %llu, \"gc_update\": %llu}, \"spilled_pages_end\": %llu, \"reads_spilled\": %llu, \"reads_ftp\": %llu}}",
			 f->spill, f->ipf, (unsigned long long)f->n_ftp, (unsigned long long)f->gtd_bytes, (unsigned long long)(f->ct_nb * 4 * f->ct_ent),
			 (unsigned long long)f->sp_n, (unsigned long long)f->sp_ius, (unsigned long long)f->sp_pages, (unsigned long long)f->sp_unspill,
			 (unsigned long long)f->sp_unspill_pages, (unsigned long long)f->sp_fold, (unsigned long long)f->sp_refold, (unsigned long long)f->sp_gcfold,
			 (unsigned long long)f->sp_copy, (unsigned long long)f->sp_lazy_drop,
			 (unsigned long long)f->sp_rd[0], (unsigned long long)f->sp_rd[1], (unsigned long long)f->sp_rd[2], (unsigned long long)f->sp_rd[3],
			 (unsigned long long)f->sp_rd[4], (unsigned long long)f->sp_wr[0], (unsigned long long)f->sp_wr[1], (unsigned long long)f->spages,
			 (unsigned long long)f->rd_spilled, (unsigned long long)f->rd_ftp);
	}
	if (f->spill_rp) {   /* ftlsim spill_rp counters (cumulative incl. warmup) */
		int o = (int)strlen(buf);
		if (o > 0 && o < len) buf[--o] = 0;
		snprintf(buf + o, len - o, ", \"spill_rp\": {\"K\": %d, \"rate\": %llu, \"counter_bits\": %d, \"promotions\": %llu, \"pages\": %llu, "
			 "\"wait\": %llu, \"rate_skips\": %llu, \"ftp_progs\": %llu, \"ftp_reads\": %llu, \"clean\": %d, \"clean_evictions\": %llu, \"clean_dirtied\": %llu}}",
			 f->spill_rp, (unsigned long long)f->spill_rp_rate, f->rp_bits, (unsigned long long)f->rp_n, (unsigned long long)f->rp_pages,
			 (unsigned long long)f->rp_wait, (unsigned long long)f->rp_rate, (unsigned long long)f->rp_wr, (unsigned long long)f->rp_rd,
			 f->rp_clean, (unsigned long long)f->rp_cevict, (unsigned long long)f->rp_cdirty);
	}
}

const struct dm_ftl_ops dm_policy = {
	.name = "proposed", .create = prop_create, .destroy = prop_destroy, .write = prop_write, .read = prop_read,
	.flush = prop_flush, .mem_bytes = prop_mem, .verify = prop_verify, .report = prop_report,
};
