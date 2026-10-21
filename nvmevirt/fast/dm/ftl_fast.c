// SPDX-License-Identifier: GPL-2.0-only
/*
 * fast: FAST log-buffer hybrid FTL (Lee et al., TECS 2007). Port of ftlsim `mode=fast` (struct HybridSim).
 *
 *  - data blocks: block-level map dmap[LB] -> physical block (LB = P consecutive logical pages)
 *  - log blocks: page-level map lmap[lpn] -> ppn in a log block (only while the newest copy is in a log)
 *  - one sequential log block SW (page offset 0 of an LB starts a new SW; appended while offsets are
 *    sequential) + nlog-1 fully-associative random log blocks RW in FIFO order (front = oldest)
 *  - RW victim = oldest; reclaimed by a full merge of every LB with valid pages in it (dead -> erase only)
 *  - SW reclaim: dead -> erase; all P valid in order -> switch merge; valid == next -> partial merge
 *    (copy the remaining offsets, then switch); otherwise full merge of every associated LB
 *  - pool: NB = NLB data + nlog log + T reserve (merge targets); no separate GC.
 *
 * Geometry as HybridSim: Lc = L - L % P logical pages are mapped (NLB = Lc / P); host pages at or beyond
 * Lc do not exist in ftlsim's device and are ignored here (see NOTES_fast.md). NB = Lc*(1+op)/P as ftlsim (param op_ppm, default 92780;
 * the extra block of g->NB, if any, stays unused), nlog = NB - NLB - T.
 *
 * Source mapping: S_HOST -> DM_P_HOST; S_FULLMERGE / S_PARTIALMERGE -> DM_P_GC (every merge program,
 * including offsets that were never written, which ftlsim counts as full/partial-merge programs);
 * merge copies of existing pages -> DM_R_GC (= ftlsim read_gc_copy); host reads -> DM_R_HOST.
 */
#include "ftlcore.h"

enum { FAST_FREE = 0, FAST_DATA = 1, FAST_LOG = 2 };
enum { FAST_K_NONE = 0, FAST_K_SW = 1, FAST_K_RW = 2 };

struct fast_lblk { u8 kind; u32 next; u32 lb; };

struct fast {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u64 Lc; u32 P, NB, NLB, T; s32 nlog;
	u32 rwcap;              /* max random log blocks (nlog-1, or fewer under a cache_bytes DRAM budget) */
	u32 *dmap;              /* LB -> data block */
	u32 *lmap;              /* lpn -> ppn in a log block, or NONE */
	u32 *p2l;               /* ppn -> lpn */
	u8 *btag;
	s32 *bvalid;
	struct fast_lblk *lg;
	s32 *freelist; u32 nfree;
	s32 sw;                 /* sequential log block or -1 */
	s32 *rw; u32 rw_head, rw_n;   /* FIFO ring of random log blocks (capacity nlog) */
	u64 logpages;           /* live lmap entries (for the RAM model) */
	/* events (ftlsim report "events") */
	u64 n_switch, n_partial, n_full, n_dead, n_merge_reclaim, assoc_sum;
};

static s32 fast_pop_free(struct fast *f, u8 tag)
{
	s32 b;
	if (!f->nfree) dm_die("fast: free list exhausted\n");
	b = f->freelist[--f->nfree];
	f->btag[b] = tag; f->bvalid[b] = 0;
	f->lg[b].kind = FAST_K_NONE; f->lg[b].next = 0; f->lg[b].lb = DM_NONE32;
	return b;
}

static void fast_erase(struct fast *f, u32 b)
{
	if (f->bvalid[b] != 0) dm_die("fast: erase block %u valid=%d tag=%u\n", b, f->bvalid[b], f->btag[b]);
	f->btag[b] = FAST_FREE;
	f->lg[b].kind = FAST_K_NONE; f->lg[b].next = 0; f->lg[b].lb = DM_NONE32;
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}

static void fast_drop_log_copy(struct fast *f, u64 lpn)
{
	u32 old = f->lmap[lpn];
	if (old == DM_NONE32) return;
	f->bvalid[old / f->P]--;
	f->lmap[lpn] = DM_NONE32;
	f->logpages--;
}

