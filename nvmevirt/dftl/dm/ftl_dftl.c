// SPDX-License-Identifier: GPL-2.0-only
/*
 * dftl: DFTL (Gupta et al., ASPLOS'09) cached page mapping. Port of ftlsim mode=dftl.
 *
 * Data path is identical to page4k (ftlsim is_pg(): page_write / page_gc / pick_victim / alloc_block /
 * fine_alloc_page): LIFO free list (block 0 first), separate host / GC frontiers, GC when free blocks <= T
 * (not re-entered while GC runs), greedy victim = min valid, ties -> lowest block id, frontiers excluded.
 * The full lpn->ppn table (pmap) stands for the on-flash translation pages; only its I/O cost is modeled:
 *
 *   CMT (cached mapping table): LRU of at most cmt_cap entries, lpn -> (lru pos, dirty).
 *     cmt_cap = (u64)(cache_bytes * 0.7 / 8.0) evaluated exactly as IEEE double (integer emulation below).
 *   host write  (ftlsim cmt_update):   hit -> MRU, dirty. miss -> map_miss++, translation-page read
 *     (skipped if dftl_tpbuf and the page is the one in SRAM), evict until size < cap, insert dirty at MRU.
 *   eviction    (ftlsim cmt_evict_one): clean victim -> dropped. dirty victim -> read-modify-write of its
 *     translation page (read skipped on tpbuf hit), and every cached entry of that page becomes clean.
 *   GC relocate (ftlsim cmt_gc_update / cmt_gc_flush): cached -> dirty in place; uncached -> its translation
 *     page joins the GC set; after the victim is emptied (before erase) each distinct page = 1 read + 1 write.
 *   host read   (not in ftlsim; see NOTES_dftl.md): hit -> MRU (dirty bit unchanged). miss -> map_miss++,
 *     translation-page read (unless tpbuf hit), evict until size < cap, insert CLEAN at MRU; then data read.
 *
 * Translation-page I/O: fo.read/prog(ppn = DM_NOPPN, DM_R_MAP / DM_P_MAP), counted in read[DM_R_MAP],
 * prog[DM_P_MAP] exactly where ftlsim counts read_map / prog[S_MAP].
 */
#include "ftlcore.h"

#define DFTL_TP_ENTRIES 1024ull

struct dftl_frontier { s32 blk; u32 next; };

/* CMT node: doubly linked LRU (head = MRU) + singly linked hash chain; indices into a preallocated pool */
struct dftl_node { u32 lpn; u32 prev, next; u32 hnext; u8 dirty; };

struct dftl {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u32 *pmap;      /* lpn -> ppn */
	u32 *p2l;       /* ppn -> lpn (OOB) */
	s32 *bvalid;
	u8 *bused;
	s32 *freelist; u32 nfree;
	struct dftl_frontier hf, gf;
	int in_gc;
	/* CMT */
	u64 cache_bytes, cmt_cap, cmt_size;
	struct dftl_node *nodes; u32 nnodes; u32 free_node;
	u32 *buckets; u32 hmask;
	u32 lru_head, lru_tail;
	int tpbuf; u64 tp_buf;
	/* GC translation-page set (ftlsim gc_tps): membership bitmap + list for clearing */
	u64 ntp; u8 *gc_tp_mark; u32 *gc_tp_list; u32 gc_tp_n;
};

/* ---------------------------------------------------------------- CMT (LRU + hash) */
static inline u32 dftl_hash(struct dftl *f, u32 lpn) { return (u32)(((u64)lpn * 0x9E3779B97F4A7C15ull) >> 32) & f->hmask; }

static u32 dftl_find(struct dftl *f, u32 lpn)
{
	u32 i = f->buckets[dftl_hash(f, lpn)];
	while (i != DM_NONE32 && f->nodes[i].lpn != lpn) i = f->nodes[i].hnext;
	return i;
}

static void dftl_lru_unlink(struct dftl *f, u32 i)
{
	struct dftl_node *n = &f->nodes[i];
	if (n->prev != DM_NONE32) f->nodes[n->prev].next = n->next; else f->lru_head = n->next;
	if (n->next != DM_NONE32) f->nodes[n->next].prev = n->prev; else f->lru_tail = n->prev;
}

