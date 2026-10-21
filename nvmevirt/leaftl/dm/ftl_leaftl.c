// SPDX-License-Identifier: GPL-2.0-only
/*
 * leaftl: LeaFTL (Sun et al., ASPLOS'23) learned-index page mapping, ported from ftlsim mode=leaftl
 * (struct Sim: is_pg()/is_lf() paths + the "LeaFTL" block after the DFTL CMT code).
 *
 * Data layout = page-level log structure, identical to page4k (LIFO free list block 0 first, separate host /
 * GC frontiers, GC when free <= T, greedy victim min valid / lowest id), except:
 *  - host pages are staged in an LPN-sorted batch (leaftl_batch, default 2048) before flash allocation; the
 *    batch never absorbs a rewrite (a duplicate LPN flushes the batch first);
 *  - GC relocates the victim's valid pages in LPN order (leaftl_gcsort) and relearns them;
 *  - the L2P table is a set of learned segments per 256-LPN frame (greedy PLR, error bound gamma,
 *    float16-rounded slope), cached in DRAM by frame with an LRU budget (cache_bytes).
 *
 * Floating point: the PLR must reproduce ftlsim's doubles bit for bit, including the places where the
 * reference binary (clang, -march=native) contracted a*b+c into an FMA. Every FP routine is a noinline
 * function with target("sse2,fma") and only runs between leaftl_fpu_begin()/end() (kernel_fpu_begin/end in
 * the kernel); all other code in this file is compiled general-regs-only (#pragma below) so the compiler can
 * never emit SSE/AVX instructions outside those sections. Doubles are stored as u64 bit patterns so non-FP
 * code only moves integers. No allocation / sleeping happens inside an FPU section.
 */
#pragma GCC target("general-regs-only")
#include "ftlcore.h"
#ifdef __KERNEL__
#include <asm/fpu/api.h>
#include <asm/cpufeature.h>
#define leaftl_fpu_begin() kernel_fpu_begin()
#define leaftl_fpu_end() kernel_fpu_end()
#else
#define leaftl_fpu_begin() do { } while (0)
#define leaftl_fpu_end() do { } while (0)
#endif

#define LEAFTL_FRAME 256
#define LEAFTL_NOSEG 0xFF
#define LEAFTL_NONE 0xFFFFFFFFu
#define LEAFTL_F_RES 1
#define LEAFTL_F_DIRTY 2
#define LEAFTL_F_PART 4
#define LEAFTL_F_EXIST 8
#define LEAFTL_NCLS 9                 /* pooled segment arrays: capacity 1 << cls, cls 0..8 (<= 256 segs/frame) */
#define LEAFTL_CHUNK (256u << 10)
#define LEAFTL_OOBC 16                /* artifact's last_oob_page FIFO */

/* learned segment; k/b are IEEE-754 double bit patterns, x1/x2 are frame offsets (first/last covered LPN).
 * Coverage lives in the per-LPN owner table (segments of one frame have disjoint coverage). */
struct leaftl_seg { u64 kb, bb; u8 x1, x2, acc, consec; };
struct leaftl_frame { struct leaftl_seg *segs; u32 prev, next; s32 mem; u16 nseg; u8 cls, fl; };
struct leaftl_chunk { struct leaftl_chunk *next; };
struct leaftl_frontier { s32 blk; u32 next; };

struct leaftl {
	struct dm_geom g; struct dm_flash_ops fo; struct dm_stats *st;
	u32 *pmap, *p2l; s32 *bvalid; u8 *bused; s32 *freelist; u32 nfree;
	struct leaftl_frontier hf, gf; int in_gc;
	/* learned table */
	u64 nfr; struct leaftl_frame *fr; u8 *own;       /* own[lpn] = segment index in its frame, NOSEG = none (see leaftl_own) */
	u32 lru_head, lru_tail, nres; s64 cached;         /* LRU of resident frames, head = MRU */
	u64 cache_bytes, gamma_u, gm_bits; u32 batch; int gcsort;
	u64 lookups, mispredict, oob_fail;
	/* staging batch + duplicate hash set */
	u32 *stage, nstage; u32 *hset; u32 hmask;
	u64 *fpairs, *gpairs; u32 *glpn;                  /* (lpn << 32 | ppn) pairs: flush batch / GC victim */
	/* insert scratch */
	struct leaftl_seg ns[LEAFTL_FRAME], tmp[LEAFTL_FRAME];
	u16 nstart[LEAFTL_FRAME + 1]; short first[LEAFTL_FRAME], last[LEAFTL_FRAME]; u16 remap[LEAFTL_FRAME], bnd[LEAFTL_FRAME];
	/* segment pool */
	void *pfree[LEAFTL_NCLS]; struct leaftl_chunk *chunks; u8 *cur; u32 cur_left;
	/* read path: artifact's last_oob_page FIFO */
	u64 oobc[LEAFTL_OOBC]; u32 noobc, oobc_pos, oobw;
};

