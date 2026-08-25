// Dual-Granularity FTL Simulator (v1)
// Terminology: IU = 64K logical unit; chunk = its 16-page coarse copy; fragment = 4K copy in the fine tier.
//   fold = fragments (+chunk) -> new chunk; refold = fold done for free inside coarse GC; unfold = chunk -> fragments (hot IU).
//   Policy view: fine tier is a hot-IU cache; unfold = promote, fold = demote.
// Modes: proposed (IU coarse map + 4K fine tier), page4k, iu_rmw (IU map + RMW), dftl (cached page map),
//        fast, last (log-block hybrids), mu (mu-FTL extent mapping)
// See SPEC.md.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <list>
#include <map>
#include <cmath>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;
static const u32 NONE = 0xFFFFFFFFu;

// RAM model for hash-managed maps: open addressing, load factor 0.7
static const double HASH_LF = 0.7;
static inline double hash_bytes(double entries, double entry_bytes) { return entries * entry_bytes / HASH_LF; }

#define DIE(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); fprintf(stderr, "\n"); abort(); } while (0)

// ----------------------------------------------------------------------------
struct Cfg {
    u64 capacity = 64ull << 30;   // logical bytes
    int P = 0;                    // pages per block; 0 = auto: capacity * 3/1024 (4 TiB : 12 GiB superblock ratio -> 192 MiB @ 64 GiB)
    int U = 16;                   // pages per IU
    double op = 0.07;
    double fine_quota = 0.022;    // fraction of base blocks (8 x 192 MiB @ 64 GiB -> mapping RAM = 10% of page4k, fixed)
    int T = 2;                    // free threshold / reserve
    std::string mode = "proposed";
    double zipf = 0.0;
    int min_k = 0, max_k = 6;     // request size = 2^k pages, log-uniform by request count
    u64 wbuf_pages = 256;         // device write buffer (default 1 MiB) (pages): coalesce fragments per IU; full IU -> aligned write, LRU IU evicted as fragments. 0 = off
    bool seq_pattern = false;     // sequential host writes (request starts advance sequentially, wrap at end) instead of zipf/uniform
    std::string size_weights;     // optional comma list of weights for k=min_k..max_k (e.g. "64,32,16,8,4,2,1" = small-heavy)
    u64 seed = 1;
    double age_x = 8.0;           // workload aging multiples of capacity (after seq fill)
    double measure_x = 1.0;       // measured workload multiples of capacity
    bool seq_fill = true;
    u64 ts_interval = 0;          // host pages between time-series samples (0 = capacity/200)
    std::string out_ts;           // csv path for time-series
    std::string tag;              // free-form label echoed in output
    bool fine_extra = true;       // fine quota is physical space ADDED on top of OP (else carved from OP)
    bool gc_frontier = true;      // separate active blocks for GC/reclaim writes vs host writes (hot/cold separation)
    bool zipf_shuffle = false;
    std::string fine_cap = "blocks"; // proposed: blocks (fixed fine block quota) | ram (no block quota; fine map capped by RAM budget, global greedy GC across tiers)
    u64 fine_budget_bytes = 0;    // mixed/ram: RAM budget for the fine map (0 = 10% of page4k L2P minus coarse map)
    std::string fine_policy = "cost";  // fine victim handling: cost (default, parameter-free: fold IU iff 16/d < v/(P-v)) | cold (pressure-gated coldest-first) | greedy (d>=fold_d) | block (whole-block cost test)
                                       // | fifo_fold (oldest fine block, fold all) | greedy_copy (min-valid block, copy only) | alt (alternate fifo_fold / greedy_copy 1:1)
                                       // | cost (per-IU, parameter-free: fold iff 16/d < v/(P-v)) | cost_cold (cost + fine-cold filter)
    std::string unfold = "off";   // coarse GC: off | hot (unfold hot IUs: move remaining chunk pages to fine, kill chunk)
    double unfold_tau = 0.5;      // hot if last fragment write within unfold_tau x (fine tier capacity) of fine host writes
    int unfold_k = 2;             // ... and at least unfold_k host fragment writes within that window (frequency; 1 = recency only)
    int eager_u = -1;             // proposed: -1 = cost-based (default) (see eager_worth); >0 = fixed threshold: if the IU would end up with > eager_u
                                  // of its 16 pages present (new pages + existing fragments), complete it to a full
                                  // chunk right away (selective RMW) instead of appending fragments. 0 = off.
    int fold_d = 8;               // greedy/cold: fold an IU only when it has >= fold_d fragments (half a chunk)
    double fold_tau = 0.5;        // cold policy: IU is cold if its last fragment write is older than fold_tau x (fine tier capacity)
    double fold_gate = 0.0;       // cold policy: fold only when fine victim valid ratio v/P >= fold_gate (fine-pressure gate; 0 = always)
    // FAST / LAST baselines
    double last_seq_frac = 0.25;  // fraction of log blocks used as sequential log buffer (LAST)
    u64 last_seq_thresh = 4;      // request size (pages) > thresh -> sequential log buffer (LAST; paper 4KB@2KB pages; 16KB best here)
    double last_hot_frac = 0.5;   // initial hot partition fraction of the random log buffer (LAST)
    bool last_fixed_caps = true;  // disable the paper's reclaimed-block reassignment (fixed partition sizes)
    u64 cache_bytes = 12ull << 20; // dftl / mu: RAM for cached mapping (translation-page cache / mu-tree cache)
    u64 bitmap_cache_bytes = 32768; // mu: bitmap (invalid-extent) cache
    int mu_parts = 8;             // mu: logical address space partitions (each has its own update block)
    int mu_max_extent = 128;      // mu: max extent length (pages)
    int mu_leaf_fanout = 256;     // mu: extents per mu-tree leaf page (one 4K page holds the path; leaf ~ half)
    double janus_u = 0.7;         // janus: PMA utilization target (defuse when above, else GC)
    int janus_cap = 0;            // janus: max fused blocks (0 = from 10% RAM budget; -1 = unlimited)
    u64 last_k = 0;               // hot update-interval threshold in pages (0 = auto: hot partition capacity)    // true: random rank->IU permutation (hot IUs scattered); false: rank==IU (hot region contiguous)
};

enum Src { S_HOST = 0, S_FINEGC, S_FOLD, S_CGC_CLEAN, S_CGC_REFOLD, S_RMW, S_PGC, S_ZEROFILL, S_FULLMERGE, S_PARTIALMERGE, S_MAP, S_UNFOLD, S_N };
static const char* SRC_NAME[S_N] = {"host", "finegc", "fold", "cgc_clean", "cgc_refold", "rmw", "pgc", "zerofill", "fullmerge", "partialmerge", "map", "unfold"};

struct Stats {
    u64 host_pages = 0;        // host pages that reached the FTL (after write-buffer absorption)
    u64 host_submitted = 0;   // host pages the workload issued (WAF denominator)
    u64 prog[S_N] = {};
    u64 read_rmw = 0, read_fold_fine = 0, read_fold_coarse = 0, read_gc_copy = 0, read_map = 0;
    u64 map_flush = 0, map_miss = 0;
    u64 erases = 0, erase_fine = 0, erase_coarse = 0;
    u64 n_finegc = 0, n_blockfold = 0, n_fold = 0, n_refold = 0, n_cgc = 0, n_pgc = 0;
    u64 cgc_dead_extents = 0, cgc_clean_extents = 0, n_extent_drop = 0;
    u64 finegc_valid_sum = 0, cgc_valid_sum = 0, pgc_valid_sum = 0;
    std::vector<u64> d_fold, d_refold;  // histogram over d
    u64 fine_pages_valid_peak = 0, wasted_slots = 0;
    u64 n_greedy = 0, greedy_fold_pages = 0, greedy_copy_pages = 0, n_forced_fold = 0;
    u64 n_eager = 0, eager_fill = 0;
    u64 frag_folded = 0, frag_freed = 0;   // fine pages retired by an explicit fold vs. for free (refold / overwrite)
    u64 host_fine = 0, host_coarse = 0;  // host pages by tier
    u64 n_unfold = 0, unfold_pages = 0; std::vector<u64> d_unfold;
    u64 n_budget_fold = 0, n_gcfold = 0;
    u64 n_switch = 0, n_partial = 0, n_full = 0, n_dead_reclaim = 0, n_merge_reclaim = 0, full_assoc_sum = 0, n_seq_req = 0, n_rand_req = 0, n_hot_pages = 0, n_cold_pages = 0;
    u64 total_prog() const { u64 s = 0; for (auto p : prog) s += p; return s; }
    void reset(int U) { *this = Stats(); d_fold.assign(U + 1, 0); d_refold.assign(U + 1, 0); d_unfold.assign(U + 1, 0); }
};

enum Tag : uint8_t { B_FREE = 0, B_FINE = 1, B_COARSE = 2 };

struct Frontier { int blk = -1; int next = 0; };

struct Sim {
    Cfg c;
    u64 L;          // logical pages
    u64 NP;         // physical pages
    int NB;         // blocks
    u64 NIU;        // number of IUs
    int P, U;
    int fine_quota_blocks;

    // maps
    std::vector<u32> coarse_map;   // IU -> extent start ppn
    std::vector<u32> pmap;         // page4k: lpn -> ppn
    std::vector<int32_t> fine_idx; // IU -> fine pool index or -1
    std::vector<u32> fbitmap;      // pool idx -> bitmap
    std::vector<u32> fslot;        // pool idx*U + off -> ppn
    std::vector<u64> flast;        // pool idx -> fine-host-write clock at last host fragment write (unfold hotness)
    std::vector<uint8_t> fcnt;     // pool idx -> host *requests* that wrote fragments to this IU within the hotness window (saturating)
    std::vector<u32> freq;         // pool idx -> id of the last host request that touched this IU (sim-only; dedups pages of one request)
    u32 req_id = 0;                // host request counter
    u64 fine_clock = 0;            // cumulative host fine pages (not reset)
    std::vector<int32_t> fpool_free;
    // physical
    std::vector<u32> p2l;          // ppn -> lpn
    std::vector<Tag> btag;
    std::vector<int32_t> bvalid;   // valid 4K pages per block
    std::vector<u64> bseq;         // block allocation sequence (FIFO victim)
    u64 alloc_seq = 0; u64 alt_turn = 0;
    std::vector<int32_t> freelist;
    int fine_blocks = 0, coarse_blocks = 0;
    Frontier ff, cf;               // fine / coarse host frontiers
    Frontier fgf, cgf;             // fine / coarse GC frontiers (used when gc_frontier)
    bool in_cgc = false, in_fine_reclaim = false;
    bool fold_allowed = true;      // cold policy: set per victim by the pressure gate
    double last_fine_v = 0;        // EWMA of the fine victim's valid pages (fine-tier pressure signal)
    // ---- mixed mode: one pool (no fine/coarse block tags), fine map capped by RAM budget, single greedy GC
    bool mixed = false;
    bool ram_cap = false;          // proposed + fine_cap=ram
    double fine_budget = 0;                       // bytes
    std::vector<std::unordered_set<u32>> by_d;    // d -> IUs with that many fragments (for budget eviction)
    u64 fine_ius_n = 0, fine_frags_n = 0;
    double fine_mem() const { return hash_bytes((double)fine_ius_n, 4 + 2 + 4) + 4.0 * fine_frags_n; }
    void track_d(u64 iu, int oldd, int newd) {
        if (!mixed && !ram_cap) return;
        if (oldd > 0) by_d[oldd].erase((u32)iu);
        if (newd > 0) by_d[newd].insert((u32)iu);
        fine_frags_n += newd - oldd;
        if (oldd == 0 && newd > 0) fine_ius_n++;
        if (oldd > 0 && newd == 0) fine_ius_n--;
    }
    int victim_v = 0;              // cost policy: valid pages of the current fine victim
    Stats st;
    FILE* ts = nullptr;
    u64 ts_next = 0, ts_interval = 0;
    bool measuring = false;

    std::mt19937_64 rng;