static void dftl_lru_push_front(struct dftl *f, u32 i)
{
	struct dftl_node *n = &f->nodes[i];
	n->prev = DM_NONE32; n->next = f->lru_head;
	if (f->lru_head != DM_NONE32) f->nodes[f->lru_head].prev = i; else f->lru_tail = i;
	f->lru_head = i;
}

static void dftl_insert(struct dftl *f, u32 lpn, int dirty)
{
	u32 i = f->free_node, h;
	if (i == DM_NONE32) dm_die("dftl: CMT node pool exhausted\n");
	f->free_node = f->nodes[i].hnext;
	f->nodes[i].lpn = lpn; f->nodes[i].dirty = (u8)dirty;
	h = dftl_hash(f, lpn);
	f->nodes[i].hnext = f->buckets[h]; f->buckets[h] = i;
	dftl_lru_push_front(f, i);
	f->cmt_size++;
}

static void dftl_remove(struct dftl *f, u32 i)
{
	u32 h = dftl_hash(f, f->nodes[i].lpn), *pp = &f->buckets[h];
	while (*pp != i) pp = &f->nodes[*pp].hnext;
	*pp = f->nodes[i].hnext;
	dftl_lru_unlink(f, i);
	f->nodes[i].hnext = f->free_node; f->free_node = i;
	f->cmt_size--;
}

static void dftl_map_read(struct dftl *f) { f->st->read[DM_R_MAP]++; f->fo.read(f->fo.ctx, DM_NOPPN, DM_R_MAP); }
static void dftl_map_prog(struct dftl *f) { f->st->prog[DM_P_MAP]++; f->fo.prog(f->fo.ctx, DM_NOPPN, DM_P_MAP); }

/* ftlsim cmt_evict_one */
static void dftl_evict_one(struct dftl *f)
{
	u32 v = f->lru_tail, vl; u64 tp, l, end; int dirty;
	if (v == DM_NONE32) dm_die("dftl: evict from empty CMT (cmt_cap=%llu)\n", (unsigned long long)f->cmt_cap);
	vl = f->nodes[v].lpn; dirty = f->nodes[v].dirty;
	dftl_remove(f, v);
	if (!dirty) return;
	tp = vl / DFTL_TP_ENTRIES;
	if (!f->tpbuf || tp != f->tp_buf) dftl_map_read(f);
	if (f->tpbuf) f->tp_buf = tp;
	dftl_map_prog(f);
	end = (tp + 1) * DFTL_TP_ENTRIES; if (end > f->g.L) end = f->g.L;
	for (l = tp * DFTL_TP_ENTRIES; l < end; l++) { u32 j = dftl_find(f, (u32)l); if (j != DM_NONE32) f->nodes[j].dirty = 0; }
}

/* miss handling shared by host write (dirty) and host read (clean): ftlsim cmt_update's miss branch */
static void dftl_miss(struct dftl *f, u32 lpn, int dirty)
{
	u64 tp = lpn / DFTL_TP_ENTRIES;
	f->st->map_miss++;
	if (!f->tpbuf || tp != f->tp_buf) dftl_map_read(f);
	while (f->cmt_size >= f->cmt_cap) dftl_evict_one(f);
	if (f->tpbuf) f->tp_buf = tp;
	dftl_insert(f, lpn, dirty);
}

/* ftlsim cmt_update */
static void dftl_cmt_update(struct dftl *f, u32 lpn)
{
	u32 i = dftl_find(f, lpn);
	if (i != DM_NONE32) { dftl_lru_unlink(f, i); dftl_lru_push_front(f, i); f->nodes[i].dirty = 1; return; }
	dftl_miss(f, lpn, 1);
}

/* ftlsim cmt_gc_update */
static void dftl_cmt_gc_update(struct dftl *f, u32 lpn)
{
	u32 i = dftl_find(f, lpn); u64 tp;
	if (i != DM_NONE32) { f->nodes[i].dirty = 1; return; }
	tp = lpn / DFTL_TP_ENTRIES;
	if (!f->gc_tp_mark[tp]) { f->gc_tp_mark[tp] = 1; f->gc_tp_list[f->gc_tp_n++] = (u32)tp; }
}