/* ------------------------------------------------------------------ floating point (FPU sections only) */
#define LEAFTL_FPT __attribute__((target("sse2,fma")))
#define LEAFTL_FP __attribute__((target("sse2,fma"), __noinline__))
union leaftl_dbl { double d; u64 u; };
static inline LEAFTL_FPT double leaftl_u2d(u64 u) { union leaftl_dbl x; x.u = u; return x.d; }
static inline LEAFTL_FPT u64 leaftl_d2u(double d) { union leaftl_dbl x; x.d = d; return x.u; }
/* std::llround (half away from zero); v - trunc(v) is exact for |v| < 2^63 */
static inline LEAFTL_FPT s64 leaftl_llround(double v)
{
	s64 t = (s64)v; double d = v - (double)t;
	if (d >= 0.5) t++; else if (d <= -0.5) t--;
	return t;
}
/* ftlsim f16(): ldexp(nearbyint(ldexp(frexp(v), 11)), e - 11) = round the significand to 11 bits, ties-to-even */
static inline LEAFTL_FPT double leaftl_f16n(double v)
{
	union leaftl_dbl x; u64 lo, half = 1ull << 41, mask = (1ull << 42) - 1;
	x.d = v;
	lo = x.u & mask; x.u &= ~mask;
	if (lo > half || (lo == half && ((x.u >> 42) & 1))) x.u += 1ull << 42;   /* carry into the exponent is correct */
	return x.d;
}
static inline LEAFTL_FPT double leaftl_f16(double v)
{
	u32 e;
	if (v == 0) return 0;
	e = (u32)(leaftl_d2u(v) >> 52) & 0x7ff;
	if (e == 0x7ff) return v;                                                 /* inf / nan pass through */
	if (e == 0) return leaftl_f16n(v * 18446744073709551616.0) * (1.0 / 18446744073709551616.0);  /* subnormal */
	return leaftl_f16n(v);
}
static LEAFTL_FP u64 leaftl_gamma_bits(u64 gamma_u) { return leaftl_d2u((double)(s64)gamma_u / 1000000.0); }

/* lf_emit: points [a, b) of pairs; state 1 = single-point form (k = 1, b = y0 - x0) */
static inline LEAFTL_FPT void leaftl_emit(struct leaftl *f, int *nseg, const u64 *pp, int a, int b, int state,
					   double k, double bb, u32 base)
{
	struct leaftl_seg *g = &f->ns[*nseg]; int i;
	u64 d01 = b - a >= 2 ? (pp[a + 1] >> 32) - (pp[a] >> 32) : 0;
	if (state == 1) { k = 1; bb = (double)(s64)(u32)pp[a] - (double)(s64)(pp[a] >> 32); }
	g->kb = leaftl_d2u(k); g->bb = leaftl_d2u(bb);
	g->x1 = (u8)((pp[a] >> 32) - base); g->x2 = (u8)((pp[b - 1] >> 32) - base);
	g->acc = 1; g->consec = 1;
	for (i = a; i < b; i++) {
		double x = (double)(s64)(pp[i] >> 32);
		if (leaftl_llround(__builtin_fma(x, k, bb)) != (s64)(u32)pp[i]) g->acc = 0;
		if (i - a >= 2 && (pp[i] >> 32) - (pp[i - 1] >> 32) != d01) g->consec = 0;
	}
	f->nstart[*nseg] = (u16)a;
	(*nseg)++;
}

/* lf_learn: greedy PLR state machine (artifact PLR.process/build_segment). FMA placement mirrors the reference
 * binary: b = fma(-k, x1, y1); sy = fma(up.k, lo.b, -(lo.k * up.b)) / (up.k - lo.k); bound tests fma(x, k, b). */
static LEAFTL_FP int leaftl_learn(struct leaftl *f, const u64 *pp, int n, u32 base)
{
	const double gm = leaftl_u2d(f->gm_bits);
	int state = 0, i, cs = 0, nseg = 0;
	double s0x = 0, s0y = 0, sx = 0, sy = 0, lok = 0, lob = 0, upk = 0, upb = 0;
	for (i = 0; i < n; i++) {
		double x = (double)(s64)(pp[i] >> 32), y = (double)(s64)(u32)pp[i];
		if (state == 0) { s0x = x; s0y = y; state = 1; }
		else if (state == 1) {
			double ly = s0y + gm, uy = s0y - gm, dx = x - s0x;
			lok = ((y - gm) - ly) / dx; lob = __builtin_fma(-lok, s0x, ly);
			upk = ((y + gm) - uy) / dx; upb = __builtin_fma(-upk, s0x, uy);
			sx = (lob - upb) / (upk - lok);
			sy = __builtin_fma(upk, lob, -(lok * upb)) / (upk - lok);
			state = 2;
		} else {
			double lv = __builtin_fma(x, lok, lob), uv = __builtin_fma(x, upk, upb);
			if (!(y > lv) || !(y < uv)) {
				double k = leaftl_f16((lok + upk) * 0.5);
				leaftl_emit(f, &nseg, pp, cs, i, 2, k, __builtin_fma(-k, sx, sy), base);
				cs = i; s0x = x; s0y = y; state = 1;
			} else {
				double yu = y + gm, yl = y - gm;
				if (yu < uv) { upk = (yu - sy) / (x - sx); upb = __builtin_fma(-upk, sx, sy); }
				if (yl > lv) { lok = (yl - sy) / (x - sx); lob = __builtin_fma(-lok, sx, sy); }
			}
		}
	}
	if (state == 1) leaftl_emit(f, &nseg, pp, cs, n, 1, 0, 0, base);
	else if (state == 2) { double k = leaftl_f16((lok + upk) * 0.5); leaftl_emit(f, &nseg, pp, cs, n, 2, k, __builtin_fma(-k, sx, sy), base); }
	f->nstart[nseg] = (u16)n;
	return nseg;
}