    Sim(const Cfg& cfg) : c(cfg) {
        P = c.P; U = c.U;
        if (P % U) DIE("P must be multiple of U");
        L = c.capacity / 4096;
        L -= L % U;
        NIU = L / U;
        NP = (u64)((double)L * (1.0 + c.op));
        NB = (int)(NP / P);
        if (c.mode == "proposed") {
            fine_quota_blocks = std::max(1, (int)(NB * c.fine_quota));
            if (c.fine_extra) NB += fine_quota_blocks;   // fine tier is extra physical space
            if (c.fine_cap == "ram") {
                ram_cap = true; fine_quota_blocks = NB;    // no block quota
                fine_budget = c.fine_budget_bytes ? (double)c.fine_budget_bytes : 0.10 * (double)L * 4.0 - (double)NIU * 4.0;
                by_d.resize(U + 1);
            }
        } else if (c.mode == "mixed") {
            mixed = true;
            fine_quota_blocks = std::max(1, (int)(NB * c.fine_quota));
            if (c.fine_extra) NB += fine_quota_blocks;   // same physical space as proposed
            fine_quota_blocks = NB;                       // no block quota: fine map capped by RAM instead
            fine_budget = c.fine_budget_bytes ? (double)c.fine_budget_bytes : 0.10 * (double)L * 4.0 - (double)NIU * 4.0;
            by_d.resize(U + 1);
        } else fine_quota_blocks = NB;  // page4k/cftl: no limit; iu_rmw: unused
        NP = (u64)NB * P;
        coarse_map.assign(NIU, NONE);
        if (is_pg()) { pmap.assign(L, NONE); cmt_cap = (u64)(c.cache_bytes * HASH_LF / 8.0); }
        fine_idx.assign(NIU, -1);
        p2l.assign(NP, NONE);
        btag.assign(NB, B_FREE);
        bvalid.assign(NB, 0); bseq.assign(NB, 0);
        freelist.reserve(NB);
        for (int b = NB - 1; b >= 0; --b) freelist.push_back(b);
        st.reset(U);
        rng.seed(c.seed);
        if (mixed || ram_cap) fprintf(stderr, "[cfg] %s: fine map budget = %.2f MiB\n", mixed ? "mixed" : "ram-cap", fine_budget / (1 << 20));
        fprintf(stderr, "[cfg] mode=%s L=%llu pages (%.1f GB) NP=%llu NB=%d P=%d U=%d OP=%.3f fine_quota=%d blocks (extra=%d) T=%d zipf=%.2f\n",
                c.mode.c_str(), (unsigned long long)L, L * 4096.0 / (1 << 30), (unsigned long long)NP, NB, P, U, c.op,
                fine_quota_blocks, (int)c.fine_extra, c.T, c.zipf);
    }

    // ------------------------------------------------------------------ helpers
    inline int blk_of(u32 ppn) const { return (int)(ppn / P); }
    inline int popc(u32 x) const { return __builtin_popcount(x); }
    bool is_pg() const { return c.mode == "page4k" || c.mode == "dftl"; }

    // ---- DFTL (Gupta et al., ASPLOS'09) cached page mapping (CFTL's write path is identical): tier-2 translation pages (1024 entries) in flash, CMT in RAM (LRU).
    static const int TP_ENTRIES = 1024;
    u64 cmt_cap = 0;
    std::list<u64> cmt_lru;                                     // front = MRU
    std::unordered_map<u64, std::pair<std::list<u64>::iterator, bool>> cmt;  // lpn -> (lru pos, dirty)
    void cmt_evict_one() {
        u64 victim = cmt_lru.back(); cmt_lru.pop_back();
        auto it = cmt.find(victim); bool dirty = it->second.second; cmt.erase(it);
        if (!dirty) return;
        // batch update: write back the translation page, cleaning every cached dirty entry of the same page
        u64 tp = victim / TP_ENTRIES;
        st.read_map++; st.prog[S_MAP]++; st.map_flush++;
        for (u64 l = tp * TP_ENTRIES; l < (tp + 1) * TP_ENTRIES; ++l) { auto j = cmt.find(l); if (j != cmt.end()) j->second.second = false; }
    }
    // mapping update for lpn on a host write (DFTL: miss -> read translation page, insert; dirty)
    void cmt_update(u64 lpn) {
        if (c.mode != "dftl") return;
        auto it = cmt.find(lpn);
        if (it != cmt.end()) { cmt_lru.erase(it->second.first); cmt_lru.push_front(lpn); it->second = {cmt_lru.begin(), true}; return; }
        st.map_miss++; st.read_map++;
        while (cmt.size() >= cmt_cap) cmt_evict_one();
        cmt_lru.push_front(lpn); cmt[lpn] = {cmt_lru.begin(), true};
    }
    // GC relocation: cached entries are updated in place; others update their translation pages in batch
    std::unordered_set<u64> gc_tps;
    void cmt_gc_update(u64 lpn) {
        if (c.mode != "dftl") return;
        auto it = cmt.find(lpn);
        if (it != cmt.end()) { it->second.second = true; return; }
        gc_tps.insert(lpn / TP_ENTRIES);
    }
    void cmt_gc_flush() { for (u64 tp : gc_tps) { (void)tp; st.read_map++; st.prog[S_MAP]++; st.map_flush++; } gc_tps.clear(); }

    int32_t fine_get(u64 iu) const { return fine_idx[iu]; }
    int32_t fine_get_or_create(u64 iu) {
        int32_t i = fine_idx[iu];
        if (i >= 0) return i;
        if (!fpool_free.empty()) { i = fpool_free.back(); fpool_free.pop_back(); }
        else { i = (int32_t)fbitmap.size(); fbitmap.push_back(0); fslot.resize(fslot.size() + U, NONE); flast.push_back(0); fcnt.push_back(0); freq.push_back(0); }
        fbitmap[i] = 0; flast[i] = 0; fcnt[i] = 0; freq[i] = 0;
        fine_idx[iu] = i;
        return i;
    }
    void fine_release(u64 iu) {
        int32_t i = fine_idx[iu];
        if (i < 0) return;
        fine_idx[iu] = -1;
        fbitmap[i] = 0;
        fpool_free.push_back(i);
    }

    void erase_block(int b) {
        if (bvalid[b] != 0) DIE("erase block %d with valid=%d tag=%d", b, bvalid[b], btag[b]);
        if (btag[b] == B_FINE) { fine_blocks--; st.erase_fine++; }
        else if (btag[b] == B_COARSE) { coarse_blocks--; st.erase_coarse++; }
        else DIE("erase free block");
        btag[b] = B_FREE;
        freelist.push_back(b);
        st.erases++;
    }

    int pop_free(Tag t) {
        if (freelist.empty()) DIE("free list exhausted (T=%d too small?)", c.T);
        int b = freelist.back(); freelist.pop_back();
        btag[b] = t; bvalid[b] = 0; bseq[b] = ++alloc_seq;
        if (t == B_FINE) fine_blocks++; else coarse_blocks++;
        return b;
    }

    // allocate a new block for a frontier, applying triggers
    int alloc_block(Tag t) {
        if (t == B_FINE && !in_fine_reclaim && c.mode == "proposed") {
            int stall = 0;
            while (fine_blocks >= fine_quota_blocks) {
                int before = fine_blocks;
                fine_reclaim(stall >= 4);        // after 4 reclaims without net progress, force fold (copying cannot free space)
                if (fine_blocks >= before) stall++; else stall = 0;
                if (stall > 64) DIE("fine tier livelock: quota=%d too small for copy-based reclaim", fine_quota_blocks);
            }
        }
        if (!in_cgc) {
            int stall = 0;
            while ((int)freelist.size() <= c.T) {
                size_t before = freelist.size();
                if (is_pg()) page_gc();
                else if (mixed) mixed_gc();
                else if (ram_cap) {
                    int vf = pick_victim(B_FINE), vc = pick_victim(B_COARSE);
                    bool use_fine = vf >= 0 && (vc < 0 || bvalid[vf] <= bvalid[vc]);
                    if (use_fine) { in_cgc = true; fine_reclaim(); in_cgc = false; } else coarse_gc();
                }
                else coarse_gc();
                if (freelist.size() <= before) { if (++stall > 64) DIE("GC makes no progress: coarse tier over-committed (free=%zu fine=%d coarse=%d quota=%d NB=%d). Try fine_extra=1 or smaller fine_quota.", freelist.size(), fine_blocks, coarse_blocks, fine_quota_blocks, NB); }
                else stall = 0;
            }
        }
        return pop_free(t);
    }

    // get one page slot on the fine frontier
    u32 fine_alloc_page(bool gc = false) {
        Frontier& f = mixed ? ((gc && c.gc_frontier) ? cgf : cf) : ((gc && c.gc_frontier) ? fgf : ff);
        if (f.blk < 0 || f.next == P) { f.blk = alloc_block(mixed ? B_COARSE : B_FINE); f.next = 0; }
        return (u32)f.blk * P + f.next++;
    }
    // get U consecutive slots on the coarse frontier
    u32 coarse_alloc_extent(bool gc = false) {
        Frontier& f = (gc && c.gc_frontier) ? cgf : cf;
        if (f.blk >= 0 && P - f.next < U) { for (int i = f.next; i < P; ++i) p2l[(u32)f.blk * P + i] = NONE; f.next = P; }  // mixed: chunk must not straddle
        if (f.blk < 0 || f.next == P) { f.blk = alloc_block(B_COARSE); f.next = 0; }
        u32 ppn = (u32)f.blk * P + f.next; f.next += U;
        return ppn;
    }
    bool is_frontier(int b) const { return b == ff.blk || b == cf.blk || b == fgf.blk || b == cgf.blk; }

    // ------------------------------------------------------------------ write primitives
    // 4K write into fine tier (proposed)
    void fine_write(u64 lpn, Src src) {
        u32 ppn = fine_alloc_page();
        p2l[ppn] = (u32)lpn;
        bvalid[blk_of(ppn)]++;
        st.prog[src]++; if (src == S_HOST) { st.host_fine++; fine_clock++; }
        u64 iu = lpn / U; int off = (int)(lpn % U);
        int32_t fi = fine_get_or_create(iu);
        if (src == S_HOST) {
            if (freq[fi] != req_id) {   // count once per host request
                double tau = c.unfold_tau * (double)fine_quota_blocks * P;
                if ((double)(fine_clock - flast[fi]) >= tau) fcnt[fi] = 1; else if (fcnt[fi] < 255) fcnt[fi]++;
                freq[fi] = req_id;
            }
            flast[fi] = fine_clock;
        }
        u32 bit = 1u << off;
        if (fbitmap[fi] & bit) {
            u32 old = fslot[(u64)fi * U + off];
            bvalid[blk_of(old)]--;
            if (src == S_HOST) st.frag_freed++;
        } else {
            if (coarse_map[iu] != NONE) bvalid[blk_of(coarse_map[iu])]--;  // superseded in extent
            int od = popc(fbitmap[fi]);
            fbitmap[fi] |= bit;
            track_d(iu, od, od + 1);
            if (popc(fbitmap[fi]) == U && coarse_map[iu] != NONE) { coarse_map[iu] = NONE; st.n_extent_drop++; }  // extent fully dead
        }
        fslot[(u64)fi * U + off] = ppn;
        if ((mixed || ram_cap) && src == S_HOST) mixed_enforce_budget();
    }


    // ---- eager merge (selective RMW): complete a nearly-full IU into one chunk instead of appending fragments
    int eager_union(u64 iu, u64 start, int n) const {
        u32 mask = 0;
        for (int i = 0; i < n; ++i) mask |= 1u << ((start + i) % U);
        int32_t fi = fine_idx[iu];
        if (fi >= 0) mask |= fbitmap[fi];
        return __builtin_popcount(mask);
    }
    bool eager_worth(u64 iu, u64 start, int n) {
        if (c.eager_u <= -10) return n > -c.eager_u - 10;   // -1x: threshold on this request's pages only
        int u = eager_union(iu, start, n);
        if (c.eager_u > 0) return u > c.eager_u;
        // Cost per host page. Completing the IU now writes U pages for n host pages -> U/n.
        // Writing fragments costs 1 page now, plus a later fold (U pages shared by u fragments)
        // but only for the fraction p of fragments that actually end up folded; the rest are
        // retired for free by a coarse-GC merge or by a host overwrite.  p is measured at runtime.
        double p = (st.frag_folded + st.frag_freed) ? (double)st.frag_folded / (st.frag_folded + st.frag_freed) : 0.5;
        return (double)U / n < 1.0 + p * (double)U / std::max(1, u);
    }
    void eager_write(u64 iu, u64 start, int n) {
        int u = eager_union(iu, start, n);
        int32_t fi = fine_idx[iu];
        int d = fi >= 0 ? popc(fbitmap[fi]) : 0;
        u32 old = coarse_map[iu];
        st.n_eager++; st.eager_fill += U - n;
        st.read_fold_fine += d;                       // fragments folded in
        if (old != NONE) st.read_rmw += U - u;        // the rest still comes from the old chunk
        coarse_write(iu, S_HOST, n, S_RMW);           // n host pages + (U-n) filler counted as RMW
    }

    // full IU write into coarse tier; invalidates old extent and fine fragments
    void coarse_write(u64 iu, Src src, u64 nprog_src = 0, Src src2 = S_HOST) {
        u32 ppn = coarse_alloc_extent(src != S_HOST);
        install_extent(iu, ppn, src, nprog_src, src2);
    }
    void install_extent(u64 iu, u32 ppn, Src src, u64 nprog_src = 0, Src src2 = S_HOST) {
        for (int k = 0; k < U; ++k) p2l[ppn + k] = (u32)(iu * U + k);
        bvalid[blk_of(ppn)] += U;
        if (nprog_src) { st.prog[src] += nprog_src; st.prog[src2] += U - nprog_src; }
        else st.prog[src] += U;
        if (src == S_HOST) st.host_coarse += U;
        invalidate_iu(iu);
        coarse_map[iu] = ppn;
    }

    // invalidate old extent + fine fragments of IU (does not touch coarse_map)
    void invalidate_iu(u64 iu) {
        int32_t fi = fine_idx[iu];
        int d = 0;
        if (fi >= 0) {
            u32 bm = fbitmap[fi];
            d = popc(bm);
            while (bm) {
                int off = __builtin_ctz(bm); bm &= bm - 1;
                bvalid[blk_of(fslot[(u64)fi * U + off])]--;
            }
            track_d(iu, d, 0);
            fine_release(iu);
        }
        u32 old = coarse_map[iu];
        if (old != NONE) bvalid[blk_of(old)] -= (U - d);
    }

