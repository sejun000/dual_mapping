/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTL core interface. A core owns mapping + GC policy only; every NAND operation goes out through
 * dm_flash_ops, which the kernel glue (dm_ftl.c) turns into NVMeVirt timing and the userspace harness
 * turns into counters. One policy per build: each ftl_<policy>.c defines `const struct dm_ftl_ops dm_policy`.
 *
 * Geometry: L logical 4 KiB pages; NB blocks of P pages (a block == an NVMeVirt line / superblock);
 * physical page number ppn = blk * P + offset, offsets are written in increasing order within a block.
 * Host writes reach the core AFTER the shared device write buffer (dm_wbuf, same as ftlsim's Runner wb).
 */
#ifndef DM_FTLCORE_H
#define DM_FTLCORE_H
#include "dm_port.h"

enum dm_psrc { DM_P_HOST = 0, DM_P_GC, DM_P_RMW, DM_P_FOLD, DM_P_MAP, DM_P_OTHER, DM_P_N };   /* program sources */
enum dm_rsrc { DM_R_HOST = 0, DM_R_GC, DM_R_RMW, DM_R_FOLD, DM_R_MAP, DM_R_OTHER, DM_R_N };   /* read sources */

struct dm_geom {
	u64 L;          /* logical pages */
	u32 P;          /* pages per block */
	u32 NB;         /* blocks (total physical space = NB * P pages) */
	u32 U;          /* IU pages (write-buffer granularity) */
	u32 T;          /* free-block GC threshold / reserve (ftlsim default 2) */
};

struct dm_flash_ops {
	void *ctx;
	/* ppn == DM_NOPPN: a mapping-table page (not placed in data blocks; timing only) */
	void (*prog)(void *ctx, u64 ppn, int src);
	void (*read)(void *ctx, u64 ppn, int src);
	void (*erase)(void *ctx, u32 blk);
};

struct dm_stats {
	u64 host_submitted;     /* pages submitted by the host (before the write buffer) */
	u64 host_pages;         /* pages handed to the core by the write buffer */
	u64 host_read_pages;
	u64 prog[DM_P_N];
	u64 read[DM_R_N];
	u64 erases;
	u64 map_miss;
	u64 staged;           /* host pages held by the core outside the write buffer, not yet programmed (LeaFTL sort batch) */
	u64 stage_flushes;    /* times the core programmed its staged pages */
};

struct dm_ftl_ops {
	const char *name;
	/* params: space-separated key=value policy options (may be NULL / empty) */
	void *(*create)(const struct dm_geom *g, const struct dm_flash_ops *fo, struct dm_stats *st, const char *params);
	void (*destroy)(void *core);
	void (*write)(void *core, u64 lpn, u64 n);    /* post-buffer host write of n consecutive pages */
	void (*read)(void *core, u64 lpn, u64 n);     /* host read (pages still in the write buffer never get here) */
	void (*flush)(void *core);                    /* drain internal staging (verify / shutdown) */
	u64 (*mem_bytes)(void *core);                 /* mapping DRAM, measured end state */
	int (*verify)(void *core);                    /* 0 = consistent */
	void (*report)(void *core, char *buf, int len); /* policy-specific JSON fragment ("\"k\": v, ...") or "" */
	/* optional: logical pages the policy actually exposes (<= g->L), e.g. FAST truncates to whole blocks.
	 * NULL = g->L. The kernel glue sizes the namespace with it. */
	u64 (*logical_pages)(void *core);
};

extern const struct dm_ftl_ops dm_policy;

/* helpers for policies: parse "key=value" from params */
static inline int dm_param_u64(const char *params, const char *key, u64 *out)
{
	const char *p = params; size_t kl = strlen(key);
	if (!p) return 0;
	while (*p) {
		while (*p == ' ' || *p == ',') p++;
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			u64 v = 0; p += kl + 1;
			while (*p >= '0' && *p <= '9') v = v * 10 + (u64)(*p++ - '0');
			if (*p == 'K') v <<= 10; else if (*p == 'M') v <<= 20; else if (*p == 'G') v <<= 30;
			*out = v; return 1;
		}
		while (*p && *p != ' ' && *p != ',') p++;
	}
	return 0;
}
#endif