/* predicted ppn of lpn under segment g */
static LEAFTL_FP s64 leaftl_predict_fp(const struct leaftl_seg *g, u64 lpn)
{
	return leaftl_llround(__builtin_fma((double)(s64)lpn, leaftl_u2d(g->kb), leaftl_u2d(g->bb)));
}

/* lf_verify over [l0, l1): predictions of every mapped LPN against pmap */
static LEAFTL_FP void leaftl_verify_fp(struct leaftl *f, u64 l0, u64 l1, u64 *bad, u64 *miss, u64 *approx)
{
	u64 l;
	for (l = l0; l < l1; l++) {
		const struct leaftl_frame *fr; u8 o; s64 pr;
		if (f->pmap[l] == LEAFTL_NONE) continue;
		fr = &f->fr[l / LEAFTL_FRAME]; o = f->own[l];
		if (o == LEAFTL_NOSEG && fr->nseg < LEAFTL_FRAME) { (*miss)++; continue; }
		pr = leaftl_llround(__builtin_fma((double)(s64)l, leaftl_u2d(fr->segs[o].kb), leaftl_u2d(fr->segs[o].bb)));
		if (pr != (s64)f->pmap[l]) { if (fr->segs[o].acc) (*bad)++; else (*approx)++; }
	}
}

/* ------------------------------------------------------------------ segment pool (no FP below this line) */
static struct leaftl_seg *leaftl_pool_get(struct leaftl *f, int cls)
{
	u32 sz = (u32)sizeof(struct leaftl_seg) << cls; void *p = f->pfree[cls];
	if (p) { f->pfree[cls] = *(void **)p; return p; }
	if (f->cur_left < sz) {
		struct leaftl_chunk *c = dm_zalloc(LEAFTL_CHUNK);
		if (!c) dm_die("leaftl: pool alloc failed\n");
		c->next = f->chunks; f->chunks = c;
		f->cur = (u8 *)c + 64; f->cur_left = LEAFTL_CHUNK - 64;
	}
	p = f->cur; f->cur += sz; f->cur_left -= sz;
	return p;
}
static void leaftl_pool_put(struct leaftl *f, struct leaftl_seg *p, int cls)
{
	*(void **)p = f->pfree[cls]; f->pfree[cls] = p;
}
static int leaftl_cls(u32 n) { int c = 0; while ((1u << c) < n) c++; return c; }

/* owner lookup: NOSEG means "no segment" unless the frame holds 256 segments (then every LPN is covered) */
static inline int leaftl_own(const struct leaftl *f, const struct leaftl_frame *fr, u64 lpn)
{
	u8 o = f->own[lpn];
	return (o == LEAFTL_NOSEG && fr->nseg < LEAFTL_FRAME) ? -1 : (int)o;
}

/* ------------------------------------------------------------------ frame cache (LRU, head = MRU) */
static void leaftl_lru_unlink(struct leaftl *f, u32 fno)
{
	struct leaftl_frame *x = &f->fr[fno];
	if (x->prev != LEAFTL_NONE) f->fr[x->prev].next = x->next; else f->lru_head = x->next;
	if (x->next != LEAFTL_NONE) f->fr[x->next].prev = x->prev; else f->lru_tail = x->prev;
	x->prev = x->next = LEAFTL_NONE;
}
static void leaftl_lru_push(struct leaftl *f, u32 fno)
{
	struct leaftl_frame *x = &f->fr[fno];
	x->prev = LEAFTL_NONE; x->next = f->lru_head;
	if (f->lru_head != LEAFTL_NONE) f->fr[f->lru_head].prev = fno; else f->lru_tail = fno;
	f->lru_head = fno;
}
static void leaftl_map_read(struct leaftl *f) { f->st->read[DM_R_MAP]++; f->fo.read(f->fo.ctx, DM_NOPPN, DM_R_MAP); }

/* lf_make_resident */
static void leaftl_make_resident(struct leaftl *f, u32 fno, int load)
{
	struct leaftl_frame *x = &f->fr[fno];
	if (x->fl & LEAFTL_F_RES) { leaftl_lru_unlink(f, fno); leaftl_lru_push(f, fno); return; }
	if (load) { f->st->map_miss++; leaftl_map_read(f); x->fl &= ~LEAFTL_F_PART; }
	else if (x->nseg) x->fl |= LEAFTL_F_PART;          /* update without loading: merged (read) at eviction */
	x->fl |= LEAFTL_F_RES; leaftl_lru_push(f, fno); f->nres++; f->cached += x->mem;
}