static void fast_invalidate(struct fast *f, u64 lpn)
{
	if (f->lmap[lpn] != DM_NONE32) fast_drop_log_copy(f, lpn);
	else { u32 db = f->dmap[lpn / f->P]; if (db != DM_NONE32) f->bvalid[db]--; }
}

static u32 fast_latest(struct fast *f, u64 lpn)
{
	u32 db;
	if (f->lmap[lpn] != DM_NONE32) return f->lmap[lpn];
	db = f->dmap[lpn / f->P];
	return db == DM_NONE32 ? DM_NONE32 : db * f->P + (u32)(lpn % f->P);
}

static void fast_prog(struct fast *f, u32 ppn, int src) { f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn, src); }
static void fast_rd(struct fast *f, u32 ppn, int src) { f->st->read[src]++; f->fo.read(f->fo.ctx, ppn, src); }

/* preemption point every FAST_RESCHED pages of a merge / scan loop (kernel: cond_resched, see dm_port.h) */
#define FAST_RESCHED 4096u

/* Copy offsets [from, P) of LB lb to block dst (same offsets), newest copy first: log copy if any (dropped from
 * lmap), else the data block's page (db, if mapped). Per offset: one DM_R_GC read if a copy exists, then one
 * DM_P_GC program, in offset order (= ftlsim full_merge / partial_merge). Hot loop of every merge: no u64
 * division, flash-op counters added once per call (the callbacks never look at dm_stats). */
static void fast_copy_lb(struct fast *f, u32 lb, u32 from, u32 dst, u32 db)
{
	u32 P = f->P, off, nrd = 0, nlog = 0, lbase = lb * P, dbase = db * P, nbase = dst * P;
	u32 *lm = f->lmap + lbase, *p2l = f->p2l + nbase;
	void *ctx = f->fo.ctx;
	for (off = from; off < P; off++) {
		u32 l = lm[off];
		if (l != DM_NONE32) {                     /* newest copy is in a log block: read it, drop it */
			f->fo.read(ctx, l, DM_R_GC); nrd++;
			f->bvalid[l / P]--; lm[off] = DM_NONE32; nlog++;
		} else if (db != DM_NONE32) {
			f->fo.read(ctx, dbase + off, DM_R_GC); nrd++;
		}
		f->fo.prog(ctx, nbase + off, DM_P_GC);
		p2l[off] = lbase + off;
		if (!((off + 1) % FAST_RESCHED)) dm_cond_resched();
	}
	f->st->read[DM_R_GC] += nrd; f->st->prog[DM_P_GC] += P - from; f->logpages -= nlog;
}

/* full merge of LB lb: a fresh block receives the newest copy of every offset (P programs) */
static void fast_full_merge(struct fast *f, u32 lb)
{
	u32 nb, old, P = f->P;
	f->n_full++;
	nb = (u32)fast_pop_free(f, FAST_DATA);
	fast_copy_lb(f, lb, 0, nb, f->dmap[lb]);
	f->bvalid[nb] = (s32)P;
	old = f->dmap[lb];
	if (old != DM_NONE32) { f->bvalid[old] = 0; fast_erase(f, old); }
	f->dmap[lb] = nb;
}

/* switch merge: log block b holds all P pages of its LB in order and becomes the data block */
static void fast_switch_merge(struct fast *f, u32 b)
{
	u32 lb = f->lg[b].lb, off, old, P = f->P;
	f->n_switch++;
	for (off = 0; off < P; off++) {
		u64 lpn = (u64)lb * P + off;
		if (f->lmap[lpn] != b * P + off) dm_die("fast: switch merge: page %u not in log block\n", off);
		f->lmap[lpn] = DM_NONE32; f->logpages--;
		if (!((off + 1) % FAST_RESCHED)) dm_cond_resched();
	}
	old = f->dmap[lb];
	if (old != DM_NONE32) { f->bvalid[old] = 0; fast_erase(f, old); }
	f->dmap[lb] = b; f->btag[b] = FAST_DATA; f->bvalid[b] = (s32)P;
	f->lg[b].kind = FAST_K_NONE; f->lg[b].next = 0; f->lg[b].lb = DM_NONE32;
}

