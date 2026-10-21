// SPDX-License-Identifier: GPL-2.0-only
/*
 * mu: mu-FTL (Lee, Jung, Kang, Kim, EMSOFT'08) - extent mapping <l,p,n> kept in a flash-resident mu-tree with a
 * RAM page cache. Port of ftlsim `mode=mu` (struct MuSim), bit-identical on the validation traces.
 *
 *  - logical space split into mu_parts partitions, each with one update block shared by host writes AND GC copies
 *    (paper 4.3.1 / 4.4; no host/GC separation).
 *  - write: append to the partition's update block -> remove old mapping (Cases 0-3: delete / split / trim right /
 *    trim left) -> insert extents of <= mu_max_extent pages (merge with the logically+physically contiguous
 *    predecessor in the same block when the result stays <= mu_max_extent).
 *  - mu-tree modeled as leaves of <= mu_leaf_fanout extents (split in half on overflow, removed when empty);
 *    the flash I/O of the tree is an LRU page cache of cache_bytes/4096 leaf pages (miss -> DM_R_MAP read, dirty
 *    eviction -> DM_P_MAP program).
 *  - invalid-page bitmap: per-block counters + a "bitmap cache" of bitmap_cache_bytes/4 invalid-extent entries;
 *    when full it is flushed by dirtying one bitmap leaf per group of F/4 blocks.
 *  - GC when free blocks <= T (not re-entered), victim = most invalid pages (ties -> lowest id), update blocks
 *    excluded, valid runs copied to their partition's update block, repeated until free >= T+2.
 *
 * Representation (kernel-friendly, O(L) arrays, no per-extent allocation): the extent map is stored implicitly as
 *   pmap[lpn] (4 B/page) + an extent-start bitmap (1 bit/page); an extent = a start bit followed by mapped pages up
 *   to the next start bit / unmapped page. Leaves are a sorted array of {front lpn, extent count, leaf id}.
 *   This is exactly equivalent to MuSim's vector<Leaf{vector<Ext>}> (see NOTES_mu.md).
 * The order in which bm_flush() touches bitmap leaves follows libc++'s unordered_map/unordered_set iteration
 * order in ftlsim (zig c++ = libc++); a small exact model of the libc++ hash table reproduces it (mu_lht_*).
 */
#include "ftlcore.h"

#define MU_NONE DM_NONE32

/* ------------------------------------------------------------------------------------------------------------
 * Exact model of libc++ std::__hash_table (unique keys, std::hash<integer> = identity, max_load_factor 1) for
 * small integer keys in [0, K). Only membership and iteration order are modeled (the mapped values are unused).
 * Node "pointers" are key indices; index K is the before-begin sentinel (__p1_).
 * ------------------------------------------------------------------------------------------------------------ */
struct mu_lht {
	u32 K;
	s32 *next;      /* K + 1 entries: next[K] = before-begin->next; -1 = nullptr */
	u8 *in;
	s32 *bkt;       /* bucket -> node before the bucket's first node (key or K), -1 = nullptr */
	u64 bc, size, bcap;
};

static int mu_is_prime(u64 n)
{
	u64 d;
	if (n < 2) return 0;
	if (n < 4) return 1;
	if (!(n & 1)) return 0;
	for (d = 3; d * d <= n; d += 2) if (n % d == 0) return 0;
	return 1;
}
static u64 mu_next_prime(u64 n) { while (!mu_is_prime(n)) n++; return n; }
static int mu_is_hash_pow2(u64 bc) { return bc > 2 && !(bc & (bc - 1)); }
static u64 mu_constrain(u64 h, u64 bc) { return !(bc & (bc - 1)) ? (h & (bc - 1)) : (h < bc ? h : h % bc); }
static u64 mu_lht_growth(u64 bc, u64 size)
{
	u64 n = 2 * bc + !mu_is_hash_pow2(bc);
	if (size + 1 > n) n = size + 1;
	if (n == 1) n = 2; else if (n & (n - 1)) n = mu_next_prime(n);
	return n;
}

static int mu_lht_init(struct mu_lht *h, u32 K)
{
	u64 bc = 0, s;
	u32 i;
	for (s = 0; s < K; s++) if (s + 1 > bc) bc = mu_lht_growth(bc, s);   /* largest bucket count ever needed */
	h->K = K; h->bcap = bc ? bc : 2;
	h->next = dm_zalloc(sizeof(s32) * (K + 1)); h->in = dm_zalloc(K ? K : 1); h->bkt = dm_zalloc(sizeof(s32) * h->bcap);
	if (!h->next || !h->in || !h->bkt) return -1;
	for (i = 0; i <= K; i++) h->next[i] = -1;
	for (s = 0; s < h->bcap; s++) h->bkt[s] = -1;
	h->bc = 0; h->size = 0;
	return 0;
}
static void mu_lht_free(struct mu_lht *h) { dm_free(h->next); dm_free(h->in); dm_free(h->bkt); }

