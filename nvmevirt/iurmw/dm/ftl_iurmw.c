// SPDX-License-Identifier: GPL-2.0-only
/*
 * iurmw: coarse IU mapping with read-modify-write (ftlsim mode=iu_rmw).
 * One map entry per IU (U pages, NIU x 4 B): coarse_map[iu] = ppn of a U-page extent that is
 * contiguous and U-aligned inside one block. A partial-IU host write reads the U-w missing pages
 * of the old extent (DM_R_RMW) and rewrites the whole IU: w pages DM_P_HOST + U-w pages DM_P_RMW.
 * Port of ftlsim Sim: host_write (iu_rmw branch), coarse_write / install_extent / invalidate_iu,
 * coarse_alloc_extent (coarse_class=0, gc_frontier=1 -> separate host / GC coarse frontiers),
 * coarse_gc with greedy pick_victim(B_COARSE), alloc_block's GC loop (iu_rmw-reachable part:
 * "useless victim" check + stall backstop), erase_block, verify(); RAM model NIU*4.
 */
#include "ftlcore.h"

struct iurmw_frontier { s32 blk; u32 next; };
struct iurmw {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u64 NIU;
	u32 *cmap;      /* iu -> extent start ppn (DM_NONE32 = unmapped) */
	u32 *p2l;       /* ppn -> lpn (OOB); not cleared on erase, as in ftlsim */
	s32 *bvalid;
	u8 *bused;      /* ftlsim btag: 0 = B_FREE, 1 = B_COARSE */
	s32 *freelist; u32 nfree;
	struct iurmw_frontier cf, cgf;   /* host / GC coarse frontiers */
	int in_cgc;
	u64 n_cgc, cgc_valid_sum, cgc_dead_ext, cgc_clean_ext;
};

static void iurmw_erase_block(struct iurmw *f, u32 b)
{
	if (f->bvalid[b] != 0) dm_die("iurmw: erase block %u with valid=%d\n", b, f->bvalid[b]);
	if (!f->bused[b]) dm_die("iurmw: erase free block %u\n", b);
	f->bused[b] = 0;
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}

static int iurmw_is_frontier(struct iurmw *f, u32 b) { return (s32)b == f->cf.blk || (s32)b == f->cgf.blk; }

/* greedy: min valid, ties -> lowest block id, frontiers excluded */
static s32 iurmw_pick_victim(struct iurmw *f)
{
	s32 best = -1, bv = 0x7fffffff; u32 b;
	for (b = 0; b < f->g.NB; b++) {
		if (!f->bused[b] || iurmw_is_frontier(f, b)) continue;
		if (f->bvalid[b] < bv) { bv = f->bvalid[b]; best = (s32)b; }
	}
	return best;
}

static void iurmw_coarse_write(struct iurmw *f, u64 iu, int gc, u64 s_off, u64 e_off);

static void iurmw_coarse_gc(struct iurmw *f)
{
	s32 b; u32 base, e, k, U = f->g.U;
	f->in_cgc = 1;
	b = iurmw_pick_victim(f);
	if (b < 0) dm_die("iurmw: no coarse victim (free=%u)\n", f->nfree);
	f->n_cgc++; f->cgc_valid_sum += (u64)f->bvalid[b];
	base = (u32)b * f->g.P;
	for (e = 0; e < f->g.P / U && f->bvalid[b] > 0; e++) {
		u32 ppn = base + e * U, lpn = f->p2l[ppn]; u64 iu;
		if (lpn == DM_NONE32) continue;
		iu = lpn / U;
		if (f->cmap[iu] != ppn) { f->cgc_dead_ext++; continue; }
		f->cgc_clean_ext++;
		for (k = 0; k < U; k++) { f->st->read[DM_R_GC]++; f->fo.read(f->fo.ctx, ppn + k, DM_R_GC); }
		iurmw_coarse_write(f, iu, 1, 0, U);
	}
	if (f->bvalid[b] != 0) dm_die("iurmw: coarse_gc left valid=%d\n", f->bvalid[b]);
	iurmw_erase_block(f, (u32)b);
	f->in_cgc = 0;
}

static u32 iurmw_alloc_block(struct iurmw *f)
{
	s32 b;
	if (!f->in_cgc) {
		int stall = 0;
		while (f->nfree <= f->g.T) {
			u32 before = f->nfree; s32 cv = iurmw_pick_victim(f);
			int useless = cv >= 0 && f->bvalid[cv] >= (s32)f->g.P;   /* fully valid best victim */
			if (!useless) iurmw_coarse_gc(f);
			if (useless || f->nfree <= before) {
				if (++stall > 64) dm_die("iurmw: GC makes no progress (free=%u NB=%u)\n", f->nfree, f->g.NB);
			} else stall = 0;
		}
	}
	if (!f->nfree) dm_die("iurmw: free list exhausted\n");
	b = f->freelist[--f->nfree];
	f->bused[b] = 1; f->bvalid[b] = 0;
	return (u32)b;
}

static u32 iurmw_alloc_extent(struct iurmw *f, int gc)
{
	struct iurmw_frontier *fr = gc ? &f->cgf : &f->cf; u32 ppn;
	if (fr->blk >= 0 && f->g.P - fr->next < f->g.U) fr->next = f->g.P;   /* unreachable: P % U == 0 */
	if (fr->blk < 0 || fr->next == f->g.P) { fr->blk = (s32)iurmw_alloc_block(f); fr->next = 0; }
	ppn = (u32)fr->blk * f->g.P + fr->next; fr->next += f->g.U;
	return ppn;
}

/* write the whole IU at a new extent. Offsets [s_off, e_off) are new data (DM_P_HOST, or DM_P_GC
 * for a GC copy), the rest is RMW filler (DM_P_RMW). Then invalidate the old extent (install_extent). */