/* partial merge: in-order log block with next < P: copy the remaining offsets (in order), then switch */
static void fast_partial_merge(struct fast *f, u32 b)
{
	u32 lb = f->lg[b].lb, off, P = f->P, from = f->lg[b].next;
	f->n_partial++;
	/* the copied offsets land in b itself (offsets from..P-1, in order) and become its log pages; b's own
	 * pages are all < from, so the copy never reads b */
	fast_copy_lb(f, lb, from, b, f->dmap[lb]);
	for (off = from; off < P; off++) f->lmap[(u64)lb * P + off] = b * P + off;
	f->bvalid[b] += (s32)(P - from); f->logpages += P - from;
	f->lg[b].next = P;
	fast_switch_merge(f, b);
}

/* reclaim a log block by full-merging every LB that still has valid pages in it, then erase it.
 * ftlsim walks its per-block assoc hash; the merge order does not change any count (each merge is
 * independent and always P programs into a fresh block), here the LBs are found by an offset scan. */
static void fast_merge_reclaim(struct fast *f, u32 b)
{
	u32 off, base = b * f->P, next = f->lg[b].next;
	f->n_merge_reclaim++;
	for (off = 0; off < next && f->bvalid[b] > 0; off++) {
		u32 ppn = base + off, lpn = f->p2l[ppn];
		if (off + 16 < next && f->p2l[ppn + 16] != DM_NONE32) dm_prefetch(&f->lmap[f->p2l[ppn + 16]]);
		if (!((off + 1) % FAST_RESCHED)) dm_cond_resched();
		if (lpn == DM_NONE32 || f->lmap[lpn] != ppn) continue;
		f->assoc_sum++;
		fast_full_merge(f, lpn / f->P);
	}
	if (f->bvalid[b] != 0) dm_die("fast: merge_reclaim left valid=%d\n", f->bvalid[b]);
	fast_erase(f, b);
}

static void fast_seq_reclaim(struct fast *f, u32 b)
{
	if (f->bvalid[b] == 0) { f->n_dead++; fast_erase(f, b); return; }
	/* the FAST SW is always written in order (ftlsim lg.inorder stays true) */
	if (f->lg[b].next == f->P && f->bvalid[b] == (s32)f->P) { fast_switch_merge(f, b); return; }
	if (f->bvalid[b] == (s32)f->lg[b].next) { fast_partial_merge(f, b); return; }
	fast_merge_reclaim(f, b);
}

static void fast_log_append(struct fast *f, u32 b, u64 lpn)
{
	u32 ppn = b * f->P + f->lg[b].next++;
	fast_invalidate(f, lpn);
	f->p2l[ppn] = (u32)lpn; f->lmap[lpn] = ppn; f->bvalid[b]++; f->logpages++;
	fast_prog(f, ppn, DM_P_HOST);
}

static void fast_write_page(struct fast *f, u64 lpn)
{
	u32 lb = (u32)(lpn / f->P), off = (u32)(lpn % f->P), rwcap = f->rwcap;
	if (off == 0) {
		if (f->sw >= 0) { fast_seq_reclaim(f, (u32)f->sw); f->sw = -1; }
		f->sw = fast_pop_free(f, FAST_LOG); f->lg[f->sw].kind = FAST_K_SW; f->lg[f->sw].lb = lb;
	}
	if (f->sw >= 0 && f->lg[f->sw].lb == lb) {
		if (f->lg[f->sw].next == off) {          /* sequential continuation */
			fast_log_append(f, (u32)f->sw, lpn);
			if (f->lg[f->sw].next == f->P) { fast_seq_reclaim(f, (u32)f->sw); f->sw = -1; }
			return;
		}
		fast_seq_reclaim(f, (u32)f->sw); f->sw = -1;   /* same LB out of order -> merge SW now */
	}
	/* random log */
	if (!f->rw_n || f->lg[f->rw[(f->rw_head + f->rw_n - 1) % f->nlog]].next == f->P) {
		s32 b;
		if (f->rw_n >= rwcap) {
			u32 v = (u32)f->rw[f->rw_head];
			f->rw_head = (f->rw_head + 1) % f->nlog; f->rw_n--;
			if (f->bvalid[v] == 0) { f->n_dead++; fast_erase(f, v); } else fast_merge_reclaim(f, v);
		}
		b = fast_pop_free(f, FAST_LOG); f->lg[b].kind = FAST_K_RW;
		f->rw[(f->rw_head + f->rw_n) % f->nlog] = b; f->rw_n++;
	}
	fast_log_append(f, (u32)f->rw[(f->rw_head + f->rw_n - 1) % f->nlog], lpn);
}