/* a fresh (default-constructed) table: bucket_count 0 */
static void mu_lht_reset(struct mu_lht *h)
{
	s32 k = h->next[h->K];
	u64 s;
	while (k >= 0) { s32 nx = h->next[k]; h->in[k] = 0; h->next[k] = -1; k = nx; }
	h->next[h->K] = -1;
	for (s = 0; s < h->bc; s++) h->bkt[s] = -1;
	h->bc = 0; h->size = 0;
}

/* clear(): nodes freed, buckets nulled, bucket_count kept */
static void mu_lht_clear(struct mu_lht *h)
{
	u64 bc = h->bc;
	if (!h->size) return;
	mu_lht_reset(h);
	h->bc = bc;
}

static void mu_lht_do_rehash(struct mu_lht *h, u64 nbc)
{
	s32 SENT = (s32)h->K, pp, cp;
	u64 i, chash, phash;
	if (nbc > h->bcap) dm_die("mu: lht bucket overflow %llu > %llu\n", (unsigned long long)nbc, (unsigned long long)h->bcap);
	h->bc = nbc;
	for (i = 0; i < nbc; i++) h->bkt[i] = -1;
	pp = SENT; cp = h->next[pp];
	if (cp < 0) return;
	chash = mu_constrain((u64)cp, nbc); h->bkt[chash] = pp; phash = chash;
	for (pp = cp, cp = h->next[cp]; cp >= 0; cp = h->next[pp]) {
		chash = mu_constrain((u64)cp, nbc);
		if (chash == phash) pp = cp;
		else if (h->bkt[chash] < 0) { h->bkt[chash] = pp; pp = cp; phash = chash; }
		else {
			s32 np = cp;
			h->next[pp] = h->next[np];
			h->next[np] = h->next[h->bkt[chash]];
			h->next[h->bkt[chash]] = cp;
		}
	}
}

static void mu_lht_insert(struct mu_lht *h, u32 k)
{
	s32 SENT = (s32)h->K, pn;
	u64 chash;
	if (h->in[k]) return;
	if (h->size + 1 > h->bc || h->bc == 0) {
		u64 n = mu_lht_growth(h->bc, h->size);
		if (n > h->bc) mu_lht_do_rehash(h, n);
	}
	chash = mu_constrain(k, h->bc); pn = h->bkt[chash];
	if (pn < 0) {
		h->next[k] = h->next[SENT]; h->next[SENT] = (s32)k; h->bkt[chash] = SENT;
		if (h->next[k] >= 0) h->bkt[mu_constrain((u64)h->next[k], h->bc)] = (s32)k;
	} else {
		h->next[k] = h->next[pn]; h->next[pn] = (s32)k;
	}
	h->in[k] = 1; h->size++;
}

static void mu_lht_erase(struct mu_lht *h, u32 k)
{
	s32 SENT = (s32)h->K, pn, cn = (s32)k;
	u64 chash;
	if (!h->in[k]) return;
	chash = mu_constrain(k, h->bc); pn = h->bkt[chash];
	while (h->next[pn] != cn) pn = h->next[pn];
	if (pn == SENT || mu_constrain((u64)pn, h->bc) != chash) {
		if (h->next[cn] < 0 || mu_constrain((u64)h->next[cn], h->bc) != chash) h->bkt[chash] = -1;
	}
	if (h->next[cn] >= 0) {
		u64 nh = mu_constrain((u64)h->next[cn], h->bc);
		if (nh != chash) h->bkt[nh] = pn;
	}
	h->next[pn] = h->next[cn]; h->next[cn] = -1;
	h->in[k] = 0; h->size--;
}

/* ------------------------------------------------------------------------------------------------------------ */
struct mu_cnode { u64 key; s32 prev, next, hnext; u8 dirty; };
struct mu_leaf { u32 front; u32 cnt; u64 id; };

struct mu {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u64 L, NP; u32 NB, P, T;
	u64 cache_bytes, bm_bytes;
	u32 parts, F, MAXE, gdiv;
	u64 part_pages;
	/* extent map (implicit) */
	u32 *pmap;                 /* lpn -> ppn */
	u64 *estart;               /* extent start bitmap */
	u64 nwords;
	struct mu_leaf *lv; u64 nleaves, lcap; u64 next_leaf_id;
	/* leaf-search guide (performance only): lg[c] ~ mu_leaf_for(c << MU_GSH), used as bounds of the binary
	 * search after an exact validity check (else: full search, entry repaired); lhint = last result */
	u32 *lg; u64 nlg, lhint;
	/* physical */
	u32 *p2l;
	u8 *btag;
	s32 *binvalid, *bwritten;
	s32 *freelist; u32 nfree;
	s32 *upd;
	int in_gc;
	/* mu-tree page cache (LRU) */
	u64 cache_pages, ccount;
	struct mu_cnode *cn; s32 chead, ctail, cfree;
	s32 *chash; u32 chbits;
	/* bitmap cache */
	u64 bm_cap, bm_n;
	struct mu_lht bm_runs, groups;
	/* events */
	u64 n_extents, ext_split, ext_merge, bm_flushes, n_gc, gc_valid_sum;
};