/* ftlsim cmt_gc_flush */
static void dftl_cmt_gc_flush(struct dftl *f)
{
	u32 k;
	for (k = 0; k < f->gc_tp_n; k++) { f->gc_tp_mark[f->gc_tp_list[k]] = 0; dftl_map_read(f); dftl_map_prog(f); }
	f->gc_tp_n = 0;
}

/* ---------------------------------------------------------------- data path (= page4k) */
static void dftl_erase_block(struct dftl *f, u32 b)
{
	if (f->bvalid[b] != 0) dm_die("erase block %u with valid=%d\n", b, f->bvalid[b]);
	f->bused[b] = 0;
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}

static int dftl_is_frontier(struct dftl *f, u32 b) { return (s32)b == f->hf.blk || (s32)b == f->gf.blk; }

static s32 dftl_pick_victim(struct dftl *f)
{
	s32 best = -1, bv = 0x7fffffff; u32 b;
	for (b = 0; b < f->g.NB; b++) {
		if (!f->bused[b] || dftl_is_frontier(f, b)) continue;
		if (f->bvalid[b] < bv) { bv = f->bvalid[b]; best = (s32)b; }
	}
	return best;
}

static void dftl_page_write(struct dftl *f, u64 lpn, int src);

/* ftlsim page_gc */
static void dftl_do_gc(struct dftl *f)
{
	s32 b = dftl_pick_victim(f); u32 s, base;
	if (b < 0) dm_die("dftl: no victim\n");
	f->in_gc = 1;
	base = (u32)b * f->g.P;
	for (s = 0; s < f->g.P && f->bvalid[b] > 0; s++) {
		u32 ppn = base + s, lpn = f->p2l[ppn];
		if (lpn == DM_NONE32 || f->pmap[lpn] != ppn) continue;
		f->st->read[DM_R_GC]++; f->fo.read(f->fo.ctx, ppn, DM_R_GC);
		dftl_page_write(f, lpn, DM_P_GC);
	}
	if (f->bvalid[b] != 0) dm_die("gc left valid=%d\n", f->bvalid[b]);
	dftl_cmt_gc_flush(f);
	dftl_erase_block(f, (u32)b);
	f->in_gc = 0;
}

static u32 dftl_alloc_block(struct dftl *f)
{
	s32 b;
	if (!f->in_gc) while (f->nfree <= f->g.T) dftl_do_gc(f);
	if (!f->nfree) dm_die("free list exhausted\n");
	b = f->freelist[--f->nfree];
	f->bused[b] = 1; f->bvalid[b] = 0;
	return (u32)b;
}

static u32 dftl_alloc_page(struct dftl *f, int gc)
{
	struct dftl_frontier *fr = gc ? &f->gf : &f->hf;
	if (fr->blk < 0 || fr->next == f->g.P) { fr->blk = (s32)dftl_alloc_block(f); fr->next = 0; }
	return (u32)fr->blk * f->g.P + fr->next++;
}

/* ftlsim page_write */
static void dftl_page_write(struct dftl *f, u64 lpn, int src)
{
	u32 ppn = dftl_alloc_page(f, src != DM_P_HOST), old;
	f->p2l[ppn] = (u32)lpn;
	f->bvalid[ppn / f->g.P]++;
	f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn, src);
	old = f->pmap[lpn];
	if (old != DM_NONE32) f->bvalid[old / f->g.P]--;
	f->pmap[lpn] = ppn;
	if (src == DM_P_HOST) dftl_cmt_update(f, (u32)lpn); else dftl_cmt_gc_update(f, (u32)lpn);
}

/*
 * cmt_cap = (u64)(cache_bytes * HASH_LF / 8.0) with HASH_LF = 0.7 as an IEEE-754 double, without FPU:
 * 0.7 = M / 2^53, M = 0x16666666666666. The double product is round-to-nearest-even of cb * M / 2^53 to a
 * 53-bit significand; /8.0 is exact; the cast truncates. (cb * 0.7 / 8 is often an exact integer, e.g.
 * 409600 -> 35840, where a plain integer floor(cb * 7 / 80) would agree but floor(cb * M / 2^56) would not.)
 */