/* lf_enforce */
static void leaftl_enforce(struct leaftl *f)
{
	while (f->cached > (s64)f->cache_bytes && f->nres > 1) {
		u32 v = f->lru_tail; struct leaftl_frame *x = &f->fr[v];
		leaftl_lru_unlink(f, v); f->nres--;
		if (x->fl & LEAFTL_F_DIRTY) {
			if (x->fl & LEAFTL_F_PART) leaftl_map_read(f);
			f->st->prog[DM_P_MAP]++; f->fo.prog(f->fo.ctx, DM_NOPPN, DM_P_MAP);
		}
		x->fl &= ~(LEAFTL_F_RES | LEAFTL_F_DIRTY | LEAFTL_F_PART); f->cached -= x->mem;
	}
}

/* lf_touch_lookup: mapping lookup on the write path (invalidate the old copy) or a read */
static void leaftl_touch_lookup(struct leaftl *f, u64 lpn)
{
	u32 fno = (u32)(lpn / LEAFTL_FRAME); struct leaftl_frame *x = &f->fr[fno];
	x->fl |= LEAFTL_F_EXIST; f->lookups++;
	if (!(x->fl & LEAFTL_F_RES)) leaftl_make_resident(f, fno, 1);
	else {
		if (x->fl & LEAFTL_F_PART) { f->st->map_miss++; leaftl_map_read(f); x->fl &= ~LEAFTL_F_PART; }
		leaftl_make_resident(f, fno, 0);
	}
}

/* lf_set_mem: 4 B/frame + per segment 5 B (single point), 8 B (consecutive), 8 B + boundary runs */
static void leaftl_set_mem(struct leaftl *f, u32 fno)
{
	struct leaftl_frame *x = &f->fr[fno]; u64 base = (u64)fno * LEAFTL_FRAME; s32 m = 4; int i, s;
	for (s = 0; s < x->nseg; s++) f->bnd[s] = 0;
	for (i = 1; i < LEAFTL_FRAME; i++) {                 /* transitions of seg s's coverage in (x1, x2) exclusive of x2 */
		int a = leaftl_own(f, x, base + i - 1), b = leaftl_own(f, x, base + i);
		if (a == b) continue;
		if (a >= 0 && i >= x->segs[a].x1 + 1 && i <= x->segs[a].x2 - 1) f->bnd[a]++;
		if (b >= 0 && i >= x->segs[b].x1 + 1 && i <= x->segs[b].x2 - 1) f->bnd[b]++;
	}
	for (s = 0; s < x->nseg; s++) {
		const struct leaftl_seg *g = &x->segs[s];
		if (g->x1 == g->x2) m += 5;
		else if (g->consec) m += 8;
		else m += 8 + f->bnd[s];
	}
	if (x->fl & LEAFTL_F_RES) f->cached += m - x->mem;
	x->mem = m;
}

/* lf_insert: pairs sorted by lpn, all current */
static void leaftl_insert(struct leaftl *f, const u64 *pp, int n)
{
	int i = 0;
	while (i < n) {
		u32 fno = (u32)((pp[i] >> 32) / LEAFTL_FRAME), base = fno * LEAFTL_FRAME; int j = i, nn, s, r, t, cls;
		struct leaftl_frame *x = &f->fr[fno]; u8 *own = f->own + base; u64 nc[4] = {0, 0, 0, 0};
		while (j < n && (pp[j] >> 32) / LEAFTL_FRAME == fno) j++;
		leaftl_fpu_begin();
		nn = leaftl_learn(f, pp + i, j - i, base);
		leaftl_fpu_end();
		x->fl |= LEAFTL_F_EXIST;
		leaftl_make_resident(f, fno, 0);
		for (t = i; t < j; t++) { r = (int)((pp[t] >> 32) - base); nc[r >> 6] |= 1ull << (r & 63); }
		/* older segments lose the coverage the new ones shadow */
		for (s = 0; s < x->nseg; s++) f->first[s] = -1;
		for (r = 0; r < LEAFTL_FRAME; r++) {
			int o;
			if ((nc[r >> 6] >> (r & 63)) & 1) continue;
			o = leaftl_own(f, x, base + r);
			if (o < 0) continue;
			if (f->first[o] < 0) f->first[o] = (short)r;
			f->last[o] = (short)r;
		}
		for (s = 0; s < nn; s++) f->tmp[s] = f->ns[s];
		t = nn;
		for (s = 0; s < x->nseg; s++) {
			if (f->first[s] < 0) continue;
			f->tmp[t] = x->segs[s]; f->tmp[t].x1 = (u8)f->first[s]; f->tmp[t].x2 = (u8)f->last[s];
			f->remap[s] = (u16)t++;
		}
		if (t > LEAFTL_FRAME) dm_die("leaftl: frame %u has %d segments\n", fno, t);
		for (r = 0; r < LEAFTL_FRAME; r++) {
			int o;
			if ((nc[r >> 6] >> (r & 63)) & 1) continue;
			o = leaftl_own(f, x, base + r);
			own[r] = o < 0 ? LEAFTL_NOSEG : (u8)f->remap[o];
		}
		for (s = 0; s < nn; s++)
			for (r = f->nstart[s]; r < f->nstart[s + 1]; r++) own[(pp[i + r] >> 32) - base] = (u8)s;
		cls = leaftl_cls((u32)t);
		if (!x->segs || cls > x->cls || cls + 1 < x->cls) {
			if (x->segs) leaftl_pool_put(f, x->segs, x->cls);
			x->segs = leaftl_pool_get(f, cls); x->cls = (u8)cls;
		}
		memcpy(x->segs, f->tmp, sizeof(struct leaftl_seg) * t);
		x->nseg = (u16)t; x->fl |= LEAFTL_F_DIRTY;
		leaftl_set_mem(f, fno);
		i = j;
	}
	leaftl_enforce(f);
}