    // Fold(IU): merge fine d + extent (U-d) into a new extent. refold=true when done by coarse GC.
    void fold(u64 iu, bool refold) {
        u32 ppn = coarse_alloc_extent(true);   // may run nested coarse GC -> evaluate state AFTER
        int32_t fi = fine_idx[iu];
        int d = fi >= 0 ? popc(fbitmap[fi]) : 0;
        u32 old = coarse_map[iu];
        st.read_fold_fine += d;
        Src src;
        if (old != NONE) { st.read_fold_coarse += U - d; src = refold ? S_CGC_REFOLD : S_FOLD; }
        else if (d == U) src = refold ? S_CGC_REFOLD : S_FOLD;  // whole IU lives in fine
        else src = S_ZEROFILL;  // IU never had an extent: remaining pages are zero-fill
        if (refold) { st.n_refold++; st.d_refold[d]++; st.frag_freed += d; } else { st.n_fold++; st.d_fold[d]++; st.frag_folded += d; }
        install_extent(iu, ppn, src);
    }


    // ---- Unfold(IU): during coarse GC, instead of re-folding a HOT IU, move the chunk's remaining (U-d) pages into the
    // fine tier as fragments and kill the chunk. The IU then lives entirely in the fine tier (like extent_drop).
    bool iu_hot(u64 iu) const {
        int32_t fi = fine_idx[iu]; if (fi < 0) return false;
        double tau = c.unfold_tau * (double)fine_quota_blocks * P;
        return (double)(fine_clock - flast[fi]) < tau && fcnt[fi] >= c.unfold_k;
    }
    // unfold runs inside coarse GC (reserve blocks in use): allow it only when the fine GC frontier can absorb the
    // pages, or a fine block can be taken without triggering fine reclaim and without draining the reserve.
    bool can_unfold(int need) const {
        int room = (fgf.blk >= 0) ? (P - fgf.next) : 0;
        if (room >= need) return true;
        return fine_blocks < fine_quota_blocks && (int)freelist.size() > c.T;
    }
    void unfold(u64 iu) {
        u32 old = coarse_map[iu];
        int32_t fi = fine_idx[iu];
        u32 bm = fbitmap[fi]; int d = popc(bm);
        st.n_unfold++; st.d_unfold[d]++;
        bool saved = in_fine_reclaim; in_fine_reclaim = true;   // we verified quota room: skip fine reclaim inside
        for (int off = 0; off < U; ++off) {
            if (bm & (1u << off)) continue;
            u32 np = fine_alloc_page(true);          // may run nested GC (in_cgc set: no recursion), quota bypassed? no: fine quota applies
            fi = fine_idx[iu];                        // re-read: nested activity may have changed state
            if (fi < 0 || coarse_map[iu] != old) { st.wasted_slots++; in_fine_reclaim = saved; return; }   // IU got rewritten meanwhile
            if (fbitmap[fi] & (1u << off)) { st.wasted_slots++; continue; }
            u64 lpn = iu * U + off;
            st.read_fold_coarse++;
            p2l[np] = (u32)lpn; bvalid[blk_of(np)]++; st.prog[S_UNFOLD]++; st.unfold_pages++;
            { int od = popc(fbitmap[fi]); fbitmap[fi] |= (1u << off); track_d(iu, od, od + 1); } fslot[(u64)fi * U + off] = np;
            bvalid[blk_of(old)]--;                    // that chunk page is now superseded
        }
        if (popc(fbitmap[fi]) == U) { coarse_map[iu] = NONE; st.n_extent_drop++; }
        in_fine_reclaim = saved;
    }