static void *fast_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct fast *f = dm_zalloc(sizeof(*f)); u64 i, np;
	if (!f) return NULL;
	f->g = *g; f->fo = *fo; f->st = st;
	f->P = g->P; f->NB = g->NB; f->T = g->T;
	f->Lc = g->L - g->L % g->P; f->NLB = (u32)(f->Lc / g->P);
	if (f->Lc != g->L) {
		/* ftlsim derives NB from its truncated L: NB = (Lc * (1 + op)) / P. The glue/harness derived g->NB
		 * from the untruncated L, which can be one block more; use ftlsim's count, leave the rest unused. */
		u64 op_ppm = 92780, nb;
		dm_param_u64(params, "op_ppm", &op_ppm);
		nb = f->Lc * (1000000 + op_ppm) / 1000000 / g->P;
		if (nb < f->NB) f->NB = (u32)nb;
	}
	f->nlog = (s32)f->NB - (s32)f->NLB - (s32)g->T;
	if (f->nlog < 3) dm_die("fast: not enough log blocks (NB=%u NLB=%u T=%u)\n", f->NB, f->NLB, g->T);
	f->rwcap = (u32)(f->nlog - 1);
	{	/* cache_bytes: mapping-DRAM budget. The RAM model (fast_mem) is NLB*4 + live log pages * 8/0.7, and the SW
		 * block plus rwcap RW blocks can hold (rwcap+1)*P live log pages, so cap rwcap to keep the peak within the
		 * budget (the spare blocks beyond it stay unused as log space). Default: all spare blocks are log blocks. */
		u64 cb = 0, per = ((u64)g->P * 80 + 6) / 7, nl;
		if (dm_param_u64(params, "cache_bytes", &cb) && cb > (u64)f->NLB * 4) {
			nl = (cb - (u64)f->NLB * 4) / per;          /* log blocks (SW + RW) that fit */
			if (nl < 2) nl = 2;
			if (nl - 1 < f->rwcap) f->rwcap = (u32)(nl - 1);
		}
	}
	np = (u64)g->NB * g->P;
	if (np > 0xFFFFFFFEull) dm_die("fast: NB*P does not fit in u32\n");
	f->dmap = dm_zalloc(sizeof(u32) * f->NLB); f->lmap = dm_zalloc(sizeof(u32) * f->Lc);
	f->p2l = dm_zalloc(sizeof(u32) * np); f->btag = dm_zalloc(g->NB); f->bvalid = dm_zalloc(sizeof(s32) * g->NB);
	f->lg = dm_zalloc(sizeof(struct fast_lblk) * g->NB); f->freelist = dm_zalloc(sizeof(s32) * g->NB);
	f->rw = dm_zalloc(sizeof(s32) * f->nlog);
	if (!f->dmap || !f->lmap || !f->p2l || !f->btag || !f->bvalid || !f->lg || !f->freelist || !f->rw)
		dm_die("fast: alloc failed\n");
	for (i = 0; i < f->NLB; i++) f->dmap[i] = DM_NONE32;
	for (i = 0; i < f->Lc; i++) f->lmap[i] = DM_NONE32;
	for (i = 0; i < np; i++) { f->p2l[i] = DM_NONE32; if (!(i & 0xFFFFFF)) dm_cond_resched(); }
	for (i = 0; i < g->NB; i++) f->lg[i].lb = DM_NONE32;
	for (i = 0; i < f->NB; i++) f->freelist[i] = (s32)(f->NB - 1 - i);   /* LIFO, block 0 first */
	f->nfree = f->NB; f->sw = -1;
	dm_log("fast: Lc=%llu NB=%u (of %u) NLB=%u nlog=%d rwcap=%u P=%u T=%u (ignored tail pages: %llu)\n", (unsigned long long)f->Lc,
	       f->NB, g->NB, f->NLB, f->nlog, f->rwcap, g->P, g->T, (unsigned long long)(g->L - f->Lc));
	return f;
}