/* ------------------------------------------------------------------ page-mapped data path (as page4k) */
static void leaftl_erase_block(struct leaftl *f, u32 b)
{
	if (f->bvalid[b] != 0) dm_die("erase block %u with valid=%d\n", b, f->bvalid[b]);
	f->bused[b] = 0;
	f->freelist[f->nfree++] = (s32)b;
	f->st->erases++;
	f->fo.erase(f->fo.ctx, b);
}
static int leaftl_is_frontier(struct leaftl *f, u32 b) { return (s32)b == f->hf.blk || (s32)b == f->gf.blk; }
static s32 leaftl_pick_victim(struct leaftl *f)
{
	s32 best = -1, bv = 0x7fffffff; u32 b;
	for (b = 0; b < f->g.NB; b++) {
		if (!f->bused[b] || leaftl_is_frontier(f, b)) continue;
		if (f->bvalid[b] < bv) { bv = f->bvalid[b]; best = (s32)b; }
	}
	return best;
}
static void leaftl_page_write(struct leaftl *f, u64 lpn, int src);
static int leaftl_cmp_u32(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }
static int leaftl_cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }

/* page_gc, is_lf() branch: valid pages found via OOB, rewritten in LPN order and relearned */
static void leaftl_page_gc(struct leaftl *f)
{
	s32 b = leaftl_pick_victim(f); u32 s, base, n = 0, i, k = 0;
	if (b < 0) dm_die("leaftl: no victim\n");
	f->in_gc = 1;
	base = (u32)b * f->g.P;
	for (s = 0; s < f->g.P; s++) { u32 ppn = base + s, lpn = f->p2l[ppn]; if (lpn != LEAFTL_NONE && f->pmap[lpn] == ppn) f->glpn[n++] = lpn; }
	if (f->gcsort) dm_sort(f->glpn, n, sizeof(u32), leaftl_cmp_u32);
	for (i = 0; i < n; i++) {
		u32 lpn = f->glpn[i];
		f->st->read[DM_R_GC]++; f->fo.read(f->fo.ctx, f->pmap[lpn], DM_R_GC);
		leaftl_page_write(f, lpn, DM_P_GC);
		f->gpairs[i] = ((u64)lpn << 32) | f->pmap[lpn];
	}
	if (f->bvalid[b] != 0) dm_die("leaftl gc left valid=%d\n", f->bvalid[b]);
	leaftl_erase_block(f, (u32)b);
	f->in_gc = 0;
	for (i = 0; i < n; i++) if (f->pmap[f->gpairs[i] >> 32] == (u32)f->gpairs[i]) f->gpairs[k++] = f->gpairs[i];
	if (!f->gcsort) dm_sort(f->gpairs, k, sizeof(u64), leaftl_cmp_u64);
	leaftl_insert(f, f->gpairs, (int)k);
}

/* alloc_block (is_pg): GC while free <= T, with ftlsim's dead-block escape when a GC round frees no net block */
static u32 leaftl_alloc_block(struct leaftl *f)
{
	s32 b;
	if (!f->in_gc) {
		int stall = 0;
		while (f->nfree <= f->g.T) {
			u32 before = f->nfree;
			leaftl_page_gc(f);
			if (f->nfree <= before) {
				u32 d; s32 dead = -1;
				stall++;
				for (d = 0; d < f->g.NB; d++)
					if (f->bused[d] && !leaftl_is_frontier(f, d) && f->bvalid[d] == 0) { dead = (s32)d; break; }
				if (dead >= 0) { leaftl_erase_block(f, (u32)dead); stall = 0; continue; }
				if (stall > 64) dm_die("leaftl: GC makes no progress\n");
			} else stall = 0;
		}
	}
	if (!f->nfree) dm_die("free list exhausted\n");
	b = f->freelist[--f->nfree];
	f->bused[b] = 1; f->bvalid[b] = 0;
	return (u32)b;
}
static u32 leaftl_alloc_page(struct leaftl *f, int gc)
{
	struct leaftl_frontier *fr = gc ? &f->gf : &f->hf;
	if (fr->blk < 0 || fr->next == f->g.P) { fr->blk = (s32)leaftl_alloc_block(f); fr->next = 0; }
	return (u32)fr->blk * f->g.P + fr->next++;
}
static void leaftl_page_write(struct leaftl *f, u64 lpn, int src)
{
	u32 ppn = leaftl_alloc_page(f, src != DM_P_HOST), old;
	f->p2l[ppn] = (u32)lpn;
	f->bvalid[ppn / f->g.P]++;
	f->st->prog[src]++; f->fo.prog(f->fo.ctx, ppn, src);
	old = f->pmap[lpn];
	if (old != LEAFTL_NONE) f->bvalid[old / f->g.P]--;
	f->pmap[lpn] = ppn;
}