    // ------------------------------------------------------------------ reclaim
    // FineGC(block): copy valid 4K pages to fine frontier, erase
    void fine_gc_block(int b) {
        int v0 = bvalid[b];
        st.n_finegc++; st.finegc_valid_sum += v0;
        u32 base = (u32)b * P;
        for (int s = 0; s < P && bvalid[b] > 0; ++s) {
            u32 ppn = base + s;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) continue;
            // allocate new page first: this may run nested coarse GC which can re-fold this IU
            u32 np = fine_alloc_page(true);
            fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) { st.wasted_slots++; continue; }
            st.read_gc_copy++;
            p2l[np] = lpn;
            bvalid[blk_of(np)]++;
            st.prog[S_FINEGC]++;
            fslot[(u64)fi * U + off] = np;
            bvalid[b]--;
        }
        erase_block(b);
    }

    // fold every IU that has valid pages in block b (block-wise fold), then erase
    void fold_block(int b) {
        st.n_blockfold++;
        u32 base = (u32)b * P;
        for (int s = 0; s < P && bvalid[b] > 0; ++s) {
            u32 ppn = base + s;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) continue;
            fold(iu, false);  // invalidates all fragments of iu incl. this one
        }
        if (bvalid[b] != 0) DIE("fold_block left valid=%d", bvalid[b]);
        erase_block(b);
    }


    // greedy: per valid page in victim, fold its IU if the IU has >= fold_d fragments (fold retires all of them,
    // in this and other fine blocks), otherwise copy the page within the fine tier. Then erase.
    void greedy_reclaim_block(int b) {
        st.n_greedy++;
        u32 base = (u32)b * P;
        for (int s0 = 0; s0 < P && bvalid[b] > 0; ++s0) {
            u32 ppn = base + s0;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) continue;
            int d = popc(fbitmap[fi]);
            bool cold = (double)(fine_clock - flast[fi]) >= c.fold_tau * (double)fine_quota_blocks * P;
            bool do_fold;
            if (c.fine_policy == "cost" || c.fine_policy == "cost_cold") {
                // per-slot cost: fold = U/d, copy-path = v/(P-v)  ->  fold iff d*v > U*(P-v)
                bool cheaper = (double)d * victim_v > (double)U * (P - victim_v);
                do_fold = c.fine_policy == "cost" ? cheaper : (cheaper && cold);
            } else do_fold = c.fine_policy == "cold" ? (fold_allowed && cold && d >= c.fold_d) : (d >= c.fold_d);
            if (do_fold) { st.greedy_fold_pages += d; fold(iu, false); continue; }
            // copy within fine tier (same as fine_gc_block's per-page path)
            u32 np = fine_alloc_page(true);
            fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) { st.wasted_slots++; continue; }
            st.read_gc_copy++; st.greedy_copy_pages++;
            p2l[np] = lpn; bvalid[blk_of(np)]++; st.prog[S_FINEGC]++;
            fslot[(u64)fi * U + off] = np; bvalid[b]--;
        }
        if (bvalid[b] != 0) DIE("greedy_reclaim left valid=%d", bvalid[b]);
        erase_block(b);
    }

    int pick_oldest(Tag t) {
        int best = -1; u64 bs = ~0ull;
        for (int b = 0; b < NB; ++b) { if (btag[b] != t || is_frontier(b)) continue; if (bseq[b] < bs) { bs = bseq[b]; best = b; } }
        return best;
    }
    int pick_victim(Tag t) {
        int best = -1, bv = INT32_MAX;
        for (int b = 0; b < NB; ++b) {
            if (btag[b] != t || is_frontier(b)) continue;
            if (bvalid[b] < bv) { bv = bvalid[b]; best = b; }
        }
        return best;
    }

    void fine_reclaim(bool force_fold = false) {
        in_fine_reclaim = true;
        if (c.fine_policy == "fifo_fold" || c.fine_policy == "greedy_copy" || c.fine_policy == "alt") {
            bool do_fifo = c.fine_policy == "fifo_fold" || (c.fine_policy == "alt" && (alt_turn++ % 2 == 0));
            if (force_fold) do_fifo = true;
            if (do_fifo) { int v = pick_oldest(B_FINE); if (v < 0) DIE("no fine victim"); if (bvalid[v] == 0) erase_block(v); else fold_block(v); }
            else { int v = pick_victim(B_FINE); if (v < 0) DIE("no fine victim"); if (bvalid[v] == 0) erase_block(v); else fine_gc_block(v); }
            in_fine_reclaim = false; return;
        }
        int b = pick_victim(B_FINE);
        if (b < 0) DIE("no fine victim");
        int v = bvalid[b];
        last_fine_v = last_fine_v ? 0.7 * last_fine_v + 0.3 * v : v;
        if (v == 0) { erase_block(b); in_fine_reclaim = false; return; }
        // distinct IUs among valid pages, and the fragments of those IUs that live in OTHER fine blocks
        // (a fold retires them too, so they count as extra freed fine slots)
        std::unordered_set<u64> ius; u64 extra = 0;
        u32 base = (u32)b * P;
        for (int s = 0; s < P; ++s) {
            u32 lpn = p2l[base + s];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != base + s) continue;
            if (ius.insert(iu).second) extra += popc(fbitmap[fi]);   // all fragments of this IU ...
            extra--;                                                  // ... minus the ones in this block
        }
        if (force_fold) { st.n_forced_fold++; fold_block(b); in_fine_reclaim = false; return; }
        if (c.fine_policy == "greedy" || c.fine_policy == "cold" || c.fine_policy == "cost" || c.fine_policy == "cost_cold") { fold_allowed = (double)v / P >= c.fold_gate; victim_v = v; greedy_reclaim_block(b); in_fine_reclaim = false; return; }
        double k = (double)ius.size();
        // cost (writes) per freed fine slot
        double cost_gc = v >= P ? 1e18 : (double)v / (double)(P - v);        // copy v fine pages, net gain P-v
        double cost_fold = (double)U * k / (double)(P + extra);              // write 16k coarse pages, gain P + fragments freed elsewhere
        if (cost_fold < cost_gc) fold_block(b);
        else fine_gc_block(b);
        in_fine_reclaim = false;
    }

    // CoarseGC(block): victim = min valid coarse block
    void coarse_gc() {
        in_cgc = true;
        int b = pick_victim(B_COARSE);
        if (b < 0) DIE("no coarse victim (free=%zu fine=%d coarse=%d)", freelist.size(), fine_blocks, coarse_blocks);
        st.n_cgc++; st.cgc_valid_sum += bvalid[b];
        u32 base = (u32)b * P;
        for (int e = 0; e < P / U && bvalid[b] > 0; ++e) {
            u32 ppn = base + e * U;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U;
            if (coarse_map[iu] != ppn) { st.cgc_dead_extents++; continue; }
            int32_t fi = fine_idx[iu];
            if (fi >= 0 && fbitmap[fi]) {
                if (c.unfold == "hot" && iu_hot(iu) && can_unfold(U - popc(fbitmap[fi]))) unfold(iu); else fold(iu, true);
            } else {
                st.cgc_clean_extents++;
                st.read_gc_copy += U;
                coarse_write(iu, S_CGC_CLEAN);
            }
        }
        if (bvalid[b] != 0) DIE("coarse_gc left valid=%d", bvalid[b]);
        erase_block(b);
        in_cgc = false;
    }


    // ================= mixed mode (archived; not used in the paper) =================
    // Rejected 2026-08-23: +4~29% WAF vs tiered because fragments and chunks share blocks (see SPEC.md). Kept for ablation only.
    // Budget eviction: when the fine map exceeds its RAM budget, fold the IUs with the most fragments (max entries freed per write).
    void mixed_enforce_budget() {
        int guard = 0;
        while (fine_mem() > fine_budget) {
            int d = U; while (d > 0 && by_d[d].empty()) d--;
            if (d == 0) break;
            u64 iu = *by_d[d].begin();
            st.n_budget_fold++;
            fold(iu, false);
            if (++guard > 100000) DIE("mixed: budget eviction stuck");
        }
    }
    // Single greedy GC over the shared pool. Walks slots: live chunk -> rewrite (refold if fragments);
    // live fragment whose IU still has a chunk elsewhere or none -> fold iff U/d < v/(P-v), else copy.
    void mixed_gc() {
        in_cgc = true;
        int b = pick_victim(B_COARSE);
        if (b < 0) DIE("mixed: no victim");
        st.n_cgc++; st.cgc_valid_sum += bvalid[b];
        int v = bvalid[b];
        u32 base = (u32)b * P;
        for (int s0 = 0; s0 < P && bvalid[b] > 0; ++s0) {
            u32 ppn = base + s0;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            u32 cm = coarse_map[iu];
            if (cm != NONE && ppn >= cm && ppn < cm + (u32)U) {
                // this slot belongs to the live chunk of iu
                if (ppn != cm) continue;                   // handled at chunk start
                int32_t fi = fine_idx[iu];
                if (fi >= 0 && fbitmap[fi]) fold(iu, true);  // refold: absorb fragments
                else { st.cgc_clean_extents++; st.read_gc_copy += U; coarse_write(iu, S_CGC_CLEAN); }
                s0 += U - 1;
                continue;
            }
            int32_t fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) { continue; }  // dead
            int d = popc(fbitmap[fi]);
            bool do_fold = (double)d * v > (double)U * (P - v);   // U/d < v/(P-v)
            if (do_fold) { st.n_gcfold++; fold(iu, false); continue; }
            // copy fragment (GC frontier)
            u32 np = fine_alloc_page(true);
            fi = fine_idx[iu];
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) { st.wasted_slots++; continue; }
            st.read_gc_copy++;
            p2l[np] = lpn; bvalid[blk_of(np)]++; st.prog[S_FINEGC]++;
            fslot[(u64)fi * U + off] = np; bvalid[b]--;
        }
        if (bvalid[b] != 0) DIE("mixed_gc left valid=%d", bvalid[b]);
        erase_block(b);
        in_cgc = false;
    }

    // ------------------------------------------------------------------ page4k mode
    void page_write(u64 lpn, Src src) {
        u32 ppn = fine_alloc_page(src != S_HOST);
        p2l[ppn] = (u32)lpn;
        bvalid[blk_of(ppn)]++;
        st.prog[src]++;
        u32 old = pmap[lpn];
        if (old != NONE) bvalid[blk_of(old)]--;
        pmap[lpn] = ppn;
        if (src == S_HOST) cmt_update(lpn); else cmt_gc_update(lpn);
    }
    void page_gc() {
        in_cgc = true;
        int b = pick_victim(B_FINE);
        if (b < 0) DIE("no page4k victim");
        st.n_pgc++; st.pgc_valid_sum += bvalid[b];
        u32 base = (u32)b * P;
        for (int s = 0; s < P && bvalid[b] > 0; ++s) {
            u32 ppn = base + s; u32 lpn = p2l[ppn];
            if (lpn == NONE || pmap[lpn] != ppn) continue;
            st.read_gc_copy++;
            page_write(lpn, S_PGC);  // decrements bvalid[b] via old
        }
        if (bvalid[b] != 0) DIE("page_gc left valid=%d", bvalid[b]);
        cmt_gc_flush();
        erase_block(b);
        in_cgc = false;
    }

    // ------------------------------------------------------------------ host write
    void host_write(u64 lpn, u64 n) {
        st.host_pages += n; req_id++;
        u64 end = lpn + n;
        if (is_pg()) {
            for (u64 p = lpn; p < end; ++p) page_write(p, S_HOST);
        } else if (c.mode == "proposed" || c.mode == "mixed") {
            u64 cur = lpn;
            while (cur < end) {
                if (cur % U == 0 && cur + U <= end) { coarse_write(cur / U, S_HOST); cur += U; }
                else {
                    u64 iu = cur / U;
                    u64 e = std::min(end, (iu + 1) * U);          // this request's part of the IU
                    int n = (int)(e - cur);
                    if (c.eager_u != 0 && eager_worth(iu, cur, n)) { eager_write(iu, cur, n); cur = e; }
                    else { while (cur < e) { fine_write(cur, S_HOST); cur++; } }
                }
            }
        } else if (c.mode == "iu_rmw") {
            u64 cur = lpn;
            while (cur < end) {
                u64 iu = cur / U;
                u64 s = std::max(cur, iu * U), e = std::min(end, (iu + 1) * U);
                u64 w = e - s;
                if (w < (u64)U) { st.read_rmw += U - w; coarse_write(iu, S_HOST, w, S_RMW); }
                else coarse_write(iu, S_HOST);
                cur = e;
            }
        } else DIE("unknown mode %s", c.mode.c_str());
        if (measuring && ts && st.host_pages >= ts_next) { ts_sample(); ts_next += ts_interval; }
        u64 fv = 0; (void)fv;
    }

    void ts_header() { fprintf(ts, "host_pages,waf_cum,f,fine_blocks,coarse_blocks,free\n"); }
    void ts_sample() {
        u64 tp = st.total_prog();
        fprintf(ts, "%llu,%.5f,%.5f,%d,%d,%zu\n", (unsigned long long)st.host_pages,
                st.host_pages ? (double)tp / st.host_pages : 0.0,
                (double)fine_blocks / NB, fine_blocks, coarse_blocks, freelist.size());
    }

    void verify() {
        // recompute valid counts from maps and compare
        std::vector<int32_t> v(NB, 0);
        u64 fine_valid = 0;
        for (u64 l = 0; l < L; ++l) {
            if (is_pg()) { if (pmap[l] != NONE) v[blk_of(pmap[l])]++; continue; }
            u64 iu = l / U; int off = (int)(l % U);
            int32_t fi = fine_idx[iu];
            if (fi >= 0 && (fbitmap[fi] & (1u << off))) { v[blk_of(fslot[(u64)fi * U + off])]++; fine_valid++; }
            else if (coarse_map[iu] != NONE) v[blk_of(coarse_map[iu] + off)]++;
        }
        for (int b = 0; b < NB; ++b) if (v[b] != bvalid[b]) DIE("verify: block %d tag=%d computed=%d tracked=%d", b, btag[b], v[b], bvalid[b]);
        fprintf(stderr, "[verify] ok. fine_valid_pages=%llu fine_blocks=%d coarse_blocks=%d free=%zu\n",
                (unsigned long long)fine_valid, fine_blocks, coarse_blocks, freelist.size());
        int hist[11] = {};
        for (int b = 0; b < NB; ++b) if (btag[b] != B_FREE) hist[std::min(10, bvalid[b] * 10 / P)]++;
        fprintf(stderr, "[verify] block valid histogram (deciles 0..100%%):");
        for (int i = 0; i <= 10; ++i) fprintf(stderr, " %d", hist[i]);
        fprintf(stderr, "\n");
    }

    void report() {
        u64 tp = st.total_prog();
        printf("{\n");
        printf("  \"tag\": \"%s\", \"mode\": \"%s\", \"zipf\": %.3f, \"seed\": %llu,\n", c.tag.c_str(), c.mode.c_str(), c.zipf, (unsigned long long)c.seed);
        printf("  \"capacity_gb\": %.2f, \"P\": %d, \"U\": %d, \"op\": %.3f, \"fine_quota\": %.3f, \"fine_quota_blocks\": %d, \"NB\": %d,\n",
               L * 4096.0 / (1 << 30), P, U, c.op, c.fine_quota, fine_quota_blocks, NB);
        { u64 hs = st.host_submitted ? st.host_submitted : st.host_pages;
          printf("  \"host_pages\": %llu, \"host_pages_ftl\": %llu, \"buffer_absorbed\": %.4f, \"total_prog\": %llu, \"WAF\": %.4f, \"WAF_ftl\": %.4f,\n",
                 (unsigned long long)hs, (unsigned long long)st.host_pages, hs ? 1.0 - (double)st.host_pages / hs : 0.0,
                 (unsigned long long)tp, hs ? (double)tp / hs : 0.0, st.host_pages ? (double)tp / st.host_pages : 0.0); }
        printf("  \"prog\": {");
        for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %llu", i ? ", " : "", SRC_NAME[i], (unsigned long long)st.prog[i]);
        printf("},\n");
        printf("  \"prog_frac\": {");
        for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %.4f", i ? ", " : "", SRC_NAME[i], tp ? (double)st.prog[i] / tp : 0.0);
        printf("},\n");
        printf("  \"reads\": {\"rmw\": %llu, \"fold_fine\": %llu, \"fold_coarse\": %llu, \"gc_copy\": %llu, \"map\": %llu},\n",
               (unsigned long long)st.read_rmw, (unsigned long long)st.read_fold_fine, (unsigned long long)st.read_fold_coarse, (unsigned long long)st.read_gc_copy, (unsigned long long)st.read_map);
        printf("  \"RAF\": %.4f, \"map_miss\": %llu, \"map_flush\": %llu,\n", st.host_pages ? (double)(st.read_rmw + st.read_fold_fine + st.read_fold_coarse + st.read_gc_copy + st.read_map) / st.host_pages : 0.0,
               (unsigned long long)st.map_miss, (unsigned long long)st.map_flush);
        printf("  \"erases\": %llu, \"erase_fine\": %llu, \"erase_coarse\": %llu,\n", (unsigned long long)st.erases, (unsigned long long)st.erase_fine, (unsigned long long)st.erase_coarse);
        printf("  \"events\": {\"finegc\": %llu, \"blockfold\": %llu, \"fold\": %llu, \"refold\": %llu, \"cgc\": %llu, \"pgc\": %llu, \"cgc_dead_ext\": %llu, \"cgc_clean_ext\": %llu, \"extent_drop\": %llu, \"wasted_slots\": %llu, \"greedy\": %llu, \"greedy_fold_pages\": %llu, \"greedy_copy_pages\": %llu, \"forced_fold\": %llu, \"eager\": %llu, \"eager_fill\": %llu, \"unfold\": %llu, \"unfold_pages\": %llu, \"budget_fold\": %llu, \"gc_fold\": %llu},\n",
               (unsigned long long)st.n_finegc, (unsigned long long)st.n_blockfold, (unsigned long long)st.n_fold, (unsigned long long)st.n_refold,
               (unsigned long long)st.n_cgc, (unsigned long long)st.n_pgc, (unsigned long long)st.cgc_dead_extents, (unsigned long long)st.cgc_clean_extents, (unsigned long long)st.n_extent_drop, (unsigned long long)st.wasted_slots, (unsigned long long)st.n_greedy, (unsigned long long)st.greedy_fold_pages, (unsigned long long)st.greedy_copy_pages, (unsigned long long)st.n_forced_fold, (unsigned long long)st.n_eager, (unsigned long long)st.eager_fill, (unsigned long long)st.n_unfold, (unsigned long long)st.unfold_pages, (unsigned long long)st.n_budget_fold, (unsigned long long)st.n_gcfold);
        printf("  \"avg_victim_valid\": {\"finegc\": %.1f, \"cgc\": %.1f, \"pgc\": %.1f},\n",
               st.n_finegc ? (double)st.finegc_valid_sum / st.n_finegc : 0.0, st.n_cgc ? (double)st.cgc_valid_sum / st.n_cgc : 0.0,
               st.n_pgc ? (double)st.pgc_valid_sum / st.n_pgc : 0.0);
        printf("  \"d_hist_fold\": [");
        for (int d = 0; d <= U; ++d) printf("%s%llu", d ? "," : "", (unsigned long long)st.d_fold[d]);
        printf("],\n  \"d_hist_unfold\": [");
        for (int d = 0; d <= U; ++d) printf("%s%llu", d ? "," : "", (unsigned long long)st.d_unfold[d]);
        printf("],\n  \"d_hist_refold\": [");
        for (int d = 0; d <= U; ++d) printf("%s%llu", d ? "," : "", (unsigned long long)st.d_refold[d]);
        printf("],\n");
        { u64 fine_ius = 0, fine_pages = 0; for (u64 i = 0; i < NIU; ++i) if (fine_idx[i] >= 0) { fine_ius++; fine_pages += popc(fbitmap[fine_idx[i]]); }
          printf("  \"fine_ius_end\": %llu, \"fine_pages_end\": %llu,\n", (unsigned long long)fine_ius, (unsigned long long)fine_pages);
          // RAM model (measured end state). PPN = 4B. Hash-managed maps: (key+value)/0.7.
          double mem = 0, a = 0, b2 = 0, c2 = 0;
          if (c.mode == "page4k") { a = (double)L * 4; mem = a; }
          else if (c.mode == "dftl") { a = hash_bytes((double)cmt.size(), 8); b2 = (double)(L / TP_ENTRIES) * 4; mem = a + b2; }
          else if (c.mode == "iu_rmw") { a = (double)NIU * 4; mem = a; }
          else { a = (double)NIU * 4; b2 = hash_bytes((double)fine_ius, 4 + 2 + 4 + ((c.unfold != "off" || c.fine_policy == "cold") ? 4 : 0)); c2 = (double)fine_pages * 4; mem = a + b2 + c2; }
          printf("  \"mem\": {\"total_bytes\": %.0f, \"total_mib\": %.2f, \"primary_map\": %.0f, \"hash_index\": %.0f, \"slots\": %.0f},\n", mem, mem / (1 << 20), a, b2, c2); }
        printf("  \"host_fine\": %llu, \"host_coarse\": %llu,\n", (unsigned long long)st.host_fine, (unsigned long long)st.host_coarse);
        printf("  \"fine_blocks_end\": %d, \"coarse_blocks_end\": %d, \"free_end\": %zu, \"f_end\": %.4f\n", fine_blocks, coarse_blocks, freelist.size(), (double)fine_blocks / NB);
        printf("}\n");
    }

};


// ----------------------------------------------------------------------------
// HybridSim: classic log-buffer hybrid FTLs (block-level data mapping + page-level log blocks).
//   mode=fast : FAST  (Lee et al., TECS 2007)  — 1 sequential log block (SW) + fully-associative random log blocks (RW),
//               RW victim = oldest (FIFO), reclaimed by full merge of every associated data block.
//   mode=last : LAST  (Lee et al., SIGOPS OSR 2008) — sequential log buffer (block-associative, several blocks)
//               + random log buffer split into hot/cold partitions; hot victim = dead block (else LRU), cold victim =
//               min associativity (merge cost); reclaimed block goes to cold if victim was dead-hot, else to hot.
// Pool: NB = NLB data blocks + nlog log blocks + T reserve (merge targets).
enum HTag : uint8_t { H_FREE = 0, H_DATA = 1, H_LOG = 2 };
enum LKind : uint8_t { K_NONE = 0, K_SW = 1, K_RW = 2, K_SEQ = 3, K_HOT = 4, K_COLD = 5 };

