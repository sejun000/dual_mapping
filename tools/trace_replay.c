// trace_replay: closed-loop block-trace replayer for the NVMeVirt emulated SSD.
//
//   trace_replay --dev /dev/nvmeXn1 --trace t.csv --out DIR [--qd 32] [--warmup 0.5] [--precond 1|0]
//                [--precond-bs 131072] [--hook CMD] [--max-seconds S] [--allow-loop]
//
// Trace CSV: dev,op,byte_off,len_bytes[,...] (op W or R; offsets/lengths 4 KiB aligned, as tools/trace_prep emits in
// segment mode). Ops are issued in trace order with a fixed number in flight (closed loop, QD 32 by default),
// timestamps are ignored. Linux native AIO, O_DIRECT, 4 KiB-aligned buffers.
//
// Phases, mirroring ftlsim's trace mode:
//   1. precondition: one sequential pass over the whole device with --precond-bs writes (ftlsim: seq fill);
//   2. warm-up: trace ops until the cumulative written pages reach --warmup x total write pages (ftlsim:
//      trace_warmup=0.5, stats reset after the op that crosses it); the queue is drained there;
//   3. measured: the rest of the trace. Latency histograms (per op, submit to completion) for reads and writes
//      cover this phase only.
// The hook command, if given, runs as `CMD <phase>` after the queue is drained at: precond_end, warm_end, end.
// The driver uses it to dump the device FTL counters.
//
// Output in DIR: summary.json, lat_hist.csv (measured-phase histograms), iops.csv (per-second completions).
//
// Device safety: by default only a whole-disk NVMe namespace whose sysfs model starts with "CSL_Virt" is accepted.
// nvme0n1, nvme1n1 and nvme2n1 are always refused, as is any device that appears in /proc/mounts.
// --allow-loop (testing only) additionally accepts /dev/loopN.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/aio_abi.h>
#include <linux/fs.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t u32;

#define PG 4096ull
#define MAXQD 1024
#define CHUNK_PAGES 8192u   /* ops longer than 32 MiB are cut into 32 MiB ops at load (none in the prepared traces) */

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#include <stdarg.h>
static void die(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); fprintf(stderr, "trace_replay: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
	exit(1);
}