/* ---- bitmap helpers on estart */
static inline int mu_bit(struct mu *f, u64 i) { return (f->estart[i >> 6] >> (i & 63)) & 1; }
static inline void mu_bset(struct mu *f, u64 i) { f->estart[i >> 6] |= 1ull << (i & 63); }
static inline void mu_bclr(struct mu *f, u64 i) { f->estart[i >> 6] &= ~(1ull << (i & 63)); }

/* first start bit in [from, lim), else lim */
static u64 mu_next_start(struct mu *f, u64 from, u64 lim)
{
	u64 w, x, r;
	if (from >= lim) return lim;
	w = from >> 6; x = f->estart[w] & (~0ull << (from & 63));
	for (;;) {
		if (x) { r = (w << 6) + dm_ctz64(x); return r < lim ? r : lim; }
		w++;
		if ((w << 6) >= lim || w >= f->nwords) return lim;
		x = f->estart[w];
	}
}

/* last start bit <= from, else -1 */
static s64 mu_prev_start(struct mu *f, u64 from)
{
	u64 w = from >> 6, b = from & 63, x;
	x = f->estart[w] & (b == 63 ? ~0ull : ((2ull << b) - 1));
	for (;;) {
		if (x) return (s64)((w << 6) + 63 - dm_clz64(x));
		if (!w) return -1;
		x = f->estart[--w];
	}
}

/* k-th (0-based) start bit at or after from */
static u64 mu_select(struct mu *f, u64 from, u64 k)
{
	u64 w = from >> 6, x = f->estart[w] & (~0ull << (from & 63)), c;
	for (;;) {
		c = (u64)dm_popcount64(x);
		if (k < c) { while (k--) x &= x - 1; return (w << 6) + dm_ctz64(x); }
		k -= c; w++;
		if (w >= f->nwords) dm_die("mu: select past end\n");
		x = f->estart[w];
	}
}

/* ---- mu-tree page cache model (MuSim::touch_leaf / drop_leaf) */
static inline u32 mu_chash_of(struct mu *f, u64 key) { return (u32)((key * 0x9E3779B97F4A7C15ull) >> (64 - f->chbits)); }

static s32 mu_cfind(struct mu *f, u64 key)
{
	s32 i = f->chash[mu_chash_of(f, key)];
	while (i >= 0 && f->cn[i].key != key) i = f->cn[i].hnext;
	return i;
}
static void mu_cunlink(struct mu *f, s32 i)
{
	struct mu_cnode *n = &f->cn[i];
	if (n->prev >= 0) f->cn[n->prev].next = n->next; else f->chead = n->next;
	if (n->next >= 0) f->cn[n->next].prev = n->prev; else f->ctail = n->prev;
}
static void mu_cpush_front(struct mu *f, s32 i)
{
	struct mu_cnode *n = &f->cn[i];
	n->prev = -1; n->next = f->chead;
	if (f->chead >= 0) f->cn[f->chead].prev = i; else f->ctail = i;
	f->chead = i;
}
static void mu_cremove(struct mu *f, s32 i)
{
	u32 h = mu_chash_of(f, f->cn[i].key);
	s32 *pp = &f->chash[h];
	while (*pp != i) pp = &f->cn[*pp].hnext;
	*pp = f->cn[i].hnext;
	mu_cunlink(f, i);
	f->cn[i].hnext = f->cfree; f->cfree = i; f->ccount--;
}

static void mu_touch(struct mu *f, u64 id, int dirty)
{
	s32 i;
	u32 h;
	/* already MRU (e.g. unmap_range then insert_ext on the same leaf): moving it to the front is a no-op */
	if (f->chead >= 0 && f->cn[f->chead].key == id) { f->cn[f->chead].dirty |= (u8)dirty; return; }
	i = mu_cfind(f, id);
	if (i >= 0) {
		mu_cunlink(f, i); mu_cpush_front(f, i);
		f->cn[i].dirty |= (u8)dirty;
		return;
	}
	f->st->map_miss++;
	f->st->read[DM_R_MAP]++; f->fo.read(f->fo.ctx, DM_NOPPN, DM_R_MAP);
	while (f->ccount >= f->cache_pages) {
		s32 v = f->ctail;
		if (f->cn[v].dirty) { f->st->prog[DM_P_MAP]++; f->fo.prog(f->fo.ctx, DM_NOPPN, DM_P_MAP); }
		mu_cremove(f, v);
	}
	i = f->cfree; f->cfree = f->cn[i].hnext; f->ccount++;
	f->cn[i].key = id; f->cn[i].dirty = (u8)dirty;
	h = mu_chash_of(f, id); f->cn[i].hnext = f->chash[h]; f->chash[h] = i;
	mu_cpush_front(f, i);
}
static void mu_drop(struct mu *f, u64 id) { s32 i = mu_cfind(f, id); if (i >= 0) mu_cremove(f, i); }