struct HybridSim {
    Cfg c;
    u64 L, NP, NIU; int NB, P, U; u32 NLB; int nlog, T;
    std::vector<u32> dmap;   // LB -> data block
    std::vector<u32> lmap;   // lpn -> ppn in a log block, or NONE (latest copy is in data block)
    std::vector<u32> p2l;
    std::vector<HTag> btag;
    std::vector<int32_t> bvalid;
    std::vector<int32_t> freelist;
    struct LogBlk {
        LKind kind = K_NONE; int next = 0; u32 lb = NONE; bool inorder = true; u64 last = 0;
        std::unordered_map<u32, int> assoc;  // LB -> valid pages of that LB in this block
    };
    std::vector<LogBlk> lg;
    std::vector<u32> wseq;   // ppn -> random-log write sequence (LAST hotness)
    u64 rseq = 0, tick = 0;
    // FAST
    int sw = -1;                 // sequential log block
    std::vector<int> rw;         // random log blocks, FIFO order (front = oldest); back = active
    // LAST
    int nseq = 0, R = 0, hot_cap = 0, cold_cap = 0;
    std::vector<int> seqb, hotb, coldb;   // membership lists (active = back)
    std::unordered_map<u32, int> seq_of_lb;
    u64 last_k = 0;
    // Janus-FTL (Kwon et al., EMSOFT 2010): BMA (block-mapped data blocks) + PMA (page-mapped: OP blocks + fused data blocks).
    std::vector<uint8_t> fused; std::vector<u64> lb_last; int fused_n = 0, fused_cap = 0; int janus_active = -1;
    u64 n_fusion = 0, n_defusion = 0, n_jgc = 0, jgc_valid_sum = 0;

    Stats st; FILE* ts = nullptr; u64 ts_next = 0, ts_interval = 0; bool measuring = false;
    std::mt19937_64 rng;

    HybridSim(const Cfg& cfg) : c(cfg) {
        P = c.P; U = c.U; T = c.T;
        L = c.capacity / 4096; L -= L % P; NIU = L / U; NLB = (u32)(L / P);
        NP = (u64)((double)L * (1.0 + c.op)); NB = (int)(NP / P); NP = (u64)NB * P;
        nlog = NB - (int)NLB - T;
        if (nlog < 3) DIE("not enough log blocks (NB=%d NLB=%u T=%d)", NB, NLB, T);
        dmap.assign(NLB, NONE); lmap.assign(L, NONE); p2l.assign(NP, NONE);
        btag.assign(NB, H_FREE); bvalid.assign(NB, 0); lg.resize(NB);
        if (c.mode == "last") wseq.assign(NP, 0);
        for (int b = NB - 1; b >= 0; --b) freelist.push_back(b);
        if (c.mode == "last") {
            nseq = std::max(1, (int)(nlog * c.last_seq_frac));
            R = nlog - nseq; if (R < 2) DIE("random log buffer too small");
            hot_cap = std::max(1, std::min(R - 1, (int)(R * c.last_hot_frac))); cold_cap = R - hot_cap;
            last_k = c.last_k ? c.last_k : (u64)hot_cap * P;
        }
        if (c.mode == "janus") {
            fused.assign(NLB, 0); lb_last.assign(NLB, 0);
            double budget = 0.10 * (double)L * 4.0;                       // mapping RAM = 10% of page4k
            fused_cap = std::max(1, (int)((budget - (double)NLB * 4.0) / ((double)P * 4.0)));
            if (c.janus_cap > 0) fused_cap = c.janus_cap; else if (c.janus_cap < 0) fused_cap = (int)NLB;
            fprintf(stderr, "[cfg] janus: fused block cap = %d (RAM budget %.2f MiB), u*=%.2f\n", fused_cap, budget / (1 << 20), c.janus_u);
        }
        st.reset(U); rng.seed(c.seed);
        fprintf(stderr, "[cfg] mode=%s L=%llu pages (%.1f GB) NB=%d NLB=%u nlog=%d (seq=%d hot=%d cold=%d) P=%d (%.0f MiB) T=%d zipf=%.2f\n",
                c.mode.c_str(), (unsigned long long)L, L * 4096.0 / (1 << 30), NB, NLB, nlog, nseq, hot_cap, cold_cap, P, P * 4096.0 / (1 << 20), T, c.zipf);
    }

    inline int blk_of(u32 ppn) const { return (int)(ppn / P); }

    int pop_free(HTag t) {
        if (freelist.empty()) DIE("hybrid: free list exhausted");
        int b = freelist.back(); freelist.pop_back();
        btag[b] = t; bvalid[b] = 0; lg[b] = LogBlk(); return b;
    }
    void erase(int b) {
        if (bvalid[b] != 0) DIE("hybrid erase block %d valid=%d tag=%d kind=%d", b, bvalid[b], btag[b], lg[b].kind);
        btag[b] = H_FREE; lg[b] = LogBlk(); freelist.push_back(b); st.erases++;
    }

    // drop the log copy of lpn (if any) — bookkeeping only
    void drop_log_copy(u64 lpn) {
        u32 old = lmap[lpn]; if (old == NONE) return;
        int b = blk_of(old); bvalid[b]--;
        u32 lb = (u32)(lpn / P);
        auto it = lg[b].assoc.find(lb); if (--it->second == 0) lg[b].assoc.erase(it);
        lmap[lpn] = NONE;
    }
    // invalidate the current version of lpn (before writing a new one)
    void invalidate(u64 lpn) {
        if (lmap[lpn] != NONE) drop_log_copy(lpn);
        else { u32 db = dmap[lpn / P]; if (db != NONE) bvalid[db]--; }
    }
    // physical page holding the latest version of lpn (NONE = never written)
    u32 latest(u64 lpn) const {
        if (lmap[lpn] != NONE) return lmap[lpn];
        u32 db = dmap[lpn / P]; return db == NONE ? NONE : db * P + (u32)(lpn % P);
    }

    // ---- full merge of logical block lb: new data block = latest of every page
    void full_merge(u32 lb) {
        st.n_full++;
        int nb = pop_free(H_DATA);
        for (int off = 0; off < P; ++off) {
            u64 lpn = (u64)lb * P + off;
            u32 src = latest(lpn);
            if (src != NONE) st.read_gc_copy++;
            st.prog[S_FULLMERGE]++;
            p2l[(u32)nb * P + off] = (u32)lpn;
            if (lmap[lpn] != NONE) drop_log_copy(lpn);
        }
        bvalid[nb] = P;
        u32 old = dmap[lb];
        if (old != NONE) { bvalid[old] = 0; erase(old); }
        dmap[lb] = nb;
        if (c.mode == "last") { auto it = seq_of_lb.find(lb); if (it != seq_of_lb.end()) { /* seq block pages now invalid */ } }
    }
    // ---- switch merge: log block b (all P pages of lb written in order) becomes the data block
    void switch_merge(int b) {
        st.n_switch++;
        u32 lb = lg[b].lb;
        for (int off = 0; off < P; ++off) { u64 lpn = (u64)lb * P + off; if (lmap[lpn] != (u32)b * P + off) DIE("switch merge: page %d not in log block", off); lmap[lpn] = NONE; }
        lg[b].assoc.clear();
        u32 old = dmap[lb]; if (old != NONE) { bvalid[old] = 0; erase(old); }
        dmap[lb] = (u32)b; btag[b] = H_DATA; bvalid[b] = P; lg[b] = LogBlk();
    }
    // ---- partial merge: in-order log block with next<P: copy the remaining pages, then switch
    void partial_merge(int b) {
        st.n_partial++;
        u32 lb = lg[b].lb;
        for (int off = lg[b].next; off < P; ++off) {
            u64 lpn = (u64)lb * P + off;
            u32 src = latest(lpn);
            if (src != NONE) st.read_gc_copy++;
            st.prog[S_PARTIALMERGE]++;
            if (lmap[lpn] != NONE) drop_log_copy(lpn);
            p2l[(u32)b * P + off] = (u32)lpn; lmap[lpn] = (u32)b * P + off; bvalid[b]++; lg[b].assoc[lb]++;
        }
        lg[b].next = P;
        switch_merge(b);
    }
    // ---- reclaim a random/seq log block by full-merging every associated data block, then erase
    void merge_reclaim(int b) {
        st.full_assoc_sum += lg[b].assoc.size(); st.n_merge_reclaim++;
        while (!lg[b].assoc.empty()) full_merge(lg[b].assoc.begin()->first);
        if (bvalid[b] != 0) DIE("merge_reclaim left valid=%d", bvalid[b]);
        erase(b);
    }
    // reclaim a block-associative sequential log block (switch / partial / full)
    void seq_reclaim(int b) {
        seq_of_lb.erase(lg[b].lb);
        if (bvalid[b] == 0) { st.n_dead_reclaim++; erase(b); return; }
        if (lg[b].inorder && lg[b].next == P && bvalid[b] == P) { switch_merge(b); return; }
        if (lg[b].inorder && bvalid[b] == lg[b].next) { partial_merge(b); return; }
        merge_reclaim(b);
    }

    // write one page into log block b at its next slot
    void log_append(int b, u64 lpn, Src src) {
        u32 ppn = (u32)b * P + lg[b].next++;
        invalidate(lpn);
        p2l[ppn] = (u32)lpn; lmap[lpn] = ppn; bvalid[b]++; lg[b].assoc[(u32)(lpn / P)]++;
        lg[b].last = ++tick; st.prog[src]++;
        if (c.mode == "last") wseq[ppn] = (u32)(++rseq);
    }

    // ================= FAST =================
    void fast_write_page(u64 lpn) {
        u32 lb = (u32)(lpn / P); int off = (int)(lpn % P);
        if (off == 0) {
            if (sw >= 0) { seq_reclaim(sw); sw = -1; }
            sw = pop_free(H_LOG); lg[sw].kind = K_SW; lg[sw].lb = lb;
        }
        if (sw >= 0 && lg[sw].lb == lb) {
            if (lg[sw].next == off) {            // case 2.1: sequential continuation
                log_append(sw, lpn, S_HOST);
                if (lg[sw].next == P) { seq_reclaim(sw); sw = -1; }
                return;
            }
            seq_reclaim(sw); sw = -1;            // case 2.2/2.3: same LB, out of order -> merge SW now (paper Fig.5)
        }
        // random log
        int rwcap = nlog - 1;
        if (rw.empty() || lg[rw.back()].next == P) {
            if ((int)rw.size() >= rwcap) { int v = rw.front(); rw.erase(rw.begin()); if (bvalid[v] == 0) { st.n_dead_reclaim++; erase(v); } else merge_reclaim(v); }
            int b = pop_free(H_LOG); lg[b].kind = K_RW; rw.push_back(b);
        }
        log_append(rw.back(), lpn, S_HOST);
    }

    // ================= LAST =================
    void last_seq_write_page(u64 lpn) {
        u32 lb = (u32)(lpn / P); int off = (int)(lpn % P);
        auto it = seq_of_lb.find(lb);
        int b = it == seq_of_lb.end() ? -1 : it->second;
        if (b < 0) {
            if ((int)seqb.size() >= nseq) {
                // victim: a block with all pages valid in order (switch), else LRU
                int v = -1; u64 best = ~0ull;
                for (int x : seqb) if (lg[x].inorder && lg[x].next == P && bvalid[x] == P) { v = x; break; }
                if (v < 0) for (int x : seqb) if (lg[x].last < best) { best = lg[x].last; v = x; }
                seqb.erase(std::find(seqb.begin(), seqb.end(), v));
                seq_reclaim(v);
            }
            b = pop_free(H_LOG); lg[b].kind = K_SEQ; lg[b].lb = lb; seqb.push_back(b); seq_of_lb[lb] = b;
        }
        if (lg[b].inorder && off != lg[b].next) lg[b].inorder = false;
        log_append(b, lpn, S_HOST);
        if (lg[b].next == P) { seqb.erase(std::find(seqb.begin(), seqb.end(), b)); seq_reclaim(b); }
    }
    // two-step random log buffer replacement; returns nothing, adjusts caps, frees one block
    void last_random_reclaim() {
        int v = -1; bool from_hot = false, dead = false;
        for (int x : hotb) if (bvalid[x] == 0) { v = x; from_hot = true; dead = true; break; }
        if (v < 0 && coldb.empty()) { u64 best = ~0ull; for (int x : hotb) if (lg[x].last < best) { best = lg[x].last; v = x; } from_hot = true; }
        if (v < 0) {
            size_t bestc = ~size_t(0); u64 bestt = ~0ull;
            for (int x : coldb) { size_t cst = lg[x].assoc.size(); if (cst < bestc || (cst == bestc && lg[x].last < bestt)) { bestc = cst; bestt = lg[x].last; v = x; } }
        }
        auto& lst = from_hot ? hotb : coldb;
        lst.erase(std::find(lst.begin(), lst.end(), v));
        if (bvalid[v] == 0) { st.n_dead_reclaim++; erase(v); } else merge_reclaim(v);
        // reassign the reclaimed slot
        if (c.last_fixed_caps) {}
        else if (dead) { if (hot_cap > 1) { hot_cap--; cold_cap++; } }
        else if (!from_hot) { if (cold_cap > 1) { cold_cap--; hot_cap++; } }
    }
    int last_get_active(bool hot) {
        auto& lst = hot ? hotb : coldb; int cap = hot ? hot_cap : cold_cap;
        if (!lst.empty() && lg[lst.back()].next < P) return lst.back();
        while ((int)hotb.size() + (int)coldb.size() >= R || (int)lst.size() >= cap) {
            last_random_reclaim();
            cap = hot ? hot_cap : cold_cap;
        }
        int b = pop_free(H_LOG); lg[b].kind = hot ? K_HOT : K_COLD; lst.push_back(b); return b;
    }
    void last_rand_write_page(u64 lpn) {
        bool hot = false;
        u32 old = lmap[lpn];
        if (old != NONE) {
            LKind k = lg[blk_of(old)].kind;
            if (k == K_HOT) hot = true;
            else if (k == K_COLD && rseq - wseq[old] < last_k) hot = true;
        }
        if (hot) st.n_hot_pages++; else st.n_cold_pages++;
        int b = last_get_active(hot);
        log_append(b, lpn, S_HOST);
    }