/* ------------------------------------------------------------------ staging batch */
static u32 leaftl_hash(struct leaftl *f, u32 lpn) { return (u32)(((u64)lpn * 0x9E3779B97F4A7C15ull) >> 32) & f->hmask; }
static int leaftl_staged(struct leaftl *f, u32 lpn)
{
	u32 h = leaftl_hash(f, lpn);
	while (f->hset[h] != LEAFTL_NONE) { if (f->hset[h] == lpn) return 1; h = (h + 1) & f->hmask; }
	return 0;
}
static void leaftl_stage_add(struct leaftl *f, u32 lpn)
{
	u32 h = leaftl_hash(f, lpn);
	while (f->hset[h] != LEAFTL_NONE) h = (h + 1) & f->hmask;
	f->hset[h] = lpn; f->stage[f->nstage++] = lpn;
	f->st->staged = f->nstage;
}
/* lf_flush_stage */
static void leaftl_flush_stage(struct leaftl *f)
{
	u32 i, n = f->nstage, k = 0;
	if (!n) return;
	dm_sort(f->stage, n, sizeof(u32), leaftl_cmp_u32);
	for (i = 0; i < n; i++) {
		u32 lpn = f->stage[i];
		if (f->pmap[lpn] != LEAFTL_NONE) leaftl_touch_lookup(f, lpn);
		leaftl_page_write(f, lpn, DM_P_HOST);
		f->fpairs[i] = ((u64)lpn << 32) | f->pmap[lpn];
	}
	f->nstage = 0; memset(f->hset, 0xff, sizeof(u32) * (f->hmask + 1));
	f->st->staged = 0; f->st->stage_flushes++;
	for (i = 0; i < n; i++) if (f->pmap[f->fpairs[i] >> 32] == (u32)f->fpairs[i]) f->fpairs[k++] = f->fpairs[i];   /* GC may have moved some */
	leaftl_insert(f, f->fpairs, (int)k);
}
/* lf_host: never absorb a rewrite inside the sort batch */
static void leaftl_host(struct leaftl *f, u32 lpn)
{
	if (leaftl_staged(f, lpn)) leaftl_flush_stage(f);
	leaftl_stage_add(f, lpn);
	if (f->nstage >= f->batch) leaftl_flush_stage(f);
}

/* ------------------------------------------------------------------ ops */
static void leaftl_destroy(void *c);
static void *leaftl_create(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params)
{
	struct leaftl *f = dm_zalloc(sizeof(*f)); u64 i, np = (u64)g->NB * g->P, v, hs;
	if (!f) return NULL;
#ifdef __KERNEL__
	if (!boot_cpu_has(X86_FEATURE_FMA) || !boot_cpu_has(X86_FEATURE_AVX)) { dm_log("leaftl: CPU lacks FMA/AVX\n"); dm_free(f); return NULL; }
#else
	if (!__builtin_cpu_supports("fma")) { dm_log("leaftl: CPU lacks FMA\n"); dm_free(f); return NULL; }
#endif
	f->g = *g; f->fo = *fo; f->st = st;
	f->cache_bytes = 12ull << 20; f->gamma_u = 100; f->batch = 2048; f->gcsort = 1;   /* ftlsim defaults */
	if (dm_param_u64(params, "cache_bytes", &v)) f->cache_bytes = v;
	if (dm_param_u64(params, "leaftl_gamma_u", &v)) f->gamma_u = v;
	if (dm_param_u64(params, "leaftl_batch", &v) && v > 0) f->batch = (u32)v;
	if (dm_param_u64(params, "leaftl_gcsort", &v)) f->gcsort = v != 0;
	leaftl_fpu_begin();
	f->gm_bits = leaftl_gamma_bits(f->gamma_u);
	leaftl_fpu_end();
	v = 2 * f->gamma_u / 1000000; f->oobw = (u32)(v < 32 ? v : 32);        /* artifact num_p2l_entries = min(2*gamma, 128/4) */
	f->nfr = (g->L + LEAFTL_FRAME - 1) / LEAFTL_FRAME;
	for (hs = 1; hs < 4ull * f->batch; hs <<= 1) ;
	f->hmask = (u32)(hs - 1);
	f->pmap = dm_zalloc(sizeof(u32) * g->L); f->p2l = dm_zalloc(sizeof(u32) * np);
	f->bvalid = dm_zalloc(sizeof(s32) * g->NB); f->bused = dm_zalloc(g->NB); f->freelist = dm_zalloc(sizeof(s32) * g->NB);
	f->fr = dm_zalloc(sizeof(struct leaftl_frame) * f->nfr); f->own = dm_zalloc(f->nfr * LEAFTL_FRAME);
	f->stage = dm_zalloc(sizeof(u32) * f->batch); f->hset = dm_zalloc(sizeof(u32) * hs);
	f->fpairs = dm_zalloc(sizeof(u64) * f->batch); f->gpairs = dm_zalloc(sizeof(u64) * g->P); f->glpn = dm_zalloc(sizeof(u32) * g->P);
	if (!f->pmap || !f->p2l || !f->bvalid || !f->bused || !f->freelist || !f->fr || !f->own || !f->stage || !f->hset ||
	    !f->fpairs || !f->gpairs || !f->glpn) { dm_log("leaftl: alloc failed\n"); leaftl_destroy(f); return NULL; }
	for (i = 0; i < g->L; i++) f->pmap[i] = LEAFTL_NONE;
	for (i = 0; i < np; i++) f->p2l[i] = LEAFTL_NONE;
	for (i = 0; i < g->NB; i++) f->freelist[i] = (s32)(g->NB - 1 - i);
	for (i = 0; i < f->nfr; i++) f->fr[i].prev = f->fr[i].next = LEAFTL_NONE;
	memset(f->own, LEAFTL_NOSEG, f->nfr * LEAFTL_FRAME);
	memset(f->hset, 0xff, sizeof(u32) * hs);
	f->lru_head = f->lru_tail = LEAFTL_NONE;
	f->nfree = g->NB; f->hf.blk = f->gf.blk = -1;
	return f;
}