static u64 dftl_cmt_cap(u64 cb)
{
	const u64 M = 0x16666666666666ull;
	unsigned __int128 p = (unsigned __int128)cb * M, q, rem, half;
	int nb = 0, sh;
	{ unsigned __int128 t = p; while (t) { nb++; t >>= 1; } }
	if (nb > 53) {
		sh = nb - 53;
		q = p >> sh; rem = p & ((((unsigned __int128)1) << sh) - 1); half = ((unsigned __int128)1) << (sh - 1);
		if (rem > half || (rem == half && (q & 1))) q++;
		p = q << sh;
	}
	return (u64)(p >> 56);
}

static void *dftl_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct dftl *f = dm_zalloc(sizeof(*f)); u64 i, np = (u64)g->NB * g->P, v, nbk;
	if (!f) return NULL;
	f->g = *g; f->fo = *fo; f->st = st;
	if (g->L >= DM_NONE32 || np >= DM_NONE32) dm_die("dftl: geometry exceeds 32-bit ppn/lpn\n");
	/* options may be separated by ' ' or ',' (validate.sh word-splits its harness args, so
	 * "params=cache_bytes=409600,dftl_tpbuf=1" is the only way to pass two options through it) */
	{ size_t n = params ? strlen(params) : 0; char *pc = dm_zalloc(n + 1), *q;
	  if (!pc) dm_die("dftl: alloc failed\n");
	  if (n) memcpy(pc, params, n);
	  for (q = pc; *q; q++) if (*q == ',') *q = ' ';
	  f->cache_bytes = 12ull << 20;                     /* ftlsim default */
	  if (dm_param_u64(pc, "cache_bytes", &v)) f->cache_bytes = v;
	  f->tpbuf = 0;                                     /* ftlsim default */
	  if (dm_param_u64(pc, "dftl_tpbuf", &v)) f->tpbuf = v != 0;
	  dm_free(pc); }
	f->tp_buf = ~0ull;
	f->cmt_cap = dftl_cmt_cap(f->cache_bytes);
	if (f->cmt_cap == 0 || f->cmt_cap >= DM_NONE32) dm_die("dftl: bad cmt_cap %llu\n", (unsigned long long)f->cmt_cap);

	f->pmap = dm_zalloc(sizeof(u32) * g->L); f->p2l = dm_zalloc(sizeof(u32) * np);
	f->bvalid = dm_zalloc(sizeof(s32) * g->NB); f->bused = dm_zalloc(g->NB); f->freelist = dm_zalloc(sizeof(s32) * g->NB);
	f->nnodes = (u32)f->cmt_cap + 1;
	f->nodes = dm_zalloc(sizeof(struct dftl_node) * f->nnodes);
	for (nbk = 1; nbk < f->nnodes; nbk <<= 1) ;
	f->hmask = (u32)(nbk - 1);
	f->buckets = dm_zalloc(sizeof(u32) * nbk);
	f->ntp = (g->L + DFTL_TP_ENTRIES - 1) / DFTL_TP_ENTRIES;
	f->gc_tp_mark = dm_zalloc(f->ntp);
	f->gc_tp_list = dm_zalloc(sizeof(u32) * g->P);      /* at most one insert per relocated page */
	if (!f->pmap || !f->p2l || !f->bvalid || !f->bused || !f->freelist || !f->nodes || !f->buckets ||
	    !f->gc_tp_mark || !f->gc_tp_list) dm_die("dftl: alloc failed\n");
	for (i = 0; i < g->L; i++) f->pmap[i] = DM_NONE32;
	for (i = 0; i < np; i++) f->p2l[i] = DM_NONE32;
	for (i = 0; i < g->NB; i++) f->freelist[i] = (s32)(g->NB - 1 - i);
	f->nfree = g->NB; f->hf.blk = f->gf.blk = -1;
	for (i = 0; i < nbk; i++) f->buckets[i] = DM_NONE32;
	for (i = 0; i < f->nnodes; i++) f->nodes[i].hnext = (i + 1 < f->nnodes) ? (u32)(i + 1) : DM_NONE32;
	f->free_node = 0; f->lru_head = f->lru_tail = DM_NONE32;
	dm_log("dftl: cache_bytes=%llu cmt_cap=%llu tpbuf=%d\n", (unsigned long long)f->cache_bytes,
	       (unsigned long long)f->cmt_cap, f->tpbuf);
	return f;
}