    // ================= Janus-FTL =================
    bool is_pma(int b) const { return btag[b] == H_LOG; }
    double pma_util() const {   // valid data in PMA / PMA space
        u64 valid = 0, space = 0;
        for (int b = 0; b < NB; ++b) if (is_pma(b)) { valid += bvalid[b]; space += P; }
        return space ? (double)valid / space : 0.0;
    }
    void fusion(u32 lb) {
        u32 db = dmap[lb];
        n_fusion++; fused[lb] = 1; fused_n++;
        if (db != NONE) {
            for (int off = 0; off < P; ++off) { u64 lpn = (u64)lb * P + off; if (lmap[lpn] == NONE && p2l[db * P + off] == (u32)lpn) lmap[lpn] = db * P + off; }
            // physical data block becomes a PMA block: its valid pages are now page-mapped in place
            btag[db] = H_LOG; lg[db] = LogBlk(); lg[db].kind = K_RW; lg[db].next = P;
            for (int off = 0; off < P; ++off) { u64 lpn = (u64)lb * P + off; if (lmap[lpn] == db * P + off) lg[db].assoc[lb]++; }
            dmap[lb] = NONE;
        }
    }
    void defusion(u32 lb) {
        n_defusion++;
        int nb = pop_free(H_DATA);
        for (int off = 0; off < P; ++off) {
            u64 lpn = (u64)lb * P + off;
            u32 src = latest(lpn);
            if (src != NONE) st.read_gc_copy++;
            st.prog[S_FULLMERGE]++;
            p2l[(u32)nb * P + off] = (u32)lpn;
            if (lmap[lpn] != NONE) drop_log_copy(lpn);
        }
        bvalid[nb] = P; dmap[lb] = nb; fused[lb] = 0; fused_n--;
    }
    void janus_gc() {
        int v = -1, bv = INT32_MAX;
        for (int b = 0; b < NB; ++b) if (is_pma(b) && b != janus_active && bvalid[b] < bv) { bv = bvalid[b]; v = b; }
        if (v < 0) DIE("janus: no PMA victim");
        n_jgc++; jgc_valid_sum += bvalid[v];
        u32 base = (u32)v * P;
        for (int s0 = 0; s0 < P && bvalid[v] > 0; ++s0) {
            u32 lpn = p2l[base + s0];
            if (lpn == NONE || lmap[lpn] != base + s0) continue;
            st.read_gc_copy++;
            log_append(janus_log_block(true), lpn, S_PGC);
        }
        if (bvalid[v] != 0) DIE("janus gc left valid");
        erase(v);
    }
    int janus_log_block(bool in_gc) {
        if (janus_active >= 0 && lg[janus_active].next < P) return janus_active;
        if (!in_gc) {
            while ((int)freelist.size() <= T) {
                bool over_cap = fused_n > fused_cap;
                if (over_cap || pma_util() > c.janus_u) {
                    // defuse the LRU fused logical block
                    u32 lb = NONE; u64 best = ~0ull;
                    for (u32 x = 0; x < NLB; ++x) if (fused[x] && lb_last[x] < best) { best = lb_last[x]; lb = x; }
                    if (lb == NONE) { janus_gc(); continue; }
                    defusion(lb);
                    // blocks left with no valid pages after defusion are freed immediately
                    for (int b = 0; b < NB; ++b) if (is_pma(b) && b != janus_active && bvalid[b] == 0) erase(b);
                } else janus_gc();
            }
        }
        janus_active = pop_free(H_LOG); lg[janus_active].kind = K_RW; return janus_active;
    }
    void janus_enforce_cap(u32 keep) {
        while (fused_n > fused_cap) {
            u32 lb = NONE; u64 best = ~0ull;
            for (u32 x = 0; x < NLB; ++x) if (fused[x] && x != keep && lb_last[x] < best) { best = lb_last[x]; lb = x; }
            if (lb == NONE) return;
            while ((int)freelist.size() <= T) janus_gc();    // defusion needs an empty block
            defusion(lb);
            for (int b = 0; b < NB; ++b) if (is_pma(b) && b != janus_active && bvalid[b] == 0) erase(b);
        }
    }
    void janus_write_page(u64 lpn) {
        u32 lb = (u32)(lpn / P);
        if (!fused[lb]) { fusion(lb); janus_enforce_cap(lb); }
        lb_last[lb] = ++tick;
        log_append(janus_log_block(false), lpn, S_HOST);
    }

    void host_write(u64 lpn, u64 n) {
        st.host_pages += n;
        if (c.mode == "janus") { for (u64 p = lpn; p < lpn + n; ++p) janus_write_page(p); }
        else if (c.mode == "fast") { for (u64 p = lpn; p < lpn + n; ++p) fast_write_page(p); }
        else if (c.mode == "last") {
            if (n > c.last_seq_thresh) { st.n_seq_req++; for (u64 p = lpn; p < lpn + n; ++p) last_seq_write_page(p); }
            else { st.n_rand_req++; for (u64 p = lpn; p < lpn + n; ++p) last_rand_write_page(p); }
        } else DIE("hybrid: unknown mode %s", c.mode.c_str());
        if (measuring && ts && st.host_pages >= ts_next) { ts_sample(); ts_next += ts_interval; }
    }
    void ts_header() { fprintf(ts, "host_pages,waf_cum,log_blocks,hot_cap,cold_cap,free\n"); }
    void ts_sample() {
        u64 tp = st.total_prog();
        fprintf(ts, "%llu,%.5f,%zu,%d,%d,%zu\n", (unsigned long long)st.host_pages, st.host_pages ? (double)tp / st.host_pages : 0.0,
                rw.size() + seqb.size() + hotb.size() + coldb.size() + (sw >= 0), hot_cap, cold_cap, freelist.size());
    }
    void verify() {
        std::vector<int32_t> v(NB, 0);
        for (u64 l = 0; l < L; ++l) { u32 p = latest(l); if (p != NONE) v[blk_of(p)]++; }
        for (int b = 0; b < NB; ++b) if (v[b] != bvalid[b]) DIE("hybrid verify: block %d tag=%d kind=%d computed=%d tracked=%d", b, btag[b], lg[b].kind, v[b], bvalid[b]);
        u64 logpages = 0; for (u64 l = 0; l < L; ++l) logpages += lmap[l] != NONE;
        fprintf(stderr, "[verify] ok. log_pages=%llu log_blocks=%zu free=%zu hot_cap=%d cold_cap=%d\n", (unsigned long long)logpages,
                rw.size() + seqb.size() + hotb.size() + coldb.size() + (sw >= 0), freelist.size(), hot_cap, cold_cap);
    }
    void report() {
        u64 tp = st.total_prog();
        printf("{\n");
        printf("  \"tag\": \"%s\", \"mode\": \"%s\", \"zipf\": %.3f, \"seed\": %llu,\n", c.tag.c_str(), c.mode.c_str(), c.zipf, (unsigned long long)c.seed);
        printf("  \"capacity_gb\": %.2f, \"P\": %d, \"U\": %d, \"op\": %.3f, \"NB\": %d, \"NLB\": %u, \"nlog\": %d, \"nseq\": %d,\n", L * 4096.0 / (1 << 30), P, U, c.op, NB, NLB, nlog, nseq);
        { u64 hs = st.host_submitted ? st.host_submitted : st.host_pages;
          printf("  \"host_pages\": %llu, \"host_pages_ftl\": %llu, \"buffer_absorbed\": %.4f, \"total_prog\": %llu, \"WAF\": %.4f, \"WAF_ftl\": %.4f,\n",
                 (unsigned long long)hs, (unsigned long long)st.host_pages, hs ? 1.0 - (double)st.host_pages / hs : 0.0,
                 (unsigned long long)tp, hs ? (double)tp / hs : 0.0, st.host_pages ? (double)tp / st.host_pages : 0.0); }
        printf("  \"prog\": {"); for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %llu", i ? ", " : "", SRC_NAME[i], (unsigned long long)st.prog[i]); printf("},\n");
        printf("  \"prog_frac\": {"); for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %.4f", i ? ", " : "", SRC_NAME[i], tp ? (double)st.prog[i] / tp : 0.0); printf("},\n");
        printf("  \"reads\": {\"rmw\": 0, \"fold_fine\": 0, \"fold_coarse\": 0, \"gc_copy\": %llu},\n", (unsigned long long)st.read_gc_copy);
        printf("  \"RAF\": %.4f,\n", st.host_pages ? (double)st.read_gc_copy / st.host_pages : 0.0);
        printf("  \"erases\": %llu,\n", (unsigned long long)st.erases);
        printf("  \"events\": {\"switch\": %llu, \"partial\": %llu, \"full\": %llu, \"dead_reclaim\": %llu, \"avg_assoc\": %.2f, \"seq_req\": %llu, \"rand_req\": %llu, \"hot_pages\": %llu, \"cold_pages\": %llu},\n",
               (unsigned long long)st.n_switch, (unsigned long long)st.n_partial, (unsigned long long)st.n_full, (unsigned long long)st.n_dead_reclaim,
               st.n_merge_reclaim ? (double)st.full_assoc_sum / st.n_merge_reclaim : 0.0,
               (unsigned long long)st.n_seq_req, (unsigned long long)st.n_rand_req, (unsigned long long)st.n_hot_pages, (unsigned long long)st.n_cold_pages);
        { u64 logpages = 0; for (u64 l = 0; l < L; ++l) logpages += lmap[l] != NONE;
          double a = (double)NLB * 4;                                       // block map
          double b2 = c.mode == "janus" ? (double)fused_n * P * 4.0 : hash_bytes((double)logpages, c.mode == "last" ? 12 : 8);  // janus: dense page map per fused block
          double c2 = c.mode == "last" ? (double)nlog * 1 : 0;              // per-log-block associativity degree
          double mem = a + b2 + c2;
          printf("  \"log_pages_end\": %llu,\n", (unsigned long long)logpages);
          printf("  \"mem\": {\"total_bytes\": %.0f, \"total_mib\": %.2f, \"primary_map\": %.0f, \"hash_index\": %.0f, \"slots\": %.0f},\n", mem, mem / (1 << 20), a, b2, c2); }
        printf("  \"janus\": {\"fusion\": %llu, \"defusion\": %llu, \"gc\": %llu, \"avg_gc_valid\": %.0f, \"fused_end\": %d, \"fused_cap\": %d, \"pma_util_end\": %.3f},\n", (unsigned long long)n_fusion, (unsigned long long)n_defusion, (unsigned long long)n_jgc, n_jgc ? (double)jgc_valid_sum / n_jgc : 0.0, fused_n, fused_cap, c.mode == "janus" ? pma_util() : 0.0);
        printf("  \"hot_cap_end\": %d, \"cold_cap_end\": %d\n", hot_cap, cold_cap);
        printf("}\n");
    }
};

// ----------------------------------------------------------------------------
// MuSim: mu-FTL (Lee et al., EMSOFT 2008) — extent-based mapping <l,p,n> in a flash-resident mu-tree with a RAM cache,
// per-partition update blocks (GC copies go to the same update block), victim = most invalid pages,
// invalid-page bitmap kept in the mu-tree, buffered by a hash "bitmap cache" of invalid extents.
// Mapping correctness is kept in memory (leaf-structured extent tree); mu-tree flash I/O is modeled by
// tracking which leaf pages get touched/dirtied and an LRU page cache of cache_bytes.
struct MuSim {
    Cfg c;
    u64 L, NP, NIU; int NB, P, U, T;
    struct Ext { u64 l; u32 p; int n; };
    struct Leaf { u64 id; std::vector<Ext> e; };          // sorted by l; leaves sorted by e.front().l
    std::vector<Leaf> leaves; u64 next_leaf_id = 0;
    std::vector<u32> p2l;
    std::vector<uint8_t> btag;      // 0 free, 1 data
    std::vector<int32_t> binvalid;  // per-block invalid page counter (paper: 1B/block)
    std::vector<int32_t> bwritten;  // pages written so far in block (frontier position)
    std::vector<int32_t> freelist;
    std::vector<int> upd;           // partition -> update block (-1 none)
    u64 part_pages;
    int F, MAXE;
    // mu-tree cache model
    u64 cache_pages; std::list<u64> lru; std::unordered_map<u64, std::pair<std::list<u64>::iterator, bool>> cached;
    // bitmap cache: invalid extents (4B each); key = physical block
    u64 bm_cap, bm_n = 0; std::unordered_map<u32, u32> bm_runs;   // pblock -> #invalid extents buffered
    u64 n_extents = 0, ext_split = 0, ext_merge = 0, bm_flushes = 0, n_gc = 0, gc_valid_sum = 0;
    Stats st; FILE* ts = nullptr; u64 ts_next = 0, ts_interval = 0; bool measuring = false;
    std::mt19937_64 rng;
    bool in_gc = false;