/* ---- leaves
 * MuSim::leaf_for = binary search for the last leaf with (cnt && front <= l), leaf 0 if none. The predicate is
 * monotone in the leaf index: fronts strictly increase and a leaf with cnt == 0 exists only when it is the only
 * leaf (mu_unmap_range removes emptied leaves otherwise); mu_verify checks both. So the result is the unique
 * last-true index, and any window [lo, hi) with (lo == 0 || pred(lo)) and (hi == nleaves || !pred(hi)) yields
 * the same leaf. The guide lg[] (one entry per 2^MU_GSH logical pages) and the last result supply such windows;
 * they are hints only (checked exactly, else the full search runs and repairs the guide). */
#define MU_GSH 12
static inline int mu_lpred(const struct mu_leaf *x, u64 l) { return x->cnt && x->front <= l; }
/* last true index in [lo, hi) given (lo == 0 || pred(lo)); branch-free (the answer stays in the window) */
static u64 mu_leaf_search(struct mu *f, u64 l, u64 lo, u64 hi)
{
	u64 n = hi - lo;
	while (n > 1) { u64 half = n / 2; lo += mu_lpred(&f->lv[lo + half], l) ? half : 0; n -= half; }
	return lo;
}
static u64 mu_leaf_for(struct mu *f, u64 l)   /* last leaf whose first key <= l (leaf 0 otherwise) */
{
	u64 c = l >> MU_GSH, n = f->nleaves, lo, hi, r;
	if (n <= 1) return 0;
	lo = f->lhint;                                  /* same leaf as the previous lookup? */
	if (lo < n && (lo == 0 || mu_lpred(&f->lv[lo], l)) && (lo + 1 == n || !mu_lpred(&f->lv[lo + 1], l))) return lo;
	lo = f->lg[c]; hi = c + 1 < f->nlg ? (u64)f->lg[c + 1] + 1 : n;
	if (hi > n) hi = n;
	if (lo < hi && (lo == 0 || mu_lpred(&f->lv[lo], l)) && (hi == n || !mu_lpred(&f->lv[hi], l)))
		return f->lhint = mu_leaf_search(f, l, lo, hi);
	/* stale guide (a split / removal / front change since it was set): full search, repair both bounds */
	r = mu_leaf_search(f, l, 0, n);
	f->lg[c] = (u32)mu_leaf_search(f, c << MU_GSH, 0, n);
	if (c + 1 < f->nlg) f->lg[c + 1] = (u32)mu_leaf_search(f, (c + 1) << MU_GSH, 0, n);
	return f->lhint = r;
}
/* keep the guide close after leaf index shifts (+1 for entries >= pos on insert, -1 for entries > pos on remove) */
static void mu_lg_shift(struct mu *f, u64 pos, int ins)
{
	u32 *g = f->lg, p = (u32)pos; u64 c, n = f->nlg;
	if (ins) { for (c = 0; c < n; c++) g[c] += g[c] >= p; }
	else { for (c = 0; c < n; c++) g[c] -= g[c] > p || (g[c] == p && p); }
}
static void mu_leaf_insert_at(struct mu *f, u64 pos, u32 front, u32 cnt)
{
	if (f->nleaves == f->lcap) {   /* bounded growth: leaves <= extents <= L; process context (may sleep) */
		u64 nc = f->lcap * 2;
		struct mu_leaf *nl = dm_zalloc(sizeof(*nl) * nc);
		if (!nl) dm_die("mu: leaf array alloc failed (%llu)\n", (unsigned long long)nc);
		memcpy(nl, f->lv, sizeof(*nl) * f->nleaves);
		dm_free(f->lv); f->lv = nl; f->lcap = nc;
	}
	memmove(&f->lv[pos + 1], &f->lv[pos], sizeof(*f->lv) * (f->nleaves - pos));
	f->lv[pos].front = front; f->lv[pos].cnt = cnt; f->lv[pos].id = f->next_leaf_id++;
	f->nleaves++;
	mu_lg_shift(f, pos, 1);
}
static void mu_leaf_remove_at(struct mu *f, u64 pos)
{
	memmove(&f->lv[pos], &f->lv[pos + 1], sizeof(*f->lv) * (f->nleaves - pos - 1));
	f->nleaves--;
	mu_lg_shift(f, pos, 0);
}