static void dftl_destroy(void *c)
{
	struct dftl *f = c;
	dm_free(f->pmap); dm_free(f->p2l); dm_free(f->bvalid); dm_free(f->bused); dm_free(f->freelist);
	dm_free(f->nodes); dm_free(f->buckets); dm_free(f->gc_tp_mark); dm_free(f->gc_tp_list); dm_free(f);
}

static void dftl_write(void *c, u64 lpn, u64 n)
{
	struct dftl *f = c; u64 p;
	f->st->host_pages += n;
	for (p = lpn; p < lpn + n; p++) dftl_page_write(f, p, DM_P_HOST);
}

/* DFTL read (ftlsim has no read path): translate through the CMT (clean insert on miss), then read data */
static void dftl_read(void *c, u64 lpn, u64 n)
{
	struct dftl *f = c; u64 p;
	for (p = lpn; p < lpn + n; p++) {
		u32 i = dftl_find(f, (u32)p);
		if (i != DM_NONE32) { dftl_lru_unlink(f, i); dftl_lru_push_front(f, i); }
		else dftl_miss(f, (u32)p, 0);
		if (f->pmap[p] != DM_NONE32) { f->st->read[DM_R_HOST]++; f->fo.read(f->fo.ctx, f->pmap[p], DM_R_HOST); }
	}
}

static void dftl_flush(void *c) { (void)c; }

/* ftlsim report(): hash_bytes(cmt.size(), 8) + (L / 1024) * 4, printed "%.0f".
 * size * 8 / 0.7 (double) = 80*size/7 + tiny; the fraction k/7 is never near .5 -> round(80*size/7). */
static u64 dftl_mem(void *c)
{
	struct dftl *f = c;
	return (f->cmt_size * 80 + 3) / 7 + (f->g.L / DFTL_TP_ENTRIES) * 4;
}

static int dftl_verify(void *c)
{
	struct dftl *f = c; u64 l, cnt = 0; u32 b, i, prev = DM_NONE32; int bad = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->g.NB);
	if (!v) return 1;
	for (l = 0; l < f->g.L; l++) if (f->pmap[l] != DM_NONE32) v[f->pmap[l] / f->g.P]++;
	for (b = 0; b < f->g.NB; b++) if (v[b] != f->bvalid[b]) { bad = 1; break; }
	dm_free(v);
	/* CMT invariants: LRU list consistent, every node findable, size <= cap, GC set empty */
	for (i = f->lru_head; i != DM_NONE32 && !bad; i = f->nodes[i].next) {
		if (f->nodes[i].prev != prev || dftl_find(f, f->nodes[i].lpn) != i || f->nodes[i].lpn >= f->g.L) bad = 1;
		prev = i; if (++cnt > f->cmt_size) bad = 1;
	}
	if (cnt != f->cmt_size || f->lru_tail != prev || f->cmt_size > f->cmt_cap || f->gc_tp_n) bad = 1;
	return bad;
}

static void dftl_report(void *c, char *buf, int len)
{
	struct dftl *f = c;
	snprintf(buf, len, "\"dftl\": {\"cache_bytes\": %llu, \"cmt_cap\": %llu, \"cmt_size\": %llu, \"tpbuf\": %d}",
		 (unsigned long long)f->cache_bytes, (unsigned long long)f->cmt_cap, (unsigned long long)f->cmt_size,
		 f->tpbuf);
}

const struct dm_ftl_ops dm_policy = {
	.name = "dftl", .create = dftl_create, .destroy = dftl_destroy, .write = dftl_write, .read = dftl_read,
	.flush = dftl_flush, .mem_bytes = dftl_mem, .verify = dftl_verify, .report = dftl_report,
};