static void fast_destroy(void *c)
{
	struct fast *f = c;
	dm_free(f->dmap); dm_free(f->lmap); dm_free(f->p2l); dm_free(f->btag); dm_free(f->bvalid);
	dm_free(f->lg); dm_free(f->freelist); dm_free(f->rw); dm_free(f);
}

static void fast_write(void *c, u64 lpn, u64 n)
{
	struct fast *f = c; u64 p;
	f->st->host_pages += n;
	for (p = lpn; p < lpn + n; p++) if (p < f->Lc) fast_write_page(f, p);
}

static void fast_read(void *c, u64 lpn, u64 n)
{
	struct fast *f = c; u64 p;
	for (p = lpn; p < lpn + n && p < f->Lc; p++) {
		u32 ppn = fast_latest(f, p);
		if (ppn != DM_NONE32) fast_rd(f, ppn, DM_R_HOST);
	}
}

static void fast_flush(void *c) { (void)c; }

/* ftlsim HybridSim::report (fast): block map NLB*4 + hash_bytes(log_pages, 8) = log_pages*8/0.7, "%.0f" */
static u64 fast_mem(void *c)
{
	struct fast *f = c;
	return (u64)f->NLB * 4 + (f->logpages * 80 + 3) / 7;
}

static int fast_verify(void *c)
{
	struct fast *f = c; u64 l, lp = 0; u32 b; int bad = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->NB);
	if (!v) return 1;
	for (l = 0; l < f->Lc; l++) {
		u32 p = fast_latest(f, l);
		if (!(l & 0xFFFFF)) dm_cond_resched();
		if (p != DM_NONE32) v[p / f->P]++;
		if (f->lmap[l] != DM_NONE32) { lp++; if (f->p2l[f->lmap[l]] != (u32)l || f->btag[f->lmap[l] / f->P] != FAST_LOG) bad = 1; }
	}
	for (b = 0; b < f->NB; b++) if (v[b] != f->bvalid[b]) { bad = 1; break; }
	if (lp != f->logpages) bad = 1;
	dm_free(v);
	return bad;
}

static void fast_report(void *c, char *buf, int len)
{
	struct fast *f = c;
	snprintf(buf, len, "\"fast\": {\"NLB\": %u, \"nlog\": %d, \"switch\": %llu, \"partial\": %llu, \"full\": %llu, "
		 "\"dead_reclaim\": %llu, \"merge_reclaim\": %llu, \"assoc_sum\": %llu, \"log_pages_end\": %llu}",
		 f->NLB, f->nlog, (unsigned long long)f->n_switch, (unsigned long long)f->n_partial, (unsigned long long)f->n_full,
		 (unsigned long long)f->n_dead, (unsigned long long)f->n_merge_reclaim, (unsigned long long)f->assoc_sum,
		 (unsigned long long)f->logpages);
}

static u64 fast_logical_pages(void *c) { return ((struct fast *)c)->Lc; }

const struct dm_ftl_ops dm_policy = {
	.logical_pages = fast_logical_pages,
	.name = "fast", .create = fast_create, .destroy = fast_destroy, .write = fast_write, .read = fast_read,
	.flush = fast_flush, .mem_bytes = fast_mem, .verify = fast_verify, .report = fast_report,
};
