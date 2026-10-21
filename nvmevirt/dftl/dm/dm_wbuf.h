/* SPDX-License-Identifier: GPL-2.0-only */
/* Device write buffer shared by every policy: port of ftlsim Runner::wb_write (IU-coalescing LRU).
 * A fully covered IU is emitted as one aligned U-page write; when over capacity the LRU IU is emitted
 * as its contiguous runs; overwrites of buffered pages are absorbed. */
#ifndef DM_WBUF_H
#define DM_WBUF_H
#include "dm_port.h"

typedef void (*dm_emit_fn)(void *ctx, u64 lpn, u64 n);

struct dm_wbuf_ent { u64 iu; u32 bm; s32 prev, next; };
struct dm_wbuf {
	u32 U; u64 cap_pages; u64 pages;
	u32 hsize; s32 *htab;               /* open addressing, linear probing, -1 empty */
	struct dm_wbuf_ent *ent; s32 nent_max; s32 free_head;
	s32 mru, lru;                       /* doubly linked list over ent[] */
	dm_emit_fn emit; void *ctx;
	u64 *submitted;                     /* -> dm_stats.host_submitted */
};

int dm_wbuf_init(struct dm_wbuf *wb, u32 U, u64 cap_pages, dm_emit_fn emit, void *ctx, u64 *submitted);
void dm_wbuf_destroy(struct dm_wbuf *wb);
void dm_wbuf_write(struct dm_wbuf *wb, u64 lpn, u64 n);
void dm_wbuf_drain(struct dm_wbuf *wb);
int dm_wbuf_contains(struct dm_wbuf *wb, u64 lpn);
#endif
