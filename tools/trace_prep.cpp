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
//
// Input/output CSV: dev,op,byte_off,len_bytes[,ts]. Output holds writes only.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using u64 = uint64_t;

struct Args { std::string in, out; u64 stride = 0, unscale = 1, region = 1ull << 20; bool reads = false; };

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

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: trace_prep in out [stride=] [unscale=] [region=]\n"); return 1; }
    Args a; a.in = argv[1]; a.out = argv[2];
    for (int i = 3; i < argc; ++i) {
        const char* s = argv[i];
        if (!strncmp(s, "stride=", 7)) a.stride = strtoull(s + 7, nullptr, 10);
        else if (!strncmp(s, "unscale=", 8)) a.unscale = strtoull(s + 8, nullptr, 10);
        else if (!strncmp(s, "region=", 7)) a.region = strtoull(s + 7, nullptr, 10);
        else if (!strncmp(s, "reads=", 6)) a.reads = atoi(s + 6) != 0;
        else { fprintf(stderr, "bad arg %s\n", s); return 1; }
    }
    const u64 PG = 4096;

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