static inline u64 now_ns(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- aio syscalls */
static int io_setup(unsigned n, aio_context_t *ctx) { return syscall(SYS_io_setup, n, ctx); }
static int io_destroy(aio_context_t ctx) { return syscall(SYS_io_destroy, ctx); }
static int io_submit(aio_context_t ctx, long n, struct iocb **iocbs) { return syscall(SYS_io_submit, ctx, n, iocbs); }
static int io_getevents(aio_context_t ctx, long min, long max, struct io_event *ev, struct timespec *to)
{ return syscall(SYS_io_getevents, ctx, min, max, ev, to); }

/* ---- latency histogram: log2 buckets with 128 linear sub-buckets (< 0.8% relative error) */
#define SUB 128
#define NBUCK (64 * SUB)
struct hist { u64 n, sum, max, min; u64 b[NBUCK]; };
static inline int hidx(u64 v)
{
	if (v < SUB) return (int)v;
	int e = 63 - __builtin_clzll(v);           /* v in [2^e, 2^(e+1)) */
	int s = (int)((v >> (e - 7)) & (SUB - 1)); /* 7 = log2(SUB) */
	return (e - 6) * SUB + s;
}
static inline u64 hlow(int i)   /* lower bound of bucket i */
{
	if (i < SUB) return (u64)i;
	int e = i / SUB + 6, s = i % SUB;
	return ((u64)1 << e) + ((u64)s << (e - 7));
}
static void hadd(struct hist *h, u64 v)
{
	h->n++; h->sum += v; if (v > h->max) h->max = v; if (!h->min || v < h->min) h->min = v;
	h->b[hidx(v)]++;
}
static double hpct(const struct hist *h, double q)
{
	if (!h->n) return 0;
	u64 target = (u64)ceil(q / 100.0 * h->n), c = 0;
	if (target < 1) target = 1;
	for (int i = 0; i < NBUCK; i++) {
		c += h->b[i];
		if (c >= target) {
			u64 lo = hlow(i), hi = i + 1 < NBUCK ? hlow(i + 1) : lo;
			double mid = (lo + hi) / 2.0;
			return mid > h->max ? (double)h->max : mid;
		}
	}
	return (double)h->max;
}

/* ---- device checks */
static const char *basename_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void check_device(const char *dev, int allow_loop)
{
	char real[4096], path[8192], model[256] = "";
	if (!realpath(dev, real)) die("cannot resolve %s: %s", dev, strerror(errno));
	const char *name = basename_of(real);
	struct stat st;
	if (stat(real, &st) || !S_ISBLK(st.st_mode)) die("%s is not a block device", real);
	if (!strcmp(name, "nvme0n1") || !strcmp(name, "nvme1n1") || !strcmp(name, "nvme2n1"))
		die("refusing %s: real data disk", real);
	/* whole disk only: /sys/block/<name> must exist */
	snprintf(path, sizeof path, "/sys/block/%s", name);
	if (access(path, F_OK)) die("refusing %s: not a whole disk", real);
	/* not mounted (whole disk or any partition of it) */
	FILE *m = fopen("/proc/mounts", "r");
	if (!m) die("cannot read /proc/mounts");
	char line[8192];
	while (fgets(line, sizeof line, m)) {
		char src[4096]; if (sscanf(line, "%4095s", src) != 1) continue;
		char rs[4096]; const char *cmp = realpath(src, rs) ? rs : src;
		if (!strncmp(cmp, real, strlen(real))) die("refusing %s: mounted (%s)", real, src);
	}
	fclose(m);
	if (!strncmp(name, "loop", 4)) {
		if (!allow_loop) die("refusing %s: loop devices need --allow-loop (testing only)", real);
		fprintf(stderr, "trace_replay: TEST MODE on loop device %s\n", real);
		return;
	}
	snprintf(path, sizeof path, "/sys/block/%s/device/model", name);
	FILE *f = fopen(path, "r");
	if (!f || !fgets(model, sizeof model, f)) die("refusing %s: no model in sysfs", real);
	fclose(f);
	if (strncmp(model, "CSL_Virt", 8)) die("refusing %s: model '%s' is not CSL_Virt*", real, model);
	model[strcspn(model, " \n")] = 0;
	fprintf(stderr, "trace_replay: device %s model %s\n", real, model);
}

/* ---- trace: 8 bytes per op: page offset (u32) and (pages << 1 | is_read) */
struct op { u32 lpn; u32 lr; };
static struct op *ops; static u64 nops, cap_ops;
static void push(u64 lpn, u64 n, int rd)
{
	if (nops == cap_ops) {
		cap_ops = cap_ops ? cap_ops * 2 : 1 << 20;
		ops = realloc(ops, cap_ops * sizeof *ops);
		if (!ops) die("out of memory");
	}
	ops[nops].lpn = (u32)lpn; ops[nops].lr = (u32)(n << 1 | (rd ? 1 : 0)); nops++;
}
static void load_trace(const char *fn, u64 dev_pages)
{
	int fd = open(fn, O_RDONLY);
	if (fd < 0) die("open %s: %s", fn, strerror(errno));
	struct stat st; fstat(fd, &st);
	if (!st.st_size) die("empty trace");
	char *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) die("mmap trace");
	madvise(p, st.st_size, MADV_SEQUENTIAL);
	char *e = p + st.st_size, *s = p;
	u64 lineno = 0;
	while (s < e) {
		char *nl = memchr(s, '\n', e - s); if (!nl) nl = e;
		lineno++;
		char *c = memchr(s, ',', nl - s);
		if (c && c + 1 < nl && (c[1] == 'W' || c[1] == 'R') && c + 2 < nl && c[2] == ',') {
			int rd = c[1] == 'R';
			char *q = c + 3; u64 off = 0, len = 0;
			while (q < nl && *q >= '0' && *q <= '9') off = off * 10 + (*q++ - '0');
			if (q >= nl || *q != ',') die("line %" PRIu64 ": bad offset", lineno);
			q++;
			while (q < nl && *q >= '0' && *q <= '9') len = len * 10 + (*q++ - '0');
			if (off % PG || len % PG || !len) die("line %" PRIu64 ": offset/length not 4 KiB aligned (prepare with trace_prep order=touch)", lineno);
			u64 lpn = off / PG, n = len / PG;
			if (lpn + n > dev_pages) die("line %" PRIu64 ": op [%" PRIu64 ", +%" PRIu64 ") beyond device (%" PRIu64 " pages)", lineno, lpn, n, dev_pages);
			if (lpn + n > 0xFFFFFFFFull) die("device too large for 32-bit page offsets");
			while (n) { u64 k = n > CHUNK_PAGES ? CHUNK_PAGES : n; push(lpn, k, rd); lpn += k; n -= k; }
		}
		s = nl + 1;
	}
	munmap(p, st.st_size); close(fd);
}