/* ---- invalid bitmap cache (MuSim::invalidate_phys / bm_flush) */
static void mu_bm_flush(struct mu *f)
{
	s32 k;
	f->bm_flushes++;
	mu_lht_reset(&f->groups);
	for (k = f->bm_runs.next[f->bm_runs.K]; k >= 0; k = f->bm_runs.next[k]) mu_lht_insert(&f->groups, (u32)k / f->gdiv);
	for (k = f->groups.next[f->groups.K]; k >= 0; k = f->groups.next[k]) mu_touch(f, (1ull << 40) | (u64)k, 1);
	mu_lht_clear(&f->bm_runs); f->bm_n = 0;
}
static void mu_invalidate_phys(struct mu *f, u32 p, u32 n)
{
	u32 b = p / f->P;
	f->binvalid[b] += (s32)n;
	if (!f->in_gc) {
		mu_lht_insert(&f->bm_runs, b);
		f->bm_n++;
		if (f->bm_n >= f->bm_cap) mu_bm_flush(f);
	}
}

/* extent starting at xs: end (exclusive) = next start bit or first unmapped page */
static u64 mu_ext_end(struct mu *f, u64 xs)
{
	u64 e = xs + 1;
	while (e < f->L && f->pmap[e] != MU_NONE && !mu_bit(f, e)) e++;
	return e;
}

/* remove mapping for [l, l+n) (Cases 0-3), invalidating the physical pages (MuSim::unmap_range) */
static void mu_unmap_range(struct mu *f, u64 l, u64 n)
{
	u64 end = l + n, xs, li;
	if (f->pmap[l] != MU_NONE) { s64 s = mu_prev_start(f, l); if (s < 0) dm_die("mu: mapped page without extent\n"); xs = (u64)s; }
	else { xs = mu_next_start(f, l, end); if (xs >= end) return; }
	li = mu_leaf_for(f, xs);
	while (xs < end) {
		/* one leaf group: extents of leaf li overlapping [l, end) */
		u64 nfront = li + 1 < f->nleaves ? f->lv[li + 1].front : ~0ull;
		struct mu_leaf *lf = &f->lv[li];
		while (xs < end && xs < nfront) {
			u64 xe = mu_ext_end(f, xs), ol = xs > l ? xs : l, oe = xe < end ? xe : end, q, nx;
			mu_invalidate_phys(f, f->pmap[ol], (u32)(oe - ol));
			for (q = ol; q < oe; q++) f->pmap[q] = MU_NONE;
			if (ol == xs && oe == xe) {                                          /* case 0: delete */
				mu_bclr(f, xs); lf->cnt--; f->n_extents--;
				if (xs == lf->front && lf->cnt) lf->front = (u32)mu_next_start(f, xe, f->L);
			} else if (ol > xs && oe < xe) {                                     /* case 1: split */
				mu_bset(f, oe); lf->cnt++; f->n_extents++; f->ext_split++;
			} else if (ol > xs) {                                                /* case 2: trim right */
			} else {                                                             /* case 3: trim left */
				mu_bclr(f, xs); mu_bset(f, oe);
				if (xs == lf->front) lf->front = (u32)oe;
			}
			nx = mu_next_start(f, xe, end);
			xs = nx;
		}
		mu_touch(f, lf->id, 1);
		if (!lf->cnt && f->nleaves > 1) { mu_drop(f, lf->id); mu_leaf_remove_at(f, li); }
		else li++;
	}
}

/* MuSim::insert_ext; pmap for the pages is set here */
static void mu_insert_ext(struct mu *f, u64 l, u32 p, u32 n)
{
	u64 li = mu_leaf_for(f, l), q;
	struct mu_leaf *lf = &f->lv[li];
	for (q = 0; q < n; q++) f->pmap[l + q] = p + (u32)q;
	if (l > 0 && f->pmap[l - 1] != MU_NONE && lf->cnt && lf->front < l) {
		u32 pq = f->pmap[l - 1];
		if (pq + 1 == p && pq / f->P == p / f->P) {
			s64 s = mu_prev_start(f, l - 1);
			if (s < 0) dm_die("mu: predecessor without start\n");
			if ((l - (u64)s) + n <= f->MAXE) { f->ext_merge++; mu_touch(f, lf->id, 1); return; }
		}
	}
	mu_bset(f, l);
	if (!lf->cnt || l < lf->front) lf->front = (u32)l;
	lf->cnt++; f->n_extents++;
	mu_touch(f, lf->id, 1);
	if (lf->cnt > f->F) {   /* split leaf: left keeps size/2 */
		u32 k = lf->cnt / 2, rc = lf->cnt - k;
		u64 s = mu_select(f, lf->front, k);
		lf->cnt = k;
		mu_leaf_insert_at(f, li + 1, (u32)s, rc);
		mu_touch(f, f->lv[li + 1].id, 1);
	}
}

