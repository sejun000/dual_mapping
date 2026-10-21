// SPDX-License-Identifier: GPL-2.0-only
#include "dm_wbuf.h"

static u32 h_slot(struct dm_wbuf *wb, u64 iu) { return (u32)((iu * 0x9E3779B97F4A7C15ull) >> 32) & (wb->hsize - 1); }

static s32 h_find(struct dm_wbuf *wb, u64 iu)
{
	u32 s = h_slot(wb, iu);
	while (wb->htab[s] >= 0) { if (wb->ent[wb->htab[s]].iu == iu) return (s32)s; s = (s + 1) & (wb->hsize - 1); }
	return -1 - (s32)s;   /* not found: encode insertion slot */
}

static void h_del(struct dm_wbuf *wb, u32 s)   /* backward-shift deletion */
{
	u32 j = s;
	wb->htab[s] = -1;
	for (;;) {
		u32 k;
		j = (j + 1) & (wb->hsize - 1);
		if (wb->htab[j] < 0) return;
		k = h_slot(wb, wb->ent[wb->htab[j]].iu);
		if ((j > s && (k <= s || k > j)) || (j < s && (k <= s && k > j))) { wb->htab[s] = wb->htab[j]; wb->htab[j] = -1; s = j; }
	}
}

static void l_unlink(struct dm_wbuf *wb, s32 e)
{
	struct dm_wbuf_ent *x = &wb->ent[e];
	if (x->prev >= 0) wb->ent[x->prev].next = x->next; else wb->mru = x->next;
	if (x->next >= 0) wb->ent[x->next].prev = x->prev; else wb->lru = x->prev;
}
static void l_push_mru(struct dm_wbuf *wb, s32 e)
{
	wb->ent[e].prev = -1; wb->ent[e].next = wb->mru;
	if (wb->mru >= 0) wb->ent[wb->mru].prev = e; else wb->lru = e;
	wb->mru = e;
}

int dm_wbuf_init(struct dm_wbuf *wb, u32 U, u64 cap_pages, dm_emit_fn emit, void *ctx, u64 *submitted)
{
	s32 i;
	memset(wb, 0, sizeof(*wb));
	wb->U = U; wb->cap_pages = cap_pages; wb->emit = emit; wb->ctx = ctx; wb->submitted = submitted;
	wb->nent_max = (s32)cap_pages + 2;
	wb->hsize = 16; while (wb->hsize < 4u * (u32)wb->nent_max) wb->hsize <<= 1;
	wb->htab = dm_zalloc(sizeof(s32) * wb->hsize);
	wb->ent = dm_zalloc(sizeof(struct dm_wbuf_ent) * wb->nent_max);
	if (!wb->htab || !wb->ent) return -1;
	for (i = 0; i < (s32)wb->hsize; i++) wb->htab[i] = -1;
	for (i = 0; i < wb->nent_max; i++) wb->ent[i].next = i + 1 < wb->nent_max ? i + 1 : -1;
	wb->free_head = 0; wb->mru = wb->lru = -1;
	return 0;
}

void dm_wbuf_destroy(struct dm_wbuf *wb) { dm_free(wb->htab); dm_free(wb->ent); }

static void flush_runs(struct dm_wbuf *wb, u64 iu, u32 bm)
{
	u32 off = 0, U = wb->U;
	while (off < U) {
		u32 start;
		if (!(bm & (1u << off))) { off++; continue; }
		start = off; while (off < U && (bm & (1u << off))) off++;
		wb->emit(wb->ctx, iu * U + start, off - start);
	}
	wb->pages -= (u64)dm_popcount(bm);
}

/* remove entry e (hash slot s) and return it to the free list */
static void ent_release(struct dm_wbuf *wb, s32 e, u32 s)
{
	l_unlink(wb, e); h_del(wb, s);
	wb->ent[e].next = wb->free_head; wb->free_head = e;
}

static void evict_lru(struct dm_wbuf *wb)
{
	s32 e = wb->lru; u64 iu = wb->ent[e].iu; u32 bm = wb->ent[e].bm;
	s32 s = h_find(wb, iu);
	ent_release(wb, e, (u32)s);
	flush_runs(wb, iu, bm);
}

void dm_wbuf_write(struct dm_wbuf *wb, u64 lpn, u64 n)
{
	u64 cur = lpn, end = lpn + n; u32 U = wb->U;
	*wb->submitted += n;
	if (!wb->cap_pages) { wb->emit(wb->ctx, lpn, n); return; }
	while (cur < end) {
		u64 iu = cur / U; u32 off = (u32)(cur % U);
		u32 w = (u32)((end - cur) < (u64)(U - off) ? (end - cur) : (U - off));
		u32 bits = (w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1)) << off;
		s32 s = h_find(wb, iu), e; u32 old, nb;
		if (s < 0) {
			e = wb->free_head; if (e < 0) dm_die("wbuf: out of entries\n");
			wb->free_head = wb->ent[e].next;
			wb->ent[e].iu = iu; wb->ent[e].bm = 0;
			s = -1 - s; wb->htab[s] = e; l_push_mru(wb, e);
		} else { e = wb->htab[s]; l_unlink(wb, e); l_push_mru(wb, e); }
		old = wb->ent[e].bm; nb = old | bits;
		wb->pages += (u64)dm_popcount(nb) - (u64)dm_popcount(old);
		wb->ent[e].bm = nb;
		if ((u32)dm_popcount(nb) == U) { ent_release(wb, e, (u32)s); wb->emit(wb->ctx, iu * U, U); wb->pages -= U; }
		cur += w;
		while (wb->pages > wb->cap_pages && wb->lru >= 0) evict_lru(wb);
	}
}

void dm_wbuf_drain(struct dm_wbuf *wb) { while (wb->lru >= 0) evict_lru(wb); }

int dm_wbuf_contains(struct dm_wbuf *wb, u64 lpn)
{
	s32 s; if (!wb->cap_pages) return 0;
	s = h_find(wb, lpn / wb->U);
	return s >= 0 && (wb->ent[wb->htab[s]].bm >> (lpn % wb->U)) & 1;
}