    MuSim(const Cfg& cfg) : c(cfg) {
        P = c.P; U = c.U; T = c.T; F = c.mu_leaf_fanout; MAXE = c.mu_max_extent;
        L = c.capacity / 4096; L -= L % U; NIU = L / U;
        NP = (u64)((double)L * (1.0 + c.op)); NB = (int)(NP / P); NP = (u64)NB * P;
        p2l.assign(NP, NONE); btag.assign(NB, 0); binvalid.assign(NB, 0); bwritten.assign(NB, 0);
        for (int b = NB - 1; b >= 0; --b) freelist.push_back(b);
        upd.assign(c.mu_parts, -1); part_pages = (L + c.mu_parts - 1) / c.mu_parts;
        cache_pages = std::max<u64>(1, c.cache_bytes / 4096); bm_cap = std::max<u64>(1, c.bitmap_cache_bytes / 4);
        leaves.push_back({next_leaf_id++, {}});
        st.reset(U); rng.seed(c.seed);
        fprintf(stderr, "[cfg] mode=mu L=%llu pages (%.1f GB) NB=%d P=%d parts=%d cache=%llu pages bitmap_cache=%llu entries F=%d maxext=%d T=%d zipf=%.2f\n",
                (unsigned long long)L, L * 4096.0 / (1 << 30), NB, P, c.mu_parts, (unsigned long long)cache_pages, (unsigned long long)bm_cap, F, MAXE, T, c.zipf);
    }
    inline int blk_of(u32 ppn) const { return (int)(ppn / P); }

    // ---- mu-tree page cache model
    void touch_leaf(u64 id, bool dirty) {
        auto it = cached.find(id);
        if (it != cached.end()) { lru.erase(it->second.first); lru.push_front(id); it->second.first = lru.begin(); it->second.second |= dirty; return; }
        st.map_miss++; st.read_map++;
        while (cached.size() >= cache_pages) {
            u64 v = lru.back(); lru.pop_back(); auto j = cached.find(v);
            if (j->second.second) { st.prog[S_MAP]++; st.map_flush++; }
            cached.erase(j);
        }
        lru.push_front(id); cached[id] = {lru.begin(), dirty};
    }
    void drop_leaf(u64 id) { auto it = cached.find(id); if (it != cached.end()) { lru.erase(it->second.first); cached.erase(it); } }

    // ---- leaf-structured extent map
    size_t leaf_for(u64 l) {  // last leaf whose first key <= l
        size_t lo = 0, hi = leaves.size();
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (!leaves[mid].e.empty() && leaves[mid].e.front().l <= l) lo = mid; else hi = mid; }
        return lo;
    }
    void insert_ext(const Ext& x) {
        size_t li = leaf_for(x.l); Leaf& lf = leaves[li];
        auto pos = std::lower_bound(lf.e.begin(), lf.e.end(), x.l, [](const Ext& a, u64 v) { return a.l < v; });
        // extent merge: logically & physically contiguous with predecessor in the same physical block
        if (pos != lf.e.begin()) {
            Ext& pr = *(pos - 1);
            if (pr.l + pr.n == x.l && pr.p + pr.n == x.p && blk_of(pr.p) == blk_of(x.p) && pr.n + x.n <= MAXE) { pr.n += x.n; ext_merge++; touch_leaf(lf.id, true); return; }
        }
        lf.e.insert(pos, x); n_extents++; touch_leaf(lf.id, true);
        if ((int)lf.e.size() > F) {  // split leaf
            Leaf nl{next_leaf_id++, {}};
            nl.e.assign(lf.e.begin() + lf.e.size() / 2, lf.e.end()); lf.e.resize(lf.e.size() / 2);
            leaves.insert(leaves.begin() + li + 1, nl);
            touch_leaf(nl.id, true);
        }
    }
    // invalidate physical range [p, p+n): bitmap cache + counters
    void invalidate_phys(u32 p, int n) {
        int b = blk_of(p); binvalid[b] += n;
        if (!in_gc) { if (++bm_runs[(u32)b] == 1) {} bm_n++; if (bm_n >= bm_cap) bm_flush(); }
    }
    void bm_flush() {  // write buffered invalid extents into the mu-tree: one bitmap leaf per group of blocks
        bm_flushes++;
        std::unordered_set<u64> groups; for (auto& kv : bm_runs) groups.insert(kv.first / std::max(1, F / 4));
        for (u64 g : groups) touch_leaf((1ull << 40) | g, true);
        bm_runs.clear(); bm_n = 0;
    }
    // remove mapping for logical range [l, l+n) (Cases 0-3), invalidating the physical pages
    void unmap_range(u64 l, u64 n) {
        u64 end = l + n;
        size_t li = leaf_for(l);
        while (li < leaves.size()) {
            Leaf& lf = leaves[li];
            if (lf.e.empty()) { li++; continue; }
            if (lf.e.front().l >= end) break;
            bool touched = false;
            for (size_t i = 0; i < lf.e.size();) {
                Ext& x = lf.e[i];
                if (x.l >= end) break;
                u64 xe = x.l + x.n;
                if (xe <= l) { i++; continue; }
                touched = true;
                u64 ol = std::max(l, x.l), oe = std::min(end, xe);
                invalidate_phys(x.p + (u32)(ol - x.l), (int)(oe - ol));
                if (ol == x.l && oe == xe) { lf.e.erase(lf.e.begin() + i); n_extents--; continue; }          // case 0
                if (ol > x.l && oe < xe) {                                                                   // case 1: split
                    Ext r{oe, x.p + (u32)(oe - x.l), (int)(xe - oe)};
                    x.n = (int)(ol - x.l);
                    lf.e.insert(lf.e.begin() + i + 1, r); n_extents++; ext_split++; i += 2; continue;
                }
                if (ol > x.l) { x.n = (int)(ol - x.l); i++; continue; }                                     // case 2: trim right
                x.p += (u32)(oe - x.l); x.n = (int)(xe - oe); x.l = oe; i++;                                // case 3: trim left
            }
            if (touched) touch_leaf(lf.id, true);
            if (lf.e.empty() && leaves.size() > 1) { drop_leaf(lf.id); leaves.erase(leaves.begin() + li); continue; }
            li++;
        }
    }
    // current physical page of lpn (NONE if unmapped)
    u32 lookup(u64 lpn) {
        size_t li = leaf_for(lpn); Leaf& lf = leaves[li];
        auto pos = std::upper_bound(lf.e.begin(), lf.e.end(), lpn, [](u64 v, const Ext& a) { return v < a.l; });
        if (pos == lf.e.begin()) return NONE;
        --pos; if (lpn < pos->l + pos->n) return pos->p + (u32)(lpn - pos->l);
        return NONE;
    }

    // ---- blocks
    void erase(int b) { btag[b] = 0; binvalid[b] = 0; bwritten[b] = 0; bm_runs.erase((u32)b); freelist.push_back(b); st.erases++; }
    int pop_free() { if (freelist.empty()) DIE("mu: free list exhausted"); int b = freelist.back(); freelist.pop_back(); btag[b] = 1; return b; }
    void gc() {
        in_gc = true;
        while ((int)freelist.size() < T + 2) {
            int v = -1, best = -1;
            for (int b = 0; b < NB; ++b) { if (!btag[b]) continue; bool isupd = false; for (int u : upd) if (u == b) isupd = true; if (isupd) continue; if (binvalid[b] > best) { best = binvalid[b]; v = b; } }
            if (v < 0) DIE("mu: no victim");
            if (best <= 0) DIE("mu: victim has no invalid pages (free=%zu)", freelist.size());
            n_gc++; gc_valid_sum += bwritten[v] - binvalid[v];
            bm_runs.erase((u32)v);
            u32 base = (u32)v * P;
            // copy valid runs to their partition's update block
            for (int s0 = 0; s0 < bwritten[v];) {
                u32 lpn = p2l[base + s0];
                if (lpn == NONE || lookup(lpn) != base + s0) { s0++; continue; }
                int run = 1;
                while (s0 + run < bwritten[v] && p2l[base + s0 + run] == lpn + run && lookup(lpn + run) == base + s0 + run && run < MAXE) run++;
                st.read_gc_copy += run;
                write_pages(lpn, run, S_PGC);   // remaps; old pages invalidated via unmap_range
                s0 += run;
            }
            if (binvalid[v] != bwritten[v]) DIE("mu gc: block %d invalid=%d written=%d", v, binvalid[v], bwritten[v]);
            erase(v);
        }
        in_gc = false;
    }
    int update_block(int part) {
        int b = upd[part];
        if (b >= 0 && bwritten[b] < P) return b;
        if (!in_gc && (int)freelist.size() <= T) gc();
        b = pop_free(); upd[part] = b; return b;
    }
    // write n logical pages starting at l into the partition's update block; creates extents
    void write_pages(u64 l, int n, Src src) {
        u64 cur = l, end = l + n;
        while (cur < end) {
            int part = (int)(cur / part_pages);
            int b = update_block(part);
            int room = P - bwritten[b];
            int w = (int)std::min<u64>(end - cur, (u64)room);
            // never cross a partition boundary inside one extent
            u64 pend = (u64)(part + 1) * part_pages; if (cur + w > pend) w = (int)(pend - cur);
            u32 ppn = (u32)b * P + bwritten[b];
            for (int i = 0; i < w; ++i) p2l[ppn + i] = (u32)(cur + i);
            bwritten[b] += w; st.prog[src] += w;
            unmap_range(cur, w);
            for (int off = 0; off < w; off += MAXE) insert_ext({cur + off, ppn + off, std::min(MAXE, w - off)});
            cur += w;
        }
    }
    void host_write(u64 lpn, u64 n) {
        st.host_pages += n;
        write_pages(lpn, (int)n, S_HOST);
        if (measuring && ts && st.host_pages >= ts_next) { ts_sample(); ts_next += ts_interval; }
    }
    void ts_header() { fprintf(ts, "host_pages,waf_cum,extents,leaves,free\n"); }
    void ts_sample() { u64 tp = st.total_prog(); fprintf(ts, "%llu,%.5f,%llu,%zu,%zu\n", (unsigned long long)st.host_pages, st.host_pages ? (double)tp / st.host_pages : 0.0, (unsigned long long)n_extents, leaves.size(), freelist.size()); }
    void verify() {
        std::vector<int32_t> valid(NB, 0); u64 mapped = 0, cnt = 0;
        for (auto& lf : leaves) for (auto& x : lf.e) { valid[blk_of(x.p)] += x.n; mapped += x.n; cnt++; }
        if (cnt != n_extents) DIE("mu verify: extent count %llu != %llu", (unsigned long long)cnt, (unsigned long long)n_extents);
        for (int b = 0; b < NB; ++b) if (btag[b] && valid[b] != bwritten[b] - binvalid[b]) DIE("mu verify: block %d valid=%d written=%d invalid=%d", b, valid[b], bwritten[b], binvalid[b]);
        fprintf(stderr, "[verify] ok. mapped=%llu extents=%llu leaves=%zu avg_len=%.1f free=%zu\n", (unsigned long long)mapped, (unsigned long long)n_extents, leaves.size(), n_extents ? (double)mapped / n_extents : 0.0, freelist.size());
    }
    void report() {
        u64 tp = st.total_prog();
        printf("{\n");
        printf("  \"tag\": \"%s\", \"mode\": \"mu\", \"zipf\": %.3f, \"seed\": %llu,\n", c.tag.c_str(), c.zipf, (unsigned long long)c.seed);
        printf("  \"capacity_gb\": %.2f, \"P\": %d, \"U\": %d, \"op\": %.3f, \"NB\": %d, \"parts\": %d, \"cache_bytes\": %llu, \"bitmap_cache_bytes\": %llu,\n", L * 4096.0 / (1 << 30), P, U, c.op, NB, c.mu_parts, (unsigned long long)c.cache_bytes, (unsigned long long)c.bitmap_cache_bytes);
        { u64 hs = st.host_submitted ? st.host_submitted : st.host_pages;
          printf("  \"host_pages\": %llu, \"host_pages_ftl\": %llu, \"buffer_absorbed\": %.4f, \"total_prog\": %llu, \"WAF\": %.4f, \"WAF_ftl\": %.4f,\n",
                 (unsigned long long)hs, (unsigned long long)st.host_pages, hs ? 1.0 - (double)st.host_pages / hs : 0.0,
                 (unsigned long long)tp, hs ? (double)tp / hs : 0.0, st.host_pages ? (double)tp / st.host_pages : 0.0); }
        printf("  \"prog\": {"); for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %llu", i ? ", " : "", SRC_NAME[i], (unsigned long long)st.prog[i]); printf("},\n");
        printf("  \"prog_frac\": {"); for (int i = 0; i < S_N; ++i) printf("%s\"%s\": %.4f", i ? ", " : "", SRC_NAME[i], tp ? (double)st.prog[i] / tp : 0.0); printf("},\n");
        printf("  \"reads\": {\"rmw\": 0, \"fold_fine\": 0, \"fold_coarse\": 0, \"gc_copy\": %llu, \"map\": %llu},\n", (unsigned long long)st.read_gc_copy, (unsigned long long)st.read_map);
        printf("  \"RAF\": %.4f,\n", st.host_pages ? (double)(st.read_gc_copy + st.read_map) / st.host_pages : 0.0);
        printf("  \"erases\": %llu,\n", (unsigned long long)st.erases);
        printf("  \"events\": {\"gc\": %llu, \"avg_victim_valid\": %.1f, \"ext_split\": %llu, \"ext_merge\": %llu, \"bm_flushes\": %llu, \"map_miss\": %llu, \"map_flush\": %llu},\n",
               (unsigned long long)n_gc, n_gc ? (double)gc_valid_sum / n_gc : 0.0, (unsigned long long)ext_split, (unsigned long long)ext_merge, (unsigned long long)bm_flushes, (unsigned long long)st.map_miss, (unsigned long long)st.map_flush);
        u64 mapped = 0; for (auto& lf : leaves) for (auto& x : lf.e) mapped += x.n;
        printf("  \"extents_end\": %llu, \"leaves_end\": %zu, \"avg_extent_len\": %.2f,\n", (unsigned long long)n_extents, leaves.size(), n_extents ? (double)mapped / n_extents : 0.0);
        // RAM: fixed caches + per-block invalid counter (paper: 1B/block)
        double mem = (double)c.cache_bytes + (double)c.bitmap_cache_bytes + (double)NB;
        printf("  \"mem\": {\"total_bytes\": %.0f, \"total_mib\": %.2f, \"tree_cache\": %llu, \"bitmap_cache\": %llu, \"tree_pages_total\": %zu, \"tree_bytes_in_flash\": %zu}\n",
               mem, mem / (1 << 20), (unsigned long long)c.cache_bytes, (unsigned long long)c.bitmap_cache_bytes, leaves.size(), leaves.size() * 4096);
        printf("}\n");
    }
};

