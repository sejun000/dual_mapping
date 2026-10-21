// trace_prep: turn a REFlash-style rescaled block trace back into a single-copy write trace
// and pack it into a footprint-sized device for ftlsim's trace= replay.
//
//   trace_prep in.csv out.csv [stride=BYTES] [unscale=N] [region=BYTES] > stats.json
//
//   stride=S   the input replicates every request at offsets off + i*S (make_*x_*.py);
//              keep only the first copy (off < S). 0 = no replication.
//   unscale=N  the input multiplied both offset and length by N (scale_ssdtrace.py); divide back.
//   reads=1    also emit reads (remapped; reads outside write-touched regions are dropped)
//   region=R   touched R-byte regions (default 1 MiB) are packed densely in address order, so
//              offsets inside a region, IU alignment and adjacency of touched regions are kept
//              while the untouched address space is dropped. Device capacity = #regions * R.
//   sample=k/K spatial sampling (SHARDS-style): keep source region r iff hash(r) mod K < k, so each kept region keeps
//              its full access stream and in-region layout; requests are cut at region boundaries and the parts in
//              dropped regions are discarded. Selects the segment mode below.
//   order=touch number kept regions in first-write-touch order (the order ftlsim's own dense remap assigns, so ftlsim
//              replays the file with an identity remap and a block-device replayer sees the same LBAs). order=addr
//              (default) keeps address order. order=touch also selects the segment mode.
//   seed=N     hash seed for sample= (default 0).
//
// Segment mode (sample= or order=touch): every request is rounded out to 4 KiB pages first (as ftlsim does), cut per
// region, and kept segments that stay contiguous after the remap are merged back into one output request; reads are
// kept per segment when the region was write-touched (instead of whole-request drop). Statistics describe the output.
//
// Input/output CSV: dev,op,byte_off,len_bytes[,ts]. Output holds writes only (plus reads with reads=1).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using u64 = uint64_t;

struct Args { std::string in, out; u64 stride = 0, unscale = 1, region = 1ull << 20; bool reads = false;
              u64 samp_k = 0, samp_K = 0, seed = 0; bool touch = false; };

static bool next_io(FILE* f, const Args& a, u64& off, u64& len, bool want_reads, char* opc) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char op[8]; long long o, l;
        if (sscanf(line, "%*[^,],%7[^,],%lld,%lld", op, &o, &l) != 3) continue;
        if (l <= 0 || !(op[0] == 'W' || (want_reads && op[0] == 'R'))) continue;
        *opc = op[0];
        if (a.stride && (u64)o >= a.stride) continue;  // replica
        off = (u64)o / a.unscale; len = (u64)l / a.unscale;
        if (len == 0) continue;
        return true;
    }
    return false;
}
static bool next_write(FILE* f, const Args& a, u64& off, u64& len) { char c; return next_io(f, a, off, len, false, &c); }

