/* SPDX-License-Identifier: GPL-2.0-only */
/* dual_mapping FTL cores: one source compiles in the kernel (NVMeVirt) and in userspace (harness). */
#ifndef DM_PORT_H
#define DM_PORT_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/sort.h>
#include <linux/log2.h>
#include <linux/bitops.h>
#define dm_popcount(x) hweight32(x)
#define dm_popcount64(x) hweight64(x)
#define dm_ctz64(x) __ffs64(x)
#define dm_clz64(x) (63 - __fls(x))
#define dm_zalloc(sz) vzalloc(sz)
#define dm_free(p) vfree(p)
#define dm_log(...) printk(KERN_INFO "dm_ftl: " __VA_ARGS__)
#define dm_die(...) do { printk(KERN_ERR "dm_ftl FATAL: " __VA_ARGS__); BUG(); } while (0)
#define dm_sort(base, n, sz, cmp) sort(base, n, sz, cmp, NULL)
#else
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
#define dm_zalloc(sz) calloc(1, (sz))
#define dm_free(p) free(p)
#define dm_log(...) fprintf(stderr, "dm_ftl: " __VA_ARGS__)
#define dm_die(...) do { fprintf(stderr, "dm_ftl FATAL: " __VA_ARGS__); abort(); } while (0)
#define dm_sort(base, n, sz, cmp) qsort(base, n, sz, cmp)
#define dm_popcount(x) __builtin_popcount(x)
#define dm_popcount64(x) __builtin_popcountll(x)
#define dm_ctz64(x) __builtin_ctzll(x)
#define dm_clz64(x) __builtin_clzll(x)
#endif
/* bit helpers: never call __builtin_popcount* directly (no libgcc in the kernel) */

/* NO floating point in core code: the kernel build has no FPU context. Use integer / fixed-point math. */
static inline u64 dm_isqrt(u64 x) { u64 r = 0, b = 1ull << 62; while (b > x) b >>= 2;
	while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

/* Preemption point for long core loops (GC / merge / verify). Kernel: the NVMeVirt dispatcher is a kthread in
 * process context and the glue holds no spinlock around core calls, so cond_resched() is allowed there; never call
 * it inside a kernel_fpu_begin()/kernel_fpu_end() section or while holding a spinlock. Userspace: no-op.
 * dm_prefetch(): read prefetch hint (no semantic effect). */
#ifdef __KERNEL__
#include <linux/sched.h>
#include <linux/prefetch.h>
#define dm_cond_resched() cond_resched()
#define dm_prefetch(p) prefetch(p)
#else
#define dm_cond_resched() do {} while (0)
#define dm_prefetch(p) __builtin_prefetch(p)
#endif

#define DM_NONE32 0xFFFFFFFFu
#define DM_NOPPN (~0ull)   /* ppn argument of a mapping-table (translation page) access */
#endif