// ----------------------------------------------------------------------------
// Workload driver shared by all simulators. S must provide: c, L, U, NIU, rng, st, ts, ts_next, ts_interval,
// measuring, host_write(lpn,n), ts_sample(), verify(), report(), ts_header().
template <class S>
struct Runner {
    S& s; const Cfg& c; u64 L, NIU; int U;
    Runner(S& sim) : s(sim), c(sim.c), L(sim.L), NIU(sim.NIU), U(sim.U) {}
    // ------------------------------------------------------------------ workload
    std::vector<double> cdf;   // over NIU ranks
    std::vector<u32> rank2iu;
    std::vector<double> size_cdf;
    u64 seq_pos = 0;
    void build_workload() {
        if (!c.size_weights.empty()) {
            double acc = 0; size_t pos = 0; std::string w = c.size_weights + ",";
            while ((pos = w.find(',')) != std::string::npos) { acc += atof(w.substr(0, pos).c_str()); size_cdf.push_back(acc); w.erase(0, pos + 1); }
            if ((int)size_cdf.size() != c.max_k - c.min_k + 1) DIE("size_weights needs %d entries", c.max_k - c.min_k + 1);
            for (auto& x : size_cdf) x /= acc;
        }
        if (c.zipf > 0) {
            cdf.resize(NIU);
            double acc = 0;
            for (u64 r = 0; r < NIU; ++r) { acc += 1.0 / std::pow((double)(r + 1), c.zipf); cdf[r] = acc; }
            for (auto& x : cdf) x /= acc;
            rank2iu.resize(NIU);
            for (u64 i = 0; i < NIU; ++i) rank2iu[i] = (u32)i;
            if (c.zipf_shuffle) std::shuffle(rank2iu.begin(), rank2iu.end(), s.rng);
        }
    }
    u64 sample_iu() {
        if (c.zipf <= 0) return s.rng() % NIU;
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(s.rng);
        u64 r = std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin();
        if (r >= NIU) r = NIU - 1;
        return rank2iu[r];
    }
    // ---- device write buffer (applies to every FTL mode identically)
    std::list<u64> wb_lru;                                                   // front = MRU, holds IU ids
    std::unordered_map<u64, std::pair<std::list<u64>::iterator, u32>> wb;  // iu -> (lru pos, bitmap)
    u64 wb_pages = 0;
    void wb_flush_iu(u64 iu, u32 bm) {
        // emit contiguous runs of buffered pages
        int off = 0;
        while (off < U) {
            if (!(bm & (1u << off))) { off++; continue; }
            int start = off; while (off < U && (bm & (1u << off))) off++;
            s.host_write(iu * U + start, off - start);
        }
        wb_pages -= __builtin_popcount(bm);
    }
    void wb_write(u64 lpn, u64 n) {
        s.st.host_submitted += n;
        if (!c.wbuf_pages) { s.host_write(lpn, n); return; }
        u64 cur = lpn, end = lpn + n;
        while (cur < end) {
            u64 iu = cur / U; int off = (int)(cur % U);
            int w = (int)std::min<u64>(end - cur, (u64)(U - off));
            u32 bits = ((w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1)) << off);
            auto it = wb.find(iu);
            if (it == wb.end()) { wb_lru.push_front(iu); it = wb.emplace(iu, std::make_pair(wb_lru.begin(), 0u)).first; }
            else { wb_lru.erase(it->second.first); wb_lru.push_front(iu); it->second.first = wb_lru.begin(); }
            u32 old = it->second.second; u32 nb = old | bits;
            wb_pages += __builtin_popcount(nb) - __builtin_popcount(old);   // overwrites of buffered pages are absorbed
            it->second.second = nb;
            if (__builtin_popcount(nb) == U) { wb_lru.erase(it->second.first); wb.erase(it); s.host_write(iu * U, U); wb_pages -= U; }
            cur += w;
            while (wb_pages > c.wbuf_pages && !wb_lru.empty()) {           // evict LRU IU as fragments
                u64 v = wb_lru.back(); wb_lru.pop_back(); u32 bm = wb[v].second; wb.erase(v); wb_flush_iu(v, bm);
            }
        }
    }
    void wb_drain() { while (!wb_lru.empty()) { u64 v = wb_lru.back(); wb_lru.pop_back(); u32 bm = wb[v].second; wb.erase(v); wb_flush_iu(v, bm); } }
    void run_workload(u64 pages) {
        u64 done = 0;
        while (done < pages) {
            u64 iu = sample_iu();
            int k;
            if (size_cdf.empty()) k = c.min_k + (int)(s.rng() % (u64)(c.max_k - c.min_k + 1));
            else { double u = std::uniform_real_distribution<double>(0.0, 1.0)(s.rng); k = c.min_k + (int)(std::lower_bound(size_cdf.begin(), size_cdf.end(), u) - size_cdf.begin()); if (k > c.max_k) k = c.max_k; }
            u64 n = 1ull << k;
            u64 lpn = iu * U + (s.rng() % U);
            if (c.seq_pattern) { lpn = seq_pos; seq_pos += n; if (seq_pos >= L) seq_pos = 0; }
            if (lpn + n > L) lpn = L - n;
            wb_write(lpn, n);
            done += n;
        }
    }
    void seq_fill() {
        const u64 chunk = 64;  // 256K sequential writes
        for (u64 p = 0; p < L; p += chunk) wb_write(p, std::min(chunk, L - p));
    }

    void run() {
        build_workload();
        if (c.seq_fill) { fprintf(stderr, "[phase] seq fill\n"); seq_fill(); }
        if (c.age_x > 0) { fprintf(stderr, "[phase] aging %.2fx\n", c.age_x); run_workload((u64)(L * c.age_x)); }
        s.verify();
        wb_drain();
        fprintf(stderr, "[phase] reset stats, measure %.2fx\n", c.measure_x);
        s.st.reset(U);
        s.measuring = true;
        s.ts_interval = c.ts_interval ? c.ts_interval : std::max<u64>(1, L / 200);
        s.ts_next = s.ts_interval;
        if (!c.out_ts.empty()) { s.ts = fopen(c.out_ts.c_str(), "w"); if (s.ts) s.ts_header(); }
        run_workload((u64)(L * c.measure_x));
        wb_drain();
        if (s.ts) { s.ts_sample(); fclose(s.ts); }
        s.verify();
        s.report();
    }
};

// ----------------------------------------------------------------------------
static u64 parse_bytes(const std::string& s) {
    char* end; double v = strtod(s.c_str(), &end);
    std::string suf(end);
    if (suf == "G" || suf == "GB") return (u64)(v * (1ull << 30));
    if (suf == "M" || suf == "MB") return (u64)(v * (1ull << 20));
    if (suf == "K" || suf == "KB") return (u64)(v * 1024);
    return (u64)v;
}

int main(int argc, char** argv) {
    Cfg c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) { fprintf(stderr, "bad arg %s (expect key=value)\n", argv[i]); return 1; }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "capacity") c.capacity = parse_bytes(v);
        else if (k == "P") c.P = atoi(v.c_str());
        else if (k == "U") c.U = atoi(v.c_str());
        else if (k == "op") c.op = atof(v.c_str());
        else if (k == "fine_quota") c.fine_quota = atof(v.c_str());
        else if (k == "T") c.T = atoi(v.c_str());
        else if (k == "mode") c.mode = v;
        else if (k == "zipf") c.zipf = atof(v.c_str());
        else if (k == "min_k") c.min_k = atoi(v.c_str());
        else if (k == "max_k") c.max_k = atoi(v.c_str());
        else if (k == "size_weights") c.size_weights = v;
        else if (k == "wbuf") c.wbuf_pages = parse_bytes(v) / 4096;
        else if (k == "seq_pattern") c.seq_pattern = atoi(v.c_str()) != 0;
        else if (k == "fine_budget_bytes") c.fine_budget_bytes = parse_bytes(v);
        else if (k == "fine_cap") c.fine_cap = v;
        else if (k == "seed") c.seed = strtoull(v.c_str(), nullptr, 10);
        else if (k == "age_x") c.age_x = atof(v.c_str());
        else if (k == "measure_x") c.measure_x = atof(v.c_str());
        else if (k == "seq_fill") c.seq_fill = atoi(v.c_str()) != 0;
        else if (k == "ts_interval") c.ts_interval = strtoull(v.c_str(), nullptr, 10);
        else if (k == "out_ts") c.out_ts = v;
        else if (k == "tag") c.tag = v;
        else if (k == "fine_extra") c.fine_extra = atoi(v.c_str()) != 0;
        else if (k == "gc_frontier") c.gc_frontier = atoi(v.c_str()) != 0;
        else if (k == "fine_policy") c.fine_policy = v;
        else if (k == "unfold") c.unfold = v;
        else if (k == "unfold_tau") c.unfold_tau = atof(v.c_str());
        else if (k == "unfold_k") c.unfold_k = atoi(v.c_str());
        else if (k == "eager_u") c.eager_u = atoi(v.c_str());
        else if (k == "fold_d") c.fold_d = atoi(v.c_str());
        else if (k == "fold_tau") c.fold_tau = atof(v.c_str());
        else if (k == "fold_gate") c.fold_gate = atof(v.c_str());
        else if (k == "zipf_shuffle") c.zipf_shuffle = atoi(v.c_str()) != 0;
        else if (k == "last_seq_frac") c.last_seq_frac = atof(v.c_str());
        else if (k == "last_seq_thresh") c.last_seq_thresh = strtoull(v.c_str(), nullptr, 10);
        else if (k == "last_hot_frac") c.last_hot_frac = atof(v.c_str());
        else if (k == "last_fixed_caps") c.last_fixed_caps = atoi(v.c_str()) != 0;
        else if (k == "cache_bytes") c.cache_bytes = parse_bytes(v);
        else if (k == "bitmap_cache_bytes") c.bitmap_cache_bytes = parse_bytes(v);
        else if (k == "mu_parts") c.mu_parts = atoi(v.c_str());
        else if (k == "mu_max_extent") c.mu_max_extent = atoi(v.c_str());
        else if (k == "mu_leaf_fanout") c.mu_leaf_fanout = atoi(v.c_str());
        else if (k == "janus_u") c.janus_u = atof(v.c_str());
        else if (k == "janus_cap") c.janus_cap = atoi(v.c_str());
        else if (k == "last_k") c.last_k = strtoull(v.c_str(), nullptr, 10);
        else { fprintf(stderr, "unknown key %s\n", k.c_str()); return 1; }
    }
    if (c.mode == "cftl") c.mode = "dftl";  // alias
    if (c.U > 32) { fprintf(stderr, "U > 32 unsupported (bitmap)\n"); return 1; }
    if (c.P == 0) { u64 pages = c.capacity / 4096; u64 p = pages * 3 / 1024; p -= p % (64 * c.U); if (p < (u64)(64 * c.U)) p = 64 * c.U; c.P = (int)p; }
    if (c.mode == "fast" || c.mode == "last" || c.mode == "janus") { HybridSim s(c); Runner<HybridSim> r(s); r.run(); }
    else if (c.mode == "mu") { MuSim s(c); Runner<MuSim> r(s); r.run(); }
    else { Sim s(c); Runner<Sim> r(s); r.run(); }
    return 0;
}