/* ---- blocks */
static void mu_erase(struct mu *f, u32 b)
{
	f->btag[b] = 0; f->binvalid[b] = 0; f->bwritten[b] = 0;
	mu_lht_erase(&f->bm_runs, b);
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}
static u32 mu_pop_free(struct mu *f)
{
	s32 b;
	if (!f->nfree) dm_die("mu: free list exhausted\n");
	b = f->freelist[--f->nfree]; f->btag[b] = 1;
	return (u32)b;
}

static void mu_write_pages(struct mu *f, u64 l, u64 n, int src);

#define MU_PF 16          /* GC prefetch distance (victim pages) */
#define MU_RESCHED 1024u  /* GC victim pages between preemption points (kernel: cond_resched, see dm_port.h) */
static void mu_gc(struct mu *f)
{
	u32 nit = 0;
	f->in_gc = 1;
	while (f->nfree < f->T + 2) {
		s32 v = -1, best = -1; u32 b, i, base, s0, bw;
		for (b = 0; b < f->NB; b++) {
			int isupd = 0;
			if (!f->btag[b]) continue;
			for (i = 0; i < f->parts; i++) if (f->upd[i] == (s32)b) isupd = 1;
			if (isupd) continue;
			if (f->binvalid[b] > best) { best = f->binvalid[b]; v = (s32)b; }
		}
		if (v < 0) dm_die("mu: no victim\n");
		if (best <= 0) dm_die("mu: victim has no invalid pages (free=%u)\n", f->nfree);
		f->n_gc++; f->gc_valid_sum += (u64)(f->bwritten[v] - f->binvalid[v]);
		mu_lht_erase(&f->bm_runs, (u32)v);
		base = (u32)v * f->P;
		for (s0 = 0; s0 < (u32)f->bwritten[v];) {
			u32 lpn = f->p2l[base + s0], run = 1, r;
			/* the valid check and the copy's unmap/insert touch pmap/estart at random lpns: prefetch ahead */
			if (s0 + MU_PF < (u32)f->bwritten[v]) {
				u32 pl = f->p2l[base + s0 + MU_PF];
				if (pl != MU_NONE) { dm_prefetch(&f->pmap[pl]); dm_prefetch(&f->estart[pl >> 6]); }
			}
			if (!(++nit % MU_RESCHED)) dm_cond_resched();
			if (lpn == MU_NONE || f->pmap[lpn] != base + s0) { s0++; continue; }
			bw = (u32)f->bwritten[v];
			while (s0 + run < bw && f->p2l[base + s0 + run] == lpn + run && f->pmap[lpn + run] == base + s0 + run && run < f->MAXE) run++;
			for (r = 0; r < run; r++) { f->st->read[DM_R_GC]++; f->fo.read(f->fo.ctx, base + s0 + r, DM_R_GC); }
			mu_write_pages(f, lpn, run, DM_P_GC);
			s0 += run;
		}
		if (f->binvalid[v] != f->bwritten[v]) dm_die("mu gc: block %d invalid=%d written=%d\n", v, f->binvalid[v], f->bwritten[v]);
		mu_erase(f, (u32)v);
		dm_cond_resched();
	}
	f->in_gc = 0;
}

static u32 mu_update_block(struct mu *f, u32 part)
{
	s32 b = f->upd[part];
	if (b >= 0 && (u32)f->bwritten[b] < f->P) return (u32)b;
	if (!f->in_gc && f->nfree <= f->T) mu_gc(f);
	b = (s32)mu_pop_free(f); f->upd[part] = b;
	return (u32)b;
}

/* MuSim::write_pages */
static void mu_write_pages(struct mu *f, u64 l, u64 n, int src)
{
	u64 cur = l, end = l + n;
	while (cur < end) {
		u32 part = (u32)(cur / f->part_pages), b = mu_update_block(f, part), ppn, i, off;
		u64 room = f->P - (u32)f->bwritten[b], w = end - cur < room ? end - cur : room, pend = (u64)(part + 1) * f->part_pages;
		if (cur + w > pend) w = pend - cur;
		ppn = b * f->P + (u32)f->bwritten[b];
		for (i = 0; i < w; i++) { f->p2l[ppn + i] = (u32)(cur + i); f->fo.prog(f->fo.ctx, ppn + i, src); }
		f->bwritten[b] += (s32)w; f->st->prog[src] += w;
		mu_unmap_range(f, cur, w);
		for (off = 0; off < w; off += f->MAXE) mu_insert_ext(f, cur + off, ppn + off, (u32)(w - off < f->MAXE ? w - off : f->MAXE));
		cur += w;
	}
}

/* ------------------------------------------------------------------------------------------------------------ */
static void mu_destroy(void *c);