static void iurmw_coarse_write(struct iurmw *f, u64 iu, int gc, u64 s_off, u64 e_off)
{
	u32 U = f->g.U, ppn = iurmw_alloc_extent(f, gc), old, k;
	for (k = 0; k < U; k++) {
		int src = gc ? DM_P_GC : ((k >= s_off && k < e_off) ? DM_P_HOST : DM_P_RMW);
		f->p2l[ppn + k] = (u32)(iu * U + k);
		f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn + k, src);
	}
	f->bvalid[ppn / f->g.P] += (s32)U;
	old = f->cmap[iu];
	if (old != DM_NONE32) f->bvalid[old / f->g.P] -= (s32)U;
	f->cmap[iu] = ppn;
}

static void *iurmw_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct iurmw *f = dm_zalloc(sizeof(*f)); u64 i, np = (u64)g->NB * g->P;
	if (!f) return NULL;
	if (g->P % g->U) dm_die("iurmw: P must be a multiple of U\n");
	if (np > 0xFFFFFFFEull) dm_die("iurmw: too many physical pages for u32 ppn\n");
	f->g = *g; f->fo = *fo; f->st = st; f->NIU = g->L / g->U;
	f->cmap = dm_zalloc(sizeof(u32) * f->NIU); f->p2l = dm_zalloc(sizeof(u32) * np);
	f->bvalid = dm_zalloc(sizeof(s32) * g->NB); f->bused = dm_zalloc(g->NB); f->freelist = dm_zalloc(sizeof(s32) * g->NB);
	if (!f->cmap || !f->p2l || !f->bvalid || !f->bused || !f->freelist) dm_die("iurmw: alloc failed\n");
	for (i = 0; i < f->NIU; i++) f->cmap[i] = DM_NONE32;
	for (i = 0; i < np; i++) f->p2l[i] = DM_NONE32;
	for (i = 0; i < g->NB; i++) f->freelist[i] = (s32)(g->NB - 1 - i);
	f->nfree = g->NB; f->cf.blk = f->cgf.blk = -1;
	(void)params;
	return f;
}

static void iurmw_destroy(void *c)
{
	struct iurmw *f = c;
	dm_free(f->cmap); dm_free(f->p2l); dm_free(f->bvalid); dm_free(f->bused); dm_free(f->freelist); dm_free(f);
}

static void iurmw_write(void *c, u64 lpn, u64 n)
{
	struct iurmw *f = c; u64 cur = lpn, end = lpn + n, U = f->g.U;
	f->st->host_pages += n;
	while (cur < end) {
		u64 iu = cur / U, s = cur, e = end < (iu + 1) * U ? end : (iu + 1) * U, w = e - s;
		if (w < U) {
			/* ftlsim counts U-w RMW reads unconditionally; flash reads only exist for a mapped IU */
			u32 old = f->cmap[iu]; u64 k;
			f->st->read[DM_R_RMW] += U - w;
			if (old != DM_NONE32)
				for (k = 0; k < U; k++)
					if (k < s - iu * U || k >= e - iu * U) f->fo.read(f->fo.ctx, old + k, DM_R_RMW);
		}
		iurmw_coarse_write(f, iu, 0, s - iu * U, e - iu * U);
		cur = e;
	}
}

static void iurmw_read(void *c, u64 lpn, u64 n)
{
	struct iurmw *f = c; u64 p, U = f->g.U;
	for (p = lpn; p < lpn + n; p++) {
		u32 base = f->cmap[p / U];
		if (base != DM_NONE32) { f->st->read[DM_R_HOST]++; f->fo.read(f->fo.ctx, base + p % U, DM_R_HOST); }
	}
}

static void iurmw_flush(void *c) { (void)c; }
static u64 iurmw_mem(void *c) { struct iurmw *f = c; return f->NIU * 4; }

static int iurmw_verify(void *c)
{
	struct iurmw *f = c; u64 iu; u32 b; int bad = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->g.NB);
	if (!v) return 1;
	for (iu = 0; iu < f->NIU && !bad; iu++) {
		u32 ppn = f->cmap[iu], k;
		if (ppn == DM_NONE32) continue;
		if (ppn % f->g.U) { bad = 1; break; }                 /* extents are U-aligned in their block */
		for (k = 0; k < f->g.U; k++) if (f->p2l[ppn + k] != (u32)(iu * f->g.U + k)) { bad = 1; break; }
		v[ppn / f->g.P] += (s32)f->g.U;
	}
	for (b = 0; b < f->g.NB && !bad; b++) if (v[b] != f->bvalid[b] || (!f->bused[b] && v[b])) bad = 1;
	dm_free(v);
	return bad;
}

static void iurmw_report(void *c, char *buf, int len)
{
	struct iurmw *f = c;
	if (len <= 0) return;
	snprintf(buf, len, "\"cgc\": %llu, \"cgc_avg_victim_valid\": %llu, \"cgc_dead_ext\": %llu, \"cgc_clean_ext\": %llu, \"free_end\": %u",
		 (unsigned long long)f->n_cgc, (unsigned long long)(f->n_cgc ? f->cgc_valid_sum / f->n_cgc : 0),
		 (unsigned long long)f->cgc_dead_ext, (unsigned long long)f->cgc_clean_ext, f->nfree);
}

const struct dm_ftl_ops dm_policy = {
	.name = "iurmw", .create = iurmw_create, .destroy = iurmw_destroy, .write = iurmw_write, .read = iurmw_read,
	.flush = iurmw_flush, .mem_bytes = iurmw_mem, .verify = iurmw_verify, .report = iurmw_report,
};
