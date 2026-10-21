// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace replay harness for dm FTL cores. Mirrors ftlsim's trace mode so a core can be
 * cross-validated against `ftlsim mode=<policy> trace=...` with identical geometry:
 *   seq fill (256 KiB chunks) -> trace writes through the shared write buffer, stats reset after
 *   `warmup` of the trace's write pages -> drain -> verify -> JSON report on stdout.
 * Flash callbacks enforce in-order programming inside each block (erase before reuse).
 *
 *   replay_<policy> trace=<csv> capacity=<bytes|G> [op=0.09278] [P=0(auto)] [U=16] [wbuf=1M] [T=2]
 *                   [warmup=0.5] [seq_fill=1] [reads=0] [params="k=v k=v"]
 */
#include "../ftlcore.h"
#include "../dm_wbuf.h"
#include <math.h>

static struct dm_geom G; static struct dm_stats ST; static u32 *wp; static void *core;
static u64 bad_order;

static void h_prog(void *ctx, u64 ppn, int src)
{
	u32 b, off; (void)ctx; (void)src;
	if (ppn == DM_NOPPN) return;
	b = (u32)(ppn / G.P); off = (u32)(ppn % G.P);
	if (b >= G.NB || off != wp[b]) { if (bad_order++ < 5) fprintf(stderr, "ORDER: prog blk %u off %u expected %u\n", b, off, b < G.NB ? wp[b] : 0); }
	else wp[b]++;
}
static void h_read(void *ctx, u64 ppn, int src) { (void)ctx; (void)ppn; (void)src; }
static void h_erase(void *ctx, u32 blk) { (void)ctx; wp[blk] = 0; }
static FILE *dumpf;
static void emit(void *ctx, u64 lpn, u64 n) { u64 i; (void)ctx;
	if (dumpf) for (i = 0; i < n; i++) fprintf(dumpf, "0 1 %llu 4096\n", (unsigned long long)(lpn + i));
	dm_policy.write(core, lpn, n); }

/* ftlsim run_trace dense remap: 1 MiB source regions get dense slots in first-touch order */
#define RPG 256ull
static u64 *rk, *rv; static u64 rcap, rnext;
static u64 region_slot(u64 r, int create)
{
	u64 h = (r * 0x9E3779B97F4A7C15ull) & (rcap - 1);
	while (rk[h] != ~0ull) { if (rk[h] == r) return rv[h]; h = (h + 1) & (rcap - 1); }
	if (!create) return ~0ull;
	rk[h] = r; rv[h] = rnext++; return rv[h];
}

static u64 parse_bytes(const char *s) { char *e; double v = strtod(s, &e);
	if (*e == 'G') return (u64)(v * (1ull << 30)); if (*e == 'M') return (u64)(v * (1ull << 20)); if (*e == 'K') return (u64)(v * 1024); return (u64)v; }

static void check(const char *when)
{
	dm_policy.flush(core);
	if (dm_policy.verify(core)) { fprintf(stderr, "FATAL: verify failed (%s)\n", when); exit(1); }
	fprintf(stderr, "[verify] ok (%s)\n", when);
}