static void leaftl_destroy(void *c)
{
	struct leaftl *f = c; struct leaftl_chunk *k = f->chunks;
	while (k) { struct leaftl_chunk *n = k->next; dm_free(k); k = n; }
	dm_free(f->pmap); dm_free(f->p2l); dm_free(f->bvalid); dm_free(f->bused); dm_free(f->freelist);
	dm_free(f->fr); dm_free(f->own); dm_free(f->stage); dm_free(f->hset); dm_free(f->fpairs); dm_free(f->gpairs); dm_free(f->glpn);
	dm_free(f);
}

static void leaftl_write(void *c, u64 lpn, u64 n)
{
	struct leaftl *f = c; u64 p;
	f->st->host_pages += n;
	for (p = lpn; p < lpn + n; p++) leaftl_host(f, (u32)p);
}

static void leaftl_rd(struct leaftl *f, u64 ppn, int src)
{
	u64 np = (u64)f->g.NB * f->g.P;
	f->st->read[src]++; f->fo.read(f->fo.ctx, ppn < np ? ppn : np - 1, src);
}

/* Host read (ftlsim has none). Staged pages are served from the staging buffer. Otherwise: frame lookup
 * (flash-resident or partial frame -> one translation read, then lf_enforce), predict from the covering
 * segment. Accurate segment -> one data read. Approximate segment -> artifact FlashMetadata.lpn_to_ppn:
 * if the true ppn is within the predicted page's OOB window W = min(2*gamma, 32): 1 read when a recently
 * read OOB page (16-entry FIFO) already covers it or the prediction is right, else OOB read of the
 * predicted page + data read; the predicted page enters the FIFO. Outside the window: read the predicted
 * page, then the neighbouring block's boundary page (previous block's last page if the prediction is in the
 * first half of its block, else next block's first page), then the data page unless it was that boundary page.
 * pmap stands in for the OOB contents (ground truth). */
static void leaftl_read(void *c, u64 lpn, u64 n)
{
	struct leaftl *f = c; u64 l;
	for (l = lpn; l < lpn + n; l++) {
		u32 fno = (u32)(l / LEAFTL_FRAME); struct leaftl_frame *x = &f->fr[fno]; const struct leaftl_seg *g;
		s64 pr, real; int o;
		if (leaftl_staged(f, (u32)l)) continue;
		if (!(x->fl & LEAFTL_F_EXIST)) continue;            /* no GTD entry: never written */
		leaftl_touch_lookup(f, l);
		leaftl_enforce(f);
		o = leaftl_own(f, x, l);
		if (o < 0) continue;                                 /* not covered: unmapped */
		g = &x->segs[o];
		leaftl_fpu_begin();
		pr = leaftl_predict_fp(g, l);
		leaftl_fpu_end();
		real = (s64)f->pmap[l];
		if (g->acc) { if (pr != real) f->oob_fail++; leaftl_rd(f, (u64)real, DM_R_HOST); continue; }
		if (pr != real) f->mispredict++;
		if ((real > pr ? real - pr : pr - real) <= (s64)f->oobw) {
			u32 i; int cached = 0;
			for (i = 0; i < f->noobc; i++) { s64 d = real - (s64)f->oobc[i]; if ((d < 0 ? -d : d) <= (s64)f->oobw) { cached = 1; break; } }
			if (!cached && pr != real) leaftl_rd(f, (u64)(pr < 0 ? 0 : pr), DM_R_OTHER);
			leaftl_rd(f, (u64)real, DM_R_HOST);
			f->oobc[f->oobc_pos] = (u64)pr; f->oobc_pos = (f->oobc_pos + 1) % LEAFTL_OOBC;
			if (f->noobc < LEAFTL_OOBC) f->noobc++;
		} else {
			s64 P = f->g.P, nb = (pr % P) * 2 < P ? (pr / P) * P - 1 : (pr / P + 1) * P, d;
			leaftl_rd(f, (u64)(pr < 0 ? 0 : pr), DM_R_OTHER);
			if (nb == real) { leaftl_rd(f, (u64)nb, DM_R_HOST); continue; }
			leaftl_rd(f, (u64)(nb < 0 ? 0 : nb), DM_R_OTHER);
			d = real - nb; if ((d < 0 ? -d : d) > (s64)f->oobw) f->oob_fail++;   /* artifact asserts here */
			leaftl_rd(f, (u64)real, DM_R_HOST);
		}
	}
}

