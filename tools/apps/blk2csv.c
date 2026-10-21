// tools/apps/blk2csv.c - blktrace output dir -> ftlsim replay CSV + JSON sidecar.
//
//   blk2csv <trace_dir> [-o out.csv] [-j out.json] [-p prefix] [-m start_marker] [-c capacity_bytes]
//
// Reads the binary per-CPU files <trace_dir>/<prefix>.blktrace.<cpu> (prefix default "trace", as written by
// tools/apps/blkcap.sh) directly - no blkparse - and k-way merges them by event timestamp, so the CSV is in issue
// order. Only D (issue) events become CSV lines; each request is issued once after all merging, so each request
// appears exactly once (a requeued request is issued again - requeues are counted and reported). Lines:
//     0,<W|R>,<byte_off>,<len_bytes>,<ts_seconds>        (dev = 0, byte_off = sector * 512, ts relative to the
//                                                         first trace record, ns resolution)
// Skipped and counted: zero-length requests (flush-only), discards, passthrough (PC) requests.
// blktrace message events (written to /sys/kernel/debug/block/<dev>/msg) are markers; for each marker the JSON
// records the number of CSV lines before it and the cumulative counters at that point. 4 KiB pages are counted the
// way ftlsim does: p0 = off / 4096, p1 = (off + len + 4095) / 4096.
// Defaults: out = <trace_dir>/trace.csv and <trace_dir>/trace.json; start marker "dm_measure_start";
// capacity from device_bytes= in <trace_dir>/blkcap_meta.txt.
// Build: gcc -O2 -o tools/apps/blk2csv tools/apps/blk2csv.c
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16;

struct rec {   // struct blk_io_trace (48 bytes, native endian)
    u32 magic, sequence; u64 time, sector; u32 bytes, action, pid, device, cpu; u16 error, pdu_len;
};
#define BLK_IO_TRACE_MAGIC 0x65617400u
#define TC_READ (1u << 0)
#define TC_WRITE (1u << 1)
#define TC_PC (1u << 9)
#define TC_NOTIFY (1u << 10)
#define TC_DISCARD (1u << 13)
#define TA_REQUEUE 6
#define TA_ISSUE 7
#define TA_CGROUP (1u << 8)
#define TN_MESSAGE 2

static void die(const char *fmt, const char *a) { fprintf(stderr, "blk2csv: "); fprintf(stderr, fmt, a); fputc('\n', stderr); exit(1); }

// ---------------------------------------------------------------- per-CPU input streams
struct in { FILE *f; char *path; struct rec r; char pdu[4096]; int ok; u64 nrec; };
static int rd(struct in *s) {
    if (fread(&s->r, sizeof s->r, 1, s->f) != 1) return s->ok = 0;
    if ((s->r.magic & 0xffffff00u) != BLK_IO_TRACE_MAGIC) die("bad magic in %s (corrupt or truncated file)", s->path);
    u16 n = s->r.pdu_len;
    if (n) {
        size_t k = n < sizeof s->pdu ? n : sizeof s->pdu;
        if (fread(s->pdu, 1, k, s->f) != k) return s->ok = 0;
        if (n > k && fseek(s->f, n - k, SEEK_CUR)) return s->ok = 0;
    }
    s->nrec++;
    return s->ok = 1;
}
static struct in *ins; static int nin; static int *heap; static int nheap;
static int less(int a, int b) {
    const struct rec *x = &ins[a].r, *y = &ins[b].r;
    if (x->time != y->time) return x->time < y->time;
    if (x->cpu != y->cpu) return x->cpu < y->cpu;
    return x->sequence < y->sequence;
}
static void sift_down(int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < nheap && less(heap[l], heap[m])) m = l;
        if (r < nheap && less(heap[r], heap[m])) m = r;
        if (m == i) return;
        int t = heap[i]; heap[i] = heap[m]; heap[m] = t; i = m;
    }
}

// ---------------------------------------------------------------- output
static char obuf[1 << 22]; static size_t olen; static FILE *ocsv;
static inline void oflush(void) { if (olen && fwrite(obuf, 1, olen, ocsv) != olen) die("write error on %s", "csv"); olen = 0; }
static inline void oput_u64(u64 v) {
    char t[24]; int n = 0; do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) obuf[olen++] = t[--n];
}