/* ---- per-second log */
struct sec { u64 rops, wops, rpages, wpages; int phase; };
static struct sec *secs; static u64 nsecs;
static void sec_add(u64 t_rel_ns, int phase, int rd, u64 pages)
{
	u64 s = t_rel_ns / 1000000000ull;
	if (s >= nsecs) {
		u64 nn = s + 1 > nsecs * 2 ? s + 1 : nsecs * 2; if (nn < 4096) nn = 4096;
		secs = realloc(secs, nn * sizeof *secs); if (!secs) die("oom");
		memset(secs + nsecs, 0, (nn - nsecs) * sizeof *secs); nsecs = nn;
	}
	struct sec *x = &secs[s];
	if (rd) { x->rops++; x->rpages += pages; } else { x->wops++; x->wpages += pages; }
	if (phase > x->phase) x->phase = phase;
}

/* ---- engine */
struct slot { struct iocb cb; u64 t0; u64 pages; int rd; int busy; char *buf; };
static aio_context_t ctx;
static struct slot slots[MAXQD];
static int qd = 32, inflight;
static int fd_dev;
static u64 t_origin;
static int phase;   /* 0 precond, 1 warm-up, 2 measured */
static struct hist hr, hw, hw_warm, hr_warm;
static u64 errors;

static void reap(int min)
{
	struct io_event ev[MAXQD];
	int n;
	do n = io_getevents(ctx, min, MAXQD, ev, NULL); while (n < 0 && errno == EINTR);
	if (n < 0) die("io_getevents: %s", strerror(errno));
	u64 t = now_ns();
	for (int i = 0; i < n; i++) {
		struct slot *s = (struct slot *)(uintptr_t)ev[i].data;
		if ((long long)ev[i].res != (long long)(s->pages * PG)) {
			errors++;
			fprintf(stderr, "trace_replay: I/O error res=%lld (%s off=%llu len=%llu)\n", (long long)ev[i].res,
				s->rd ? "read" : "write", (unsigned long long)s->cb.aio_offset, (unsigned long long)s->cb.aio_nbytes);
			if (errors > 16) die("too many I/O errors");
		}
		u64 lat = t - s->t0;
		if (phase == 2) hadd(s->rd ? &hr : &hw, lat);
		else if (phase == 1) hadd(s->rd ? &hr_warm : &hw_warm, lat);
		sec_add(t - t_origin, phase, s->rd, s->pages);
		s->busy = 0; inflight--;
	}
}
static void drain(void) { while (inflight) reap(1); }

static struct slot *free_slot(void)
{
	static int hint;
	for (int k = 0; k < qd; k++) { int i = (hint + k) % qd; if (!slots[i].busy) { hint = i + 1; return &slots[i]; } }
	return NULL;
}
/* queue up to `max` ops produced by next(); submit in one batch */
static void submit_batch(struct iocb **v, int n)
{
	int done = 0;
	while (done < n) {
		int r = io_submit(ctx, n - done, v + done);
		if (r < 0) { if (errno == EAGAIN || errno == EINTR) { reap(1); continue; } die("io_submit: %s", strerror(errno)); }
		done += r;
	}
}
static void prep(struct slot *s, int rd, u64 lpn, u64 pages)
{
	memset(&s->cb, 0, sizeof s->cb);
	s->cb.aio_data = (u64)(uintptr_t)s;
	s->cb.aio_lio_opcode = rd ? IOCB_CMD_PREAD : IOCB_CMD_PWRITE;
	s->cb.aio_fildes = fd_dev;
	s->cb.aio_buf = (u64)(uintptr_t)s->buf;
	s->cb.aio_nbytes = pages * PG;
	s->cb.aio_offset = lpn * PG;
	s->rd = rd; s->pages = pages; s->busy = 1; inflight++;
}