static void leaftl_flush(void *c) { leaftl_flush_stage(c); }
static u64 leaftl_mem(void *c) { struct leaftl *f = c; return (u64)f->cached + f->nfr * 4; }   /* resident frames + GTD */

static int leaftl_verify(void *c)
{
	struct leaftl *f = c; u64 l, bad = 0, miss = 0, approx = 0, fno; u32 b, nres = 0, cnt; s64 cached = 0; int err = 0;
	s32 *v = dm_zalloc(sizeof(s32) * f->g.NB);
	if (!v) return 1;
	for (l = 0; l < f->g.L; l++) if (f->pmap[l] != LEAFTL_NONE) v[f->pmap[l] / f->g.P]++;
	for (b = 0; b < f->g.NB; b++) if (v[b] != f->bvalid[b]) { dm_log("leaftl verify: block %u valid %d != %d\n", b, v[b], f->bvalid[b]); err = 1; break; }
	dm_free(v);
	/* per frame: owner table vs segment extents, memory model, unmapped LPNs uncovered */
	for (fno = 0; fno < f->nfr && !err; fno++) {
		struct leaftl_frame *x = &f->fr[fno]; u64 base = fno * LEAFTL_FRAME; int r, s; s32 m = x->mem;
		for (s = 0; s < x->nseg; s++) f->first[s] = -1;
		for (r = 0; r < LEAFTL_FRAME && base + r < f->g.L; r++) {
			int o = leaftl_own(f, x, base + r);
			if (o >= x->nseg) { err = 1; break; }
			if (o < 0) { if (f->pmap[base + r] != LEAFTL_NONE) miss++; continue; }
			if (f->pmap[base + r] == LEAFTL_NONE) { dm_log("leaftl verify: unmapped lpn %llu covered\n", (unsigned long long)(base + r)); err = 1; break; }
			if (f->first[o] < 0) f->first[o] = (short)r;
			f->last[o] = (short)r;
		}
		for (s = 0; s < x->nseg && !err; s++)
			if (f->first[s] != x->segs[s].x1 || f->last[s] != x->segs[s].x2) { dm_log("leaftl verify: frame %llu seg %d extent\n", (unsigned long long)fno, s); err = 1; }
		if (err) break;
		if (x->nseg) { s64 save = f->cached; leaftl_set_mem(f, (u32)fno); f->cached = save; if (x->mem != m) { dm_log("leaftl verify: frame mem\n"); err = 1; } x->mem = m; }
		if (x->fl & LEAFTL_F_RES) { nres++; cached += x->mem; }
	}
	for (cnt = 0, b = f->lru_head; b != LEAFTL_NONE; b = f->fr[b].next) cnt++;
	if (!err && (nres != f->nres || cnt != nres || cached != f->cached)) { dm_log("leaftl verify: cache accounting\n"); err = 1; }
	for (l = 0; l < f->g.L && !err; l += 65536) {           /* predictions, in bounded FPU sections */
		u64 e = l + 65536 < f->g.L ? l + 65536 : f->g.L;
		leaftl_fpu_begin();
		leaftl_verify_fp(f, l, e, &bad, &miss, &approx);
		leaftl_fpu_end();
	}
	if (bad || miss) { dm_log("leaftl verify: %llu wrong accurate predictions, %llu uncovered LPNs\n", (unsigned long long)bad, (unsigned long long)miss); err = 1; }
	if (!err) dm_log("[verify] leaftl ok: approx-mispredict=%llu resident=%u cached=%lld\n", (unsigned long long)approx, f->nres, (long long)f->cached);
	return err;
}

static void leaftl_report(void *c, char *buf, int len)
{
	struct leaftl *f = c; u64 fno, full = 0, nseg = 0, nacc = 0, ncons = 0, nfr = 0; int s;
	for (fno = 0; fno < f->nfr; fno++) {
		struct leaftl_frame *x = &f->fr[fno];
		if (!(x->fl & LEAFTL_F_EXIST)) continue;
		nfr++; full += (u64)x->mem;
		for (s = 0; s < x->nseg; s++) { nseg++; nacc += x->segs[s].acc; ncons += x->segs[s].consec; }
	}
	snprintf(buf, len, "\"leaftl\": {\"full_table_bytes\": %llu, \"segments\": %llu, \"accurate\": %llu, \"consecutive\": %llu, "
		 "\"frames\": %llu, \"lookups\": %llu, \"gamma_u\": %llu, \"batch\": %u, \"resident\": %u, \"cached_bytes\": %lld, "
		 "\"read_mispredict\": %llu, \"read_oob_fail\": %llu}",
		 (unsigned long long)full, (unsigned long long)nseg, (unsigned long long)nacc, (unsigned long long)ncons,
		 (unsigned long long)nfr, (unsigned long long)f->lookups, (unsigned long long)f->gamma_u, f->batch, f->nres,
		 (long long)f->cached, (unsigned long long)f->mispredict, (unsigned long long)f->oob_fail);
}

const struct dm_ftl_ops dm_policy = {
	.name = "leaftl", .create = leaftl_create, .destroy = leaftl_destroy, .write = leaftl_write, .read = leaftl_read,
	.flush = leaftl_flush, .mem_bytes = leaftl_mem, .verify = leaftl_verify, .report = leaftl_report,
};