static void *mu_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct mu *f = dm_zalloc(sizeof(*f));
	u64 v, i, nb;
	if (!f) return NULL;
	f->g = *g; f->fo = *fo; f->st = st;
	f->L = g->L; f->P = g->P; f->NB = g->NB; f->T = g->T; f->NP = (u64)g->NB * g->P;
	f->cache_bytes = 12ull << 20; f->bm_bytes = 32768; f->parts = 8; f->MAXE = 128; f->F = 256;
	if (dm_param_u64(params, "cache_bytes", &v)) f->cache_bytes = v;
	if (dm_param_u64(params, "bitmap_cache_bytes", &v)) f->bm_bytes = v;
	if (dm_param_u64(params, "mu_parts", &v)) f->parts = (u32)v;
	if (dm_param_u64(params, "mu_max_extent", &v)) f->MAXE = (u32)v;
	if (dm_param_u64(params, "mu_leaf_fanout", &v)) f->F = (u32)v;
	if (!f->parts || !f->MAXE || !f->F || f->NP >= MU_NONE || f->L >= MU_NONE) { dm_log("mu: bad params/geometry\n"); dm_free(f); return NULL; }
	f->gdiv = f->F / 4 ? f->F / 4 : 1;
	f->part_pages = (f->L + f->parts - 1) / f->parts;
	f->cache_pages = f->cache_bytes / 4096 ? f->cache_bytes / 4096 : 1;
	f->bm_cap = f->bm_bytes / 4 ? f->bm_bytes / 4 : 1;

	f->nwords = f->L / 64 + 1;
	f->pmap = dm_zalloc(sizeof(u32) * f->L); f->estart = dm_zalloc(sizeof(u64) * f->nwords);
	f->p2l = dm_zalloc(sizeof(u32) * f->NP);
	f->btag = dm_zalloc(f->NB); f->binvalid = dm_zalloc(sizeof(s32) * f->NB); f->bwritten = dm_zalloc(sizeof(s32) * f->NB);
	f->freelist = dm_zalloc(sizeof(s32) * f->NB); f->upd = dm_zalloc(sizeof(s32) * f->parts);
	f->lcap = f->L / 256 > 1024 ? f->L / 256 : 1024;
	f->lv = dm_zalloc(sizeof(*f->lv) * f->lcap);
	f->nlg = (f->L >> MU_GSH) + 1; f->lg = dm_zalloc(sizeof(u32) * f->nlg);
	f->cn = dm_zalloc(sizeof(*f->cn) * f->cache_pages);
	for (f->chbits = 1; (1ull << f->chbits) < 2 * f->cache_pages; f->chbits++) ;
	f->chash = dm_zalloc(sizeof(s32) * (1ull << f->chbits));
	if (!f->pmap || !f->estart || !f->p2l || !f->btag || !f->binvalid || !f->bwritten || !f->freelist || !f->upd || !f->lv || !f->lg ||
	    !f->cn || !f->chash || mu_lht_init(&f->bm_runs, f->NB) || mu_lht_init(&f->groups, (f->NB - 1) / f->gdiv + 1)) {
		dm_log("mu: alloc failed\n"); mu_destroy(f); return NULL;
	}
	for (i = 0; i < f->L; i++) f->pmap[i] = MU_NONE;
	for (i = 0; i < f->NP; i++) { f->p2l[i] = MU_NONE; if (!(i & 0xFFFFFF)) dm_cond_resched(); }
	nb = f->NB;
	for (i = 0; i < nb; i++) f->freelist[i] = (s32)(nb - 1 - i);
	f->nfree = f->NB;
	for (i = 0; i < f->parts; i++) f->upd[i] = -1;
	for (i = 0; i < (1ull << f->chbits); i++) f->chash[i] = -1;
	for (i = 0; i < f->cache_pages; i++) f->cn[i].hnext = i + 1 < f->cache_pages ? (s32)(i + 1) : -1;
	f->cfree = 0; f->chead = f->ctail = -1;
	f->nleaves = 1; f->lv[0].id = f->next_leaf_id++; f->lv[0].cnt = 0; f->lv[0].front = 0;
	dm_log("[cfg] mode=mu L=%llu pages (%llu.%llu GB) NB=%u P=%u parts=%u cache=%llu pages bitmap_cache=%llu entries F=%u maxext=%u T=%u\n",
	       (unsigned long long)f->L, (unsigned long long)(((f->L * 40960) + (1ull << 29)) >> 30) / 10,
	       (unsigned long long)(((f->L * 40960) + (1ull << 29)) >> 30) % 10, f->NB, f->P, f->parts,
	       (unsigned long long)f->cache_pages, (unsigned long long)f->bm_cap, f->F, f->MAXE, f->T);
	return f;
}

static void mu_destroy(void *c)
{
	struct mu *f = c;
	dm_free(f->pmap); dm_free(f->estart); dm_free(f->p2l); dm_free(f->btag); dm_free(f->binvalid); dm_free(f->bwritten);
	dm_free(f->freelist); dm_free(f->upd); dm_free(f->lv); dm_free(f->lg); dm_free(f->cn); dm_free(f->chash);
	if (f->bm_runs.next || f->bm_runs.in || f->bm_runs.bkt) mu_lht_free(&f->bm_runs);
	if (f->groups.next || f->groups.in || f->groups.bkt) mu_lht_free(&f->groups);
	dm_free(f);
}