// ---------------------------------------------------------------- stats
#define MAXMARK 64
#define HMAX 4096   // request-size histogram in 4 KiB pages, exact up to HMAX-1, the rest in the last bucket
struct cum { u64 lines, wreq, wpages, wbytes, rreq, rpages, rbytes, footprint; };
struct mark { char name[132]; u64 time; struct cum c; };
static struct mark marks[MAXMARK]; static int nmarks;
static struct cum cur;
static u64 whist[HMAX], rhist[HMAX], w_unaligned, r_unaligned;
static unsigned char *lastseg; static u64 lastseg_n;   // per 4K page: 1 + #markers seen at its most recent write
static u64 max_end_byte, max_wend_byte, max_wreq_bytes, max_rreq_bytes;
static u64 n_flush0, n_discard, discard_bytes, n_pc, n_requeue, n_other_issue, n_msg, n_probe, n_notes, n_events, n_other;

static void lastseg_grow(u64 need) {
    if (need <= lastseg_n) return;
    u64 n = lastseg_n ? lastseg_n : (1u << 20);
    while (n < need) n *= 2;
    lastseg = realloc(lastseg, n);
    if (!lastseg) die("out of memory (%s)", "page map");
    memset(lastseg + lastseg_n, 0, n - lastseg_n); lastseg_n = n;
}