static const char *hook;
static void run_hook(const char *ph)
{
	if (!hook) return;
	char cmd[8192]; snprintf(cmd, sizeof cmd, "%s %s", hook, ph);
	int r = system(cmd);
	if (r) fprintf(stderr, "trace_replay: hook '%s' returned %d\n", cmd, r);
}

static void usage(void)
{
	fprintf(stderr, "usage: trace_replay --dev DEV --trace CSV --out DIR [--qd 32] [--warmup 0.5] [--precond 1] "
			"[--precond-bs 131072] [--hook CMD] [--max-seconds S] [--allow-loop]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *trace = NULL, *out = NULL;
	double warmup = 0.5, max_s = 0; int precond = 1, allow_loop = 0; u64 pbs = 131072;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define ARG() (i + 1 < argc ? argv[++i] : (usage(), (char *)NULL))
		if (!strcmp(a, "--dev")) dev = ARG();
		else if (!strcmp(a, "--trace")) trace = ARG();
		else if (!strcmp(a, "--out")) out = ARG();
		else if (!strcmp(a, "--qd")) qd = atoi(ARG());
		else if (!strcmp(a, "--warmup")) warmup = atof(ARG());
		else if (!strcmp(a, "--precond")) precond = atoi(ARG());
		else if (!strcmp(a, "--precond-bs")) pbs = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--hook")) hook = ARG();
		else if (!strcmp(a, "--max-seconds")) max_s = atof(ARG());
		else if (!strcmp(a, "--allow-loop")) allow_loop = 1;
		else usage();
	}
	if (!dev || !trace || !out) usage();
	if (qd < 1 || qd > MAXQD) die("bad --qd");
	if (pbs % PG || !pbs) die("bad --precond-bs");

	check_device(dev, allow_loop);
	fd_dev = open(dev, O_RDWR | O_DIRECT);
	if (fd_dev < 0) die("open %s: %s", dev, strerror(errno));
	u64 dev_bytes = 0;
	if (ioctl(fd_dev, BLKGETSIZE64, &dev_bytes)) die("BLKGETSIZE64: %s", strerror(errno));
	u64 dev_pages = dev_bytes / PG;

	u64 tl0 = now_ns();
	load_trace(trace, dev_pages);
	u64 tot_w = 0, tot_r = 0, nw = 0, nr = 0, maxp = pbs / PG;
	for (u64 i = 0; i < nops; i++) {
		u64 n = ops[i].lr >> 1;
		if (ops[i].lr & 1) { tot_r += n; nr++; } else { tot_w += n; nw++; }
		if (n > maxp) maxp = n;
	}
	u64 warm = (u64)(tot_w * warmup), bidx = nops, acc = 0;   /* bidx: last warm-up op (ftlsim resets after it) */
	for (u64 i = 0; i < nops && warmup > 0; i++) {
		if (!(ops[i].lr & 1)) acc += ops[i].lr >> 1;
		if (acc >= warm) { bidx = i; break; }
	}
	if (warmup <= 0) bidx = (u64)-1;
	fprintf(stderr, "trace_replay: %" PRIu64 " ops (%" PRIu64 " writes %.1f GiB, %" PRIu64 " reads %.1f GiB) loaded in %.1fs; device %.2f GiB; "
		"write turnovers %.2f; warm-up ends after op %" PRIu64 "\n",
		nops, nw, tot_w * PG / 1073741824.0, nr, tot_r * PG / 1073741824.0, (now_ns() - tl0) / 1e9,
		dev_bytes / 1073741824.0, (double)tot_w / dev_pages, bidx);

	for (int i = 0; i < qd; i++) {
		if (posix_memalign((void **)&slots[i].buf, 4096, maxp * PG)) die("buffer alloc");
		for (u64 k = 0; k < maxp * PG; k += 8) *(u64 *)(slots[i].buf + k) = 0x5a5aa5a5ull * (k + 1) ^ (u64)i << 56;
	}
	if (io_setup(qd, &ctx)) die("io_setup: %s", strerror(errno));
	if (mkdir(out, 0755) && errno != EEXIST) die("mkdir %s", out);

	t_origin = now_ns();
	struct iocb *batch[MAXQD];
	u64 t_pre = 0;
	/* 1. sequential precondition */
	if (precond) {
		phase = 0;
		u64 lp = 0, step = pbs / PG;
		while (lp < dev_pages) {
			int nb = 0; struct slot *s;
			while (lp < dev_pages && (s = free_slot())) {
				u64 n = dev_pages - lp < step ? dev_pages - lp : step;
				prep(s, 0, lp, n); batch[nb++] = &s->cb; lp += n;
			}
			if (nb) submit_batch(batch, nb);
			reap(1);
		}
		drain();
		t_pre = now_ns() - t_origin;
		fprintf(stderr, "trace_replay: precondition %.2f GiB in %.1fs (%.0f MiB/s)\n", dev_bytes / 1073741824.0, t_pre / 1e9,
			dev_bytes / 1048576.0 / (t_pre / 1e9));
		run_hook("precond_end");
	}
	/* 2./3. trace */
	phase = warmup > 0 ? 1 : 2;
	if (phase == 2) run_hook("warm_end");
	u64 t_start = now_ns(), t_meas = phase == 2 ? t_start : 0, t_end = 0;
	u64 next = 0, truncated = 0, mw = 0, mr = 0, mwp = 0, mrp = 0;
	u64 last_report = t_start;
	while (next < nops) {
		int nb = 0; struct slot *s;
		while (next < nops && (s = free_slot())) {
			struct op o = ops[next];
			int rd = o.lr & 1; u64 n = o.lr >> 1;
			prep(s, rd, o.lpn, n); batch[nb++] = &s->cb;
			if (phase == 2) { if (rd) { mr++; mrp += n; } else { mw++; mwp += n; } }
			next++;
			if (phase == 1 && next - 1 == bidx) break;
		}
		if (nb) {
			u64 t0 = now_ns();
			for (int k = 0; k < nb; k++) ((struct slot *)(uintptr_t)batch[k]->aio_data)->t0 = t0;
			submit_batch(batch, nb);
		}
		if (phase == 1 && next > 0 && next - 1 == bidx) {
			drain();
			fprintf(stderr, "trace_replay: warm-up done after %" PRIu64 " ops in %.1fs\n", next, (now_ns() - t_start) / 1e9);
			run_hook("warm_end");
			phase = 2; t_meas = now_ns();
			continue;
		}
		reap(1);
		u64 t = now_ns();
		if (t - last_report > 60000000000ull) {
			last_report = t;
			fprintf(stderr, "trace_replay: %5.1f%% of ops, phase %d, %.0fs\n", 100.0 * next / nops, phase, (t - t_start) / 1e9);
		}
		if (max_s > 0 && (t - t_origin) / 1e9 > max_s) {
			truncated = next;
			fprintf(stderr, "trace_replay: --max-seconds reached, stopping after %" PRIu64 " of %" PRIu64 " ops\n", next, nops);
			break;
		}
	}
	drain();
	t_end = now_ns();
	run_hook("end");
	io_destroy(ctx);
	close(fd_dev);

	/* outputs */
	char fn[4096];
	double meas_s = t_meas ? (t_end - t_meas) / 1e9 : 0, warm_s = t_meas ? (t_meas - t_start) / 1e9 : (t_end - t_start) / 1e9;
	snprintf(fn, sizeof fn, "%s/lat_hist.csv", out);
	FILE *f = fopen(fn, "w");
	if (f) {
		fprintf(f, "lat_ns_low,read_count,write_count\n");
		for (int i = 0; i < NBUCK; i++) if (hr.b[i] || hw.b[i]) fprintf(f, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", hlow(i), hr.b[i], hw.b[i]);
		fclose(f);
	}
	snprintf(fn, sizeof fn, "%s/iops.csv", out);
	f = fopen(fn, "w");
	if (f) {
		fprintf(f, "sec,phase,read_iops,write_iops,read_MiBps,write_MiBps\n");
		u64 last = (t_end - t_origin) / 1000000000ull;
		for (u64 s = 0; s <= last && s < nsecs; s++)
			fprintf(f, "%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64 ",%.1f,%.1f\n", s, secs[s].phase, secs[s].rops, secs[s].wops,
				secs[s].rpages / 256.0, secs[s].wpages / 256.0);
		fclose(f);
	}
	snprintf(fn, sizeof fn, "%s/summary.json", out);
	f = fopen(fn, "w");
	if (!f) die("cannot write %s", fn);
	fprintf(f, "{\n \"trace\": \"%s\", \"device\": \"%s\", \"device_bytes\": %" PRIu64 ", \"qd\": %d, \"warmup\": %.3f,\n", trace, dev, dev_bytes, qd, warmup);
	fprintf(f, " \"trace_ops\": %" PRIu64 ", \"trace_writes\": %" PRIu64 ", \"trace_write_pages\": %" PRIu64 ", \"trace_reads\": %" PRIu64 ", \"trace_read_pages\": %" PRIu64 ",\n",
		nops, nw, tot_w, nr, tot_r);
	fprintf(f, " \"truncated\": %s, \"ops_issued\": %" PRIu64 ", \"errors\": %" PRIu64 ",\n", truncated ? "true" : "false", truncated ? truncated : nops, errors);
	fprintf(f, " \"precond_s\": %.1f, \"warm_s\": %.1f, \"measure_s\": %.1f, \"total_s\": %.1f,\n", t_pre / 1e9, warm_s, meas_s, (t_end - t_origin) / 1e9);
	fprintf(f, " \"measured\": {\"reads\": %" PRIu64 ", \"writes\": %" PRIu64 ", \"read_pages\": %" PRIu64 ", \"write_pages\": %" PRIu64 ",\n", mr, mw, mrp, mwp);
	fprintf(f, "   \"iops\": %.1f, \"read_iops\": %.1f, \"write_iops\": %.1f, \"read_MiBps\": %.1f, \"write_MiBps\": %.1f},\n",
		meas_s ? (mr + mw) / meas_s : 0, meas_s ? mr / meas_s : 0, meas_s ? mw / meas_s : 0, meas_s ? mrp / 256.0 / meas_s : 0, meas_s ? mwp / 256.0 / meas_s : 0);
	const struct hist *hs[4] = {&hr, &hw, &hr_warm, &hw_warm};
	const char *hn[4] = {"read_lat_us", "write_lat_us", "warm_read_lat_us", "warm_write_lat_us"};
	for (int k = 0; k < 4; k++) {
		const struct hist *h = hs[k];
		fprintf(f, " \"%s\": {\"n\": %" PRIu64 ", \"mean\": %.1f, \"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f, \"p99.9\": %.1f, \"p99.99\": %.1f, \"max\": %.1f}%s\n",
			hn[k], h->n, h->n ? h->sum / 1e3 / h->n : 0, hpct(h, 50) / 1e3, hpct(h, 90) / 1e3, hpct(h, 99) / 1e3, hpct(h, 99.9) / 1e3,
			hpct(h, 99.99) / 1e3, h->max / 1e3, k == 3 ? "" : ",");
	}
	fprintf(f, "}\n");
	fclose(f);
	fprintf(stderr, "trace_replay: done in %.1fs (measured %.1fs: %.0f IOPS, reads %" PRIu64 " p99 %.0fus, writes %" PRIu64 " p99 %.0fus)%s\n",
		(t_end - t_origin) / 1e9, meas_s, meas_s ? (mr + mw) / meas_s : 0, hr.n, hpct(&hr, 99) / 1e3, hw.n, hpct(&hw, 99) / 1e3,
		truncated ? " TRUNCATED" : "");
	return errors ? 1 : 0;
}