static inline u64 mix64(u64 x) {   // splitmix64 finalizer: deterministic, well mixed
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static int segment_mode(const Args& a) {
    const u64 PG = 4096, RPG = a.region / PG, NONE = ~0ull;
    if (a.region % PG || !RPG) { fprintf(stderr, "region must be a multiple of 4096\n"); return 1; }
    auto keep = [&](u64 r) { return !a.samp_K || mix64(r ^ a.seed) % a.samp_K < a.samp_k; };
    // pass 1: which source regions are written (all / kept), and the dense number of each kept one
    FILE* f = fopen(a.in.c_str(), "r");
    if (!f) { perror("in"); return 1; }
    std::vector<u64> remap;            // source region -> dense region (NONE = not kept or never written)
    std::vector<uint8_t> touched;      // source region written at all (before sampling)
    u64 nreg = 0, src_regions = 0, src_wpages = 0, off, len;
    while (next_write(f, a, off, len)) {
        u64 p0 = off / PG, p1 = (off + len + PG - 1) / PG;
        src_wpages += p1 - p0;
        u64 r0 = p0 / RPG, r1 = (p1 - 1) / RPG;
        if (r1 + 1 > remap.size()) { u64 n = std::max(r1 + 1, remap.size() * 2); remap.resize(n, NONE); touched.resize(n, 0); }
        for (u64 r = r0; r <= r1; ++r) {
            if (!touched[r]) { touched[r] = 1; src_regions++; }
            if (remap[r] == NONE && keep(r)) remap[r] = a.touch ? nreg++ : 0;
        }
    }
    fclose(f);
    if (!a.touch) for (u64 r = 0; r < remap.size(); ++r) if (remap[r] != NONE) remap[r] = nreg++;
    const u64 L = nreg * RPG;

    // pass 2: emit, statistics on the output stream
    f = fopen(a.in.c_str(), "r");
    FILE* o = fopen(a.out.c_str(), "w");
    if (!f || !o) { perror("pass2"); return 1; }
    static char obuf[1 << 20]; setvbuf(o, obuf, _IOFBF, sizeof obuf);
    std::vector<uint8_t> pages((L >> 3) + 1, 0);
    std::vector<u64> regw(nreg, 0);
    u64 nw = 0, wpages = 0, nr = 0, rpages = 0, rdrop_pages = 0, seq = 0, prev_end = NONE, max_pages = 0;
    u64 hist[66] = {0}, histp[66] = {0}, full_iu[2] = {0, 0}, rhist[66] = {0};
    char opc;
    auto emit = [&](char op, u64 d0, u64 n) {
        fprintf(o, "0,%c,%llu,%llu\n", op, (unsigned long long)(d0 * PG), (unsigned long long)(n * PG));
        max_pages = std::max(max_pages, n);
        int k = n >= 64 ? 65 : (int)n;
        if (op == 'R') { nr++; rpages += n; rhist[k]++; return; }
        nw++; wpages += n; hist[k]++; histp[k] += n;
        if (d0 == prev_end) seq++;
        prev_end = d0 + n;
        for (int u = 0; u < 2; ++u) {
            u64 U = u ? 16 : 8, q0 = (d0 + U - 1) / U * U, q1 = (d0 + n) / U * U;
            if (q1 > q0) full_iu[u] += q1 - q0;
        }
        for (u64 p = d0; p < d0 + n; ++p) { pages[p >> 3] |= 1 << (p & 7); regw[p / RPG]++; }
    };
    while (next_io(f, a, off, len, a.reads, &opc)) {
        u64 p0 = off / PG, p1 = (off + len + PG - 1) / PG, run0 = NONE, runn = 0;
        for (u64 cur = p0; cur < p1; ) {
            u64 r = cur / RPG, se = std::min(p1, (r + 1) * RPG), n = se - cur;
            u64 d = r < remap.size() ? remap[r] : NONE;
            if (d == NONE) {                                   // dropped by sampling, or a read of a never-written region
                if (runn) { emit(opc, run0, runn); runn = 0; }
                if (opc == 'R' && (r >= touched.size() || !touched[r]) && keep(r)) rdrop_pages += n;   // sampled-out pages not counted
            } else {
                u64 d0 = d * RPG + (cur - r * RPG);
                if (runn && run0 + runn == d0) runn += n;
                else { if (runn) emit(opc, run0, runn); run0 = d0; runn = n; }
            }
            cur = se;
        }
        if (runn) emit(opc, run0, runn);
    }
    fclose(f); fclose(o);

    u64 foot = 0;
    for (uint8_t b : pages) foot += __builtin_popcount(b);
    std::vector<u64> w(regw); std::sort(w.rbegin(), w.rend());
    u64 tot = 0; for (u64 x : w) tot += x;
    auto top = [&](double frac) { u64 n = std::max<u64>(1, (u64)(w.size() * frac)), s = 0; for (u64 i = 0; i < n && i < w.size(); ++i) s += w[i]; return tot ? (double)s / tot : 0.0; };
    const double G = 1ull << 30;
    u64 cap = nreg * a.region;
    printf("{\n \"in\": \"%s\", \"out\": \"%s\", \"stride\": %llu, \"unscale\": %llu, \"region\": %llu,\n",
           a.in.c_str(), a.out.c_str(), (unsigned long long)a.stride, (unsigned long long)a.unscale, (unsigned long long)a.region);
    printf(" \"sample\": \"%llu/%llu\", \"sample_rate\": %.6f, \"seed\": %llu, \"order\": \"%s\", \"reads_included\": %d,\n",
           (unsigned long long)(a.samp_K ? a.samp_k : 1), (unsigned long long)(a.samp_K ? a.samp_K : 1), a.samp_K ? (double)a.samp_k / a.samp_K : 1.0,
           (unsigned long long)a.seed, a.touch ? "touch" : "addr", a.reads ? 1 : 0);
    printf(" \"source_regions\": %llu, \"source_write_gib\": %.2f, \"kept_regions\": %llu, \"kept_region_frac\": %.4f,\n",
           (unsigned long long)src_regions, src_wpages * 4096.0 / G, (unsigned long long)nreg, src_regions ? (double)nreg / src_regions : 0.0);
    printf(" \"writes\": %llu, \"write_pages\": %llu, \"write_gib\": %.2f, \"reads\": %llu, \"read_pages\": %llu, \"read_gib\": %.2f, \"read_dropped_pages\": %llu,\n",
           (unsigned long long)nw, (unsigned long long)wpages, wpages * 4096.0 / G, (unsigned long long)nr, (unsigned long long)rpages, rpages * 4096.0 / G,
           (unsigned long long)rdrop_pages);
    printf(" \"footprint_gib\": %.2f, \"capacity_bytes\": %llu, \"capacity_gib\": %.2f, \"max_req_bytes\": %llu,\n",
           foot * 4096.0 / G, (unsigned long long)cap, cap / G, (unsigned long long)(max_pages * PG));
    printf(" \"turnovers\": %.2f, \"footprint_frac_of_capacity\": %.3f, \"seq_frac\": %.4f,\n",
           cap ? (double)wpages * 4096 / cap : 0.0, cap ? foot * 4096.0 / cap : 0.0, nw ? (double)seq / nw : 0.0);
    printf(" \"top1pct_region_share\": %.4f, \"top10pct_region_share\": %.4f,\n", top(0.01), top(0.10));
    printf(" \"pages_in_full_32K_IU\": %.4f, \"pages_in_full_64K_IU\": %.4f,\n", wpages ? (double)full_iu[0] / wpages : 0.0, wpages ? (double)full_iu[1] / wpages : 0.0);
    auto pr = [&](const char* name, const u64* h, u64 den, bool last) {
        printf(" \"%s\": {", name);
        const char* sep = "";
        for (int k = 1; k < 66; ++k) if (h[k] && (k == 65 || (k & (k - 1)) == 0 || h[k] * 50 > den)) {
            printf("%s\"%s%dK\": %.4f", sep, k == 65 ? ">=" : "", k == 65 ? 256 : 4 * k, (double)h[k] / den); sep = ", ";
        }
        printf("}%s\n", last ? "" : ",");
    };
    pr("size_req_frac", hist, std::max<u64>(1, nw), false);
    pr("size_page_frac", histp, std::max<u64>(1, wpages), false);
    pr("read_size_req_frac", rhist, std::max<u64>(1, nr), true);
    printf("}\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: trace_prep in out [stride=] [unscale=] [region=]\n"); return 1; }
    Args a; a.in = argv[1]; a.out = argv[2];
    for (int i = 3; i < argc; ++i) {
        const char* s = argv[i];
        if (!strncmp(s, "stride=", 7)) a.stride = strtoull(s + 7, nullptr, 10);
        else if (!strncmp(s, "unscale=", 8)) a.unscale = strtoull(s + 8, nullptr, 10);
        else if (!strncmp(s, "region=", 7)) a.region = strtoull(s + 7, nullptr, 10);
        else if (!strncmp(s, "reads=", 6)) a.reads = atoi(s + 6) != 0;
        else if (!strncmp(s, "sample=", 7)) {
            if (sscanf(s + 7, "%llu/%llu", (unsigned long long*)&a.samp_k, (unsigned long long*)&a.samp_K) != 2 || !a.samp_K || a.samp_k > a.samp_K) {
                fprintf(stderr, "bad sample=%s (want k/K)\n", s + 7); return 1; }
        }
        else if (!strncmp(s, "order=", 6)) {
            if (!strcmp(s + 6, "touch")) a.touch = true; else if (strcmp(s + 6, "addr")) { fprintf(stderr, "bad order\n"); return 1; }
        }
        else if (!strncmp(s, "seed=", 5)) a.seed = strtoull(s + 5, nullptr, 10);
        else { fprintf(stderr, "bad arg %s\n", s); return 1; }
    }
    const u64 PG = 4096;
    if (a.samp_K || a.touch) return segment_mode(a);

    // pass 1: footprint, regions, request-shape statistics
    FILE* f = fopen(a.in.c_str(), "r");
    if (!f) { perror("in"); return 1; }
    std::vector<uint8_t> pages;    // 4K footprint bitmap
    std::vector<u64> regw;         // writes (pages) per region
    u64 nw = 0, wpages = 0, seq = 0, prev_end = ~0ull, unaligned = 0;
    u64 hist[66] = {0}, histp[66] = {0};     // by request size in pages (65 = >=256K): count, pages
    u64 full_iu[2] = {0, 0};                 // pages inside fully covered aligned 32K / 64K IUs
    u64 off, len;
    while (next_write(f, a, off, len)) {
        u64 p0 = off / PG, p1 = (off + len + PG - 1) / PG, n = p1 - p0;
        if (off % PG || len % PG) unaligned++;
        nw++; wpages += n;
        int k = n >= 64 ? 65 : (int)n; hist[k]++; histp[k] += n;
        if (off == prev_end) seq++;
        prev_end = off + len;
        for (int u = 0; u < 2; ++u) {
            u64 U = u ? 16 : 8, q0 = (p0 + U - 1) / U * U, q1 = p1 / U * U;
            if (q1 > q0) full_iu[u] += q1 - q0;
        }
        if ((p1 >> 3) + 1 > pages.size()) pages.resize(std::max((p1 >> 3) + 1, pages.size() * 2));
        for (u64 p = p0; p < p1; ++p) pages[p >> 3] |= 1 << (p & 7);
        u64 r0 = off / a.region, r1 = (off + len - 1) / a.region;
        if (r1 + 1 > regw.size()) regw.resize(std::max(r1 + 1, regw.size() * 2));
        for (u64 r = r0; r <= r1; ++r) regw[r] += n;   // approx: whole request credited to each region it spans
    }
    fclose(f);

    std::vector<u64> remap(regw.size(), ~0ull);
    u64 nreg = 0;
    for (u64 r = 0; r < regw.size(); ++r) if (regw[r]) remap[r] = nreg++;
    u64 foot = 0;
    for (uint8_t b : pages) foot += __builtin_popcount(b);
    std::vector<u64> w; w.reserve(nreg);
    for (u64 x : regw) if (x) w.push_back(x);
    std::sort(w.rbegin(), w.rend());
    u64 tot = 0; for (u64 x : w) tot += x;
    auto top = [&](double frac) { u64 n = std::max<u64>(1, (u64)(w.size() * frac)), s = 0; for (u64 i = 0; i < n; ++i) s += w[i]; return (double)s / tot; };

    // pass 2: emit packed writes
    f = fopen(a.in.c_str(), "r");
    FILE* o = fopen(a.out.c_str(), "w");
    if (!f || !o) { perror("pass2"); return 1; }
    static char obuf[1 << 20]; setvbuf(o, obuf, _IOFBF, sizeof obuf);
    char opc; u64 nreads = 0, dropped = 0;
    while (next_io(f, a, off, len, a.reads, &opc)) {
        u64 r = off / a.region, r1 = (off + len - 1) / a.region;
        if (opc == 'R') {                                   // reads: only inside regions that writes touched
            bool ok = r1 < remap.size();
            for (u64 q = r; ok && q <= r1; ++q) ok = remap[q] != ~0ull && remap[q] == remap[r] + (q - r);
            if (!ok) { dropped++; continue; }
            nreads++;
        }
        u64 noff = remap[r] * a.region + off % a.region;   // touched regions are contiguous in rank, so spans stay contiguous
        fprintf(o, "0,%c,%llu,%llu\n", opc, (unsigned long long)noff, (unsigned long long)len);
    }
    if (a.reads) fprintf(stderr, "reads kept %llu, dropped (outside written regions) %llu\n", (unsigned long long)nreads, (unsigned long long)dropped);
    fclose(f); fclose(o);

    u64 cap = nreg * a.region;
    printf("{\n \"in\": \"%s\", \"out\": \"%s\", \"stride\": %llu, \"unscale\": %llu, \"region\": %llu,\n",
           a.in.c_str(), a.out.c_str(), (unsigned long long)a.stride, (unsigned long long)a.unscale, (unsigned long long)a.region);
    printf(" \"writes\": %llu, \"write_gib\": %.2f, \"footprint_gib\": %.2f, \"capacity_bytes\": %llu, \"capacity_gib\": %.2f,\n",
           (unsigned long long)nw, wpages * 4096.0 / (1ull << 30), foot * 4096.0 / (1ull << 30), (unsigned long long)cap, cap / (double)(1ull << 30));
    printf(" \"turnovers\": %.2f, \"footprint_frac_of_capacity\": %.3f, \"seq_frac\": %.4f, \"unaligned\": %llu,\n",
           (double)wpages * 4096 / cap, foot * 4096.0 / cap, (double)seq / nw, (unsigned long long)unaligned);
    printf(" \"top1pct_region_share\": %.4f, \"top10pct_region_share\": %.4f,\n", top(0.01), top(0.10));
    printf(" \"pages_in_full_32K_IU\": %.4f, \"pages_in_full_64K_IU\": %.4f,\n", (double)full_iu[0] / wpages, (double)full_iu[1] / wpages);
    printf(" \"size_req_frac\": {");
    const char* sep = "";
    for (int k = 1; k < 66; ++k) if (hist[k] && (k == 65 || (k & (k - 1)) == 0 || hist[k] * 50 > nw)) {
        printf("%s\"%s%dK\": %.4f", sep, k == 65 ? ">=" : "", k == 65 ? 256 : 4 * k, (double)hist[k] / nw); sep = ", ";
    }
    printf("},\n \"size_page_frac\": {");
    sep = "";
    for (int k = 1; k < 66; ++k) if (histp[k] && (k == 65 || (k & (k - 1)) == 0 || histp[k] * 50 > wpages)) {
        printf("%s\"%s%dK\": %.4f", sep, k == 65 ? ">=" : "", k == 65 ? 256 : 4 * k, (double)histp[k] / wpages); sep = ", ";
    }
    printf("}\n}\n");
    return 0;
}