static void json_str(FILE *j, const char *s) {
    fputc('"', j);
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') fprintf(j, "\\%c", c);
        else if (c < 0x20) fprintf(j, "\\u%04x", c);
        else fputc(c, j);
    }
    fputc('"', j);
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    const char *dir = NULL, *csvp = NULL, *jsonp = NULL, *prefix = "trace", *mstart = "dm_measure_start";
    u64 capacity = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) csvp = argv[++i];
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) jsonp = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mstart = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) capacity = strtoull(argv[++i], NULL, 10);
        else if (argv[i][0] != '-' && !dir) dir = argv[i];
        else { fprintf(stderr, "usage: blk2csv <trace_dir> [-o csv] [-j json] [-p prefix] [-m start_marker] [-c capacity_bytes]\n"); return 2; }
    }
    if (!dir) { fprintf(stderr, "usage: blk2csv <trace_dir> [-o csv] [-j json] [-p prefix] [-m start_marker] [-c capacity_bytes]\n"); return 2; }
    char path[4096], csvbuf[4096], jsonbuf[4096];
    if (!csvp) { snprintf(csvbuf, sizeof csvbuf, "%s/trace.csv", dir); csvp = csvbuf; }
    if (!jsonp) { snprintf(jsonbuf, sizeof jsonbuf, "%s/trace.json", dir); jsonp = jsonbuf; }

    // metadata written by blkcap.sh (key=value lines), echoed into the JSON
    char *meta_k[256], *meta_v[256]; int nmeta = 0;
    snprintf(path, sizeof path, "%s/blkcap_meta.txt", dir);
    FILE *mf = fopen(path, "r");
    if (mf) {
        char line[4096];
        while (fgets(line, sizeof line, mf) && nmeta < 256) {
            char *eq = strchr(line, '='); if (!eq) continue;
            *eq = 0; char *v = eq + 1; v[strcspn(v, "\n")] = 0;
            meta_k[nmeta] = strdup(line); meta_v[nmeta] = strdup(v); nmeta++;
            if (!capacity && !strcmp(line, "device_bytes")) capacity = strtoull(v, NULL, 10);
        }
        fclose(mf);
    }
    // dropped events as printed by blktrace at exit ("Total: ... (dropped N)" per device)
    long long dropped = -1;
    snprintf(path, sizeof path, "%s/blktrace.log", dir);
    FILE *lf = fopen(path, "r");
    if (lf) {
        char line[4096];
        while (fgets(line, sizeof line, lf)) {
            char *p = strstr(line, "(dropped ");
            if (p) { if (dropped < 0) dropped = 0; dropped += strtoll(p + 9, NULL, 10); }
        }
        fclose(lf);
    }

    // input files
    DIR *d = opendir(dir); if (!d) die("cannot open %s", dir);
    char *names[1024]; int nn = 0; struct dirent *de;
    size_t pl = strlen(prefix);
    while ((de = readdir(d)) && nn < 1024)
        if (!strncmp(de->d_name, prefix, pl) && !strncmp(de->d_name + pl, ".blktrace.", 10)) names[nn++] = strdup(de->d_name);
    closedir(d);
    if (!nn) die("no %s.blktrace.* files", prefix);
    qsort(names, nn, sizeof *names, cmpstr);
    ins = calloc(nn, sizeof *ins); heap = calloc(nn, sizeof *heap);
    for (int i = 0; i < nn; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        ins[nin].path = strdup(path);
        ins[nin].f = fopen(path, "r"); if (!ins[nin].f) die("cannot open %s", path);
        setvbuf(ins[nin].f, NULL, _IOFBF, 1 << 20);
        if (rd(&ins[nin])) heap[nheap++] = nin;
        nin++;
    }
    for (int i = nheap / 2 - 1; i >= 0; i--) sift_down(i);

    ocsv = fopen(csvp, "w"); if (!ocsv) die("cannot create %s", csvp);
    if (capacity) lastseg_grow((capacity + 4095) / 4096 + 1);
    u64 t0 = 0, tlast = 0; int have_t0 = 0; int seg = 1;
    time_t wall0 = time(NULL);

    while (nheap) {
        struct in *s = &ins[heap[0]];
        const struct rec *r = &s->r;
        n_events++;
        if (!have_t0) { t0 = r->time; have_t0 = 1; }
        if (r->time > tlast) tlast = r->time;
        u32 cats = r->action >> 16, code = r->action & 0xff;
        if (cats & TC_NOTIFY) {
            if (code == TN_MESSAGE) {
                n_msg++;
                const char *txt = s->pdu; u32 n = r->pdu_len < sizeof s->pdu ? r->pdu_len : sizeof s->pdu;
                if (r->action & TA_CGROUP) { txt += 8; n = n >= 8 ? n - 8 : 0; }
                if (n >= 15 && !memcmp(txt, "dm_blkcap_probe", 15)) n_probe++;   // blkcap.sh start-up probes
                else if (nmarks < MAXMARK) {
                    struct mark *m = &marks[nmarks++];
                    size_t k = n < sizeof m->name - 1 ? n : sizeof m->name - 1;
                    memcpy(m->name, txt, k); m->name[k] = 0;
                    m->name[strcspn(m->name, "\n")] = 0;
                    while (k && (m->name[k - 1] == ' ' || m->name[k - 1] == 0)) m->name[--k] = 0;
                    m->time = r->time; m->c = cur;
                    if (seg < 255) seg++;
                }
            } else n_notes++;
        } else if (code == TA_ISSUE) {
            u64 off = r->sector * 512, len = r->bytes;
            if (cats & TC_PC) n_pc++;
            else if (cats & TC_DISCARD) { n_discard++; discard_bytes += len; }
            else if (len == 0) n_flush0++;
            else if (!(cats & (TC_READ | TC_WRITE))) n_other_issue++;
            else {
                int w = (cats & TC_WRITE) != 0;
                u64 p0 = off / 4096, p1 = (off + len + 4095) / 4096, np = p1 - p0;
                u64 ts = r->time - t0;
                if (olen > sizeof obuf - 128) oflush();
                obuf[olen++] = '0'; obuf[olen++] = ','; obuf[olen++] = w ? 'W' : 'R'; obuf[olen++] = ',';
                oput_u64(off); obuf[olen++] = ','; oput_u64(len); obuf[olen++] = ',';
                oput_u64(ts / 1000000000ull); obuf[olen++] = '.';
                { u64 f = ts % 1000000000ull; char t[9]; for (int i = 8; i >= 0; i--) { t[i] = '0' + f % 10; f /= 10; } memcpy(obuf + olen, t, 9); olen += 9; }
                obuf[olen++] = '\n';
                cur.lines++;
                if (off + len > max_end_byte) max_end_byte = off + len;
                int unal = (off % 4096) || (len % 4096);
                if (w) {
                    if (len > max_wreq_bytes) max_wreq_bytes = len;
                    cur.wreq++; cur.wpages += np; cur.wbytes += len; whist[np < HMAX ? np : HMAX - 1]++; w_unaligned += unal;
                    if (off + len > max_wend_byte) max_wend_byte = off + len;
                    lastseg_grow(p1 + 1);
                    for (u64 p = p0; p < p1; p++) { if (!lastseg[p]) cur.footprint++; lastseg[p] = (unsigned char)seg; }
                } else {
                    if (len > max_rreq_bytes) max_rreq_bytes = len;
                    cur.rreq++; cur.rpages += np; cur.rbytes += len; rhist[np < HMAX ? np : HMAX - 1]++; r_unaligned += unal;
                }
            }
        } else if (code == TA_REQUEUE) n_requeue++;
        else n_other++;
        if (!rd(s)) { heap[0] = heap[--nheap]; }
        if (nheap) sift_down(0);
    }
    oflush();
    if (fclose(ocsv)) die("close failed on %s", csvp);

    // distinct pages whose most recent write is after marker k == written after marker k
    u64 segcnt[256] = {0};
    for (u64 p = 0; p < lastseg_n; p++) segcnt[lastseg[p]]++;
    u64 after[MAXMARK + 1];
    for (int k = 0; k <= nmarks && k < MAXMARK; k++) { u64 a = 0; for (int sgi = k + 2; sgi < 256; sgi++) a += segcnt[sgi]; after[k] = a; }

    FILE *j = fopen(jsonp, "w"); if (!j) die("cannot create %s", jsonp);
    fprintf(j, "{\n  \"trace_dir\": "); json_str(j, dir);
    fprintf(j, ",\n  \"csv\": "); json_str(j, csvp);
    fprintf(j, ",\n  \"csv_format\": \"dev,op,byte_off,len_bytes,ts (dev=0, op W|R, ts s since first record, D/issue events in time order)\"");
    fprintf(j, ",\n  \"page_bytes\": 4096");
    if (capacity) fprintf(j, ",\n  \"capacity_bytes\": %llu,\n  \"capacity_pages\": %llu", (unsigned long long)capacity, (unsigned long long)(capacity / 4096));
    else fprintf(j, ",\n  \"capacity_bytes\": null,\n  \"capacity_pages\": null");
    fprintf(j, ",\n  \"duration_s\": %.6f", have_t0 ? (tlast - t0) / 1e9 : 0.0);
    fprintf(j, ",\n  \"csv_lines\": %llu", (unsigned long long)cur.lines);
    fprintf(j, ",\n  \"write_requests\": %llu,\n  \"write_pages\": %llu,\n  \"write_bytes\": %llu", (unsigned long long)cur.wreq, (unsigned long long)cur.wpages, (unsigned long long)cur.wbytes);
    fprintf(j, ",\n  \"read_requests\": %llu,\n  \"read_pages\": %llu,\n  \"read_bytes\": %llu", (unsigned long long)cur.rreq, (unsigned long long)cur.rpages, (unsigned long long)cur.rbytes);
    fprintf(j, ",\n  \"write_unaligned_requests\": %llu,\n  \"read_unaligned_requests\": %llu", (unsigned long long)w_unaligned, (unsigned long long)r_unaligned);
    fprintf(j, ",\n  \"max_byte_end\": %llu,\n  \"max_sector_end\": %llu,\n  \"max_page_touched\": %lld,\n  \"max_write_byte_end\": %llu",
            (unsigned long long)max_end_byte, (unsigned long long)(max_end_byte / 512), max_end_byte ? (long long)((max_end_byte - 1) / 4096) : -1LL, (unsigned long long)max_wend_byte);
    fprintf(j, ",\n  \"max_write_request_bytes\": %llu,\n  \"max_read_request_bytes\": %llu", (unsigned long long)max_wreq_bytes, (unsigned long long)max_rreq_bytes);
    fprintf(j, ",\n  \"footprint_write_pages_total\": %llu", (unsigned long long)cur.footprint);
    int si = -1, pi = -1;
    for (int k = 0; k < nmarks; k++) { if (si < 0 && !strcmp(marks[k].name, mstart)) si = k; if (pi < 0 && !strcmp(marks[k].name, "dm_measure_prefill_done")) pi = k; }
    int idx[2] = {si, pi};
    for (int q = 0; q < 2; q++) {
        int k = idx[q];
        const char *pre = q ? "prefill_done" : "warmup";
        if (k < 0) { fprintf(j, ",\n  \"%s_marker\": null, \"%s_write_pages\": null", pre, pre); continue; }
        const struct mark *m = &marks[k];
        fprintf(j, ",\n  \"%s_marker\": ", pre); json_str(j, m->name);
        fprintf(j, ",\n  \"%s_write_pages\": %llu,\n  \"%s_write_requests\": %llu,\n  \"%s_read_pages\": %llu,\n  \"%s_read_requests\": %llu",
                pre, (unsigned long long)m->c.wpages, pre, (unsigned long long)m->c.wreq, pre, (unsigned long long)m->c.rpages, pre, (unsigned long long)m->c.rreq);
        fprintf(j, ",\n  \"%s_csv_lines_before\": %llu,\n  \"%s_first_csv_line_after\": %llu",
                pre, (unsigned long long)m->c.lines, pre, (unsigned long long)(m->c.lines + 1));
        fprintf(j, ",\n  \"%s_ts\": %.9f", pre, (m->time - t0) / 1e9);
        fprintf(j, ",\n  \"%s_fraction_of_write_pages\": %.9f", pre, cur.wpages ? (double)m->c.wpages / cur.wpages : 0.0);
        fprintf(j, ",\n  \"%s_footprint_pages_before\": %llu,\n  \"%s_footprint_pages_after\": %llu,\n  \"%s_new_pages_after\": %llu",
                pre, (unsigned long long)m->c.footprint, pre, (unsigned long long)after[k], pre, (unsigned long long)(cur.footprint - m->c.footprint));
        fprintf(j, ",\n  \"%s_measured_write_pages\": %llu,\n  \"%s_measured_read_pages\": %llu",
                pre, (unsigned long long)(cur.wpages - m->c.wpages), pre, (unsigned long long)(cur.rpages - m->c.rpages));
    }
    fprintf(j, ",\n  \"markers\": [");
    for (int k = 0; k < nmarks; k++) {
        const struct mark *m = &marks[k];
        fprintf(j, "%s\n    {\"name\": ", k ? "," : ""); json_str(j, m->name);
        fprintf(j, ", \"ts\": %.9f, \"csv_lines_before\": %llu, \"first_csv_line_after\": %llu, \"write_requests_before\": %llu, \"write_pages_before\": %llu, \"write_bytes_before\": %llu, \"read_requests_before\": %llu, \"read_pages_before\": %llu, \"footprint_pages_before\": %llu, \"footprint_pages_after\": %llu}",
                (m->time - t0) / 1e9, (unsigned long long)m->c.lines, (unsigned long long)(m->c.lines + 1), (unsigned long long)m->c.wreq,
                (unsigned long long)m->c.wpages, (unsigned long long)m->c.wbytes, (unsigned long long)m->c.rreq, (unsigned long long)m->c.rpages,
                (unsigned long long)m->c.footprint, (unsigned long long)(k < MAXMARK ? after[k] : 0));
    }
    fprintf(j, "%s]", nmarks ? "\n  " : "");
    for (int q = 0; q < 2; q++) {
        u64 *h = q ? rhist : whist;
        fprintf(j, ",\n  \"%s_size_hist_pages\": {", q ? "read" : "write");
        int first = 1;
        for (int k = 0; k < HMAX; k++) if (h[k]) { fprintf(j, "%s\"%d%s\": %llu", first ? "" : ", ", k, k == HMAX - 1 ? "+" : "", (unsigned long long)h[k]); first = 0; }
        fprintf(j, "}");
    }
    fprintf(j, ",\n  \"skipped\": {\"flush_or_zero_length\": %llu, \"discard_requests\": %llu, \"discard_bytes\": %llu, \"passthrough\": %llu, \"issue_without_rw\": %llu}",
            (unsigned long long)n_flush0, (unsigned long long)n_discard, (unsigned long long)discard_bytes, (unsigned long long)n_pc, (unsigned long long)n_other_issue);
    fprintf(j, ",\n  \"requeue_events\": %llu,\n  \"message_events\": %llu,\n  \"probe_messages\": %llu,\n  \"other_notes\": %llu,\n  \"other_events\": %llu,\n  \"events_total\": %llu",
            (unsigned long long)n_requeue, (unsigned long long)n_msg, (unsigned long long)n_probe, (unsigned long long)n_notes, (unsigned long long)n_other, (unsigned long long)n_events);
    if (dropped >= 0) fprintf(j, ",\n  \"dropped_events\": %lld", dropped); else fprintf(j, ",\n  \"dropped_events\": null");
    fprintf(j, ",\n  \"input_files\": %d,\n  \"input_files_with_records\": %d", nin, ({ int c = 0; for (int i = 0; i < nin; i++) c += ins[i].nrec > 0; c; }));
    fprintf(j, ",\n  \"meta\": {");
    for (int k = 0; k < nmeta; k++) { fprintf(j, "%s", k ? ", " : ""); json_str(j, meta_k[k]); fprintf(j, ": "); json_str(j, meta_v[k]); }
    fprintf(j, "}\n}\n");
    if (fclose(j)) die("close failed on %s", jsonp);
    fprintf(stderr, "blk2csv: %llu events -> %llu CSV lines (W %llu req / %llu pages, R %llu req / %llu pages), %d markers (+%llu probes), dropped %lld, %lds\n",
            (unsigned long long)n_events, (unsigned long long)cur.lines, (unsigned long long)cur.wreq, (unsigned long long)cur.wpages,
            (unsigned long long)cur.rreq, (unsigned long long)cur.rpages, nmarks, (unsigned long long)n_probe, dropped, (long)(time(NULL) - wall0));
    return 0;
}
