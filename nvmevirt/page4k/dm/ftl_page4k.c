// SPDX-License-Identifier: GPL-2.0-only
/*
 * page4k: page-level mapping, all in DRAM (L x 4 B). Reference core and template for the other policies.
 * Semantics match ftlsim mode=page4k: LIFO free list (block 0 first), separate host / GC frontiers,
 * GC when free blocks <= T (not re-entered while GC runs; the reserve absorbs GC's own allocations),
 * greedy victim = min valid, ties -> lowest block id, frontiers excluded.
 */
#include "ftlcore.h"

struct frontier { s32 blk; u32 next; };
struct p4k {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u32 *pmap;      /* lpn -> ppn */
	u32 *p2l;       /* ppn -> lpn (OOB) */
	s32 *bvalid;
	u8 *bused;
	s32 *freelist; u32 nfree;
	struct frontier hf, gf;
	int in_gc;
};

static void p4k_erase_block(struct p4k *f, u32 b)
{
	if (f->bvalid[b] != 0) dm_die("erase block %u with valid=%d\n", b, f->bvalid[b]);
	f->bused[b] = 0;
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}

static int p4k_is_frontier(struct p4k *f, u32 b) { return (s32)b == f->hf.blk || (s32)b == f->gf.blk; }

static s32 p4k_pick_victim(struct p4k *f)
{
	s32 best = -1, bv = 0x7fffffff; u32 b;
	for (b = 0; b < f->g.NB; b++) {
		if (!f->bused[b] || p4k_is_frontier(f, b)) continue;
		if (f->bvalid[b] < bv) { bv = f->bvalid[b]; best = (s32)b; }
	}
	return best;
}

static void p4k_page_write(struct p4k *f, u64 lpn, int src);

static void p4k_do_gc(struct p4k *f)
{
	s32 b = p4k_pick_victim(f); u32 s, base;
	if (b < 0) dm_die("page4k: no victim\n");
	f->in_gc = 1;
	base = (u32)b * f->g.P;
	for (s = 0; s < f->g.P && f->bvalid[b] > 0; s++) {
		u32 ppn = base + s, lpn = f->p2l[ppn];
		if (lpn == DM_NONE32 || f->pmap[lpn] != ppn) continue;
		f->st->read[DM_R_GC]++; f->fo.read(f->fo.ctx, ppn, DM_R_GC);
		p4k_page_write(f, lpn, DM_P_GC);
	}
	if (f->bvalid[b] != 0) dm_die("gc left valid=%d\n", f->bvalid[b]);
	p4k_erase_block(f, (u32)b);
	f->in_gc = 0;
}

static u32 p4k_alloc_block(struct p4k *f)
{
	s32 b;
	if (!f->in_gc) while (f->nfree <= f->g.T) p4k_do_gc(f);
	if (!f->nfree) dm_die("free list exhausted\n");
	b = f->freelist[--f->nfree];
	f->bused[b] = 1; f->bvalid[b] = 0;
	return (u32)b;
}

static u32 p4k_alloc_page(struct p4k *f, int gc)
{
	struct frontier *fr = gc ? &f->gf : &f->hf;
	if (fr->blk < 0 || fr->next == f->g.P) { fr->blk = (s32)p4k_alloc_block(f); fr->next = 0; }
	return (u32)fr->blk * f->g.P + fr->next++;
}

static void p4k_page_write(struct p4k *f, u64 lpn, int src)
{
	u32 ppn = p4k_alloc_page(f, src != DM_P_HOST), old;
	f->p2l[ppn] = (u32)lpn;
	f->bvalid[ppn / f->g.P]++;
	f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn, src);
	old = f->pmap[lpn];
	if (old != DM_NONE32) f->bvalid[old / f->g.P]--;
	f->pmap[lpn] = ppn;
}

static void *p4k_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct p4k *f = dm_zalloc(sizeof(*f)); u64 i, np = (u64)g->NB * g->P;
	if (!f) return NULL;
	f->g = *g; f->fo = *fo; f->st = st;
	f->pmap = dm_zalloc(sizeof(u32) * g->L); f->p2l = dm_zalloc(sizeof(u32) * np);
	f->bvalid = dm_zalloc(sizeof(s32) * g->NB); f->bused = dm_zalloc(g->NB); f->freelist = dm_zalloc(sizeof(s32) * g->NB);
	if (!f->pmap || !f->p2l || !f->bvalid || !f->bused || !f->freelist) dm_die("page4k: alloc failed\n");
	for (i = 0; i < g->L; i++) f->pmap[i] = DM_NONE32;
	for (i = 0; i < np; i++) f->p2l[i] = DM_NONE32;
	for (i = 0; i < g->NB; i++) f->freelist[i] = (s32)(g->NB - 1 - i);
	f->nfree = g->NB; f->hf.blk = f->gf.blk = -1;
	(void)params;
	return f;
}

static void p4k_destroy(void *c)
{
	struct p4k *f = c;
	dm_free(f->pmap); dm_free(f->p2l); dm_free(f->bvalid); dm_free(f->bused); dm_free(f->freelist); dm_free(f);
}

static void p4k_write(void *c, u64 lpn, u64 n)
{
	struct p4k *f = c; u64 p;
	f->st->host_pages += n;
	for (p = lpn; p < lpn + n; p++) p4k_page_write(f, p, DM_P_HOST);
}

static void p4k_read(void *c, u64 lpn, u64 n)
{
	struct p4k *f = c; u64 p;
	for (p = lpn; p < lpn + n; p++) {
		if (f->pmap[p] != DM_NONE32) { f->st->read[DM_R_HOST]++; f->fo.read(f->fo.ctx, f->pmap[p], DM_R_HOST); }
	}
}

static void p4k_flush(void *c) { (void)c; }
static u64 p4k_mem(void *c) { struct p4k *f = c; return f->g.L * 4; }

static int p4k_verify(void *c)
{
	struct p4k *f = c; u64 l; u32 b; int bad = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->g.NB);
	for (l = 0; l < f->g.L; l++) if (f->pmap[l] != DM_NONE32) v[f->pmap[l] / f->g.P]++;
	for (b = 0; b < f->g.NB; b++) if (v[b] != f->bvalid[b]) { bad = 1; break; }
	dm_free(v);
	return bad;
}

static void p4k_report(void *c, char *buf, int len) { (void)c; if (len) buf[0] = 0; }

const struct dm_ftl_ops dm_policy = {
	.name = "page4k", .create = p4k_create, .destroy = p4k_destroy, .write = p4k_write, .read = p4k_read,
	.flush = p4k_flush, .mem_bytes = p4k_mem, .verify = p4k_verify, .report = p4k_report,
};