int main(int argc, char **argv)
{
	const char *trace = NULL, *params = ""; u64 cap = 64ull << 30, wbuf = 1 << 20; double op = 0.09278, warmup = 0.5;
	u32 P = 0, U = 16, T = 2; int seq_fill = 1, reads = 0, i;
	struct dm_flash_ops fo = { NULL, h_prog, h_read, h_erase }; struct dm_wbuf wb;
	FILE *f; char line[256]; u64 total = 0, done = 0, warm, L, pages, tp = 0; int measuring = 0; char rep[4096];
	for (i = 1; i < argc; i++) {
		char *a = argv[i], *v = strchr(a, '='); if (!v) { fprintf(stderr, "bad arg %s\n", a); return 1; } *v++ = 0;
		if (!strcmp(a, "trace")) trace = v; else if (!strcmp(a, "capacity")) cap = parse_bytes(v);
		else if (!strcmp(a, "op")) op = atof(v); else if (!strcmp(a, "P")) P = (u32)atoi(v); else if (!strcmp(a, "U")) U = (u32)atoi(v);
		else if (!strcmp(a, "wbuf")) wbuf = parse_bytes(v); else if (!strcmp(a, "T")) T = (u32)atoi(v);
		else if (!strcmp(a, "warmup")) warmup = atof(v); else if (!strcmp(a, "seq_fill")) seq_fill = atoi(v);
		else if (!strcmp(a, "reads")) reads = atoi(v); else if (!strcmp(a, "params")) params = v;
		else { fprintf(stderr, "unknown key %s\n", a); return 1; }
	}
	if (!trace) { fprintf(stderr, "trace= required\n"); return 1; }
	if (getenv("DM_DUMP")) dumpf = fopen(getenv("DM_DUMP"), "w");
	pages = cap / 4096;
	if (!P) { u64 p = pages * 3 / 1024; p -= p % (64 * U); if (p < 64 * U) p = 64 * U; P = (u32)p; }   /* ftlsim auto P */
	L = pages - pages % U;
	G.L = L; G.P = P; G.U = U; G.T = T; G.NB = (u32)((u64)((double)L * (1.0 + op)) / P);
	wp = calloc(G.NB, sizeof(u32));
	core = dm_policy.create(&G, &fo, &ST, params);
	if (!core) { fprintf(stderr, "create failed\n"); return 1; }
	fprintf(stderr, "[cfg] policy=%s L=%llu NB=%u P=%u U=%u T=%u wbuf=%llu params=\"%s\"\n", dm_policy.name,
		(unsigned long long)L, G.NB, P, U, T, (unsigned long long)wbuf, params);
	dm_wbuf_init(&wb, U, wbuf / 4096, emit, NULL, &ST.host_submitted);
	if (seq_fill) { u64 p; for (p = 0; p < L; p += 64) dm_wbuf_write(&wb, p, (L - p) < 64 ? (L - p) : 64); }
	f = fopen(trace, "r"); if (!f) { perror(trace); return 1; }
	while (fgets(line, sizeof line, f)) { char op_c; long long off, len;
		if (sscanf(line, "%*[^,],%c,%lld,%lld", &op_c, &off, &len) != 3 || op_c != 'W') continue;
		total += ((u64)off + (u64)len + 4095) / 4096 - (u64)off / 4096; }
	warm = (u64)((double)total * warmup);
	fprintf(stderr, "[trace] %llu write pages (%.2fx device), warmup %.0f%%\n", (unsigned long long)total, (double)total / L, 100 * warmup);
	rewind(f);
	rcap = 1; while (rcap < 4 * (L / RPG + 16)) rcap <<= 1;
	rk = malloc(rcap * 8); rv = malloc(rcap * 8); memset(rk, 0xff, rcap * 8);
	while (fgets(line, sizeof line, f)) {
		char op_c; long long off, len; u64 p0, p1, lpn, n;
		if (sscanf(line, "%*[^,],%c,%lld,%lld", &op_c, &off, &len) != 3) continue;
		p0 = (u64)off / 4096; p1 = ((u64)off + (u64)len + 4095) / 4096; (void)lpn; (void)n;
		if (op_c != 'W' && op_c != 'R') continue;
		if (op_c == 'R' && (!reads || !measuring)) continue;
		{ u64 cur = p0;
		  while (cur < p1) {                       /* per-source-region segments, as ftlsim */
			u64 r = cur / RPG, seg_end = p1 < (r + 1) * RPG ? p1 : (r + 1) * RPG, slot, l, q;
			slot = region_slot(r, op_c == 'W');
			if (slot == ~0ull) { cur = seg_end; continue; }        /* read of never-written region */
			l = slot * RPG + (cur - r * RPG);
			if (l + (seg_end - cur) > L) { fprintf(stderr, "FATAL: dense space exceeds device\n"); return 1; }
			if (op_c == 'W') { dm_wbuf_write(&wb, l, seg_end - cur); done += seg_end - cur; }
			else for (q = 0; q < seg_end - cur; q++) { ST.host_read_pages++; if (!dm_wbuf_contains(&wb, l + q)) dm_policy.read(core, l + q, 1); }
			cur = seg_end;
		  } }
		if (op_c == 'W' && !measuring && done >= warm) { check("warmup"); memset(&ST, 0, sizeof ST); measuring = 1; }
	}
	fclose(f);
	dm_wbuf_drain(&wb);
	check("end");
	for (i = 0; i < DM_P_N; i++) tp += ST.prog[i];
	dm_policy.report(core, rep, sizeof rep);
	printf("{\n  \"policy\": \"%s\", \"L\": %llu, \"NB\": %u, \"P\": %u, \"U\": %u,\n", dm_policy.name, (unsigned long long)L, G.NB, P, U);
	printf("  \"host_submitted\": %llu, \"host_pages\": %llu, \"total_prog\": %llu, \"WAF\": %.4f,\n",
	       (unsigned long long)ST.host_submitted, (unsigned long long)ST.host_pages, (unsigned long long)tp,
	       ST.host_submitted ? (double)tp / ST.host_submitted : 0.0);
	printf("  \"prog\": {\"host\": %llu, \"gc\": %llu, \"rmw\": %llu, \"fold\": %llu, \"map\": %llu, \"other\": %llu},\n",
	       (unsigned long long)ST.prog[0], (unsigned long long)ST.prog[1], (unsigned long long)ST.prog[2], (unsigned long long)ST.prog[3], (unsigned long long)ST.prog[4], (unsigned long long)ST.prog[5]);
	printf("  \"read\": {\"host\": %llu, \"gc\": %llu, \"rmw\": %llu, \"fold\": %llu, \"map\": %llu, \"other\": %llu}, \"host_read_pages\": %llu,\n",
	       (unsigned long long)ST.read[0], (unsigned long long)ST.read[1], (unsigned long long)ST.read[2], (unsigned long long)ST.read[3], (unsigned long long)ST.read[4], (unsigned long long)ST.read[5], (unsigned long long)ST.host_read_pages);
	printf("  \"erases\": %llu, \"map_miss\": %llu, \"mem_bytes\": %llu, \"order_violations\": %llu%s%s\n}\n",
	       (unsigned long long)ST.erases, (unsigned long long)ST.map_miss, (unsigned long long)dm_policy.mem_bytes(core),
	       (unsigned long long)bad_order, rep[0] ? ",\n  " : "", rep);
	dm_policy.destroy(core); dm_wbuf_destroy(&wb);
	return bad_order ? 2 : 0;
}