static void mu_write(void *c, u64 lpn, u64 n)
{
	struct mu *f = c;
	f->st->host_pages += n;
	mu_write_pages(f, lpn, n, DM_P_HOST);
}

/* ftlsim has no read path: look up the leaf holding lpn (miss -> one mu-tree page read), then read the data */
static void mu_read(void *c, u64 lpn, u64 n)
{
	struct mu *f = c; u64 p;
	for (p = lpn; p < lpn + n; p++) {
		mu_touch(f, f->lv[mu_leaf_for(f, p)].id, 0);
		if (f->pmap[p] != MU_NONE) { f->st->read[DM_R_HOST]++; f->fo.read(f->fo.ctx, f->pmap[p], DM_R_HOST); }
	}
}

static void mu_flush(void *c) { (void)c; }   /* ftlsim never writes back the tree cache at the end */

/* RAM model = ftlsim "mem.total_bytes": tree cache + bitmap cache + 1 B/block invalid counter */
static u64 mu_mem(void *c) { struct mu *f = c; return f->cache_bytes + f->bm_bytes + f->NB; }

static int mu_verify(void *c)
{
	struct mu *f = c; u64 l, li, cnt = 0, xs; u32 b; int bad = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->NB);
	if (!v) return 1;
	/* extents: contiguous, same block, <= MAXE, start bits only on mapped pages */
	for (l = 0; l < f->L && !bad; l++) {
		if (!(l & 0xFFFFF)) dm_cond_resched();
		if (f->pmap[l] == MU_NONE) { if (mu_bit(f, l)) bad = 2; continue; }
		v[f->pmap[l] / f->P]++;
		if (mu_bit(f, l)) { cnt++; xs = l; }
		else {
			s64 s = mu_prev_start(f, l);
			if (s < 0 || f->pmap[l - 1] == MU_NONE || f->pmap[l - 1] + 1 != f->pmap[l] || f->pmap[l - 1] / f->P != f->pmap[l] / f->P) bad = 3;
			else if (l - (u64)s + 1 > f->MAXE) bad = 4;
			(void)xs;
		}
	}
	if (!bad && cnt != f->n_extents) bad = 5;
	for (b = 0; b < f->NB && !bad; b++) if (f->btag[b] && v[b] != f->bwritten[b] - f->binvalid[b]) bad = 6;
	/* leaves: sorted fronts on start bits, counts match the start bits in each leaf's range */
	if (!bad) {
		u64 sum = 0;
		for (li = 0; li < f->nleaves && !bad; li++) {
			struct mu_leaf *lf = &f->lv[li];
			u64 lo = lf->front, hi = li + 1 < f->nleaves ? f->lv[li + 1].front : f->L, k = 0, s;
			if (!lf->cnt) { if (f->nleaves > 1 || f->n_extents) bad = 7; continue; }
			if (!mu_bit(f, lo) || (li + 1 < f->nleaves && hi <= lo)) { bad = 8; break; }
			if (li == 0 && mu_next_start(f, 0, lo) != lo) { bad = 9; break; }
			for (s = lo; s < hi; s = mu_next_start(f, s + 1, hi)) k++;
			if (!(li & 0xFFF)) dm_cond_resched();
			if (k != lf->cnt) bad = 10;
			sum += lf->cnt;
		}
		if (!bad && sum != f->n_extents) bad = 11;
	}
	dm_free(v);
	if (bad) dm_log("mu verify failed (code %d)\n", bad);
	return bad;
}

static void mu_report(void *c, char *buf, int len)
{
	struct mu *f = c;
	snprintf(buf, len, "\"mu\": {\"parts\": %u, \"cache_pages\": %llu, \"bitmap_cache_entries\": %llu, \"gc\": %llu, "
		 "\"victim_valid_sum\": %llu, \"ext_split\": %llu, \"ext_merge\": %llu, \"bm_flushes\": %llu, "
		 "\"map_flush\": %llu, \"extents_end\": %llu, \"leaves_end\": %llu}",
		 f->parts, (unsigned long long)f->cache_pages, (unsigned long long)f->bm_cap, (unsigned long long)f->n_gc,
		 (unsigned long long)f->gc_valid_sum, (unsigned long long)f->ext_split, (unsigned long long)f->ext_merge,
		 (unsigned long long)f->bm_flushes, (unsigned long long)f->st->prog[DM_P_MAP],
		 (unsigned long long)f->n_extents, (unsigned long long)f->nleaves);
}

const struct dm_ftl_ops dm_policy = {
	.name = "mu", .create = mu_create, .destroy = mu_destroy, .write = mu_write, .read = mu_read,
	.flush = mu_flush, .mem_bytes = mu_mem, .verify = mu_verify, .report = mu_report,
};
